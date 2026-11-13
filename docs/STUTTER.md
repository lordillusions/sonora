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

## Auto presets (PLG-23d): one click, it plays itself while the voice is there

PLG-23c's Auto engaged in the pauses of the voice. Measured on a real, reverb-wet vocal stem (60 s): it fired once, because
sung lines never drop to a pause the detector can trust. PLG-23d replaced it with presets: a choreography of stutters on
the song's bars that plays only while the voice is there.

**AUTO** on, pick a **preset**. A preset is a cycle of 2, 4 or 8 bars (4/4) with **moves** at fixed beats
(`Stutter::preset`, `Stutter::Move`); each move engages one mode for its length, with ramps of Motion, Pitch, Speed and
Tone (`Stutter::moveSettings`), its own Mix and Space, and a tier:

| Preset | Cycle | Moves (beat: move; tier 2 / 3 in brackets) |
|---|---|---|
| Vocal Chop (default) | 4 bars | [3.5 1/16 roll] · 7 ROLL 1/8 to 1/16 · [[10 Shatter]] · 12 STRETCH (Swell, slow) · [14 REWIND] · 15 BUILD 1/16 to 1/64 rising 7 st |
| Stretch & Freeze | 4 bars | [[2 Spread under]] · 6 STRETCH to a standstill · [10 WIDE] · 12 FREEZE pad under the singing (Mix 55 %) |
| Build-Up | 8 bars | [16 1/4] · 20 1/8 · 24 1/16 (+5 st) · 28 1/32 (to +11 st, thinner) · 31 1/64 (+12 st) |
| Tape Stop | 4 bars | [[3 1/8]] · [6.5 SPIN] · 13.5 TAPE STOP (Speed 1 to 0, Pitch 0 to -24 st, darker) |
| Backspin | 2 bars | [3.5 1/16] · [[5 GLITCH]] · 7 BACKSPIN (Rewind, fast) |
| Glitch | 2 bars | [1.5 SHATTER] · 2.75 1/32 · [[3.5 LOW roll -12 st]] · [5 CHOP] · 6.5 ROLL · 7 SHATTER |
| Dub Tunnel | 4 bars | [3 ECHO] · 7 TUNNEL · [[11 ECHO]] · 14 DIVE (Tunnel, slow to fast, big Space) |
| Cloud | 4 bars | 0 CLOUD all along under the voice (Mix 30 %) · [[6 STRETCH]] · [12 FREEZE] |

- **Amount**: Light (< 34 %) plays the tier-1 moves, Medium (default) tiers 1-2, Wild all of them. The latest-starting
  move wins where moves overlap (`Stutter::moveAt`).
- **Follow**: Bars (default) puts the cycle on the transport's bars from the song's start. Voice starts it on the beat
  nearest to the voice coming in after a bar of silence. With the transport stopped it always follows the voice (a free
  clock at the tempo, 120 BPM without a host).
- **The voice gate**: a move starts on its beat only if the voice was heard in the last 1.5 beats (else the face says
  "no voice, resting"); a long move (a beat or more left) also starts late when the voice comes in during it. The read
  point is the last thing sung: now while the voice sounds, else 30 ms after its last syllable, so a roll after the line
  ends repeats the line's end. The detector: an envelope (3 ms up, 25 ms down) against a noise floor that follows the quiet
  parts down and creeps up 1.5 dB/s but never above -45 dBFS (a long sung line must not lift it up to the voice);
  threshold = the floor + 6..24 dB with an absolute gate (**Sensitivity**).
- Retrigger plays one repeat at a time: a new repeat cuts the last with a 2 ms fade (a roll that speeds up does not stack
  copies of a loud syllable). A **peak guard** keeps the stutter at or under the input's own recent peak (3 s decay;
  instant down, 80 ms up). A non-finite input sample counts as silence.
- The hand (pad, MIDI, Hold/Latch) always wins over Auto.

Measured with `fx_render ... --set "Auto=1" --text "Auto Preset=<name>" --tempo 126` on a 126 BPM vocal hook (45 s, peak
0 dBFS, RMS -13.6 dB): every preset acts in time on the bars (31-66 % of the time changed; Cloud all of it), peaks
-0.6..+1.0 dBFS, RMS -13.2..-16.8 dB.

## What engages it

- **HOLD** pad on the face (momentary) and **Latch** (= the **Hold** control, host-automatable).
- **MIDI**: the white keys of any octave play their mode while held (C Swell, D Rewind, E Shatter, F Freeze, G Spread,
  A Tunnel, B Retrigger), black keys the selected mode. Every new note re-anchors the read head at that moment.
- Changing the mode while engaged keeps the anchor (the modes morph).

## Face (`src/ui/views/StutterFace.cpp`, ProFace kit: docs/PRO_FACES.md)

Full (its own window, the VST3; 1160 x 758): the wordmark and what plays now ("FREEZE  frozen . 120 ms grains every
107 ms", "AUTO . waiting for a pause in the voice"), Free / Sync to tempo; the display: INPUT (the last 12 s recorded, the capture window shaded, the read head, where
the grains read) and OUTPUT (the last 4 s out, every grain drawn as its envelope, pink = left, violet = right) with the
Capture choice; the AUTO panel (AUTO, the voice light and level, the eight preset chips, Follow, Amount, Sensitivity, and
the preset's cycle as a timeline: one block per move coloured by mode, dim blocks for a higher Amount, the song's position
as a white line, the move playing lit with its progress); the header says the preset, the bar, the move playing or the
next one and in how many beats; the HOLD pad and Latch; MOTION & MODE (the Motion knob, **Link** (the Macro parameter:
Motion turns the grain knobs; it is not a sequence), the seven modes with their MIDI keys (a ring around the mode
playing when AUTO or MIDI plays another), what
Slow and Fast do in the selected mode); GRAIN (driven by Motion while Macro is on); OUT (Tone, Mix, Level, meter); the
help line. Compact (the device chain, 700 x 170): HOLD, AUTO, the preset (a click: the next one), Latch, the display with
the preset's timeline under it, Motion, Link, the modes, Speed, Mix; the title bar's window icon opens the full face.

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
- `sonora_tests "stutter Auto presets"`: the tables (moves inside the cycle, sorted, a tier-1 move each, blurbs), the ramps
  (Tape Stop from 1x/0 st to 0x/-24 st, Build-Up 1/8 to 1/64), moveAt/nextMove; on a sung line with no pauses at 120 BPM
  Vocal Chop plays each move on its beat for two cycles, dry and bit-exact in between; the voice gate (a roll a beat after
  the line ends repeats its end over the silence; a move 3 s after it is skipped; no voice = nothing; a long move starts
  late); Amount light/wild; the transport stopped (the cycle from the voice coming in); Follow Voice (on the nearest
  beat); Hold wins; every move of every preset plays its mode, finite, never louder than the voice (Mix < 100 % aside);
  a NaN input block.
- `sonora_tests "stutter plugin"`: the VST3's full face with real clicks (modes, Sync, Capture, Motion drag, the HOLD pad
  while a voice plays, a grain knob taking over from Macro, the help line), `sonora_stutter_plugin.png`; the compact face
  in the chain (mapped, window icon, a mode click), the window's full face, `sonora_stutter_chain.png`.
- `fx_render "<path>/Sonora Stutter.vst3" --in vocal.wav --out stutter.wav --set "Hold=1" --set "Mode=..."` for real
  material.
