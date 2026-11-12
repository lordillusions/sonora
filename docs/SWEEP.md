# Sonora Sweep (PLG-19)

Risers, downlifters and reverse swells of any length that end on the beat. Built-in instrument (catalog `SonoraSweep`,
Instruments > Synthesis), a VST3 instrument (`cmake --build build --target SonoraSweep_VST3`, code `SnSw`) and a clip
command in the app. Requested 2026-11-11 ("sweep up, sweep down, any length, take any sample and do a sweep from it,
chord reverse going up, crash reverse feeling, vocal reverse going up").

## The instrument (`src/devices/native/Sweep.cpp`)

A note starts one sweep that lasts **Length** (1 Beat, 2 Beats, 1/2/4/8/16/32 Bars at the host tempo) and ends exactly
there (a 4 ms fade at the very end). A new note restarts it. Over the progress p, shaped by **Curve** (p^(4^Curve)):

| | Up (riser) | Down (downlifter) |
|---|---|---|
| Low-pass | Low -> High (opens) | High -> Low (closes) |
| Pitch | Noise/Tone: 0 -> +Pitch st; Sample: -Pitch -> 0 st | 0 -> -Pitch st |
| Level | 1 - Swell -> 1 | 1 -> 1 - Swell |
| Width | 0 -> Width | Width -> 0 |
| Rhythm gate | 1/4 -> 1/16 (etc.), speeding up | slowing down |

Sources (PLG-23 added the generators that need no sample; indices 0-2 kept from 0.1.32): **White Noise** (independent per
side), **Pink Noise** (Paul Kellet's filter on white), **Supersaw** (five detuned polyBLEP saws per chord note, Detune),
**Sine** (pure, for sub drops: play a low note), **Whoosh** (noise through the filter's resonant band, which wobbles
+-22 % at 0.35 Hz, out of phase left/right), **Metal** (six inharmonic square partials split left/right plus bright noise,
first-difference high-passed: a cymbal-like wash, so a reverse crash needs no sample), **Sample**. **Chord** stacks notes
on Supersaw and Sine (one note, octave, fifth, major, minor). **Air** adds a high-passed noise layer under any source; it
goes through the same filter.
A loaded sample is prepared on the message thread: resampled to the device rate, + a Freeverb-style reverb tail
(**Tail**: 1-5 s), reversed for Up when **Reverse** is on. For an Up sample sweep the start position is computed (the
integral of the rising playback speed over the sweep) so the reversed audio ends on the sample's attack exactly at the
sweep's end: reverse chord going up, reverse crash, reverse vocal. Prepared buffers are swapped to the audio thread
lock-free (the audio thread acknowledges what it uses; older ones are freed on the message thread).

## Types, Preview

`Sweep::types()`: White Riser, Pink Wash, Supersaw Rise, Tension Gate, Whoosh Up, Reverse Crash, Sub Drop, Downlifter,
Laser Down, Reverse Sample. A type sets every parameter but Length and Output (`applyType`); `matchingType` tells which
one the current settings are (the face lights it). **Preview** (`Sweep::preview`, the face's button) plays one sweep at
C3 (C2 for Sine) without MIDI; a second click stops it with a short fade.

## Face (`src/ui/views/SweepFace.cpp`, ProFace kit: docs/PRO_FACES.md)

Full (1100 x 700): what it makes ("Riser . 4 Bars . 6.9 s at 139 BPM") and how to place the note; Preview; TYPE (one
click) and SAMPLE (name, Choose..., drop); the display: the sweep itself rendered offline (22.05 kHz, time compressed to
6 s for long sweeps, re-rendered when the settings rest for 4 ticks) under the filter / pitch / level curves, the bars, the
drop (or the hit) marked, the play head; SOURCE, Up/Down, LENGTH; FILTER (Low, High, Reso, Air), PITCH (Pitch, Detune,
Chord), SHAPE (Curve, Swell, Width, Rhythm), OUT (Level, Tail, Reverse sample); the help line. Compact (the chain,
720 x 170): the display, the sources, Up/Down, Preview, Length, Swell, High, Level.

**Samples from the Browser**: drop a sample from Sonora's Browser (or the Finder) anywhere on the face: the path goes into
the device's native content as one undoable edit ("Load Sample", `ui::loadSweepSample`) and Source becomes Sample. In the
VST3, drop a file on the window or use Choose....

## Clip > Make Riser / Make Downlifter from Clip (`src/engine/SweepFromClip.cpp`)

Right-click an audio clip in the arrangement: 1, 2, 4 or 8 bars. The clip's sample (first 20 s) is rendered by the same
device offline at the set's tempo (Low 400 Hz, High 20 kHz, Swell 70 %, Output 0 dB) into
`Samples/Processed/Sweep/<name> [riser].wav`, and a new audio track ("Riser" / "Downlifter", same group) under the clip's
track gets the clip: a riser ends where the clip starts, a downlifter starts with it. One undo step. Refused when there is
not enough room before the clip.

## Verification

- `sonora_tests sweep`: exact length then silence; Up opens and swells, Down mirrors (brightness and level early vs late);
  Tone pitch at start/end by zero crossings (110 -> 220 Hz for +12 st); the Rhythm gate closed where it should be; a sample
  sweep Up peaks on the sample's click within the last 10 ms of the sweep, the tail swells before it, Reverse off plays it
  forwards; offline render length; the path in native content loads with the device; sweepFromClip places the riser
  8 beats before the clip on a new track and leaves the original alone; the VST3 plays a note, Load Sample, state with
  the path, Up/Down clicks; the chain face.
- PLG-23: every generator sounds without a sample and stays finite (with Chord and Air); types set everything but Length
  and Output and are recognised, a changed setting is no type; Preview starts and a second click stops; the VST3's full
  face with clicks on types, sources, Up/Down and Length, Preview sounding, a file dropped on the window
  (`sonora_sweep_plugin.png`); the compact face takes a Browser drop as one "Load Sample" edit (`sonora_sweep_chain.png`,
  `sonora_sweep_window.png`).
