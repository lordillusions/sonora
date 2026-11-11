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

Sources: **Noise** (white, independent per side), **Tone** (five detuned polyBLEP saws at the note, Detune), **Sample**.
A loaded sample is prepared on the message thread: resampled to the device rate, + a Freeverb-style reverb tail
(**Tail**: 1-5 s), reversed for Up when **Reverse** is on. For an Up sample sweep the start position is computed (the
integral of the rising playback speed over the sweep) so the reversed audio ends on the sample's attack exactly at the
sweep's end: reverse chord going up, reverse crash, reverse vocal. Prepared buffers are swapped to the audio thread
lock-free (the audio thread acknowledges what it uses; older ones are freed on the message thread).

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
