use std::fmt;
use std::io::{self, BufWriter, Read};
use std::os::fd::AsRawFd;
use std::process::{Child, ChildStdin, ChildStdout, Command, Stdio};
use std::sync::atomic::{AtomicU32, Ordering};
use std::sync::Mutex;
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
    stdin: BufWriter<ChildStdin>,
    stdout: ChildStdout,
}

pub struct Worker {
    launcher: String,
    version: String,
    request_timeout: Duration,
    next_id: AtomicU32,
    proc: Mutex<Option<WorkerProcess>>,
}

impl Worker {
    pub fn new(launcher: &str, version: String) -> Self {
        Self {
            launcher: launcher.to_string(),
            version,
            request_timeout: worker_timeout(),
            next_id: AtomicU32::new(1),
            proc: Mutex::new(None),
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
        }
        *guard = Some(self.spawn()?);
        Ok(())
    }

    pub fn health(&self) -> Result<WorkerResponse, WorkerError> {
        self.request_json(protocol::OP_HEALTH, Value::Null)
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
        let mut guard = self
            .proc
            .lock()
            .map_err(|_| WorkerError::Unavailable("worker mutex poisoned".to_string()))?;
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
        if let Err(e) = protocol::write_frame(&mut proc.stdin, &req) {
            *guard = None;
            return Err(WorkerError::Io(e.to_string()));
        }
        let resp = match read_frame_timeout(&mut proc.stdout, self.request_timeout) {
            Ok(frame) => frame,
            Err(e) => {
                if e.kind() == io::ErrorKind::TimedOut {
                    eprintln!(
                        "wrapperd: worker request opcode={opcode} timed out after {:?}; restarting worker",
                        self.request_timeout
                    );
                    kill_worker(proc);
                }
                *guard = None;
                return Err(WorkerError::Io(e.to_string()));
            }
        };
        if resp.kind != protocol::KIND_RESPONSE || resp.request_id != id || resp.opcode != opcode {
            kill_worker(proc);
            *guard = None;
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
        let _ = &self.version;
        Ok(WorkerProcess {
            child,
            stdin: BufWriter::new(stdin),
            stdout,
        })
    }

    fn restart_after_delay(&self) {
        let mut guard = match self.proc.lock() {
            Ok(g) => g,
            Err(_) => return,
        };
        if let Some(mut p) = guard.take() {
            let _ = p.child.kill();
            let _ = p.child.wait();
        }
        thread::sleep(Duration::from_secs(1));
        match self.spawn() {
            Ok(p) => *guard = Some(p),
            Err(e) => eprintln!("wrapperd: worker restart failed: {e}"),
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

fn kill_worker(proc: &mut WorkerProcess) {
    let _ = proc.child.kill();
    let _ = proc.child.wait();
}

fn read_frame_timeout(stdout: &mut ChildStdout, timeout: Duration) -> io::Result<protocol::Frame> {
    let deadline = Instant::now() + timeout;
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
            Err(e) => return Err(e),
        }
    }
    Ok(())
}

fn wait_readable(fd: i32, deadline: Instant) -> io::Result<()> {
    let mut pfd = libc::pollfd {
        fd,
        events: libc::POLLIN,
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
                return Err(io::Error::new(
                    io::ErrorKind::UnexpectedEof,
                    "worker stdout closed",
                ));
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
