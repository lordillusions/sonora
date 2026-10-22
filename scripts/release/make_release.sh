#!/usr/bin/env bash
# COM-4: builds a Sonora release archive and the Sparkle appcast (+ delta updates from recent releases), ready to upload.
# Publishes nothing: it prints the files to attach to the GitHub Release and the appcast to upload. See docs/UPDATES.md.
#
#   scripts/release/make_release.sh --version 0.2.0 \
#       --feed-url https://example.org/sonora/appcast.xml --public-key "$SONORA_UPDATE_PUBLIC_KEY" \
#       --download-url-prefix https://github.com/OWNER/REPO/releases/download/v0.2.0/ \
#       --sign-identity "Developer ID Application: NAME (TEAMID)" --notary-profile sonora-notary
#
# The archives folder (default dist/<channel>/) keeps every previous release zip and appcast.xml: generate_appcast reads
# them to write deltas from the last --max-deltas versions to the new one and moves releases it drops into old_updates/.
# Keep that folder between releases (it is the input for the next deltas), but do not upload old_updates/.
set -euo pipefail
cd "$(dirname "$0")/../.."
ROOT="$(pwd)"
SPARKLE="$ROOT/third_party/sparkle"

usage() { sed -n '2,13p' "$0"; cat <<'EOF'

Options (environment variable in brackets):
  --version X               CFBundleShortVersionString, required
  --build N                 CFBundleVersion, the value Sparkle compares (default: the version)
  --channel C               stable (default), beta (stable app, item tagged beta) or nightly (separate app and feed)
  --feed-url URL            SUFeedURL compiled into the app [SONORA_UPDATE_FEED_URL]
  --public-key KEY          SUPublicEDKey compiled into the app [SONORA_UPDATE_PUBLIC_KEY]
  --download-url-prefix URL where this release's zip/deltas will be downloaded from (required)
  --archives DIR            previous releases + appcast.xml (default dist/<stable|nightly>)
  --ed-key-file FILE        private EdDSA key file (default: generate_keys' login keychain item) [SONORA_ED_KEY_FILE]
  --sign-identity ID        codesign identity; "-" = ad hoc for local tests (default) [SONORA_SIGN_IDENTITY]
  --notary-profile P        notarytool keychain profile; empty skips notarization [SONORA_NOTARY_PROFILE]
  --notes FILE              release notes (.md/.html) placed next to the archive
  --max-deltas N            delta updates from the N latest previous versions (default 3)
  --phased-rollout SECONDS  staged rollout interval for this item (default: none)
  --build-dir DIR           CMake build directory (default build-release-app)
  --config CFG              Release (default) or Debug
  --local-test              local update test build: plain-HTTP localhost feed, SONORA_UPDATE_AUTOTEST hook
  --bundle-id ID            bundle id override (local tests)
EOF
}

VERSION="" BUILD="" CHANNEL="stable" PREFIX="" ARCHIVES="" NOTES="" MAX_DELTAS=3 PHASED="" BUILD_DIR="build-release-app"
CONFIG="Release" LOCAL_TEST=OFF BUNDLE_ID=""
FEED_URL="${SONORA_UPDATE_FEED_URL:-}" PUBLIC_KEY="${SONORA_UPDATE_PUBLIC_KEY:-}" ED_KEY_FILE="${SONORA_ED_KEY_FILE:-}"
IDENTITY="${SONORA_SIGN_IDENTITY:--}" NOTARY="${SONORA_NOTARY_PROFILE:-}"
while [ $# -gt 0 ]; do
    case "$1" in
        --version) VERSION="$2"; shift 2 ;;
        --build) BUILD="$2"; shift 2 ;;
        --channel) CHANNEL="$2"; shift 2 ;;
        --feed-url) FEED_URL="$2"; shift 2 ;;
        --public-key) PUBLIC_KEY="$2"; shift 2 ;;
        --download-url-prefix) PREFIX="$2"; shift 2 ;;
        --archives) ARCHIVES="$2"; shift 2 ;;
        --ed-key-file) ED_KEY_FILE="$2"; shift 2 ;;
        --sign-identity) IDENTITY="$2"; shift 2 ;;
        --notary-profile) NOTARY="$2"; shift 2 ;;
        --notes) NOTES="$2"; shift 2 ;;
        --max-deltas) MAX_DELTAS="$2"; shift 2 ;;
        --phased-rollout) PHASED="$2"; shift 2 ;;
        --build-dir) BUILD_DIR="$2"; shift 2 ;;
        --config) CONFIG="$2"; shift 2 ;;
        --local-test) LOCAL_TEST=ON; shift ;;
        --bundle-id) BUNDLE_ID="$2"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

fail() { echo "make_release: $*" >&2; exit 1; }
[ -n "$VERSION" ] || fail "--version is required"
[ -n "$PREFIX" ] || fail "--download-url-prefix is required (the URL the new archive will be downloaded from)"
[ -n "$FEED_URL" ] || fail "--feed-url / SONORA_UPDATE_FEED_URL is required"
[ -n "$PUBLIC_KEY" ] || fail "--public-key / SONORA_UPDATE_PUBLIC_KEY is required (generate_keys -p prints it)"
[ -x "$SPARKLE/bin/generate_appcast" ] || fail "Sparkle tools missing: run scripts/bootstrap.sh"
case "$CHANNEL" in stable|beta|nightly) ;; *) fail "--channel must be stable, beta or nightly" ;; esac
case "$PREFIX" in */) ;; *) PREFIX="$PREFIX/" ;; esac
BUILD="${BUILD:-$VERSION}"
APP_CHANNEL=stable; [ "$CHANNEL" = nightly ] && APP_CHANNEL=nightly
PRODUCT="Sonora"; [ "$APP_CHANNEL" = nightly ] && PRODUCT="Sonora Nightly"
ARCHIVES="${ARCHIVES:-$ROOT/dist/$APP_CHANNEL}"
mkdir -p "$ARCHIVES"
ARCHIVES="$(cd "$ARCHIVES" && pwd)"

echo "== build $PRODUCT $VERSION ($BUILD), $CONFIG, channel $CHANNEL"
cmake -S "$ROOT" -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE="$CONFIG" \
    -DSONORA_APP_VERSION="$VERSION" -DSONORA_APP_BUILD="$BUILD" -DSONORA_UPDATE_CHANNEL="$APP_CHANNEL" \
    -DSONORA_UPDATE_FEED_URL="$FEED_URL" -DSONORA_UPDATE_PUBLIC_KEY="$PUBLIC_KEY" \
    -DSONORA_UPDATE_ALLOW_LOCAL_HTTP="$LOCAL_TEST" -DSONORA_BUNDLE_ID="$BUNDLE_ID" -DSONORA_WITH_SPARKLE=ON >/dev/null
cmake --build "$BUILD_DIR" --target Sonora
BUILT="$BUILD_DIR/src/Sonora_artefacts/$CONFIG/$PRODUCT.app"
[ -d "$BUILT/Contents/Frameworks/Sparkle.framework" ] || fail "$BUILT has no embedded Sparkle.framework"
PLIST="$BUILT/Contents/Info.plist"
[ "$(/usr/libexec/PlistBuddy -c 'Print :CFBundleVersion' "$PLIST")" = "$BUILD" ] || fail "Info.plist CFBundleVersion is not $BUILD"

STAGE="$(mktemp -d "${TMPDIR:-/tmp}/sonora-release.XXXXXX")"
trap 'rm -rf "$STAGE"' EXIT
APP="$STAGE/$PRODUCT.app"
ditto "$BUILT" "$APP"

echo "== sign ($([ "$IDENTITY" = "-" ] && echo "ad hoc, local tests only" || echo "$IDENTITY"))"
ENTITLEMENTS="$ROOT/scripts/release/Sonora.entitlements"
FW="$APP/Contents/Frameworks/Sparkle.framework"
if [ "$IDENTITY" = "-" ]; then
    # Ad hoc: Sparkle keeps its own signature; the app and the plug-in scanner get an ad hoc one. Not for distribution.
    codesign --force --sign - "$APP/Contents/MacOS/sonora_plugin_scanner"
    codesign --force --sign - "$APP"
else
    # Inside-out, hardened runtime (required for notarization). Order and flags follow Sparkle's documentation.
    SIGN=(codesign --force --timestamp --options runtime --sign "$IDENTITY")
    "${SIGN[@]}" "$FW/Versions/B/XPCServices/Installer.xpc"
    "${SIGN[@]}" --preserve-metadata=entitlements "$FW/Versions/B/XPCServices/Downloader.xpc"
    "${SIGN[@]}" "$FW/Versions/B/Autoupdate"
    "${SIGN[@]}" "$FW/Versions/B/Updater.app"
    "${SIGN[@]}" "$FW"
    "${SIGN[@]}" --entitlements "$ENTITLEMENTS" "$APP/Contents/MacOS/sonora_plugin_scanner"
    "${SIGN[@]}" --entitlements "$ENTITLEMENTS" "$APP"
fi
codesign --verify --deep --strict "$APP"

if [ -n "$NOTARY" ]; then
    [ "$IDENTITY" != "-" ] || fail "notarization needs a Developer ID signature (--sign-identity)"
    echo "== notarize (profile $NOTARY)"
    ditto -c -k --keepParent "$APP" "$STAGE/notarize.zip"
    xcrun notarytool submit "$STAGE/notarize.zip" --keychain-profile "$NOTARY" --wait
    xcrun stapler staple "$APP"
else
    echo "== notarize: skipped (no --notary-profile); Gatekeeper will block this build on other Macs"
fi

BASENAME="${PRODUCT// /-}-$VERSION"
ZIP="$ARCHIVES/$BASENAME.zip"
[ ! -e "$ZIP" ] || fail "$ZIP already exists: a released version is never rebuilt, bump --version"
echo "== archive $ZIP"
ditto -c -k --sequesterRsrc --keepParent "$APP" "$ZIP"
[ -z "$NOTES" ] || cp "$NOTES" "$ARCHIVES/$BASENAME.${NOTES##*.}"
# First-install disk image. Kept out of $ARCHIVES: generate_appcast would publish it as a second update archive.
INSTALLERS="$(dirname "$ARCHIVES")/installers"
DMG="$INSTALLERS/$BASENAME.dmg"
"$ROOT/packaging/macos/make_dmg.sh" "$APP" "$DMG" --sign-identity "$IDENTITY" ${NOTARY:+--notary-profile "$NOTARY"}

echo "== appcast + deltas"
MARK="$STAGE/mark"; touch "$MARK"; sleep 1
# generate_appcast applies --download-url-prefix to every item it touches. Each GitHub Release has its own URL, so files
# published earlier keep the URL they were published under (pin_urls.py), otherwise fallback items would point at the new tag.
[ -f "$ARCHIVES/appcast.xml" ] && cp "$ARCHIVES/appcast.xml" "$STAGE/appcast.before.xml"
GEN=("$SPARKLE/bin/generate_appcast" --download-url-prefix "$PREFIX" --maximum-deltas "$MAX_DELTAS" --embed-release-notes
     -o "$ARCHIVES/appcast.xml")
[ "$CHANNEL" = beta ] && GEN+=(--channel beta)
[ -n "$PHASED" ] && GEN+=(--phased-rollout-interval "$PHASED")
[ -n "$ED_KEY_FILE" ] && GEN+=(--ed-key-file "$ED_KEY_FILE")
"${GEN[@]}" "$ARCHIVES"
[ -f "$STAGE/appcast.before.xml" ] && python3 -I "$ROOT/scripts/release/pin_urls.py" "$STAGE/appcast.before.xml" "$ARCHIVES/appcast.xml"

UPLOAD=()
while IFS= read -r file; do UPLOAD+=("$file"); done < <(cd "$ARCHIVES" && find . -maxdepth 1 -type f \( -newer "$MARK" -o -name "$BASENAME.*" \) ! -name appcast.xml | sed 's|^\./||' | sort)
(cd "$ARCHIVES" && shasum -a 256 "${UPLOAD[@]}" > "$BASENAME.sha256")
echo
echo "Ready, nothing was uploaded. Attach to the release ($PREFIX):"
for file in "${UPLOAD[@]}" "$BASENAME.sha256"; do echo "  $ARCHIVES/$file  ($(du -h "$ARCHIVES/$file" | cut -f1))"; done
echo "  $DMG  ($(du -h "$DMG" | cut -f1), installer for new users)"
echo "Then upload the appcast to the feed URL ($FEED_URL):"
echo "  $ARCHIVES/appcast.xml"
