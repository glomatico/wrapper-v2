# Clean Linux installation

This runbook describes the validated x86_64 installation path for Wrapper v2 1.0.0. It keeps the complete setup procedure in one place; the repository README contains only the quick start and project overview.

## 1. Validation boundary

The live reproduction used Windows 10 IoT Enterprise LTSC 2021, openSUSE Tumbleweed under WSL, Docker Compose inside Linux, Wrapper 1.0.0, an x86_64 native worker, and Apple Music 3.6.0-beta build 1109. Gamdl was installed separately on Windows.

Fresh login, 2FA, session-credential retrieval, worker replacement/session recovery, playback, and `/key` HTTP requests succeeded on x86_64. The test did not validate ARM64. Its build and runtime profiles exist, but treat ARM64 live authentication, playback, and context capture as unverified.

See [the source audit](source-audit-2026-11.md) for the sanitized event sequence, exact test scope, and unresolved findings.

## 2. Linux host prerequisites

Use Linux on x86_64 for the validated path. The codebase also contains an ARM64 build profile, but that target has not been end-to-end validated by this reproduction.

Install and configure:

- Docker Engine with a running daemon.
- Docker Compose plugin 2.24 or newer.
- Docker Buildx with BuildKit enabled.
- Git, Bash, coreutils (including `realpath`, `sha256sum`, and `mktemp`), and `curl`.

Rust, Android NDK, CMake, Ninja, and cross-compilers are installed inside the Docker builder; they are not required on the host.

Verify the environment:

```bash
uname -m
git --version
docker version
docker compose version
docker buildx version
docker buildx inspect --bootstrap
```

The Docker CLI must show both client and server sections. Buildx must report a usable builder. Docker daemon access is effectively privileged host access; grant it only to trusted users.

## 3. Clone a source revision containing the correction

After the upstream pull request is merged, use the canonical repository:

```bash
git clone https://github.com/glomatico/wrapper-v2.git
cd wrapper-v2
```

While the upstream pull request is pending, the tested correction is on this fork branch:

```bash
git clone --branch fix/device-info-apple-auth-1109 --single-branch \
  https://github.com/TuPapaPendejo/wrapper-v2-Tests.git wrapper-v2
cd wrapper-v2
```

Confirm that the working tree is clean and the selected revision contains the correction:

```bash
git status --short --branch
git log -1 --oneline
```

Do not assume that upstream `main` contains the correction until the maintainer merges it.

## 4. Supply the local Apple Music build input

The runtime profiles target Apple Music package `com.apple.android.music`, version `3.6.0-beta`, build `1109`. Supply your own authorized local package for the selected ABI. The build script accepts either:

- A standalone APK containing the required manifest and architecture-specific native libraries.
- A split APKM bundle containing `base.apk` and the matching architecture split.

A base APK extracted by itself from a split package may not contain the required native libraries. The package must match the profile selected by `--arch`. This repository does not host the package and this runbook does not identify third-party download sources.

Set a shell variable to the local file you already possess:

```bash
APPLE_MUSIC_PACKAGE="/absolute/path/to/your/authorized-Apple-Music-build-1109.apk"
test -f "$APPLE_MUSIC_PACKAGE"
```

The path may point to an `.apkm` file instead. Do not place the package in Git, a shared build context, or an image layer.

**What the build verifies:** `tools/prepare_rootfs.py` checks the expected base/split Android manifest digest, the selected ABI, the ELF machine type, and the SHA-256 hash of each required Apple native library. It stages only the allowlisted files. The helper also calculates the complete input archive's SHA-256 and passes that value through BuildKit; this preserves input consistency and cache invalidation, but it is not a publisher-authenticity check or a comparison to a trusted, hard-coded whole-archive checksum.

## 5. Build the tested x86_64 image

```bash
./tools/build-docker.sh "$APPLE_MUSIC_PACKAGE" --arch x86_64
```

The multi-stage Docker build installs the pinned NDK and Rust toolchain, compiles the native worker and namespace launcher, builds the Rust HTTP service, validates the selected runtime files, and assembles the Android root filesystem inside the image.

The build must complete without manifest, hash, architecture, missing-library, or compilation errors. Do not start an image produced by an interrupted or failed build.

An ARM64 build can be requested with:

```bash
./tools/build-docker.sh "$APPLE_MUSIC_PACKAGE" --arch arm64-v8a
```

This command is present in the build scripts, but it is not validated by the live reproduction documented here. Cross-building may require binfmt/QEMU support in Buildx or a remote multi-platform builder. Do not treat a successful image build alone as proof of ARM64 authentication or playback.

## 6. Start Wrapper and check the service

A normal clean installation does not need a `.env` file:

```bash
docker compose up -d
docker compose ps
docker compose logs -f wrapper
```

The service binds to `127.0.0.1:8080` on the Linux host and stores session data in the persistent `wrapper_data` volume by default.

Check liveness, runtime profile, and session state:

```bash
curl -fsS http://127.0.0.1:8080/health
curl -fsS http://127.0.0.1:8080/info
curl -fsS http://127.0.0.1:8080/session
```

- `/health` confirms that the HTTP server is responding. It does not prove the native worker is authenticated.
- `/info` reports the version, architecture, and selected runtime profile.
- `/session` reports the session/worker state.

A new volume has no cached Apple Music session. An initial restore attempt may report `no_cached_session` or `session_expired`; continue through a fresh login.

## 7. Connect Windows Gamdl to Wrapper under WSL

The validated arrangement ran Wrapper inside WSL and installed Gamdl separately on Windows. With WSL localhost forwarding enabled and the default Compose port mapping, Windows can reach the service at:

```text
http://127.0.0.1:8080
```

Check from PowerShell:

```powershell
curl.exe -fsS http://127.0.0.1:8080/health
```

Configure Gamdl to use Wrapper as its authentication provider and supply the endpoint when prompted. Complete Apple Account login and any 2FA challenge in the client. Keep the WSL distribution, Docker daemon, and Wrapper container running while Gamdl uses the service.

If Windows cannot reach the endpoint, check WSL localhost forwarding and the Compose port mapping first. Do not expose the unauthenticated HTTP service on all host interfaces as a workaround.

## 8. Expected authentication and runtime sequence

The validated x86_64 run exercised this sequence:

1. The API and native worker start.
2. A fresh `POST /session/login` returns HTTP 202 while the 2FA challenge is pending.
3. Challenge submission returns HTTP 200 and Wrapper logs `session.authenticated`.
4. `GET /session` and `GET /session/credentials` return HTTP 200.
5. After worker replacement, Wrapper logs `session.recovered`.
6. Playback and `/key` requests return HTTP 200.

These results confirm the exercised x86_64 routes. They do not validate every catalog item, codec, playback edge case, internal context-byte correctness, or ARM64.

## 9. Preserve session state

Stop the service without deleting the persistent volume:

```bash
docker compose down
```

Rebuild and restart while keeping the same Compose project and volume:

```bash
./tools/build-docker.sh "$APPLE_MUSIC_PACKAGE" --arch x86_64
docker compose up -d
```

**Destructive reset — use only when intentionally deleting the saved device identity and Apple Music session:**

```bash
docker compose down -v
```

The `-v` option removes the Compose-managed named volume. Never use it as a routine update command. For installations using the legacy `WRAPPER_DATA_DIR=./data` bind mount, preserve that directory and its original `WRAPPER_DEVICE_INFO`; do not silently switch to the named volume.

## 10. Security

- Keep the host binding on loopback. The HTTP API has no built-in authentication or TLS.
- Do not expose port 8080 directly to the Internet; use a separately authenticated TLS gateway for deliberate remote access.
- Treat Docker daemon access, the session volume, `.env`, and operational logs as sensitive.
- Do not commit the Apple Music package, extracted proprietary libraries, credentials, or session state.
- Do not publish or redistribute the locally built image: it contains extracted proprietary Apple libraries.
- Keep the generated device identity paired with its session volume across restarts and image upgrades.
