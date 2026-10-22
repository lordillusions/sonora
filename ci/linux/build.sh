#!/usr/bin/env bash
# Builds Sonora on Linux and runs the unit tests. Runs inside ci/linux/Dockerfile (via run.sh) or on
# any Ubuntu 24.04 machine with the same packages (the GitHub Actions ubuntu job).
#
#   ci/linux/build.sh headless   # -DSONORA_WITH_UI=OFF: core/io/engine/devices/plugins/midi + sonora_tests
#   ci/linux/build.sh ui         # full build incl. Skia UI, app, ui_snapshot
#   ci/linux/build.sh all        # both
#   ci/linux/build.sh package    # Release app + AppImage and .deb in dist/linux (SONORA_APP_VERSION optional)
#
# Env: SONORA_THIRD_PARTY_DIR (default <repo>/third_party), SONORA_BUILD_ROOT (default <repo>),
#      SONORA_JOBS (default: nproc), SONORA_BUILD_TYPE (default Debug).
set -euo pipefail
cd "$(dirname "$0")/../.."
mode="${1:-all}"
third_party="${SONORA_THIRD_PARTY_DIR:-$PWD/third_party}"
build_root="${SONORA_BUILD_ROOT:-$PWD}"
jobs="${SONORA_JOBS:-$(nproc)}"
build_type="${SONORA_BUILD_TYPE:-Debug}"

fetch_skia=1
[ "$mode" = headless ] && fetch_skia=0
SONORA_THIRD_PARTY_DIR="$third_party" SONORA_FETCH_SKIA="$fetch_skia" scripts/bootstrap.sh

run_tests() {
    local dir="$1"
    # UI tests create JUCE components; give them an X server when none is set.
    if [ -z "${DISPLAY:-}" ] && command -v xvfb-run >/dev/null; then
        xvfb-run -a "$dir/tests/sonora_tests" | tee "$dir/tests.log"
    else
        "$dir/tests/sonora_tests" | tee "$dir/tests.log"
    fi
    grep -q " 0 failed checks" "$dir/tests.log"
}

build() {
    local name="$1" ui="$2"
    local dir="$build_root/build-linux-$name"
    cmake -S . -B "$dir" -G Ninja -DCMAKE_BUILD_TYPE="$build_type" -DSONORA_WITH_UI="$ui" \
        -DSONORA_THIRD_PARTY_DIR="$third_party"
    cmake --build "$dir" -j "$jobs" 2>&1 | tee "$dir/build.log"
    echo "warnings: $(grep -c 'warning:' "$dir/build.log" || true)"
    run_tests "$dir"
}

# Installers (docs/INSTALLERS.md): a separate Release tree, so it never rebuilds the Debug trees above.
package() {
    local dir="$build_root/build-linux-release"
    cmake -S . -B "$dir" -G Ninja -DCMAKE_BUILD_TYPE=Release -DSONORA_WITH_UI=ON -DSONORA_THIRD_PARTY_DIR="$third_party" \
        ${SONORA_APP_VERSION:+-DSONORA_APP_VERSION="$SONORA_APP_VERSION"}
    cmake --build "$dir" -j "$jobs" --target Sonora 2>&1 | tee "$dir/build.log"
    packaging/linux/make_packages.sh "$dir" "${SONORA_DIST_DIR:-dist/linux}" ${SONORA_APP_VERSION:+"$SONORA_APP_VERSION"}
}

case "$mode" in
    headless) build headless OFF ;;
    ui) build ui ON ;;
    all) build headless OFF; build ui ON ;;
    package) package ;;
    *) echo "usage: $0 [headless|ui|all|package]" >&2; exit 2 ;;
esac
