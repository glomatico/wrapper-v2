# Runtime-only packaging: build native and Rust binaries on the host first.
# The build context deliberately excludes APKs, Android libraries, rootfs,
# cached sessions, and other user-private files.
FROM debian:13.2
ARG TARGETARCH

RUN apt-get update \
    && apt-get install -y --no-install-recommends ca-certificates libcap2-bin curl \
    && rm -rf /var/lib/apt/lists/*

COPY --chmod=0755 build/package/${TARGETARCH}/wrapperd /usr/local/bin/wrapperd
COPY --chmod=0755 build/package/${TARGETARCH}/worker-launch /usr/local/bin/worker-launch

# Only the launcher needs filesystem namespace/chroot capabilities. The HTTP
# server runs as an ordinary UID; successful exec of the native worker drops
# the launcher's file capabilities again.
RUN setcap cap_sys_admin,cap_sys_chroot+ep /usr/local/bin/worker-launch

LABEL org.opencontainers.image.title="Wrapper-v2" \
      org.opencontainers.image.version="1.0.0" \
      org.opencontainers.image.description="Standalone Temari-compatible Apple Music context provider"

ENV WRAPPER_HOST=0.0.0.0 \
    WRAPPER_PORT=80 \
    WRAPPER_LAUNCHER=/usr/local/bin/worker-launch \
    WRAPPER_ROOTFS=/srv/wrapper/rootfs

WORKDIR /srv/wrapper
USER 1000:1000
EXPOSE 80
ENTRYPOINT ["/usr/local/bin/wrapperd"]
