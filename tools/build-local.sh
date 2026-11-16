#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
arch=${WRAPPER_ARCH:-$(uname -m)}
if [[ ${1:-} == --arch ]]; then arch=$2; shift 2; fi
[[ $# == 0 ]] || { echo "usage: tools/build-local.sh [--arch x86_64|arm64-v8a]" >&2; exit 2; }
arch=$(python3 tools/architecture.py --arch "$arch" --field architecture)
image=${WORKER_BUILD_IMAGE:-wrapper-v2:native-build}
libcxx=${APPLE_LIBCXX:-$PWD/build/$arch/apple-libs/libc++_shared.so}
test -f "$libcxx"
docker run --rm --network none --user "$(id -u):$(id -g)" \
  -v "$PWD:/work" -v "$libcxx:/apple/libc++_shared.so:ro" \
  -e WORKER_ARCH="$arch" --entrypoint /bin/bash "$image" -c '
set -euo pipefail
ndk=/opt/android-ndk-r23b
deps=/opt/wrapper-deps
export SOURCE_DATE_EPOCH=0
test "$(git -c safe.directory="$deps/dobby-src" -C "$deps/dobby-src" rev-parse HEAD)" = 9d271803881f467aabfc24593c7dcaeb710fe861
test "$(git -c safe.directory="$deps/json-src" -C "$deps/json-src" rev-parse HEAD)" = 9cca280a4d0ccf0c08f47a99aa71d1b0e52f8d03
grep -q "Pkg.Revision = 23.1.7779620" "$ndk/source.properties"
arch=$WORKER_ARCH
build=/work/build/$arch
cmake -S /work -B "$build/android" -G Ninja \
 -DCMAKE_TOOLCHAIN_FILE="$ndk/build/cmake/android.toolchain.cmake" \
 -DANDROID_ABI="$arch" -DANDROID_PLATFORM=android-21 -DANDROID_STL=c++_shared \
 -DCMAKE_BUILD_TYPE=Release -DAPPLE_LIBCXX=/apple/libc++_shared.so \
 -DFETCHCONTENT_SOURCE_DIR_JSON="$deps/json-src" -DFETCHCONTENT_SOURCE_DIR_DOBBY="$deps/dobby-src"
cmake --build "$build/android" --target apple-music-worker common_tests -j2
native=$(python3 /work/tools/architecture.py --field architecture)
host_flags=()
if [[ "$arch" != "$native" ]]; then
  linux_arch=$(python3 /work/tools/architecture.py --arch "$arch" --field linux_architecture)
  host_flags=(-DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR="$linux_arch"
              -DCMAKE_CXX_COMPILER="$linux_arch-linux-gnu-g++")
fi
cmake -S /work -B "$build/host" -G Ninja -DWORKER_ARCH="$arch" \
 -DFETCHCONTENT_SOURCE_DIR_JSON="$deps/json-src" "${host_flags[@]}"
cmake --build "$build/host" -j2
if [[ "$arch" == "$native" ]]; then ctest --test-dir "$build/host" --output-on-failure; fi
'
