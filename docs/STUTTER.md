# Sonora Stutter (PLG-18) and Beat Repeat (PLG-18b)

Vocal stutters and retriggers in time with the track. Built-in device (catalog `SonoraStutter`, Audio Effects > Delay &
Reverb) and a VST3 (`cmake --build build --target SonoraStutter_VST3`, plug-in code `SnSt`, `Sonora Stutter.vst3`, MIDI
input). Requested 2026-11-11, after a video of a vocal stutter plug-in. Stutter is Sonora's own design:
nothing of that product (look, names, presets) was copied, and there is no reference device face to match (AGENTS rule 7
does not apply).

## How it works (`src/devices/native/Stutter.cpp`)

The input is recorded all the time into an 8 s stereo ring. While Stutter is engaged it plays a slice from the ring
instead of the input:

| Mode | What plays |
|---|---|
| Repeat | The slice that starts where it was engaged, again and again. The first pass is the live input being captured. |
| Reverse | The slice that ended where it was engaged, backwards, again and again. |
| Freeze | The sound at the engage point held by two overlapping 90 ms Hann grains (a little random jitter). |
| Stop | Tape stop: the playback slows from normal speed to a halt over one slice, then silence. |
| Gate | The live input chopped in time with the Rate (Spacing = the open part, 50 % when Spacing is 100 %). |

Slice length: Rate (1 Bar, 1/2, 1/4, 1/4T, 1/8, 1/8T, 1/16, 1/16T, 1/32, 1/32T, 1/64) at the host tempo, or Length in
ms with Sync off. Per repeat: **Shrink** (each repeat up to half the one before, down to 4 ms: a roll that speeds up),
**Pitch** + **Ramp** x repeat (semitones, read faster/slower inside the repeat), **Decay** (gain (1-Decay)^repeat),
**Wide** (repeats alternate left/right). **Spacing**: the part of each repeat that sounds. **Smooth**: the fade at every
edge and the dry/wet crossfade. **Tone**: low-pass (left) / high-pass (right) on the wet sound. **Mix**: wet over dry
while engaged. **Output**: wet gain. Idle, the input passes untouched (bit-exact). No latency.

## What engages it

Priority: face pads > MIDI > Hold > pattern.

- **Pads** (face): hold one with the mouse to stutter in that mode; it also selects the Mode. **Latch** = the Hold control.
- **Hold** (host-automatable): engages the selected Mode. Waits for the **Quantize** grid (Off, 1/16, 1/8, 1/4, 1 Bar)
  while the transport plays.
- **MIDI**: C1 Repeat, D1 Reverse, E1 Freeze, F1 Stop, G1 Gate (36..47, white keys) while held. C2 (48) and up: Repeat
  pitched by (note - C3) semitones, looping the slice that ended at the note so the pitch is heard at once. A new note
  retriggers. Quantize applies too.
- **Pattern**: 16 steps of 1/16 (one 4/4 bar, from the host's beat position) while the transport plays; each step Off or
  a mode. Neighbouring steps with the same mode are one event (the repeat keeps going); **Chance** rolls once per event.
  Defaults: the last four steps Repeat (the classic end-of-bar stutter), Pattern off.

## Face (`src/ui/views/StutterFace.cpp`)

TRIGGER (five pads with their MIDI keys, Latch) | SLICE display (the captured slice in the mode's colour, play head,
"Repeat x3") over the PATTERN strip (click: the selected mode or back to Off; drag paints; Alt/right-click clears) |
TIME (Rate or Length, Shrink, Spacing, Quantize, Smooth, Sync) | PITCH (Pitch, Ramp, Decay, Wide, Chance) | OUT (Tone,
Mix, Output). The VST3 window is a 24 px header above the same face.

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

- `sonora_tests stutter`: idle pass-through (bit-exact), Repeat / Reverse / Shrink / Decay / Pitch sample positions on a
  ramp, tape stop lag and silence, Gate open/closed, the pattern firing on its step, Quantize waiting for the beat, MIDI
  modes and pitched notes, every mode finite and bounded on noise, catalog/factory/state; the VST3 window with real
  mouse input (pads momentary, Latch, pattern click/drag/clear, Pattern/Sync buttons), MIDI through `processBlock`, the
  face in the app's device chain, PNG renders.
- `sonora_tests "beat repeat"`: tables, a repeat at the interval point (Insert), Chance 0 never, Repeat button now.
- `plugin_probe --inspect-file "Sonora Stutter.vst3" --stimulus sine --midi-event 24000:90247F`: loads, 0 latency,
  state saved, finite render.
- `fx_render "<path>/Sonora Stutter.vst3" --in vocal.wav --out stutter.wav --set "Hold=1"` for real material.
