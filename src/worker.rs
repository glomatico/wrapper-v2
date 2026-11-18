//! Single native-worker supervisor. Rust owns process lifecycle, IPC timeouts,
//! and HTTP-facing 2FA continuations; proprietary code remains in the worker.
use std::{
    process::Stdio,
    sync::{Arc, Mutex as StdMutex},
    time::Duration,
};

use serde_json::{json, Value};
use tokio::{
    io::{AsyncReadExt, AsyncWriteExt},
    process::{Child, ChildStdin, ChildStdout, Command},
    sync::{oneshot, Mutex, OwnedMutexGuard},
    time::timeout,
};
use zeroize::{Zeroize, Zeroizing};

use crate::{config::Config, error::ApiError};

// Best-effort wiping for owned JSON values that may contain credentials,
// signed URLs or context material. Allocator/internal serde copies cannot be
// guaranteed erased; IPC and HTTP framing buffers are wiped separately.
pub(crate) fn wipe_json_value(value: &mut Value) {
    match value {
        Value::String(s) => s.zeroize(),
        Value::Object(map) => map.values_mut().for_each(wipe_json_value),
        Value::Array(values) => values.iter_mut().for_each(wipe_json_value),
        _ => {}
    }
}

const MAX_REQUEST: usize = 1024 * 1024;
const MAX_RESPONSE: usize = 4 * 1024 * 1024;
const COMMAND_LIMIT: Duration = Duration::from_secs(125);
// Bound native login activity, but not the time a human takes to answer 2FA.
const LOGIN_STEP_LIMIT: Duration = Duration::from_secs(610);

// Only these fixed enums and bounded primitive values from native IPC may
// reach logs. In particular, never log Apple's textual errors or dialog text.
#[derive(Debug)]
struct AuthDiagnostic<'a> {
    stage: &'a str,
    callback_failure: &'a str,
    response_type: Option<i32>,
    apple_error_code: Option<i32>,
    credential_callbacks: u64,
    dialog_callbacks: u64,
    unmatched_dialogs: u64,
    hsa2_requested: bool,
    known_dialog_selected: bool,
}

fn parse_auth_diagnostic(value: &Value) -> Option<AuthDiagnostic<'_>> {
    let stage = value.get("stage")?.as_str()?;
    if !matches!(
        stage,
        "before_flow"
            | "flow_unavailable"
            | "running_flow"
            | "response_missing"
            | "native_response"
            | "callback_rejected"
    ) {
        return None;
    }
    let callback_failure = value.get("callback_failure")?.as_str()?;
    if !matches!(
        callback_failure,
        "none" | "inactive_login" | "credential_limit" | "code_cancelled" | "unhandled_dialog"
    ) {
        return None;
    }
    let bounded_count = |name| value.get(name)?.as_u64().filter(|count| *count <= 16);
    let bounded_int = |name| {
        value
            .get(name)
            .and_then(Value::as_i64)
            .and_then(|n| i32::try_from(n).ok())
    };
    Some(AuthDiagnostic {
        stage,
        callback_failure,
        response_type: bounded_int("response_type"),
        apple_error_code: bounded_int("apple_error_code"),
        credential_callbacks: bounded_count("credential_callbacks")?,
        dialog_callbacks: bounded_count("dialog_callbacks")?,
        unmatched_dialogs: bounded_count("unmatched_dialogs")?,
        hsa2_requested: value.get("hsa2_requested")?.as_bool()?,
        known_dialog_selected: value.get("known_dialog_selected")?.as_bool()?,
    })
}

fn log_auth_diagnostic(error: &Value) {
    if let Some(diag) = error.get("auth").and_then(parse_auth_diagnostic) {
        tracing::warn!(
            event = "session.auth_diagnostic",
            stage = diag.stage,
            callback_failure = diag.callback_failure,
            response_type = ?diag.response_type,
            apple_error_code = ?diag.apple_error_code,
            credential_callbacks = diag.credential_callbacks,
            dialog_callbacks = diag.dialog_callbacks,
            unmatched_dialogs = diag.unmatched_dialogs,
            hsa2_requested = diag.hsa2_requested,
            known_dialog_selected = diag.known_dialog_selected
        );
    }
}

pub enum LoginReply {
    Finished(Value),
    Challenge { id: String, kind: String },
}

type LoginDelivery = oneshot::Sender<Result<LoginReply, ApiError>>;
type Answer = (Zeroizing<String>, LoginDelivery);

struct Pending {
    id: String,
    answer: oneshot::Sender<Answer>,
}

pub struct Supervisor {
    settings: Config,
    control: Arc<Mutex<Controller>>,
    pending: StdMutex<Option<Pending>>,
}

struct Controller {
    next_id: u64,
    worker: Option<NativeWorker>,
    // Preserve only the fact that a live worker was authenticated. A new
    // native process must revalidate the cached session before serving work.
    restore_after_restart: bool,
}

struct NativeWorker {
    child: Child,
    stdin: ChildStdin,
    stdout: ChildStdout,
}

enum Packet {
    Success(Value),
    Error(ApiError),
    Challenge { id: String, kind: String },
}

impl Supervisor {
    pub fn new(settings: Config) -> Arc<Self> {
        Arc::new(Self {
            settings,
            control: Arc::new(Mutex::new(Controller {
                next_id: 1,
                worker: None,
                restore_after_restart: false,
            })),
            pending: StdMutex::new(None),
        })
    }

    pub async fn call(&self, op: &str, params: Value) -> Result<Value, ApiError> {
        let mut control = self.control.try_lock().map_err(|_| busy())?;
        let result = control.command(&self.settings, op, params).await;
        match result {
            Ok(Packet::Success(value)) => {
                if matches!(op, "session.status" | "session.restore") {
                    control.restore_after_restart =
                        value.get("state").and_then(Value::as_str) == Some("authenticated");
                }
                if op == "session.restore" {
                    tracing::info!(event = "session.restored");
                }
                if op == "session.close" {
                    control.restore_after_restart = false;
                    tracing::info!(event = "session.closed");
                    control.discard().await;
                }
                Ok(value)
            }
            Ok(Packet::Error(error)) => {
                if matches!(
                    error.code.as_str(),
                    "session_expired"
                        | "authentication_required"
                        | "session_validation_failed"
                        | "no_cached_session"
                ) {
                    control.restore_after_restart = false;
                }
                tracing::warn!(event = "worker.error", operation = op,
                    code = %error.code, restart_required = error.restart_required);
                if error.restart_required {
                    control.discard().await;
                }
                Err(error)
            }
            Ok(Packet::Challenge { .. }) => {
                tracing::error!(
                    event = "worker.error",
                    operation = op,
                    code = "unexpected_worker_event"
                );
                control.discard().await;
                Err(ApiError::worker_unavailable("unexpected_worker_event"))
            }
            Err(error) => {
                tracing::error!(event = "worker.error", operation = op,
                    code = %error.code);
                control.discard().await;
                Err(error)
            }
        }
    }

    pub async fn login(
        self: &Arc<Self>,
        username: Zeroizing<String>,
        password: Zeroizing<String>,
    ) -> Result<LoginReply, ApiError> {
        let control = self.control.clone().try_lock_owned().map_err(|_| busy())?;
        let (reply, received) = oneshot::channel();
        let this = Arc::clone(self);
        tokio::spawn(async move {
            this.login_task(control, username, password, reply).await;
        });
        received
            .await
            .map_err(|_| ApiError::worker_unavailable("login_worker_closed"))?
    }

    pub async fn challenge(
        &self,
        id: &str,
        code: Zeroizing<String>,
    ) -> Result<LoginReply, ApiError> {
        let pending = {
            let mut slot = self
                .pending
                .lock()
                .map_err(|_| ApiError::worker_unavailable("internal_error"))?;
            match slot.as_ref() {
                Some(p) if p.id == id => slot.take(),
                Some(_) => return Err(ApiError::bad_request("invalid_challenge")),
                None => return Err(ApiError::bad_request("no_pending_challenge")),
            }
        };
        let (reply, received) = oneshot::channel();
        pending
            .expect("checked pending")
            .answer
            .send((code, reply))
            .map_err(|_| ApiError::worker_unavailable("challenge_expired"))?;
        timeout(LOGIN_STEP_LIMIT, received)
            .await
            .map_err(|_| ApiError::worker_unavailable("worker_timeout"))?
            .map_err(|_| ApiError::worker_unavailable("login_worker_closed"))?
    }

    pub fn pending_challenge(&self) -> Result<Option<LoginReply>, ApiError> {
        let pending = self
            .pending
            .lock()
            .map_err(|_| ApiError::worker_unavailable("internal_error"))?;
        Ok(pending.as_ref().map(|challenge| LoginReply::Challenge {
            id: challenge.id.clone(),
            kind: "two_factor".into(),
        }))
    }

    async fn login_task(
        self: Arc<Self>,
        mut control: OwnedMutexGuard<Controller>,
        username: Zeroizing<String>,
        password: Zeroizing<String>,
        first: LoginDelivery,
    ) {
        let mut reply = Some(first);
        let mut terminal_session = None;
        let result: Result<(), ApiError> = async {
            control.ensure(&self.settings).await?;
            let id = control.allocate_id();
            let params = json!({"username":username.as_str(),"password":password.as_str()});
            timeout(
                LOGIN_STEP_LIMIT,
                control.send(&id, "session.login", params, 600000),
            )
            .await
            .map_err(|_| ApiError::worker_unavailable("worker_timeout"))??;
            // The native worker is synchronous. Responses may include 2FA
            // challenges before the final session.login response.
            loop {
                let packet = timeout(LOGIN_STEP_LIMIT, control.read(&id))
                    .await
                    .map_err(|_| ApiError::worker_unavailable("worker_timeout"))??;
                match packet {
                    Packet::Success(value) => {
                        terminal_session = Some(value);
                        return Ok(());
                    }
                    Packet::Error(error) => return Err(error),
                    Packet::Challenge {
                        id: challenge_id,
                        kind,
                    } => {
                        let (tx, rx) = oneshot::channel();
                        {
                            let mut pending = self
                                .pending
                                .lock()
                                .map_err(|_| ApiError::worker_unavailable("internal_error"))?;
                            if pending.is_some() {
                                return Err(ApiError::worker_unavailable(
                                    "invalid_challenge_state",
                                ));
                            }
                            *pending = Some(Pending {
                                id: challenge_id.clone(),
                                answer: tx,
                            });
                        }
                        let answer = LoginReply::Challenge {
                            id: challenge_id.clone(),
                            kind,
                        };
                        if reply
                            .take()
                            .expect("active login reply")
                            .send(Ok(answer))
                            .is_err()
                        {
                            return Err(ApiError::worker_unavailable("login_client_closed"));
                        }
                        // Waiting for an operator has no Wrapper-imposed
                        // deadline. An empty code explicitly cancels login.
                        let (code, continuation) = rx
                            .await
                            .map_err(|_| ApiError::worker_unavailable("challenge_expired"))?;
                        reply = Some(continuation);
                        timeout(
                            LOGIN_STEP_LIMIT,
                            control.send(
                                &id,
                                "auth.respond",
                                json!({"challenge_id":challenge_id,"code":code.as_str()}),
                                180000,
                            ),
                        )
                        .await
                        .map_err(|_| ApiError::worker_unavailable("worker_timeout"))??;
                    }
                }
            }
        }
        .await;

        if let Err(error) = &result {
            tracing::warn!(event = "worker.error", operation = "session.login",
                code = %error.code);
            control.restore_after_restart = false;
            control.discard().await;
        } else {
            control.restore_after_restart = terminal_session
                .as_ref()
                .and_then(|value| value.get("state"))
                .and_then(Value::as_str)
                == Some("authenticated");
        }
        if let Ok(mut pending) = self.pending.lock() {
            *pending = None;
        }
        // The HTTP caller must never observe success or failure while the
        // worker lock is still held: otherwise an immediate follow-up request
        // spuriously receives worker_busy during the cleanup race.
        drop(control);
        if let Some(destination) = reply.take() {
            let outcome = match result {
                Ok(()) => {
                    tracing::info!(event = "session.authenticated");
                    Ok(LoginReply::Finished(
                        terminal_session.expect("successful login has a terminal response"),
                    ))
                }
                Err(error) => Err(error),
            };
            let _ = destination.send(outcome);
        }
    }
}

fn busy() -> ApiError {
    ApiError::new(
        "worker_busy",
        hyper::StatusCode::SERVICE_UNAVAILABLE,
        true,
        false,
    )
}

impl Controller {
    fn allocate_id(&mut self) -> String {
        let id = self.next_id;
        self.next_id = self.next_id.wrapping_add(1);
        id.to_string()
    }

    async fn ensure(&mut self, config: &Config) -> Result<(), ApiError> {
        if let Some(worker) = self.worker.as_mut() {
            if worker
                .child
                .try_wait()
                .map_err(|_| ApiError::worker_unavailable("worker_unavailable"))?
                .is_none()
            {
                return Ok(());
            }
            self.discard().await;
        }
        let mut command = Command::new(&config.launcher);
        command
            .arg(&config.rootfs)
            // Native code only needs the framed IPC, not the login secrets.
            .env_remove("WRAPPER_USERNAME")
            .env_remove("WRAPPER_PASSWORD")
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::null())
            .kill_on_drop(true);
        let mut child = command
            .spawn()
            .map_err(|_| ApiError::worker_unavailable("worker_spawn_failed"))?;
        let stdin = child
            .stdin
            .take()
            .ok_or_else(|| ApiError::worker_unavailable("worker_spawn_failed"))?;
        let stdout = child
            .stdout
            .take()
            .ok_or_else(|| ApiError::worker_unavailable("worker_spawn_failed"))?;
        self.worker = Some(NativeWorker {
            child,
            stdin,
            stdout,
        });
        let id = self.allocate_id();
        let params = json!({
            "session_dir":config.session_dir,
            "device_info":config.device_info,
            "dns":config.dns,
            "architecture":config.profile.architecture,
            "profile":config.profile.profile,
        });
        let outcome = timeout(COMMAND_LIMIT, async {
            self.send(&id, "initialize", params, 120000).await?;
            self.read(&id).await
        })
        .await
        .map_err(|_| ApiError::worker_unavailable("worker_timeout"))?;
        match outcome? {
            Packet::Success(result)
                if result.get("profile").and_then(Value::as_str)
                    == Some(config.profile.profile.as_str())
                    && result.get("architecture").and_then(Value::as_str)
                        == Some(config.profile.architecture.as_str())
                    && result.get("context_supported").and_then(Value::as_bool)
                        == Some(config.profile.context_supported)
                    && result.get("protocol").and_then(Value::as_u64) == Some(1) =>
            {
                tracing::info!(event = "worker.started",
                    architecture = %config.profile.architecture);
            }
            Packet::Error(error) => return Err(error),
            _ => return Err(ApiError::worker_unavailable("unsupported_worker_profile")),
        }
        if self.restore_after_restart {
            // The worker has no in-memory authentication after recycling. The
            // saved native session survives in session_dir, just as it does
            // after a full container restart. Do not silently restore sessions
            // on first startup or after an explicit session.close.
            self.restore_after_restart = false;
            let id = self.allocate_id();
            let restored = timeout(COMMAND_LIMIT, async {
                self.send(&id, "session.restore", json!({}), 120000).await?;
                self.read(&id).await
            })
            .await
            .map_err(|_| ApiError::worker_unavailable("worker_timeout"))??;
            match restored {
                Packet::Success(value)
                    if value.get("state").and_then(Value::as_str) == Some("authenticated") =>
                {
                    self.restore_after_restart = true;
                    tracing::info!(event = "session.recovered");
                }
                Packet::Error(error) => {
                    tracing::warn!(event = "session.recovery_failed", code = %error.code);
                    if error.restart_required {
                        return Err(error);
                    }
                }
                Packet::Success(_) => {
                    tracing::warn!(
                        event = "session.recovery_failed",
                        code = "not_authenticated"
                    );
                }
                Packet::Challenge { .. } => {
                    return Err(ApiError::worker_unavailable("unexpected_worker_event"));
                }
            }
        }
        Ok(())
    }

    async fn command(
        &mut self,
        config: &Config,
        op: &str,
        params: Value,
    ) -> Result<Packet, ApiError> {
        self.ensure(config).await?;
        let id = self.allocate_id();
        timeout(COMMAND_LIMIT, async {
            self.send(&id, op, params, 120000).await?;
            self.read(&id).await
        })
        .await
        .map_err(|_| ApiError::worker_unavailable("worker_timeout"))?
    }

    async fn send(
        &mut self,
        id: &str,
        op: &str,
        params: Value,
        timeout_ms: u32,
    ) -> Result<(), ApiError> {
        let mut request = json!({"v":1,"id":id,"op":op,"params":params,"timeout_ms":timeout_ms});
        let serialized = serde_json::to_vec(&request);
        wipe_json_value(&mut request);
        let mut bytes =
            serialized.map_err(|_| ApiError::worker_unavailable("worker_protocol_error"))?;
        if bytes.is_empty() || bytes.len() > MAX_REQUEST {
            bytes.zeroize();
            return Err(ApiError::worker_unavailable("worker_request_too_large"));
        }
        let mut framed = (bytes.len() as u32).to_be_bytes().to_vec();
        framed.extend_from_slice(&bytes);
        bytes.zeroize();
        let result = match self.worker.as_mut() {
            Some(worker) => worker.stdin.write_all(&framed).await,
            None => return Err(ApiError::worker_unavailable("worker_unavailable")),
        };
        framed.zeroize();
        result.map_err(|_| ApiError::worker_unavailable("worker_io_error"))
    }

    async fn read(&mut self, id: &str) -> Result<Packet, ApiError> {
        let worker = self
            .worker
            .as_mut()
            .ok_or_else(|| ApiError::worker_unavailable("worker_unavailable"))?;
        let mut header = [0u8; 4];
        worker
            .stdout
            .read_exact(&mut header)
            .await
            .map_err(|_| ApiError::worker_unavailable("worker_io_error"))?;
        let size = u32::from_be_bytes(header) as usize;
        if size == 0 || size > MAX_RESPONSE {
            return Err(ApiError::worker_unavailable("worker_protocol_error"));
        }
        let mut bytes = vec![0; size];
        let result = worker
            .stdout
            .read_exact(&mut bytes)
            .await
            .map_err(|_| ApiError::worker_unavailable("worker_io_error"));
        let parsed = result.and_then(|_| {
            serde_json::from_slice::<Value>(&bytes)
                .map_err(|_| ApiError::worker_unavailable("worker_protocol_error"))
        });
        bytes.zeroize();
        let packet = parsed?;
        if packet.get("v").and_then(Value::as_u64) != Some(1)
            || packet.get("id").and_then(Value::as_str) != Some(id)
        {
            return Err(ApiError::worker_unavailable("worker_protocol_error"));
        }
        if packet.get("event").is_some() {
            if packet["event"] != "auth.challenge" || packet["kind"] != "two_factor" {
                return Err(ApiError::worker_unavailable("worker_protocol_error"));
            }
            let challenge_id = packet["challenge_id"]
                .as_str()
                .filter(|s| !s.is_empty() && s.len() <= 64)
                .ok_or_else(|| ApiError::worker_unavailable("worker_protocol_error"))?;
            return Ok(Packet::Challenge {
                id: challenge_id.to_string(),
                kind: "two_factor".into(),
            });
        }
        match packet.get("ok").and_then(Value::as_bool) {
            Some(true) => packet
                .get("result")
                .filter(|v| v.is_object())
                .cloned()
                .map(Packet::Success)
                .ok_or_else(|| ApiError::worker_unavailable("worker_protocol_error")),
            Some(false) => {
                let error = packet
                    .get("error")
                    .ok_or_else(|| ApiError::worker_unavailable("worker_protocol_error"))?;
                let code = error
                    .get("code")
                    .and_then(Value::as_str)
                    .filter(|s| {
                        !s.is_empty()
                            && s.len() <= 64
                            && s.bytes().all(|b| b.is_ascii_lowercase() || b == b'_')
                    })
                    .ok_or_else(|| ApiError::worker_unavailable("worker_protocol_error"))?;
                let retryable = error
                    .get("retryable")
                    .and_then(Value::as_bool)
                    .ok_or_else(|| ApiError::worker_unavailable("worker_protocol_error"))?;
                let restart = error
                    .get("restart_required")
                    .and_then(Value::as_bool)
                    .ok_or_else(|| ApiError::worker_unavailable("worker_protocol_error"))?;
                if code == "authentication_failed" {
                    log_auth_diagnostic(error);
                }
                Ok(Packet::Error(ApiError::native(code, retryable, restart)))
            }
            None => Err(ApiError::worker_unavailable("worker_protocol_error")),
        }
    }

    async fn discard(&mut self) {
        if let Some(mut worker) = self.worker.take() {
            // Closing the response reader tells namespace PID 1 to exit. The
            // launcher waits for kernel teardown before we start a replacement.
            drop(worker.stdout);
            drop(worker.stdin);
            if timeout(Duration::from_secs(5), worker.child.wait())
                .await
                .is_err()
            {
                let _ = worker.child.kill().await;
            }
            tracing::info!(event = "worker.stopped");
        }
    }
}

#[cfg(test)]
mod auth_diagnostic_tests {
    use super::*;

    fn safe() -> Value {
        json!({
            "stage":"native_response","callback_failure":"none",
            "response_type":4,"apple_error_code":-42,
            "credential_callbacks":1,"dialog_callbacks":1,"unmatched_dialogs":0,
            "hsa2_requested":false,"known_dialog_selected":true
        })
    }

    #[test]
    fn only_explicitly_allowed_native_values_are_accepted() {
        let valid = safe();
        let parsed = parse_auth_diagnostic(&valid).unwrap();
        assert_eq!(parsed.apple_error_code, Some(-42));
        assert_eq!(parsed.response_type, Some(4));
        assert!(parsed.known_dialog_selected);
        let mut invalid = safe();
        invalid["stage"] = json!("attacker-controlled-text");
        assert!(parse_auth_diagnostic(&invalid).is_none());
        invalid = safe();
        invalid["callback_failure"] = json!("private-password");
        assert!(parse_auth_diagnostic(&invalid).is_none());
        invalid = safe();
        invalid["credential_callbacks"] = json!(1700);
        assert!(parse_auth_diagnostic(&invalid).is_none());
        invalid = safe();
        invalid["hsa2_requested"] = json!("true");
        assert!(parse_auth_diagnostic(&invalid).is_none());
        assert!(parse_auth_diagnostic(&json!({})).is_none());
    }
}
