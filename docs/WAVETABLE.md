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
(194 files, 2048-sample frames, e.g. Retro Harmonics 1 = 92 frames). Sonora reads them **at run time** from any installed
`/Applications/Ableton Live*.app` (or `SONORA_WAVETABLE_SPRITES`); nothing is copied into the repository. User tables
load from the set's sample (cut to whole 2048-sample frames; a shorter file becomes one resampled frame). Without the
library, or when a user sample is missing, a Sonora-generated 16-frame sine/triangle/saw/square morph plays and the build
log says so.

## Engine (what is measured and what is not)

| Part | Status |
|---|---|
| Filters | Simpler's measured laws (docs/PARITY.md): Clean / OSR / MS2 / SMP damping, PRD ladder, Morph mix, drive saturation. Not yet measured on Wavetable itself. |
| Envelope curves | Simpler's slope law; the capture's Amp (attack slope 0.21, decay 0.5) and Env 2 midpoints match it within 0.01. |
| Loop modes | None / Trigger (release right after the decay) / Loop (restart the attack after the release while held): behaviour from the manual, timing not measured. |
| LFOs | Sine / Triangle / Saw / Square / Random, shaping as a phase power law, offset, attack fade-in, retrigger or free-running, Hz or synced (the 22-entry synced list is assumed: 8 bars .. 1/64). |
| Oscillator effects | FM (sine modulator at 2^(2 FX1), phase modulation up to 1.5 cycles), Classic (phase-distortion pulse width, hard sync to 8x), Modern (power-law warp, sine fold): Sonora approximations. |
| Unison | Classic / Phase Sync: +-50 ct spread; Shimmer / Noise: random drift; Position Spread: position +-50 %; Random Note: per-note random detune. Approximations. |
| Matrix scaling | Additive in each target's own travel (filter frequency ~10 octaves per unit, times ~14 octaves, pitch 48 st); Amp multiplies (unipolar sources 1 - a + a s). Global Mod Amount scales all. Time scales envelope times by 10^Time (assumed). |
| Level | Voice output x 0.5 x Volume: not calibrated against the reference. |

## Face

`WavetableFace` (a `ReferenceFace`, 197-px body of the capture, origin x 5 / y 320): Sub column, the oscillator display
(category / table choosers over the installed library, stacked-frame view with the playing position in amber, gain and
position bars, effect mode, FX values named by mode, Semi / Det), both filters (activators, glyph type choosers, 12 / 24,
circuits, response of both as routed with draggable handles, routing, Res / Drive / Morph / Frequency), the matrix
(Matrix / MIDI / MPE columns, rows for every used target, drag cells, add a row from the target menu, wheel scroll,
Global Time / Amount) and the global column. Title widgets (expand switch, Osc 1 / Osc 2 tabs with their activators,
Matrix / MIDI / MPE) are added to the device box. The expanded view (three envelopes in Time / Slope / Value modes and
two LFOs) opens to the right of the body, scaled to the body height, instead of above the chain.

## Parity (DEV-28b2, not measured yet)

Suite `tools/parity_probe/suites/wavetable_laws.txt` (20 law cases on the reference's default preset + 12 factory presets):

```
build/tools/parity_probe make   "/private/tmp/Sonora Parity/WavetableLaws" tools/parity_probe/suites/wavetable_laws.txt
build/tools/parity_probe render "/private/tmp/Sonora Parity/WavetableLaws"
# reference: open Parity.als in the reference app, export All Individual Tracks into WavetableLaws/live (docs/PARITY.md)
build/tools/parity_probe compare "/private/tmp/Sonora Parity/WavetableLaws" "/private/tmp/Sonora Parity/WavetableLaws/live"
```

On 2026-11-06 the set and Sonora's 32 stems were made (all finite; default patch peak -15.0 dBFS); the reference export
was not possible (access to the reference application was declined), so no difference is known yet. First things to
fit from the export: output level (Sonora's 0.5 voice scale), velocity -> Amp law, matrix scaling of filter frequency,
the oscillator effects and unison laws, the LFO synced-rate list and filter type 5. Factory presets with full-range
envelope -> resonant filter sweeps (Kitchen Sync +8.9 dBFS, Flux Capacitor +2.7 dBFS peaks) are the most likely to differ.
