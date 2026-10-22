#!/usr/bin/env bash
# Host-side wrapper (macOS/Linux with Docker): builds the image, then runs ci/linux/build.sh in it.
# The repository is mounted read-write at /src; JUCE/Skia for Linux and the build directories live in
# named volumes, so a macOS checkout's third_party/ and build*/ are never touched.
#
#   ci/linux/run.sh [headless|ui|all|package]
#   SONORA_JOBS=6 ci/linux/run.sh ui       # x64 under emulation on Apple silicon: keep jobs <= RAM/1.5 GB
set -euo pipefail
cd "$(dirname "$0")/../.."
image="${SONORA_LINUX_IMAGE:-sonora-linux}"
docker build --platform linux/amd64 -t "$image" ci/linux
docker run --rm --platform linux/amd64 \
    -v "$PWD":/src \
    -v sonora-linux-deps:/deps \
    -v sonora-linux-build:/build \
    -e SONORA_THIRD_PARTY_DIR=/deps \
    -e SONORA_BUILD_ROOT=/build \
    -e SONORA_JOBS="${SONORA_JOBS:-6}" \
    -e SONORA_BUILD_TYPE="${SONORA_BUILD_TYPE:-Debug}" \
    -e SONORA_APP_VERSION="${SONORA_APP_VERSION:-}" \
    "$image" ci/linux/build.sh "${1:-all}"
