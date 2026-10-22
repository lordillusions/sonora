# Sonora Resynth

Native stem-to-MIDI and resynthesis foundation (**RESYN-1**). Sonora-only device identity `SonoraResynth`.

## Stem → editable MIDI → instrument

1. Select an audio clip containing a dry, isolated bass or melody, with one note sounding at a time.
2. Choose **Create → Resynthesize to New MIDI Track**, or the same command in the clip context menu.
3. Sonora analyzes the audible clip range and inserts a new MIDI track immediately after the source track.
   The new track contains an editable MIDI clip and Sonora Resynth with the analyzed sound. The source remains available.
4. Play the result. Edit the MIDI notes in the normal piano roll to change pitches, timing and durations.
   Solo the source/result tracks separately when comparing them.
5. Save the set. The analysis and mono source reference are embedded in the device's versioned state;
   reopening, Save As and Collect All and Save do not require the original WAV.

Arrangement results retain the source clip's Arrangement start. Session results occupy the same scene on
the new track. Take-lane sources create a new Arrangement result at their clip start. The new track retains
the source's parent group and colour. One Undo removes the result; Redo restores it.

For an instrument without an existing audio clip, add **Instruments → Sonora Resynth** from the browser.
Choose **Load Audio...** or drop an audio file onto its panel. After analysis, **Create MIDI** creates a
new track with the analyzed phrase and a copy of the instrument, starting at the transport position.
Analysis runs on a cancellable worker, using the existing media-job modal; playback stops for the job.
Replacing the analysis is one structural Undo. A failed/cancelled analysis retains the previous device.

## Sound generation and source-free patches

Synth playback creates the signal from evolving harmonic coefficients and generated, filtered noise.
It does not play the recorded waveform. Analysis extracts up to 96 complex partials per frame, rather
than finding the original synth preset or recovering its oscillator/filter settings. The model is an
approximation of the recording. The number of front-panel knobs does not describe the model size.

**Remove recording** removes all reference PCM from the instrument in one undoable edit, retaining the
spectral model, duration and note regions. The badge changes to **MODEL ONLY** and Hybrid/Reference become
unavailable. Synth produces identical samples before and after removal. Save/reopen and Create MIDI
continue to work without the recording. Undo restores the embedded reference. This removes the recording
from this instrument; an original audio track elsewhere in the set remains an independent track.

## Device controls

The editor has a fixed 1080 × 368 logical-pixel content size (404-high dock including the title and margins).
Its divider is disabled; window resizing and saved clip-editor heights do not resize it. Narrow windows
scroll the device chain horizontally. Other editors retain their own saved sizes.

The expanded editor separates sound, envelopes and motion into three pages. The spectrum, generated-cycle
preview and note regions remain visible on each page. Grey bars show captured partial magnitudes; coloured
bars show the shaped result. Drag a harmonic vertically to adjust its gain (0–200%); Shift is fine and
double-click/Alt-click resets it. **Reset spectrum** restores all individual gains; the harmonic group
controls remain as set. The waveform preview is calculated from partials, never sampled from the recording.

- **Synth**: evolving harmonic tables or edited additive partials, plus four-band modeled noise.
- **Hybrid**: Synth blended with a short, fading recorded attack (60 ms maximum). **Sampled attack** sets its amount.
- **Reference**: the original mono note region, pitched with the played MIDI note, for comparison.
- **Sound / Harmonics**: fundamental, odd/even harmonic levels, spectral tilt in dB/octave, partial count
  and an independently generated sub sine one octave below. Individual partial edits combine with these controls.
- **Modeled noise**: master amount and four independently scaled residual bands (low, mid, high, air).
  New patches start with the master at **0%**. Existing saved amounts remain explicit. Independent
  noise excitation keeps the combined RMS power calibrated instead of coherently boosting crossovers.
- **Dry bass** (Sound page): switches to Synth, mutes modeled noise and sampled attack, and sets a
  30 ms amplifier release. This suppresses synthesized hiss and lingering note releases. Tonal room
  sound captured in the harmonic model can remain; this is not full dereverberation.
- **Filter**: on/off, low-pass/high-pass/band-pass/notch, cutoff, resonance, bipolar envelope-to-cutoff
  amount and pre-filter soft drive. **Amplifier**: attack/decay/sustain/release, velocity response and output gain.
- **Envelopes**: independent amplifier and filter ADSR controls with graphs that follow their settings.
- **Motion & voices / Timbre scan**: start/end positions and speed through captured frames; evolve/hold,
  loop or freeze. **Pitch contour** reproduces the example's continuous pitch relative to its rounded root.
- **LFO**: six shapes, rate/depth and destinations pitch (±12 semitones at full depth), timbre scan,
  cutoff (±48 semitones), amplitude or stereo pan. Tempo sync interprets rate as cycles per beat.
- **Voices & pitch**: Mono, Legato or Poly 8; glide, bend range (0–24 semitones), root correction.
  Root correction changes the assumed root: increasing it makes played output lower.
- **Audition** toggles a preview note; clicking again or leaving the panel releases it.
- Click a note region to select/pin an example. **Auto key map** chooses the example nearest each played
  pitch. The readout shows its analyzed MIDI pitch. Detection confidence is not a fidelity score.
- Drag a selected region's edges inward to trim it. The harmonic/noise frames are interpolated; reference
  bounds and subsequently created MIDI follow the trim. Undo restores a larger range. Trimming does not
  refit pitch detection. A model-only patch shows its harmonic amplitude contour in place of PCM.

Generated phrase tracks enable Pitch contour and use a 100% hold position. Normal imported instruments
default to MIDI pitch and a 75% hold. Generated MIDI contains discrete notes; detailed slides/vibrato remain
in the model and can be disabled with Pitch contour. They are not separately editable MIDI bend lanes.

Generated phrase clips also contain a **CC119 envelope** that selects each captured note example at its
note-on, preserving different attacks/dynamics when the source repeats the same pitch. CC119 value 0 clears
the override; 1–64 selects an example. Remove/edit this envelope in the normal MIDI controller editor when
rearranging the phrase if you prefer automatic key mapping. Source-waveform playback is still disabled in Synth.

Playback has eight fixed voices, channel-aware notes, adjustable MIDI bend, sustain pedal and channel
panic/reset. Mono releases the preceding voice; Legato retains a held voice's envelope/scan on overlapping
notes. Mono/Legato held-note fallback selects the highest remaining held key on that channel. Poly 8 plays
independent notes and steals the oldest voice at the bound. Input transcription remains monophonic.

## Model and limits

Audio input is limited to **0.08–30 seconds**, mono/stereo, **8–96 kHz**, at most **64 detected regions**.
Longer sources are not refused silently: a clip or file over 30 s opens a prompt that names its length and offers
**Use First N bars** (the last bar line inside 30 s; Arrangement prompts suggest Split Clip ⌘E for the rest) or
**Use First 30.0 s** for a file. A source with more than 64 detected notes fails after pitch tracking with the note
count and offers **Use First 64 Notes**, cut midway between the 64th and 65th notes so no kept note is truncated.
Cancel leaves the set unchanged. The picker/browser accept the audio formats already supported by Sonora, including WAV. Stereo analysis
chooses the higher-energy channel, avoiding cancellation when channels are phase opposed. The modeled
timbre is mono; LFO pan can distribute it to stereo outputs. Very short/quiet/unvoiced material may have no usable pitch and is refused.

Worker analysis uses filtered pitch decimation and continuous periodicity estimates, note segmentation,
phase-aligned harmonic projection (96 harmonics), evolving amplitude/phase data and four-band residual
energy measured after subtracting the reconstructed tonal waveform (bounded by each source band),
rather than subtracting unrelated spectral powers. Playback compiles immutable band-limited harmonic tables before processing. Spectral edits use direct
additive synthesis with a Nyquist taper; no audio-thread table rebuilding or allocation is required. It uses no
cloud service, neural model or reference application's code/assets. The residual model is approximate;
it measures filtered residual energy after reconstructing the harmonics, rather than retaining exact residual audio.

The embedded `sonora-resynth-v2` payload includes duration, optional reference PCM, note/frame data and a
checksum. Version 1 patches remain readable. Removing PCM preserves duration and the complete spectral model.
Counts, ranges, finiteness, ordering and version are validated before table preparation; prepared data is capped
at 128 MiB. Invalid/unsupported state produces a warning and a silent device while the rest of the set loads.
Models and tables are immutable during playback. Structural replacement uses the existing native-device graph
lifecycle. The audio callback only uses fixed voice/controller arrays, atomics and prepared tables/filters.

This is an initial musical approximation. Mixed/polyphonic sources, reverb, strong stereo unison, noise,
distortion and fast articulations can produce wrong notes or a poor sound match. No original synthesizer
patch or effect chain is recovered. Separate same-pitch attacks without a clear gap, automatic correction of
octave errors, dedicated patch-file browsing, separately editable MIDI pitch curves,
external AU/VST3 packaging and real-corpus listening/CPU qualification remain **RESYN-1d**.
Save/reopen of the instrument in Sonora is supported; other `.als` readers do not implement this custom device.

## Verification tool

```sh
build-ui-review/tools/resynth_probe --demo /tmp/sonora-resynth-review
build-ui-review/tools/resynth_probe /absolute/bass.wav /tmp/my-resynth-review
build-ui-review/tests/sonora_tests 'resynth'
build-ui-review/tools/ui_snapshot /tmp/resynth.png /tmp/sonora-resynth-review/Resynth.als --no-browser --track 1 --size 1500x700
```

The probe creates original, Synth, Hybrid, Source, octave-up, source-free and edited-synth WAVs, `phrase.mid` (notes and CC119 example choices at 120 BPM),
the embedded analysis payload, playable `Resynth.als` and `Resynth-SourceFree.als` sets, and a JSON timing/region report including payload
size, edited-spectrum render timing, and the maximum Synth sample difference after removing PCM (must be zero). Generated audio is
authored by Sonora and uses no third-party content. Synthetic checks establish behavior on those fixtures;
they do not establish perceptual equivalence for arbitrary bass recordings.
