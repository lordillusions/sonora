#!/usr/bin/env bash
# COM-4/COM-7: Linux x64 installers from a Release UI build (docs/INSTALLERS.md).
#
#   packaging/linux/make_packages.sh <build-dir> <out-dir> [version]
#
# Writes <out-dir>/Sonora-<version>-x86_64.AppImage (shared libraries bundled by linuxdeploy) and
# <out-dir>/sonora_<version>_amd64.deb (/opt/sonora, dependencies from dpkg-shlibdeps). Runs inside
# ci/linux/Dockerfile, which provides linuxdeploy, appimagetool and the pinned AppImage runtime.
#
# Both packages keep the app's layout: the executable, sonora_plugin_scanner and Resources/ in one folder
# (PluginHost::scannerExecutable and the Help menu look next to the executable).
set -euo pipefail
cd "$(dirname "$0")/../.."
ROOT="$(pwd)"

fail() { echo "make_packages: $*" >&2; exit 1; }
[ $# -ge 2 ] || { sed -n '2,11p' "$0"; exit 2; }
BUILD="$(cd "$1" && pwd)"
mkdir -p "$2"
OUT="$(cd "$2" && pwd)"
VERSION="${3:-$(sed -nE 's/^project\(Sonora VERSION ([0-9.]+).*/\1/p' CMakeLists.txt)}"
[ -n "$VERSION" ] || fail "no version"
for tool in linuxdeploy appimagetool dpkg-deb dpkg-shlibdeps; do command -v "$tool" >/dev/null || fail "$tool missing (use ci/linux/Dockerfile)"; done
[ -f "${APPIMAGE_RUNTIME:-}" ] || fail "APPIMAGE_RUNTIME must name the type2 runtime file (set in ci/linux/Dockerfile)"

EXE="$(find "$BUILD/src/Sonora_artefacts" -maxdepth 2 -type f -name Sonora -perm -u+x | head -1)"
[ -n "$EXE" ] || fail "no Sonora executable under $BUILD/src/Sonora_artefacts (build with -DSONORA_WITH_UI=ON)"
APPDIR_SRC="$(dirname "$EXE")"
[ -x "$APPDIR_SRC/sonora_plugin_scanner" ] || fail "sonora_plugin_scanner missing next to $EXE"
[ -d "$APPDIR_SRC/Resources" ] || fail "Resources/ missing next to $EXE"

STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT

# Copies the app folder (executable, scanner, Resources) into $1 and strips the binaries.
payload() {
    mkdir -p "$1"
    cp "$EXE" "$APPDIR_SRC/sonora_plugin_scanner" "$1/"
    cp -r "$APPDIR_SRC/Resources" "$1/"
    strip --strip-unneeded "$1/Sonora" "$1/sonora_plugin_scanner"
}

icons() {
    install -Dm644 resources/brand/icon-256.png "$1/share/icons/hicolor/256x256/apps/sonora.png"
    install -Dm644 resources/brand/icon.svg "$1/share/icons/hicolor/scalable/apps/sonora.svg"
}

# ---------------------------------------------------------------- AppImage
echo "== AppImage"
APPDIR="$STAGE/Sonora.AppDir"
payload "$APPDIR/usr/bin"
icons "$APPDIR/usr"
sed 's/@EXEC@/Sonora/' packaging/linux/sonora.desktop.in > "$STAGE/sonora.desktop"
install -Dm644 packaging/linux/sonora-mime.xml "$APPDIR/usr/share/mime/packages/sonora.xml"
# linuxdeploy copies every shared library the two executables need, except the base system ones on its exclude
# list (glibc, libGL, libX11, fontconfig, ...), and writes AppRun + the root .desktop/icon.
(cd "$STAGE" && linuxdeploy --appdir "$APPDIR" \
    --executable "$APPDIR/usr/bin/Sonora" --executable "$APPDIR/usr/bin/sonora_plugin_scanner" \
    --desktop-file "$STAGE/sonora.desktop" --icon-file "$APPDIR/usr/share/icons/hicolor/256x256/apps/sonora.png")
APPIMAGE="$OUT/Sonora-$VERSION-x86_64.AppImage"
rm -f "$APPIMAGE"
ARCH=x86_64 VERSION="$VERSION" appimagetool --no-appstream --runtime-file "$APPIMAGE_RUNTIME" "$APPDIR" "$APPIMAGE"

# ---------------------------------------------------------------- .deb
echo "== .deb"
DEB="$STAGE/deb"
payload "$DEB/opt/sonora"
mkdir -p "$DEB/usr/bin"
ln -s /opt/sonora/Sonora "$DEB/usr/bin/sonora"
icons "$DEB/usr"
sed 's/@EXEC@/sonora/' packaging/linux/sonora.desktop.in | install -Dm644 /dev/stdin "$DEB/usr/share/applications/sonora.desktop"
install -Dm644 packaging/linux/sonora-mime.xml "$DEB/usr/share/mime/packages/sonora.xml"
install -Dm644 LICENSE "$DEB/usr/share/doc/sonora/copyright"

# dpkg-shlibdeps reads debian/control from the current directory.
mkdir -p "$STAGE/debian"
printf 'Source: sonora\n\nPackage: sonora\nArchitecture: amd64\n' > "$STAGE/debian/control"
DEPENDS="$(cd "$STAGE" && dpkg-shlibdeps -O -edeb/opt/sonora/Sonora -edeb/opt/sonora/sonora_plugin_scanner 2>/dev/null \
    | sed -n 's/^shlibs:Depends=//p')"
[ -n "$DEPENDS" ] || fail "dpkg-shlibdeps found no dependencies"
# JUCE opens the X11 libraries with dlopen at run time, so dpkg-shlibdeps cannot see them.
DEPENDS="$DEPENDS, libx11-6, libxext6, libxrandr2, libxinerama1, libxcursor1, libxrender1"
mkdir -p "$DEB/DEBIAN"
cat > "$DEB/DEBIAN/control" <<EOF
Package: sonora
Version: $VERSION
Section: sound
Priority: optional
Architecture: amd64
Maintainer: Sonora <noreply@sonora.invalid>
Depends: $DEPENDS
Recommends: fonts-dejavu-core
Suggests: jackd2
Installed-Size: $(du -sk "$DEB" | cut -f1)
Homepage: https://github.com/lordillusions/sonora
Description: Digital audio workstation that opens .als music sets
 Sonora plays and edits .als sets, hosts VST3 plug-ins and supports MIDI controllers.
 Desktop entry, icon and the .als file type are registered on install.
EOF
DEBFILE="$OUT/sonora_${VERSION}_amd64.deb"
dpkg-deb --root-owner-group --build "$DEB" "$DEBFILE" >/dev/null

(cd "$OUT" && sha256sum "$(basename "$APPIMAGE")" "$(basename "$DEBFILE")" > "Sonora-$VERSION-linux.sha256")
echo
echo "Linux installers in $OUT:"
ls -la "$APPIMAGE" "$DEBFILE" "$OUT/Sonora-$VERSION-linux.sha256"
