# Sonora Correlation (PLG-24)

A stereo correlation meter: how much left and right have in common, where in the spectrum, and whether the mix survives a
mono sum, said in words. Built-in device (catalog `SonoraCorrelation`, Audio Effects > Utilities) and a VST3
(`cmake --build build --target SonoraCorrelation_VST3`, code `SnCo`, `Sonora Correlation.vst3`, category Fx Analyzer).
Requested 2026-11-14 ("a correlometer, native and VST3"). Sonora's own device: no reference face to match. It is called
Correlation, not Correlometer: that is a third-party product's name.

Audio passes through untouched (bit-exact) while Listen is Stereo. No latency, no tail.

**Measure** (PLG-24c, 2026-11-17): **L x R** (the stereo meter below) or **Kick x Bass** (further down): is the bass in phase
with the kick where they meet? Requested for psytrance basslines, to replace a manual check (a group with the kick panned
left and the bass right, Integration 30-50 ms, reading the low bands one by one), together with Integration in note values
(1/4, 1/16 at the tempo) and the readings that matter shown big, with decimals.

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

- **Integration** can follow the tempo: **Sync** (face: ms / note) and **Integration Note** (1/64, 1/32, 1/16, 1/8, 1/4, 1/2,
  1 bar, 2 bars; a bar from the host's time signature). Window = beats x 60000 / tempo ms, 5 ms - 10 s. The engine and the
  VST3 (`AudioPlayHead`) pass the tempo through `setHostInfo`.

## Kick x Bass (PLG-24c, `Correlation::pairStep`, all on the audio thread)

Where: the bass track, with the kick track chosen in the device's **Sidechain** section (the app; `takesSidechain()`, saved
in the .als `<SideChain>`) or the VST3's sidechain bus. **Kick From Left** keeps the old pan trick: on a group, the kick
left and the bass right. Kick = sidechain (L+R)/2 x the Sidechain Gain; bass = input (L+R)/2 (Left: kick = L, bass = R).

- Both through a 25 Hz high-pass and a Linkwitz-Riley low-pass at 160 Hz (the same filters: their relative phase is kept),
  then every Nth sample (~3 kHz).
- **Overlap weight**: rho = smooth(2 |k b|) / smooth(k^2 + b^2) over 10 ms (1 = equal levels, ~2x the quieter one's amplitude
  ratio when far below). Smoothing the product, not each signal, leaves no memory of a kick that stopped before the bass came
  in (a product of envelopes did: the bass's fading envelope met the next kick's loud body and diluted the reading to 0.1
  for a bass exactly in phase). Then a ramp: w = 0 at rho 0.06 (the quieter one ~30 dB down: it moves the sum by 0.3 dB at
  most, and a loud kick over a trace of bass would swamp the real overlap), 1 at rho 0.5 (~12 dB down).
- Over Integration: **correlation** = sum(w k b) / sqrt(sum(w k^2) sum(w b^2)) (the meter, needle, holds and history show
  it); **sounding together** = sum(w (k^2 + b^2)) / sum(k^2 + b^2); the **sum where they meet** = 10 log10(1 + 2 sum(w k b) /
  sum(w (k^2 + b^2))) and the same with the bass inverted (1 - ...), dB, floor -40. Under 1 % together: "take turns" (no
  reading). Kick or bass below -70 dBFS: not playing.
- **Best shift**: the weighted correlation of the kick against the bass delayed by every step within +-12 ms (window at
  least 160 ms), the smallest shift within 0.01 of the best, refined by a parabola. A shift of half a period is the same as
  a polarity flip: the face then says so.
- The **bands** compare the kick with the bass with the same weight (their own windows ring ~8 periods, longer than an
  overlap, so they read less extreme than the main value: 50 Hz +0.6 when the low end reads +0.88). A band measures only
  where they meet in it (>= 1 % of its energy).
- **Beat view**: a ring of the filtered kick, bass and weight (8192 points, ~2.7 s). Kick onsets: a peak follower (30 ms
  release) that rises 6 dB within 8 ms to at least half the loudest recent kick, 70 ms apart. `beat()` returns one beat
  (at the tempo) from 10 ms before the last onset whose beat is complete (else the newest beat), max-magnitude per point.
- **Listen Bass Inverted** (value 5): the bass's polarity flipped (Sidechain: both channels; Left: the right one), to hear it
  against the kick track. With Kick From Left the face offers Stereo / Mono / Mono with the bass inverted (= Side).
- The sidechain pointer is used for one block and forgotten (the engine sets it before every block that has one).

Verdict (`CorrelationFace::pairVerdict`): no kick (says how to route it) / waiting for the bass / take turns (good) / in
phase (>= +0.3: "nothing to fix") / out of phase (<= -0.3: flip the bass, with the dB it gains; minor when the sum moves
less than 0.5 dB) / unrelated (different notes: normal), each with the best shift when it helps by 0.25.

## Verdict (the face, `CorrelationFace::verdict`)

In order: off / waiting for audio / only one side has sound / Mono (>= 0.97, width < 5 %) / Out of phase (wideband < -0.2,
unless the low end alone explains it: the energy-weighted correlation of the bands above 160 Hz is >= 0) / Low end out of
phase (< -0.1) or too wide (< 0.2), naming the worst low band / Phasey (< 0) / Very wide (< 0.15) / Mono-compatible. The
worst band is the lowest correlation among the bands within 40 dB of the loudest.

## Face (`src/ui/views/CorrelationFace.cpp`, ProFace kit: docs/PRO_FACES.md, accent violet)

Top: MEASURE (L x R / Kick x Bass), LISTEN, Freeze, Reset. The METER panel has ms / note (Sync) over Integration (the note
knob shows "207 ms at 145 BPM"). Numbers carry two decimals; the main value is drawn at 50 px (L x R) and 66 px (Kick x
Bass); the band under the pointer gets a callout with its value at 24 px; the history legend shows the values now.

Kick x Bass, full: KICK x BASS (the value, its word: in phase / out of phase / unrelated / take turns, held range, the
verdict on two lines, the meter from "cancels" to "adds up"); ONE BEAT FROM THE KICK (both low ends over each other, kick
orange filled, bass violet, the overlap lit by its weight, a strip under them green where they add up and red where they
cancel, 16ths dashed); WHERE THEY MEET (sounding together, the sum there, with the bass inverted, the best shift, levels,
Kick From + "sidechain connected"); HISTORY (the reading + how much sounded together); LOW BANDS 25-250 Hz with their values;
METER (Sync, Integration, Hold). Compact: the beat view, the value and word, sum / bass inverted, meter, low bands with values,
Listen, L x R / K x B, Integration, Hold, Freeze, Reset, ms / note (the L x R compact face moved Lissajous / Polar to the
window for ms / note).

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

PLG-24c: `sonora_tests "kick x bass"` (a synthetic K-B-B-B beat at 145 BPM, 50 Hz kick tail under the bass): in phase
+0.88 / sum +0.8 dB / inverted -1.0 dB, inverted -0.88, 90 degrees ~0 with a 5 ms shift found, a bass 4 ms late found at
-4 ms (+0.94), a short kick "takes turns", no sidechain = no kick, Kick From Left, Sync note values (4/4, 7/8, 3/4),
Listen Bass Inverted (both sources), the beat view from the onset; the VST3 with the kick on its sidechain bus and a 145 BPM
play head (verdicts, band callout, Listen, Sync drag, take turns, Kick From, state); the compact face in the chain; through
the engine with the kick track routed (`test_correlation_kickbass_engine.cpp`); the .als `<SideChain>` round trip. PNGs:
`sonora_correlation_kickbass_in_phase.png`, `sonora_correlation_kickbass_plugin.png` (out of phase, the site's screenshot),
`sonora_correlation_kickbass_turns.png`, `sonora_correlation_kickbass_chain.png`.

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
