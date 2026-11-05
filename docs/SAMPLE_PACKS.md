# Sample packs (LIB-1)

Optional, free sample packs a user can browse and download from inside Sonora: **Browser > Places > Packs**. Nothing is
bundled with the app and nothing is fetched until the user asks. The first "Get Packs..." click fetches the pack list
and counts as the opt-in: after that, opening Packs refreshes the list once per run. A download starts only from a pack's
Download button, its menu or a double-click.

## The licence rule

Every file in a pack must be one that **anyone may redistribute and use commercially with no attribution or share-alike
terms**. In practice this means **CC0 1.0**, the only licence `packs::allowedLicences()` accepts. The rule is enforced in
three places: `pack_tool build` refuses the source folder, the app skips the catalogue entry, and the installer refuses
the archive.

- Each pack ships its full licence text as `LICENSE.txt` at the top of the archive (≥ 100 bytes; `pack_tool` checks).
- `pack.json`'s `source` says where the files came from or how they were made (a commit, a URL per file, a recording session).
- **Never** put samples, presets or recordings from the reference app or any other commercial product into a pack (AGENTS.md
  golden rule 5). The same goes for "royalty-free" libraries whose terms forbid redistribution. These were checked on
  2026-11-04 and are **ruled out**:
  - 99Sounds: no redistribution, even for free.
  - Sonniss GDC bundles: no standalone redistribution, no AI training.
  - Philharmonia samples: not "as is".
  - University of Iowa MIS: redistribution unclear.
  - Salamander Grand Piano: CC-BY 3.0; a public-domain claim is unverified.
  - AVL drum kits: CC-BY-SA.
  - BBC Sound Effects: non-commercial.
- Candidates that pass the rule but are **not used yet** (owner's choice for the first catalogue: Sonora-rendered packs only).
  Check the licence file inside each download before using them:
  - Versilian VSCO 2 CE and VCSL (CC0).
  - Karoryfer's free drum kits (CC0 per sfzinstruments.github.io).
  - Sonic Pi's bundled samples (CC0, from Freesound, with per-file URLs).
  - Kenney audio packs (CC0).
  - Hand-picked CC0 sounds from Freesound (record each sound's URL in `source` or SOURCES.txt).

## The first catalogue: packs Sonora renders itself

`scripts/packs/render_sonora_packs.py` synthesises every sound from oscillators, noise and filters (Python standard
library only, fixed seeds, so it is reproducible). No recording, sample or preset is read. The owner releases the output
under CC0.

| Pack id | Style | Types | Files | Archive |
|---|---|---|---|---|
| `sonora-house-kit` | House | Drums, One-shots, Loops (124 BPM) | 29 | 5.6 MB |
| `sonora-techno-kit` | Techno | Drums, One-shots, Loops (130 BPM) | 24 | 6.1 MB |
| `sonora-hip-hop-kit` | Hip-Hop | Drums, One-shots, Loops (90 BPM, swung), Bass (tuned 808s) | 28 | 7.1 MB |
| `sonora-ambient-textures` | Ambient | Pads (chords), Textures, Drones | 14 | 35 MB |
| `sonora-cinematic-fx` | Cinematic | FX (impacts, risers, downlifters, whooshes) | 11 | 12 MB |
| `sonora-synth-multisamples` | Electronic | Instruments, Multisamples, Bass (sub bass C1–C4, pluck C2–C5) | 26 | 6.1 MB |

All files are 24-bit / 44.1 kHz WAV, normalised to −1 dBFS. A full render takes about 70 s.

## How to add or update a pack

1. **Make a source folder** per pack (`<sources>/<Pack Name>/`). It holds the audio files in subfolders, `LICENSE.txt`, an
   optional `README.txt` / `SOURCES.txt`, and `pack-source.json`:
   ```json
   {
     "id": "sonora-house-kit", "name": "Sonora House Kit", "version": "1.0",
     "description": "...", "author": "Sonora contributors", "licence": "CC0-1.0",
     "styles": ["House"], "types": ["Drums", "One-shots", "Loops"], "tags": ["House drums"],
     "source": "Synthesised by scripts/packs/render_sonora_packs.py at commit abc1234",
     "folderTags": { "Kicks": { "Sound": "Kick", "Kind": "One-shot" }, "Loops": { "Sound": "Drum loop", "Kind": "Loop", "BPM": "124" } },
     "fileTags": { "808s/Hip-Hop 808 C2.wav": { "Key": "C" } }
   }
   ```
   - `id` is lowercase letters, digits and `-`, and never changes between versions.
   - Raise `version` for any content change: installed copies then show **Update**.
   - Folder tags apply to everything below that folder, and the nearest folder wins. File tags override both.
   - The Browser adds `Style` and `Pack` to every file.
   - For Sonora's own packs: `python3 scripts/packs/render_sonora_packs.py <sources> [--version 1.1] [--only <id>] [--quick]`.
2. **Build and sign:**
   ```bash
   build/tools/pack_tool build <sources> <out> --base-url https://github.com/lordillusions/sonora/releases/download/packs/ --key <pack-signing.key>
   build/tools/pack_tool verify <out>/packs.json --pubkey <base64 public key>
   ```
   `build` checks the rules, then writes `<id>-<version>.zip` per pack, `packs.json` (sizes, SHA-256) and `packs.json.sig`.
   It refuses non-audio files other than the licence/readme, hidden or unsafe paths, a missing LICENSE.txt or `source`,
   and any licence but CC0.
3. **Test locally** before publishing. The first command serves the build folder; the second fetches, resumes, verifies and installs:
   ```bash
   python3 scripts/packs/serve_packs.py <out> --port 18765 --drop-after 1500000
   ```
   ```bash
   build/tools/pack_tool client --feed http://127.0.0.1:18765/packs.json --pubkey <key> --root /tmp/packs --download sonora-house-kit
   ```
   The real app takes the same feed through environment variables. `SONORA_PACKS_AUTOTEST` downloads one pack at launch
   and logs `[sonora] sample pack ...` lines:
   ```bash
   SONORA_DATA_FOLDER="Sonora Packs Test" SONORA_PACKS_FEED=http://127.0.0.1:18765/packs.json SONORA_PACKS_PUBLIC_KEY=<key> SONORA_PACKS_ROOT=/tmp/app-packs SONORA_PACKS_AUTOTEST=sonora-hip-hop-kit build/src/Sonora_artefacts/Debug/Sonora.app/Contents/MacOS/Sonora
   ```
   `ui_snapshot out.png --browser-packs <root>` renders the Packs list. Add `--browser-folder "<root>/<Pack>/Loops"` to see
   a pack's files and their tag facets.
4. **Publish (owner's go-ahead only):**
   - Upload the zips as assets of the `packs` release on `lordillusions/sonora`, created with `make_latest=false` so the
     app's `releases/latest/download/...` links keep pointing at the app.
   - Copy `packs.json` and `packs.json.sig` into the website repo next to the appcasts (`https://sonoradaw.com/packs.json`).
   - Always publish the zips first, then the catalogue.
   - An old zip may stay until no catalogue points to it.

## Signing key

The catalogue is signed with its **own** Ed25519 key, not the update key (owner's decision, 2026-11-04). A leaked pack
key cannot sign app updates, and packs can be signed on the owner's machine.

```bash
build/tools/pack_tool keygen ~/.sonora/pack-signing.key   # owner-only file; prints the public key; refuses to overwrite
build/tools/pack_tool pubkey ~/.sonora/pack-signing.key   # prints the public key again
```

The real key was made on 2026-11-05. The private key is `~/.sonora/pack-signing.key` on the owner's Mac (folder 0700,
file 0600, backed up offline by the owner). Its public key `oS+ffbaQwtk/fsetxAHVsfUnGWtm14rq/zdXPJu46lI=` is the default of CMake's
`SONORA_PACKS_PUBLIC_KEY` (src/CMakeLists.txt). A build folder configured before that keeps an empty cached value:
`cmake -U SONORA_PACKS_PUBLIC_KEY <build>`. A new key means changing that default in an app release. The feed is `SONORA_PACKS_FEED_URL`:
empty by default until the first catalogue is hosted (LIB-1b; 0.1.10 shipped with downloads off), then
`https://sonoradaw.com/packs.json`. While the feed or the key is empty, the Packs list says the packs come in a later
version (local tests set `SONORA_PACKS_FEED`). Keep the private key out of the repository and back it up: losing it means shipping a new public key in an app
release.

## How it works

- `src/packs/PackManifest` reads and writes `packs.json` and `pack.json`. It holds the licence rule, safe-path rules and
  archive URL resolution (a relative `url` resolves next to `packs.json`). `verifyCatalogue` checks the Ed25519 signature
  over the exact bytes with `core/Ed25519` (Monocypher), the same check the Linux updater uses.
- `platform::downloadToFile` (`platform/HttpClient.h`) is a resumable GET into a file:
  - When the file is partly there, it sends `Range: bytes=N-`. 206 appends, 200 rewrites, 416 leaves the file.
  - It follows redirects to https only (GitHub assets redirect to their storage host). Plain http is allowed only for loopback.
  - It reports progress and cancels within ~50 ms.
  - macOS uses NSURLSession; Windows and Linux use JUCE's WebInputStream.
- `src/packs/PackLibrary` manages the installed packs on disk. Default root: `~/Music/Sonora/Packs` (setting
  `samplePacksPath`; `SONORA_PACKS_ROOT` overrides it).
  - One folder per pack, named after the pack.
  - `.sonora/` holds the partial downloads (kept so the next try resumes), the staging area and the last verified catalogue.
  - Installing unpacks into staging, checks `pack.json` (same id/version, allowed licence) and `LICENSE.txt`, then moves the
    folder into place, replacing an older version.
  - Archives are refused if they hold links, `..` or absolute paths, more than 20000 entries, or unpack to more than twice
    the catalogue's size. Remove moves the folder to the Trash.
- `src/packs/PackStore` is a single worker that runs jobs in order:
  - fetch and verify the catalogue (on failure, the last verified copy stays listed);
  - download (three tries, each resuming);
  - check size and SHA-256 (on a mismatch the download is discarded);
  - install and remove.
  - Cancel pauses: the partial file stays, so Download again (or after a restart) resumes it.
- `src/app/AppSamplePacks.cpp` connects the store to `UiContext` (`samplePacks`, `samplePackFileTags`, the callbacks).
  `BrowserPanel` draws the list:
  - The status row: "Get Packs...", loading, or the error with retry.
  - The reference app's factory packs folder, when it exists.
  - One row per pack, installed first, with Style / Type / Pack tags for the facet filters.
  - Each row's button: Download / Resume / Cancel (with progress) / Update / Retry.
  - Double-clicking an installed pack opens it like a folder; Up returns to the list. Its files carry the pack's tags
    (Sound, Kind, BPM, Key, Style, Pack).

Tests: `sonora_tests packs` runs the manifest rules, signatures, resume against a loopback Range server, install/remove,
the store end to end, and the Browser list gestures.
