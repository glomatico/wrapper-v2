use std::fmt;
use std::io::{self, Read, Write};
use std::os::fd::AsRawFd;
use std::process::{Child, ChildStdin, ChildStdout, Command, Stdio};
use std::sync::atomic::{AtomicU32, Ordering};
use std::sync::{Mutex, MutexGuard, TryLockError};
use std::thread;
use std::time::{Duration, Instant};

use serde_json::{json, Value};

use crate::protocol;

#[derive(Debug)]
pub enum WorkerError {
    Io(String),
    Protocol(String),
    Unavailable(String),
}

impl fmt::Display for WorkerError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            WorkerError::Io(e) | WorkerError::Protocol(e) | WorkerError::Unavailable(e) => {
                f.write_str(e)
            }
        }
    }
}

pub struct WorkerResponse {
    pub http_status: u16,
    pub content_type: String,
    pub body: Vec<u8>,
    pub restart_worker: bool,
}

struct WorkerProcess {
    child: Child,
    stdin: ChildStdin,
    stdout: ChildStdout,
}

pub struct Worker {
    launcher: String,
    version: String,
    request_timeout: Duration,
    next_id: AtomicU32,
    restart_count: AtomicU32,
    timeout_count: AtomicU32,
    waiting_count: AtomicU32,
    pid: AtomicU32,
    proc: Mutex<Option<WorkerProcess>>,
    state: Mutex<WorkerState>,
}

struct WorkerState {
    current: Option<CurrentRequest>,
    last_error: Option<String>,
    last_restart_reason: Option<String>,
}

#[derive(Clone)]
struct CurrentRequest {
    id: u32,
    opcode: u16,
    started: Instant,
}

struct RequestTracker<'a> {
    worker: &'a Worker,
    id: u32,
}

struct WaitTracker<'a> {
    worker: &'a Worker,
}

impl Worker {
    pub fn new(launcher: &str, version: String) -> Self {
        Self {
            launcher: launcher.to_string(),
            version,
            request_timeout: worker_timeout(),
            next_id: AtomicU32::new(1),
            restart_count: AtomicU32::new(0),
            timeout_count: AtomicU32::new(0),
            waiting_count: AtomicU32::new(0),
            pid: AtomicU32::new(0),
            proc: Mutex::new(None),
            state: Mutex::new(WorkerState {
                current: None,
                last_error: None,
                last_restart_reason: None,
            }),
        }
    }

    pub fn ensure_started(&self) -> Result<(), WorkerError> {
        let mut guard = self
            .proc
            .lock()
            .map_err(|_| WorkerError::Unavailable("worker mutex poisoned".to_string()))?;
        if let Some(p) = guard.as_mut() {
            if p.child.try_wait().map_err(io_err)?.is_none() {
                return Ok(());
            }
            self.pid.store(0, Ordering::Relaxed);
        }
        *guard = Some(self.spawn()?);
        Ok(())
    }

    pub fn health(&self) -> Result<WorkerResponse, WorkerError> {
        self.request_json(protocol::OP_HEALTH, Value::Null)
    }

    pub fn snapshot(&self) -> Value {
        let current = self.state.lock().ok().and_then(|s| s.current.clone());
        let (last_error, last_restart_reason) = self
            .state
            .lock()
            .map(|s| (s.last_error.clone(), s.last_restart_reason.clone()))
            .unwrap_or((None, None));
        let pid = match self.pid.load(Ordering::Relaxed) {
            0 => None,
            pid => Some(pid),
        };
        json!({
            "pid": pid,
            "request_timeout_secs": self.request_timeout.as_secs(),
            "restart_count": self.restart_count.load(Ordering::Relaxed),
            "timeout_count": self.timeout_count.load(Ordering::Relaxed),
            "waiting_count": self.waiting_count.load(Ordering::Relaxed),
            "current_request": current.map(|r| json!({
                "id": r.id,
                "opcode": r.opcode,
                "elapsed_ms": r.started.elapsed().as_millis(),
            })),
            "last_error": last_error,
            "last_restart_reason": last_restart_reason,
        })
    }

    pub fn request_json(&self, opcode: u16, payload: Value) -> Result<WorkerResponse, WorkerError> {
        let bytes = if payload.is_null() {
            Vec::new()
        } else {
            serde_json::to_vec(&payload).map_err(|e| WorkerError::Protocol(e.to_string()))?
        };
        let frame = self.request(opcode, bytes)?;
        parse_worker_response(frame)
    }

    pub fn decrypt_batch(
        &self,
        adam: &str,
        uri: &str,
        samples: Vec<Vec<u8>>,
    ) -> Result<Vec<Vec<u8>>, WorkerError> {
        let payload = protocol::decrypt_batch_payload(adam, uri, &samples)
            .map_err(|e| WorkerError::Protocol(e.to_string()))?;
        let frame = self.request(protocol::OP_DECRYPT_BATCH, payload)?;
        if frame.flags & 1 == 1 {
            return protocol::parse_decrypt_samples_payload(&frame.payload)
                .map_err(|e| WorkerError::Protocol(e.to_string()));
        }
        let r = parse_worker_response(frame)?;
        if r.restart_worker {
            self.restart_after_delay();
        }
        Err(WorkerError::Unavailable(
            String::from_utf8_lossy(&r.body).to_string(),
        ))
    }

    fn request(&self, opcode: u16, payload: Vec<u8>) -> Result<protocol::Frame, WorkerError> {
        let id = self.next_id.fetch_add(1, Ordering::Relaxed);
        let deadline = Instant::now() + self.request_timeout;
        let wait_tracker = self.track_wait();
        let mut guard = match lock_worker_timeout(&self.proc, deadline) {
            Ok(g) => g,
            Err(e) => {
                if e.to_string().contains("timed out") {
                    eprintln!(
                        "wrapperd: worker request opcode={opcode} timed out waiting for worker after {:?}",
                        self.request_timeout
                    );
                    self.timeout_count.fetch_add(1, Ordering::Relaxed);
                    self.recover_stuck_worker_or_exit("lock wait timeout");
                }
                self.record_error(e.to_string());
                return Err(e);
            }
        };
        drop(wait_tracker);
        let _tracker = self.track_request(id, opcode);
        if guard.is_none() {
            *guard = Some(self.spawn()?);
        }
        let proc = guard
            .as_mut()
            .ok_or_else(|| WorkerError::Unavailable("worker missing".to_string()))?;
        if proc.child.try_wait().map_err(io_err)?.is_some() {
            *guard = Some(self.spawn()?);
        }
        let proc = guard
            .as_mut()
            .ok_or_else(|| WorkerError::Unavailable("worker missing".to_string()))?;
        let req = protocol::Frame {
            kind: protocol::KIND_REQUEST,
            request_id: id,
            opcode,
            flags: 0,
            payload,
        };
        if let Err(e) = write_frame_timeout(&mut proc.stdin, &req, deadline) {
            if e.kind() == io::ErrorKind::TimedOut {
                eprintln!(
                    "wrapperd: worker request opcode={opcode} timed out while writing after {:?}; restarting worker",
                    self.request_timeout
                );
                self.timeout_count.fetch_add(1, Ordering::Relaxed);
                self.abandon_locked_worker(&mut guard, "write timeout");
            } else {
                self.abandon_locked_worker(&mut guard, "ipc write error");
            }
            self.record_error(e.to_string());
            return Err(WorkerError::Io(e.to_string()));
        }
        let resp = match read_frame_timeout(&mut proc.stdout, deadline) {
            Ok(frame) => frame,
            Err(e) => {
                if e.kind() == io::ErrorKind::TimedOut {
                    eprintln!(
                        "wrapperd: worker request opcode={opcode} timed out after {:?}; restarting worker",
                        self.request_timeout
                    );
                    self.timeout_count.fetch_add(1, Ordering::Relaxed);
                    self.abandon_locked_worker(&mut guard, "response timeout");
                } else {
                    self.abandon_locked_worker(&mut guard, "ipc read error");
                }
                self.record_error(e.to_string());
                return Err(WorkerError::Io(e.to_string()));
            }
        };
        if resp.kind != protocol::KIND_RESPONSE || resp.request_id != id || resp.opcode != opcode {
            self.abandon_locked_worker(&mut guard, "mismatched ipc response");
            self.record_error("mismatched ipc response");
            return Err(WorkerError::Protocol("mismatched ipc response".to_string()));
        }
        Ok(resp)
    }

    fn spawn(&self) -> Result<WorkerProcess, WorkerError> {
        eprintln!("wrapperd: starting ipc worker {}", self.launcher);
        let mut child = Command::new(&self.launcher)
            .env("WRAPPER_MODE", "ipc-worker")
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::inherit())
            .spawn()
            .map_err(io_err)?;
        let stdin = child
            .stdin
            .take()
            .ok_or_else(|| WorkerError::Io("worker stdin unavailable".to_string()))?;
        let stdout = child
            .stdout
            .take()
            .ok_or_else(|| WorkerError::Io("worker stdout unavailable".to_string()))?;
        set_nonblocking(&stdin)?;
        set_nonblocking(&stdout)?;
        self.pid.store(child.id(), Ordering::Relaxed);
        let _ = &self.version;
        Ok(WorkerProcess {
            child,
            stdin,
            stdout,
        })
    }

    fn restart_after_delay(&self) {
        let old = {
            let mut guard = match self.proc.lock() {
                Ok(g) => g,
                Err(_) => return,
            };
            guard.take()
        };
        self.pid.store(0, Ordering::Relaxed);
        cleanup_worker(old, "restart requested by worker response");
        self.record_restart("restart requested by worker response");
        thread::sleep(Duration::from_secs(1));
        let mut guard = match self.proc.lock() {
            Ok(g) => g,
            Err(_) => return,
        };
        if guard.is_some() {
            return;
        }
        match self.spawn() {
            Ok(p) => *guard = Some(p),
            Err(e) => eprintln!("wrapperd: worker restart failed: {e}"),
        }
    }

    fn track_wait(&self) -> WaitTracker<'_> {
        self.waiting_count.fetch_add(1, Ordering::Relaxed);
        WaitTracker { worker: self }
    }

    fn track_request(&self, id: u32, opcode: u16) -> RequestTracker<'_> {
        if let Ok(mut state) = self.state.lock() {
            state.current = Some(CurrentRequest {
                id,
                opcode,
                started: Instant::now(),
            });
        }
        RequestTracker { worker: self, id }
    }

    fn record_error(&self, error: impl Into<String>) {
        if let Ok(mut state) = self.state.lock() {
            state.last_error = Some(error.into());
        }
    }

    fn record_restart(&self, reason: impl Into<String>) {
        self.restart_count.fetch_add(1, Ordering::Relaxed);
        if let Ok(mut state) = self.state.lock() {
            state.last_restart_reason = Some(reason.into());
        }
    }

    fn abandon_locked_worker(
        &self,
        guard: &mut MutexGuard<'_, Option<WorkerProcess>>,
        reason: &'static str,
    ) {
        let old = guard.take();
        self.pid.store(0, Ordering::Relaxed);
        cleanup_worker(old, reason);
        self.record_restart(reason);
    }

    fn recover_stuck_worker_or_exit(&self, reason: &'static str) {
        let stuck = self
            .state
            .lock()
            .ok()
            .and_then(|s| s.current.clone())
            .map(|r| r.started.elapsed() >= self.request_timeout)
            .unwrap_or(false);
        if !stuck {
            return;
        }
        let pid = self.pid.swap(0, Ordering::Relaxed);
        if pid == 0 {
            eprintln!(
                "wrapperd: fatal: worker request is stale, but no worker pid is available; exiting for container restart"
            );
            std::process::exit(70);
        }
        self.record_restart(reason);
        thread::spawn(move || {
            eprintln!("wrapperd: killing stuck worker pid={pid}: {reason}");
            unsafe {
                libc::kill(pid as libc::pid_t, libc::SIGKILL);
            }
        });
    }
}

fn set_nonblocking<T: AsRawFd>(fd: &T) -> Result<(), WorkerError> {
    let raw = fd.as_raw_fd();
    let flags = unsafe { libc::fcntl(raw, libc::F_GETFL) };
    if flags < 0 {
        return Err(io_err(io::Error::last_os_error()));
    }
    let rc = unsafe { libc::fcntl(raw, libc::F_SETFL, flags | libc::O_NONBLOCK) };
    if rc < 0 {
        return Err(io_err(io::Error::last_os_error()));
    }
    Ok(())
}

impl Drop for WaitTracker<'_> {
    fn drop(&mut self) {
        self.worker.waiting_count.fetch_sub(1, Ordering::Relaxed);
    }
}

impl Drop for RequestTracker<'_> {
    fn drop(&mut self) {
        if let Ok(mut state) = self.worker.state.lock() {
            if state.current.as_ref().map(|r| r.id) == Some(self.id) {
                state.current = None;
            }
        }
    }
}

fn worker_timeout() -> Duration {
    std::env::var("WRAPPER_WORKER_TIMEOUT_SECS")
        .ok()
        .and_then(|v| v.parse::<u64>().ok())
        .filter(|v| *v > 0)
        .map(Duration::from_secs)
        .unwrap_or_else(|| Duration::from_secs(60))
}

fn io_err(e: io::Error) -> WorkerError {
    WorkerError::Io(e.to_string())
}

fn lock_worker_timeout(
    proc: &Mutex<Option<WorkerProcess>>,
    deadline: Instant,
) -> Result<MutexGuard<'_, Option<WorkerProcess>>, WorkerError> {
    loop {
        match proc.try_lock() {
            Ok(g) => return Ok(g),
            Err(TryLockError::Poisoned(_)) => {
                return Err(WorkerError::Unavailable(
                    "worker mutex poisoned".to_string(),
                ));
            }
            Err(TryLockError::WouldBlock) => {
                if Instant::now() >= deadline {
                    return Err(WorkerError::Unavailable(
                        "worker busy timed out".to_string(),
                    ));
                }
                thread::sleep(Duration::from_millis(10));
            }
        }
    }
}

fn cleanup_worker(proc: Option<WorkerProcess>, reason: &'static str) {
    if let Some(mut proc) = proc {
        thread::spawn(move || {
            eprintln!("wrapperd: cleaning up old worker: {reason}");
            let _ = proc.child.kill();
            let _ = proc.child.wait();
        });
    }
}

fn write_frame_timeout(
    stdin: &mut ChildStdin,
    frame: &protocol::Frame,
    deadline: Instant,
) -> io::Result<()> {
    if frame.payload.len() > u32::MAX as usize {
        return Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            "payload too large",
        ));
    }
    let mut bytes = Vec::with_capacity(20 + frame.payload.len());
    bytes.extend_from_slice(&protocol::MAGIC.to_be_bytes());
    bytes.extend_from_slice(&protocol::VERSION.to_be_bytes());
    bytes.extend_from_slice(&frame.kind.to_be_bytes());
    bytes.extend_from_slice(&frame.request_id.to_be_bytes());
    bytes.extend_from_slice(&frame.opcode.to_be_bytes());
    bytes.extend_from_slice(&frame.flags.to_be_bytes());
    bytes.extend_from_slice(&(frame.payload.len() as u32).to_be_bytes());
    bytes.extend_from_slice(&frame.payload);
    write_all_timeout(stdin, &bytes, deadline)?;
    stdin.flush()
}

fn read_frame_timeout(stdout: &mut ChildStdout, deadline: Instant) -> io::Result<protocol::Frame> {
    let mut h = [0u8; 20];
    read_exact_timeout(stdout, &mut h, deadline)?;
    let magic = u32::from_be_bytes([h[0], h[1], h[2], h[3]]);
    let version = u16::from_be_bytes([h[4], h[5]]);
    if magic != protocol::MAGIC {
        return Err(io::Error::new(io::ErrorKind::InvalidData, "bad ipc magic"));
    }
    if version != protocol::VERSION {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "bad ipc version",
        ));
    }
    let kind = u16::from_be_bytes([h[6], h[7]]);
    let request_id = u32::from_be_bytes([h[8], h[9], h[10], h[11]]);
    let opcode = u16::from_be_bytes([h[12], h[13]]);
    let flags = u16::from_be_bytes([h[14], h[15]]);
    let payload_len = u32::from_be_bytes([h[16], h[17], h[18], h[19]]) as usize;
    let mut payload = vec![0u8; payload_len];
    read_exact_timeout(stdout, &mut payload, deadline)?;
    Ok(protocol::Frame {
        kind,
        request_id,
        opcode,
        flags,
        payload,
    })
}

fn write_all_timeout<W: Write + AsRawFd>(
    writer: &mut W,
    mut buf: &[u8],
    deadline: Instant,
) -> io::Result<()> {
    while !buf.is_empty() {
        wait_writable(writer.as_raw_fd(), deadline)?;
        match writer.write(buf) {
            Ok(0) => {
                return Err(io::Error::new(
                    io::ErrorKind::WriteZero,
                    "worker stdin closed",
                ))
            }
            Ok(n) => buf = &buf[n..],
            Err(e) if e.kind() == io::ErrorKind::Interrupted => continue,
            Err(e) if e.kind() == io::ErrorKind::WouldBlock => continue,
            Err(e) => return Err(e),
        }
    }
    Ok(())
}

fn read_exact_timeout<R: Read + AsRawFd>(
    reader: &mut R,
    mut buf: &mut [u8],
    deadline: Instant,
) -> io::Result<()> {
    while !buf.is_empty() {
        wait_readable(reader.as_raw_fd(), deadline)?;
        match reader.read(buf) {
            Ok(0) => {
                return Err(io::Error::new(
                    io::ErrorKind::UnexpectedEof,
                    "worker stdout closed",
                ))
            }
            Ok(n) => {
                let tmp = buf;
                buf = &mut tmp[n..];
            }
            Err(e) if e.kind() == io::ErrorKind::Interrupted => continue,
            Err(e) if e.kind() == io::ErrorKind::WouldBlock => continue,
            Err(e) => return Err(e),
        }
    }
    Ok(())
}

fn wait_readable(fd: i32, deadline: Instant) -> io::Result<()> {
    wait_fd(fd, libc::POLLIN, "worker stdout closed", deadline)
}

fn wait_writable(fd: i32, deadline: Instant) -> io::Result<()> {
    wait_fd(fd, libc::POLLOUT, "worker stdin closed", deadline)
}

fn wait_fd(fd: i32, events: i16, closed_message: &str, deadline: Instant) -> io::Result<()> {
    let mut pfd = libc::pollfd {
        fd,
        events,
        revents: 0,
    };
    loop {
        let now = Instant::now();
        if now >= deadline {
            return Err(io::Error::new(
                io::ErrorKind::TimedOut,
                "worker ipc response timed out",
            ));
        }
        let remaining = deadline.saturating_duration_since(now);
        let timeout_ms = remaining.as_millis().min(i32::MAX as u128) as i32;
        pfd.revents = 0;
        let n = unsafe { libc::poll(&mut pfd, 1, timeout_ms) };
        if n > 0 {
            if pfd.revents & (libc::POLLERR | libc::POLLHUP | libc::POLLNVAL) != 0 {
                return Err(io::Error::new(io::ErrorKind::UnexpectedEof, closed_message));
            }
            return Ok(());
        }
        if n == 0 {
            return Err(io::Error::new(
                io::ErrorKind::TimedOut,
                "worker ipc response timed out",
            ));
        }
        let e = io::Error::last_os_error();
        if e.kind() != io::ErrorKind::Interrupted {
            return Err(e);
        }
        if Instant::now() >= deadline {
            return Err(io::Error::new(
                io::ErrorKind::TimedOut,
                "worker ipc response timed out",
            ));
        }
    }
}

fn parse_worker_response(frame: protocol::Frame) -> Result<WorkerResponse, WorkerError> {
    let v: Value =
        serde_json::from_slice(&frame.payload).map_err(|e| WorkerError::Protocol(e.to_string()))?;
    let http_status = v.get("http_status").and_then(|v| v.as_u64()).unwrap_or(502) as u16;
    let content_type = v
        .get("content_type")
        .and_then(|v| v.as_str())
        .unwrap_or("application/json")
        .to_string();
    let restart_worker = v
        .get("restart_worker")
        .and_then(|v| v.as_bool())
        .unwrap_or(false);
    let body = match v.get("body") {
        Some(Value::String(s)) => s.as_bytes().to_vec(),
        Some(other) => serde_json::to_vec(other).unwrap_or_else(|_| {
            json!({"error":"invalid_worker_body"})
                .to_string()
                .into_bytes()
        }),
        None => Vec::new(),
    };
    Ok(WorkerResponse {
        http_status,
        content_type,
        body,
        restart_worker,
    })
}
