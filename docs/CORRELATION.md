# Sonora Correlation (PLG-24)

A stereo correlation meter: how much left and right have in common, where in the spectrum, and whether the mix survives a
mono sum, said in words. Built-in device (catalog `SonoraCorrelation`, Audio Effects > Utilities) and a VST3
(`cmake --build build --target SonoraCorrelation_VST3`, code `SnCo`, `Sonora Correlation.vst3`, category Fx Analyzer).
Requested 2026-11-14 ("a correlometer, native and VST3"). Sonora's own device: no reference face to match. It is called
Correlation, not Correlometer: that is a third-party product's name.

Audio passes through untouched (bit-exact) while Listen is Stereo. No latency, no tail.

## Measurements (`src/devices/native/Correlation.cpp`, all on the audio thread, no allocation)

- **Correlation** = sum(L R) / sqrt(sum(L^2) sum(R^2)) over an exponential window of **Integration** (30 ms - 3 s, 300 ms):
  +1 = mono, 0 = unrelated, -1 = one side inverted. Below -70 dBFS (mean RMS) nothing is measured ("no signal").
- **Bands**: 31 ISO third-octave centres (20 Hz - 20 kHz, `bandCentre` = 1 kHz x 2^((i-17)/3)). Each band is two cascaded
  RBJ band-passes with Q 2.77 (together -3 dB at the third-octave edges, 2^(+-1/6)), run in double on L and R; the band's
  window is the longer of Integration and 8 periods of its centre (400 ms at 20 Hz). A band whose upper edge is above
  0.48 x the sample rate is inactive (20 kHz at 44.1 kHz). Below -84 dBFS a band has no signal.
- **Low end**: the bands up to 160 Hz together (sum of their L R, L^2, R^2), where a mono sum hurts most (clubs, phones).
- **Mono sum** = energy of (L+R)/2 against the mean of L and R energy, in dB (0 = nothing lost, -3 = unrelated, floor -60).
- **Width** = sqrt(side energy / mid energy), side = (L-R)/sqrt 2, mid = (L+R)/sqrt 2: 0 mono, 1 unrelated, > 1 more side.
- **Balance** = 10 log10(R energy / L energy) (+ = right louder). **Levels**: RMS per side; peaks per reading.
- **Held extremes** (wideband min and max, per-band min): kept for **Hold** (1 s, 3 s, 10 s, Infinite), then they move back
  at 1.0 per second; Infinite keeps them until **Reset** (which also clears the history).
- Readings are published 40 times a second (atomics); the **history** keeps 320 of them (8 s): all and the low end.
- The **scope** ring keeps the newest 8192 samples as x = (R-L)/sqrt 2, y = (L+R)/sqrt 2 (mono is vertical, a side alone
  leans 45 degrees, inverted is horizontal; every sample of a -1..1 signal lands inside the diamond |x| + |y| <= sqrt 2).
- NaN/inf input counts as silence (it would poison the running sums for good).
- **Freeze** (face button, not a parameter) stops the readings and the scope; audio keeps passing.
- **Listen**: Stereo (untouched), Mono ((L+R)/2 on both), Side ((L-R)/2 on both), Left, Right; a switch crossfades over
  10 ms. The face lights the check modes amber and says "the output is not the input".

## Verdict (the face, `CorrelationFace::verdict`)

In order: off / waiting for audio / only one side has sound / Mono (>= 0.97, width < 5 %) / Out of phase (wideband < -0.2,
unless the low end alone explains it: the energy-weighted correlation of the bands above 160 Hz is >= 0) / Low end out of
phase (< -0.1) or too wide (< 0.2), naming the worst low band / Phasey (< 0) / Very wide (< 0.15) / Mono-compatible. The
worst band is the lowest correlation among the bands within 40 dB of the loudest.

## Face (`src/ui/views/CorrelationFace.cpp`, ProFace kit: docs/PRO_FACES.md, accent violet)

Full (1120 x 720, the window and the VST3): the wordmark, LISTEN (Stereo, Mono, Side, L, R), Freeze, Reset; the VERDICT;
CORRELATION (the value big, coloured red -> amber -> green; a bar from 0, the needle, the held range with a marker at each
end, -1 "out of phase", 0 "unrelated", +1 "mono"); SCOPE (Lissajous or Polar = the same folded into a half disc; four ages
of samples added light on light; auto zoom: louder at once, quieter back at 15 dB/s, 0..30 dB; the Zoom knob takes over);
BANDS (bars from 0 coloured by value and faded by level against the loudest band, the held minimum, the level as a faint
spectrum behind, the low end tinted; the pointer reads a band in the help line); HISTORY (8 s: all, filled green above 0
and red below, and the low end in orange); STEREO (balance +-12 dB, width 0-200 %, mono sum, levels); METER (Integration,
Hold, Zoom). Compact (760 x 170, the chain): the scope, the value and verdict, the meter bar and the bands, Listen,
Integration, Hold, Freeze, Reset, Lissajous / Polar. The chain's window icon opens the full face.

## Verification

- `sonora_tests correlation`: mono (> 0.999, width 0, mono sum 0 dB), inverted (< -0.999, mono sum at the floor), unrelated
  noise (|c| < 0.1, mono sum -3 dB, width 1), 90 degrees (|c| < 0.02), balance -6.02 dB for half the level on the right,
  output bit-exact; an inverted 63 Hz under a mono 1 kHz: 63 Hz band < -0.9, 1 kHz > 0.9, low end < -0.8; 20 kHz band
  inactive at 44.1 kHz; silence, NaN input, holds (1 s then release, Infinite, Reset, per band), history (40/s, 320),
  scope geometry, Freeze; every Listen mode exact after the crossfade, the crossfade itself, Stereo bit-exact again;
  catalog, class id -> built-in stand-in, state round trip.
- `sonora_tests "correlation plugin"`: the VST3 processor and window; a wide mix -> "Mono-compatible" and untouched; the
  same with an inverted 55 Hz bass -> "Low end out of phase" naming the band; band hover in the help line; Polar, Mono,
  Stereo, Integration, Zoom (Auto off), Freeze, Reset by real clicks and drags; state. PNGs in `$TMPDIR/sonora_tests/`:
  `sonora_correlation_healthy.png`, `sonora_correlation_plugin.png` (the site's screenshot), `sonora_correlation_polar_mono.png`,
  `sonora_correlation_chain.png` (compact face in the chain; the window shows the full face).
