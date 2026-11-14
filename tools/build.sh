#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
: "${ANDROID_NDK_HOME:?Set ANDROID_NDK_HOME to Android NDK r23b}"
: "${APPLE_LIBCXX:?Set APPLE_LIBCXX to the extracted, pinned libc++_shared.so}"
if ! grep -q 'Pkg.Revision = 23.1.7779620' "$ANDROID_NDK_HOME/source.properties"; then
  echo "Only NDK r23b (23.1.7779620) is validated" >&2; exit 1
fi
export SOURCE_DATE_EPOCH=0
cmake -S . -B build/android -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=x86_64 -DANDROID_PLATFORM=android-21 -DANDROID_STL=c++_shared \
  -DAPPLE_LIBCXX="$APPLE_LIBCXX" -DCMAKE_BUILD_TYPE=Release
cmake --build build/android --target apple-music-worker common_tests -j2
cmake -S . -B build/host -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/host -j2
ctest --test-dir build/host --output-on-failure
python3 -m unittest discover -s tests -p 'test_*.py'
