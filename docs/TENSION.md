# Tension import (`StringStudio`, DEV-28j)

Sets and presets that use the reference's Tension instrument now play it natively. The device keeps the catalog name
"Tension" (an import adapter like Wavetable). Files: `src/devices/native/Tension.{h,cpp}` (DSP and value laws),
`src/ui/views/TensionFace.cpp` (face), `tests/test_tension.cpp` (`sonora_tests Tension`), parity suite
`tools/parity_probe/suites/tension_laws.txt`. The reference's factory presets (107, `Core Library/Devices/Instruments/
Tension`) open from the browser (LIB-2) and are read at run time; nothing from the reference is copied.

## Format (schema 12.0_12402)

Every parameter is a `<Tag><Manual Value/>` leaf stored **normalised** (0..1, -1..1 for the Key / Vel / Env / LFO
modulation boxes, enum indices); Sonora keeps them as stored, ids = tags. `CurrentTab` (0 String, 1 Filter/Global) is a
plain leaf. Enums read from captures of the reference's face (24 captures, every parameter at k / 11):

- `ExcitatorType` 0 Bow, 1 Hammer, 2 Hammer (bouncing), 3 Plectrum. `ExcitatorParameterX` is Force / Mass / Protrusion,
  `ExcitatorStiffness` Friction (Bow) / Stiffness. `TerminationFingerForce` is shown as "Finger Mass".
- `BodyType` Piano, Guitar, Violin, Generic; `BodySize` XS, S, M, L, XL. `BodyLevel` is the Volume knob, `BodyMix` Str/Body.
- `FilterType` LP12, LP24, BP6, BP12, N2P, N4P, HP12, HP24, F6, F12. `LFOWaveShape` sine, triangle, rectangle, random
  (steps), random (smooth). `LFOSync` 24 entries, triples dotted / plain / triplet; 18-23 read as 1/16d .. 1/32t, 0-17
  assumed to continue the pattern (4 bars .. 1/8).
- `Polyphony` Mono, 2, 4, 8, 12, 16, 20, 24, 28, 32. `KeyboardPriority` High, Low, Last. `KeyboardUnison` 2 / 4 voices.
- Pressure / Slide targets: None, Voice Volume, Vibrato Amount, Vibrato Speed, Unison Detune, String Inharmon., LFO
  Rate, F. Cutoff, F. Cut. LFO Depth, F. Cut. Env Depth, F. Q, F. Q LFO Depth, F. Q Env Depth.

## Value laws of the face (stored v -> shown)

| Control | Law |
|---|---|
| Filter Freq | 30 Hz x (22000 / 30)^v |
| Filter envelope A / D / R | 5 ms .. 15 s, log-interpolated through the captured points (0.5 = 283 ms) |
| Vibrato Delay / Attack | 0 at 0, else 50.9 ms x 100^v (0.5 = 509 ms); Rate 0.2 + 9.8 v Hz |
| F. LFO Delay / Attack | 0 at 0, else 200 ms x 25^v; Rate 0.1 + 6.4 v^3 Hz (only up to v = 0.5 seen) |
| Unison Delay | 0 at 0, else 1.96 ms x 51^v (0.5 = 14 ms) |
| Exciter / Damper / String / Termination Key and Vel boxes | sign(v) 0.101 x 158.5^abs(v) (1 = 16.00, 0 = 0.00) |
| Filter Freq < Env | 16 sign(v) v^4; the other filter boxes 4 v abs(v); envelope Vel boxes 8 v |
| Positions | v x 50 %; Stretch and Str/Body (2 v - 1) x 100 %; Detune v - 0.5 |
| Volume | linear (v / 0.705)^2 (the default 0.705 = 0 dB, 1 = +6 dB) |

## Engine

A bidirectional waveguide per string (velocity waves, integer L side + Lagrange-read fractional R side), the bridge
reflection through a one-pole loss filter and one dispersion allpass, the nut through the termination. Measured on the
reference's stems (suite `tension_laws.txt`, 86 law cases + all 107 factory presets, exported 2026-11-07 at 44.1 kHz /
32-bit; stems in `/private/tmp/Sonora Parity/TensionLaws/live/`, keep them: a re-export needs the owner):

| Part | Law | Status |
|---|---|---|
| String decay | partial k decays at a + b k^2 dB/s; a from Decay only: ln a = 7.25 - 6.8 Decay (1.6 dB/s at 100 %, 48 at 50 %, ~1400 at 0), independent of pitch | measured |
| Damping | b from the loss filter's pole: 0.41 at 50 % (1.9 dB/s per k^2 at C3), 0 at 100 %, 0.975 at 0 % (a few periods) | 3 points |
| Ratio | after the key, the loop uses Decay x (1 - Ratio) | measured (0 / 0.5 / 1) |
| Key boxes | parameter + keyAmount / 32 per octave from C3 (Damping < Key 16 = 0.5 per octave) | Damping / Decay fitted |
| Inharmonicity | one first-order allpass; partial 4 sharpened by 0.036 Inharm^2 (0.88 % at 50 %, 3.6 % at 100 %), x 0.83 .. 1.89 from C1 to C5; reproduces k = 2..10 within 0.2 % | measured |
| Positions | fraction 0..0.5 of the string from the bridge's effective end (half the bridge filters' delay is subtracted): 25 % nulls the 4th harmonic | measured |
| Plectrum | force ramp for the push time, then slips off with tau; levels Protrusion -10.8 / +11.9, Velocity -8.9 / +20.7, Damping -11 / +25.3 dB at 0 / 100 %; tau 0.26 ms at Stiffness 50 % | levels measured, mechanism Sonora's |
| Hammer | mass / felt (power 2.3) contact, force bounded per sample; bouncing hammer pushed back while held | approximate |
| Bow | friction curve min(1, (abs(dv slope) + 0.75)^-e) normalised to the bow speed; Force 0 silent | level, decay and pitch (+15 cents) close; Velocity law not |
| Damper | dashpot contact, R <= 1, landing time 1..30 ms, gated = falls at note-off | approximate |
| Termination | lossy finger at the nut, not pitch-compensated (the reference is -12 cents) | approximate |
| Pickup | velocity at its position instead of the bridge force | approximate |
| Body | 12 resonators per type, scaled by size, Q x 25^Decay with peak gain growing with Q, Low / High Cut | Sonora's own mode sets |
| Filter | Res sets a peak gain G (0.09 / 3.5 / 120 at 0 / 50 / 100 %): LP12 / HP12 / BP6 / N2P one stage Q = G, LP24 / HP24 / N4P two stages Q = sqrt G, BP12 two BP6; formants walk I-E-A-O-U | LP / HP / BP measured, formants approximate |
| Level | default pluck -13.6 dBFS, hammer -5.9, bow -2.9 at Volume 0 dB; velocity has no effect unless a Vel box is set | measured |

## Parity (2026-11-07)

`parity_probe compare`: law cases 63 / 86 within 3 dB of the reference's level (median 2.0 dB), presets 38 / 107 within
6 dB (median 7.6 dB, bias -2.6 dB). No case nulls: the reference's internals (contact models, body) are not known, so
waveforms differ even where levels, decays and spectra agree. Next (DEV-28j2): per-section probe cases for the body
(transfer functions per type / size), damper, termination and the hammer's mass law; the bow's Velocity law.
