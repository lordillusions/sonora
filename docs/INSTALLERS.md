# Installers

First-install packages for the three platforms (ROADMAP COM-4k). Updates after the first install are separate:
Sparkle on macOS ([UPDATES.md](UPDATES.md)); Windows and Linux have no updater yet (COM-4l).

| OS | Package | What it installs | Made by |
|---|---|---|---|
| macOS arm64 | `Sonora-<v>.dmg` | `Sonora.app` next to an Applications link (drag to install) | `packaging/macos/make_dmg.sh`, called by `scripts/release/make_release.sh` |
| macOS arm64 | `Sonora-Prism-VST3-<v>.pkg` | `Sonora Prism.vst3` in `/Library/Audio/Plug-Ins/VST3` (all users) or `~/Library/Audio/Plug-Ins/VST3` ("Install for me only") | `packaging/macos/make_vst3_pkg.sh` (PLG-11c) |
| Windows x64 | `Sonora-<v>-windows-x64-setup.exe` | `Program Files\Sonora` (or per user), Start menu entry, `.als` "Open with" entry | `packaging/windows/sonora.iss` (Inno Setup 6) |
| Linux x64 | `Sonora-<v>-x86_64.AppImage` | nothing: one self-contained file (`chmod +x`, run) | `packaging/linux/make_packages.sh` |
| Linux x64 | `sonora_<v>_amd64.deb` | `/opt/sonora`, `/usr/bin/sonora`, desktop entry, icon, `.als` MIME type | `packaging/linux/make_packages.sh` |

Every package keeps the app's folder layout: the executable, `sonora_plugin_scanner` and `Resources/` side by side
(`PluginHost::scannerExecutable` and Help > User Guide look next to the executable). Each output folder also gets a
`.sha256` file.

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
4. **Sparkle EdDSA key** (updates): `third_party/sparkle/bin/generate_keys` once, then `generate_keys -x FILE` to an
   offline backup. Its public key goes to `--public-key`. Losing the private key strands every installed copy.
5. **The appcast URL**, fixed for the life of the app (for example `https://sonoradaw.com/appcast.xml`, served by the
   site). See [UPDATES.md](UPDATES.md) "Hosting".
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
