# Native worker lifecycle

Each invocation of `worker-launch ROOTFS` creates a private mount namespace and a new PID namespace. Rust supervises the launcher in the container namespace. The launcher's first child becomes PID 1 in the worker namespace, then forks the Android worker as PID 2. The worker mounts a read-only procfs for its PID namespace, binds the existing devices, enters the Android rootfs and execs the staged binary. Each replacement starts with a fresh PID allocator.

## Why the worker needs its own PID namespace

On both supported architectures (ARM64 and x86-64), the pinned Android linker and libc encode the owner of a recursive mutex in the upper 16 bits of a 32-bit word. The lock operation compares that field with the full thread ID. Above 65535 a thread can wait indefinitely trying to re-acquire its own lock.

With the original launcher, a local ARM64 reproduction answered a framed shutdown request at PID 65535 and blocked before `main()` at PID 65536. The futex address was `linker64 + 0x22120`, the linker's `g_dl_mutex`. Its wait word was `0x4002`, with owner zero. The pinned linker's owner comparison is at `0x9e50` and its 32-bit shift for storing the owner is at `0x9e5c`. The matching instructions in libc are at `0x20440` and `0x2044c`. This explains why replacement workers kept timing out once the container's allocator crossed the boundary. The trigger for the original worker exit is a separate question.

The native x86-64 worker reproduced the same boundary. At PID 65536 its futex wait was at the x86-64 linker’s `g_dl_mutex` (offset `0x17040`) with wait word `0x4002`. With the fixed launcher, the worker had outer PID 75002, saw PID 2 inside, and answered the shutdown request.

Isolation resets thread numbering at each launch. The same Bionic limit would still apply if a single worker allocated enough threads during its own lifetime to reach TID 65536.

## Supervision and cleanup

The worker inherits the existing framed IPC on stdin and stdout. Namespace PID 1 waits for the worker and reaps orphaned children. It also watches the response pipe for loss of its Rust reader and a separate pipe for loss of the launcher. Pipe closure stays observable across startup races, including a worker stuck before installing its own parent-death signal.

When the worker exits, PID 1 returns its exit code (or 128 plus its terminating signal). Closing the response reader during a recycle also wakes PID 1. Linux then kills and reaps the remaining namespace processes. The launcher waits for PID 1, so normal replacement waits for teardown and release of the session lock. Rust allows five seconds for the launcher to exit before forcing termination. Both waiting processes clear their capabilities. [Linux PID namespace semantics](https://man7.org/linux/man-pages/man7/pid_namespaces.7.html)

## Operation budget and recovery

The native worker owns a budget of 128 playback and context attempts by default. After authentication and input validation it charges one slot just before entering the native operation. A failed native call can therefore consume a slot. Session status, credentials and explicit close remain usable at the limit.

The next budgeted request receives an internal `worker_recycle` with `restart_required` before the worker executes the operation. Rust holds the supervisor lock, replaces the worker, revalidates the previously authenticated session and submits the request once to the replacement. Gamdl sees the resulting playback or context response. A second recycle from a fresh worker remains a visible 409. Crashes, timeouts, malformed replies and native operation errors propagate to the caller. None establishes that the requested work was rejected before execution.

If automatic session restoration fails, a protected request receives the restoration error. A later request can attempt recovery again after a transport or validation failure. A missing or expired cache clears the intent to restore. Explicit restore performs one validation, and explicit close skips recovery. Once an HTTP request acquires the worker, its bounded IPC transaction runs through cleanup even if the client disconnects. Overlapping callers retain the existing `503 worker_busy` behavior.

A supervisor count of successful replies would miss charged failures. A native notification on the final charged response could save one local IPC exchange per generation, but replacement would still incur startup and authentication costs. Eager replacement would also incur those costs when the next action is close or the client goes idle. Changing the limit warrants a longer per-generation measurement of memory, threads, file descriptors, failures and restoration latency on representative workloads.

## Container deployment

Build the private image with `tools/build-docker.sh --arch x86_64` (or `arm64-v8a`) and your pinned Apple Music APK. The Dockerfile compiles `wrapperd`, `worker-launch`, and the native Android worker, then embeds the verified Android rootfs and extracted Apple libraries in the resulting image. **Do not publish this image**, as it contains proprietary Apple code. Keep the private session volume and device configuration across image rebuilds; do not mount a host rootfs over the embedded one. The Compose service enables `init: true` so the container init can reap a launcher or namespace init orphaned by an abrupt kill. Use `--init` for an equivalent `docker run`.

The launcher uses the existing `CAP_SYS_ADMIN` for namespace and mount operations and `CAP_SYS_CHROOT` for chroot. Preserve its image file capabilities and the existing Compose security options. The HTTP service runs as the configured non-root UID. A custom seccomp or LSM policy must permit the combined `unshare(CLONE_NEWNS | CLONE_NEWPID)`, a procfs mount, and the existing device bind and chroot operations. Namespace setup fails closed on error.

After recreating a container, `GET /session` exercises the native handshake. `GET /health` only checks the HTTP service. From the container's procfs, the worker's `/proc/<worker-pid>/status` should show an `NSpid` line ending in 2. For a previously authenticated session, check for `session.auto_auth_restored` on configured auto-auth startup or `session.recovered` after a worker recycle.

## Regression test

The Linux integration test uses a static probe in a disposable rootfs and creates its own outer PID and mount namespaces. Where the kernel's PID range permits it, the test advances only that outer namespace's allocation cursor above 70000. It checks the inner PID and procfs view, exit and signal status, orphan reaping, and teardown on IPC closure, launcher death and supervisor death.

```sh
g++ -std=c++17 -Wall -Wextra -Werror native/launcher.cpp -o /tmp/worker-launch
unshare --user --map-root-user python3 tests/launcher_namespace.py /tmp/worker-launch -v
```

On Linux hosts that restrict unprivileged user namespaces, run the Python command with `sudo`, as in CI. The test needs a C compiler with static libc and util-linux `unshare`.
