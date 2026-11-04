#!/usr/bin/env bash
# COM-4l: Linux update end-to-end test (docs/UPDATES.md "Windows and Linux"). Runs in ci/linux/Dockerfile's image:
#
#   docker run --rm --platform linux/amd64 -v "$PWD":/src -v sonora-linux-deps:/deps -v sonora-linux-build:/build \
#       -v <empty scratch dir>:/w sonora-linux ci/linux/update_e2e.sh
#
# Builds a Release "0.1.6" with a local feed and a throwaway key (never the release key), packages it, publishes a signed
# "0.1.9" on 127.0.0.1, then checks: the AppImage replaces itself and relaunches; a tampered download is refused; the .deb
# installs the newer package (pkexec stand-in, the container runs as root) and relaunches. Prints PASS/FAIL lines.
# It reconfigures /build/build-linux-e2e only. Linux-only work files go to /w.
set -euo pipefail
FEED=http://127.0.0.1:8768/appcast-linux.xml
mkdir -p /w
# Throwaway Ed25519 key: the raw 32-byte seed (what generate_keys -x exports) and the raw public key, both base64.
openssl genpkey -algorithm ed25519 -outform DER -out /w/key.der
tail -c 32 /w/key.der | base64 -w0 > /w/test.key
PUB="$(openssl pkey -inform DER -in /w/key.der -pubout -outform DER | tail -c 32 | base64 -w0)"
B=/build/build-linux-e2e
cd /src
echo "== build 0.1.6 with the local test feed"
cmake -S . -B $B -G Ninja -DCMAKE_BUILD_TYPE=Release -DSONORA_WITH_UI=ON -DSONORA_THIRD_PARTY_DIR=/deps -DSONORA_BUILD_TESTS=OFF \
    -DSONORA_APP_VERSION=0.1.6 -DSONORA_UPDATE_FEED_URL=$FEED -DSONORA_UPDATE_PUBLIC_KEY="$PUB" -DSONORA_UPDATE_ALLOW_LOCAL_HTTP=ON >/dev/null
cmake --build $B -j "${SONORA_JOBS:-6}" --target Sonora 2>&1 | grep -E "warning:|error:|FAILED" || true
rm -rf /w/out /w/web && mkdir -p /w/out /w/web
packaging/linux/make_packages.sh $B /w/out 0.1.6 >/w/pkg-016.log 2>&1
packaging/linux/make_packages.sh $B /w/web 0.1.9 >/w/pkg-019.log 2>&1
# The "new" AppImage is a stand-in that proves the relaunch: it writes a marker.
rm -f /w/web/Sonora-0.1.9-x86_64.AppImage
printf '#!/bin/sh\necho "relaunched as 0.1.9 $*" > /tmp/relaunched\n' > /w/web/Sonora-0.1.9-x86_64.AppImage
head -c 1000 /dev/urandom > /w/web/Sonora-0.1.9-windows-x64-setup.exe
python3 -I scripts/release/write_platform_feeds.py --version 0.1.9 --dist /w/web --key-file /w/test.key --public-key "$PUB" >/dev/null
sed -i 's|https://github.com/lordillusions/sonora/releases/download/v0.1.9/|http://127.0.0.1:8768/|' /w/web/appcast-linux.xml
(cd /w/web && python3 -I -m http.server 8768 --bind 127.0.0.1 >/w/http.log 2>&1 &)
# One X server for everything: a relaunched Sonora must outlive the process that started it.
(Xvfb :99 -screen 0 1440x900x24 >/dev/null 2>&1 &)
export DISPLAY=:99
sleep 1

echo "== AppImage 0.1.6 -> 0.1.9"
mkdir -p /tmp/apptest && cp /w/out/Sonora-0.1.6-x86_64.AppImage /tmp/apptest/Sonora.AppImage && chmod +x /tmp/apptest/Sonora.AppImage
# Emulated x64 (Apple silicon) runs an AppImage only with its magic zeroed (ci/linux/Dockerfile); real x64 does not care.
printf '\000\000\000' | dd of=/w/out/Sonora-0.1.6-x86_64.AppImage bs=1 seek=8 conv=notrunc status=none
cp /w/out/Sonora-0.1.6-x86_64.AppImage /tmp/apptest/Sonora.AppImage
rm -f /tmp/relaunched
(APPIMAGE_EXTRACT_AND_RUN=1 SONORA_UPDATE_AUTOTEST=1 timeout 300 /tmp/apptest/Sonora.AppImage >/w/appimage.log 2>&1 &)
for i in $(seq 1 150); do [ -f /tmp/relaunched ] && break; sleep 1; done
grep "updates" /w/appimage.log || true
[ -f /tmp/relaunched ] && echo "PASS AppImage: $(cat /tmp/relaunched); file is now: $(head -c 40 /tmp/apptest/Sonora.AppImage | head -1)" || echo "FAIL AppImage: no relaunch"

echo "== tampered AppImage is refused"
cp /w/out/Sonora-0.1.6-x86_64.AppImage /tmp/apptest/Sonora.AppImage
cp /w/web/Sonora-0.1.9-x86_64.AppImage /w/web/good.AppImage
printf '#!/bin/sh\necho "TAMPERED" > /tmp/relaunched\n' > /w/web/Sonora-0.1.9-x86_64.AppImage
truncate -s $(stat -c %s /w/web/good.AppImage) /w/web/Sonora-0.1.9-x86_64.AppImage
rm -f /tmp/relaunched
(APPIMAGE_EXTRACT_AND_RUN=1 SONORA_UPDATE_AUTOTEST=1 timeout 60 /tmp/apptest/Sonora.AppImage >/w/tampered.log 2>&1 &)
sleep 40
grep "updates" /w/tampered.log || true
[ ! -f /tmp/relaunched ] && cmp -s /tmp/apptest/Sonora.AppImage /w/out/Sonora-0.1.6-x86_64.AppImage && echo "PASS tampered: refused, AppImage unchanged" || echo "FAIL tampered"
pkill -f Sonora.AppImage || true; pkill -f /tmp/appimage_extracted || true
mv /w/web/good.AppImage /w/web/Sonora-0.1.9-x86_64.AppImage

echo "== .deb 0.1.6 -> 0.1.9 (pkexec stand-in: the container already runs as root)"
DEBIAN_FRONTEND=noninteractive apt-get install -y -qq /w/out/sonora_0.1.6_amd64.deb >/dev/null 2>&1
printf '#!/bin/sh\nexec "$@"\n' > /usr/local/bin/pkexec && chmod +x /usr/local/bin/pkexec
dpkg-query -W sonora
(SONORA_UPDATE_AUTOTEST=1 timeout 300 /opt/sonora/Sonora >/w/deb.log 2>&1 &)
for i in $(seq 1 200); do [ "$(dpkg-query -W -f '${Version}' sonora)" = 0.1.9 ] && break; sleep 1; done
sleep 8
grep "updates" /w/deb.log || true
[ "$(dpkg-query -W -f '${Version}' sonora)" = 0.1.9 ] && echo "PASS deb: installed $(dpkg-query -W sonora)" || echo "FAIL deb: still $(dpkg-query -W sonora)"
pgrep -af "^/opt/sonora/Sonora" && echo "PASS deb: relaunched" || echo "FAIL deb: not relaunched"
