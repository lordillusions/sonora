# Wavetable import (`InstrumentVector`, DEV-28b)

Sets that use the reference's Wavetable instrument now play it natively. The device keeps the catalog name "Wavetable"
(an import adapter, like Simpler, Reverb or EQ Eight); Sonora Prism (`SonoraWavetable`) and Sonora Vector stay separate.

Files: `src/devices/native/WavetableInstrument.*` (DSP), `src/devices/native/WavetableSprites.*` (tables),
`src/ui/views/WavetableFace.cpp` (face), `tests/test_wavetable_instrument.cpp` (`sonora_tests InstrumentVector:`),
fixture `tests/fixtures/wavetable_set.xml` (hand-written from the schema, Sonora-chosen values).

## Format (schema 12.0_12402, two owner sets, 271 factory presets)

- Automatable leaves (`<X><Manual Value/>`) are the `Voice_*` parameters plus `Volume`; the parameter ids are the tags.
- Plain leaves read into `Device::plain`: `Voice_Oscillator{1,2}_Effects_EffectMode` (0 None, 1 FM, 2 Classic, 3 Modern,
  confirmed by the presets' matrix row names), `Voice_Unison_Mode` (0 None, 1 Classic, 2 Shimmer, 3 Noise, 4 Phase Sync,
  5 Position Spread, 6 Random Note; the capture shows 4 = Phase Sync), `Voice_Unison_VoiceCount` (2-8),
  `Voice_Global_FilterRouting` (0 Serial, 1 Parallel, 2 Split), `MonoPoly` (0 Mono, 1 Poly), `PolyVoices`
  (index + 2 voices: 6 = 8 in the capture), `HiQ`, `UseRawUserWavetable1/2`, the envelope display modes.
- Value laws read from the capture: oscillator / sub gain and `Volume` are linear amplitudes (0.325 = -9.8 dB), sustain of
  the amplitude envelope is linear (0.501 = -6.0 dB), pan shows `x 50` ("27L" for -0.53), detune is in semitones
  (shown in cents), `Voice_Filter1_Drive` in dB, `Voice_SubOscillator_Transpose` 0 / 1 / 2 = octave 0 / -1 / -2,
  `CircuitLpHp` 0 Clean 1 OSR 2 MS2 3 SMP 4 PRD, `CircuitBpNoMo` 0 Clean 1 OSR, `Slope` 0 = 12 dB.
- `ModulationConnections` holds 52 `ModulationConnectionsForInstrumentVector` items, always in the same `TargetId` order
  (checked on all presets); `TargetName` follows the effect mode ("Osc 1 FM Pitch", "PW", "Warp"...). Each has
  `ModulationAmounts.0..12`: 0 Amp env, 1 Env 2, 2 Env 3, 3 LFO 1, 4 LFO 2, 5 Velocity, 6 Note, 7 Pitch Bend,
  8 Aftertouch / Pressure, 9 Mod Wheel, 10 Random, 11 MPE Note PB, 12 MPE Slide. The reader stores them as plain keys
  `ModulationConnections/ModulationConnectionsForInstrumentVector#<n>/ModulationAmounts.<m>`; the writer resolves `#n`
  as the n-th element with that tag. Pitch amounts are in units of 48 semitones (pitch bend 0.0417 = 2 st, MPE note bend 1.0).
- `SpriteName1/2` name the factory table without its category ("Harmonics 1" = Retro / Harmonics 1); every name used by the
  presets resolves to exactly one factory file. `UserSprite1/2` hold a `SampleRef` for user tables.
- Filter `Type` 5 appears in one preset (Silver Keys); its meaning is unknown, Sonora plays Morph and reports it.

## Tables

The reference's factory tables are plain 16-bit mono WAVs in the application bundle:
`/Applications/Ableton Live 12 Suite.app/Contents/App-Resources/Builtin/Samples/Vector/Sprites/Vector_Sprite_<Category> <Name>.wav`
(194 files). **Frames are 1024 samples** (DEV-28b2): every file is a whole multiple of 1024 samples, 60 are not of 2048,
the largest are 256 x 1024, and the reference plays "Basic Shapes" (4096 samples) as 4 frames sine / triangle / saw /
square (P01 a pure sine, P04 / P05 / P06 triangle / saw / square at positions 1/3, 2/3, 1). The first import cut them
into 2048-sample frames, which played every table an octave up with two frames mixed. Sonora reads them **at run time**
from any installed `/Applications/Ableton Live*.app` (or `SONORA_WAVETABLE_SPRITES`); nothing is copied into the
repository. User tables load from the set's sample, cut to whole 1024-sample frames (`kWavetableFrameLength`; assumed
to be the same as the factory files, not measured; a shorter file becomes one resampled frame). Without the library, or
when a user sample is missing, a Sonora-generated 16-frame sine/triangle/saw/square morph plays and the build log says so.

## Engine (what is measured and what is not)

Measured = fitted on the reference's own stems (DEV-28b2, suite `wavetable_laws.txt`, see "Parity"); the case that
pins each law is named. Everything else is a Sonora approximation.

| Part | Law | Status |
|---|---|---|
| Level | voice x 1/sqrt 2 x Volume: a full-scale table at Gain 1, Volume 1, Sustain 1 peaks at 0.70707 (P20); Volume, Sustain, Gain linear (0.3548 x 0.501 -> 0.1257, P01). Old guess voice x 0.5 was 3.0 dB low. | measured |
| Velocity | Amp <- Velocity 50 % (default patch) = 0.5 + 0.5 v / 127 (0.894 at 100, P02 / P12) | measured |
| Envelope stages | normalised exponential (1 - e^(-k x)) / (1 - e^(-k)), mirrored for negative slopes. Slope 0.5 -> k = 3.749 within 0.1 % over the whole decay / release (P11, P12); slope 0 linear (P10). k = 7.5 x slope between and beyond is assumed. Simpler's blend law (used before) covered 0.745 of a slope-0.5 stage at its midpoint, the reference 0.867. | 0 and 0.5 measured |
| Time (global) | does **not** scale the Amp envelope (Drifting Ambient Pad, Time 0.28: the 2.9 s release ends on time; Release Yourself, Time 0.47: the tail decays like the unscaled release). Env 2 / 3 and LFO rates keep 10^Time. | Amp measured; rest assumed |
| Sub | Tone 0: pure sine an octave down at the oscillators' level (P13, harmonics < -100 dB). Tone 1: a +-0.921 pulse high for 59.7 % of the cycle centred on the sine's positive peak (h5 / h10 / h15 39.1 dB down with the phases of a 0.597 pulse), plateaus drooping like a 5 Hz one-pole high-pass (P14). The sub starts 1.1 samples after the oscillators (all harmonics lag 1.17 deg x k). Tone in between crossfades (assumed). Old: tanh-shaped sine. | 0 and 1 measured |
| Clean resonance | d = 1 - r (P08: LP 1 kHz 24 dB r 0.5 = two RBJ stages Q 1.0 / 0.67, 0.085 dB rms over 24 harmonics). Simpler's (1 - r) / (1 + 3 r) gave Q 2.5 (+9.7 dB at 1 kHz). Second stage keeps (1 + d) / 2. r = 0 Clean 12 dB matches (P07). | r 0 / 0.5 measured |
| MS2 | Drive is a clean input gain (P09: Drive 12 dB = +11.8 dB, ramps keep 0.98 of their slope, no clipping) into a TPT state-variable filter whose integrators take V tanh(u / V), V = 4.72 (3.34 at the voice level), at 0.86 x the cutoff: waveform null -8.6 -> -25.4 dB. High harmonics (> 2 kHz) still differ by 2-5 dB; resonance + drive only seen in presets. | one case |
| OSR / SMP / PRD, drive | Simpler's laws on Wavetable's d (input soft clip for OSR / SMP, PRD ladder). | not measured on Wavetable |
| Filter routing | Parallel splits the mix equally (both filters at half level: Flux Capacitor +5.4 -> -0.6 dB; equals Serial when both are open). Split / Serial unchanged. | Parallel from one preset |
| FM | sine modulator at 2^(2 FX1) x pitch (FX1 0.5 = ratio 2, odd harmonics only), starting at phase 0, phase modulation index 10 x Amount rad (P16: beta 5.01 at 0.5; old 4.71). | one point |
| Classic | the table plays inside a window of w of each cycle from phase 0, S times (hard sync at the window start), silent for the rest: PW 0.5 / Sync 0.5 -> w 0.5186, S 2.510 (P17, h1..h30 within 0.7 dB / 7 deg). Laws through the point assumed: w = 1 - 0.963 |PW|, S = 1 + 3.02 Sync, negative PW = window at the cycle's end. Old: phase distortion around the half cycle + sync to 8x. | one point |
| Modern | Warp: Sonora's phase power law (not measured). Fold: **not applied** - with Warp 0 the reference's Fold adds no harmonics (Martian Ham Radio Fold 0.6 on Spectral 2: bands between the table's partials stay 30-40 dB down; Descenting Dreams Fold 0.42). With Warp 0.5 / Fold 0.5 (P18) the reference folds the table phase back and forth (the output is a sine read along a phase that turns around inside each half cycle): not modelled. | open |
| Unison | Classic 4 voices Amount 0.5: -25 / -8.3 / +8.3 / +25 ct (+-50 ct at 100 %, as assumed), voices panned hard left / right / left / right, each sqrt(2 / N) (0.707 at N = 4: equal power per channel); random start phases (0 / 25.6 / 95.7 / 207.8 deg on the probe, no pattern) (P15). Odd counts: middle voice centred at equal power (assumed). Shimmer / Noise / Position Spread / Random Note detune laws: Sonora approximations. | Classic measured |
| Glide | Mono legato only, linear in semitones over Glide (P19 matches); a note after a released one starts at its own pitch even while the release sounds (old: glided from the last note). | measured |
| LFO | Triangle in phase with the sine (0, +1, 0, -1; it used to fall first: Spacefolder -5.8 -> -1.2 dB). Synced rate list (22 entries, 8 bars .. 1/64): index 15 = 1/8 fits Kitchen Sync's 4.04 Hz random-step sidebands; index 21 (1/64 = 32 Hz) is **not** confirmed: Kitchen Sync's LFO 1 at index 21 -> Osc 2 Sync leaves no 32 Hz sidebands in the reference (Sonora: -37 dB); with a slow rate there Kitchen Sync would be +0.6 dB instead of +3.4 dB. Shaping, Random, Saw / Square polarity: assumed. | partly |
| Matrix scaling | filter frequency 10 octaves per unit (the knob's 20 Hz - 20.48 kHz travel; 5 octaves changed nothing measurable), times ~14 octaves, pitch 48 st; Amp multiplies (unipolar sources 1 - a + a s). Global Mod Amount scales all. | assumed |
| Phase | oscillators start at phase 0 (P01 / P20 onsets). Saw frames lead the reference by ~0.3 samples per harmonic (P05 null -22.7 dB), unexplained. | measured |

## Face

`WavetableFace` (a `ReferenceFace`, 197-px body of the capture, origin x 5 / y 320): Sub column, the oscillator display
(category / table choosers over the installed library, stacked-frame view with the playing position in amber, gain and
position bars, effect mode, FX values named by mode, Semi / Det), both filters (activators, glyph type choosers, 12 / 24,
circuits, response of both as routed with draggable handles, routing, Res / Drive / Morph / Frequency), the matrix
(Matrix / MIDI / MPE columns, rows for every used target, drag cells, add a row from the target menu, wheel scroll,
Global Time / Amount) and the global column. Title widgets (expand switch, Osc 1 / Osc 2 tabs with their activators,
Matrix / MIDI / MPE) are added to the device box. The expanded view (three envelopes in Time / Slope / Value modes and
two LFOs) opens to the right of the body, scaled to the body height, instead of above the chain.

## Parity (DEV-28b2, 2026-11-06)

Suite `tools/parity_probe/suites/wavetable_laws.txt` (20 law cases on the reference's default preset + 12 factory
presets). The reference export (44.1 kHz / 24-bit, All Individual Tracks) is in
`/private/tmp/Sonora Parity/WavetableLaws/live/` (`SonoraParityWT <track>.wav`); do not regenerate `Parity.als` (the
export cannot be redone without the owner). To iterate, copy the probe folder (Parity.als, cases.tsv, Samples), link
`live/` into it, then `parity_probe render <copy>` and `parity_probe compare <copy> <copy>/live`.

Result: **15 match + 1 close of 32** (was 0 / 32). Law cases: 15 match (P01-P08, P10-P13, P16, P19, P20), P14 close.
Still DIFF: P09 MS2 drive (null -25 dB, high harmonics 4.4 dB), P15 Classic unison (random phases: the beating cannot null;
levels / detunes / panning match), P17 Classic (level 0.04 dB, null -14 dB; the reference aliases, e.g. a -43 dB line at
139 Hz = 44100 - 168 x f0, which Sonora's band-limited window does not), P18 Modern (Warp / Fold unknown). All 12 factory
presets stay DIFF (random Shimmer / Noise unison never nulls; levels now within -4 .. +6 dB, see the table in HANDOFF).

Next (DEV-28b2 follow-ups): Modern Warp / Fold and the Shimmer / Noise / Position Spread unison laws need their own probe
cases (one variable each); Classic PW / Sync, FM Amount and slope laws need a second point each; MS2 with resonance and
no drive; the LFO synced-rate list (index 21); Drifting / Spacey Ambient Pad are ~5.6 dB loud (notch + Shimmer +
Env 3 -> Unison Amount: unresolved).
