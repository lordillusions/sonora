# Platforms

Sonora's reference platform is **macOS on Apple silicon (arm64)**. Linux x64 and Windows x64 build from the same tree
(ROADMAP **COM-7**); they are qualification targets, not supported releases.

| | macOS arm64 | Linux x64 | Windows x64 |
|---|---|---|---|
| Headless build + `sonora_tests` | yes (local) | yes (Docker, CI) | CI only (GitHub Actions, MSVC) |
| Skia UI, app, `ui_snapshot` | Metal (GPU), raster fallback | raster (CPU) | not built yet |
| Compiler | Apple Clang | GCC 13 (matches skia-pack's gcc build) | MSVC 2022 |
| Skia prebuilt | `out/Release-macos-arm64` | `out/Release-linux-x64` | `out/Release-windows-x64` (fetched, unused) |

## Building

### macOS (unchanged)

See the [README](../README.md) "Build from source". `scripts/bootstrap.sh`, then CMake + Ninja into `build/`.

### Linux x64 (Docker on any host, including Apple silicon)

```bash
ci/linux/run.sh headless     # core/io/engine/devices/plugins/midi + sonora_tests
ci/linux/run.sh ui           # + Skia UI, Sonora app, ui_snapshot; tests run under Xvfb
ci/linux/run.sh all          # both
```

- `ci/linux/Dockerfile` is Ubuntu 24.04 with JUCE's Linux dependencies (ALSA, JACK headers, libcurl, FreeType,
  fontconfig, X11, Mesa GL headers), DejaVu fonts and Xvfb.
- `ci/linux/run.sh` builds the image (`--platform linux/amd64`) and runs `ci/linux/build.sh` with the repository
  mounted at `/src`. JUCE and the Linux Skia go to the Docker volume `sonora-linux-deps` (via
  `SONORA_THIRD_PARTY_DIR=/deps`), build trees to the volume `sonora-linux-build` (`/build/build-linux-{headless,ui}`),
  so the macOS `third_party/` and `build*/` are never touched. Remove the volumes with
  `docker volume rm sonora-linux-deps sonora-linux-build`.
- On Apple silicon the x64 container runs under emulation (Rosetta): clean builds take tens of minutes, incremental
  ones a few. `SONORA_JOBS` (default 6) bounds parallel compiles; 8 jobs fit in Docker Desktop's ~8 GB of RAM.
  gdb cannot attach under emulation; for a crash, preload a small SIGSEGV handler that calls `backtrace_symbols_fd`
  and resolve the offsets with `addr2line`.
- Expected container noise: `ALSA lib seq_hw.c ... /dev/snd/seq failed` and a JUCE assertion in `juce_Midi_linux.cpp`
  (no MIDI sequencer in the container).
- On a native Ubuntu 24.04 x64 machine, install the packages from the Dockerfile and run `ci/linux/build.sh all`.
- To inspect a build: `docker run --rm -it --platform linux/amd64 -v "$PWD":/src -v sonora-linux-deps:/deps
  -v sonora-linux-build:/build sonora-linux bash`.

### Windows x64

Built only by GitHub Actions for now (`.github/workflows/platforms.yml`, job `windows`): MSVC x64 developer prompt,
`SONORA_FETCH_SKIA=0 scripts/bootstrap.sh` in Git Bash (clones JUCE), then

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DSONORA_WITH_UI=OFF
cmake --build build
build/tests/sonora_tests.exe
```

Set `git config core.autocrlf false` before cloning: bash scripts and byte-exact fixtures need LF.

### CI

`.github/workflows/platforms.yml` runs on pushes to `main`, pull requests and manual dispatch:

- `linux`: builds `ci/linux/Dockerfile` natively on `ubuntu-24.04`, runs `ci/linux/build.sh all`, uploads build/test logs.
- `windows`: headless MSVC build and `sonora_tests`.

macOS is built and tested locally, not in CI; the macOS build is the reference and must stay warning-free.

## Platform layer

Code that differs per operating system lives behind small seams. The macOS implementation of each is the code that
existed before COM-7, moved without behaviour changes.

| Seam | macOS | Linux | Windows |
|---|---|---|---|
| `platform/CNumeric.h` — C-locale `strtod` (Serum JSON, SFZ) | `newlocale` + `strtod_l` | same | `_create_locale` + `_strtod_l` |
| `platform/Paths.h` — `appDataFolder()` | `~/Library/Application Support/Sonora` | `~/.config/Sonora` | `%APPDATA%\Sonora` |
| `platform/HttpClient.h` — no-redirect HTTPS POST (agent providers) | `HttpClientMac.mm` (NSURLSession, ephemeral) | `HttpClientJuce.cpp` (JUCE + libcurl) | `HttpClientJuce.cpp` (JUCE + WinINet) |
| `platform/SecretStore.h` — provider API keys | `SecretStoreMac.mm` (Keychain) | `SecretStoreFile.cpp` (`Secrets.json`, mode 0600) | `SecretStoreFile.cpp` (profile ACL only) |
| `gfx/MetalRenderer.h` — GPU canvas | `MetalLayerView.mm` | `MetalRendererNone.cpp` → raster | same |
| `gfx/Fonts.cpp` — Skia font manager | CoreText | fontconfig + FreeType | DirectWrite |
| `app/WindowChrome.h` — title bar blending | `WindowChrome.mm` | no-op | no-op |
| `app/AssistantDock.h` — Agent Control dock | `AssistantDock.mm` (AppKit) | `AssistantDockPortable.cpp` (JUCE widgets) | same |
| `control/LocalEndpoint` — MCP bridge socket | Unix socket, `getpeereid` | Unix socket, `SO_PEERCRED` | `LocalEndpointUnsupported.cpp` |
| `engine/StemSeparator` — external tool runner | fork/exec, process group | same | `juce::ChildProcess` |
| JUCE plugin formats | VST3 + AU | VST3 | VST3 |

Also per platform: on Windows/Linux Ctrl is JUCE's command modifier, so `SkiaCanvasComponent::keyPressed` reports it
as `cmd` only (Cmd+S bindings answer Ctrl+S). `#if` blocks stay inside these files; everything else is shared. Do not add `__APPLE__` checks elsewhere — add or
extend a seam.

### Native agent settings on Linux/Windows

The portable dock has no provider setup sheet yet. Configure the optional native LLM by hand in `appDataFolder()`:

- `Agent.json`: `{"provider": "openai", "baseUrl": "https://api.openai.com/v1", "model": "...", "protocol": "responses",
  "enabled": true}` (the same file macOS writes).
- `Secrets.json`: `{"org.sonora.daw.native-agent": {"openai:responses:https://api.openai.com/v1": "<api key>"}}`, readable only by
  you (`chmod 600` on Linux).

## Collab network (libdatachannel)

Collab sessions (COL-3, [COLLAB_DESIGN.md](COLLAB_DESIGN.md) section 8) use **libdatachannel** for WebRTC data channels
and the WebSocket signalling client/server. It is optional on every platform.

- `scripts/bootstrap.sh` clones the pinned tag (`LIBDATACHANNEL_TAG`, currently v0.24.6, with its submodules) into
  `third_party/libdatachannel`. `SONORA_FETCH_COLLAB_NET=0` skips it on Linux/Windows.
- `cmake/CollabNet.cmake` defines option `SONORA_WITH_COLLAB_NET`: ON by default when the checkout and OpenSSL 3 are
  present, otherwise OFF with a `Sonora: collab network OFF (...)` status line. An existing build directory keeps its cached
  value; pass `-DSONORA_WITH_COLLAB_NET=ON|OFF` to change it. ON builds only `datachannel-static` (no media/SRTP, no
  examples/tests, WebSocket on) and only when something links it; its warnings are silenced and its headers are SYSTEM.
- Code links `sonora_collab_net` (INTERFACE, always exists) and tests `#if SONORA_COLLAB_NET` (1 = libdatachannel linked, 0 = off).

| | macOS arm64 | Linux x64 | Windows x64 |
|---|---|---|---|
| OpenSSL 3 | built by `scripts/bootstrap.sh` from the pinned source tarball (`OPENSSL_VERSION`, 3.5 LTS, SHA-256 checked) into `third_party/openssl-macos-arm64`: static only, `-mmacosx-version-min=13.0`, `OPENSSLDIR=/etc/ssl` so default CA roots come from macOS's `/etc/ssl/cert.pem`. Override with `SONORA_OPENSSL_ROOT`. No dylib in the app. | distro `libssl-dev` (not in `ci/linux/Dockerfile` yet, so OFF there) | static `/MT` libraries (`lib/VC/x64/MT/libcrypto_static.lib`) of an OpenSSL 3 install; the runner's preinstalled `C:\Program Files\OpenSSL` has them. Only DLL import libraries found: OFF, because the installer ships no OpenSSL DLL (0.1.1 failed to start on users' machines asking for `libcrypto-3-x64.dll`). `installers.yml` fails if `Sonora.exe` imports an OpenSSL or VC++ runtime DLL. |

Homebrew's `openssl@3` is not used: its static libraries are built for the newest macOS only (`minos 26.0`), which
makes `ld` warn on every object at Sonora's 13.0 deployment target. To move to a new OpenSSL, bump `OPENSSL_VERSION`
and `OPENSSL_SHA256` in `scripts/bootstrap.sh` and re-run it (it rebuilds when `third_party/openssl-macos-arm64/VERSION` differs).

The app's Info.plist (`src/CMakeLists.txt`) carries `NSLocalNetworkUsageDescription` and `NSBonjourServices`
(`_sonora-collab._tcp`): macOS 15+ asks for Local Network access the first time collab traffic reaches the LAN
(Apple TN3179). Command-line tools such as `collab_probe` are allowed automatically.

Licences: libdatachannel and its bundled libjuice/usrsctp/plog are MPL-2.0/BSD/MIT, compatible with Sonora's AGPL;
OpenSSL 3 is Apache-2.0. Ship their licence texts with the app when collab networking is enabled.

## Not available per platform

| Feature | Linux | Windows |
|---|---|---|
| Audio files: M4A/AAC/ALAC, CAF, MP4, AIFC (CoreAudio containers) | no (WAV, AIFF, FLAC, Ogg, MP3 via JUCE) | same |
| GPU rendering (Skia Ganesh GL / D3D) | raster CPU only | UI not built |
| Audio Units | no (macOS format) | no |
| Keychain-grade secret storage | plain file (0600); libsecret is open | plain file; DPAPI is open |
| Native LLM provider setup UI, result-details window, dock snapshots for `control_probe` | no | no |
| Title bar blended into the canvas | no (native title bar) | no |
| MCP local endpoint | yes | no (named pipe is open) |
| Stem tool cancel also ends helper processes | yes | no (Job object is open) |
| Installers (COM-4k, [INSTALLERS.md](INSTALLERS.md)) | AppImage + `.deb` | Inno Setup `.exe` (CI) |
| Signing, updates (COM-4l/m) | no | no |
| Audio/MIDI hardware, plugin and path qualification | not done | not done |
| `control_probe` (agent/MCP acceptance tool) | not built | not built |
| UI typography | DejaVu/Noto/Liberation Sans instead of SF Pro: wider, so a few narrow labels elide (the key-signature `♭#` button shows `...`) | Segoe UI, untested |
| Paths in the UI/defaults that point at macOS locations (VST3 folders, Serum presets, Live themes, UVR) | still macOS paths | still macOS paths |

Linux raster rendering of the minimal fixture (`ui_snapshot`, Xvfb, DejaVu Sans):
[screenshots/linux-arrangement-raster.png](screenshots/linux-arrangement-raster.png).

Open work is tracked as COM-7 sub-items in [ROADMAP.md](ROADMAP.md).
