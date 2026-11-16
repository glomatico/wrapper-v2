#!/usr/bin/env bash
# The image contains only Linux executables. User-supplied libraries stay local.
set -euo pipefail
cd "$(dirname "$0")/.."
arch=${WRAPPER_ARCH:-$(uname -m)}
if [[ ${1:-} == --arch ]]; then arch=$2; shift 2; fi
[[ $# == 0 ]] || { echo "usage: tools/package.sh [--arch x86_64|arm64-v8a]" >&2; exit 2; }
arch=$(python3 tools/architecture.py --arch "$arch" --field architecture)
target=$(python3 tools/architecture.py --arch "$arch" --field rust_target)
platform=$(python3 tools/architecture.py --arch "$arch" --field docker_platform)
linux_arch=$(python3 tools/architecture.py --arch "$arch" --field linux_architecture)
native_arch=$(python3 tools/architecture.py --field architecture)
build=$PWD/build/$arch
test -f "$build/android/apple-music-worker"
test -f "$build/host/worker-launch"
test -f "$build/rootfs/system/lib64/libCoreLSKD.so"
test -f "$build/rootfs/system/bin/linker64"
test ! -L "$build/rootfs/system/bin/apple-music-worker"
# Cross-builds must use a target linker, not the host's default `cc`.
if [[ "$arch" != "$native_arch" ]]; then
  linker_var="CARGO_TARGET_${target^^}_LINKER"
  linker_var=${linker_var//-/_}
  linker=${!linker_var:-${CROSS_CC:-$linux_arch-linux-gnu-gcc}}
  if ! command -v "$linker" >/dev/null 2>&1; then
    echo "Missing $arch Linux cross-linker: $linker (set $linker_var or CROSS_CC)" >&2
    exit 1
  fi
  export "$linker_var=$linker"
fi
cargo build --release --locked --target "$target"
python3 tools/verify_artifacts.py --arch "$arch" \
  "$build/android/apple-music-worker" "$build/host/worker-launch" \
  "target/$target/release/wrapperd" "$build/rootfs/system/bin/linker64"
test -d data || mkdir -m 700 data
test -w data
package="build/package/${platform#linux/}"
mkdir -p "$package"
install -m 755 "target/$target/release/wrapperd" "$package/wrapperd"
install -m 755 "$build/host/worker-launch" "$package/worker-launch"
install -m 755 "$build/android/apple-music-worker" "$build/rootfs/system/bin/apple-music-worker"
docker build --platform "$platform" --tag "wrapper-v2:1.0.0-local-${platform#linux/}" --file Dockerfile .
echo "Built local image for $platform. Set WRAPPER_ROOTFS_DIR=$build/rootfs."
