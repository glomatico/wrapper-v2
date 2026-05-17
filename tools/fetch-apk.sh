#!/usr/bin/env bash
# fetch-apk.sh - Download a pinned artifact and verify its SHA-256 against
# LIBS_VERSION.json.
#
# Usage:
#   APK_URL=https://example.com/bundle.apkm \
#     tools/fetch-apk.sh --expect apkm --out .tmp/bundle.apkm
#
# The --expect value must be a top-level string key in LIBS_VERSION.json (e.g. apkm).
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBS_VERSION="$REPO_ROOT/LIBS_VERSION.json"

URL="${APK_URL:-}"
EXPECT=""
OUT=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --url)    URL="$2";    shift 2 ;;
        --expect) EXPECT="$2"; shift 2 ;;
        --out)    OUT="$2";    shift 2 ;;
        -h|--help)
            sed -n '2,15p' "$0"
            exit 0
            ;;
        *) echo "unknown arg: $1" >&2; exit 2 ;;
    esac
done

if [[ -z "$URL" ]];    then echo "fetch-apk: missing --url or APK_URL"   >&2; exit 2; fi
if [[ -z "$EXPECT" ]]; then echo "fetch-apk: missing --expect <libs_version_key>" >&2; exit 2; fi
if [[ -z "$OUT" ]];    then echo "fetch-apk: missing --out <path>"      >&2; exit 2; fi

if ! command -v jq        >/dev/null; then echo "fetch-apk: jq is required"        >&2; exit 3; fi
if ! command -v sha256sum >/dev/null; then echo "fetch-apk: sha256sum is required" >&2; exit 3; fi

EXPECT_HASH="$(jq -r --arg k "$EXPECT" 'if (.[$k] | type) == "string" then .[$k] else empty end' "$LIBS_VERSION" | tr -d '\r')"
if [[ -z "$EXPECT_HASH" ]]; then
    echo "fetch-apk: no string SHA-256 pin for top-level '$EXPECT' in LIBS_VERSION.json" >&2
    exit 4
fi

mkdir -p "$(dirname "$OUT")"

echo "fetch-apk: GET $URL"
if   command -v curl  >/dev/null; then curl -fL --retry 3 -o "$OUT" "$URL"
elif command -v wget  >/dev/null; then wget -q -O "$OUT" "$URL"
elif command -v aria2c >/dev/null; then aria2c -q -o "$(basename "$OUT")" -d "$(dirname "$OUT")" "$URL"
else echo "fetch-apk: need curl, wget, or aria2c" >&2; exit 3
fi

ACTUAL_HASH="$(sha256sum "$OUT" | awk '{print $1}')"
if [[ "$ACTUAL_HASH" != "$EXPECT_HASH" ]]; then
    echo "fetch-apk: SHA-256 mismatch for $EXPECT" >&2
    echo "  expected: $EXPECT_HASH" >&2
    echo "  actual:   $ACTUAL_HASH" >&2
    rm -f "$OUT"
    exit 5
fi

echo "fetch-apk: OK ($EXPECT, $ACTUAL_HASH)"
