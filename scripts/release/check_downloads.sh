#!/usr/bin/env bash
# COM-4n: checks a published release the way a visitor meets it (docs/INSTALLERS.md "Release checks").
#
#   scripts/release/check_downloads.sh [--site https://sonoradaw.com] [--repo lordillusions/sonora] [--version 0.1.4]
#
# 1. The latest release on GitHub is --version (default: project() in CMakeLists.txt) and is not a draft.
# 2. Every page of the live site and of site/ links only to files of v<version> (releases/download/v<version>/<name>,
#    or releases/latest/download/<name>) that exist.
# 3. Each of those links answers HTTP 200 after redirects with the byte size of the release asset.
# 4. The live pages carry the same download links as site/ (the site repository was synced).
# 5. The update feeds (COM-4b, COM-4l): appcast.xml (macOS), appcast-windows.xml and appcast-linux.xml live equal site/,
#    their newest item is <version>, and each of its files downloads with the feed's size and an EdDSA signature valid
#    for packaging/macos/updates.env's key.
# Exit code 0 only when everything passes. Needs curl, gh (signed in), python3 and swift.
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

links() { grep -ohE 'releases/(latest/download|download/v[0-9.]+)/[^"]*' "$@" | sed 's|^releases/||' | sort -u; }
links "$ROOT"/site/*.html > "$TMP/local"
for page in $(cd "$ROOT/site" && ls *.html); do
    code="$(curl -s -o "$TMP/live-$page" -w '%{http_code}' "$SITE/$page")"
    [ "$code" = 200 ] || bad "$SITE/$page answered $code"
done
links "$TMP"/live-*.html > "$TMP/live" || true
if diff -q "$TMP/local" "$TMP/live" >/dev/null; then echo "ok   live site links the same $(wc -l < "$TMP/local" | xargs) files as site/"
else bad "live site and site/ link different files (sync the site repository):"; diff "$TMP/local" "$TMP/live" || true; fi

for link in $(sort -u "$TMP/local" "$TMP/live"); do
    name="${link##*/}"
    case "$link" in latest/download/*|download/v$VERSION/*) ;; *) bad "$link names another version than v$VERSION"; continue ;; esac
    want="$(awk -v n="$name" '$1 == n { print $2 }' "$TMP/assets")"
    [ -n "$want" ] || { bad "$name is linked but not attached to v$VERSION"; continue; }
    read -r code size < <(curl -sL -o /dev/null -r 0-0 -w '%{http_code} %{size_download}\n' \
        "https://github.com/$REPO/releases/$link")
    total="$(curl -sIL "https://github.com/$REPO/releases/$link" | tr -d '\r' |
        awk 'tolower($1) == "content-length:" { n = $2 } END { print n }')"
    if [ "$code" = 206 ] || [ "$code" = 200 ]; then
        [ "$total" = "$want" ] && echo "ok   $name ($total bytes)" || bad "$name is $total bytes, the asset is $want"
    else bad "$name answered HTTP $code"; fi
done

set +u; . "$ROOT/packaging/macos/updates.env"; set -u
check_feed() { # <feed url>: live = site/<name>, newest stable version = $VERSION, every enclosure of it downloads and verifies
    local url="$1" name="${1##*/}" code
    code="$(curl -s -o "$TMP/$name" -w '%{http_code}' "$url")"
    [ "$code" = 200 ] || { bad "$url answered $code"; return; }
    cmp -s "$TMP/$name" "$ROOT/site/$name" && echo "ok   live $name equals site/$name" ||
        bad "live $name differs from site/$name (copy the release's $name there and sync the site)"
    local found=0 newest file length signature
    while read -r newest file length signature; do
        found=1
        [ "$newest" = "$VERSION" ] || { bad "$name: newest stable item is $newest, expected $VERSION"; continue; }
        curl -sL -o "$TMP/update.bin" "$file"
        local got; got="$(wc -c < "$TMP/update.bin" | xargs)"
        [ "$got" = "$length" ] && echo "ok   $name: ${file##*/} ($got bytes)" || bad "$name: ${file##*/} is $got bytes, the feed says $length"
        if [ -z "$SONORA_UPDATE_PUBLIC_KEY" ]; then bad "packaging/macos/updates.env has no public key"
        elif [ "$(swift "$ROOT/scripts/release/verify_ed_signature.swift" "$SONORA_UPDATE_PUBLIC_KEY" "$TMP/update.bin" "$signature" 2>&1)" = valid ]; then
            echo "ok   $name: ${file##*/} signature is valid for the apps' public key"
        else bad "$name: ${file##*/} signature does not verify with packaging/macos/updates.env's key"; fi
    done < <(python3 -I - "$TMP/$name" <<'PY'
import sys, xml.etree.ElementTree as ET
ns = "{http://www.andymatuschak.org/xml-namespaces/sparkle}"
items = []
for item in ET.parse(sys.argv[1]).iter("item"):
    v = item.findtext(ns + "version")
    if v and not item.findtext(ns + "channel"):
        for e in item.findall("enclosure"):
            items.append(([int(p) for p in v.split(".")], v, e.get("url"), e.get("length"), e.get(ns + "edSignature")))
if items:
    top = max(i[0] for i in items)
    for i in items:
        if i[0] == top:
            print(*i[1:])
PY
)
    [ "$found" = 1 ] || bad "$name has no stable item"
}
check_feed "$SONORA_UPDATE_FEED_URL"
check_feed "$SONORA_UPDATE_FEED_URL_WINDOWS"
check_feed "$SONORA_UPDATE_FEED_URL_LINUX"

[ "$fails" = 0 ] && echo "all downloads check out" || { echo "$fails problem(s)"; exit 1; }
