#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
# Existing local build image supplies the pinned NDK and pinned dependency trees.
# No old Wrapper sources are compiled or linked.
image=${WORKER_BUILD_IMAGE:-wrapper-v2:temari-build}
docker run --rm --network none --user "$(id -u):$(id -g)" \
  -v "$PWD:/work" --entrypoint /bin/bash "$image" -c '
set -euo pipefail
ndk=/opt/android-ndk-r23b
deps=/app/build/daemon-x86_64/_deps
cmake -S /work -B /work/build/android -G Ninja \
 -DCMAKE_TOOLCHAIN_FILE="$ndk/build/cmake/android.toolchain.cmake" \
 -DANDROID_ABI=x86_64 -DANDROID_PLATFORM=android-21 -DANDROID_STL=c++_shared \
 -DCMAKE_BUILD_TYPE=Release \
 -DFETCHCONTENT_SOURCE_DIR_JSON="$deps/nlohmann_json-src" \
 -DFETCHCONTENT_SOURCE_DIR_DOBBY="$deps/dobby-src"
cmake --build /work/build/android -j2
cmake -S /work -B /work/build/host -G Ninja \
 -DFETCHCONTENT_SOURCE_DIR_JSON="$deps/nlohmann_json-src"
cmake --build /work/build/host -j2
ctest --test-dir /work/build/host --output-on-failure
'
