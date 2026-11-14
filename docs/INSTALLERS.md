# Installers

First-install packages for the three platforms (ROADMAP COM-4k). Updates after the first install are separate:
Sparkle on macOS, WinSparkle on Windows and Sonora's own AppImage/.deb updater on Linux ([UPDATES.md](UPDATES.md); a
tagged CI release builds and signs every update and writes the three feeds).

| OS | Package | What it installs | Made by |
|---|---|---|---|
| macOS arm64 | `Sonora-<v>.dmg` | `Sonora.app` next to an Applications link (drag to install) | `packaging/macos/make_dmg.sh`, called by `scripts/release/make_release.sh` |
| macOS arm64 | `Sonora-Prism-VST3-<v>.pkg` | `Sonora Prism.vst3` in `/Library/Audio/Plug-Ins/VST3` (all users) or `~/Library/Audio/Plug-Ins/VST3` ("Install for me only") | `packaging/macos/make_vst3_pkg.sh` (PLG-11c) |
| macOS arm64 | `Sonora-Curve-LFO-VST3-<v>.pkg` | `Sonora Curve LFO.vst3` (drawn-curve modulation effect), same folders | `packaging/macos/make_vst3_pkg.sh` (PLG-12b) |
| macOS arm64 | `Sonora-Hush-VST3-<v>.pkg` | `Sonora Hush.vst3` (resonance suppressor effect), same folders | `packaging/macos/make_vst3_pkg.sh` (PLG-14) |
| macOS arm64 | `Sonora-Kickstract-VST3-<v>.pkg` | `Sonora Kickstract.vst3` (kick extractor effect), same folders | `packaging/macos/make_vst3_pkg.sh` (PLG-17) |
| macOS arm64 | `Sonora-Stutter-VST3-<v>.pkg` | `Sonora Stutter.vst3` (vocal stutter / retrigger effect), same folders | `packaging/macos/make_vst3_pkg.sh` (PLG-18) |
| macOS arm64 | `Sonora-Phase-Align-VST3-<v>.pkg` | `Sonora Phase Align.vst3` (kick / bass phase aligner, sidechain input), same folders | `packaging/macos/make_vst3_pkg.sh` (PLG-21) |
| macOS arm64 | `Sonora-Sweep-VST3-<v>.pkg` | `Sonora Sweep.vst3` (riser / downlifter instrument), same folders | `packaging/macos/make_vst3_pkg.sh` (PLG-19) |
| macOS arm64 | `Sonora-Correlation-VST3-<v>.pkg` | `Sonora Correlation.vst3` (stereo correlation meter), same folders | `packaging/macos/make_vst3_pkg.sh` (PLG-24) |
| Windows x64 | `Sonora-<v>-windows-x64-setup.exe` | `Program Files\Sonora` (or per user), Start menu entry, `.als` "Open with" entry | `packaging/windows/sonora.iss` (Inno Setup 6) |
| Linux x64 | `Sonora-<v>-x86_64.AppImage` | nothing: one self-contained file (`chmod +x`, run) | `packaging/linux/make_packages.sh` |
| Linux x64 | `sonora_<v>_amd64.deb` | `/opt/sonora`, `/usr/bin/sonora`, desktop entry, icon, `.als` MIME type | `packaging/linux/make_packages.sh` |

Every package keeps the app's folder layout: the executable, `sonora_plugin_scanner` and `Resources/` side by side
(`PluginHost::scannerExecutable` and Help > User Guide look next to the executable). Each output folder also gets a
`.sha256` file.

## Release checks

These run on every release, and a release is not done until they pass.

- **One version.** `project(Sonora VERSION x.y.z)` in the top-level `CMakeLists.txt` is the version users see: About,
  the splash, `CFBundleShortVersionString`, the installers and every plug-in (Sonora Hush shows it under its wordmark).
  Bump it before tagging `vx.y.z`; the installers workflow refuses a tag that differs. Existing build folders pick a bump
  up at the next configure (`SONORA_APP_VERSION` is empty unless a release passes it).
- **Every plug-in ships and is on the website.** `scripts/release/check_release_ready.sh x.y.z` fails unless each
  `sonora_add_native_vst3(... PRODUCT_NAME "Sonora X")` in `src/CMakeLists.txt` is built and packaged by the workflow as
  `Sonora-X-VST3-<v>.pkg` plus the stable `Sonora-X-VST3-macOS.pkg`, and `site/download.html` has its card (`id="x"`),
  download link and screenshot `site/assets/img/plugin-x.webp`. CI runs it before building a tag. A new plug-in therefore
  needs all of those in the same change.
- **Versioned downloads.** The site's buttons and the public README link the release's versioned files
  (`releases/download/vX.Y.Z/Sonora-X.Y.Z.dmg` ...) and show its number: run `scripts/release/set_version_links.py` after
  the bump; `check_release_ready.sh` refuses a tag whose pages name another version. The release still attaches the
  version-free copies (`Sonora-macOS.dmg` ...) so older links keep working.
- **No dead download buttons.** The release job fails if any download link in `site/*.html` has no matching file in the
  release.
- **After publishing:** `scripts/release/check_downloads.sh --version x.y.z` checks the release is Latest, the live site
  links the same files as `site/`, and every link answers HTTP 200 with the asset's exact size.
- **First launch:** the builds are not signed by Apple or Microsoft yet. `site/download.html#first-launch` explains the
  Gatekeeper (macOS 15+: System Settings > Privacy & Security > Open Anyway) and SmartScreen steps; keep it until COM-4b
  and COM-4m ship signed builds, and test a download on a Mac with Gatekeeper on.

## Building them

**All three in CI** — `.github/workflows/installers.yml`:

- A pull request that changes `packaging/`, `ci/linux/`, `cmake/`, the top `CMakeLists.txt` or the workflow builds the
  Windows and Linux installers and uploads them as workflow artifacts (`installer-windows`, `installer-linux`).
- Pushing a `v*` tag (e.g. `v0.2.0`) builds all three, including an ad hoc signed macOS disk image, and attaches them
  to a **draft** GitHub Release. Review and publish it by hand.
- Actions > installers > Run workflow (once the file is on the default branch): optional version, optional macOS.

**macOS, signed, on a maintainer's Mac** — the release script builds, signs, notarizes, writes the Sparkle archive and
appcast, then the disk image into `dist/installers/` (kept apart from the Sparkle archives folder, which
`generate_appcast` scans):

```bash
scripts/release/make_release.sh --version 0.2.0 --feed-url ... --public-key ... --download-url-prefix ... \
    --sign-identity "Developer ID Application: NAME (TEAMID)" --notary-profile sonora-notary
```

A disk image for local tests from any signed app: `packaging/macos/make_dmg.sh Sonora.app out.dmg` (add
`--sign-identity`/`--notary-profile` to sign and notarize the image as well).

**Sonora Prism VST3 package (macOS)** — a native Installer package (welcome, license, destination, conclusion
pages) for the plug-in built by the `SonoraPrism_VST3` target (PLG-11). From a Release build:

```bash
cmake -S . -B build-release-ui -G Ninja -DCMAKE_BUILD_TYPE=Release -DSONORA_APP_VERSION=0.2.0
cmake --build build-release-ui --target SonoraPrism_VST3
packaging/macos/make_vst3_pkg.sh "build-release-ui/src/SonoraPrism_artefacts/Release/VST3/Sonora Prism.vst3" \
    dist/installers/Sonora-Prism-VST3-0.2.0.pkg \
    --sign-identity "Developer ID Application: NAME (TEAMID)" \
    --installer-identity "Developer ID Installer: NAME (TEAMID)" --notary-profile sonora-notary
```

Without the three options the bundle is signed ad hoc and the package is unsigned (local tests). The component is not
relocatable and not version checked, so the package always writes its bundle to the chosen VST3 folder. The plug-in
takes the app's version (`SONORA_APP_VERSION`); the package identifier is `org.sonora.wavetable.pkg`. To remove it:
delete `Sonora Prism.vst3` from the folder, then `pkgutil --forget org.sonora.wavetable.pkg` (with `sudo` for the
all-users install).

**Sonora Hush VST3 package (macOS)** — the same script on the `SonoraHush_VST3` target (PLG-14). It reads the bundle's
`moduleinfo.json`, so the pages say "effect" instead of "instrument":

```bash
cmake --build build-release-ui --target SonoraHush_VST3
packaging/macos/make_vst3_pkg.sh "build-release-ui/src/SonoraHush_artefacts/Release/VST3/Sonora Hush.vst3" \
    dist/installers/Sonora-Hush-VST3-0.2.0.pkg   # plus the three signing options for a release
```

The package identifier is `org.sonora.hush.pkg`. To remove it, delete `Sonora Hush.vst3` and run
`pkgutil --forget org.sonora.hush.pkg`.

**Linux, locally in Docker** (x64 runs emulated on Apple silicon, so a clean Release build is slow):

```bash
SONORA_APP_VERSION=0.2.0 ci/linux/run.sh package    # -> dist/linux/
```

`ci/linux/Dockerfile` pins linuxdeploy, appimagetool and the AppImage runtime by release and SHA-256. linuxdeploy
bundles the libraries the two executables need, except base-system ones (glibc, libGL, X11, fontconfig, ALSA). The
`.deb` gets its `Depends:` from `dpkg-shlibdeps`, recommends `fonts-dejavu-core` and suggests `jackd2`.

**Windows** — CI only for now (see [PLATFORMS.md](PLATFORMS.md)). On a Windows machine with Visual Studio 2022 and
Inno Setup 6: from an x64 developer prompt, `bash scripts/bootstrap.sh`, a Release build with
`-DSONORA_WITH_UI=ON`, then

```bat
iscc /DAppVersion=0.2.0 /DSourceDir=build\src\Sonora_artefacts\Release /DOutputDir=dist\windows packaging\windows\sonora.iss
```

Windows builds must be `Release` or `RelWithDebInfo`: skia-pack's libraries use the static release C runtime (`/MT`),
so every MSVC target does too (top `CMakeLists.txt`) and no Visual C++ redistributable is needed.

## File types

`.als` is also the reference app's format, so no installer takes the default association silently:

- Windows: Sonora is added to "Open with"; "Make Sonora the default app for .als sets" is an unchecked installer task.
- Linux `.deb`: registers `application/x-sonora-als` (`*.als`) and lists it in the desktop entry; the desktop picks
  the default.
- macOS: the app declares `.als` (`DOCUMENT_EXTENSIONS`); Launch Services keeps the user's current default.

## Signing status

| | Signed | Notes |
|---|---|---|
| macOS from `make_release.sh` | Developer ID + notarized + stapled when the identity and profile are given | COM-4b: needs the signing identity and notary profile |
| macOS from CI | ad hoc only | Gatekeeper blocks it on other Macs (right-click > Open); for testing |
| macOS VST3 `.pkg` | bundle: Developer ID + hardened runtime; package: Developer ID Installer + notarized, when given | needs a "Developer ID Installer" certificate as well as the Application one; CI's is unsigned |
| Windows | no | SmartScreen warns "unknown publisher" until an Authenticode certificate or signing service is set up (COM-4m) |
| Linux | no | AppImage/.deb signatures are optional; publish the `.sha256` next to them |

## First signed macOS release (COM-4b)

The scripts already sign inside-out with the hardened runtime and `scripts/release/Sonora.entitlements`, notarize,
staple and sign the image. Everything in the bundle (the app, `sonora_plugin_scanner`, Sparkle's framework, XPC services,
Autoupdate and Updater.app) is covered by `make_release.sh`. What a maintainer's Mac still needs (checked 2026-10-23:
`security find-identity -v -p codesigning` lists 0 identities, no `sonora-notary` profile, no Sparkle key):

1. **Apple Developer Program** membership (paid, yearly). Only the Account Holder can create Developer ID certificates.
   An individual account puts the person's legal name on the certificate and in Gatekeeper's dialog; an organization
   account needs a D-U-N-S number.
2. **Developer ID Application** certificate in the login keychain: Xcode > Settings > Accounts > Manage Certificates >
   "+" > Developer ID Application, or a CSR from Keychain Access uploaded at developer.apple.com. Back up the private
   key (export a `.p12`). For the Sonora Prism package also create a **Developer ID Installer** certificate.
   Check: `security find-identity -v -p codesigning` shows `Developer ID Application: NAME (TEAMID)`.
3. **Notary credentials** stored as the profile the scripts use. With an app-specific password (account.apple.com >
   Sign-In and Security) or an App Store Connect API key:
   `xcrun notarytool store-credentials sonora-notary --apple-id APPLE_ID --team-id TEAMID` (it prompts for the
   password). Check: `xcrun notarytool history --keychain-profile sonora-notary`. The Command Line Tools are enough
   (`notarytool` and `stapler` are there); full Xcode is not required.
4. **Sparkle EdDSA key** (updates): done 2026-11-04. The maintainer's login keychain holds it, with an offline backup and the
   CI secret `SPARKLE_ED_PRIVATE_KEY`. Its public key is in `packaging/macos/updates.env`. Losing the private key
   strands every installed copy.
5. **The appcast URL**: `https://sonoradaw.com/appcast.xml` (`site/appcast.xml`), fixed for the life of the app. See
   [UPDATES.md](UPDATES.md) "Hosting".
6. **Build and sign**, with a new version (the published 0.1.2 image is CI's ad hoc one):
   `scripts/release/make_release.sh --version 0.1.3 --feed-url URL --public-key KEY --download-url-prefix
   https://github.com/lordillusions/sonora/releases/download/v0.1.3/ --sign-identity "Developer ID Application: NAME
   (TEAMID)" --notary-profile sonora-notary`, then `make_vst3_pkg.sh` with both identities (above).
7. **Check before uploading**: `spctl -a -vvv dist/installers/Sonora-0.1.3.dmg -t open --context
   context:primary-signature` and, on the mounted app, `spctl -a -vvv Sonora.app` (expect `source=Notarized Developer
   ID`) and `xcrun stapler validate`. Then download the image through a browser on another Mac or user account (so it
   is quarantined) and open it with a double-click: no Gatekeeper warning.
8. **Publish**: pushing the `v0.1.3` tag makes CI draft the release with its ad hoc images, including the stable names
   the website links to. Replace both before publishing the draft:
   `gh release upload v0.1.3 Sonora-0.1.3.dmg Sonora-macOS.dmg --clobber` (the signed image under both names), and the
   signed `.pkg` the same way. Upload the Sparkle archive and the appcast as `make_release.sh` prints.

Later: sign and notarize in CI instead (the `.p12` and an App Store Connect API key as repository secrets, a temporary
keychain in the macOS job), so a tag alone produces the signed release.

## Verification

- macOS: `make_dmg.sh` on an ad hoc signed Debug app: image verifies (`hdiutil verify`), mounts with `Sonora.app` +
  `Applications`, and the app inside passes `codesign --verify --deep --strict`.
- macOS VST3 package: from a Release build, unsigned (2026-10-20): `pkgutil --expand-full` shows the distribution
  (arm64, macOS 13+, all-users and home destinations), `relocatable="false"`, install location
  `/Library/Audio/Plug-Ins/VST3`; the payload bundle passes `codesign --verify --strict`, exports only the three VST3
  entry points, and loads and plays in `plugin_probe --inspect-file`. Not yet run through Installer.app.
- Linux: the CI job installs the `.deb` on a clean `ubuntu:24.04` container and starts `sonora` under Xvfb for 20 s.
- Windows: the CI job runs `sonora_tests` on the Release UI build before packaging; the installer itself is only
  compiled, not run, in CI.
