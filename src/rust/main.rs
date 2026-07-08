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
            let worker_status = match worker.health() {
                Ok(r) => json!({"reachable": true, "status": r.http_status}),
                Err(e) => json!({"reachable": false, "status": 0, "error": e.to_string()}),
            };
            write_json(
                &mut stream,
                200,
                json!({
                    "status": "ok",
                    "version": VERSION,
                    "mode": "rust-supervisor",
                    "worker": worker_status,
                }),
            )?;
        }
        ("GET", "/me") => proxy_json(
            &mut stream,
            worker.request_json(protocol::OP_ME, Value::Null),
        )?,
        ("POST", "/login") => {
            let v = parse_json_body(&body)?;
            proxy_json(&mut stream, worker.request_json(protocol::OP_LOGIN, v))?;
        }
        ("POST", "/login/2fa") => {
            let v = parse_json_body(&body)?;
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
    stream.set_read_timeout(Some(Duration::from_secs(60)))?;
    stream.set_write_timeout(Some(Duration::from_secs(60)))?;
    loop {
        let mut n = [0u8; 1];
        if stream.read_exact(&mut n).is_err() {
            return Ok(());
        }
        let adam_len = n[0] as usize;
        if adam_len == 0 {
            return Ok(());
        }
        let mut adam = vec![0u8; adam_len];
        stream.read_exact(&mut adam)?;

        stream.read_exact(&mut n)?;
        let uri_len = n[0] as usize;
        if uri_len == 0 {
            return Ok(());
        }
        let mut uri = vec![0u8; uri_len];
        stream.read_exact(&mut uri)?;

        let adam = String::from_utf8_lossy(&adam).to_string();
        let uri = String::from_utf8_lossy(&uri).to_string();
        loop {
            let mut len = [0u8; 4];
            stream.read_exact(&mut len)?;
            let sample_len = u32::from_ne_bytes(len) as usize;
            if sample_len == 0 {
                break;
            }
            if sample_len > 64 * 1024 * 1024 {
                return Err(io::Error::new(
                    io::ErrorKind::InvalidData,
                    "sample too large",
                ));
            }
            let mut sample = vec![0u8; sample_len];
            stream.read_exact(&mut sample)?;
            let plaintext = match worker.decrypt_sample(&adam, &uri, sample) {
                Ok(p) if p.len() == sample_len => p,
                Ok(_) => {
                    return Err(io::Error::new(
                        io::ErrorKind::InvalidData,
                        "worker returned wrong sample length",
                    ))
                }
                Err(e) => {
                    let _ = stream.shutdown(Shutdown::Both);
                    return Err(worker_io_error(e));
                }
            };
            stream.write_all(&plaintext)?;
        }
    }
}
