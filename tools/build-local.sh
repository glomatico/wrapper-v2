#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
# Existing local build image supplies the pinned NDK and pinned dependency trees.
# No old Wrapper sources are compiled or linked.
image=${WORKER_BUILD_IMAGE:-wrapper-v2:temari-build}
libcxx=${APPLE_LIBCXX:-$PWD/build/rootfs/system/lib64/libc++_shared.so}
test -f "$libcxx"
docker run --rm --network none --user "$(id -u):$(id -g)" \
  -v "$PWD:/work" -v "$libcxx:/apple/libc++_shared.so:ro" --entrypoint /bin/bash "$image" -c '
set -euo pipefail
ndk=/opt/android-ndk-r23b
deps=/app/build/daemon-x86_64/_deps
export SOURCE_DATE_EPOCH=0
test "$(git -c safe.directory="$deps/dobby-src" -C "$deps/dobby-src" rev-parse HEAD)" = 9d271803881f467aabfc24593c7dcaeb710fe861
test "$(git -c safe.directory="$deps/nlohmann_json-src" -C "$deps/nlohmann_json-src" rev-parse HEAD)" = 9cca280a4d0ccf0c08f47a99aa71d1b0e52f8d03
grep -q "Pkg.Revision = 23.1.7779620" "$ndk/source.properties"
cmake -S /work -B /work/build/android -G Ninja \
 -DCMAKE_TOOLCHAIN_FILE="$ndk/build/cmake/android.toolchain.cmake" \
 -DANDROID_ABI=x86_64 -DANDROID_PLATFORM=android-21 -DANDROID_STL=c++_shared \
 -DCMAKE_BUILD_TYPE=Release \
 -DAPPLE_LIBCXX=/apple/libc++_shared.so \
 -DFETCHCONTENT_SOURCE_DIR_JSON="$deps/nlohmann_json-src" \
 -DFETCHCONTENT_SOURCE_DIR_DOBBY="$deps/dobby-src"
cmake --build /work/build/android --target apple-music-worker common_tests -j2
cmake -S /work -B /work/build/host -G Ninja \
 -DFETCHCONTENT_SOURCE_DIR_JSON="$deps/nlohmann_json-src"
cmake --build /work/build/host -j2
ctest --test-dir /work/build/host --output-on-failure
'
