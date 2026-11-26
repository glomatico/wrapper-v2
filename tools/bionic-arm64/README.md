# ARM64 Android 5.1.1 linker

Wrapper builds `vendor/android-system/arm64-v8a/bin/linker64` from the AOSP
release corresponding to Google's API 22 revision 2 ARM64 SDK image. The
archive's `arm64-v8a/build.prop` records `LMY48X`, incremental `8789835`.
The release tag is `android-5.1.1_r25`, the manifest commit is
`6053a232feae4f07d1da97ce81d3a33ed1c50038`, and the Bionic commit is
`63d2f6db2e13aefb7802f13d597d47583897df6d`. The control build verifies
this attribution against the executable contents of the SDK image.
[AOSP build IDs](https://source.android.com/docs/setup/reference/build-numbers) ·
[Bionic source](https://android.googlesource.com/platform/bionic/+/63d2f6db2e13aefb7802f13d597d47583897df6d/linker/linker.cpp) ·
[SDK archive and checksums](../../vendor/android-system/README.md)

| ARM64 linker | SHA-256 |
| --- | --- |
| Original SDK binary | `869ca3c5781c1c32f35d9067ae627e075181babf4996f7c3a20ff32610efc094` |
| Wrapper binary | `89f530cb4aee482c8e3dc34a43a94acfa18f0632a4a315583af92e21117b5006` |

## Compatibility

The kernel supplies the vDSO mapping through `AT_SYSINFO_EHDR`. API 22's
`add_vdso` publishes a library record before calling `PrelinkImage`.
Prelinking rejects a vDSO lacking the SysV `DT_HASH` tag. The SDK linker
ignores that failure. A later global `dlsym` search visits the record and
indexes its null bucket table.

On ARM64, the patch removes the record if prelinking or linking fails. It
clears the recorded mapping size before freeing the metadata because
`soinfo_free` would otherwise unmap the kernel's pages. Compatible SysV
vDSOs still register normally. For a GNU-hash-only vDSO, the kernel mapping
and the pinned libc's independent clock lookup remain available through the
auxiliary vector. The omitted object cannot supply symbols to global
`dlsym` or appear in Bionic's `dl_iterate_phdr` list. Ordinary libraries
continue to follow API 22's existing hash rules. The patch changes no
`soinfo` layout, public ABI, libc binary or x86-64 artifact.
[Source patch](vdso.patch) ·
[Bionic libc vDSO lookup](https://android.googlesource.com/platform/bionic/+/63d2f6db2e13aefb7802f13d597d47583897df6d/libc/bionic/vdso.cpp)

## Rebuild

Use an x86-64 Linux machine with Bash, Git, Docker and network access to AOSP,
Docker Hub and Ubuntu's Trusty package repositories. Run from the Wrapper
checkout and choose a new build directory. The build uses AOSP's platform
GCC 4.9, Clang 3.5 and `libc_nomalloc` for `sdk_phone_arm64-userdebug`.

```sh
bash tools/bionic-arm64/build.sh /var/tmp/wrapper-api22-rebuild
cmp /var/tmp/wrapper-api22-rebuild/linker64 vendor/android-system/arm64-v8a/bin/linker64
```

The script verifies the tag's Git commit for every checkout against
[sources.lock](sources.lock). It builds an unmodified control first.
The SDK build used filesystem traversal order to assemble its syscall archive.
[modules.mk](modules.mk) preserves the linked syscall member order recorded
in the SDK symbol table and sorts the remainder. The first `0x121f8` bytes
of the control, covering the ELF headers and both file-backed load segments
through the end of initialized data, must match the SDK digest for that range.
The original SDK and control can differ in non-loaded debug metadata.

The script then applies [vdso.patch](vdso.patch), rebuilds the ARM64 target,
and removes the debug link to the unshipped DWARF companion. Local function
symbols remain available to GDB. The output directory contains `linker64`
and `linker64.control`. The existing Wrapper staging process checks the
complete shipped file against its [profile pin](../../profiles/apple-music-1109-arm64-v8a.json).
The upstream linker license is reproduced in the [ARM64 notices](../../vendor/android-system/arm64-v8a/NOTICE.txt).

## Standalone regression

Set `ANDROID_NDK_HOME` to the Linux x86-64 NDK r23b (23.1.7779620). The test
cross-compiles an API 22 C executable and one SysV-hashed shared library.
The SSH target must be ARM64 Linux with a GNU-hash-only vDSO, util-linux,
GNU coreutils and permission to create a PID namespace and chroot. The
runner uses a temporary directory, drops to UID/GID 65534 for the probe,
and removes the directory on exit. It transfers only the probe, fixture,
selected linker, pinned libc and pinned libm.

```sh
bash tests/arm64-linker/run.sh netcup /var/tmp/wrapper-api22-rebuild/linker64.control
bash tests/arm64-linker/run.sh netcup
```

On Netcup's Debian 13 kernel `6.12.118+deb13-arm64` with 4096-byte pages,
the original SDK linker and the clean source control both pass the constructor
and handle lookup, then segfault during `dlsym(RTLD_DEFAULT, ...)` (exit 139).
Two independent builds produced the Wrapper digest above. The corrected
linker completes that lookup, reports an absent symbol through `dlerror`,
loads the pinned `libm.so`, calls `cos` and `clock_gettime`, and closes both
library handles (exit 0). Exit 77 means the host lacks the required vDSO
hash format. API 22 can print warnings for newer ELF tags while running
the probe. The two runs use the same public test fixtures and require no
Wrapper worker, APK, account, or session data.
