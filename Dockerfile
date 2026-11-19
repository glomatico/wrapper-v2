# syntax=docker/dockerfile:1.7
#
# Build *all* executables and the complete Android runtime in Docker.
# The Apple Music APK is passed in an isolated BuildKit named context and
# bind-mounted for extraction, without copying the raw archive into a layer.
# The resulting image intentionally CONTAINS proprietary Apple libraries:
# build and use privately; do not publish or redistribute this image.
#
# Android NDK r23b ships an x86-64 Linux host toolchain. Even when producing
# ARM64 images, compilation therefore runs on linux/amd64.
FROM --platform=linux/amd64 debian:13.2 AS builder

ENV DEBIAN_FRONTEND=noninteractive \
    ANDROID_NDK_HOME=/opt/android-ndk-r23b \
    CARGO_HOME=/root/.cargo \
    RUSTUP_HOME=/root/.rustup \
    PATH=/root/.cargo/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
       ca-certificates curl unzip python3 cmake ninja-build git \
       build-essential gcc-aarch64-linux-gnu g++-aarch64-linux-gnu \
    && rm -rf /var/lib/apt/lists/*

# Official Android NDK Linux archive with its Google SDK repository SHA-1 pin.
RUN curl --fail --location --retry 3 --output /tmp/android-ndk.zip \
      https://dl.google.com/android/repository/android-ndk-r23b-linux.zip \
    && echo 'f47ec4c4badd11e9f593a8450180884a927c330d  /tmp/android-ndk.zip' | sha1sum -c - \
    && unzip -q /tmp/android-ndk.zip -d /opt \
    && rm -f /tmp/android-ndk.zip \
    && grep -q 'Pkg.Revision = 23.1.7779620' /opt/android-ndk-r23b/source.properties

RUN curl --fail --location --retry 3 --show-error --silent https://sh.rustup.rs \
      | sh -s -- -y --profile minimal --default-toolchain stable \
    && rustup target add aarch64-unknown-linux-gnu x86_64-unknown-linux-gnu

# Share the expensive toolchain installation between both Docker targets.
ARG TARGETARCH
ARG APPLE_APK_SHA256
WORKDIR /work
# .dockerignore explicitly allowlists these source files and the public Android
# runtime. It excludes local APKs, build results, .env, and cached sessions.
COPY . .

# The APK's digest is also a cache key. No password or session is supplied.
RUN --mount=type=bind,from=apple_apk,source=AppleMusic.package,target=/run/apple-music.package,readonly \
    set -eu; \
    test -n "$APPLE_APK_SHA256"; \
    echo "$APPLE_APK_SHA256  /run/apple-music.package" | sha256sum -c -; \
    case "$TARGETARCH" in \
      amd64) arch=x86_64 ;; \
      arm64) arch=arm64-v8a ;; \
      *) echo "Unsupported Docker architecture: $TARGETARCH" >&2; exit 1 ;; \
    esac; \
    python3 tools/prepare_rootfs.py --arch "$arch" \
      --apk /run/apple-music.package \
      --extract-libraries "/work/build/$arch/apple-libs"

RUN set -eu; \
    case "$TARGETARCH" in \
      amd64) arch=x86_64; host_arch=x86_64 ;; \
      arm64) arch=arm64-v8a; host_arch=aarch64 ;; \
      *) exit 1 ;; \
    esac; \
    cmake -S . -B "build/$arch/android" -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" \
      -DANDROID_ABI="$arch" -DANDROID_PLATFORM=android-21 \
      -DANDROID_STL=c++_shared -DCMAKE_BUILD_TYPE=Release \
      -DAPPLE_LIBCXX="/work/build/$arch/apple-libs/libc++_shared.so"; \
    cmake --build "build/$arch/android" --target apple-music-worker common_tests -j2; \
    if [ "$TARGETARCH" = arm64 ]; then \
      cmake -S . -B "build/$arch/host" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DWORKER_ARCH="$arch" -DCMAKE_SYSTEM_NAME=Linux \
        -DCMAKE_SYSTEM_PROCESSOR="$host_arch" \
        -DCMAKE_CXX_COMPILER=aarch64-linux-gnu-g++; \
    else \
      cmake -S . -B "build/$arch/host" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DWORKER_ARCH="$arch"; \
    fi; \
    cmake --build "build/$arch/host" -j2

RUN --mount=type=cache,target=/root/.cargo/registry \
    set -eu; \
    case "$TARGETARCH" in \
      amd64) target=x86_64-unknown-linux-gnu ;; \
      arm64) target=aarch64-unknown-linux-gnu; \
             export CARGO_TARGET_AARCH64_UNKNOWN_LINUX_GNU_LINKER=aarch64-linux-gnu-gcc ;; \
      *) exit 1 ;; \
    esac; \
    cargo build --release --locked --target "$target"

# Rootfs staging deliberately happens only after successful compilation and
# uses the verified vendored Android files plus the private extracted Apple
# libraries. Never copy accounts, preexisting rootfs, or session state.
RUN --mount=type=bind,from=apple_apk,source=AppleMusic.package,target=/run/apple-music.package,readonly \
    set -eu; \
    case "$TARGETARCH" in \
      amd64) arch=x86_64; target=x86_64-unknown-linux-gnu ;; \
      arm64) arch=arm64-v8a; target=aarch64-unknown-linux-gnu ;; \
      *) exit 1 ;; \
    esac; \
    echo "$APPLE_APK_SHA256  /run/apple-music.package" | sha256sum -c -; \
    mkdir -p /opt/wrapper; \
    python3 tools/prepare_rootfs.py --arch "$arch" \
      --apk /run/apple-music.package \
      --output /opt/wrapper/rootfs; \
    python3 tools/verify_artifacts.py --arch "$arch" \
      "build/$arch/android/apple-music-worker" \
      "build/$arch/host/worker-launch" \
      "target/$target/release/wrapperd" \
      /opt/wrapper/rootfs/system/bin/linker64; \
    install -m 755 "build/$arch/host/worker-launch" /opt/wrapper/worker-launch; \
    install -m 755 "target/$target/release/wrapperd" /opt/wrapper/wrapperd

FROM debian:13.2
RUN apt-get update \
    && apt-get install -y --no-install-recommends ca-certificates libcap2-bin curl \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder --chmod=0755 /opt/wrapper/wrapperd /usr/local/bin/wrapperd
COPY --from=builder --chmod=0755 /opt/wrapper/worker-launch /usr/local/bin/worker-launch
COPY --from=builder --chown=1000:1000 /opt/wrapper/rootfs /srv/wrapper/rootfs
COPY --from=builder /work/THIRD_PARTY_NOTICES.md /usr/share/doc/wrapper-v2/THIRD_PARTY_NOTICES.md
COPY --from=builder /work/vendor/android-system/x86_64/NOTICE.txt /usr/share/doc/wrapper-v2/NOTICE-android-x86_64.txt
COPY --from=builder /work/vendor/android-system/arm64-v8a/NOTICE.txt /usr/share/doc/wrapper-v2/NOTICE-android-arm64.txt

RUN setcap cap_sys_admin,cap_sys_chroot+ep /usr/local/bin/worker-launch

LABEL org.opencontainers.image.title="Wrapper-v2" \
      org.opencontainers.image.version="1.0.0" \
      org.opencontainers.image.description="Private, self-built Temari-compatible Apple Music context provider"

ENV WRAPPER_HOST=0.0.0.0 \
    WRAPPER_PORT=80 \
    WRAPPER_LAUNCHER=/usr/local/bin/worker-launch \
    WRAPPER_ROOTFS=/srv/wrapper/rootfs

WORKDIR /srv/wrapper
USER 1000:1000
EXPOSE 80
ENTRYPOINT ["/usr/local/bin/wrapperd"]
