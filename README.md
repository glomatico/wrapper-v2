# Wrapper v2

**A self-hosted Apple Music playback and Temari-compatible context server.**

Wrapper v2 provides an HTTP API for authenticated Apple Music sessions, playback information, and decryption contexts used by compatible clients such as [Gamdl](https://github.com/glomatico/gamdl). The 1.0.0 rewrite uses a Rust HTTP supervisor and a native C++ worker.

The server **does not decrypt or download media**. Decryption, when supported, takes place on the client side. The legacy `/decrypt` endpoint is **not** part of the 1.0.0 API.

## Features

- **Temari-compatible API** for playback and client-side decryption contexts.
- **x86-64 and ARM64** Linux hosts, with architecture-specific Android workers.
- **Session management** with cached authentication, manual login, and two-factor authentication.
- **Automatic session recovery** and transparent, bounded retries during planned worker recycling.
- **Isolated native workers** with separate mount and PID namespaces, process supervision, and cleanup.
- **Small runtime images**, with proprietary files and account data kept outside the image.
- **Structured JSON logs** with sanitized request metadata.

## Requirements

| Component | Requirement |
| --- | --- |
| Host | Linux, `x86_64` or `aarch64` |
| Deployment | Docker with Compose (recommended) |
| Apple Music | **Version 3.6.0-beta, build 1109**, for the selected architecture |
| Android runtime | Bundled, architecture-specific Android system files (x86-64: API 21; ARM64: API 22) |
| Native build | Android NDK **r23b**, CMake, Ninja, C++17 compiler, Python 3, Git, and pinned CMake dependencies |
| Server build | Rust toolchain; cross-builds also require the target's Linux cross-compiler and Rust target |

The Apple Music build and bundled Android runtime are **strictly pinned**. Other APK versions and substitute libraries are not supported merely because they have the same architecture.

**You must supply your own authorized copy of the pinned Apple Music APK.** The Android system libraries are bundled from [verified Google system images](vendor/android-system/README.md), with their [third-party notices](THIRD_PARTY_NOTICES.md). Apple libraries, account credentials, and sessions are **not** included in the repository or Docker images. The staging utility verifies hashes before installing any files into the local rootfs.

## Getting started

The example below uses `x86_64`. For ARM64, set `ARCH=arm64-v8a` and use the matching Apple Music APK. **No separate Android system image is needed** for either architecture.

### 1. Extract the Apple Music libraries

Clone this repository and choose the correct architecture:

```bash
git clone https://github.com/glomatico/wrapper-v2.git
cd wrapper-v2
export ARCH=x86_64 # or arm64-v8a
export APPLE_MUSIC_APK=/path/to/your/Apple-Music.apkm
```

The Android system libraries are already included in `vendor/android-system/<architecture>/`. Provide either the supported APKM bundle (including `base.apk` and the architecture split) or the verified architecture-specific APK.

```bash
python3 tools/prepare_rootfs.py \
  --arch "$ARCH" \
  --apk "$APPLE_MUSIC_APK" \
  --extract-libraries "build/$ARCH/apple-libs"
```

The extraction directory must not already exist. Unsupported APKs and mismatched libraries fail hash verification. Only verified Apple libraries are extracted into the local ignored `build/` directory; they are not committed or published.

### 2. Build the native worker and launcher

Install Android NDK **r23b (23.1.7779620)**, CMake, Ninja, a C++ compiler, Python 3 and Git. Set `ANDROID_NDK_HOME` to the NDK directory:

```bash
export ANDROID_NDK_HOME=/path/to/android-ndk-r23b
./tools/build.sh --arch "$ARCH"
```

For an ARM64 build on an x86-64 Linux machine, install the `aarch64-linux-gnu-g++` and `aarch64-linux-gnu-gcc` cross-compilers. Native dependencies are pinned by CMake and may be fetched during the first build. A preconfigured local Docker builder is **not required** for this standard workflow.

### 3. Stage the verified Android rootfs

```bash
python3 tools/prepare_rootfs.py \
  --arch "$ARCH" \
  --apk "$APPLE_MUSIC_APK"
```

The utility creates `build/$ARCH/rootfs` using the bundled, SHA-256-verified Android system files and refuses to overwrite an existing destination. Keep this directory **private**; it contains Apple libraries and will be mounted into the container, not bundled into the image. Advanced users may override the Android binaries with `--android-system /path/to/system`, but hashes must still match the selected profile.

### 4. Build the Docker image

```bash
./tools/package.sh --arch "$ARCH"
```

For cross-compilation, first add the corresponding Rust target with `rustup target add`, and ensure its Linux linker is on `PATH` (or set `CROSS_CC`). For example, from an x86-64 Linux host targeting ARM64:

```bash
rustup target add aarch64-unknown-linux-gnu
CROSS_CC=aarch64-linux-gnu-gcc ./tools/package.sh --arch arm64-v8a
```

The resulting image is `wrapper-v2:1.0.0-local-amd64` for x86-64 or `wrapper-v2:1.0.0-local-arm64` for ARM64. It contains **only** the Rust server and Linux launcher; the prepared Android rootfs is mounted separately.

### 5. Configure and start

```bash
cp .env.example .env
${EDITOR:-vi} .env
mkdir -p data
```

Set `WRAPPER_IMAGE`, `WRAPPER_ROOTFS_DIR`, and `WRAPPER_DEVICE_INFO` for your build. The device information must be a stable, **nine-part, slash-separated** identifier used by your own authorized Apple Music session; do not use the placeholder or share this value. Ensure that the configured UID/GID can access the staged session directory and writable `data/` directory.

For ARM64, the relevant entries are:

```dotenv
WRAPPER_IMAGE=wrapper-v2:1.0.0-local-arm64
WRAPPER_ROOTFS_DIR=./build/arm64-v8a/rootfs
```

For optional automatic login on startup, **uncomment and set both** `WRAPPER_USERNAME` and `WRAPPER_PASSWORD` in your private `.env` file. Leave both variables **absent**, not empty, when using manual authentication. A cached session is attempted before a fresh login.

```bash
docker compose up -d --no-build
docker compose logs -f wrapper
curl -fsS http://127.0.0.1:8080/info
curl -fsS http://127.0.0.1:8080/session
```

The Compose configuration binds the HTTP service to **127.0.0.1** by default and preserves account data in a separate volume. `GET /health` confirms the HTTP server is responding; use `GET /session` to check actual worker and authentication state. For manual login, send a JSON `POST` to `/session/login` containing `username` and `password`, and complete any required 2FA challenge using the endpoints below. Avoid putting real credentials directly in shell history.

## HTTP API

The API is **unversioned** in 1.0.0. Requests and responses use JSON; the internal native-worker IPC protocol is separate.

| Method | Endpoint | Purpose |
| --- | --- | --- |
| `GET` | `/health` | HTTP liveness |
| `GET` | `/info` | Version, features, profile and architecture |
| `GET` | `/session` | Current session state and operation count |
| `GET` | `/session/credentials` | Export session credentials for a compatible client |
| `POST` | `/session/restore` | Restore the previously cached session |
| `POST` | `/session/login` | Start login with `username` and `password` |
| `GET` | `/session/challenge` | Retrieve the pending two-factor challenge |
| `POST` | `/session/challenge` | Submit `challenge_id` and six-digit `code`; empty code cancels |
| `POST` | `/session/close` | Close the session |
| `GET` | `/playback?adam_id=...` | Obtain playback data for an Apple Music catalog ID |
| `GET` | `/context?adam_id=...&key_uri=...` | Obtain a Temari-compatible context for a valid `skd://` URI |
| `GET` | `/key?adamId=...&uri=...` | Compatibility alias for `/context` |

Example requests after authentication:

```bash
curl -fsS 'http://127.0.0.1:8080/playback?adam_id=123456789'
curl -fsS -G 'http://127.0.0.1:8080/context' \
  --data-urlencode 'adam_id=123456789' \
  --data-urlencode 'key_uri=skd://your-key-identifier'
```

If an interactive login requires 2FA, the login endpoint returns **HTTP 202** with a challenge ID. Use `GET /session/challenge` to retrieve it, then submit:

```json
{"challenge_id":"<challenge_id>","code":"123456"}
```

to `POST /session/challenge`. The displayed code is an example, **not** a real authentication code.

Errors use the form `{"error":{"code":"...","retryable":false,"restart_required":false}}`. Normal recycling is handled internally for playback and context requests. Simultaneous requests may receive `503 worker_busy` because the server deliberately serializes access to its single native worker.

## Security and operational notes

**Do not expose Wrapper v2 directly to the internet.** There is no built-in HTTP authentication or TLS, and `/session/credentials` returns sensitive account material. Use a loopback binding, a private network, or a separately authenticated TLS gateway. Restrict access to the host, Docker socket, `.env`, staged rootfs, and session volumes. Environment-based auto-login credentials are visible to users with access to Docker container configuration.

The Compose configuration starts the HTTP process as a non-root UID. The native launcher temporarily needs `CAP_SYS_ADMIN` and `CAP_SYS_CHROOT` for namespaces, mounts, and `chroot`; those privileges are dropped before the Android worker runs. The service also uses a read-only container filesystem and an init process for child reaping. Container runtimes with stricter seccomp/LSM policies may need corresponding namespace/mount permissions.

Workers have a bounded operation budget and are replaced when necessary, with authenticated sessions restored before protected requests continue. Each worker receives a fresh PID namespace to avoid a known mutex/TID overflow in the pinned Android runtime. See [worker lifecycle](docs/worker-launch.md).

Logs are JSON lines on stderr and intentionally omit raw URLs, query parameters, credentials, key URIs, and contexts. See [logging](docs/logging.md). Treat error logs as sensitive operational data nonetheless.

## Development and testing

```bash
cargo fmt --check
cargo clippy --locked --all-targets -- -D warnings
cargo test --locked
python3 -B -m unittest discover -s tests -p 'test_*.py' -v
```

The automated HTTP tests use a **synthetic worker**; they do not require Apple account credentials or proprietary files. Additional Linux namespace tests require `unshare` support and elevated namespace permissions; follow [worker lifecycle](docs/worker-launch.md) for instructions. Validate the actual architecture-specific worker and cached session before production deployment.

Maintainers who already have the pinned, locally provisioned `wrapper-v2:native-build` image can use `tools/build-local.sh --arch "$ARCH"` instead of `tools/build.sh`. This is an **optional local development helper**; the builder image and proprietary libraries are not distributed with Wrapper v2.

## Limitations

- Supports only the pinned Apple Music APK build and architecture-specific runtime profiles currently in `profiles/`.
- Does not implement the previous Wrapper v2 `/decrypt` API or decrypt files server-side.
- Serves one native operation at a time; concurrent requests may be rejected with `worker_busy`.
- Requires the operator to supply the pinned Apple Music APK, maintain an authorized Apple Music account, and comply with applicable service and licensing restrictions.

## License

Wrapper v2's original source code is released under [The Unlicense](LICENSE). The bundled Android system files retain their own licenses and required notices, documented in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). Apple's proprietary libraries are not redistributed. This project is not affiliated with or endorsed by Apple.
