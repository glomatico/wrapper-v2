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
    time::{timeout, timeout_at, Instant},
};
use zeroize::{Zeroize, Zeroizing};

use crate::{
    config::{Config, PROFILE},
    error::ApiError,
};

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
const LOGIN_LIMIT: Duration = Duration::from_secs(610);

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
            })),
            pending: StdMutex::new(None),
        })
    }

    pub async fn call(&self, op: &str, params: Value) -> Result<Value, ApiError> {
        let mut control = self.control.try_lock().map_err(|_| busy())?;
        let result = control.command(&self.settings, op, params).await;
        match result {
            Ok(Packet::Success(value)) => {
                if op == "session.close" {
                    control.discard().await;
                }
                Ok(value)
            }
            Ok(Packet::Error(error)) => {
                if error.restart_required {
                    control.discard().await;
                }
                Err(error)
            }
            Ok(Packet::Challenge { .. }) => {
                control.discard().await;
                Err(ApiError::worker_unavailable("unexpected_worker_event"))
            }
            Err(error) => {
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
        timeout(LOGIN_LIMIT, received)
            .await
            .map_err(|_| ApiError::worker_unavailable("challenge_expired"))?
            .map_err(|_| ApiError::worker_unavailable("login_worker_closed"))?
    }

    async fn login_task(
        self: Arc<Self>,
        mut control: OwnedMutexGuard<Controller>,
        username: Zeroizing<String>,
        password: Zeroizing<String>,
        first: LoginDelivery,
    ) {
        let deadline = Instant::now() + LOGIN_LIMIT;
        let mut reply = Some(first);
        let mut terminal_session = None;
        let result: Result<(), ApiError> = async {
            control.ensure(&self.settings).await?;
            let id = control.allocate_id();
            let params = json!({"username":username.as_str(),"password":password.as_str()});
            timeout_at(deadline, control.send(&id, "session.login", params, 600000))
                .await
                .map_err(|_| ApiError::worker_unavailable("worker_timeout"))??;
            // The native worker is synchronous. Responses may include 2FA
            // challenges before the final session.login response.
            loop {
                let packet = timeout_at(deadline, control.read(&id))
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
                        let (code, continuation) = timeout_at(deadline, rx)
                            .await
                            .map_err(|_| ApiError::worker_unavailable("challenge_expired"))?
                            .map_err(|_| ApiError::worker_unavailable("challenge_expired"))?;
                        reply = Some(continuation);
                        timeout_at(
                            deadline,
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

        if result.is_err() {
            control.discard().await;
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
                Ok(()) => Ok(LoginReply::Finished(
                    terminal_session.expect("successful login has a terminal response"),
                )),
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
        });
        let outcome = timeout(COMMAND_LIMIT, async {
            self.send(&id, "initialize", params, 120000).await?;
            self.read(&id).await
        })
        .await
        .map_err(|_| ApiError::worker_unavailable("worker_timeout"))?;
        match outcome? {
            Packet::Success(result)
                if result.get("profile").and_then(Value::as_str) == Some(PROFILE)
                    && result.get("protocol").and_then(Value::as_u64) == Some(1) =>
            {
                Ok(())
            }
            Packet::Error(error) => Err(error),
            _ => Err(ApiError::worker_unavailable("unsupported_worker_profile")),
        }
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
                Ok(Packet::Error(ApiError::native(code, retryable, restart)))
            }
            None => Err(ApiError::worker_unavailable("worker_protocol_error")),
        }
    }

    async fn discard(&mut self) {
        if let Some(mut worker) = self.worker.take() {
            let _ = worker.child.kill().await;
            let _ = worker.child.wait().await;
        }
    }
}
