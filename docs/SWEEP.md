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

## Reverb swells (PLG-19d, 0.1.52)

Owner (2026-11-18): bring Sweep closer to a commercial "reverse reverb swell" plug-in and the technique in its demo video.
The technique (decades old, nothing product specific): render a sound's reverb **on its own** (wet only), **reverse**
it, and place it so it rises out of silence and ends exactly where the dry sound starts: the sound breathes in before
it hits (vocal swells, snare pre-verbs). What that category of tools does, and what Sweep lacked before 0.1.52:

| Feature | Sweep 0.1.51 | 0.1.52 |
|---|---|---|
| Only the reverb tail reversed (the dry sound is not in the swell) | no: the whole sample reversed + a reverb under it | **Reverb Swell** mode (default); the old one stays as **Reversed Sample** |
| Several reverb characters, blended on an XY pad | one small Freeverb | **Hall** (8-line FDN), **Shimmer** (octave up), **Bloom** (spectral smear), **Grain** (grain cloud), XY blend |
| Tail length tied to the swell length | Tail 1-5 s | **Fit** (decay = 1.2 x Length at the tempo) or Decay 0.4-12 s |
| Tone of the tail | no | **Damp** |
| The dry hit after the swell | no | **Hit** (at the drop; Down: with the note, then the tail = a reverb throw) |
| Use part of the sample | no | **Region** (two handles on the waveform; a swell uses up to 12 s) |
| Get the result out as audio | Clip menu only | **Save WAV...** and **drag the display** onto a track (app) or into the host (VST3) |
| One-click starting points | 10 synth types | + Vocal Swell, Snare Pre-Verb, Shimmer Rise, Spectral Bloom, Grain Cloud, Reverb Throw |
| Capture the track's own audio, MSEG shape, export forward + reversed | no | not yet: PLG-19e |

Engine: `src/devices/native/SweepSwell.{h,cpp}` (offline, message thread; each character is normalised to the same
loudness before the blend, the blend to the dry peak). The Sample is prepared again when a swell setting, the Region,
Length or the tempo (with Fit) changes: the face does it once the settings rest, the VST3 from its 20 Hz timer.
Prepared buffers are freed only a second after a newer one replaced them (the audio thread may still be loading the old
one). Measured (`sonora_tests "sweep swell"`, Debug): each character renders a 2.5 s tail in 4-23 ms; a 4 s Fit swell of a
0.5 s hit: RMS per second 0.003 / 0.013 / 0.057 / 0.213, the last 100 ms 0.452, silence after the drop with Hit off, the
click within 1 ms of the drop with Hit on; Shimmer puts 75 % of the 100 Hz-10 kHz energy at the octave of a 440 Hz
burst (Hall 0 %); Damp 1 cuts the energy above 4 kHz by >30 % in every character; Region 0.5..1 plays the second half's
1 kHz (998 Hz measured).

## Types, Preview

`Sweep::types()`: White Riser, Pink Wash, Supersaw Rise, Tension Gate, Whoosh Up, Reverse Crash, Sub Drop, Downlifter,
Laser Down, Reverse Sample. A type sets every parameter but Length and Output (`applyType`); `matchingType` tells which
one the current settings are (the face lights it). **Preview** (`Sweep::preview`, the face's button) plays one sweep at
C3 (C2 for Sine) without MIDI; a second click stops it with a short fade.

## Face (`src/ui/views/SweepFace.cpp`, ProFace kit: docs/PRO_FACES.md)

Full (1180 x 760, PLG-19d): what it makes ("Reverb swell . 2 Bars . 4.0 s at 120 BPM"), Save WAV... and Preview; TYPE in
two groups (REVERB SWELLS of a sample, SYNTH SWEEPS); SAMPLE (name, the waveform with the Region's two handles, Choose...,
drop); the display: the sweep itself rendered offline (22.05 kHz, time compressed to 6 s for long sweeps, re-rendered
when the settings rest for 4 ticks) under the filter / pitch / level curves, the bars, the drop (+ HIT) marked, the play
head; drag it out to get the WAV; SOURCE, Up/Down, LENGTH; SWELL (the character pad with each corner's share, Decay,
Damp, Hit, Reverb Swell / Reversed Sample, Fit to length, Reverse; veiled with what to do over a generator) and FILTER
(Low, High, Reso, Air); PITCH (Pitch, Detune, Chord), SHAPE (Curve, Swell, Width, Rhythm), OUT (Level); the help line.
Compact (the chain, 720 x 170): the display, the sources, Up/Down, Preview, Length, Swell, High, Level.

**Samples from the Browser**: drop a sample from Sonora's Browser (or the Finder) anywhere on the face: the path goes into
the device's native content as one undoable edit ("Load Sample", `ui::loadSweepSample`) and Source becomes Sample. That
edit rebuilds the device chain while the drop is running; 0.1.51 crashed there (the face repainted itself after it was
deleted, PLG-19c; docs/PRO_FACES.md "Callbacks that edit the app"). In the
VST3, drop a file on the window or use Choose....

## Clip > Make Riser / Make Downlifter from Clip (`src/engine/SweepFromClip.cpp`)

Right-click an audio clip in the arrangement: 1, 2, 4 or 8 bars. The clip's sample (first 20 s; a reverb swell uses the
first 12 s) is rendered by the same device offline at the set's tempo (Low 400 Hz, High 20 kHz, Swell 70 %, Output 0 dB;
since 0.1.52 a Reverb Swell fitted to the length: the clip's own reverb reversed into the clip, or thrown after it) into
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
- PLG-19c: `sonora_tests "sweep: a Browser drop on the chain face survives"` (editDevice rebuilds the chain like the app;
  segfault, exit 139, before the fix); app autotest `SONORA_SWEEP_DROP_AUTOTEST="0:/path/sample.wav"` with
  `SONORA_DATA_FOLDER` (two drops on the real face: 0.1.51 exit 139, now "survived; Source Sample ... undo Load Sample").
- PLG-19d: `sonora_tests "sweep swell"` (above), the VST3's full face by mouse (Vocal Swell, the pad, the Region handle,
  `sonora_sweep_plugin_swell.png`), the window face's drag-out (a sample drag of `Samples/Processed/Sweep/Sweep riser.wav`).
  `fx_render "<path>/Sonora Sweep.vst3" --in silence.wav --out riser.wav --tempo 120 --note 60` (`--note` is new): the
  25 parameters, a 4-bar riser of exactly 8.000 s rising every second.
