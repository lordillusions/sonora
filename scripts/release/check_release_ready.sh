#!/usr/bin/env bash
# COM-4n: refuses a release that would ship something the website doesn't offer, or the wrong version.
#
#   scripts/release/check_release_ready.sh [<version>]
#
# - <version> (a tag without the "v", optional) equals project() in CMakeLists.txt: the version About shows.
# - Every native VST3 (sonora_add_native_vst3 ... PRODUCT_NAME "Sonora X" in src/CMakeLists.txt) is built and packaged by
#   .github/workflows/installers.yml as Sonora-X-VST3-<v>.pkg with the stable copy Sonora-X-VST3-macOS.pkg, and
#   site/download.html offers it: a download link, a card with id "x" and its screenshot site/assets/img/plugin-x.webp.
# The installers workflow runs this before building a tag. Exit code 0 when everything is in place.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"
fails=0
bad() { echo "FAIL $*"; fails=$((fails + 1)); }
project="$(sed -nE 's/^project\(Sonora VERSION ([0-9.]+).*/\1/p' CMakeLists.txt)"
if [ $# -ge 1 ] && [ "$1" != "$project" ]; then
    bad "version $1 but CMakeLists.txt has project(Sonora VERSION $project): bump project() first"
else
    echo "ok   version $project"
fi

workflow=.github/workflows/installers.yml page=site/download.html
# The PRODUCT_NAME lines inside sonora_add_native_vst3(...) calls.
products="$(awk '/sonora_add_native_vst3\(Sonora/ { inside = 1 } inside && /PRODUCT_NAME/ { sub(/.*PRODUCT_NAME "/, ""); sub(/".*/, ""); print; inside = 0 }' src/CMakeLists.txt)"
[ -n "$products" ] || bad "found no sonora_add_native_vst3 plug-ins in src/CMakeLists.txt"
while IFS= read -r product; do
    [ -n "$product" ] || continue
    short="${product#Sonora }"
    slug="Sonora-${short// /-}-VST3"
    id="$(echo "$short" | tr '[:upper:] ' '[:lower:]-')"
    target="$(awk -v p="$product" '/sonora_add_native_vst3\(/ { t = $0; sub(/.*sonora_add_native_vst3\(/, "", t); sub(/[ )].*/, "", t) }
        index($0, "PRODUCT_NAME \"" p "\"") { print t; exit }' src/CMakeLists.txt)"
    missing=()
    grep -q "${target}_VST3" "$workflow" || missing+=("build target ${target}_VST3 in $workflow")
    grep -qF "$slug-\$VERSION.pkg" "$workflow" || missing+=("make_vst3_pkg.sh ... $slug-\$VERSION.pkg in $workflow")
    grep -qF "$slug-macOS.pkg" "$workflow" || missing+=("stable copy $slug-macOS.pkg in $workflow")
    grep -qF "releases/latest/download/$slug-macOS.pkg" "$page" || missing+=("download link to $slug-macOS.pkg in $page")
    grep -qF "id=\"$id\"" "$page" || missing+=("a card with id=\"$id\" in $page")
    [ -f "site/assets/img/plugin-$id.webp" ] || missing+=("screenshot site/assets/img/plugin-$id.webp")
    grep -qF "assets/img/plugin-$id.webp" "$page" || missing+=("the screenshot on $page")
    if [ ${#missing[@]} -eq 0 ]; then echo "ok   $product ($target): installer, stable name, site card, screenshot"
    else for m in "${missing[@]}"; do bad "$product: missing $m"; done; fi
done <<< "$products"

[ "$fails" = 0 ] && echo "ready to release $project" || { echo "$fails problem(s): see docs/INSTALLERS.md, Release checks"; exit 1; }
