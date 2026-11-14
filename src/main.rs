mod config;
mod error;
mod http;
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
    if let Err(err) = serve().await {
        // No request values, credentials, URLs, or raw native errors here.
        eprintln!("wrapperd: startup failure: {}", err.code);
        std::process::exit(1);
    }
}

async fn serve() -> Result<(), error::ApiError> {
    let settings = Config::from_env()?;
    let listener = TcpListener::bind((settings.host, settings.port))
        .await
        .map_err(|_| error::ApiError::worker_unavailable("http_bind_failed"))?;
    let api = Api::new(&settings);
    eprintln!(
        "wrapperd: HTTP v1 listening at {}:{}",
        settings.host, settings.port
    );
    loop {
        let (stream, _) = listener
            .accept()
            .await
            .map_err(|_| error::ApiError::worker_unavailable("http_accept_failed"))?;
        let api = Arc::clone(&api);
        tokio::spawn(async move {
            let service = service_fn(move |request| Arc::clone(&api).serve(request));
            let _ = http1::Builder::new()
                .serve_connection(TokioIo::new(stream), service)
                .await;
            // Do not log URLs or headers; signed playback URLs may be private.
        });
    }
}
