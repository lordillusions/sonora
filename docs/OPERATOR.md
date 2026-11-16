# Operator import (`Operator`, DEV-28a)

Sets that use the reference's Operator now play it natively instead of the unsupported-instrument fallback. The device
keeps the catalog name "Operator" (an import adapter, like Wavetable or Tension); Sonora FM (`SonoraFm`) stays separate.

Files: `src/devices/native/Operator.{h,cpp}` (DSP), `src/devices/native/OperatorParams.cpp` (parameter table, value
names, shared laws), `tests/test_operator.cpp` (`sonora_tests Operator`), parity suite
`tools/parity_probe/suites/operator_laws.txt`, face `src/ui/views/OperatorFace.cpp`. Reference capture: `docs/screenshots/reference/live-operator-osc-a.png` (default preset, oscillator A).

## Format (schema 12.0_12402, the default preset and the 332 factory presets)

- Automatable leaves (`<X><Manual Value/>`) are read by their paths: `Operator.<0..3>/...` (A..D), `Globals/...`,
  `Lfo/...`, `PitchEnv/...`, `EnvScale/...`, `Filter/...`, `Shaper/...`. Plain leaves (no Manual) go into
  `Device::plain` (`collectPlainLeaves`): `Globals/NumVoices` (Simpler's voice table, index 5 = 6 voices),
  `RetriggerMode`, `Interpolation`, `AntiAlias`, every oscillator's `UserHarmonics` (64 amplitudes, `NumHarmonics` 0/1/2 =
  16/32/64, `ExpansionMode`, `Normalize`) and the MIDI routing `MidiCtrl/{KeyDst,VelDst,MidiCtrl.0..2}/ModConnections.{0,1}/
  {Amount,Connection}`. The writer puts every value back at the same place.
- `WaveForm` 0..22 = Sin, Sin 4 Bit, Sin 8 Bit, Saw 3/4/6/8/16/32/64, Saw D, Square 3/4/6/8/16/32/64, Square D, Triangle,
  Noise Looped, Noise White, User (199 of the presets' user-harmonic oscillators sit at 22; the label set and order are the
  reference binary's string table).
- `Globals/Algorithm` 0..10 (11 algorithms). `Filter/Type` 0 LP, 1 HP, 2 BP, 3 Notch, 4 Morph (302 presets use Morph,
  with Morph 0 / 0.25 / 0.5 / 0.75); circuits as Simpler. `Filter/LegacyType` (Low 12dB ... Notch 24dB, 14 entries) only
  matters in `LegacyMode`, false everywhere.
- Levels are linear amplitudes with a -70 dB floor (0.000316 = "-inf dB"); envelope times in ms (attack 0.1..20000,
  decay / release 1..60000); the oscillator envelopes keep slopes 0 / 1 / 1 in every preset (no slope handles); the filter
  and pitch envelopes have free slopes. Pitch envelope levels are semitones (-48..48) scaled by `EnvelopeAmount` (-1..1).
- `Tune/FixedFrequency` 10..2000 with `FixedFrequencyMulti` 0..5 (decades, index 4 = x1: inferred from presets, see below).
- `Lfo/LfoRange` 0 Low, 1 High, 2 Sync; `LfoRate` 0..127; `LfoRateSynced` 0..14.

## Engine (what is measured and what is not)

Measured = fitted on the reference's own renders (Live 12.4.6, exported 2026-11-14 at 44.1 kHz / 32-bit) of
`operator_laws.txt` (123 laws + 331 presets, `/private/tmp/Sonora Parity/OperatorLaws/live`) and `operator_laws2.txt`
(66 cases, `.../OperatorLaws2/live`); keep those stems, a re-export needs Live. The case that pins each law is named.

| Part | Law | Status |
|---|---|---|
| Algorithms | 1 D>C>B>A, 2 (C+D)>B>A, 3 C>B, (B+D)>A, 4 D>B, D>C, (B+C)>A, 5 D>C, C>A, C>B (A, B out), 6 D>C>B, A (A, B out), 7 (B+C+D)>A, 8 B>A, D>C (A, C out), 9 D>A, B, C (A, B, C out), 10 D>C (A, B, C out), 11 additive | measured (`o-alg1..11` sidebands) |
| FM | phase modulation by the modulators' previous sample with a negative sign; a modulator output of 1 shifts its target by 2 cycles (4 pi) | measured (`o-fm-*`: 3.97..4.06 pi; 2:1 cancels J0 - J1; 1:1 DC offset) |
| Feedback | only on oscillators nothing modulates (alg 1 A: no effect); depth 0.25 cycle at 100 % | rule measured, depth assumed |
| Waves | Saw N = N partials, Square N = N odd partials at the ideal Fourier amplitudes (2/pi, 4/pi, 8/pi^2 fundamentals); 4 / 8-bit sine = trunc(sin x 8 / 128); band-limited to the last partial under Nyquist | measured (`o-wave1..19`) |
| Velocity | level x (1 - s + 2 s v)^2 (velocity 64 neutral; Vel 100 % = -inf..+12 dB) | measured (`o-vel*`, 0.1 dB) |
| Tuning | Coarse 0 = 0.5; Fine adds thousandths of the ratio; Fixed = Freq x 10^(Multi - 4); Transpose | measured (exact) |
| Envelopes | attack linear; decay / release dB-linear (Simpler's law); Time 2^(2.95 t); Time<Key 0.55 oct / oct around MIDI 70; Time<Vel 2^(-s (vel - 70) / 16) | measured (`o-env-*`, `o-time*`, `o-timekey`, `o-env-timevel`) |
| Filter | Simpler's stages and circuits; Freq<Key pivots on MIDI 72 (C4); Envelope 100 % = 108 st; Freq<Vel = 52 st x (vel/127)^3; Morph | measured (`o-filter-*`, `of-env*`, `of-morph*`) |
| Shaper | inside the filter section (no effect with the filter off); Soft = clip(1.054 g x) / (1.054 sqrt g), Hard = cubic(1.53 g x) / (1.53 g^0.12), Sine = sin(g x pi/2) / sqrt g; 4 Bit approximate | measured (`os-*`), 4 Bit not fitted |
| LFO | Low 0.01945 e^(0.05803 Rate) Hz, High 8 e^(0.05803 Rate) Hz; Dest A depth +-34.6 st x Amount^2; triangle starts at -1; synced = RepeatTime's note values (index 3 = 1/16) | measured (`o-lfo-*`) |
| Tone | no effect on the additive waves (an output filter was wrong) | measured; its effect on FM aliasing is not modelled |
| Routing (MIDI Key / Vel / controllers, LFO and pitch-envelope Dest B) | 1-4 Volume A-D, 5 / 6 Crossfade A/C, B/D, 8 oscillator pitch +-64 st, 10 Filter Freq +-64 st, 20 Volume, 21 Pan; pitch-envelope source +-96 st | measured (`od-vel*`, `od-penv*`); 12 / 13 / 15 / 16 / 17 assumed; 7, 9, 11, 14, 18, 19, 22, 23 inert |
| Spread / glide / mono | +-20 cents, glide time linear, mono legato | not fitted (`o-spread` matches level; `o-glide` / `o-mono` differ in envelope detail) |

Results (2026-11-14): law cases 77 match + 15 close of 123 (median level error 0.01 dB) and 22 + 24 of 66; factory presets
median level error 0.37 dB, 272 / 331 within 3 dB, 224 within 1 dB (waveform nulls differ: FM phases, noise, random LFOs).
Known gaps: self-oscillating filters from silence (Dub Filter FX plays nothing), 4 Bit shaper, Tone, MS2 resonance level,
Dest B targets 7 / 9 / 11 / 14 / 18 / 19 / 22 / 23, the MIDI controller order (assumed Aftertouch, Pitch Bend, Mod Wheel).

Live probes: presets saved from inside racks carry rack macro mappings (`<KeyMidi>` channel 16) that crash Live on a plain
track; strip them from a probe set (Sonora's writer drops them since DEV-15c). Export at most ~115 tracks at a
time (454 at once crashed Live's file thread) and keep 1 GB free per chunk.

## Parity

```
build/tools/parity_probe make "/private/tmp/Sonora Parity/OperatorLaws" tools/parity_probe/suites/operator_laws.txt
#   123 law cases + 331 factory presets (454 tracks)
#   reference: export All Individual Tracks into <dir>/live (docs/PARITY.md)
build/tools/parity_probe render "/private/tmp/Sonora Parity/OperatorLaws"
build/tools/parity_probe compare "/private/tmp/Sonora Parity/OperatorLaws" "/private/tmp/Sonora Parity/OperatorLaws/live"
```

The algorithm cases (`o-alg1..11`) run all four oscillators at -12 dB with ratios A 8, B 11, C 13, D 17 on C3: a carrier
shows its own partial, each direct modulator first-order sidebands at the sum / difference, so each structure can be read
off the reference's spectrum.
