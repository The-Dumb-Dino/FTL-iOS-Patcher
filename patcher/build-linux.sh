#!/bin/sh
# The Linux build, in Docker (Ubuntu 22.04): runs on that and newer distributions.
# From the patcher/ folder:
#   ./build-linux.sh          ->  build-linux/ftl-ios-patcher        (x86_64)
#   ./build-linux.sh arm64    ->  build-linux-arm64/ftl-ios-patcher  (aarch64)
set -e
cd "$(dirname "$0")/.."
case "${1:-amd64}" in
    amd64 | x86_64) dir=build-linux; platform=linux/amd64 ;;
    arm64 | aarch64) dir=build-linux-arm64; platform=linux/arm64 ;;
    *)
        echo "usage: $0 [amd64|arm64]" >&2
        exit 2
        ;;
esac
docker run --rm --platform "$platform" -e BUILD_DIR="$dir" -v "$PWD:/src" -w /src/patcher ubuntu:22.04 sh -c '
  export DEBIAN_FRONTEND=noninteractive
  apt-get update -qq >/dev/null
  apt-get install -y -qq --no-install-recommends g++ cmake ninja-build git ca-certificates pkg-config \
    libgtk-3-dev \
    >/dev/null
  cmake -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
  cmake --build "$BUILD_DIR"
  strip "$BUILD_DIR/ftl-ios-patcher"
'
