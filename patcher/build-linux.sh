#!/bin/sh
# The Linux build, in Docker (Ubuntu 22.04, x86_64): runs on that and newer
# distributions. From the patcher/ folder:  ./build-linux.sh   ->  build-linux/ftl-ios-patcher
set -e
cd "$(dirname "$0")/.."
docker run --rm --platform linux/amd64 -v "$PWD:/src" -w /src/patcher ubuntu:22.04 sh -c '
  export DEBIAN_FRONTEND=noninteractive
  apt-get update -qq >/dev/null
  apt-get install -y -qq --no-install-recommends g++ cmake ninja-build git ca-certificates pkg-config \
    libgtk-3-dev \
    >/dev/null
  cmake -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
  cmake --build build-linux
  strip build-linux/ftl-ios-patcher
'
