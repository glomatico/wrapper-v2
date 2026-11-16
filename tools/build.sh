#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
arch=${WRAPPER_ARCH:-$(uname -m)}
if [[ ${1:-} == --arch ]]; then arch=$2; shift 2; fi
[[ $# == 0 ]] || { echo "usage: tools/build.sh [--arch x86_64|arm64-v8a]" >&2; exit 2; }
arch=$(python3 tools/architecture.py --arch "$arch" --field architecture)
host_arch=$(python3 tools/architecture.py --arch "$arch" --field linux_architecture)
native_arch=$(python3 tools/architecture.py --field architecture)
: "${ANDROID_NDK_HOME:?Set ANDROID_NDK_HOME to Android NDK r23b}"
libcxx=${APPLE_LIBCXX:-$PWD/build/$arch/apple-libs/libc++_shared.so}
grep -q 'Pkg.Revision = 23.1.7779620' "$ANDROID_NDK_HOME/source.properties" || {
  echo "Only NDK r23b (23.1.7779620) is validated" >&2; exit 1;
}
export SOURCE_DATE_EPOCH=0
build="$PWD/build/$arch"
cmake -S . -B "$build/android" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI="$arch" -DANDROID_PLATFORM=android-21 -DANDROID_STL=c++_shared \
  -DAPPLE_LIBCXX="$libcxx" -DCMAKE_BUILD_TYPE=Release
cmake --build "$build/android" --target apple-music-worker common_tests -j2
host_flags=()
if [[ "$arch" != "$native_arch" ]]; then
  host_flags=(-DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR="$host_arch"
              -DCMAKE_CXX_COMPILER="${HOST_CXX:-$host_arch-linux-gnu-g++}")
fi
cmake -S . -B "$build/host" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DWORKER_ARCH="$arch" "${host_flags[@]}"
cmake --build "$build/host" -j2
if [[ "$arch" == "$native_arch" ]]; then ctest --test-dir "$build/host" --output-on-failure; fi
