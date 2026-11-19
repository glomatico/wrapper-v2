# Third-party licenses and notices

Wrapper v2 source code is released under [The Unlicense](LICENSE). That license
**does not apply** to the Android binaries under `vendor/android-system/`.
Those files retain their original copyright notices and licenses.

## Original notices from Google

The following files are copied **verbatim** from the official Google Android
system-image archives used to extract the bundled libraries:

- [x86-64 — Android 5.0.2 (API 21) notices](vendor/android-system/x86_64/NOTICE.txt)
- [ARM64 — Android 5.1.1 (API 22) notices](vendor/android-system/arm64-v8a/NOTICE.txt)

Both are the complete upstream `NOTICE.txt` files, including notices for
additional components of the original system image which are **not** bundled
here. They were retained intact rather than selectively edited or paraphrased.
The official system-image packages and their checksums are documented in
[vendor/android-system/README.md](vendor/android-system/README.md).

## License families

The system images include multiple projects and licenses. Among them:

| Component family | License / attribution | Upstream source |
| --- | --- | --- |
| Android platform/framework | Apache License 2.0, plus file-specific notices | [AOSP](https://android.googlesource.com/platform/frameworks/base/) |
| Bionic libc and dynamic linker | BSD-style and Apache-licensed components | [AOSP Bionic](https://android.googlesource.com/platform/bionic/) |
| OpenSSL 1.0.1 series (`libssl`, `libcrypto`) | **OpenSSL License and Original SSLeay License** | [AOSP external/openssl](https://android.googlesource.com/platform/external/openssl/) |
| FreeType | FreeType Project License / GPL dual-licensing | [AOSP external/freetype](https://android.googlesource.com/platform/external/freetype/) |
| ICU | ICU License | [AOSP external/icu4c](https://android.googlesource.com/platform/external/icu4c/) |
| LLVM | University of Illinois/NCSA license | [AOSP external/llvm](https://android.googlesource.com/platform/external/llvm/) |
| libpng | libpng License | [AOSP external/libpng](https://android.googlesource.com/platform/external/libpng/) |
| SQLite | Public-domain dedication | [AOSP external/sqlite](https://android.googlesource.com/platform/external/sqlite/) |
| Other bundled libraries | Their individual upstream licenses and attributions | Official `NOTICE.txt` files above and [AOSP source](https://android.googlesource.com/) |

The tables are a **navigation aid, not a replacement for the actual license
texts**. Applicable notices are shipped in the two original upstream NOTICE
files. Some original Android image components are not individually enumerated
by filename in those files; consult the tagged upstream source for component-
specific license headers and attribution obligations. Do not assume every
Android binary is Apache-licensed.

The Google Android SDK distribution terms also apply to Google's system-image
packages except to the extent that an individual open-source component's own
license governs its redistribution; consult the [SDK terms](https://developer.android.com/studio/terms)
and original source licenses for the precise conditions.

## Excluded materials

This project **does not redistribute** the Apple Music APK, Apple proprietary
libraries, authentication credentials, or saved sessions. The original Apple
software and any private client data must be supplied separately by an
authorized operator and remain outside the published Docker image and Git
repository.

These notices do not provide a license to Apple's services, code or content.
This repository is not affiliated with or endorsed by Apple or Google.
