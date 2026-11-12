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

7. **Transparent until it helps** (PLG-23): the allpass pair changes the bass's phase even at 0 degrees, so in Auto the
   output is the untouched bass until hits show something to gain: per hit the *potential* gain (kick + bass energy over
   the window at the best angle vs as it came in) and, when it is 0.25 dB or more on at least two of the last eight
   usable hits, the rotated signal fades in over 150 ms; when none of them gains, it fades back to the untouched bass.
   Manual always processes, Hold keeps the state. **Verdict**: no kick / listening / the bass is silent under the kick
   (bass under the kick below -40 dB) / no shared frequency (coherence under Min. Shared) / already in phase / aligning
   (+x dB) / manual.

No latency: a new angle applies from the end of the hit's window, so steady patterns settle within a couple of hits
(PLG-21b: a lookahead mode). Rotating the low band moves it against the high band near the crossover (a few tenths of a
dB of ripple there): keep Crossover just above the overlap.

## In the app

Put it on the bass track, then pick the kick track: the title-row button ("Choose the kick track...") or the device menu
> Sidechain. That is PLG-14i's chooser (`ui::sidechainActions`), the same one Hush, Compressor, Glue and Gate use.

## When there is nothing to align

An off-beat bass (common in psytrance) is ducked or silent while the kick sounds and only comes in as the kick's tail
fades; where both sound, they are often at different pitches. Then the correlation is about 0 and no angle adds low end
(measured on a real 139 BPM set: the bass -119 dB under the kick, 0.00 dB to gain at any window from 90 to 300 ms). Up to
0.1.37 the bass still went through the allpass pair (a different waveform, the same energy); since PLG-23 it passes
untouched and the verdict says why.

## Face (`src/ui/views/PhaseAlignFace.cpp`, ProFace kit: docs/PRO_FACES.md)

Full (1100 x 600): the kick's source (a click opens the chooser in the app); the VERDICT in words; PHASE (the dial: applied
angle on a plate, "rotating" / "untouched", target dot, coherence ring; Auto / Hold / Manual); LAST KICK (kick, bass before
and after, each scaled to be seen with "bass drawn +x dB"; below, kick + bass as it was against aligned); HITS (per hit the
potential gain as an outline and the applied gain filled, unusable hits as dots; bass under the kick, shared low end);
ALIGN, DETECT, OUT (Listen: Bass / Kick + Bass / Original + Kick, Output); the help line. Compact (the chain, 760 x 170):
the dial, the verdict over the last kick, the kick button, modes, Listen, Crossover, Window, Angle, Output.

## Verification

- `sonora_tests "phase align"`: bass at 180 / 90 / -120 / 45 degrees from a decaying 60 Hz kick -> correlation with the
  kick 0.98 after alignment in every case (it was -0.98 / 0.00 / -0.49 / 0.70), +13.8 dB low end for the polarity flip;
  Hold keeps, Manual follows Angle, no sidechain = no hits and level within 0.5 dB; Listen Orig. = bass + kick; state.
- VST3 with the kick on the sidechain bus (channels 2-3) through `processBlock`; the chain face names the kick track.
- PLG-23: in phase already -> InPhase and the output is the input (1e-5); an off-beat bass ducked under the kick -> Silent,
  untouched; a 97 Hz bass against a 60 Hz kick -> Unrelated, untouched; a polarity flip -> Aligning, processed, +x dB
  potential in the history; no sidechain -> NoKick and bit-exact; the full face's verdict and modes/Listen/Crossover by
  click (`sonora_phase_align_plugin.png`); the compact face and the window (`sonora_phase_align_chain.png`).
