# wrapper-v2 image.
#
# This Dockerfile assumes that rootfs/system/lib64/ has already been populated
# by the host (or in CI) for the same TARGET_ARCH. The image does not download
# or extract Apple Music artifacts.
#
# The build stage is always linux/amd64: Google only publishes the Linux NDK as
# an x86_64-host zip (android-ndk-r*b-linux.zip). On TARGET_ARCH=arm64-v8a we
# cross-compile the host `wrapper` with gcc-aarch64-linux-gnu; the daemon is
# still built with the NDK for ANDROID_ABI=arm64-v8a.
#
# Final image platform (e.g. linux/arm64 vs linux/amd64) comes from
# RUNTIME_PLATFORM and must match TARGET_ARCH / the staged rootfs.
#
# Example:
#   docker build --build-arg TARGET_ARCH=arm64-v8a \
#     --build-arg RUNTIME_PLATFORM=linux/arm64 -t wrapper-v2:arm64 .

ARG RUNTIME_PLATFORM=linux/amd64
# Kept for docker-compose compatibility; the compile stage ignores this.
ARG BUILD_PLATFORM=linux/amd64

# -----------------------------------------------------------------------------
# Build stage (always amd64 — official NDK is x86_64-hosted)
# -----------------------------------------------------------------------------
FROM --platform=linux/amd64 debian:13.2 AS build

ARG TARGET_ARCH=x86_64
ARG CMAKE_BUILD_TYPE=Release
ARG NDK_VERSION=23
SHELL ["/bin/bash", "-c"]
ENV DEBIAN_FRONTEND=noninteractive

RUN --mount=type=cache,target=/var/lib/apt,sharing=locked \
    --mount=type=cache,target=/var/cache/apt,sharing=locked \
    apt-get update && \
    apt-get install -y --no-install-recommends \
        build-essential \
        ca-certificates \
        cmake \
        curl \
        git \
        jq \
        ninja-build \
        unzip \
    && if [[ "$TARGET_ARCH" == "arm64-v8a" ]]; then \
         apt-get install -y --no-install-recommends \
           gcc-aarch64-linux-gnu g++-aarch64-linux-gnu; \
       fi \
    && rm -rf /var/lib/apt/lists/*

RUN curl -fSL -o /tmp/ndk.zip \
        "https://dl.google.com/android/repository/android-ndk-r${NDK_VERSION}b-linux.zip" && \
    unzip -q /tmp/ndk.zip -d /opt && \
    rm /tmp/ndk.zip
ENV ANDROID_NDK_HOME=/opt/android-ndk-r${NDK_VERSION}b

ENV CARGO_HOME=/usr/local/cargo \
    RUSTUP_HOME=/usr/local/rustup \
    PATH=/usr/local/cargo/bin:${PATH}
RUN curl -fsSL https://sh.rustup.rs | sh -s -- -y --profile minimal --default-toolchain stable

WORKDIR /app
COPY . /app

RUN test -f rootfs/system/bin/linker64 || { \
        echo "ERROR: rootfs/system/bin/linker64 is missing." >&2; \
        echo "Run tools/stage-system.sh on the host before docker build." >&2; \
        exit 1; \
    } && \
    test -d rootfs/system/lib64 && \
    ls rootfs/system/lib64/*.so >/dev/null 2>&1 || { \
        echo "ERROR: rootfs/system/lib64/ has no .so files." >&2; \
        echo "Stage the required Apple Music native libraries before docker build." >&2; \
        exit 1; \
    }

RUN host_cc=(); \
    if [[ "$TARGET_ARCH" == "arm64-v8a" ]]; then \
      host_cc=( \
        -DCMAKE_C_COMPILER=aarch64-linux-gnu-gcc \
        -DCMAKE_CXX_COMPILER=aarch64-linux-gnu-g++ \
      ); \
    fi && \
    cmake -S . -B build -G Ninja \
        -DTARGET_ARCH="${TARGET_ARCH}" \
        "${host_cc[@]}" \
        -DCMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE}" && \
    cmake --build build -j

RUN if [[ "$TARGET_ARCH" == "arm64-v8a" ]]; then \
      rustup target add aarch64-unknown-linux-gnu && \
      mkdir -p .cargo && \
      printf '[target.aarch64-unknown-linux-gnu]\nlinker = "aarch64-linux-gnu-gcc"\n' > .cargo/config.toml && \
      cargo build --release --target aarch64-unknown-linux-gnu && \
      cp target/aarch64-unknown-linux-gnu/release/wrapperd /app/wrapperd; \
    else \
      cargo build --release && \
      cp target/release/wrapperd /app/wrapperd; \
    fi

# -----------------------------------------------------------------------------
# Runtime stage
# -----------------------------------------------------------------------------
FROM --platform=${RUNTIME_PLATFORM} debian:13.2

WORKDIR /app

COPY --from=build /app/wrapper        /app/wrapper
COPY --from=build /app/wrapperd       /app/wrapperd
COPY --from=build /app/rootfs         /app/rootfs

# Apple's libcurl inside the chroot needs CA certificates for SSL verification.
# The build stage has ca-certificates installed; copy the bundle into the rootfs
# where the launcher will point SSL_CERT_FILE / CURL_CA_BUNDLE at it.
COPY --from=build /etc/ssl/certs/ca-certificates.crt /app/rootfs/etc/ssl/certs/ca-certificates.crt

EXPOSE 80 10020

ENTRYPOINT ["/app/wrapperd"]
