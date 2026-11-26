# Changelog

## Unreleased

- Correct the generated first-install device fingerprint using the combined
  client tuple and 64-bit Android-ID representation reported to complete
  fresh login and 2FA. Existing persisted identities, native identity hashes,
  and cached sessions are not rewritten.
- Retain independent 128-bit randomness for temporary identity-file names
  and add regression coverage for fresh identity generation, concurrent
  initialization, explicit overrides, and legacy identity preservation.
- Add one clean Linux installation runbook and a separate source audit with
  the successful x86_64 reproduction and its limits. The reproduction does
  not establish ARM64 end-to-end support.
- Correct the documented Android application build target to API 21, matching
  the pinned profile and CMake configuration.

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
- Provide architecture-specific x86-64 and ARM64 worker implementations and
  runtime profiles. Live validation scope is documented separately.

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
  launcher, and Android runtime. Host-installed Rust and Android NDK are not
  required.
- Bundle Google Android system runtime components for both architectures,
  together with their original third-party notices.
- Accept an operator-supplied local Apple Music 3.6.0-beta (build 1109)
  standalone APK or supported split APKM. Validate the expected manifests,
  native libraries, and architecture before staging.
- Pass the private package through an isolated Docker BuildKit named context
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
- **No Apple Music APK is distributed with the source code.** The operator
  supplies an authorized local package compatible with the pinned profile.
- **Do not publish or redistribute locally built Docker images.** Those images
  include Apple's extracted proprietary libraries even though the Git repository
  does not.
- Saved sessions depend on their original device identity. Preserve the
  persistent Docker volume and retain existing explicit identity settings when
  upgrading an older deployment.
- This rewrite has received substantial AI coding assistance. See the
  [README AI usage note](README.md#ai-usage).

## 0.0.2 and earlier

The earlier Wrapper v2 releases used a different implementation and API. Their
published release history remains available on
[GitHub](https://github.com/glomatico/wrapper-v2/releases).
