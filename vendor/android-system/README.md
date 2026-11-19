# Bundled Android system runtime

These files were **extracted directly from official Google Android SDK system
images**, not copied from the previous Wrapper v2 repository or an unofficial
binary mirror. Their archive integrity was checked against Google's repository
metadata before extraction. Every included runtime file is pinned by SHA-256
in the corresponding `profiles/apple-music-1109-<architecture>.json` file.

| Bundle | Official system image | ZIP SHA-1 (Google repository metadata) | ZIP SHA-256 |
| --- | --- | --- | --- |
| `x86_64/` | [Android 5.0.2 / API 21 revision 5](https://dl.google.com/android/repository/sys-img/android/x86_64-21_r05.zip) | `9078a095825a69e5e215713f0866c83cef65a342` | `b652297c0733cced8e9507e5018d78f56b87141c47e4d7ada60f495469f76d49` |
| `arm64-v8a/` | [Android 5.1.1 / API 22 revision 2](https://dl.google.com/android/repository/sys-img/android/arm64-v8a-22_r02.zip) | `703e27a9a4fb7a6e763cb7d713b89e5249a8fc99` | `8176b9fdd3d9e2b031d654b60f6d327c2780927b8a06acf4af835e308c0238bf` |

Google's package index: <https://dl.google.com/android/repository/sys-img/android/sys-img2-1.xml>

The `bin/linker64`, architecture-specific `lib64/*.so` libraries, and
`usr/share/zoneinfo/tzdata` were copied *unchanged* from each system image's
root filesystem, without patches or rebuilt binaries. All binaries from the
previous Wrapper bundle match their official counterparts byte-for-byte **except
`tzdata`**. The latter was replaced by the version shipped in each respective
official image and given its own SHA-256 pin. The original official archives'
complete `NOTICE.txt` files are included alongside the binaries.

## Independent verification

1. Download the appropriate ZIP from the Google URL above and verify its
   SHA-1 and SHA-256 checksums.
2. Extract `system.img` and `NOTICE.txt` from the ZIP. Each `system.img` is
   an ext4 filesystem; the paths under its root correspond to `bin/`,
   `lib64/` and `usr/` in this directory. You can use `debugfs` to extract
   individual files without mounting the image.
3. Compare their SHA-256 hashes to the matching `android_system` map in
   `profiles/`. The project's automated `test_vendor_android.py` repeats the
   hash and file-inventory verification for the checked-in bundle.

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
