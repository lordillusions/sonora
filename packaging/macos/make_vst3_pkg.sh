#!/usr/bin/env bash
# PLG-11c: macOS installer package for the Sonora Prism VST3 (docs/INSTALLERS.md).
#
#   packaging/macos/make_vst3_pkg.sh <Sonora Prism.vst3> <out.pkg>
#       [--sign-identity "Developer ID Application: ..."] [--installer-identity "Developer ID Installer: ..."]
#       [--notary-profile P]
#
# Installer.app puts the bundle in /Library/Audio/Plug-Ins/VST3 (all users, asks for an admin password) or, with
# "Install for me only", in ~/Library/Audio/Plug-Ins/VST3. The bundle is signed here: ad hoc by default (hosts on Apple
# silicon need a signature to load it), with --sign-identity as Developer ID + hardened runtime. --installer-identity
# signs the package; --notary-profile notarizes and staples it. Without them the package is for local tests: Gatekeeper
# blocks a downloaded unsigned package (right-click > Open).
set -euo pipefail

fail() { echo "make_vst3_pkg: $*" >&2; exit 1; }
[ $# -ge 2 ] || { sed -n '2,13p' "$0"; exit 2; }
BUNDLE="${1%/}" OUT="$2"; shift 2
IDENTITY="${SONORA_SIGN_IDENTITY:--}" INSTALLER_IDENTITY="${SONORA_INSTALLER_IDENTITY:-}" NOTARY="${SONORA_NOTARY_PROFILE:-}"
while [ $# -gt 0 ]; do
    case "$1" in
        --sign-identity) IDENTITY="$2"; shift 2 ;;
        --installer-identity) INSTALLER_IDENTITY="$2"; shift 2 ;;
        --notary-profile) NOTARY="$2"; shift 2 ;;
        *) fail "unknown option: $1" ;;
    esac
done
[ -f "$BUNDLE/Contents/Info.plist" ] && [ "${BUNDLE##*.}" = "vst3" ] || fail "$BUNDLE is not a VST3 bundle"
[ -z "$NOTARY" ] || [ -n "$INSTALLER_IDENTITY" ] || fail "notarization needs a Developer ID Installer signature (--installer-identity)"
[ -z "$NOTARY" ] || [ "$IDENTITY" != "-" ] || fail "notarization needs a Developer ID Application signature (--sign-identity)"

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
NAME="$(basename "$BUNDLE" .vst3)"
PLIST="$BUNDLE/Contents/Info.plist"
VERSION="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "$PLIST")"
PKG_ID="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$PLIST").pkg"
[ "$(lipo -archs "$BUNDLE/Contents/MacOS/$NAME")" = "arm64" ] || fail "expected an arm64-only bundle (the prebuilt Skia is arm64)"
STAGE="$(mktemp -d "${TMPDIR:-/tmp}/sonora-vst3-pkg.XXXXXX")"
trap 'rm -rf "$STAGE"' EXIT

echo "== sign $NAME.vst3 ($([ "$IDENTITY" = "-" ] && echo "ad hoc, local tests only" || echo "$IDENTITY"))"
mkdir -p "$STAGE/root"
ditto "$BUNDLE" "$STAGE/root/$NAME.vst3"
if [ "$IDENTITY" = "-" ]; then
    codesign --force --sign - "$STAGE/root/$NAME.vst3" 2>/dev/null # replaces the linker's ad hoc signature
else
    codesign --force --timestamp --options runtime --sign "$IDENTITY" "$STAGE/root/$NAME.vst3"
fi
codesign --verify --strict "$STAGE/root/$NAME.vst3"

# Not relocatable: otherwise Installer "upgrades" any copy with the same bundle id it finds (a build folder) instead.
# Not version checked: installing this package always replaces the bundle, also with an older or equal version.
pkgbuild --analyze --root "$STAGE/root" "$STAGE/components.plist" >/dev/null 2>&1
for key in BundleIsRelocatable BundleIsVersionChecked; do
    /usr/libexec/PlistBuddy -c "Delete :0:$key" "$STAGE/components.plist" 2>/dev/null || true
    /usr/libexec/PlistBuddy -c "Add :0:$key bool false" "$STAGE/components.plist"
done
pkgbuild --quiet --root "$STAGE/root" --component-plist "$STAGE/components.plist" --identifier "$PKG_ID" \
    --version "$VERSION" --install-location "/Library/Audio/Plug-Ins/VST3" "$STAGE/component.pkg"

mkdir "$STAGE/resources"
cp "$ROOT/LICENSE" "$STAGE/resources/LICENSE.txt"
cat > "$STAGE/resources/welcome.html" <<EOF
<html><body style="font-family: -apple-system, sans-serif; font-size: 13px">
<h3>$NAME $VERSION</h3>
<p>Installs the $NAME instrument as a VST3 plug-in for Apple silicon Macs (macOS 13 or later).</p>
<p>Choose <b>Install for all users</b> (/Library/Audio/Plug-Ins/VST3) or <b>Install for me only</b>
(~/Library/Audio/Plug-Ins/VST3) on the destination step.</p>
</body></html>
EOF
cat > "$STAGE/resources/conclusion.html" <<EOF
<html><body style="font-family: -apple-system, sans-serif; font-size: 13px">
<h3>$NAME is installed</h3>
<p>Rescan plug-ins in your host (or restart it), then load <b>$NAME</b> from its VST3 instruments (vendor Sonora).</p>
<p>To remove it, delete <code>$NAME.vst3</code> from the VST3 folder above.</p>
</body></html>
EOF
cat > "$STAGE/distribution.xml" <<EOF
<?xml version="1.0" encoding="utf-8"?>
<installer-gui-script minSpecVersion="2">
    <title>$NAME $VERSION</title>
    <welcome file="welcome.html" mime-type="text/html"/>
    <license file="LICENSE.txt" mime-type="text/plain"/>
    <conclusion file="conclusion.html" mime-type="text/html"/>
    <options customize="never" require-scripts="false" hostArchitectures="arm64"/>
    <domains enable_anywhere="false" enable_currentUserHome="true" enable_localSystem="true"/>
    <volume-check><allowed-os-versions><os-version min="13.0"/></allowed-os-versions></volume-check>
    <choices-outline><line choice="$PKG_ID"/></choices-outline>
    <choice id="$PKG_ID" visible="false" title="$NAME VST3"><pkg-ref id="$PKG_ID"/></choice>
    <pkg-ref id="$PKG_ID" version="$VERSION" onConclusion="none">component.pkg</pkg-ref>
</installer-gui-script>
EOF

mkdir -p "$(dirname "$OUT")"
rm -f "$OUT"
echo "== package $OUT"
SIGN_PKG=()
[ -z "$INSTALLER_IDENTITY" ] || SIGN_PKG=(--sign "$INSTALLER_IDENTITY" --timestamp)
productbuild --quiet --distribution "$STAGE/distribution.xml" --resources "$STAGE/resources" --package-path "$STAGE" \
    ${SIGN_PKG[@]+"${SIGN_PKG[@]}"} "$OUT"
if [ -n "$NOTARY" ]; then
    echo "== notarize $OUT (profile $NOTARY)"
    xcrun notarytool submit "$OUT" --keychain-profile "$NOTARY" --wait
    xcrun stapler staple "$OUT"
fi
[ -z "$INSTALLER_IDENTITY" ] || pkgutil --check-signature "$OUT"
(cd "$(dirname "$OUT")" && shasum -a 256 "$(basename "$OUT")" > "$(basename "$OUT" .pkg).sha256")
echo "$OUT ($(du -h "$OUT" | cut -f1))"
