# Clean Linux installation — Wrapper v2 1.0.0

This guide covers a clean installation of the corrected Wrapper branch. It is
distribution-independent: install the prerequisites using the package manager
and Docker setup supported by your Linux distribution.

## Validated environment

The successful test ran Wrapper and Docker Compose inside openSUSE Tumbleweed
under an updated WSL installation on Windows 10 IoT Enterprise LTSC 2021.
Gamdl was installed separately on Windows. The tested Wrapper architecture was
x86_64 only.

The test completed login, 2FA, credentials retrieval, session recovery after
worker recycling, playback, and `/key` requests. ARM64 is expected to use the
same shared Rust device-identity code, but was not included in this live test.

## 1. Host prerequisites

Install Linux on x86_64 or aarch64, Docker Engine with a running daemon, Docker
Compose v2.24 or newer, Docker Buildx/BuildKit, Git, Bash, coreutils, and curl.

No host Rust toolchain, Android SDK/NDK, CMake, Ninja, or cross-compiler is
required. The Docker build installs compilation toolchains in its builder.

Verify:

```bash
uname -m
git --version
docker version
docker compose version
docker buildx version
docker buildx inspect --bootstrap
```

The Docker CLI must be able to reach the daemon (the `docker version` output
must include a server section).

## 2. Clone the corrected branch

Until the correction is merged upstream:

```bash
git clone --branch fix/device-info-apple-auth-1109 --single-branch \
  https://github.com/TuPapaPendejo/wrapper-v2-Tests.git wrapper-v2
cd wrapper-v2
```

After the upstream maintainer merges the PR, clone the canonical repository:

```bash
git clone https://github.com/glomatico/wrapper-v2.git
cd wrapper-v2
```

Check the current branch and commit before building:

```bash
git status --short --branch
git log -1 --oneline
```

## 3. Download the pinned Apple Music APKM

Download the APKM variant containing x86_64 and arm64-v8a splits from the
[exact APKMirror 3.6.0-beta listing](https://www.apkmirror.com/apk/apple/apple-music/apple-music-3-6-0-beta-release/apple-music-3-6-0-beta-4-android-apk-download/).

Required package: `com.apple.android.music`, version `3.6.0-beta`, build
`1109`, variant `arm64-v8a + x86_64`.

Pinned SHA-256:

```text
5be907af370a7f6bd73344e646dcfebe2e64c2da3b9ad71192110c91950f3f67
```

Verify the download:

```bash
sha256sum "$HOME/Downloads/AppleMusic-3.6.0-beta-1109.apkm"
```

The checksum must match exactly. Do not bypass validation or use another build.
Do not commit the APKM. The local image contains extracted proprietary Apple
libraries and must not be published or redistributed.

## 4. Build the Docker image

For the tested architecture:

```bash
./tools/build-docker.sh "$HOME/Downloads/AppleMusic-3.6.0-beta-1109.apkm" --arch x86_64
```

The build helper sends the package through an isolated BuildKit context. The
Dockerfile checks its digest, pinned manifests, Apple library hashes, bundled
Android system files, and executable architectures, then compiles the native
worker/launcher and Rust server and stages the Android rootfs.

ARM64 build command (not live-tested in the reported reproduction):

```bash
./tools/build-docker.sh "$HOME/Downloads/AppleMusic-3.6.0-beta-1109.apkm" --arch arm64-v8a
```

Cross-building ARM64 may require binfmt/QEMU or a remote multi-platform builder.

## 5. Start and verify Wrapper

No `.env` file is required for a normal fresh installation:

```bash
docker compose up -d
docker compose ps
docker compose logs -f wrapper
```

Check service health and runtime profile:

```bash
curl -fsS http://127.0.0.1:8080/health
curl -fsS http://127.0.0.1:8080/info
curl -fsS http://127.0.0.1:8080/session
```

A fresh data volume has no saved Apple Music session. A restore attempt may
return `no_cached_session` or `session_expired`; proceed with a new login.

## 6. Connect Windows Gamdl to Wrapper under WSL

In the validated arrangement Gamdl ran on Windows while Wrapper ran under WSL.
With WSL localhost forwarding enabled and the default Compose port mapping,
use this endpoint from Windows:

```text
http://127.0.0.1:8080
```

Test reachability in PowerShell:

```powershell
curl.exe -fsS http://127.0.0.1:8080/health
```

Configure Gamdl to use Wrapper as its authentication provider and enter the
endpoint when requested. Keep WSL, Docker, and the Wrapper container running.

If Windows cannot reach the service, check WSL localhost forwarding and the
Compose port mapping. Do not expose the service on all host interfaces as a
workaround; the HTTP API has no built-in authentication or TLS.

## 7. Expected successful flow

The validated x86_64 run showed:

1. `server.started` and `worker.started`.
2. `POST /session/login` returning HTTP 202 for the pending 2FA challenge.
3. Challenge completion with HTTP 200 and `session.authenticated`.
4. `GET /session` and `GET /session/credentials` returning HTTP 200.
5. `session.recovered` after worker replacement.
6. Playback and `/key` requests returning HTTP 200.

These results verify the exercised x86_64 paths, not ARM64.

## 8. Preserve or intentionally delete session state

Stop without deleting persisted session state:

```bash
docker compose down
```

Rebuild and restart while retaining the same Compose volume:

```bash
./tools/build-docker.sh "$HOME/Downloads/AppleMusic-3.6.0-beta-1109.apkm" --arch x86_64
docker compose up -d
```

Only when intentionally deleting the generated device identity and cached
Apple Music session:

```bash
docker compose down -v
```

Never use `down -v` as a routine update command. Preserve the volume so the
device identity remains paired with the cached session.

## 9. Security

Keep the default loopback binding, restrict Docker daemon access, and keep the
data volume and logs private. Do not expose Wrapper directly to the Internet,
publish the local image, or commit the APKM, credentials, or session data.
