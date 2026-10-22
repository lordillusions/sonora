# Linux x64 build environment for Sonora (COM-7, docs/PLATFORMS.md).
#
#   docker build --platform linux/amd64 -t sonora-linux ci/linux
#   ci/linux/run.sh [headless|ui|all]        # from the repository root, on the host
#
# GCC is the compiler: skia-pack's Linux x64 Skia is built with gcc-10 (see cmake/Skia.cmake).
FROM ubuntu:24.04

ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build pkg-config git curl ca-certificates unzip python3 \
        libasound2-dev libjack-jackd2-dev \
        libcurl4-openssl-dev \
        libfreetype-dev libfontconfig1-dev \
        libx11-dev libxcomposite-dev libxcursor-dev libxext-dev libxinerama-dev libxrandr-dev libxrender-dev \
        libgl1-mesa-dev \
        fonts-dejavu-core xvfb xauth \
        dpkg-dev file \
    && rm -rf /var/lib/apt/lists/*

# Installer tools (packaging/linux/make_packages.sh, docs/INSTALLERS.md), pinned by release and SHA-256.
# The AppImages are unpacked because containers have no FUSE. Zeroing the AppImage magic (ELF bytes 8-10) lets
# emulated x64 (Rosetta/qemu on Apple silicon) run them; native x64 does not care.
ARG LINUXDEPLOY_URL=https://github.com/linuxdeploy/linuxdeploy/releases/download/1-alpha-20251107-1/linuxdeploy-x86_64.AppImage
ARG LINUXDEPLOY_SHA256=c20cd71e3a4e3b80c3483cef793cda3f4e990aca14014d23c544ca3ce1270b4d
ARG APPIMAGETOOL_URL=https://github.com/AppImage/appimagetool/releases/download/1.9.1/appimagetool-x86_64.AppImage
ARG APPIMAGETOOL_SHA256=ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0
ARG RUNTIME_URL=https://github.com/AppImage/type2-runtime/releases/download/20251108/runtime-x86_64
ARG RUNTIME_SHA256=2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d
RUN set -eu; cd /tmp; \
    curl -fsSL -o linuxdeploy.AppImage "$LINUXDEPLOY_URL"; echo "$LINUXDEPLOY_SHA256  linuxdeploy.AppImage" | sha256sum -c -; \
    curl -fsSL -o appimagetool.AppImage "$APPIMAGETOOL_URL"; echo "$APPIMAGETOOL_SHA256  appimagetool.AppImage" | sha256sum -c -; \
    curl -fsSL -o /opt/appimage-runtime-x86_64 "$RUNTIME_URL"; echo "$RUNTIME_SHA256  /opt/appimage-runtime-x86_64" | sha256sum -c -; \
    for t in linuxdeploy appimagetool; do \
        printf '\000\000\000' | dd of=$t.AppImage bs=1 seek=8 conv=notrunc status=none; \
        chmod +x $t.AppImage; ./$t.AppImage --appimage-extract >/dev/null; mv squashfs-root /opt/$t; \
        ln -s /opt/$t/AppRun /usr/local/bin/$t; rm $t.AppImage; \
    done
ENV APPIMAGE_RUNTIME=/opt/appimage-runtime-x86_64

# The source tree is bind-mounted here; dependencies and build output live in volumes (see run.sh).
WORKDIR /src
