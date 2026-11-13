# Sonora Stutter (PLG-18, reworked by PLG-23) and Beat Repeat (PLG-18b)

A granular voice stutter. Built-in device (catalog `SonoraStutter`, Audio Effects > Delay & Reverb) and a VST3
(`cmake --build build --target SonoraStutter_VST3`, plug-in code `SnSt`, `Sonora Stutter.vst3`, MIDI input). Requested
2026-11-11 after a video of a vocal plug-in; the first version (0.1.32: a slice repeater with five modes and a 16-step
pattern) did not do what the video showed and was hard to use. PLG-23 (2026-11-12) rebuilt it as a granulator played by
one knob. Sonora's own design: no look, names or presets of that product were copied, and there is no
reference device face (AGENTS rule 7 does not apply).

## How it works (`src/devices/native/Stutter.cpp`)

The input is recorded all the time into a 20 s stereo ring. While Stutter is engaged it plays **grains** read from the
recording instead of the input:

- every **Every** ms (a note value with **Sync**, on the transport's grid while it plays) a grain of **Grain** ms starts;
  **Shape** is its fade in/out (0 = a hard gate with 2 ms edges, 1 = it swells in and out); overlapping grains are scaled
  by 1/sqrt(overlap);
- each grain starts at the **read head**, which starts where Stutter was engaged and moves through the recording at
  **Speed** (1 = real time, 0.25 = four times slower, 0 = frozen, negative = backwards) inside the **Capture** window (the
  last beat, bar, 2 or 4 bars before the engage point, or All). It wraps at the window's edges and never reads what is not
  recorded yet (nor before the first sample recorded);
- **Pitch** transposes the grains (the speed stays), **Width** pans odd grains left and even ones right, **Jitter**
  scatters their start (back into the window) and timing;
- the wet sound goes through **Tone** (low-pass left, high-pass right), the mode's extras and **Space** (a 4-line
  feedback-delay room that rings on after the stutter stops); **Mix** keeps the dry voice under it, **Level** sets the
  stutter's volume. Engaging and releasing crossfade over 12 ms. Idle, the input passes untouched. No latency.

### Motion and the modes

**Motion** goes from Slow (long, swelling grains that barely move) to Fast (short, tight grains racing). With **Macro**
on, Motion drives Grain, Every, Shape, Speed, Width, Jitter and Space (`Stutter::macroSettings`); the face shows those
knobs moving. Turning one of them by hand calls `detachMacro`: the knobs take the values Motion had, Macro goes off, the
sound does not jump. Each mode gives Motion its own character:

| Mode (MIDI) | Slow | Fast | Extra |
|---|---|---|---|
| Swell (C) | 450 ms grains, 0.03x, every grain swells in | 55 ms grains racing at 1x | - |
| Rewind (D) | long reversed swells drifting back (-0.08x) | rushes backwards (-2.2x) | every grain reversed |
| Shatter (E) | scattered fragments with gaps | a dense spray of tiny glitches | random reversed / +-12 st grains |
| Freeze (F) | stops dead, 600 ms grains: a soft pad | 45 ms grains every 95 ms: hard chops | speed 0 |
| Spread (G) | a huge slow cloud left and right | a hard wide doubler | odd grains -detune, even +detune |
| Tunnel (A) | sinks into a dark echoing tube (32 ms comb) | a metallic fluttering pipe (3.2 ms) | resonant comb per side, darker tone |
| Retrigger (B) | repeats the last beat | a 1/64 roll | the slice before the engage point, repeated (grid-locked with Sync) |

## Auto (PLG-23c): it plays itself from the voice

One click on **AUTO** and Stutter listens for the voice: an envelope (3 ms up, 25 ms down) against a noise floor that
follows the quiet parts down at once and creeps up 1.5 dB/s; the threshold is the floor + 6..24 dB with an absolute gate
(**Sensitivity**). The voice "speaks" while the envelope is over the threshold; a pause starts after 150 ms under it
(a phrase must have lasted 200 ms). **When**:

| When | Engages | Length "Whole" |
|---|---|---|
| Each pause (default) | 150 ms into each pause, anchored 30 ms after the last syllable, so the grains read that syllable | until the voice comes back |
| Phrase start | at each phrase's first sound | until the phrase ends |
| End of bar / End of 2 bars | on the last beats of every bar (or two) while the transport plays and the voice was heard in the last 2 s; releases on the bar line | 1 beat |

**Length** otherwise: 1/8, 1/4, 1/2, 1 bar, 2 bars (at the tempo; "Whole" stops after 4 bars at most). **Variation**
picks a mode and a Motion for each one: with chance Variation a mode from a pool that suits the moment (pauses: Swell,
Freeze, Rewind, Spread, Tunnel; phrase starts: Swell, Spread, Shatter, Tunnel; bar ends: Retrigger, Shatter, Rewind,
Freeze, Swell), and Motion = the knob + up to +-45 % x Variation; 0 = always the selected mode and Motion. The hand (pad,
MIDI, Hold/Latch) always wins over Auto. The face shows the voice light, its level against the threshold, and where the
voice was heard (a green line under the input).

## What engages it

- **HOLD** pad on the face (momentary) and **Latch** (= the **Hold** control, host-automatable).
- **MIDI**: the white keys of any octave play their mode while held (C Swell, D Rewind, E Shatter, F Freeze, G Spread,
  A Tunnel, B Retrigger), black keys the selected mode. Every new note re-anchors the read head at that moment.
- Changing the mode while engaged keeps the anchor (the modes morph).

## Face (`src/ui/views/StutterFace.cpp`, ProFace kit: docs/PRO_FACES.md)

Full (its own window, the VST3; 1160 x 718): the wordmark and what plays now ("FREEZE  frozen . 120 ms grains every
107 ms", "AUTO . waiting for a pause in the voice"), Free / Sync to tempo; the display: INPUT (the last 12 s recorded, the capture window shaded, the read head, where
the grains read) and OUTPUT (the last 4 s out, every grain drawn as its envelope, pink = left, violet = right) with the
Capture choice; the AUTO strip (AUTO, the voice light and level, When, Length, Variation, Sensitivity); the HOLD pad and
Latch; MOTION & MODE (the Motion knob, Macro, the seven modes with their MIDI keys, what
Slow and Fast do in the selected mode); GRAIN (driven by Motion while Macro is on); OUT (Tone, Mix, Level, meter); the
help line. Compact (the device chain, 700 x 170): HOLD, AUTO, Latch, the display, Motion, Macro, the modes, Speed, Mix; the title
bar's window icon opens the full face.

## Beat Repeat (`src/devices/native/BeatRepeat.cpp`)

The reference's Beat Repeat on the same record-and-repeat idea, with the set's parameter names and Manual values:
Interval 0..7 (1/32 .. 4 Bars) and Offset (sixteenths) place the trigger points; Chance decides; the first Grid after a
trigger is the input itself, then that grid repeats until Gate sixteenths have passed. Grid 0..15 = 1/128 .. 4 Bars
(odd indices up to 1/3 are triplets; No Triplets uses the straight value below), Variation picks a finer grid at random
per repeat, Decay fades over the gate, Pitch (0..12 st down) and Pitch Decay resample the repeats, Mix Mode Mix/Insert/Gate,
Volume, a band filter (Frequency, Width in octaves), Repeat = repeat now. The value tables come from the reference's
own presets (`Core Library/Devices/Audio Effects/Beat Repeat/*.adv` were read, nothing copied), not from measured output:
`TODO(PLG-18b)` check them with `parity_probe` stems.

## Verification

- `sonora_tests stutter`: idle pass-through (bit-exact); Freeze keeps playing the 200 Hz from before the engage point while
  the input went on at 1 kHz (zero crossings); Speed 0.25 / -0.5 moves the read head by that much in half a second and
  the grains read later / earlier; Rewind grains reversed, Width alternates sides; Retrigger with Sync starts a grain on
  every 1/16 of the grid from the same slice; Motion slow vs fast per mode, snapToNote, detachMacro keeps the settings;
  MIDI white/black keys and the face's pad engage and release; catalog/factory/state; every mode at Motion 0 and 1 finite,
  bounded and sounding on noise (also when engaged before anything was recorded).
- `sonora_tests "stutter Auto"`: on a speech-like signal (phrases and pauses over a -70 dBFS floor): it stutters in
  each pause (the output plays while the input is silent) and the voice passes dry while it speaks, bit-exact; phrase
  starts for a beat; bar ends only while the voice is there; Variation gives three or more modes; Hold wins.
- `sonora_tests "stutter plugin"`: the VST3's full face with real clicks (modes, Sync, Capture, Motion drag, the HOLD pad
  while a voice plays, a grain knob taking over from Macro, the help line), `sonora_stutter_plugin.png`; the compact face
  in the chain (mapped, window icon, a mode click), the window's full face, `sonora_stutter_chain.png`.
- `fx_render "<path>/Sonora Stutter.vst3" --in vocal.wav --out stutter.wav --set "Hold=1" --set "Mode=..."` for real
  material.
