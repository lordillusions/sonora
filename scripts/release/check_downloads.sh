#!/usr/bin/env bash
# COM-4n: checks a published release the way a visitor meets it (docs/INSTALLERS.md "Release checks").
#
#   scripts/release/check_downloads.sh [--site https://sonoradaw.com] [--repo lordillusions/sonora] [--version 0.1.4]
#
# 1. The latest release on GitHub is --version (default: project() in CMakeLists.txt) and is not a draft.
# 2. Every page of the live site and of site/ links only to releases/latest/download/<name> files that exist.
# 3. Each of those links answers HTTP 200 after redirects with the byte size of the release asset.
# 4. The live pages carry the same download links as site/ (the site repository was synced).
# Exit code 0 only when everything passes. Needs curl and gh (signed in).
set -euo pipefail

SITE="https://sonoradaw.com" REPO="lordillusions/sonora" VERSION=""
while [ $# -gt 0 ]; do
    case "$1" in
        --site) SITE="${2%/}"; shift 2 ;;
        --repo) REPO="$2"; shift 2 ;;
        --version) VERSION="$2"; shift 2 ;;
        *) sed -n '2,11p' "$0"; exit 2 ;;
    esac
done
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
[ -n "$VERSION" ] || VERSION="$(sed -nE 's/^project\(Sonora VERSION ([0-9.]+).*/\1/p' "$ROOT/CMakeLists.txt")"
TMP="$(mktemp -d "${TMPDIR:-/tmp}/sonora-check.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT
fails=0
bad() { echo "FAIL $*"; fails=$((fails + 1)); }

latest="$(gh release view --repo "$REPO" --json tagName,isDraft -q '.tagName + " " + (.isDraft|tostring)')"
[ "$latest" = "v$VERSION false" ] && echo "ok   latest release is v$VERSION" || bad "latest release is '$latest', expected v$VERSION"
gh release view "v$VERSION" --repo "$REPO" --json assets -q '.assets[] | .name + " " + (.size|tostring)' > "$TMP/assets"

links() { grep -oh 'releases/latest/download/[^"]*' "$@" | sed 's|.*/||' | sort -u; }
links "$ROOT"/site/*.html > "$TMP/local"
for page in $(cd "$ROOT/site" && ls *.html); do
    code="$(curl -s -o "$TMP/live-$page" -w '%{http_code}' "$SITE/$page")"
    [ "$code" = 200 ] || bad "$SITE/$page answered $code"
done
links "$TMP"/live-*.html > "$TMP/live" || true
if diff -q "$TMP/local" "$TMP/live" >/dev/null; then echo "ok   live site links the same $(wc -l < "$TMP/local" | xargs) files as site/"
else bad "live site and site/ link different files (sync the site repository):"; diff "$TMP/local" "$TMP/live" || true; fi

for name in $(sort -u "$TMP/local" "$TMP/live"); do
    want="$(awk -v n="$name" '$1 == n { print $2 }' "$TMP/assets")"
    [ -n "$want" ] || { bad "$name is linked but not attached to v$VERSION"; continue; }
    read -r code size < <(curl -sL -o /dev/null -r 0-0 -w '%{http_code} %{size_download}\n' \
        "https://github.com/$REPO/releases/latest/download/$name")
    total="$(curl -sIL "https://github.com/$REPO/releases/latest/download/$name" | tr -d '\r' |
        awk 'tolower($1) == "content-length:" { n = $2 } END { print n }')"
    if [ "$code" = 206 ] || [ "$code" = 200 ]; then
        [ "$total" = "$want" ] && echo "ok   $name ($total bytes)" || bad "$name is $total bytes, the asset is $want"
    else bad "$name answered HTTP $code"; fi
done

[ "$fails" = 0 ] && echo "all downloads check out" || { echo "$fails problem(s)"; exit 1; }
