# wrapper-v2

A clean rewrite of the Apple Music FairPlay decryption wrapper. Currently in
**Phase 1.3** — same as 1.1, plus **`POST /decrypt`** for FairPlay sample
decryption over JSON (batch base64 in/out). The **`/m3u8`** asset-URL helper is
still planned (Phase 1.2).

## What it is

A small daemon that runs Apple Music's Android native libraries inside a
Linux chroot, exposes a local HTTP API for FairPlay key fetching and sample
decryption, and gives downstream tooling (e.g. [`gamdl`](https://github.com/glomatico/gamdl))
a uniform interface that does not depend on platform or language.

The daemon ships *no* Apple code. At build time, libraries are extracted from
a pinned Apple Music for Android APK split (3.6.0-beta, build 1109) whose
SHA-256 digests are committed in `LIBS_VERSION.json`. Without a matching APK
the build fails loudly.

## Status

| Phase | Goal | State |
| --- | --- | --- |
| 0 | Repo skeleton, build chain, NDK toolchain, CI smoke test | **Done** |
| 1.0 | Apple lib runtime init, dlopen loader, vendored AOSP closure | **Done** |
| 1.1 | `POST /login`, `POST /login/2fa`, token harvest, `/me`, startup session restore (warm `WRAPPER_BASE_DIR`) | **Done** |
| 1.2 | `/m3u8` (asset URL fetch) | Pending |
| 1.3 | `POST /decrypt` (FairPlay FPS sample decrypt, JSON base64 batch) | **Done** |
| 2 | Rate limit, dedupe, request queue | Pending |
| 3 | arm64-v8a build, multi-arch Docker | Pending |

## HTTP API

Every endpoint accepts and returns `application/json`.

| Method | Path | Description |
| --- | --- | --- |
| `GET` | `/health` | Liveness probe. `{status, phase, version, runtime}` — `runtime.playback_ready` is true when FairPlay decrypt is available. |
| `GET` | `/me` | `{version, runtime, auth}` — same runtime flags as `/health`. |
| `POST` | `/login` | Body: `{"username": "...", "password": "..."}` or `{"apple_id": "...", "password": "..."}` (synonyms). Drives Apple's `AuthenticateFlow`. Returns `200` + token snapshot, `202` if **2FA** is required (then `POST /login/2fa`), or `401` on failure. |
| `POST` | `/login/2fa` | Body: `{"code": "123456"}`. Continues a login waiting for HSA2. |
| `POST` | `/decrypt` | Body: `{"adam_id":"<store adam id>","uri":"<skd://...>","samples":["<base64>",...]}` or a single `"sample":"..."`. Returns `200` `{"samples":["<base64 plaintext>",...]}`. Needs **authenticated** session and `playback_ready`; otherwise `401` / `503`. Apple errors → `502`. |
| `DELETE` | `/login` | Aborts an in-flight login or clears cached tokens from memory. Apple's on-disk `mpl_db` cache is unchanged. |

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
├── LIBS_VERSION.json         pinned APK + per-.so SHA-256 digests
├── src/
│   ├── daemon/               C++ daemon (cross-compiled with the NDK)
│   │   ├── CMakeLists.txt
│   │   ├── main.cpp          process entry: env parsing, lifecycle
│   │   ├── server.{hpp,cpp}  HTTP route mounting (cpp-httplib)
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
│   ├── fetch-apk.sh          download an APK / .apkm, verify SHA-256
│   ├── extract-libs.sh       extract .so files from APK, verify SHA-256
│   └── stage-system.sh       copy committed Android binaries into rootfs/
└── vendor/
    └── android-system/       linker64 + bionic + AOSP libs, SHA-pinned
        └── x86_64/
            ├── bin/linker64
            └── lib64/*.so
```

## Building

### One-time setup

You need a working Docker installation. Apart from that, the entire build
runs inside the image. There is no host toolchain prerequisite for the
default workflow.

For the build to succeed you must obtain Apple Music for Android **3.6.0-beta
(1109)** as APK splits. The APK is *not* committed and *not* fetched
automatically by the Dockerfile.

### Local build

```bash
# 1. Fetch the APKMirror bundle (you provide the URL).
APK_URL=https://your-mirror.example/apple-music-3.6.0-beta-1109.apkm \
    tools/fetch-apk.sh --expect apkm \
                        --out    .tmp/bundle.apkm

# 2. Extract Apple libs into rootfs/. The .apkm must match LIBS_VERSION.json
#    .apkm; each extracted .so matches .libs.x86_64.
tools/extract-libs.sh --bundle .tmp/bundle.apkm \
                       --arch   x86_64 \
                       --out    rootfs/system/lib64

# 3. Stage the committed Android system binaries (linker64 + bionic + AOSP)
#    into rootfs/, verifying their SHA-256 against LIBS_VERSION.json.
tools/stage-system.sh --arch x86_64

# 4. Build and run.
docker compose up --build

# 5. Smoke-test.
curl http://127.0.0.1/health
curl http://127.0.0.1/me

# 6. Sign in (use your real Apple ID; 2FA may require a second request).
curl -X POST http://127.0.0.1/login \
     -H 'content-type: application/json' \
     -d '{"username":"you@example.com","password":"your-app-specific-password"}'
curl http://127.0.0.1/me
curl -X DELETE http://127.0.0.1/login
```

The daemon binds port 80 inside the container and the compose file maps it
to host port 80 by default. Override with `HTTP_PORT=8080 docker compose up`
on machines that already have something on `:80`.

### Daemon configuration

The daemon reads `WRAPPER_*` environment variables (forwarded via
`compose.yaml`). See `.env.example` for the full list. The most useful are:

- `WRAPPER_HOST`, `WRAPPER_PORT` - bind address inside the chroot.
- `WRAPPER_BASE_DIR` - filesystem dir Apple's libs use for the FairPlay
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

### CI build

The `.github/workflows/build.yml` workflow runs on **push** to `main` or `phase-1`,
on **pull_request** (same-repo only for the full job), and **workflow_dispatch**.
It uses the same host steps as above plus a Docker build and `/health` smoke test,
with one repository secret:

- `APK_URL` - URL of the pinned `.apkm` (must match `LIBS_VERSION.json` → `apkm`)

Pull requests opened from forks skip the build job (they cannot read the
secret).

## License

[Unlicense](./LICENSE) - public domain dedication.

This project is not affiliated with Apple Inc. The Apple-authored libraries
it loads at runtime are not redistributed by this repository.
