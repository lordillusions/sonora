#!/bin/sh
# Renames the product on every page (COM-1: "Sonora" is a working name).
# Usage: site/rename.sh NewName
# Replaces the word in the HTML text only. The logo files in assets/brand/ draw the letters, so regenerate them with
# tools/brand_render (src/gfx/Brand.cpp) and copy them in. Also check the GitHub URLs if the repository moves.
set -eu
new=${1:?usage: site/rename.sh NewName}
dir=$(cd "$(dirname "$0")" && pwd)
for f in "$dir"/*.html; do
    sed -i.bak "s/Sonora/$new/g" "$f" && rm "$f.bak"
done
grep -l "$new" "$dir"/*.html
echo "Renamed to $new. Remaining 'sonora' (URLs, file names):"
grep -o -i "[a-z/.]*sonora[a-z/.]*" "$dir"/*.html | sort | uniq -c || true
