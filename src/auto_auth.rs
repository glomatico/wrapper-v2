//! Optional noninteractive login for managed deployments.
//! Credentials are consumed once at startup, never stored in Config or logs.
use std::env;

use serde_json::{json, Value};
use zeroize::Zeroizing;

use crate::{
    error::ApiError,
    worker::{LoginReply, Supervisor},
};

pub struct Credentials {
    username: Zeroizing<String>,
    password: Zeroizing<String>,
}

impl Credentials {
    pub fn take_from_env() -> Result<Option<Self>, ApiError> {
        let username = env::var("WRAPPER_USERNAME").ok().map(Zeroizing::new);
        let password = env::var("WRAPPER_PASSWORD").ok().map(Zeroizing::new);
        // Avoid mutating the process environment after Tokio has started its
        // threads. The native worker explicitly excludes these variables.
        // Docker's container configuration still retains them: keep env files private.
        match (username, password) {
            (None, None) => Ok(None),
            (Some(username), Some(password))
                if !username.is_empty()
                    && username.len() <= 320
                    && !username.contains('\0')
                    && !password.is_empty()
                    && password.len() <= 4096
                    && !password.contains('\0') =>
            {
                Ok(Some(Self { username, password }))
            }
            _ => Err(ApiError::bad_config("WRAPPER_CREDENTIALS")),
        }
    }
}

fn authenticated(value: &Value) -> bool {
    value.get("state").and_then(Value::as_str) == Some("authenticated")
}

pub async fn initialize(supervisor: &std::sync::Arc<Supervisor>, creds: Credentials) {
    // A cached login takes precedence, so restarting never invalidates an
    // existing session merely because WRAPPER_USERNAME/PASSWORD are present.
    if let Ok(session) = supervisor.call("session.status", json!({})).await {
        if authenticated(&session) {
            tracing::info!(event = "session.auto_auth_cached");
            return;
        }
    }
    if let Ok(session) = supervisor.call("session.restore", json!({})).await {
        if authenticated(&session) {
            tracing::info!(event = "session.auto_auth_restored");
            return;
        }
    }
    match supervisor.login(creds.username, creds.password).await {
        Ok(LoginReply::Finished(session)) if authenticated(&session) => {
            tracing::info!(event = "session.auto_auth_complete");
        }
        Ok(LoginReply::Finished(_)) => {
            tracing::warn!(
                event = "session.auto_auth_failed",
                code = "not_authenticated"
            );
        }
        Ok(LoginReply::Challenge { id, .. }) => {
            // Never leave the native worker waiting for 2FA indefinitely. This
            // cancels the pending login; operators can retry interactively via
            // POST /session/login and POST /session/challenge.
            let _ = supervisor
                .challenge(&id, Zeroizing::new(String::new()))
                .await;
            tracing::warn!(event = "session.auto_auth_2fa_required");
        }
        Err(error) => {
            tracing::warn!(event = "session.auto_auth_failed", code = %error.code);
        }
    }
}
