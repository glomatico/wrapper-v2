#!/usr/bin/env bash
# stage-system.sh - Verify and copy committed Android system binaries into
# rootfs/system/. These binaries (linker64 + bionic + a few AOSP libs) are
# committed under vendor/android-system/<arch>/ with SHA-256 pins in
# LIBS_VERSION.json.
#
# Usage:
#   tools/stage-system.sh [--arch <x86_64|arm64-v8a>] [--rootfs <path>]
#
# Defaults: --arch x86_64, --rootfs ./rootfs
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBS_VERSION="$REPO_ROOT/LIBS_VERSION.json"

ARCH="x86_64"
ROOTFS="$REPO_ROOT/rootfs"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --arch)   ARCH="$2";   shift 2 ;;
        --rootfs) ROOTFS="$2"; shift 2 ;;
        -h|--help)
            sed -n '2,11p' "$0"
            exit 0
            ;;
        *) echo "unknown arg: $1" >&2; exit 2 ;;
    esac
done

VENDOR="$REPO_ROOT/vendor/android-system/$ARCH"
if [[ ! -d "$VENDOR" ]]; then
    echo "stage-system: vendor dir does not exist: $VENDOR" >&2
    echo "  (current arch '$ARCH' may not have committed binaries yet)" >&2
    exit 4
fi

for c in jq sha256sum install; do
    command -v "$c" >/dev/null || { echo "stage-system: $c is required" >&2; exit 3; }
done

# `jq.exe` on msys2/MSVC emits CRLF on Windows; strip CR defensively before
# iterating, otherwise paths get a stray \r appended and every lookup fails.
mapfile -t EXPECTED < <(
    jq -r --arg arch "$ARCH" '.android_system[$arch] | keys[]' "$LIBS_VERSION" | tr -d '\r'
)
[[ ${#EXPECTED[@]} -gt 0 ]] || { echo "stage-system: no .android_system.$ARCH section in LIBS_VERSION.json" >&2; exit 4; }

ok=0
fail=0
for rel in "${EXPECTED[@]}"; do
    src="$VENDOR/$rel"
    if [[ ! -f "$src" ]]; then
        echo "stage-system: missing committed file: vendor/android-system/$ARCH/$rel" >&2
        fail=$((fail+1))
        continue
    fi
    expect="$(jq -r --arg arch "$ARCH" --arg p "$rel" '.android_system[$arch][$p]' "$LIBS_VERSION" | tr -d '\r')"
    actual="$(sha256sum "$src" | awk '{print $1}')"
    if [[ "$expect" != "$actual" ]]; then
        echo "stage-system: SHA-256 mismatch on $rel" >&2
        echo "  expected: $expect" >&2
        echo "  actual:   $actual" >&2
        fail=$((fail+1))
        continue
    fi
    case "$rel" in
        bin/*)   mode=0755 ;;
        lib64/*) mode=0644 ;;
        *)       mode=0644 ;;
    esac
    dest="$ROOTFS/system/$rel"
    mkdir -p "$(dirname "$dest")"
    install -m "$mode" "$src" "$dest"
    ok=$((ok+1))
done

echo "stage-system: $ok ok, $fail failed (arch=$ARCH rootfs=$ROOTFS)"
[[ $fail -eq 0 ]]
