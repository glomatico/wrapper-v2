//! Unversioned HTTP facade for Wrapper-v2 1.0.0; native IPC uses protocol v1.
use std::{
    collections::HashMap,
    convert::Infallible,
    sync::{
        atomic::{AtomicU64, Ordering},
        Arc,
    },
    time::{Duration, Instant},
};

use bytes::Bytes;
use form_urlencoded::parse;
use http_body_util::{BodyExt, Full};
use hyper::{body::Incoming, header, Method, Request, Response, StatusCode};
use serde_json::{json, Value};
use tokio::time::timeout;
use zeroize::{Zeroize, Zeroizing};

use crate::{
    config::Config,
    error::ApiError,
    profile::RuntimeProfile,
    worker::{wipe_json_value, LoginReply, Supervisor},
};

const BODY_LIMIT: usize = 16 * 1024;
const QUERY_LIMIT: usize = 8 * 1024;
type HttpResponse = Response<Full<Bytes>>;

pub struct Api {
    supervisor: Arc<Supervisor>,
    profile: RuntimeProfile,
    next_request_id: AtomicU64,
}

impl Api {
    pub fn new(config: &Config) -> Arc<Self> {
        Arc::new(Self {
            supervisor: Supervisor::new(config.clone()),
            profile: config.profile.clone(),
            next_request_id: AtomicU64::new(1),
        })
    }

    pub async fn serve(
        self: Arc<Self>,
        request: Request<Incoming>,
    ) -> Result<HttpResponse, Infallible> {
        let request_id = self.next_request_id.fetch_add(1, Ordering::Relaxed);
        // Only log *known*, literal routes. Raw paths, query parameters, HTTP
        // headers, and bodies can contain signed URLs, tokens or key URIs.
        let path = log_route(request.uri().path());
        let method = if request.method() == Method::GET {
            "GET"
        } else if request.method() == Method::POST {
            "POST"
        } else {
            "OTHER"
        };
        let started = Instant::now();
        let (status, payload, error_code) = match self.handle(request).await {
            Ok((status, payload)) => (status, payload, None),
            Err(e) => {
                let status = e.status;
                let code = e.code.clone();
                (status, e.json(), Some(code))
            }
        };
        let duration_ms = started.elapsed().as_millis().min(u64::MAX as u128) as u64;
        let status_code = status.as_u16();
        if status.is_server_error() {
            tracing::error!(
                event = "http.request",
                request_id,
                method,
                path,
                status = status_code,
                duration_ms,
                code = error_code.as_deref().unwrap_or("internal_error")
            );
        } else if status.is_client_error() {
            tracing::warn!(
                event = "http.request",
                request_id,
                method,
                path,
                status = status_code,
                duration_ms,
                code = error_code.as_deref().unwrap_or("client_error")
            );
        } else if path == "/health" {
            // Successful health probes are very noisy; RUST_LOG=debug enables them.
            tracing::debug!(
                event = "http.request",
                request_id,
                method,
                path,
                status = status_code,
                duration_ms
            );
        } else {
            tracing::info!(
                event = "http.request",
                request_id,
                method,
                path,
                status = status_code,
                duration_ms
            );
        }
        Ok(answer(status, payload))
    }

    async fn handle(&self, request: Request<Incoming>) -> Result<(StatusCode, Value), ApiError> {
        let (parts, body) = request.into_parts();
        let method = parts.method;
        let path = parts.uri.path();
        if path == "/health" && method == Method::GET {
            return Ok((
                StatusCode::OK,
                json!({"status":"ok","version":env!("CARGO_PKG_VERSION")}),
            ));
        }
        if parts.uri.to_string().len() > QUERY_LIMIT {
            return Err(ApiError::bad_request("request_target_too_large"));
        }
        let query = parse_query(parts.uri.query().unwrap_or(""))?;
        match (method, path) {
            (Method::GET, "/info") => Ok((
                StatusCode::OK,
                json!({
                    "version":env!("CARGO_PKG_VERSION"), "protocol_version":1,
                    "context_format":"temari", "profile":self.profile.profile,
                    "architecture":self.profile.architecture,
                    "linux_architecture":self.profile.linux_architecture,
                    "context_supported":self.profile.context_supported,
                    "features":if self.profile.context_supported {
                        vec!["session","two_factor","credentials","playback","context"]
                    } else { vec!["session","two_factor","credentials","playback"] },
                    "decryption":"client_side"
                }),
            )),
            (Method::GET, "/session") => self.result("session.status", json!({})).await,
            (Method::GET, "/session/credentials") => {
                self.result("session.credentials", json!({})).await
            }
            (Method::POST, "/session/restore") => {
                empty_body(body).await?;
                self.result("session.restore", json!({})).await
            }
            (Method::POST, "/session/close") => {
                empty_body(body).await?;
                self.result("session.close", json!({})).await
            }
            (Method::POST, "/session/login") => {
                let mut raw = read_body(body).await?;
                let parsed = serde_json::from_slice::<Value>(&raw)
                    .map_err(|_| ApiError::bad_request("invalid_json"));
                raw.zeroize();
                let mut parsed = parsed?;
                let username = field(&parsed, "username", 320, false)
                    .map(|value| Zeroizing::new(value.to_string()));
                let password = field(&parsed, "password", 4096, false)
                    .map(|value| Zeroizing::new(value.to_string()));
                wipe_json_value(&mut parsed);
                let username = username?;
                let password = password?;
                // Never log username, password or a request body.
                login_response(self.supervisor.login(username, password).await?)
            }
            (Method::POST, "/session/challenge") => {
                let mut raw = read_body(body).await?;
                let parsed = serde_json::from_slice::<Value>(&raw)
                    .map_err(|_| ApiError::bad_request("invalid_json"));
                raw.zeroize();
                let mut parsed = parsed?;
                let id = field(&parsed, "challenge_id", 64, false).map(str::to_string);
                let code =
                    field(&parsed, "code", 6, true).map(|value| Zeroizing::new(value.to_string()));
                wipe_json_value(&mut parsed);
                let id = id?;
                let code = code?;
                if !code.is_empty()
                    && (code.len() != 6 || !code.bytes().all(|b| b.is_ascii_digit()))
                {
                    return Err(ApiError::bad_request("invalid_auth_code"));
                }
                login_response(self.supervisor.challenge(&id, code).await?)
            }
            (Method::GET, "/playback") => {
                let adam_id = get_param(&query, "adam_id")?;
                validate_adam(adam_id)?;
                self.result("playback.get", json!({"adam_id":adam_id}))
                    .await
            }
            (Method::GET, "/context") | (Method::GET, "/key") => {
                let alias = path == "/key";
                let adam_id = get_param(&query, if alias { "adamId" } else { "adam_id" })?;
                let key_uri = get_param(&query, if alias { "uri" } else { "key_uri" })?;
                validate_adam(adam_id)?;
                validate_key_uri(key_uri)?;
                let mut worker_result = self
                    .supervisor
                    .call("context.get", json!({"adam_id":adam_id,"key_uri":key_uri}))
                    .await?;
                let mut context = worker_result
                    .as_object_mut()
                    .and_then(|map| map.remove("context"))
                    .ok_or_else(|| ApiError::worker_unavailable("invalid_context"))?;
                if !self.profile.validate_context(&context) {
                    wipe_json_value(&mut context);
                    return Err(ApiError::worker_unavailable("invalid_context"));
                }
                Ok((StatusCode::OK, context))
            }
            _ => Err(ApiError::simple("not_found", StatusCode::NOT_FOUND)),
        }
    }

    async fn result(
        &self,
        operation: &str,
        params: Value,
    ) -> Result<(StatusCode, Value), ApiError> {
        Ok((
            StatusCode::OK,
            self.supervisor.call(operation, params).await?,
        ))
    }
}

fn log_route(path: &str) -> &'static str {
    match path {
        "/health" => "/health",
        "/info" => "/info",
        "/session" => "/session",
        "/session/credentials" => "/session/credentials",
        "/session/restore" => "/session/restore",
        "/session/close" => "/session/close",
        "/session/login" => "/session/login",
        "/session/challenge" => "/session/challenge",
        "/playback" => "/playback",
        "/context" => "/context",
        "/key" => "/key",
        _ => "<unknown>",
    }
}

fn answer(status: StatusCode, mut payload: Value) -> HttpResponse {
    let bytes = serde_json::to_vec(&payload)
        .unwrap_or_else(|_| b"{\"error\":{\"code\":\"internal_error\"}}".to_vec());
    wipe_json_value(&mut payload);
    Response::builder()
        .status(status)
        .header(header::CONTENT_TYPE, "application/json; charset=utf-8")
        .header(header::CACHE_CONTROL, "no-store")
        .header(header::X_CONTENT_TYPE_OPTIONS, "nosniff")
        .header("content-security-policy", "default-src 'none'")
        .body(Full::new(Bytes::from(bytes)))
        .expect("static response headers")
}

fn login_response(value: LoginReply) -> Result<(StatusCode, Value), ApiError> {
    Ok(match value {
        LoginReply::Finished(session) => (StatusCode::OK, session),
        LoginReply::Challenge { id, kind } => (
            StatusCode::ACCEPTED,
            json!({"status":"challenge","kind":kind,"challenge_id":id}),
        ),
    })
}

fn parse_query(query: &str) -> Result<HashMap<String, String>, ApiError> {
    if query.len() > QUERY_LIMIT {
        return Err(ApiError::bad_request("request_target_too_large"));
    }
    let mut params = HashMap::new();
    for (key, value) in parse(query.as_bytes()) {
        if params
            .insert(key.into_owned(), value.into_owned())
            .is_some()
        {
            return Err(ApiError::bad_request("duplicate_parameter"));
        }
    }
    Ok(params)
}

fn get_param<'a>(params: &'a HashMap<String, String>, name: &str) -> Result<&'a str, ApiError> {
    params
        .get(name)
        .map(String::as_str)
        .ok_or_else(|| ApiError::bad_request("missing_parameter"))
}

fn field<'a>(json: &'a Value, name: &str, max: usize, empty: bool) -> Result<&'a str, ApiError> {
    let value = json
        .get(name)
        .and_then(Value::as_str)
        .ok_or_else(|| ApiError::bad_request("invalid_params"))?;
    if value.len() > max || (!empty && value.is_empty()) || value.contains('\0') {
        return Err(ApiError::bad_request("invalid_params"));
    }
    Ok(value)
}

fn validate_adam(adam: &str) -> Result<(), ApiError> {
    if adam.is_empty() || adam.len() > 20 || !adam.bytes().all(|c| c.is_ascii_digit()) {
        return Err(ApiError::bad_request("invalid_adam_id"));
    }
    Ok(())
}

fn validate_key_uri(uri: &str) -> Result<(), ApiError> {
    if !uri.starts_with("skd://")
        || uri.len() <= 6
        || uri.len() > 4096
        || uri.bytes().any(|c| c <= 32 || c == 127)
    {
        return Err(ApiError::bad_request("invalid_key_uri"));
    }
    Ok(())
}

async fn read_body(mut body: Incoming) -> Result<Vec<u8>, ApiError> {
    let mut bytes = Vec::new();
    timeout(Duration::from_secs(10), async {
        while let Some(frame) = body.frame().await {
            let frame = frame.map_err(|_| ApiError::bad_request("invalid_body"))?;
            if let Some(data) = frame.data_ref() {
                if bytes.len().saturating_add(data.len()) > BODY_LIMIT {
                    return Err(ApiError::simple(
                        "body_too_large",
                        StatusCode::PAYLOAD_TOO_LARGE,
                    ));
                }
                bytes.extend_from_slice(data);
            }
        }
        Ok(bytes)
    })
    .await
    .map_err(|_| ApiError::bad_request("body_timeout"))?
}

async fn empty_body(body: Incoming) -> Result<(), ApiError> {
    if !read_body(body).await?.is_empty() {
        return Err(ApiError::bad_request("unexpected_body"));
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn strict_query_and_sample_uri_validation() {
        assert!(parse_query("adam_id=1&adam_id=2").is_err());
        assert_eq!(
            get_param(&parse_query("uri=skd%3A%2F%2Fabc").unwrap(), "uri").unwrap(),
            "skd://abc"
        );
        assert!(validate_adam("1&redirect=2").is_err());
        assert!(validate_key_uri("https://example.org/").is_err());
        assert!(validate_key_uri("skd://bad\n").is_err());
        assert!(validate_key_uri("skd://valid-key").is_ok());
    }
    #[test]
    fn log_routes_never_include_untrusted_request_paths() {
        assert_eq!(log_route("/context"), "/context");
        assert_eq!(log_route("/session/credentials"), "/session/credentials");
        assert_eq!(log_route("/unknown/secret-value"), "<unknown>");
        assert_eq!(log_route("/context/secret-value"), "<unknown>");
    }
}
