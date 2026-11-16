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
| `platform/ReferenceLibrary.h` — the reference app's Core Library, User Library and pack folders (LIB-2 device presets, read only) | `/Applications/Ableton Live*.app/Contents/App-Resources/Core Library`, `~/Music/Ableton/...` | none (User Library / packs under `~/Music`) | `%ProgramData%\Ableton\Live*\Resources\Core Library`, `Documents\Ableton\...` |
| `platform/FolderList.h` — one folder's names and types without asking about each entry (COM-15 privacy prompts) | POSIX `readdir` | same | JUCE iterator |
| `platform/HttpClient.h` — no-redirect HTTPS POST (agent providers); resumable GET into a file (LIB-1 sample packs) | `HttpClientMac.mm` (NSURLSession, ephemeral) | `HttpClientJuce.cpp` (JUCE + libcurl) | `HttpClientJuce.cpp` (JUCE + WinINet) |
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

## VST2 hosting (FST, PLG-15)

`scripts/bootstrap.sh` clones FST (clean-room VST2 headers, GPL-3.0-or-later) at a pinned commit into
`<third_party>/fst` on every OS and applies `src/plugins/vst2/fst-host-layout.patch`. CMake (`cmake/SonoraJuce.cmake`) turns
`JUCE_PLUGINHOST_VST` on only when the patched header is there; otherwise it prints a status line and builds without VST2.
The native-device VST3 targets always build with it off (they host nothing). Default VST2 folders come from JUCE
(macOS `/Library/Audio/Plug-Ins/VST` and `~/Library/Audio/Plug-Ins/VST`; Windows/Linux their usual VST paths).

## Collab network (libdatachannel)

Collab sessions (COL-3, [COLLAB_DESIGN.md](COLLAB_DESIGN.md) section 8) use **libdatachannel** for WebRTC data channels
and the WebSocket signalling client/server. It is optional on every platform.

- `scripts/bootstrap.sh` clones the pinned tag (`LIBDATACHANNEL_TAG`, currently v0.24.6, with its submodules) into
  `third_party/libdatachannel`. `SONORA_FETCH_COLLAB_NET=0` skips it on Linux/Windows.
- `cmake/CollabNet.cmake` defines option `SONORA_WITH_COLLAB_NET`: ON by default when the checkout and OpenSSL 3 are
  present, otherwise OFF with a `Sonora: collab network OFF (...)` status line. An existing build directory keeps its cached
  value; pass `-DSONORA_WITH_COLLAB_NET=ON|OFF` to change it. ON builds only `datachannel-static` (media on since COL-7:
  libsrtp from its `deps/` on the same OpenSSL; no examples/tests, WebSocket on) and only when something links it; its
  warnings are silenced and its headers are SYSTEM. On macOS the link passes `-no_warn_duplicate_libraries` (libsrtp and
  libdatachannel both name the static libcrypto).
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

Licences: libdatachannel and its bundled libjuice/usrsctp/plog/libsrtp are MPL-2.0/BSD/MIT, compatible with Sonora's
AGPL; OpenSSL 3 is Apache-2.0. Ship their licence texts with the app when collab networking is enabled.

## Collab voice (libopus) and camera

Voice (COL-7, owner decision 2026-11-05: on every platform) needs **libopus** (BSD-3-Clause). `scripts/bootstrap.sh`
downloads the pinned release (`OPUS_VERSION`, 1.5.2, SHA-256 from xiph's SHA256SUMS) into `third_party/opus`;
`cmake/Opus.cmake` builds it static (`-w`, SYSTEM) and defines `sonora_opus` with `SONORA_HAVE_OPUS` 1 or 0. Without it
the app announces no voice feature and the panel says "This build has no voice". The microphone is an input of the app's
audio device (JUCE), so capture is the same on every OS; macOS asks for permission through AVFoundation
(`platform/MediaAccess*`), Windows/Linux need no prompt. Opus's configure prints `ERRORRuntime cpu capability detection
needed for MAY_HAVE_NEON` on macOS arm64: it is a status line (NEON is presumed), not an error. Ship Opus's `COPYING`.

The camera (COL-8) is macOS only: AVFoundation capture and VideoToolbox H.264. Windows and Linux show it disabled.

## Privacy prompts (COM-15)

macOS (TCC) asks the person before an app reads some folders or opens a protected device. User report (2026-11-17):
Sonora asked for Photos "and several others" at launch. Audit of every prompt the app could cause, and what it does now.
The rule: **opening and playing a set asks for nothing**. A folder is read only when the person chose it (Places, User
Library, a set they opened); a device or the network is asked for only by the feature that needs it, and Sonora's own
prompt (`app/AppPrivacy.cpp`, drawn by `PromptWindow`) says why first. Only **Continue** lets macOS ask.

| Resource (TCC service) | What triggered it before COM-15 | When it is requested now | Sonora's explanation (title; Continue / Not Now) |
|---|---|---|---|
| Documents, Desktop (`SystemPolicyDocumentsFolder`, `...DesktopFolder`) | **At every launch**: the Projects list walked all of `~/Music`, `~/Documents` and `~/Desktop` recursively (`findProjectsInBackground`). | Never by a scan. Only when the person opens a set or adds a Place there (the set's folder is then listed). Recent sets in those folders are listed without reading the folder. Hush reads `~/Documents/oeksound/soothe3/Presets` and `~/Documents/Sonora/Hush Presets` only when its preset menu opens, or when a saved (not factory) preset is Hush's default. | none (the person's own file choice) |
| Media & Apple Music (`MediaLibrary`) | At launch (Projects walk) and 20 s later (sound index, LIB-4c), on a browser search of `~/Music` and when the Samples category listed `~/Music`. Even listing `~/Music` with JUCE's iterator was enough: it asks Foundation (NSDirectoryEnumerator, `NSURLIsHiddenKey`) about each child, and asking about `~/Music/Music` made tccd log a `kTCCServiceMediaLibrary preflight=no` request (measured, see below). | Never. `core::walkFolder` does not enter `~/Music/Music`, `~/Music/iTunes`, `~/Movies/TV` or `*.musiclibrary`/`*.tvlibrary`, and lists folders with `platform::listFolder` (POSIX `readdir`, names and types only); the browser's folder listing uses it too. Opening `~/Music/Music` in the browser yourself still asks (your choice). | none |
| Photos (`Photos`) | Most likely the launch walks: they followed symbolic links (`RangedDirectoryIterator`'s default) under Music/Documents/Desktop, and a link to the home folder or to Pictures reaches `Photos Library.photoslibrary`. Not seen directly: the Mac's TCC log keeps about two days, after the prompt had been answered. No other code reads Pictures. | Never. Media library packages (`*.photoslibrary`, `*.photolibrary`, `*.aplibrary`, Photo Booth Library) are never entered, even in a chosen folder; links are followed only to ordinary folders outside the walk. | none |
| Downloads, iCloud Drive, other apps' data, removable and network volumes (`...DownloadsFolder`, `FileProviderDomain`, `SystemPolicyAppData`, `...RemovableVolumes`, `...NetworkVolumes`) | Reachable through followed links in the launch walks. | Only for a Place or set the person chose there. A walk never crosses into another guarded place (`~/Library`, `/Volumes/<volume>`, Desktop, Documents, Downloads, Pictures) than its own root's. | none |
| Microphone (`Microphone`) | (1) Once an input was used, its device was saved and **reopened at every launch**. (2) Arming, Record, input monitoring, the audio settings window and the collab mic opened the input straight away: JUCE's `RuntimePermissions::request` grants at once on macOS, so the system prompt came from CoreAudio with no explanation. (3) A set saved with armed tracks opened inputs while loading. | Launch opens the output only (`AudioEngine::initialiseAudio` strips the saved input and remembers it). Inputs open when the person arms a track, presses Record, changes a track's input/monitoring, picks an input in the audio settings window, or turns the collab mic on. A set opened with armed tracks opens them only if already allowed; otherwise the status bar says to arm or record. | **Use your audio inputs?** Recording, input monitoring and your voice in a shared session come in through your audio interface or microphone; playing a set never needs one. |
| Camera (`Camera`) | Collab camera button: system prompt at once (AVFoundation). | Same button, explanation first. | **Show your camera in this session?** It starts off in every session and stops when you turn it off or leave. |
| Local Network (macOS 15+, no TCC API) | Opening the Collaborate panel started a Bonjour browse (the nearby sessions list); sharing/joining. | The panel explains once per run (not again after Not Now); sharing, joining by link and joining a nearby session explain until Continue. The panel lists nearby sessions only after Continue. | **Find collaborators on your network?** Shared sessions connect straight to the other people's computers on the same network; Sonora uses it only for shared sessions. |
| Keychain item (not TCC: "Sonora wants to use your confidential information...") | With Splice on and signed in, the credit balance was fetched at launch, reading the stored sign-in. Ad hoc-signed updates change the app's signature, so macOS may ask to confirm the read. | At the first Splice search (or sign-in / turning Splice on). Launch only checks that an item exists (attributes, `interactionNotAllowed`), which never asks. The collab relay key and agent API keys are read only by their own dialogs/actions. | none |
| Contacts, Calendars, Reminders, Screen Recording, Input Monitoring, Accessibility, Automation, Bluetooth, Location | Never by Sonora's code (searched: no Contacts/EventKit/ScreenCaptureKit/CGEventTap/AX/Apple Events/CoreBluetooth/CoreLocation use). The TCC log shows only *preflight* checks made by system services for Sonora (AppKit's open panel and AutoFill helper, Spotlight), which never prompt. | Never. | none |

What a choice does:

- **Continue**: macOS shows its own prompt (microphone/camera through `AVCaptureDevice requestAccessForMediaType`; local
  network on the session's first LAN packet). macOS remembers its answer; Sonora does not explain again.
- **Not Now**: remembered in the settings (`privacyNotNowMicrophone`, `privacyNotNowCamera`, `privacyNotNowLocalNetwork`).
  Background requests (the audio settings window, the Collaborate panel) then stay quiet; the person's own action (arm,
  Record, mic/camera button, Share/Join) explains again. Continue clears it.
- **Denied in System Settings**: no prompt; the status bar or collab notice names the System Settings pane.
- Linux and Windows have no such prompts: `platform::mediaAccess` grants and `localNetworkPromptExists()` is false, so no
  explanation shows there.

Info.plist privacy keys (all still needed, texts updated): `NSMicrophoneUsageDescription`, `NSCameraUsageDescription`,
`NSLocalNetworkUsageDescription` + `NSBonjourServices` (`_sonora-collab._tcp`). No others. Hardened-runtime entitlements
(`scripts/release/Sonora.entitlements`, used only with a Developer ID): `device.audio-input` and, since COM-15,
`device.camera` (COL-8 video would get no camera without it).

Not under Sonora's control: a hosted plug-in runs inside Sonora, so a plug-in that reads `~/Documents` (many keep presets
there) or opens a web view makes macOS ask in Sonora's name; the out-of-process scanner (`sonora_plugin_scanner`) is part
of the bundle and counts as Sonora too. Releases are signed ad hoc, so macOS may forget earlier answers after an update
(its record is tied to the exact build); a stable Developer ID signature (COM-4) fixes that.

Verification: `sonora_tests privacy:` (scan pruning on a fake home with links into Documents, Pictures' Photos library,
another volume and back up the tree; the Projects roots; the explain/skip rules; the saved audio state without inputs);
`brand_render <dir>` writes `preview-prompt-privacy-{microphone,camera,local-network}.png`; the app prints
`[sonora] projects: scanning <folder>` per root and `[sonora] privacy: ...` per explanation.

Measured 2026-11-17 (Debug app, `SONORA_DATA_FOLDER=permtest`, 45 s, `log stream --predicate 'subsystem == "com.apple.TCC"'`,
requests whose attribution names the build's binary): with the walk still on JUCE's iterator, one
`kTCCServiceMediaLibrary preflight=no` request 0.2 s after start; with `listFolder`, none. What remains are *preflight*
checks that never prompt: Microphone from CoreAudio's own device setup (voice-isolation support, no input opened),
ListenEvent and ScreenCapture from WindowServer. To repeat: run the app as above with the log stream in parallel and
list `AUTHREQ_CTX` lines with `preflight=no` for the app's pid (`AUTHREQ_ATTRIBUTION` names it).

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
