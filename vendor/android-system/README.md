# Bundled Android system runtime

The Android libraries, time zone data and x86-64 linker come from official
Google Android SDK system images. The ARM64 linker is built from the
corresponding AOSP release with a vDSO compatibility patch. The SDK archive
integrity was checked against Google's repository metadata before extraction.
Every included runtime file is pinned by SHA-256
in the corresponding `profiles/apple-music-1109-<architecture>.json` file.

| Bundle | Official system image | ZIP SHA-1 (Google repository metadata) | ZIP SHA-256 |
| --- | --- | --- | --- |
| `x86_64/` | [Android 5.0.2 / API 21 revision 5](https://dl.google.com/android/repository/sys-img/android/x86_64-21_r05.zip) | `9078a095825a69e5e215713f0866c83cef65a342` | `b652297c0733cced8e9507e5018d78f56b87141c47e4d7ada60f495469f76d49` |
| `arm64-v8a/` | [Android 5.1.1 / API 22 revision 2](https://dl.google.com/android/repository/sys-img/android/arm64-v8a-22_r02.zip) | `703e27a9a4fb7a6e763cb7d713b89e5249a8fc99` | `8176b9fdd3d9e2b031d654b60f6d327c2780927b8a06acf4af835e308c0238bf` |

Google's package index: <https://dl.google.com/android/repository/sys-img/android/sys-img2-1.xml>

The ARM64 SDK image reports Android 5.1.1, build `LMY48X`, incremental
`8789835`. Its original `bin/linker64` SHA-256 is
`869ca3c5781c1c32f35d9067ae627e075181babf4996f7c3a20ff32610efc094`.
The ARM64 profile pins the rebuilt linker. The original complete `NOTICE.txt`
files accompany both runtimes. See the [ARM64 build recipe and standalone
regression](../../tools/bionic-arm64/README.md).

## Independent verification

1. Download the appropriate ZIP from the Google URL above and verify its
   SHA-1 and SHA-256 checksums.
2. Extract `system.img` and `NOTICE.txt` from the ZIP. Each `system.img` is
   an ext4 filesystem; the paths under its root correspond to `bin/`,
   `lib64/` and `usr/` in this directory. You can use `debugfs` to extract
   individual files without mounting the image.
3. Compare extracted files against the `android_system` map in `profiles/`,
   using the original hash above for the SDK ARM64 linker. Build the patched
   linker with the linked recipe and compare its digest to the ARM64 profile.
   `tests/test_vendor_android.py` verifies the checked-in files and inventory.

Example for a locally downloaded x86-64 package:

```sh
sha1sum x86_64-21_r05.zip
7z e -so x86_64-21_r05.zip x86_64/system.img > system.img
debugfs -R 'dump /bin/linker64 ./linker64' system.img
sha256sum linker64
```

See [THIRD_PARTY_NOTICES.md](../../THIRD_PARTY_NOTICES.md) for attribution
and redistribution information. These libraries are **not licensed under the
Wrapper source code's Unlicense**. Apple's proprietary libraries are excluded
from Git but embedded in the *privately built* Docker image; authentication
credentials and session data are excluded from both.
