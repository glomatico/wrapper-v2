#!/usr/bin/env bash
# extract-libs.sh - Extract Apple native libraries from a pinned APKMirror .apkm
# bundle and verify against LIBS_VERSION.json.
#
# The .apkm file must match the SHA-256 in .apkm. Individual inner APK splits are
# not pinned; extracted .so files are still verified against .libs.<arch>.
#
# Usage:
#   extract-libs.sh --bundle <path-to-.apkm> --arch <x86_64|arm64-v8a> --out <dir>
#
# Options:
#   --arch <x86_64|arm64-v8a>    Which arch's libs to extract (default x86_64)
#   --out  <directory>           Where to drop the .so files
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBS_VERSION="$REPO_ROOT/LIBS_VERSION.json"

BUNDLE=""
ARCH="x86_64"
OUT=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --bundle) BUNDLE="$2"; shift 2 ;;
        --arch)   ARCH="$2";   shift 2 ;;
        --out)    OUT="$2";    shift 2 ;;
        -h|--help)
            sed -n '2,15p' "$0"
            exit 0
            ;;
        *) echo "unknown arg: $1" >&2; exit 2 ;;
    esac
done

if [[ -z "$BUNDLE" ]]; then
    echo "extract-libs: missing --bundle <path-to-.apkm>" >&2
    exit 2
fi
if [[ -z "$OUT" ]]; then
    echo "extract-libs: missing --out" >&2
    exit 2
fi

case "$ARCH" in
    x86_64)    SPLIT_NAME="split_config.x86_64.apk"    ;;
    arm64-v8a) SPLIT_NAME="split_config.arm64_v8a.apk" ;;
    *) echo "extract-libs: unsupported arch '$ARCH'" >&2; exit 2 ;;
esac
APK_LIB_DIR="lib/$ARCH"

for c in jq sha256sum unzip; do
    command -v "$c" >/dev/null || { echo "extract-libs: $c is required" >&2; exit 3; }
done

verify_sha() {
    local file="$1"
    local expected="$2"
    local label="$3"
    local actual
    actual="$(sha256sum "$file" | awk '{print $1}')"
    if [[ "$actual" != "$expected" ]]; then
        echo "extract-libs: SHA-256 mismatch on $label" >&2
        echo "  expected: $expected" >&2
        echo "  actual:   $actual" >&2
        return 1
    fi
}

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

EXPECT_BUNDLE="$(jq -r '.apkm // empty' "$LIBS_VERSION" | tr -d '\r')"
[[ -n "$EXPECT_BUNDLE" ]] || { echo "extract-libs: no .apkm pin in LIBS_VERSION.json" >&2; exit 4; }
verify_sha "$BUNDLE" "$EXPECT_BUNDLE" "$(basename "$BUNDLE") (apkm bundle)" || exit 5

unzip -qq "$BUNDLE" "$SPLIT_NAME" -d "$TMP"
APK="$TMP/$SPLIT_NAME"
[[ -f "$APK" ]] || {
    echo "extract-libs: bundle missing $SPLIT_NAME (wrong .apkm or arch?)" >&2
    exit 6
}

mkdir -p "$OUT"
LIB_TMP="$TMP/libs"
mkdir -p "$LIB_TMP"
unzip -qq "$APK" "$APK_LIB_DIR/*" -d "$LIB_TMP"

# `jq.exe` on msys2/MSVC emits CRLF on Windows; strip CR defensively before
# iterating, otherwise lib names get a stray \r appended and every lookup fails.
mapfile -t EXPECTED_LIBS < <(
    jq -r --arg arch "$ARCH" '.libs[$arch] | keys[]' "$LIBS_VERSION" | tr -d '\r'
)

ok=0
fail=0
for so in "${EXPECTED_LIBS[@]}"; do
    src="$LIB_TMP/$APK_LIB_DIR/$so"
    if [[ ! -f "$src" ]]; then
        echo "extract-libs: missing in APK: $so" >&2
        fail=$((fail+1))
        continue
    fi
    expect="$(jq -r --arg arch "$ARCH" --arg so "$so" '.libs[$arch][$so]' "$LIBS_VERSION" | tr -d '\r')"
    actual="$(sha256sum "$src" | awk '{print $1}')"
    if [[ "$expect" != "$actual" ]]; then
        echo "extract-libs: SHA-256 mismatch on $so" >&2
        echo "  expected: $expect" >&2
        echo "  actual:   $actual" >&2
        fail=$((fail+1))
        continue
    fi
    install -m 0644 "$src" "$OUT/$so"
    ok=$((ok+1))
done

echo "extract-libs: $ok ok, $fail failed (arch=$ARCH out=$OUT)"
[[ $fail -eq 0 ]]
