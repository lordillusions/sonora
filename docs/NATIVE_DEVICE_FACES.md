# Native device faces

UI-33 / DEV-19, 2026-10-16. All graphics are authored Skia geometry using Sonora theme tokens.
The official [instrument reference](https://www.ableton.com/en/manual/live-instrument-reference/) and
[effect reference](https://www.ableton.com/en/live-manual/12/live-audio-effect-reference/) supplied public layout references.
Reference images were inspected outside the repository; no vendor artwork, preset, font, sample or code was copied in.

| Device | Current face and controls |
| --- | --- |
| Sonora FM | Four oscillator rows at left, each with coarse/fine/level dials and A–D selection. Selected waveform, editable amplitude envelope, ADSR, algorithm selector/routing, feedback, volume, bend, pan and transpose. |
| Sonora Resynth | Fixed 1080 × 368 editor (404-high dock, resize divider disabled): editable 96-partial spectrum, generated cycle, selectable/trimmed note regions; Sound, Envelopes and Motion & voices pages; explicit Synth/Hybrid/Reference and undoable Remove recording. Audio-clip conversion creates editable MIDI and the instrument together. [Usage and limits](RESYNTH.md). |
| Sonora Drum Rack | 4×2 macro bank at left, 4×4 pads with note bank and chain names, chain table to the right. Select a loaded pad to expose its child devices. Existing sample/device drops, macro mapping and chain edits retained. |
| Sonora Instrument Rack | 4×2 macro bank, draggable/wheel chain-selector readout and chain/zone table. Macro bank button reaches macros 9–16. |
| Sonora Sampler | Sample, Filter / Env, Zones and Modulation tabs. Sample boundaries, loop, pitch/ADSR/filter and extra voice/LFO controls; editable zones. |
| Simpler (UI-39) | Sample/Controls tabs in the title; vertical Classic/1-Shot/Slice; cached precise waveform with timestamps/markers; real sample-region readouts, loop/voices/retrigger; filter/LFO/ADSR/volume bottom strip. Controls exposes modulation/pitch; Warp is disabled. |
| Redux (DEV-20) | Quantization illustration, Rate/Jitter/Bits/Shape, pre/post filters and post octave, DC shift and Dry/Wet. |
| Tuner (DEV-20) | Live monophonic pitch, needle/target/strobe and history, Hz/cents, adjustable reference and right-click spelling. |
| Gate / Auto Pan (DEV-20) | Transfer/gain-reduction and paired LFO displays with actual detector, timing, floor, flip/listen/lookahead and stereo rate/phase controls respectively. |
| EQ Eight (UI-42) | Live 12 capture match: selected band's Freq / Gain / Q column, ±15 dB display (10 Hz–22 kHz, decade grid, cyan response, amber handles of the enabled bands), eight band cells (type chooser with band glyphs, on switch + number), Mode / Edit L-R or M-S / Adapt. Q / Scale / Output column. Drag a handle (Alt: Q), double-click switches a band off. |
| EQ Three (UI-42) | Live 12 capture match: band LEDs (not metered yet), GainLow/Mid/Hi with a dB law (-inf at the bottom, 0 dB at 0.9), L/M/H kills, FreqLow / FreqHi, 24 / 48 slope. |
| Compressor (UI-42) | Live 12 capture match: foldable sidechain strip (section folded by default; its filter is disabled until DEV-7b), Ratio (Expand shows the expansion ratio) / Attack / Release / Auto, display with Thresh and Out values, GR / Output legend, activity view (input, gain reduction, output history; drag the threshold line) and transfer view, Knee / Look. / Env. footer, Makeup, Peak / RMS / Expand, Dry/Wet. |
| Limiter (UI-42) | Live 12 capture match: Input / Maximize, Release / Auto, Ceiling meters (per-channel output) and GR meters on the written 0–24 scale with the draggable ceiling pointer, ceiling value and Link, stepped Lookahead, Mode and Routing. Maximize, Soft Clip, True Peak and M/S are disabled (DEV-12c). |
| Sonora Vector | Compact wavetable instrument: oscillator tabs and generated waveform stack, filter curve and controls, amp envelope/LFO tabs, sub/volume/pan. Drag stack position, filter cutoff/resonance or amp decay/sustain. |
| Sonora Impulse | Eight named sample slots across the top, click to select/audition. Start/tune/drive/filter/decay/pan/gain below, master at right. Drop onto any slot, including an empty slot eight; storage stays sparse. |
| Sonora Echo | Independent L/R time and sync controls left; Echo/Modulation/Character tabs above tunnel and filter; input/feedback below, routing/output/dry-wet right. Drag the tunnel for left time, Shift-drag for right. |

Stock device parameters remain reachable at the normal compact height; Resynth uses an expanded editor. Continuous controls drag/wheel, Shift is fine,
double-click resets, choices open a menu, and horizontal swipes bubble to chain scrolling. Right presses do not edit.
Top-level parameter widgets retain MIDI target identities; nested dials retain existing rack macro mapping.
Structural edits still go through `UiContext::editDevice`, which owns undo, source retention and safe graph replacement.

## Face kit (UI-42): Live 12 faces from a capture

`src/ui/views/ReferenceFace.{h,cpp}` is the base for faces matched to a Live 12 capture; `EffectFaces.cpp` holds EQ Eight,
EQ Three, Compressor and Limiter. A face is written in the capture's pixels (x from the device's left edge, y from the top of
its body, body `kBodyHeight` = 239 px) and scales uniformly with the body height; the device box gives the face its whole
body under the title band and asks it for its width (`preferredWidth(bodyHeight)`).

- Controls: `knob` (Live 12 device knob, `Knob::Style::Device`, with a `Law` between the plain value and the 0..1 travel:
  linear, logarithmic, power or custom), `slider` (light box with the cyan fill), `chooser` (drop-down, optional glyph such as
  `eqBandIcon`), `displayValue` (grey label + cyan value on a display), `toggle` / `option` (amber), `button`, and
  `inertButton` / `inertKnob` for controls Sonora does not implement (drawn, disabled, tooltip with the task ID).
- Painting: `fillBody` (body, or the title colour when the reference splits the body into panels with gutters), `panel`,
  `displayPanel`, `label`, `led`; `collapseColumns` folds a column range (Compressor's sidechain section).
- Every device title ends with the hot-swap / save / menu circles (`titleIcon`). The menu circle opens the device menu (Delay and
  Reverb: the full parameter set). Hot-swap and preset saving are not implemented (status-bar notice, UI-42b).
- Text: 14.4 capture px regular, 15.6 bold, the Utility fit. The reference font is not shipped; macOS draws SF Pro, Linux DejaVu.

Matching a new device: get a capture of the reference device in the default light theme, measure boxes in its pixels (a
zoomed crop with a pixel ruler helps), write the face, render `ui_snapshot` with `--scale 239/<body height at scale 1>` and put
the capture over the render to review. Add the "reference faces" test
cases for reachability and gestures. ROADMAP UI-43 lists the existing devices still waiting for a capture; DEV-25–29 the
devices Sonora does not have yet.

## New audio implementations

`SonoraVector` uses four independently generated basic waveform frames and the existing immutable mipmap loader.
Two oscillators, sub, sixteen voices, per-channel bend/sustain, ADSR, a resonant multimode filter and position LFO
run in fixed storage. MIDI timestamps split rendering; no strings, allocation, locks or logging in processing.
The larger existing `SonoraWavetable` and its floating editor keep their separate identity and behavior.
Browser search for Wavetable finds both; search for Operator also finds Sonora FM.

`SonoraImpulse` maps its eight slots to MIDI 60, 62, 64, 65, 67, 69, 71, 72. Slots are keyed by sample root note,
not vector index. Thirty-two voices use immutable SampleSlots, linear resampling, exponential decay, per-slot
pitch/start/level/pan/saturation and low-pass filter. Note-offs leave one-shots ringing; channel panic stops them.
Click audition crosses an atomic request; GUI code never edits audio-owned voices.

`SonoraEcho` uses two prepared fractional delay rings, time smoothing, stereo/ping-pong/M-S routing,
signed feedback, serial high/low-pass filtering, delay-time modulation and input-driven ducking.
Tempo publication is atomic because both UI and audio read timing. Filter drawings are qualitative responses;
the tunnel is a timing visualization, not an audio waveform or reverb simulation.

All three use Sonora-only XML tags and authored parameters. The existing generic writer, state capture and undo
machinery retain them. Sample refs use the same `Player/MultiSampleMap/SampleParts` container as Sonora's sample
authoring; `SonoraImpulse` is explicitly included in the reader. Existing imported native tags remain unchanged.

## Limits

The faces follow the reference's broad arrangement and interaction vocabulary. Pixel equality has not been
established, and unsupported DSP controls are not represented as working controls. UI-33b and DEV-19b track
remaining controls, reference-specific subviews and import/audio parity. Imported Operator, Wavetable, Impulse
and Echo are not silently mapped to these new implementations. Simpler onset slicing now works; warping, original slice-metadata compatibility and fuller Sampler synthesis
remain device follow-ups. Compressor/Limiter lookahead remains read at prepare time, as before this pass;
changing its readout requires device reprepare before the new latency takes effect.

Verification sets, generated samples and source references are disposable; real-world projects were not modified.
