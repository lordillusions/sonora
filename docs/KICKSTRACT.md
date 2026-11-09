# Sonora Kickstract (PLG-17)

Pulls a clean kick out of a full mix. Built-in device (catalog `SonoraKickstract`, Audio Effects > EQ & Filters) and a
VST3 (`cmake --build build --target SonoraKickstract_VST3`, plug-in code `SnKk`, `Sonora Kickstract.vst3`).

## The idea

A common producer trick: find a spot where the kick plays with few other sounds, duplicate it three times, filter the
copies into low (low-pass 150 Hz), mid (band-pass 150 Hz - 1.5 kHz) and high (high-pass 1.5 kHz), cut each copy short
right after the hit, then play them together. Kickstract does that live, on every hit, with the bands adjustable.

## Signal flow (`src/devices/native/Kickstract.cpp`)

1. Input gain.
2. **Detector** on the undelayed input: the low band (LR4 low-pass at the low split), a fast (0.3 ms / 20 ms) and a slow
   (15 ms / 250 ms) envelope follower. A hit is fast > Threshold and fast > 1.5 x slow, armed again when fast < 1.15 x slow
   and at least Retrigger ms after the last hit. Sustained bass does not retrigger; a kick over it does.
3. **Lookahead** 25 ms on the audio path (the device's latency: 1103 samples at 44.1 kHz). A pitch-dropping kick's
   low band only rises above 150 Hz's reach 13-21 ms after its beater click (measured on a real drum stem), so on a hit
   the device looks back through a broadband envelope ring (3 ms release) to where it falls to the window's floor (the
   hats / bass / pad before the kick): floor x 1.3 + 1 % of the rise. That is the onset; the gates finish their 1 ms
   opening ramp half a millisecond before it, so the click survives. History: 5 ms without backtracking left a spike
   at every kick in Removed; 15 ms with a bar at 8 % of the body's peak still started 1-2 ms late.
4. **3-band split**: Linkwitz-Riley 4th order (two Butterworth TPT SVF stages per side) at Low|Mid and Mid|High, the low
   band through the high split's allpass: the three bands sum flat (unit test: within 0.1 dB, 40 Hz - 12 kHz).
5. **Chop** (per band): on a hit the gate opens (1 ms ramp ending at the onset), holds Length ms after the onset, then closes over Fade ms
   (quadratic). Defaults: Low 180/140 ms, Mid 60/50 ms, High 12/20 ms. Chop off = the bands pass through (filters only).
   Gain, On and Solo per band.
6. **Finish**: Punch (compressor with a 10 ms attack: the hit passes, the body is held; threshold -6..-30 dB, ratio
   1..6, half makeup), Drive (tanh soft clip), Output.
7. **Listen**: Kick (the result), Removed (everything the gates took out: hats, bass, pads between kicks), Input
   (the delayed input, for A/B at equal latency).

## Face (`src/ui/views/KickstractFace.cpp`)

DETECT (Threshold, Retrigger, Input, Chop, detector meter with the threshold line and a hit lamp) | SPECTRUM (input in
grey, result in each band's colour; drag the split lines, double-click resets; band regions glow with their gate) |
LAST KICK (the captured hit: input in grey, each band after its gate, the result's waveform, each band's gate envelope;
drag the dots to set Length and Fade) | LOW / MID / HIGH strips | FINISH (Punch, Drive, Output, Listen).

Kickstract is Sonora's own device: there is no reference device face to match (AGENTS rule 7 does not apply).

## Verification

- `sonora_tests kickstract`: flat sum, band solo, 8/8 hits found in a synthetic mix (kick + hats + pad), the gaps drop
  > 40 dB while the hit stays within 1.5 dB of the isolated kick, its first 4 ms (the click) within 2 dB of the same mix
  with every gate open (0.003 dB measured; a gate 4 ms late gives 9.3 dB), Removed/Input modes, thresholds, capture,
  punch/drive bounds, catalog/factory/state; the VST3 window and the chain face with real mouse input, PNG renders.
- `fx_render "<path>/Sonora Kickstract.vst3" --in mix.wav --out kick.wav` (add `--set "Listen=0.5"` for Removed) for
  real material. On 20 s of a real drum stem (44.1 kHz, latency 1103): 46 hits; Removed's peak around each onset is 0.035
  median against 1.27 at the input (it had a spike at every kick before the backtracking); the bumps left in Removed
  are the end of each body that the Low band's Fade cuts, as intended.
