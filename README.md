# wrapper-v2

A clean rewrite of the Apple Music FPS (FairPlay Streaming) decryption wrapper, based on
[`WorldObservationLog/wrapper`](https://github.com/WorldObservationLog/wrapper).

## Development note

This project has been developed with heavy AI assistance. The code should be
treated as research-grade and reviewed carefully, especially around native ABI
calls, FPS state handling, and experimental endpoints. AI-generated changes
are not assumed to be correct just because they compile.

## What it is

A small daemon that exposes a local HTTP API for account/playback control plus
a raw TCP port for FPS sample decryption, and gives downstream tooling (e.g.
[`gamdl`](https://github.com/glomatico/gamdl)) a uniform interface that does
not depend on platform or language.

At runtime `/app/wrapperd` is a host-Linux Rust supervisor. It owns the public
HTTP port, owns the raw decrypt TCP port, and starts `/app/wrapper`, the small
host chroot launcher. The launcher execs `/system/bin/main`, an Android/NDK C++
IPC worker inside the Linux chroot. Only that worker loads Apple Music's Android
native libraries. If FPS hangs, crashes, or returns a CKC/KD-style decrypt
error, the Rust supervisor can discard the worker while keeping the public
listeners alive.

The daemon ships _no_ Apple code. Apple Music native libraries must be supplied
by the person building the image and staged into `rootfs/system/lib64/`; the
expected `.so` SHA-256 digests are pinned in `LIBS_VERSION.json`.

## HTTP API

Most endpoints accept and return `application/json`. Decryption is not exposed
through HTTP; clients use the raw TCP decrypt protocol on
`${WRAPPER_DECRYPT_PORT:-10020}`.

| Method   | Path         | Description                                                                                                                                                                                                                                                                                                                                                                        |
| -------- | ------------ | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `GET`    | `/health`    | Liveness probe. `{status, version, runtime}` — `runtime.playback_ready` is true when FPS decrypt is available.                                                                                                                                                                                                                                                                     |
| `GET`    | `/me`        | `{version, runtime, auth}` — same runtime flags as `/health`.                                                                                                                                                                                                                                                                                                                      |
| `POST`   | `/login`     | Body: `{"username": "...", "password": "..."}` or `{"apple_id": "...", "password": "..."}` (synonyms). Drives Apple's `AuthenticateFlow`. Returns `200` + token snapshot, `202` if **2FA** is required (then `POST /login/2fa`), or `401` on failure.                                                                                                                              |
| `POST`   | `/login/2fa` | Body: `{"code": "123456"}`. Continues a login waiting for HSA2.                                                                                                                                                                                                                                                                                                                    |
| `GET`    | `/playback`  | Query string `?adam_id=<numeric store id>`. Returns `200` with a JSON object `{"songList":[...]}` containing the **whole MZ playback dispatch** Apple's `subDownload` URL bag returns (every flavor, key URI, asset URL, metadata field). CFData fields are base64; CFDate fields are ISO 8601. Needs an **authenticated** session; otherwise `401` / `503`. Apple errors -> `502`. |
| `DELETE` | `/login`     | Aborts an in-flight login or clears cached tokens from memory. Apple's on-disk `mpl_db` cache is unchanged.                                                                                                                                                                                                                                                                        |

## TCP Decrypt API

The decrypt listener defaults to `0.0.0.0:10020`. The Compose file maps this as
`${DECRYPT_PORT:-10020}:10020`. This branch uses wrapper-v2's versioned batch
protocol; it is not wire-compatible with the original wrapper sample stream.

All integers are big-endian. Request and response frames share this envelope:

```text
magic        4 bytes  "WV2D"
version      u16      1
kind         u16      1 = decrypt batch, 2 = decrypt ok, 3 = decrypt error, 9 = close
request_id   u32
payload_len  u32
payload      payload_len bytes
```

Decrypt batch payload:

```text
adam_id_len
uri_len
sample_count
sample_len[0]
...
sample_len[sample_count - 1]
adam_id bytes
uri bytes
sample[0] bytes
...
sample[sample_count - 1] bytes
```

Successful decrypt payload:

```text
sample_count
sample_len[0]
...
sample_len[sample_count - 1]
sample[0] bytes
...
sample[sample_count - 1] bytes
```

Error payloads are UTF-8 messages. Decrypt errors, worker crashes, or worker
timeouts close the affected TCP client connection; the Rust supervisor starts a
fresh Apple worker for later requests.

Sign-in matches the legacy wrapper model: you send **email (Apple ID) and password**
to the daemon; it fills credentials through the native presentation interface.
With a persistent `WRAPPER_BASE_DIR` volume, Apple keeps `mpl_db/kvs.sqlitedb` on
disk. On each process start the daemon tries **session restore** (default
`WRAPPER_RESTORE_SESSION=1`): if that session is still valid, `GET /me` can show
**authenticated** and fresh tokens **without** another `POST /login`. Use
`POST /login` when the volume is new, restore fails, or you need to re-auth.
Optional `WRAPPER_APPLE_ID` only sets the `apple_id` label in `/me` after restore.

## Layout

```
.
├── CMakeLists.txt            top-level build (host launcher + NDK sub-build)
├── Dockerfile                multi-stage build
├── compose.yaml              docker compose entrypoint
├── LIBS_VERSION.json         per-.so SHA-256 digests
├── src/
│   ├── rust/                 Rust supervisor (HTTP + TCP + worker lifecycle)
│   ├── daemon/               C++ Apple IPC worker (cross-compiled with the NDK)
│   │   ├── CMakeLists.txt
│   │   ├── main.cpp          process entry: env parsing, Apple init
│   │   ├── ipc.{hpp,cpp}     stdio IPC dispatch for the Rust supervisor
│   │   └── apple/
│   │       ├── abi.hpp       Apple-lib mangled symbol declarations
│   │       ├── auth.{hpp,cpp}    Apple ID login + 2FA + token cache
│   │       ├── loader.{hpp,cpp}  dlopen / dlsym
│   │       ├── runtime.{hpp,cpp} FootHillConfig + RequestContext + credential UI
│   │       └── tokens.{hpp,cpp}  dev token + music user token harvest
│   └── launcher/
│       └── wrapper.c         host-Linux chroot launcher
├── rootfs/                   chroot tree assembled at build time
│   └── system/
│       ├── bin/              <- main, linker64 (staged)
│       └── lib64/            <- Apple's .so + Android system .so (staged)
├── tools/
│   ├── extract-libs.sh       optional local helper to extract and verify Apple .so files
│   └── stage-system.sh       copy committed Android binaries into rootfs/
└── vendor/
    └── android-system/       linker64 + bionic + AOSP libs, SHA-pinned
        ├── x86_64/
        │   ├── bin/linker64
        │   └── lib64/*.so
        └── arm64-v8a/
            ├── bin/linker64
            └── lib64/*.so
```

## Building

### One-time setup

You need a working Docker installation. Apart from that, the entire build
runs inside the image. There is no host toolchain prerequisite for the
default workflow.

For the build to succeed, `rootfs/system/lib64/` must already contain the
required Apple Music native libraries for your `TARGET_ARCH`. The tested source
version is Apple Music for Android **3.6.0-beta build 1109**. Provide your own
legally obtained `.apk` or `.apkm`; this repository does not host, link to, or
redistribute Apple binaries.

### Local build

#### 1. Extract Apple Music native libraries

Provide a local Apple Music `.apk` or `.apkm` for the target architecture. The
default output is `rootfs/system/lib64/`, and every extracted `.so` must match
the hashes in `LIBS_VERSION.json`.

```bash
bash tools/extract-libs.sh --bundle path/to/local/apple-music.apk --arch x86_64
```

`.apkm` bundles are also accepted:

```bash
bash tools/extract-libs.sh --bundle path/to/local/apple-music.apkm --arch x86_64
```

#### 2. Stage Android system binaries

This copies the committed Android linker and system libraries into `rootfs/`,
verifying their SHA-256 hashes against `LIBS_VERSION.json`.

```bash
bash tools/stage-system.sh --arch x86_64
```

#### 3. Build and run

```bash
docker compose up --build
```

#### 4. Smoke test

```bash
curl http://127.0.0.1/health
curl http://127.0.0.1/me
```

The daemon binds HTTP port 80 and TCP decrypt port 10020 inside the container.
Override with `HTTP_PORT=8080` or `DECRYPT_PORT=11020` when those host ports are
already in use.

### Optional sign in

You do not need to sign in manually as part of the local build. Downstream tools
such as [`gamdl`](https://github.com/glomatico/gamdl) can ask for credentials
automatically and call `/login` / `/login/2fa` when they need an authenticated
Apple Music session.

For manual testing, use your real Apple ID. If the first request returns `202`,
continue with the 2FA request.

```bash
curl -X POST http://127.0.0.1/login \
     -H 'content-type: application/json' \
     -d '{"username":"you@example.com","password":"your-app-specific-password"}'
```

```bash
curl -X POST http://127.0.0.1/login/2fa \
     -H 'content-type: application/json' \
     -d '{"code":"123456"}'
```

Check the current session or clear the in-memory login state:

```bash
curl http://127.0.0.1/me
curl -X DELETE http://127.0.0.1/login
```

### arm64-v8a image (Apple Silicon / AArch64 Linux)

Stage **arm64-v8a** Android system binaries and Apple Music native libraries,
then build a **linux/arm64** image so `wrapper`, the NDK daemon, and the staged
`linker64` / `.so` set share the same ABI.

The Docker **compile** stage is always **linux/amd64** (Google ships the Linux NDK as an
x86_64-host ZIP only). The image then cross-compiles `wrapper` for AArch64 when
`TARGET_ARCH=arm64-v8a`. Set **runtime** platform to arm64; `BUILD_PLATFORM` in Compose is
ignored but kept for compatibility.

Extract and stage the arm64 files:

```bash
bash tools/extract-libs.sh --bundle path/to/local/apple-music.apk --arch arm64-v8a
bash tools/stage-system.sh --arch arm64-v8a
```

Or use a local `.apkm` bundle:

```bash
bash tools/extract-libs.sh --bundle path/to/local/apple-music.apkm --arch arm64-v8a
bash tools/stage-system.sh --arch arm64-v8a
```

Build the arm64 image:

```bash
TARGET_ARCH=arm64-v8a RUNTIME_PLATFORM=linux/arm64 \
  docker compose up --build
```

On an **x86_64** host, `docker compose` / `docker run` need **QEMU** (binfmt) to run a
`linux/arm64` container. On an **arm64** host, run the image **natively** (no emulation).

### Daemon configuration

The daemon reads `WRAPPER_*` environment variables (forwarded via
`compose.yaml`). See `.env.example` for the full list. The most useful are:

- `WRAPPER_HOST`, `WRAPPER_PORT` - public HTTP bind address and port.
- `WRAPPER_DECRYPT_HOST`, `WRAPPER_DECRYPT_PORT` - raw TCP decrypt bind address
  and port. Defaults are `0.0.0.0` and `10020`.
- `WRAPPER_MODE` - internal C++ worker mode. Normal users should not set it;
  the Rust supervisor sets `ipc-worker` automatically.
- `WRAPPER_WORKER_TIMEOUT_SECS` - timeout for one IPC request to the C++
  Apple worker. Default is `60`.
- `WRAPPER_BASE_DIR` - filesystem dir Apple's libs use for the FPS
  key cache and `mpl_db`. The default matches upstream wrapper.
- `WRAPPER_RESTORE_SESSION` - set to `0` to skip startup token harvest from
  an existing on-disk Apple session (default is restore on).
- `WRAPPER_APPLE_ID` - optional display label for `apple_id` in `GET /me` after
  session restore only (not sent to Apple).
- `WRAPPER_DEVICE_INFO` - 9-tuple identifying the fake Apple Music
  Android client. Same fingerprint upstream uses by default.
- `WRAPPER_APPLE_INIT=0` - skip Apple lib initialization at startup.
  Lets you bring up the HTTP server alone for `/health` smoke tests
  even on builds where you have not staged the Apple libraries yet.
- `WRAPPER_USERNAME` + `WRAPPER_PASSWORD` - if both are set and the runtime
  initialized, the daemon runs password sign-in at startup when not already
  authenticated (same semantics as `POST /login`; 2FA still needs
  `POST /login/2fa`). Treat these as secrets.

### CI build

The `.github/workflows/build.yml` workflow runs on **push** to `main`,
on **pull_request** (same-repo only for the full job), and **workflow_dispatch**.
It uses the same host steps as above plus a Docker build and `/health` smoke
test, with one repository secret:

- `APK_URL` - private/local CI URL for a compatible Apple Music `.apk` or
  `.apkm`. The artifact is downloaded inside CI only, extracted with
  `tools/extract-libs.sh`, and is not committed.

**Matrix:** both `x86_64` and `arm64-v8a` jobs use `ubuntu-latest`. The arm64 image is
`linux/arm64` at runtime; QEMU is enabled before the smoke `docker run` so the job works
on amd64 GitHub runners. The compile stage stays **linux/amd64** for the official NDK ZIP.
The job also runs `cargo test`, validates `compose.yaml`, and checks that the
TCP decrypt listener accepts connections.

Pull requests opened from forks skip the build job because they cannot read the
secret.

## License

[Unlicense](./LICENSE) - public domain dedication.

This project is not affiliated with Apple Inc. The Apple-authored libraries
it loads at runtime are not redistributed by this repository.
