# Bundled demo set (COM-14, proposal)

Goal: someone installs Sonora, opens the demo and hears a finished-sounding track within 30 seconds, made only with
Sonora's own devices. The same set feeds the launch video (COM-12), the website and the screenshots.

## Starting point

`scripts/site/make_demo_set.py` already writes "Sonora Demo" (124 BPM, 72 bars, 14 tracks in Drums/Music/FX groups,
two returns, six scenes, locators, a volume ride) from generated MIDI and synthesized WAVs, so it has no third-party
content. It is a picture, not a song: Perc, Bass, Chords, Lead and Pad have no instrument (only Arp has Sonora Prism),
the devices use default values, and the XML is only what `ui_snapshot` needs. It cannot be bundled as is.

## Proposal

- **Made in Sonora.** Use the generator only as the skeleton, then design the sounds and the mix by ear in the app
  and save with Sonora's own writer. That keeps the "made in Sonora" claim honest and tests the save path.
- **Instruments, all native:** Prism patches (saved as `.prism` too, so they become the first factory presets) on Bass,
  Chords, Arp, Lead and Pad; a Drum Rack of synthesized one-shots on Perc (shows the pad grid); the audio loops stay on
  Kick/Clap/Hats to show warping. Effects: EQ Eight, Compressor, Echo, Delay, Reverb, Curve LFO on the pad, Limiter on
  Main. At least one filter sweep in automation and one Session scene per section, so both views have something to play.
- **Length and size:** about 2 minutes of arrangement; samples 16-bit WAV, under 10 MB in total.
- **License:** CC0 for the set, its samples and the presets (a `LICENSE` next to them), so people can use it in videos
  and remixes. Nothing generated from other projects or sample packs.

## In the app

- Files under `Resources/Demo/Sonora Demo Project/` (set + `Samples/`), copied post-build like `supporters.txt`, and
  into the Windows and Linux packages (same `Resources/` layout).
- **Help > Open Demo Set**, and an "Open the demo" offer on first launch. The bundle is read-only and signed,
  so opening copies the project folder to `~/Music/Sonora/Sonora Demo Project` (or the platform equivalent) first and
  opens the copy; saving inside the app bundle would fail in `/Applications` and break the signature.
- Updates: the demo changes rarely; keep it out of delta patches when unchanged (UPDATES.md "separate optional content").

## Done when

- `als_dump` loads it with no warnings; `ui_snapshot` of Arrangement, Session and the Lead's device chain look right.
- `engine_probe --render` in Release: every track produces audio, no callback overrun on the oldest supported Mac.
- A clean install on each OS opens the demo from Help and from the first-launch offer, plays it, and saves the copy.
- The website timelapse and screenshots are regenerated from the bundled set.

## Decisions needed

Genre and tempo (the skeleton is 124 BPM melodic house), who designs the sounds and the mix, and the license (CC0
proposed).
