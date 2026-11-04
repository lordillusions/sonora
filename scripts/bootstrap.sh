#!/usr/bin/env bash
# Fetches pinned third-party dependencies into third_party/ and checks the toolchain.
# Safe to re-run. Requires: macOS arm64, Xcode Command Line Tools, Homebrew (other platforms: below).
set -euo pipefail
cd "$(dirname "$0")/.."

JUCE_TAG="8.0.16-security"
SKIA_RELEASE="m144-e2e6623374-4"
SKIA_ZIP="Skia-${SKIA_RELEASE}-macos-Release-arm64.zip"
# COM-4: Sparkle 2 updater (framework embedded in Sonora.app, tools used by scripts/release/). See docs/UPDATES.md.
SPARKLE_VERSION="2.9.6"
SPARKLE_SHA256="52bf9e88cdd972fc0c81501377a880e90d47031bd8ca5462488f843e2609e192"
# COM-4l: WinSparkle (MIT), Sparkle's Windows port: same appcast format and EdDSA key (src/app/UpdaterWinSparkle.cpp).
WINSPARKLE_VERSION="0.9.4"
WINSPARKLE_SHA256="6037df37fc263bd1650a1c4949681a9d40ffe991d01f35892a406cb5d103c976"
# COM-4l: Monocypher (CC0 / BSD-2-Clause), audited Ed25519 used to verify Linux updates against the same EdDSA key.
MONOCYPHER_VERSION="4.0.3"
MONOCYPHER_SHA256="8cc9bc341a66249016db9bd70e9142d8d0aef9945973744b1ac05dbc55d8ee66"
fetch_monocypher() {
    local dir="$1/monocypher"
    [ "$(cat "$dir/VERSION" 2>/dev/null)" = "$MONOCYPHER_VERSION" ] && return 0
    rm -rf "$dir" && mkdir -p "$dir"
    curl -fsSL -o "$dir/m.tgz" \
        "https://github.com/LoupVaillant/Monocypher/releases/download/${MONOCYPHER_VERSION}/monocypher-${MONOCYPHER_VERSION}.tar.gz"
    if (cd "$dir" && { command -v sha256sum >/dev/null && echo "${MONOCYPHER_SHA256}  m.tgz" | sha256sum -c - >/dev/null \
            || echo "${MONOCYPHER_SHA256}  m.tgz" | shasum -a 256 -c - >/dev/null; }); then
        tar -xzf "$dir/m.tgz" -C "$dir" --strip-components=1 && rm "$dir/m.tgz" && echo "$MONOCYPHER_VERSION" > "$dir/VERSION"
    else
        echo "warning: Monocypher checksum mismatch, Linux updates will be off" >&2; rm -rf "$dir"
    fi
}
# COL-3: libdatachannel (MPL-2.0) for collab WebRTC data channels + WebSocket signalling; optional, CMake
# turns SONORA_WITH_COLLAB_NET off without it (docs/PLATFORMS.md "Collab network"). Built against OpenSSL 3.
LIBDATACHANNEL_TAG="v0.24.6"
LIBDATACHANNEL_URL="https://github.com/paullouisageneau/libdatachannel.git"
# COL-3: OpenSSL 3.5 LTS for libdatachannel on macOS, built static for Sonora's deployment target (Homebrew's
# openssl@3 is built for the newest macOS only). OPENSSLDIR=/etc/ssl so default CA paths use macOS's /etc/ssl/cert.pem.
OPENSSL_VERSION="3.5.9"
OPENSSL_SHA256="603f5602e2eef00d77fbd429d34dcd5822bb301757a1bc9cdb24c670f1eb859a"
# PLG-15: FST (GPL-3.0-or-later), clean-room VST2 interface headers that let JUCE host VST2 plug-ins without Steinberg's
# SDK (docs/adr/0003-plugin-format.md). Without it CMake builds with VST2 hosting off.
FST_URL="https://git.iem.at/zmoelnig/FST.git"
FST_COMMIT="647af068765b75867e3a28b4dd8991ab9ed47f7c" # v0.177.0 + 1
# src/plugins/vst2/fst-host-layout.patch gives three structs the sizes hosts and plug-ins allocate (heap/stack overruns
# otherwise); CMake turns VST2 off when it is missing.
fetch_fst() {
    local dir="$1/fst"
    [ "$(git -C "$dir" rev-parse HEAD 2>/dev/null)" = "$FST_COMMIT" ] && grep -q "Sonora PLG-15 r2" "$dir/fst/fst.h" && return 0
    rm -rf "$dir"
    git clone --quiet "$FST_URL" "$dir" && git -C "$dir" checkout --quiet "$FST_COMMIT" \
        && git -C "$dir" apply "$PWD/src/plugins/vst2/fst-host-layout.patch" \
        || { echo "warning: could not fetch FST, VST2 hosting will be off" >&2; rm -rf "$dir"; }
}

# COM-7: Linux x64 and Windows x64 (Git Bash) fetch their own Skia build and expect cmake/ninja to be
# installed already (docs/PLATFORMS.md). SONORA_THIRD_PARTY_DIR puts the dependencies elsewhere (the
# Linux container keeps its own copy); SONORA_FETCH_SKIA=0 skips Skia for headless builds,
# SONORA_FETCH_COLLAB_NET=0 skips libdatachannel (it also needs the OpenSSL 3 dev package to be used).
case "$(uname -s)" in
    Linux) SKIA_TARGET="linux-Release-x64" ;;
    MINGW* | MSYS* | CYGWIN*) SKIA_TARGET="windows-Release-x64" ;;
    *) SKIA_TARGET="" ;;
esac
if [ -n "$SKIA_TARGET" ]; then
    tp="${SONORA_THIRD_PARTY_DIR:-third_party}"
    command -v cmake >/dev/null || { echo "cmake is required" >&2; exit 1; }
    [ -d "$tp/JUCE" ] || git clone --depth 1 --branch "$JUCE_TAG" https://github.com/juce-framework/JUCE.git "$tp/JUCE"
    skia_out="$tp/skia/out/Release-${SKIA_TARGET%%-*}-${SKIA_TARGET##*-}" # e.g. out/Release-linux-x64
    if [ "${SONORA_FETCH_SKIA:-1}" != 0 ] && [ ! -d "$skia_out" ]; then
        zip="Skia-${SKIA_RELEASE}-${SKIA_TARGET}.zip"
        mkdir -p "$tp/skia"
        curl -fL -o "$tp/skia/$zip" "https://github.com/JetBrains/skia-pack/releases/download/${SKIA_RELEASE}/${zip}"
        (cd "$tp/skia" && unzip -q -o "$zip" && rm "$zip")
    fi
    if [ "${SONORA_FETCH_COLLAB_NET:-1}" != 0 ] && [ ! -d "$tp/libdatachannel" ]; then
        git clone --depth 1 --branch "$LIBDATACHANNEL_TAG" --recurse-submodules --shallow-submodules \
            "$LIBDATACHANNEL_URL" "$tp/libdatachannel"
    fi
    fetch_fst "$tp"
    fetch_monocypher "$tp"
    if [ "$SKIA_TARGET" = windows-Release-x64 ] && [ "$(cat "$tp/winsparkle/VERSION" 2>/dev/null)" != "$WINSPARKLE_VERSION" ]; then
        rm -rf "$tp/winsparkle" && mkdir -p "$tp/winsparkle"
        curl -fL -o "$tp/winsparkle/ws.zip" \
            "https://github.com/vslavik/winsparkle/releases/download/v${WINSPARKLE_VERSION}/WinSparkle-${WINSPARKLE_VERSION}.zip"
        echo "${WINSPARKLE_SHA256}  $tp/winsparkle/ws.zip" | sha256sum -c -
        (cd "$tp/winsparkle" && unzip -q ws.zip -x '__MACOSX/*' && mv "WinSparkle-${WINSPARKLE_VERSION}"/* . \
            && rm -rf ws.zip "WinSparkle-${WINSPARKLE_VERSION}" x64/Release/WinSparkle.pdb)
        echo "$WINSPARKLE_VERSION" > "$tp/winsparkle/VERSION"
    fi
    echo "OK. Now: ci/linux/build.sh (Linux) or see docs/PLATFORMS.md (Windows)"
    exit 0
fi

command -v cmake >/dev/null || brew install cmake
command -v ninja >/dev/null || brew install ninja

if [ ! -d third_party/JUCE ]; then
    git clone --depth 1 --branch "$JUCE_TAG" https://github.com/juce-framework/JUCE.git third_party/JUCE
fi

if [ ! -f third_party/skia/out/Release-macos-arm64/libskia.a ]; then
    mkdir -p third_party/skia
    curl -fL -o "third_party/skia/$SKIA_ZIP" \
        "https://github.com/JetBrains/skia-pack/releases/download/${SKIA_RELEASE}/${SKIA_ZIP}"
    (cd third_party/skia && unzip -q "$SKIA_ZIP" && rm "$SKIA_ZIP")
fi

if [ ! -f "third_party/sparkle/VERSION" ] || [ "$(cat third_party/sparkle/VERSION)" != "$SPARKLE_VERSION" ]; then
    rm -rf third_party/sparkle && mkdir -p third_party/sparkle
    curl -fL -o "third_party/sparkle/Sparkle.tar.xz" \
        "https://github.com/sparkle-project/Sparkle/releases/download/${SPARKLE_VERSION}/Sparkle-${SPARKLE_VERSION}.tar.xz"
    echo "${SPARKLE_SHA256}  third_party/sparkle/Sparkle.tar.xz" | shasum -a 256 -c -
    # ditto/tar keep the framework's symlinks and code signature intact.
    (cd third_party/sparkle && tar -xJf Sparkle.tar.xz && rm Sparkle.tar.xz)
    echo "$SPARKLE_VERSION" > third_party/sparkle/VERSION
fi

if [ ! -d third_party/libdatachannel ]; then
    git clone --depth 1 --branch "$LIBDATACHANNEL_TAG" --recurse-submodules --shallow-submodules \
        "$LIBDATACHANNEL_URL" third_party/libdatachannel
fi

fetch_fst third_party
fetch_monocypher third_party

ossl="$PWD/third_party/openssl-macos-arm64"
if [ ! -f "$ossl/lib/libssl.a" ] || [ "$(cat "$ossl/VERSION" 2>/dev/null)" != "$OPENSSL_VERSION" ]; then
    rm -rf "$ossl"
    ossl_src="$(mktemp -d)"
    curl -fL -o "$ossl_src/openssl.tar.gz" \
        "https://github.com/openssl/openssl/releases/download/openssl-${OPENSSL_VERSION}/openssl-${OPENSSL_VERSION}.tar.gz"
    echo "${OPENSSL_SHA256}  $ossl_src/openssl.tar.gz" | shasum -a 256 -c -
    tar -xzf "$ossl_src/openssl.tar.gz" -C "$ossl_src"
    # Static only (no dylib in Sonora.app), same minimum macOS as CMAKE_OSX_DEPLOYMENT_TARGET.
    (cd "$ossl_src/openssl-${OPENSSL_VERSION}" &&
        ./Configure darwin64-arm64-cc no-shared no-tests no-docs no-apps -mmacosx-version-min=13.0 \
            --prefix="$ossl" --libdir=lib --openssldir=/etc/ssl &&
        make -j"$(sysctl -n hw.ncpu)" && make install_sw)
    rm -rf "$ossl_src"
    echo "$OPENSSL_VERSION" > "$ossl/VERSION"
fi

echo "OK. Now: cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug && cmake --build build"
