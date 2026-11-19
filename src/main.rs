mod auto_auth;
mod config;
mod device_info;
mod error;
mod http;
mod logging;
mod profile;
mod worker;

use std::sync::Arc;

use hyper::server::conn::http1;
use hyper::service::service_fn;
use hyper_util::rt::TokioIo;
use tokio::net::TcpListener;

use config::Config;
use http::Api;

#[tokio::main]
async fn main() {
    logging::init();
    if let Err(err) = serve().await {
        // No request values, credentials, URLs, or raw native errors here.
        tracing::error!(event = "server.startup_failed", code = %err.code);
        std::process::exit(1);
    }
}

async fn serve() -> Result<(), error::ApiError> {
    let settings = Config::from_env()?;
    let credentials = auto_auth::Credentials::take_from_env()?;
    let listener = TcpListener::bind((settings.host, settings.port))
        .await
        .map_err(|_| error::ApiError::worker_unavailable("http_bind_failed"))?;
    let api = Api::new(&settings);
    tracing::info!(
        event = "server.started",
        version = env!("CARGO_PKG_VERSION"),
        protocol = 1,
        architecture = %settings.profile.architecture,
        host = %settings.host,
        port = settings.port,
    );
    if let Some(credentials) = credentials {
        let supervisor = api.supervisor();
        tokio::spawn(async move {
            auto_auth::initialize(&supervisor, credentials).await;
        });
    }
    loop {
        let (stream, _) = listener
            .accept()
            .await
            .map_err(|_| error::ApiError::worker_unavailable("http_accept_failed"))?;
        let api = Arc::clone(&api);
        tokio::spawn(async move {
            let service = service_fn(move |request| Arc::clone(&api).serve(request));
            if http1::Builder::new()
                .serve_connection(TokioIo::new(stream), service)
                .await
                .is_err()
            {
                // Hyper errors can contain untrusted URLs; log a static code.
                tracing::warn!(event = "http.connection_error", code = "connection_failed");
            }
        });
    }
}
