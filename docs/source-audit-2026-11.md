# Source audit and reverse-engineering notes — November 2026

## Review identity and limits

- Repository: `TuPapaPendejo/wrapper-v2-Tests`
- Reviewed baseline: `main` at `9a6f29f493b1bac5325105ab6edf5dbd8caaae8c`
- Scope: Rust HTTP API and supervisor; native C++ worker, session and playback
  paths; x86-64 and ARM64 context-capture hooks; launcher and namespaces;
  runtime profiles and binary pins; APK/APKM staging; Docker/Compose; tests,
  CI and operational documentation.
- Method: source and configuration review through the GitHub repository
  contents API, comparison against the upstream Wrapper Lite device-identity
  implementation, and review of the supplied reproduction in
  [glomatico/wrapper-v2 issue 23](https://github.com/glomatico/wrapper-v2/issues/23).
- This is a **source-level audit**, not a claim of a completed live Apple login
  test. The proprietary APKM, Apple account credentials, and a privileged Linux
  Docker runtime were not run in this review environment. CI's synthetic worker
  cannot validate Apple's proprietary `AuthenticateFlow`.

## Executive findings

1. **Confirmed fresh-install authentication compatibility defect — fixed in this
   change.** The auto-generated device identity used an obsolete client tuple
   (`Music/4.9`, Samsung S9, build 7663313) and generated a 128-bit suffix
   (32 hex characters). Issue 23 comment
   [7162866962](https://github.com/glomatico/wrapper-v2/issues/23#issuecomment-7162866962)
   reports a controlled successful login after changing only
   `WRAPPER_DEVICE_INFO` to the `Music/5.0.2` / Pixel 8 / build 7663314 tuple
   with an eight-byte random Android ID. The author did not isolate the two
   fields experimentally. The new default adopts the complete tested tuple
   and ID representation, and adds tests for that exact shape.
2. **Persisted sessions are deliberately not migrated.** The Rust identity file
   and native `.worker-device` hash form a pair. Silently regenerating either
   value can make an existing Apple Music cache unusable. The patch changes
   only identities generated for new installations and explicitly tests that
   an existing 1.0.0 identity is left byte-for-byte unchanged.
3. **The API is not an internet-facing authenticated service.** There is no
   built-in HTTP authentication or TLS. In particular,
   `GET /session/credentials` exports session tokens. Compose publishes the
   host port on loopback by default; changing that binding or exposing the
   container port without an authenticated TLS gateway exposes account
   material and session-control operations.
4. **The native implementation is highly version- and ABI-specific by design.**
   Symbols, object layouts, linker behavior, capture hook addresses, register
   states, byte offsets and expected context sizes are coupled to exact
   Apple/Android binaries. SHA-256 pins and runtime handshakes fail closed, but
   they do not replace real integration testing against the pinned APKM.
5. **The HTTP and lifecycle tests are synthetic.** They give useful coverage of
   routing, IPC, retry behavior, 2FA orchestration, redaction, staging and
   namespace teardown. They do not execute Apple's native authentication or
   prove that a real Apple account can log in.

## Detailed findings

### F-01 — Fresh-install device identity did not match the working client profile

**Severity:** High for first-login availability  
**Status:** Fixed in this change; real Apple login still requires private verification.

Baseline implementation in `src/device_info.rs` used:

- `Music/4.9/Android/10/Samsung S9/7663313/en-US/en-US`
- 16 random bytes, formatted as a 32-character hexadecimal suffix.

The issue 23 reproduction records the same native failure on a fresh x86-64
build-1109 install and then a successful 2FA login after changing only the
device-info override to:

```text
Music/5.0.2/Android/10/Pixel 8/7663314/en-US/en-US/<16 lowercase hex chars>
```

The upstream [WorldObservationLog Wrapper Lite source](https://github.com/WorldObservationLog/wrapper/blob/lite/lite/lite_main.cpp)
also documents the nine-field device tuple and computes Android ID as a
64-bit FNV-1a value formatted to 16 hexadecimal characters. The issue's
successful test uses a random 64-bit value instead; this project continues
to use cryptographically random bytes rather than deriving an ID from the
account username.

**Change:** new installations now use that known-working client tuple and
eight random bytes (64 bits). The random ID is persisted atomically with mode
0600. Temporary-file names retain their independent 128-bit randomness.

**Compatibility:** explicit `WRAPPER_DEVICE_INFO` overrides remain supported.
Existing `.wrapper-device-info` files are read as-is, including old 32-hex
suffixes. No old identity, native hash, or saved session is rewritten.

**Unresolved experimental distinction:** the report changed both the client
tuple and the ID length at once. The evidence proves the combined value works
in that reporter's environment; it does not establish which individual field
Apple rejected. A later controlled test can vary one field at a time, but a
new installation should use the already demonstrated working combination.

### F-02 — Runtime API-level documentation disagreed with the build profiles

**Severity:** Low  
**Status:** Fixed in this change.

The README described the ARM64 Android system image as API 22. Both pinned
profiles declare API 21, and the Docker/CMake build configures and enforces
Android platform 21. The requirement table now reports API 21 for both ABIs.

### F-03 — Unauthenticated HTTP API and credential export

**Severity:** High if reachable by an untrusted client  
**Status:** Existing documented design limitation; no protocol change made.

The HTTP service has no authentication middleware or TLS termination.
`GET /session/credentials` returns developer and media-user tokens; login,
session close, playback and context routes are also directly callable.
Compose's published port is bound to `127.0.0.1` by default, and the README
warns against exposing the service directly.

**Operational control:** keep the host binding on loopback, do not publish the
container port to an untrusted network, restrict Docker/network access, and
put any remote access behind an independently authenticated TLS gateway.
Do not treat a private container network as a substitute for application
authentication when other tenants or workloads can join it.

### F-04 — Human 2FA wait has no server-side deadline

**Severity:** Medium when an untrusted client can reach the API  
**Status:** Deliberate behavior, retained to avoid expiring legitimate 2FA.

The Rust supervisor holds the single-worker lock while waiting for a human
challenge response. The native protocol pauses its operation timer only during
that wait. This prevents a short fixed timeout from killing a valid interactive
login, but also means one abandoned challenge can keep the worker occupied and
cause other worker-dependent calls to return `503 worker_busy`.

**Operational control:** restrict access to the API and cancel abandoned
challenges with an empty code. A future configurable idle timeout would need
explicit product behavior and regression tests for long-running 2FA.

### F-05 — Native ABI and capture hooks are pinned to exact binary layouts

**Severity:** High impact if pins or offsets drift  
**Status:** Guarded by hashes and fail-closed checks; requires ongoing review.

The native worker uses mangled Apple C++ symbols, hand-declared ABI return
types, shared-pointer control-block adoption, hard-coded object storage sizes,
and a fixed offset to read a response payload. The capture implementations
also use architecture-specific instruction addresses, byte prefixes, register
predicates, stack ranges, pointer remapping and hundreds of fixed state offsets.
The runtime verifies Apple shared-library SHA-256 values and hook instruction
prefixes before use; profile metadata and HTTP response contracts validate
captured context sizes.

These guards are valuable: do not relax them to make a different APK appear
compatible. Any Apple Music version, library, NDK, Dobby, linker or ABI change
must be treated as a separate reverse-engineering target and tested on both
architectures. A successful compile alone does not prove that native object
sizes or capture state are correct.

### F-06 — CI does not exercise the proprietary Apple authentication path

**Severity:** Medium  
**Status:** Test-environment limitation.

The current workflow builds the Rust API and launcher, runs Rust/C++ unit
tests, Linux namespace tests, and Python integration tests using
`tests/mock_worker.py`. APK staging tests use synthetic archives. The
workflow does not receive the proprietary APKM or Apple credentials and does
not execute the native Apple libraries.

Consequently, green CI can validate the HTTP/supervisor contracts but cannot
confirm `AuthenticateFlow`, live 2FA, token validation, playback, or actual
Temari context capture. Keep real-device tests private and report their
architecture, APKM digest, profile, and sanitized result separately.

### F-07 — Builder inputs are not fully reproducible

**Severity:** Medium for supply-chain/rebuild reproducibility  
**Status:** Not changed in this authentication patch.

The Android NDK archive is checksum-verified and external native source
dependencies are pinned to Git commits. However, the Docker base image is
referenced by a mutable tag, Debian packages are installed from the current
repository state, and Rust is installed using the moving `stable` channel.
Two builds from the same source commit can therefore use different build
toolchains or base layers over time.

**Recommendation:** pin the Docker base by digest, pin a tested Rust toolchain,
use a controlled package snapshot, and record toolchain versions in build
metadata. Keep the existing artifact hash verification.

### F-08 — Authentication validation uses a separately hard-coded request header

**Severity:** Low / investigation item  
**Status:** Not changed; no evidence it caused the reported failure.

`native/session.cpp` constructs a POST header identifying
`Music/4.9 Android/10 model/Samsung S9 build/7663313`, while the tested
working `WRAPPER_DEVICE_INFO` tuple is Music 5.0.2 / Pixel 8 / 7663314.
This header is used by the later signed session-validation request, not to
construct the nine-field `RequestContext` identity itself. The issue reporter's
working override shows that this mismatch did not prevent the reported login
from completing. It should nevertheless be tested during private session
restore and token validation before changing it; blindly deriving or changing
the header could introduce a second variable.

### F-09 — Certificate trust is build-host dependent

**Severity:** Medium when the worker must reach an Apple endpoint whose root is
absent from the builder's CA bundle  
**Status:** Not changed; separate from the demonstrated device-identity fix.

`tools/prepare_rootfs.py` stages the CA bundle supplied by the build
environment. The issue 23 report separately found that one staged bundle did
not contain the Apple Root CA needed by `gsa.apple.com`; appending that root
made `curl` verification succeed, but did not change the native
`AuthenticateFlow` result. Do not conflate this trust-store correction with
the device-identity fix. Any future CA addition must be obtained from an
authoritative Apple certificate source and checked against a published
fingerprint before being bundled.

### F-10 — Sensitive data is intentionally omitted from logs, but runtime access
still matters

**Severity:** Medium  
**Status:** Existing controls retained.

The HTTP layer logs allowlisted route names rather than raw paths or query
strings. Credentials, challenge values, key URIs, tokens and contexts are
excluded from ordinary logs. Native authentication diagnostics accept only
bounded enums and numeric fields. However, Docker logs, the data volume,
`.env`, the Docker socket and the final image remain security-sensitive.
The image contains extracted proprietary Apple libraries, and the session
volume contains persistent authentication state. Keep all of them private.

## Architecture map

1. **Build/staging:** `tools/build-docker.sh` supplies the operator's APK/APKM
   to a named BuildKit context. `tools/prepare_rootfs.py` verifies the selected
   profile's manifests and Apple library hashes, verifies vendored Android
   system files, and stages the root filesystem.
2. **Rust HTTP facade:** `src/main.rs`, `src/http.rs`, `src/config.rs`,
   `src/profile.rs` parse configuration and expose the unversioned API.
3. **Supervisor:** `src/worker.rs` serializes operations, frames IPC,
   enforces timeouts and response limits, handles worker recycling, and
   coordinates 2FA.
4. **Namespace launcher:** `native/launcher.cpp` creates mount/PID namespaces,
   chroots into the staged Android rootfs, starts the Android worker, reaps
   children and tears down the namespace when IPC closes.
5. **Native worker:** `native/main.cpp`, `runtime.cpp`, `session.cpp`,
   `playback.cpp` and `context.cpp` call the pinned Apple libraries.
6. **Context capture:** `capture_x86_64.cpp` and `capture_arm64.cpp` use
   different, profile-specific hooks and memory layouts. The Rust profile
   contract checks the serialized context before it is returned to a client.

## Verification matrix

| Area | Source/test coverage reviewed | Live validation still required |
| --- | --- | --- |
| Device identity | persistence, atomic creation, concurrent startup, explicit override and legacy identity tests | new-login and existing-session test with build 1109 |
| HTTP API | synthetic end-to-end routes, validation, error mapping, redaction | no Apple native path |
| Supervisor | mock worker recycle/recovery, cancellation, 2FA, client disconnect | actual native worker restart and session restoration |
| Native protocol | framing, size/depth checks, 2FA continuation validation, SHA-256/base64 tests | Apple IPC behavior |
| Launcher | disposable PID/mount namespaces, orphan reaping, teardown | target host's Docker seccomp/AppArmor policy |
| APKM staging | synthetic APK/APKM, manifest/hash/ELF and duplicate-library rejection | actual authorized APKM extraction and full Docker build |
| x86-64/ARM64 capture | static profiles, offsets, hashes and context length contracts | real playback/context request for each ABI |

## Exact Apple Music package required for private verification

The exact APKMirror listing identified for the build pinned by this repository is:

- Package: `com.apple.android.music`
- Version: `3.6.0-beta`
- Build: `1109`
- Variant: `arm64-v8a + x86_64`, split APKM bundle
- Listing: <https://www.apkmirror.com/apk/apple/apple-music/apple-music-3-6-0-beta-release/apple-music-3-6-0-beta-4-android-apk-download/>
- APKM SHA-256:
  `5be907af370a7f6bd73344e646dcfebe2e64c2da3b9ad71192110c91950f3f67`

Verify a downloaded file before building:

```bash
sha256sum AppleMusic-3.6.0-beta-1109.apkm
./tools/build-docker.sh /path/to/AppleMusic-3.6.0-beta-1109.apkm --arch x86_64
./tools/build-docker.sh /path/to/AppleMusic-3.6.0-beta-1109.apkm --arch arm64-v8a
```

Use only an authorized copy. Do not commit the APKM, extracted proprietary
libraries, credentials, tokens, or session data to this repository. Do not
publish the resulting image.

## Change log for this audit branch

- Changed the generated default fingerprint to the tested client tuple and
  64-bit Android-ID representation.
- Kept 128-bit entropy for temporary staging-file names.
- Added regression coverage for the new fresh-install identity and for the
  no-migration guarantee on already persisted identities.
- Updated the `.env.example` manual override.
- Documented the identity compatibility behavior and corrected the ARM64 API
  level in the README.
- Added the findings, limits, architecture map, and private validation matrix
  in this document.


## Successful clean-install reproduction — 25 November 2026 local time

A subsequent clean installation confirmed the identity correction on x86_64.
The log timestamps below were emitted in UTC on 26 November 2026; the test took
place on 25 November local time (UTC-06:00).

### Environment

- Windows host: Windows 10 IoT Enterprise LTSC 2021.
- Linux runtime: openSUSE Tumbleweed under an updated WSL installation.
- Wrapper deployment: Docker Compose in the Linux environment.
- Wrapper version: 1.0.0.
- Native worker: x86_64.
- Apple Music: pinned 3.6.0-beta, build 1109.
- Gamdl: freshly installed separately on Windows.
- Test scope: x86_64 only.

### Authentication evidence

The sanitized log shows:

- The Rust API started successfully.
- The x86_64 worker initialized successfully.
- A previous-session restore returned `session_expired` (HTTP 403), which
  preceded a new interactive login and did not prevent it.
- `POST /session/login` returned HTTP 202, the expected pending-2FA response.
- The challenge completed with HTTP 200 and Wrapper emitted
  `session.authenticated`.
- `GET /session` and `GET /session/credentials` returned HTTP 200.

### Session-recovery and playback evidence

Later, after the native worker was stopped and replaced:

- The replacement x86_64 worker started.
- Wrapper emitted `session.recovered`.
- `GET /session/credentials` returned HTTP 200.
- Two playback requests returned HTTP 200.
- Two `/key` requests returned HTTP 200.

This validates the fresh-login/2FA path and the exercised x86_64 session-recovery,
playback, and key/context HTTP paths in the reported environment. It does not
validate every catalog item, every media codec, all playback edge cases, or
the internal correctness of context bytes beyond the successful endpoint
responses in this test.

### ARM64 qualification

The identity generation and validation reside in shared Rust code, not in
architecture-specific native capture files. Thus, the device-info change is
expected to apply identically on ARM64. However, this reproduction did not
build or run an ARM64 image and did not test ARM64 authentication, playback,
or context capture. ARM64 must remain marked unverified until those steps are
performed on an ARM64-capable or emulated Buildx/runtime environment.

### Sanitized log evidence (relevant events)

```jsonl
{"timestamp":"2026-11-26T01:52:54.580290Z","level":"INFO","event":"server.started","version":"1.0.0","protocol":1,"architecture":"x86_64","host":"0.0.0.0","port":80}
{"timestamp":"2026-11-26T01:53:31.644440Z","level":"INFO","event":"http.request","request_id":2,"method":"GET","path":"/session","status":200,"duration_ms":1013}
{"timestamp":"2026-11-26T01:58:32.479258Z","level":"WARN","event":"worker.error","operation":"session.restore","code":"session_expired","restart_required":false}
{"timestamp":"2026-11-26T01:58:32.479323Z","level":"WARN","event":"http.request","request_id":6,"method":"POST","path":"/session/restore","status":403,"duration_ms":1,"code":"session_expired"}
{"timestamp":"2026-11-26T01:58:33.853034Z","level":"INFO","event":"http.request","request_id":8,"method":"GET","path":"/session","status":200,"duration_ms":0}
{"timestamp":"2026-11-26T01:58:46.069488Z","level":"INFO","event":"http.request","request_id":9,"method":"POST","path":"/session/login","status":202,"duration_ms":6311}
{"timestamp":"2026-11-26T01:59:10.838309Z","level":"INFO","event":"session.authenticated"}
{"timestamp":"2026-11-26T01:59:10.838357Z","level":"INFO","event":"http.request","request_id":10,"method":"POST","path":"/session/challenge","status":200,"duration_ms":4444}
{"timestamp":"2026-11-26T01:59:10.846869Z","level":"INFO","event":"http.request","request_id":11,"method":"GET","path":"/session","status":200,"duration_ms":0}
{"timestamp":"2026-11-26T01:59:10.853881Z","level":"INFO","event":"http.request","request_id":12,"method":"GET","path":"/session/credentials","status":200,"duration_ms":0}
{"timestamp":"2026-11-26T02:14:39.596825Z","level":"INFO","event":"worker.started","architecture":"x86_64"}
{"timestamp":"2026-11-26T02:14:43.461339Z","level":"INFO","event":"session.recovered"}
{"timestamp":"2026-11-26T02:14:43.461481Z","level":"INFO","event":"http.request","request_id":17,"method":"GET","path":"/session","status":200,"duration_ms":4540}
{"timestamp":"2026-11-26T02:14:43.471588Z","level":"INFO","event":"http.request","request_id":18,"method":"GET","path":"/session/credentials","status":200,"duration_ms":0}
{"timestamp":"2026-11-26T02:14:45.062051Z","level":"INFO","event":"http.request","request_id":19,"method":"GET","path":"/playback","status":200,"duration_ms":356}
{"timestamp":"2026-11-26T02:14:45.388081Z","level":"INFO","event":"http.request","request_id":20,"method":"GET","path":"/playback","status":200,"duration_ms":317}
{"timestamp":"2026-11-26T02:14:47.230518Z","level":"INFO","event":"http.request","request_id":21,"method":"GET","path":"/key","status":200,"duration_ms":321}
{"timestamp":"2026-11-26T02:14:47.609184Z","level":"INFO","event":"http.request","request_id":22,"method":"GET","path":"/key","status":200,"duration_ms":309}
```

No username, password, 2FA code, cookie, token value, signed URL, or account
identifier appears in this event excerpt.
