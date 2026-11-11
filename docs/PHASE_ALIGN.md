# Sonora Phase Align (PLG-21)

Keeps a bass in phase with the kick, hit by hit. Built-in device (catalog `SonoraPhaseAlign`, Audio Effects > Utilities)
and a VST3 (`cmake --build build --target SonoraPhaseAlign_VST3`, code `SnPa`, `Sonora Phase Align.vst3`, a stereo
sidechain bus). Requested 2026-11-11. Sonora's own device: no reference face to match.

## What "in phase" can mean here

Kick and bass play different pitches, so they cannot be in phase everywhere all the time. What matters is the low end
where they overlap, while the kick sounds: if the bass's lows point the other way at that moment, the two cancel and the
low end gets thinner. Phase Align rotates only the bass's low band to where the two add up, per hit, gliding smoothly.

## Signal flow (`src/devices/native/PhaseAlign.cpp`)

1. Bass -> Linkwitz-Riley 4th order split at **Crossover** (60-400 Hz, 150 default).
2. Low band -> Olli Niemitalo's 90-degree allpass pair (two chains of four 2nd-order allpasses; A one sample late): A and
   B are 90 degrees apart within about a degree from 20 Hz up at 44.1/48 kHz. Rotated low = cos(angle) A - sin(angle) B
   (180 = polarity flip). Output = rotated low + high band, times **Output**.
3. Kick (sidechain, mono) -> the same low-pass -> onset detector: fast (0.5/30 ms) and slow (30/300 ms) envelopes; a hit
   is fast > **Threshold** and fast > 1.4 x slow, at least 60 ms after the last.
4. For **Window** ms from the onset: Ca = sum(kick x A), Cb = sum(kick x B) with the kick as heard (unfiltered: the
   low-pass would shift its phase; A/B only hold the bass's lows anyway). Best angle = atan2(-Cb, Ca) (no search).
   Coherence = |C| / sqrt(kick low energy x bass low energy); hits under **Min. Coherence** are ignored.
5. **Mode**: Auto follows each coherent hit; Hold keeps the angle learned so far; Manual uses **Angle**. The applied angle
   glides along the shortest arc, a half turn in **Speed** ms.
6. **Listen**: Bass (normal), Sum (+ the kick, to hear them together), Orig. (the bass as it came in + the kick: A/B).

No latency: a new angle applies from the end of the hit's window, so steady patterns settle within a couple of hits
(PLG-21b: a lookahead mode). Rotating the low band moves it against the high band near the crossover (a few tenths of a
dB of ripple there): keep Crossover just above the overlap.

## In the app

Put it on the bass track, then pick the kick track: the title-row button ("Choose the kick track...") or the device menu
> Sidechain. That is PLG-14i's chooser (`ui::sidechainActions`), the same one Hush, Compressor, Glue and Gate use.

## Face (`src/ui/views/PhaseAlignFace.cpp`)

PHASE dial (applied angle needle, target dot, coherence ring, Auto/Hold/Manual) | LAST HIT (kick orange, bass before
grey, bass after cyan over the window; "low end +x dB" = energy of kick + bass after vs before) | HITS (one dot per hit at
the angle it asked for, brighter = more coherent, hollow = ignored) | ALIGN | DETECT | OUT.

## Verification

- `sonora_tests "phase align"`: bass at 180 / 90 / -120 / 45 degrees from a decaying 60 Hz kick -> correlation with the
  kick 0.98 after alignment in every case (it was -0.98 / 0.00 / -0.49 / 0.70), +13.8 dB low end for the polarity flip;
  Hold keeps, Manual follows Angle, no sidechain = no hits and level within 0.5 dB; Listen Orig. = bass + kick; state.
- VST3 with the kick on the sidechain bus (channels 2-3) through `processBlock`; the chain face names the kick track.
