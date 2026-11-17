# Structured logs

`wrapperd` writes **newline-delimited JSON (JSONL)** to standard error. Each
line is an independent JSON object, ready for Docker, systemd, `jq`, or Loki.
No separate JSONL export mode is required.

At the default `RUST_LOG=info` level the server reports startup, sanitized
HTTP access records, worker lifecycle events, session transitions and errors.
When `WRAPPER_USERNAME` and `WRAPPER_PASSWORD` are both configured, startup
tries the saved session first, then attempts automatic login if needed. Events
`session.auto_auth_cached`, `session.auto_auth_restored`, and
`session.auto_auth_complete` indicate success; `session.auto_auth_failed` or
`session.auto_auth_2fa_required` mean the operator must intervene. An automatic
2FA challenge is cancelled, so the `/session/login` and `/session/challenge`
HTTP flow can be used interactively without restarting.
Set `RUST_LOG=debug` to include successful `/health` probes, or `warn`/`error`
to reduce volume. Valid levels are `trace`, `debug`, `info`, `warn`, `error`;
invalid values fall back to `info`. This configuration **only enables Wrapper's
own logging target**, never third-party HTTP/library diagnostic targets.

Example (representative, not an actual account/session):

```jsonl
{"timestamp":"2026-11-17T12:30:00Z","level":"INFO","event":"server.started","version":"1.0.0","protocol":1,"architecture":"arm64-v8a","host":"0.0.0.0","port":80}
{"timestamp":"2026-11-17T12:30:03Z","level":"INFO","event":"http.request","request_id":42,"method":"GET","path":"/playback","status":200,"duration_ms":26}
{"timestamp":"2026-11-17T12:30:05Z","level":"WARN","event":"worker.error","operation":"context.get","code":"context_capture_failed","restart_required":false}
```

Export container logs without Docker's additional timestamp prefix:

```sh
docker logs <container-name> > wrapper.jsonl 2>&1
jq 'select(.level == "ERROR")' wrapper.jsonl
```

The server **never logs raw URLs, arbitrary request paths, query parameters,
request/response bodies, headers, usernames, passwords, 2FA challenges, tokens,
key URIs, signed playback URLs, Temari contexts, or sample data**. Only a
fixed allowlist of HTTP route names is logged; unknown paths appear as
`<unknown>`. `request_id` is a monotonically increasing server-local counter,
not a user-provided header. Error codes are sanitized native/protocol codes,
never raw exception messages. HTTP worker IPC remains binary and the native
worker's stderr is suppressed to keep every wrapperd log line valid JSON.

Configure rotation and retention in your container runtime or systemd rather
than inside the process. Because this service manages authentication sessions,
keep logs access-restricted even when values are deliberately minimized.
