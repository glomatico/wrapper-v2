#!/usr/bin/env bash
# Build both the unmodified release control and Wrapper's ARM64 linker.
set -euo pipefail
if (( $# != 1 )); then
    echo "usage: $0 NEW_BUILD_DIRECTORY" >&2
    exit 2
fi
here=$(cd "$(dirname "$0")" && pwd)
mkdir -- "$1"
src=$(cd "$1" && pwd)
while read -r commit project; do
    git -c advice.detachedHead=false clone --quiet --depth=1 --branch android-5.1.1_r25 \
        "https://android.googlesource.com/platform/$project" "$src/$project"
    test "$(git -C "$src/$project" rev-parse HEAD)" = "$commit"
done < "$here/sources.lock"

image=$(docker build --quiet --platform linux/amd64 "$here")
run=(docker run --rm --network none --platform linux/amd64 --user "$(id -u):$(id -g)"
     -v "$src:/src" -v "$here:/recipe:ro" "$image")
target=out/target/product/generic_arm64/system/bin/linker64
build=(make -f build/core/main.mk "-j${JOBS:-8}" ONE_SHOT_MAKEFILE=/recipe/modules.mk "$target")

"${run[@]}" "${build[@]}"
# The SDK's final file-backed PT_LOAD ends at 0x121f8. This checks the
# ELF headers, instructions, unwind tables, relocations and initialized data.
control=$(head -c 74232 "$src/$target" | sha256sum)
if [[ ${control%% *} != fc0f6b4c5aa1caa8def0e3f2efd2e74832400383fce77dfbb211356c169b6a50 ]]; then
    echo "unmodified build differs from the SDK load image" >&2
    exit 1
fi
cp "$src/$target" "$src/linker64.control"
git -C "$src/bionic" apply "$here/vdso.patch"
"${run[@]}" "${build[@]}"
# Retain local symbols. The companion DWARF file isn't distributed and its
# debuglink CRC embeds details of the build environment.
objcopy=prebuilts/gcc/linux-x86/aarch64/aarch64-linux-android-4.9/bin/aarch64-linux-android-objcopy
"${run[@]}" "$objcopy" --remove-section=.gnu_debuglink "$target" linker64
sha256sum "$src/linker64"
