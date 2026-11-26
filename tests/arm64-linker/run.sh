#!/usr/bin/env bash
# Usage: ANDROID_NDK_HOME=/path/to/android-ndk-r23b bash run.sh SSH_HOST [LINKER64]
set -euo pipefail
if (( $# < 1 || $# > 2 )); then
    echo "usage: $0 SSH_HOST [LINKER64]" >&2
    exit 2
fi
here=$(cd "$(dirname "$0")" && pwd)
system=$here/../../vendor/android-system/arm64-v8a
cc=${ANDROID_NDK_HOME:?set ANDROID_NDK_HOME to the Linux x86-64 NDK}/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android22-clang
root=$(mktemp -d)
trap 'rm -rf -- "$root"' EXIT
mkdir -p "$root/system"/{bin,lib64}
install -m755 "${2:-$system/bin/linker64}" "$root/system/bin/linker64"
cp "$system/lib64"/{libc,libm}.so "$root/system/lib64/"
"$cc" -Wall -Wextra -Werror -Wl,--hash-style=sysv -fPIC -shared "$here/fixture.c" \
    -Wl,-soname,libwrapper_fixture.so -o "$root/system/lib64/libwrapper_fixture.so"
"$cc" -Wall -Wextra -Werror -Wl,--hash-style=sysv "$here/probe.c" -ldl -o "$root/probe"
# Only these public runtime files and test fixtures enter the temporary chroot.
tar -C "$root" -cf - system probe | ssh -o BatchMode=yes -- "$1" '
    set -eu
    test "$(uname -m)" = aarch64
    scratch=$(mktemp -d)
    trap '\''rm -rf -- "$scratch"'\'' EXIT
    mkdir -m755 "$scratch/root"
    tar -C "$scratch/root" -xf -
    ulimit -c 0
    timeout 20s env -i PATH=/usr/sbin:/usr/bin unshare --pid --fork --kill-child \
        chroot --userspec=65534:65534 --groups=65534 "$scratch/root" /probe
'
