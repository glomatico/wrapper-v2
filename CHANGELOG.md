# Changelog

## 1.0.1 — 2026-11-26

### Fixed

- Fix an ARM64 Android 5.1.1 dynamic linker crash on Linux hosts with a
  GNU-hash-only vDSO, including Debian 13. Failed vDSO registration no longer
  leaves an invalid object in the global symbol lookup list.
- Retain the existing Android API 22 runtime and Bionic libraries. The x86-64
  runtime and HTTP/IPC protocol are unchanged.
- Include the pinned AOSP source revision, reproducible build recipe, updated
  runtime checksum and standalone linker regression tests.

## 1.0.0 — 2026-11-19

Wrapper v2 1.0.0 is a ground-up rewrite of the original Wrapper v2, built for
clients such as [Gamdl](https://github.com/glomatico/gamdl) that perform media
decryption locally using [Temari](https://github.com/WorldObservationLog/Temari).
**This release is not backward-compatible with earlier Wrapper APIs.**

### API and architecture

- Replace the previous server implementation with a Rust HTTP supervisor and
  isolated native C++ Apple Music worker. Communication uses a versioned internal
  IPC protocol; the external HTTP API is unversioned for 1.0.0.
- Replace the legacy server-side `/decrypt` API with Temari-compatible decryption
  contexts via `/context` and `/key`. Clients decrypt samples themselves.
- Provide authenticated playback information, session state and credential export
  through `/playback`, `/session`, and `/session/credentials`.
- Report supported features, architecture, profile and protocol through `/info`
  so clients can reject incompatible servers before attempting authentication.
- Support native **x86-64** and **ARM64** workers, each with verified
  architecture-specific runtime and Apple Music library profiles.

### Authentication and lifecycle

- Support Apple Music login, two-factor challenges, cached session restoration,
  and optional environment-based automatic login.
- Preserve active two-factor challenges without imposing a short fixed wait
  timeout, and return bounded, privacy-conscious authentication diagnostics.
- Supervise workers in isolated mount/PID namespaces. Reap child processes,
  recover from worker failures, serialize native requests, and restore authenticated
  sessions following worker recycling.
- Add safe retry behavior for eligible read-only requests after worker recycling.
  Preserve operation-count limits even for charged failures.
- Use structured, privacy-conscious JSON logging.

### Docker installation

- Use a multi-stage Docker build for the Rust server, native worker, Linux
  launcher, and full Android runtime. Host-installed Rust and Android NDK are
  not required.
- Bundle verified Google Android system image components for both supported
  architectures, together with their third-party notices.
- Accept an authorized, pinned Apple Music **3.6.0-beta (build 1109)** standalone
  `.apk` or `.apkm` bundle directly as an argument to
  `tools/build-docker.sh`. Verify the expected manifests and native libraries
  before staging.
- Pass the private package through an isolated Docker BuildKit build context
  rather than copying the raw package into an image layer.
- Start Docker Compose without a `.env` file. Generate a persistent device
  identity on first startup and retain it with account/session data in a named
  volume. Optional `.env` settings and existing explicit device identities
  remain supported.
- Bind the Docker HTTP port to localhost by default, and keep account state
  outside the image.

### Compatibility and important notices

- **Breaking change:** Existing clients using the former `/decrypt` endpoint
  must migrate to the 1.0.0 session, playback, and Temari context APIs.
- **No APK is distributed with the source code.** Users must supply their own
  authorized copy of the pinned Apple Music version.
- **Do not publish or redistribute locally built Docker images.** Those images
  include Apple's extracted proprietary libraries even though the Git repository
  does not.
- Saved sessions depend on their original device identity. Preserve the
  persistent Docker volume and retain existing explicit identity settings when
  upgrading an older deployment.
- This rewrite has received substantial AI coding assistance. See the
  [README acknowledgements and AI disclosure](README.md#acknowledgements).

## 0.0.2 and earlier

The earlier Wrapper v2 releases used a different implementation and API. Their
published release history remains available on
[GitHub](https://github.com/glomatico/wrapper-v2/releases).
