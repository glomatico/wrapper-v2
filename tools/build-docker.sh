#!/usr/bin/env bash
# Build the entire Wrapper privately in Docker, including the native worker and
# Apple libraries extracted from a pinned APK provided in an isolated, read-only
# BuildKit named context (not included in the source context or raw image layers).
set -euo pipefail
cd "$(dirname "$0")/.."

arch=${WRAPPER_ARCH:-$(uname -m)}
if [[ ${1:-} == --arch ]]; then
  [[ $# -ge 2 ]] || { echo "usage: tools/build-docker.sh [--arch x86_64|arm64-v8a]" >&2; exit 2; }
  arch=$2
  shift 2
fi
[[ $# == 0 ]] || { echo "usage: tools/build-docker.sh [--arch x86_64|arm64-v8a]" >&2; exit 2; }

case "$arch" in
  x86_64|amd64) platform=linux/amd64 ;;
  arm64-v8a|arm64|aarch64) platform=linux/arm64 ;;
  *) echo "Unsupported architecture: $arch" >&2; exit 2 ;;
esac
: "${APPLE_MUSIC_APK:?Set APPLE_MUSIC_APK to your authorized pinned Apple Music APK/APKM file}"
[[ -f "$APPLE_MUSIC_APK" ]] || { echo "APK not found: $APPLE_MUSIC_APK" >&2; exit 1; }
apk=$(realpath -- "$APPLE_MUSIC_APK")
apk_sha256=$(sha256sum "$apk" | cut -d ' ' -f 1)
image=${WRAPPER_IMAGE:-wrapper-v2:1.0.0-local-${platform#linux/}}

echo "Building $image ($platform) with the verified APK supplied privately to BuildKit."
echo "NOTE: The resulting image contains Apple's proprietary libraries; do not publish it."

# BuildKit limits the size of --secret inputs on some builders. Use an isolated
# named context containing ONLY the APK and bind mount it in the builder RUNs;
# this avoids COPY-ing the raw APK to a committed intermediate image layer.
private_context=$(mktemp -d)
trap 'rm -rf -- "$private_context"' EXIT
ln -- "$apk" "$private_context/AppleMusic.apkm" 2>/dev/null \
  || cp -- "$apk" "$private_context/AppleMusic.apkm"

docker buildx build --load --platform "$platform" \
  --build-context "apple_apk=$private_context" \
  --build-arg "APPLE_APK_SHA256=$apk_sha256" \
  --tag "$image" --file Dockerfile .

echo "Built $image. Configure WRAPPER_IMAGE=$image in .env and run docker compose up -d."
