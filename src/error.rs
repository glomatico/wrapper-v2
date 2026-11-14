use hyper::StatusCode;
use serde_json::{json, Value};

#[derive(Clone, Debug)]
pub struct ApiError {
    pub code: String,
    pub status: StatusCode,
    pub retryable: bool,
    pub restart_required: bool,
}

impl ApiError {
    pub fn new(code: &str, status: StatusCode, retryable: bool, restart_required: bool) -> Self {
        Self {
            code: code.into(),
            status,
            retryable,
            restart_required,
        }
    }
    pub fn simple(code: &str, status: StatusCode) -> Self {
        Self::new(code, status, false, false)
    }
    pub fn bad_request(code: &str) -> Self {
        Self::simple(code, StatusCode::BAD_REQUEST)
    }
    pub fn bad_config(_name: &str) -> Self {
        // Configuration names can be printed at startup, never values.
        Self::simple(_name, StatusCode::INTERNAL_SERVER_ERROR)
    }
    pub fn worker_unavailable(code: &str) -> Self {
        Self::new(code, StatusCode::SERVICE_UNAVAILABLE, true, true)
    }
    pub fn native(code: &str, retryable: bool, restart: bool) -> Self {
        let status = match code {
            "invalid_params"
            | "invalid_json"
            | "invalid_request"
            | "invalid_auth_code"
            | "invalid_auth_continuation"
            | "unsupported_protocol" => StatusCode::BAD_REQUEST,
            "authentication_required" | "session_expired" => StatusCode::FORBIDDEN,
            "no_cached_session" | "playback_unavailable" => StatusCode::NOT_FOUND,
            "already_authenticated"
            | "already_initialized"
            | "session_in_use"
            | "worker_recycle" => StatusCode::CONFLICT,
            "unsupported_library" | "unsupported_capture_hook" => StatusCode::UNPROCESSABLE_ENTITY,
            _ => StatusCode::BAD_GATEWAY,
        };
        Self::new(code, status, retryable, restart)
    }
    pub fn json(&self) -> Value {
        json!({"error":{"code":self.code,"retryable":self.retryable,"restart_required":self.restart_required}})
    }
}
