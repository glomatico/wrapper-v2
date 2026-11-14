#!/usr/bin/env bash
# Package already-built, pinned native worker/rootfs without ever including
# proprietary APK content or session state in the Docker image.
set -euo pipefail
cd "$(dirname "$0")/.."

test -f build/android/apple-music-worker || {
  echo "Missing native worker; build it with tools/build.sh or tools/build-local.sh" >&2
  exit 1
}
test -f build/rootfs/system/lib64/libCoreLSKD.so || {
  echo "Missing prepared rootfs; stage a user-supplied pinned APK first" >&2
  exit 1
}
test -f build/rootfs/system/bin/linker64 || {
  echo "Rootfs is missing its pinned Android linker; prepare it first" >&2
  exit 1
}
test ! -L build/rootfs/system/bin/apple-music-worker || {
  echo "Refusing to overwrite a symlinked rootfs worker" >&2
  exit 1
}
if [ ! -d data ]; then
  mkdir -m 700 data
fi
test -d data && test -w data || {
  echo "Session directory ./data must be writable by your configured container UID" >&2
  exit 1
}

cargo build --release --locked
mkdir -p build/host
${CXX:-c++} -std=c++17 -O2 -Wall -Wextra -Werror \
  native/launcher.cpp -o build/host/worker-launch
# Keep the pre-staged user-local rootfs executable synchronized with the
# private native build. Never package the rootfs itself into the Docker image.
install -m 755 build/android/apple-music-worker build/rootfs/system/bin/apple-music-worker
# Building an image must not require a populated .env or any account token.
docker build --tag wrapper-v2:1.0.0-local --file Dockerfile .
echo "Built local Docker image. No deployment or publication performed."
