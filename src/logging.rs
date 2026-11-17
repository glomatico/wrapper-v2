//! Explicitly allow only our structured events into logs. Other crates may
//! have access to untrusted HTTP headers and signed URLs, so do not enable
//! their tracing targets even when debugging.
use tracing::Level;
use tracing_subscriber::EnvFilter;

pub fn init() {
    let level = std::env::var("RUST_LOG")
        .ok()
        .and_then(|value| value.parse::<Level>().ok())
        .unwrap_or(Level::INFO);
    tracing_subscriber::fmt()
        .json()
        .with_writer(std::io::stderr)
        .flatten_event(true)
        .with_ansi(false)
        .with_target(false)
        .with_current_span(false)
        .with_span_list(false)
        .with_env_filter(EnvFilter::new(format!("wrapperd={level}")))
        .init();
}
