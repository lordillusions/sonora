#!/usr/bin/env bash
# COM-4: macOS installer disk image (docs/INSTALLERS.md): the app next to an Applications link, drag to install.
#
#   packaging/macos/make_dmg.sh <Sonora.app> <out.dmg> [--sign-identity ID] [--notary-profile P]
#
# The app must already be signed (and notarized + stapled for distribution): scripts/release/make_release.sh does that
# and then calls this script. With a Developer ID identity the image itself is signed; with a notary profile it is
# notarized and stapled too, so Gatekeeper accepts it offline. Without them the image is for local tests only.
set -euo pipefail

fail() { echo "make_dmg: $*" >&2; exit 1; }
[ $# -ge 2 ] || { sed -n '2,8p' "$0"; exit 2; }
APP="${1%/}" OUT="$2"; shift 2
IDENTITY="${SONORA_SIGN_IDENTITY:--}" NOTARY="${SONORA_NOTARY_PROFILE:-}"
while [ $# -gt 0 ]; do
    case "$1" in
        --sign-identity) IDENTITY="$2"; shift 2 ;;
        --notary-profile) NOTARY="$2"; shift 2 ;;
        *) fail "unknown option: $1" ;;
    esac
done
[ -d "$APP/Contents/MacOS" ] || fail "$APP is not an app bundle"
codesign --verify --deep --strict "$APP" || fail "$APP is not validly signed (make_release.sh signs it)"

NAME="$(basename "$APP" .app)"
VERSION="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "$APP/Contents/Info.plist")"
STAGE="$(mktemp -d "${TMPDIR:-/tmp}/sonora-dmg.XXXXXX")"
trap 'rm -rf "$STAGE"' EXIT
mkdir "$STAGE/root"
ditto "$APP" "$STAGE/root/$NAME.app"
ln -s /Applications "$STAGE/root/Applications"

mkdir -p "$(dirname "$OUT")"
rm -f "$OUT"
echo "== disk image $OUT"
hdiutil create -quiet -volname "$NAME $VERSION" -srcfolder "$STAGE/root" -fs HFS+ -format UDZO -imagekey zlib-level=9 \
    -ov "$OUT"

if [ "$IDENTITY" != "-" ]; then
    codesign --force --timestamp --sign "$IDENTITY" "$OUT"
    codesign --verify "$OUT"
fi
if [ -n "$NOTARY" ]; then
    [ "$IDENTITY" != "-" ] || fail "notarization needs a Developer ID signature (--sign-identity)"
    echo "== notarize $OUT (profile $NOTARY)"
    xcrun notarytool submit "$OUT" --keychain-profile "$NOTARY" --wait
    xcrun stapler staple "$OUT"
fi
hdiutil verify -quiet "$OUT"
echo "$OUT ($(du -h "$OUT" | cut -f1))"
