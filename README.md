# Wrapper v2

**A self-hosted Apple Music playback and Temari-compatible context server.**

Wrapper v2 provides an HTTP API for authenticated Apple Music sessions, playback information, and decryption contexts used by compatible clients such as [Gamdl](https://github.com/glomatico/gamdl). The 1.0.0 rewrite uses a Rust HTTP supervisor and a native C++ worker.

For release history and breaking changes, see the [Changelog](CHANGELOG.md).

The server **does not decrypt or download media**. Decryption, when supported, takes place on the client side. The legacy `/decrypt` endpoint is **not** part of the 1.0.0 API.

## Features

- **Temari-compatible API** for playback and client-side decryption contexts.
- **x86-64 and ARM64** Linux hosts, with architecture-specific Android workers.
- **Session management** with cached authentication, manual login, and two-factor authentication.
- **Automatic session recovery** and transparent, bounded retries during planned worker recycling.
- **Isolated native workers** with separate mount and PID namespaces, process supervision, and cleanup.
- **Self-contained, privately built Docker images** with the verified Android runtime and Apple Music libraries.
- **Persistent account state** stored separately from the image.
- **Structured JSON logs** with sanitized request metadata.

## Requirements

| Component | Requirement |
| --- | --- |
| Host | Linux, `x86_64` or `aarch64` |
| Deployment | Docker with Compose **2.24+** (required for optional `.env`) |
| Apple Music | **Version 3.6.0-beta, build 1109**, for the selected architecture |
| Android runtime | Bundled, architecture-specific Android system files (x86-64: API 21; ARM64: API 22) |
| Building | Docker with Buildx/BuildKit and standard Linux shell tools |
| Toolchains | Android NDK **r23b**, Rust, CMake, Ninja, and cross-compilers are installed **inside Docker** |

The Apple Music build and bundled Android runtime are **strictly pinned**. Other APK versions and substitute libraries are not supported merely because they have the same architecture.

**You must supply your own authorized copy of the pinned Apple Music APK.** The Android system libraries are bundled from [verified Google system images](vendor/android-system/README.md), with their [third-party notices](THIRD_PARTY_NOTICES.md). The APK never enters Git or the ordinary source build context: the build script supplies it through an isolated, read-only BuildKit named context. **The final, locally built image contains the extracted proprietary Apple libraries. Do not publish or redistribute that image.** Login credentials and saved sessions are never built into the image.

## Getting started

The build helper defaults to your host architecture. For ARM64, use the `--arch arm64-v8a` flag instead. **No local Android NDK, Rust toolchain, or Android system image is needed.** Docker compiles both architectures with an x86-64 NDK builder. Cross-architecture builds also require QEMU/binfmt emulation for the target image's runtime setup, or a remote multi-platform Buildx builder. On an x86-64 Linux host without ARM64 emulation, enable it with `docker run --privileged --rm tonistiigi/binfmt --install arm64` (this registers a host-level emulator). Docker Desktop may already provide this support.

### 1. Obtain the supported Apple Music APK or APKM

Clone this repository and obtain your authorized Apple Music **3.6.0-beta (build 1109)** APK/APKM:

```bash
git clone https://github.com/glomatico/wrapper-v2.git
cd wrapper-v2
```

Provide either an `.apkm` bundle (containing `base.apk` and the correct architecture split) or a standalone `.apk` with the pinned architecture-specific native libraries. A `base.apk` taken alone from a split bundle may not contain the required libraries. The Android system files are already in `vendor/android-system/`; the APK manifest and individual Apple libraries are verified against pinned SHA-256 hashes.

### 2. Build everything in Docker

```bash
./tools/build-docker.sh /path/to/AppleMusic.apk
# Or an APKM bundle:
# ./tools/build-docker.sh /path/to/AppleMusic.apkm
# For an ARM64 image:
# ./tools/build-docker.sh /path/to/AppleMusic.apkm --arch arm64-v8a
```

The file path is passed directly to the build script; no environment variable is required. The multi-stage Docker build installs the pinned NDK and a Rust toolchain, compiles the native worker, Linux launcher, and Rust supervisor, and assembles the complete Android rootfs **inside the image**. The APK or APKM is bind-mounted read-only from its isolated named build context (and its SHA-256 controls cache invalidation); the raw package is **not copied into an image layer**. The Docker builder still receives the archive, so use only a trusted, private builder. The verified Apple libraries extracted from it are present in the resulting runtime image.

The generated image is `wrapper-v2:1.0.0-local-amd64` for x86-64 or `wrapper-v2:1.0.0-local-arm64` for ARM64. It is also tagged `wrapper-v2:local`, which Docker Compose uses automatically. Initial builds download the Android NDK and toolchains and therefore require substantial time, network access, and disk space; subsequent builds can reuse Docker layer caches.

### 3. Start Wrapper

```bash
docker compose up -d
```

**No `.env` file or manual login is needed.** On first startup, Wrapper generates a unique, nine-part device identity and saves it in the persistent Docker volume alongside the session cache. The identity is reused on future restarts and image upgrades. **Preserve this volume**: deleting it (including via `docker compose down -v`) loses both the identity and cached login. The Android runtime is embedded in the image and must not be bind-mounted from the host.

To configure an alternative image, port, DNS, device identity, or optional automatic login, copy `.env.example` to `.env` and uncomment only the settings you need. For automatic login, set **both** `WRAPPER_USERNAME` and `WRAPPER_PASSWORD`; otherwise leave them absent. Manual overrides must use a stable, slash-separated nine-field `WRAPPER_DEVICE_INFO`.

**Existing installations:** Keep your current `.env` values for `WRAPPER_DEVICE_INFO` and `WRAPPER_DATA_DIR` and reuse the same session directory. A legacy cache without a saved generated identity will **not** be assigned a different identity automatically; the server rejects that setup until the original identity is supplied. Do not remove your existing `.env` or switch from a `./data` bind mount to the new named volume without migrating the session state.

```bash
docker compose logs -f wrapper
curl -fsS http://127.0.0.1:8080/info
curl -fsS http://127.0.0.1:8080/session
```

**Ready for Gamdl:** Once the container is running, point [Gamdl](https://github.com/glomatico/gamdl) to your Wrapper v2 endpoint. Gamdl prompts for your Apple Music credentials and handles two-factor authentication (2FA) when needed. You do **not** need to log in through the HTTP API or configure `WRAPPER_USERNAME` / `WRAPPER_PASSWORD` just to use Gamdl.

The Compose configuration binds the HTTP service to **127.0.0.1:8080** by default and preserves account data in a named volume. **Do not rebuild the image with account data or publish it to a registry.** `GET /health` confirms the HTTP server is responding; use `GET /session` to check actual worker and authentication state. If using the HTTP API directly instead of Gamdl, send a JSON `POST` to `/session/login` containing `username` and `password`, and complete any required 2FA challenge using the endpoints below. Avoid putting real credentials directly in shell history.

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

The Compose configuration starts the HTTP process as a non-root UID. The native launcher temporarily needs `CAP_SYS_ADMIN` and `CAP_SYS_CHROOT` for namespaces, mounts, and `chroot`; those privileges are dropped before the Android worker runs. The service also uses a read-only container filesystem and an init process for child reaping. Container runtimes with stricter seccomp/LSM policies may need corresponding namespace/mount permissions. **Any user with permission to pull/export the locally built image can extract its embedded Apple libraries.** Keep the image private and restrict Docker daemon access.

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

For development, `tools/build.sh` can still build the native worker and launcher on a host with Android NDK r23b installed. Maintainers with the locally provisioned `wrapper-v2:native-build` image can use `tools/build-local.sh --arch arm64-v8a` instead. These are **optional native-only developer workflows**; the recommended Docker build does not require either. When using the host tools, first extract verified Apple libraries with `tools/prepare_rootfs.py --extract-libraries`.

## Limitations

- Supports only the pinned Apple Music APK build and architecture-specific runtime profiles currently in `profiles/`.
- Does not implement the previous Wrapper v2 `/decrypt` API or decrypt files server-side.
- Serves one native operation at a time; concurrent requests may be rejected with `worker_busy`.
- Requires the operator to supply the pinned Apple Music APK, maintain an authorized Apple Music account, and comply with applicable service and licensing restrictions.

## Acknowledgements

Wrapper v2 owes a great deal to the projects and people who helped build the Apple Music wrapper ecosystem:

- **[zhaarey/wrapper](https://github.com/zhaarey/wrapper)** — the original Wrapper project, which laid the groundwork for running Apple Music's Android components without a full Android emulator.
- **WorldOfDecrypt / [WorldObservationLog](https://github.com/WorldObservationLog)** — for their work on [Wrapper and Wrapper Lite](https://github.com/WorldObservationLog/wrapper/tree/lite), and on the [Temari](https://github.com/WorldObservationLog/Temari) decryption library and related tooling.

Thank you to their contributors and the wider community for sharing their research and making continued development possible. Wrapper v2 1.0.0 is a rewrite with its own implementation; these acknowledgements recognize the preceding work and ideas, not an endorsement of this project by the upstream authors.

## AI usage

**This project was developed with substantial assistance from AI coding tools.** AI was used throughout development for implementation, architecture discussions, debugging, testing, and documentation. AI-generated suggestions and code can be incorrect or introduce security vulnerabilities, even when automated tests pass. The project maintainer remains responsible for reviewing, testing, and maintaining the published code. Issues and independent reviews are welcome.

## License

Wrapper v2's original source code is released under [The Unlicense](LICENSE). The bundled Android system files retain their own licenses and required notices, documented in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md), and included in locally built images. Apple's proprietary libraries are **not shipped in the source repository** but **are embedded in private Docker builds**. Do not redistribute those images without the necessary rights. This project is not affiliated with or endorsed by Apple.
