mod protocol;
mod worker;

use std::collections::HashMap;
use std::env;
use std::io::{self, BufRead, BufReader, Read, Write};
use std::net::{Shutdown, TcpListener, TcpStream};
use std::sync::Arc;
use std::thread;
use std::time::Duration;

use serde_json::{json, Value};
use worker::{Worker, WorkerError};

const VERSION: &str = "0.0.1";
const DEFAULT_HTTP_HOST: &str = "0.0.0.0";
const DEFAULT_HTTP_PORT: u16 = 80;
const DEFAULT_DECRYPT_HOST: &str = "0.0.0.0";
const DEFAULT_DECRYPT_PORT: u16 = 10020;

fn main() {
    if let Err(e) = run() {
        eprintln!("wrapperd: fatal: {e}");
        std::process::exit(1);
    }
}

fn run() -> io::Result<()> {
    let http_host = env_or("WRAPPER_HOST", DEFAULT_HTTP_HOST);
    let http_port = env_u16("WRAPPER_PORT", DEFAULT_HTTP_PORT);
    let decrypt_host = env_or("WRAPPER_DECRYPT_HOST", DEFAULT_DECRYPT_HOST);
    let decrypt_port = env_u16("WRAPPER_DECRYPT_PORT", DEFAULT_DECRYPT_PORT);

    let worker = Arc::new(Worker::new("/app/wrapper", VERSION.to_string()));
    worker.ensure_started().map_err(worker_io_error)?;

    let tcp_worker = Arc::clone(&worker);
    let tcp_addr = format!("{decrypt_host}:{decrypt_port}");
    thread::spawn(move || {
        if let Err(e) = run_decrypt_tcp(&tcp_addr, tcp_worker) {
            eprintln!("wrapperd: decrypt tcp listener stopped: {e}");
            std::process::exit(1);
        }
    });

    let http_addr = format!("{http_host}:{http_port}");
    run_http(&http_addr, worker)
}

fn env_or(name: &str, fallback: &str) -> String {
    env::var(name)
        .ok()
        .filter(|v| !v.is_empty())
        .unwrap_or_else(|| fallback.to_string())
}

fn env_u16(name: &str, fallback: u16) -> u16 {
    env::var(name)
        .ok()
        .and_then(|v| v.parse().ok())
        .filter(|v| *v > 0)
        .unwrap_or(fallback)
}

fn worker_io_error(e: WorkerError) -> io::Error {
    io::Error::new(io::ErrorKind::Other, e.to_string())
}

fn run_http(addr: &str, worker: Arc<Worker>) -> io::Result<()> {
    let listener = TcpListener::bind(addr)?;
    eprintln!("wrapperd: {VERSION} HTTP listening on {addr}");
    for conn in listener.incoming() {
        match conn {
            Ok(stream) => {
                let worker = Arc::clone(&worker);
                thread::spawn(move || {
                    if let Err(e) = handle_http_connection(stream, worker) {
                        eprintln!("wrapperd: http connection error: {e}");
                    }
                });
            }
            Err(e) => eprintln!("wrapperd: http accept error: {e}"),
        }
    }
    Ok(())
}

fn handle_http_connection(mut stream: TcpStream, worker: Arc<Worker>) -> io::Result<()> {
    stream.set_read_timeout(Some(Duration::from_secs(70)))?;
    stream.set_write_timeout(Some(Duration::from_secs(70)))?;

    let mut reader = BufReader::new(stream.try_clone()?);
    let mut head = Vec::new();
    loop {
        let mut line = Vec::new();
        let n = reader.read_until(b'\n', &mut line)?;
        if n == 0 {
            return Ok(());
        }
        head.extend_from_slice(&line);
        if head.ends_with(b"\r\n\r\n") || head.ends_with(b"\n\n") {
            break;
        }
        if head.len() > 64 * 1024 {
            write_json(&mut stream, 431, json!({"error":"headers_too_large"}))?;
            return Ok(());
        }
    }

    let mut headers = [httparse::EMPTY_HEADER; 64];
    let mut req = httparse::Request::new(&mut headers);
    let parsed = req
        .parse(&head)
        .map_err(|e| io::Error::new(io::ErrorKind::InvalidData, e.to_string()))?;
    if !parsed.is_complete() {
        write_json(&mut stream, 400, json!({"error":"bad_request"}))?;
        return Ok(());
    }

    let method = req.method.unwrap_or("").to_string();
    let target = req.path.unwrap_or("/").to_string();
    let mut content_length = 0usize;
    let mut content_type = String::new();
    for h in req.headers.iter() {
        if h.name.eq_ignore_ascii_case("content-length") {
            content_length = std::str::from_utf8(h.value)
                .ok()
                .and_then(|v| v.trim().parse().ok())
                .unwrap_or(0);
        }
        if h.name.eq_ignore_ascii_case("content-type") {
            content_type = String::from_utf8_lossy(h.value).to_string();
        }
    }

    let mut body = vec![0u8; content_length];
    if content_length > 0 {
        reader.read_exact(&mut body)?;
    }

    let (path, query) = split_target(&target);
    eprintln!("http: {method} {target}");

    match (method.as_str(), path.as_str()) {
        ("GET", "/health") => {
            let params = parse_query(&query);
            let deep = params
                .get("deep")
                .map(|v| matches!(v.as_str(), "1" | "true" | "yes"))
                .unwrap_or(false);
            let worker_ipc = if deep {
                Some(match worker.health() {
                    Ok(r) => json!({"reachable": true, "status": r.http_status}),
                    Err(e) => json!({"reachable": false, "status": 0, "error": e.to_string()}),
                })
            } else {
                None
            };
            write_json(
                &mut stream,
                200,
                json!({
                    "status": "ok",
                    "version": VERSION,
                    "mode": "rust-supervisor",
                    "worker": worker.snapshot(),
                    "worker_ipc": worker_ipc,
                }),
            )?;
        }
        ("GET", "/me") => proxy_json(
            &mut stream,
            worker.request_json(protocol::OP_ME, Value::Null),
        )?,
        ("POST", "/login") => {
            let v = match parse_json_body(&body) {
                Ok(v) => v,
                Err(e) => {
                    write_json(
                        &mut stream,
                        400,
                        json!({"error":"invalid_json","detail":e.to_string()}),
                    )?;
                    return Ok(());
                }
            };
            proxy_json(&mut stream, worker.request_json(protocol::OP_LOGIN, v))?;
        }
        ("POST", "/login/2fa") => {
            let v = match parse_json_body(&body) {
                Ok(v) => v,
                Err(e) => {
                    write_json(
                        &mut stream,
                        400,
                        json!({"error":"invalid_json","detail":e.to_string()}),
                    )?;
                    return Ok(());
                }
            };
            proxy_json(&mut stream, worker.request_json(protocol::OP_LOGIN_2FA, v))?;
        }
        ("DELETE", "/login") => proxy_json(
            &mut stream,
            worker.request_json(protocol::OP_LOGOUT, Value::Null),
        )?,
        ("GET", "/playback") => {
            let params = parse_query(&query);
            let adam_id = params
                .get("adam_id")
                .or_else(|| params.get("adamId"))
                .cloned()
                .unwrap_or_default();
            proxy_json(
                &mut stream,
                worker.request_json(protocol::OP_PLAYBACK, json!({"adam_id": adam_id})),
            )?;
        }
        ("POST", "/decrypt") => {
            let _ = content_type;
            write_json(
                &mut stream,
                404,
                json!({
                    "error": "not_found",
                    "detail": "decrypt is available on the raw TCP port, not HTTP"
                }),
            )?;
        }
        _ => write_json(&mut stream, 404, json!({"error":"not_found"}))?,
    }
    Ok(())
}

fn split_target(target: &str) -> (String, String) {
    match target.split_once('?') {
        Some((p, q)) => (p.to_string(), q.to_string()),
        None => (target.to_string(), String::new()),
    }
}

fn parse_query(query: &str) -> HashMap<String, String> {
    let mut out = HashMap::new();
    for pair in query.split('&').filter(|p| !p.is_empty()) {
        let (k, v) = pair.split_once('=').unwrap_or((pair, ""));
        out.insert(percent_decode(k), percent_decode(v));
    }
    out
}

fn percent_decode(s: &str) -> String {
    let bytes = s.as_bytes();
    let mut out = Vec::with_capacity(bytes.len());
    let mut i = 0;
    while i < bytes.len() {
        if bytes[i] == b'%' && i + 2 < bytes.len() {
            if let (Some(a), Some(b)) = (hex(bytes[i + 1]), hex(bytes[i + 2])) {
                out.push((a << 4) | b);
                i += 3;
                continue;
            }
        }
        out.push(if bytes[i] == b'+' { b' ' } else { bytes[i] });
        i += 1;
    }
    String::from_utf8_lossy(&out).to_string()
}

fn hex(b: u8) -> Option<u8> {
    match b {
        b'0'..=b'9' => Some(b - b'0'),
        b'a'..=b'f' => Some(b - b'a' + 10),
        b'A'..=b'F' => Some(b - b'A' + 10),
        _ => None,
    }
}

fn parse_json_body(body: &[u8]) -> io::Result<Value> {
    serde_json::from_slice(body)
        .map_err(|e| io::Error::new(io::ErrorKind::InvalidData, e.to_string()))
}

fn proxy_json(
    stream: &mut TcpStream,
    result: Result<worker::WorkerResponse, WorkerError>,
) -> io::Result<()> {
    match result {
        Ok(r) => write_response(stream, r.http_status, &r.content_type, &r.body),
        Err(e) => write_json(
            stream,
            503,
            json!({"error":"worker_unavailable","detail":e.to_string()}),
        ),
    }
}

fn write_json(stream: &mut TcpStream, status: u16, body: Value) -> io::Result<()> {
    write_response(
        stream,
        status,
        "application/json",
        body.to_string().as_bytes(),
    )
}

fn write_response(
    stream: &mut TcpStream,
    status: u16,
    content_type: &str,
    body: &[u8],
) -> io::Result<()> {
    let reason = match status {
        200 => "OK",
        202 => "Accepted",
        400 => "Bad Request",
        401 => "Unauthorized",
        404 => "Not Found",
        409 => "Conflict",
        431 => "Request Header Fields Too Large",
        500 => "Internal Server Error",
        502 => "Bad Gateway",
        503 => "Service Unavailable",
        504 => "Gateway Timeout",
        _ => "OK",
    };
    write!(
        stream,
        "HTTP/1.1 {status} {reason}\r\ncontent-type: {content_type}\r\ncontent-length: {}\r\nconnection: close\r\n\r\n",
        body.len()
    )?;
    stream.write_all(body)
}

fn run_decrypt_tcp(addr: &str, worker: Arc<Worker>) -> io::Result<()> {
    let listener = TcpListener::bind(addr)?;
    eprintln!("wrapperd: {VERSION} TCP decrypt listening on {addr}");
    for conn in listener.incoming() {
        match conn {
            Ok(stream) => {
                let worker = Arc::clone(&worker);
                thread::spawn(move || {
                    if let Err(e) = handle_decrypt_client(stream, worker) {
                        eprintln!("wrapperd: decrypt client closed: {e}");
                    }
                });
            }
            Err(e) => eprintln!("wrapperd: decrypt accept error: {e}"),
        }
    }
    Ok(())
}

fn handle_decrypt_client(mut stream: TcpStream, worker: Arc<Worker>) -> io::Result<()> {
    stream.set_nodelay(true)?;
    stream.set_read_timeout(Some(Duration::from_secs(60)))?;
    stream.set_write_timeout(Some(Duration::from_secs(60)))?;
    loop {
        let frame = match protocol::read_decrypt_frame(&mut stream) {
            Ok(frame) => frame,
            Err(e) if e.kind() == io::ErrorKind::UnexpectedEof => return Ok(()),
            Err(e) => return Err(e),
        };
        if frame.kind == protocol::DECRYPT_KIND_CLOSE {
            return Ok(());
        }
        if frame.kind != protocol::DECRYPT_KIND_BATCH {
            write_decrypt_error(
                &mut stream,
                frame.request_id,
                "unsupported decrypt frame kind",
            )?;
            return Ok(());
        }
        let (adam, uri, samples) = match parse_decrypt_batch_payload(&frame.payload) {
            Ok(v) => v,
            Err(e) => {
                write_decrypt_error(&mut stream, frame.request_id, &e.to_string())?;
                return Ok(());
            }
        };
        let plaintexts = match worker.decrypt_batch(&adam, &uri, samples) {
            Ok(p) => p,
            Err(e) => {
                let _ = write_decrypt_error(&mut stream, frame.request_id, &e.to_string());
                let _ = stream.shutdown(Shutdown::Both);
                return Err(worker_io_error(e));
            }
        };
        let payload = build_decrypt_samples_payload(&plaintexts)?;
        protocol::write_decrypt_frame(
            &mut stream,
            &protocol::DecryptFrame {
                kind: protocol::DECRYPT_KIND_OK,
                request_id: frame.request_id,
                payload,
            },
        )?;
    }
}

fn write_decrypt_error(stream: &mut TcpStream, request_id: u32, message: &str) -> io::Result<()> {
    protocol::write_decrypt_frame(
        stream,
        &protocol::DecryptFrame {
            kind: protocol::DECRYPT_KIND_ERROR,
            request_id,
            payload: message.as_bytes().to_vec(),
        },
    )
}

fn parse_decrypt_batch_payload(body: &[u8]) -> io::Result<(String, String, Vec<Vec<u8>>)> {
    if body.len() < 8 {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "decrypt batch too short",
        ));
    }
    let adam_len = u16::from_be_bytes([body[0], body[1]]) as usize;
    let uri_len = u16::from_be_bytes([body[2], body[3]]) as usize;
    let sample_count = u32::from_be_bytes([body[4], body[5], body[6], body[7]]) as usize;
    if adam_len == 0 || uri_len == 0 || sample_count == 0 {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "empty decrypt batch field",
        ));
    }
    let table_end =
        8usize
            .checked_add(sample_count.checked_mul(4).ok_or_else(|| {
                io::Error::new(io::ErrorKind::InvalidData, "decrypt batch too large")
            })?)
            .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidData, "decrypt batch too large"))?;
    let fixed_end = table_end
        .checked_add(adam_len)
        .and_then(|n| n.checked_add(uri_len))
        .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidData, "decrypt batch too large"))?;
    if body.len() < fixed_end {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "truncated decrypt batch header",
        ));
    }
    let mut lengths = Vec::with_capacity(sample_count);
    for i in 0..sample_count {
        let off = 8 + i * 4;
        let len =
            u32::from_be_bytes([body[off], body[off + 1], body[off + 2], body[off + 3]]) as usize;
        if len == 0 || len > 64 * 1024 * 1024 {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "invalid decrypt sample length",
            ));
        }
        lengths.push(len);
    }
    let adam_start = table_end;
    let uri_start = adam_start + adam_len;
    let sample_start = uri_start + uri_len;
    let adam = String::from_utf8(body[adam_start..uri_start].to_vec())
        .map_err(|_| io::Error::new(io::ErrorKind::InvalidData, "adam_id is not utf-8"))?;
    let uri = String::from_utf8(body[uri_start..sample_start].to_vec())
        .map_err(|_| io::Error::new(io::ErrorKind::InvalidData, "uri is not utf-8"))?;
    let mut offset = sample_start;
    let mut samples = Vec::with_capacity(sample_count);
    for len in lengths {
        let end = offset
            .checked_add(len)
            .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidData, "decrypt batch too large"))?;
        if end > body.len() {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "truncated decrypt sample",
            ));
        }
        samples.push(body[offset..end].to_vec());
        offset = end;
    }
    if offset != body.len() {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "trailing decrypt batch bytes",
        ));
    }
    Ok((adam, uri, samples))
}

fn build_decrypt_samples_payload(samples: &[Vec<u8>]) -> io::Result<Vec<u8>> {
    if samples.len() > u32::MAX as usize {
        return Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            "too many decrypt samples",
        ));
    }
    let mut size = 4usize
        .checked_add(samples.len().checked_mul(4).ok_or_else(|| {
            io::Error::new(io::ErrorKind::InvalidInput, "decrypt response too large")
        })?)
        .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidInput, "decrypt response too large"))?;
    for sample in samples {
        if sample.len() > u32::MAX as usize {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                "decrypt sample too large",
            ));
        }
        size = size.checked_add(sample.len()).ok_or_else(|| {
            io::Error::new(io::ErrorKind::InvalidInput, "decrypt response too large")
        })?;
    }
    let mut out = Vec::with_capacity(size);
    out.extend_from_slice(&(samples.len() as u32).to_be_bytes());
    for sample in samples {
        out.extend_from_slice(&(sample.len() as u32).to_be_bytes());
    }
    for sample in samples {
        out.extend_from_slice(sample);
    }
    Ok(out)
}
