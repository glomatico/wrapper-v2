# vendor/android-system

Android system binaries needed at runtime by Apple's `.so` files inside the
chroot. Committed here so the build is fully self-contained except for the
non-redistributable Apple libraries.

## What's included

Per arch under `<arch>/`:

| File                        | Source              | Why                                                                                                                                                                                                                                                                               |
| --------------------------- | ------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `bin/linker64`              | AOSP `/system/bin/` | Android dynamic loader; resolves all `.so` dependencies.                                                                                                                                                                                                                          |
| `lib64/libc.so`             | bionic              | Mandatory C runtime.                                                                                                                                                                                                                                                              |
| `lib64/libm.so`             | bionic              | Math.                                                                                                                                                                                                                                                                             |
| `lib64/libstdc++.so`        | bionic              | Tiny stub. `libCoreFP`, `libCoreLSKD`, `libcurl` link against it.                                                                                                                                                                                                                 |
| `lib64/liblog.so`           | AOSP                | Android logging.                                                                                                                                                                                                                                                                  |
| `lib64/libz.so`             | AOSP                | zlib.                                                                                                                                                                                                                                                                             |
| `lib64/libandroid.so`       | AOSP                | NDK runtime API (loaded by 6 Apple libs).                                                                                                                                                                                                                                         |
| `lib64/libOpenSLES.so`      | AOSP                | Stub satisfying `DT_NEEDED` of `libandroidappmusic`. We never call into it.                                                                                                                                                                                                       |
| `usr/share/zoneinfo/tzdata` | AOSP                | Bionic timezone database. Optional; only suppresses `__bionic_open_tzdata` warnings on every `localtime`/`gmtime` call. Extract from any factory image at the same path (`simg2img system.img system.raw && debugfs -R "dump usr/share/zoneinfo/tzdata /tmp/tzdata" system.raw`). |

`libdl.so` is intentionally absent. Android's `linker64` itself exports the
`dl_*` symbols, so a separate `libdl.so` file is not required at runtime.
This matches how AOSP system images are laid out.

## License

These binaries are Apache-2.0 (AOSP). Redistribution is permitted; see
`LICENSE` at the repo root for the wrapper-v2 project license.

## Provenance

The **x86_64** binaries were taken verbatim from
[`zhaarey/wrapper`](https://github.com/zhaarey/wrapper) `rootfs/system/`,
which sourced them from an Android x86_64 system image (likely API 21-29
era based on file sizes and exported symbols).

The **arm64-v8a** set was taken verbatim from
[`WorldObservationLog/wrapper`](https://github.com/WorldObservationLog/wrapper)
branch **`arm64`** `rootfs/system/` (same layout as x86: `bin/linker64`,
`lib64/*.so`, etc.). The IANA timezone blob `usr/share/zoneinfo/tzdata` is
the same bytes on both arches (single file path; identical SHA-256 pin).

SHA-256 digests for both arches are pinned in
[`LIBS_VERSION.json`](../../LIBS_VERSION.json) under `.android_system.<arch>`
and verified by `tools/stage-system.sh`.

**Future work:** when reproducible provenance becomes important, replace
these blobs with a `tools/fetch-android-system.sh` step that pulls a
specific Google emulator system image (e.g.
`https://dl.google.com/android/repository/sys-img/android/x86_64-29_r07.zip`),
runs `simg2img` + `debugfs` to extract the same eight files, and compares
the result against the pins committed here. The pins should not need to
change.

## Adding a new arch

For any new ABI, add `vendor/android-system/<arch>/` with the same relative
paths as the existing arches, plus a matching `.android_system.<arch>` entry
in `LIBS_VERSION.json`.
