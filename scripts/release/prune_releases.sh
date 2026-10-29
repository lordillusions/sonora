#!/usr/bin/env bash
# COM-4o: keeps only the newest published releases on the public repository (five by default).
#
#   scripts/release/prune_releases.sh [--keep 5] [--repo lordillusions/sonora] [--yes]
#
# Lists the published, non-draft releases newest first and deletes every one after the first --keep. The git tags stay,
# so the source of every version is still on GitHub. Without --yes it only prints what it would delete.
set -euo pipefail

KEEP=5 REPO="lordillusions/sonora" YES=0
while [ $# -gt 0 ]; do
    case "$1" in
        --keep) KEEP="$2"; shift 2 ;;
        --repo) REPO="$2"; shift 2 ;;
        --yes) YES=1; shift ;;
        *) sed -n '2,8p' "$0"; exit 2 ;;
    esac
done
old="$(gh release list --repo "$REPO" --exclude-drafts --limit 200 --json tagName,publishedAt \
    -q 'sort_by(.publishedAt) | reverse | .[].tagName' | tail -n +$((KEEP + 1)))"
[ -n "$old" ] || { echo "nothing to prune: $REPO has $KEEP or fewer releases"; exit 0; }
for tag in $old; do
    if [ "$YES" = 1 ]; then gh release delete "$tag" --repo "$REPO" --yes && echo "deleted release $tag (tag kept)"
    else echo "would delete release $tag"; fi
done
