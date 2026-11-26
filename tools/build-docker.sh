#!/usr/bin/env bash
# Build the entire Wrapper privately in Docker, including the native worker and
# Apple libraries extracted from a pinned APK provided in an isolated, read-only
# BuildKit named context (not included in the source context or raw image layers).
set -euo pipefail
caller_pwd=$PWD
cd "$(dirname "$0")/.."

usage() {
  echo "usage: tools/build-docker.sh <AppleMusic.apk|AppleMusic.apkm> [--arch x86_64|arm64-v8a]"
  echo "       (APPLE_MUSIC_APK is supported as a fallback for existing scripts)"
}

arch=${WRAPPER_ARCH:-$(uname -m)}
apk_input=
while [[ $# -gt 0 ]]; do
  case "$1" in
    -h|--help) usage; exit 0 ;;
    --arch)
      [[ $# -ge 2 && -n "$2" ]] || { usage >&2; exit 2; }
      arch=$2; shift 2 ;;
    --arch=*) arch=${1#--arch=}; shift ;;
    -*)
      echo "Unknown option: $1" >&2; usage >&2; exit 2 ;;
    *)
      [[ -z "$apk_input" ]] || { echo "Only one APK/APKM file can be supplied" >&2; usage >&2; exit 2; }
      apk_input=$1; shift ;;
  esac
done

case "$arch" in
  x86_64|amd64) platform=linux/amd64 ;;
  arm64-v8a|arm64|aarch64) platform=linux/arm64 ;;
  *) echo "Unsupported architecture: $arch" >&2; exit 2 ;;
esac
apk_input=${apk_input:-${APPLE_MUSIC_APK:-}}
[[ -n "$apk_input" ]] || { usage >&2; echo "Provide your pinned Apple Music .apk or .apkm path" >&2; exit 2; }
if [[ "$apk_input" != /* ]]; then apk_input="$caller_pwd/$apk_input"; fi
[[ -f "$apk_input" ]] || { echo "APK/APKM not found: $apk_input" >&2; exit 1; }
apk=$(realpath -- "$apk_input")
apk_sha256=$(sha256sum "$apk" | cut -d ' ' -f 1)
image=${WRAPPER_IMAGE:-wrapper-v2:1.0.1-local-${platform#linux/}}

echo "Building $image ($platform) with the local Apple Music package supplied privately to BuildKit."
echo "The builder checks the selected profile manifests and native-library hashes."
echo "NOTE: The resulting image contains Apple's proprietary libraries; do not publish it."

# BuildKit limits the size of --secret inputs on some builders. Use an isolated
# named context containing ONLY the APK and bind mount it in the builder RUNs;
# this avoids COPY-ing the raw APK to a committed intermediate image layer.
private_context=$(mktemp -d)
trap 'rm -rf -- "$private_context"' EXIT
ln -- "$apk" "$private_context/AppleMusic.package" 2>/dev/null \
  || cp -- "$apk" "$private_context/AppleMusic.package"

docker buildx build --load --platform "$platform" \
  --build-context "apple_apk=$private_context" \
  --build-arg "APPLE_APK_SHA256=$apk_sha256" \
  --tag "$image" --tag wrapper-v2:local --file Dockerfile .

echo "Built $image (also tagged wrapper-v2:local). Run docker compose up -d."
