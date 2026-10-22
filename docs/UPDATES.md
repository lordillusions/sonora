# Application updates (COM-4)

Sonora updates itself on macOS with **Sparkle 2** (pinned in `scripts/bootstrap.sh`, MIT licence: add it to the release
notices, COM-2). Releases are signed zip archives on GitHub Releases. One appcast (`appcast.xml`) lists them, with
**delta updates** from recent versions. If a delta is missing, corrupt or does not apply, Sparkle downloads the full
archive instead.

## Code map

| File | Role |
|---|---|
| `src/app/UpdatePolicy.h` | Platform-neutral rules: channels, which channel tags a channel accepts, `installBlocker()` (recording > render > playback > unsaved changes). Unit-tested in `tests/test_updates.cpp`. |
| `src/app/Updater.h` | Small interface the app uses (`checkNow`, `setAutomaticChecks`, `channel`, `canInstallNow`, `installPending`). `Updater::Host` supplies session state, save-before-restart, status line and settings. |
| `src/app/UpdaterSparkle.mm` | macOS backend: `SPUStandardUpdaterController` + delegate (channels, postponed relaunch, quiet scheduled checks, `[sonora] updates:` log lines). |
| `src/app/UpdaterStub.cpp` | Inert backend for builds without Sparkle. COM-7 adds Velopack (Windows) and Flatpak (Linux) behind the same interface. |
| `src/app/MainWindow.cpp` | Owns the updater. "Check for Updates..." is in the Sonora menu and the Help menu. Wires Settings > Updates. |
| `src/app/AppController.cpp` | `prepareForRestart()`: Save / Don't Save / Cancel before an update restarts. The quit that follows does not ask again. |
| `src/ui/views/PreferencesPanel.cpp` | Settings > Updates: version, Check Automatically, Update Channel, Check Now / Install and Restart. `tests/test_updates_ui.cpp`. |
| `scripts/release/make_release.sh` | Builds, signs, (notarizes), zips, runs `generate_appcast`, prints what to upload. Publishes nothing. |
| `scripts/release/pin_urls.py` | Keeps the original download URL of files earlier releases published (see "Hosting"). |
| `scripts/release/Sonora.entitlements` | Hardened-runtime entitlements for a plug-in host (library validation off, JIT, microphone). |

## Build configuration

`src/CMakeLists.txt` writes these Info.plist keys. A plain developer build leaves the feed and the key empty, so updates
are **off**. It logs `[sonora] updates: off, Info.plist has no SUFeedURL/SUPublicEDKey`, and Settings explains why.

| CMake cache variable | Info.plist | Notes |
|---|---|---|
| `SONORA_APP_VERSION` / `SONORA_APP_BUILD` | `CFBundleShortVersionString` / `CFBundleVersion` | Sparkle compares `CFBundleVersion`. `getApplicationVersion()` follows. |
| `SONORA_UPDATE_FEED_URL` | `SUFeedURL` | One feed per app: stable+beta share one, nightly has its own. |
| `SONORA_UPDATE_PUBLIC_KEY` | `SUPublicEDKey` | Base64 EdDSA public key (`generate_keys -p`). |
| `SONORA_UPDATE_CHANNEL` | `SonoraUpdateChannel` | `stable` (default) or `nightly`. Nightly builds "Sonora Nightly.app", bundle id `org.sonora.daw.nightly`. |
| (always) | `SUEnableAutomaticChecks`, `SUScheduledCheckInterval` 86400, `SUAutomaticallyUpdate` false, `SUAllowsAutomaticUpdates` false, `SUVerifyUpdateBeforeExtraction` true, `LSMinimumSystemVersion` | No silent installs in releases. |
| `SONORA_UPDATE_ALLOW_LOCAL_HTTP` | `NSAllowsLocalNetworking`, `SUAllowsAutomaticUpdates` true | **Local tests only.** Also allows a second instance and enables the `SONORA_UPDATE_AUTOTEST` hook. |
| `SONORA_BUNDLE_ID` | `CFBundleIdentifier` | Local tests use `org.sonora.daw.updatetest` so they never touch the real app's defaults. |
| `SONORA_WITH_SPARKLE` | — | ON when `third_party/sparkle/Sparkle.framework` exists. The framework is copied into `Contents/Frameworks` with `ditto`, rpath `@executable_path/../Frameworks`. |

## Channels

- **Stable** (default) sees only untagged appcast items.
- **Beta** is chosen in Settings > Updates. It also sees items tagged `<sparkle:channel>beta</sparkle:channel>`
  (`make_release.sh --channel beta`). Stored as `updateChannel` in `Sonora.settings`.
- **Nightly** is a separate app (own bundle id, own feed, own instance lock) so it never overwrites stable.
  It also has its own data folder, `~/Library/Application Support/Sonora Nightly` (COM-4c): `Sonora Nightly.settings`,
  `PluginCache.xml`, `Mappings.json`, `MidiSettings.json`, `Agent.json`, `Untitled Projects/` and the automation registry
  `Automation/instances/`. A nightly never reads or writes `Application Support/Sonora`.
  - The folder name comes from `src/CMakeLists.txt` (`SONORA_APP_DATA_FOLDER_STRING` = the product name, set on
    `app/Main.cpp` only). `SonoraApplication`'s constructor passes it to `core::setAppDataFolderName`
    (`src/core/AppDataFolder.h`); every per-user path goes through `core::appDataDirectory()`.
  - Tools and tests keep the default `Sonora`, so they share the stable app's plug-in cache.
    `plugin_probe --data-folder "Sonora Nightly" --list` reads the nightly cache instead.
  - The MCP server (`packages/mcp-server`) lists instances from both registries.
  - Still shared with stable (not data folders): the agent's provider key in the keychain
    (service `org.sonora.daw.native-agent`) and the default save folder `~/Music/Sonora Projects` (user documents).
    The bundle id keeps `NSUserDefaults`, caches and Sparkle state apart.

## Safe restart

1. Checks and downloads run on Sparkle's queues and the message thread, never near the audio callback.
2. **Scheduled checks** show Sparkle's window only right after launch with an idle session (gentle reminders,
   `immediateFocus`). Any other time the status bar says "Sonora X is available: Help > Check for Updates...". A window
   that appears mid-session takes the next Space or Return meant for the transport.
3. When the user picks **Install and Relaunch**, Sparkle asks `shouldPostponeRelaunchForUpdate`:
   - If nothing is playing, recording or rendering and there are no unsaved changes, it restarts.
   - If something is playing, recording or rendering, the restart is postponed. The status bar says so, and **no dialog
     pops up when the session goes idle**: the user installs via Help > Check for Updates..., or Sparkle installs the
     update when Sonora quits normally (both verified below). Why there is no dialog: see "Not verified".
   - If there are unsaved changes, a Save / Don't Save / Cancel prompt appears. Save failure or Cancel keeps the app running
     with the update pending.

## Making a release

One-time prerequisites for the maintainer:

1. **EdDSA key**: run `third_party/sparkle/bin/generate_keys` once. It stores the private key in the login keychain and
   prints the public key for `--public-key`. Back up the private key with `generate_keys -x` to offline storage.
   Losing it means users cannot verify future updates.
2. **Developer ID Application** certificate in the keychain for `--sign-identity`.
3. A notarytool keychain profile for `--notary-profile`, created with `xcrun notarytool store-credentials`.
4. **Appcast host URL** for `--feed-url`. It must stay the same for the life of the app. See "Hosting".

```bash
scripts/release/make_release.sh --version 0.2.0 \
    --feed-url "$FEED_URL" --public-key "$SONORA_UPDATE_PUBLIC_KEY" \
    --download-url-prefix https://github.com/OWNER/REPO/releases/download/v0.2.0/ \
    --sign-identity "Developer ID Application: NAME (TEAMID)" --notary-profile sonora-notary \
    --notes release-notes/0.2.0.md
```

The archives folder (`dist/stable/` by default) must be kept between releases. It holds the previous zips that
`generate_appcast` diffs against (`--max-deltas`, default 3) and the current `appcast.xml`. Items it prunes move to
`old_updates/`, which is not uploaded. The script prints the files for the new GitHub Release and their `.sha256`.
`--phased-rollout SECONDS` gives a staged rollout (not yet exercised). Beta: add `--channel beta`.

### Hosting

Each GitHub Release has its own URL prefix, but `generate_appcast` rewrites every item it touches with the new prefix.
`pin_urls.py` restores the URL each file was first published under. Without it, a stable user would hit a 404 when the
newest item is beta. The e2e test checks this.

The appcast itself needs one fixed URL. `releases/latest/download/appcast.xml` only works if every stable release
re-attaches the appcast, and a beta (pre-release) is never "latest". Still to choose: GitHub Pages, a fixed `appcast`
release tag re-uploaded each time, or another static host.

## Local end-to-end test (no real keys, nothing published)

Verified 2026-10-19 with `build-updates-rel/` (Release). Every step runs on this Mac against `127.0.0.1`.

1. **Throwaway key, never the keychain.** CryptoKit writes a 32-byte seed. `generate_appcast --ed-key-file` accepts
   it as base64.
   ```swift
   import CryptoKit
   let key = Curve25519.Signing.PrivateKey()
   print(key.rawRepresentation.base64EncodedString())           // private key file
   print(key.publicKey.rawRepresentation.base64EncodedString()) // --public-key
   ```
2. **Releases**: run `make_release.sh --local-test --bundle-id org.sonora.daw.updatetest --feed-url
   http://127.0.0.1:8765/appcast.xml --download-url-prefix http://127.0.0.1:8765/vX.Y.Z/ --ed-key-file test.key
   --archives <scratch>/archives --build-dir build-updates-rel` for each version (signing is ad hoc by default).
3. **Serve** a web root where `appcast.xml` and `vX.Y.Z/` link to the archives folder:
   `python3 -I -m http.server 8765 --bind 127.0.0.1 --directory <web>`.
4. **Run the old version** unzipped from its release zip. `SONORA_UPDATE_AUTOTEST=1` (local-test builds only) makes
   a background check and installs at once. `=busy` plays the transport for 20 s first.
   Use `CFFIXED_USER_HOME=<scratch>/home` to isolate. JUCE uses `NSHomeDirectory()`, so `HOME=` does not work.
   - The relaunched new version runs without the override. Kill it, then trash the Untitled project it creates
     in the real `~/Library/Application Support/Sonora/Untitled Projects`.
   - Afterwards, remove `org.sonora.daw.updatetest` from `~/Library/{Preferences,Caches,HTTPStorages}`.
   - Before each run, also delete the isolated profile's Sparkle state. Otherwise a reinstalled old build resumes the
     previous run's download instead of checking.
   - Kill every instance of the bundle path before the next run. Sparkle's installer waits for all of them to exit.
5. Check the `[sonora] updates:` lines, the installed `Info.plist` version and `codesign --verify --deep --strict`.

| Case | Result |
|---|---|
| 0.1.1 → 0.1.2, delta available | Delta 4.9 KB (full zip 13.7 MB) downloaded, extracted, installed, relaunched; signature valid. |
| Corrupt delta (random bytes) | Delta rejected, **full** 0.1.2 zip downloaded and installed. |
| Delta missing (404) | **Full** zip downloaded and installed. |
| Tampered full zip, no delta | "The update is improperly signed", aborted, still 0.1.1. |
| Beta 0.1.3 published, app 0.1.2 on Stable | "You're up to date". |
| Same app with `updateChannel=beta` | 0.1.3 installed via the 0.1.2 → 0.1.3 delta (248 KB). |
| App 0.1.1 on Stable after the beta | 0.1.2 installed via its delta at its original `v0.1.2/` URL (pin_urls). |
| 0.1.8 → 0.1.9 while playing (`busy`) | Postponed while playing and for 30 s after it stopped; no restart, no dialog. After a normal quit (Apple Event) Sparkle installed 0.1.9 without relaunching. |
| 0.1.8 → 0.1.9 final code | Delta, and full after a 404, both installed. |

## Not verified / next steps

- **Clicking through Sparkle's own windows** (Install Update / Install and Relaunch / Skip) and the save prompt.
  The autotest hook bypasses the windows; nobody clicked them.
- An earlier design asked "Restart Now / Later" when playback stopped. In two runs that dialog was answered
  "Restart Now" about 10 s after it appeared, with nobody intending to. Probably a keystroke meant for another window;
  the cause was not identified. That design was removed (Safe restart, point 3).
- Developer ID signing, notarization and Gatekeeper on a clean Mac: needs a Developer ID identity. Ad hoc builds are
  blocked by Gatekeeper on other Macs.
- Signed feeds (Sparkle can sign the appcast itself), interrupted download, full disk, read-only install location,
  installing into /Applications as a standard user, staged rollout and "pause a bad release": COM-4b.
- Recovery installer and rollback: COM-4b.
