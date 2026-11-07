# Native device faces

UI-33 / DEV-19, 2026-10-16. All graphics are authored Skia geometry using Sonora theme tokens.
The official [instrument reference](https://www.ableton.com/en/manual/live-instrument-reference/) and
[effect reference](https://www.ableton.com/en/live-manual/12/live-audio-effect-reference/) supplied public layout references.
Reference images were inspected outside the repository; no vendor artwork, preset, font, sample or code was copied in.

| Device | Current face and controls |
| --- | --- |
| Sonora FM | Four oscillator rows at left, each with coarse/fine/level dials and A–D selection. Selected waveform, editable amplitude envelope, ADSR, algorithm selector/routing, feedback, volume, bend, pan and transpose. |
| Sonora Resynth | Fixed 1080 × 368 editor (404-high dock, resize divider disabled): editable 96-partial spectrum, generated cycle, selectable/trimmed note regions; Sound, Envelopes and Motion & voices pages; explicit Synth/Hybrid/Reference and undoable Remove recording. Audio-clip conversion creates editable MIDI and the instrument together. [Usage and limits](RESYNTH.md). |
| Sonora Drum Rack (UI-66) | The Instrument Rack face (next row) with the 4×4 pads (note bank, chain names, chain table) where the chain list goes. Select a loaded pad to expose its child devices. Existing sample/device drops and chain edits retained. |
| Instrument / Audio Effect Rack (UI-66) | `RackFace` (ReferenceFace) matched to a 996 × 394 retina capture of the reference's "Ac Piano Upright" (body 342 px from y 46, `captureBody`): left strip (macros, + / − visible macros, variations [disabled, UI-66c], chain devices, chain list), macro boxes 100 × 160 on a 104 / 164 pitch with the `MacroColor` palette header (white text on dark shades), a 54-px knob and the value in the mapped parameter's own text (one continuous mapping; several or none show 0..127, On switches ignored). Chain list: auto-select [disabled, UI-66b], Key / Vel / Chain / Hide tabs, 36-px rows with chain colour and name, volume (fader law) and pan boxes, activator, exclusive solo (Cmd adds), hot-swap (UI-42b notice) and a post-fader meter, all live through `RackProcessor::chainMix`. Key / Vel / Chain open a 340-px zone editor (drag edges or the zone; Chain also drags the selector). Title: Rand (one undoable edit, `ExcludeMacroFromRandomization` respected) and Map (choose a macro, click a control of the shown chain; right click unmaps). Switches start from the set's `AreMacroControlsVisible` / `IsBranchesListVisible` / `IsRangesEditorVisible`. |
| Sonora Sampler | Sample, Filter / Env, Zones and Modulation tabs. Sample boundaries, loop, pitch/ADSR/filter and extra voice/LFO controls; editable zones. |
| Sonora Simpler (UI-39, DEV-4c) | Sample/Controls tabs in the title; vertical Classic/1-Shot/Slice. The sample row follows the mode: Classic Start / Loop / Length / Fade / LOOP / SNAP / Voices / Retrig; 1-Shot Start / Fade In / Length / Fade Out / SNAP / Trigger-Gate; Slice Start / Slice By / Length / Sensitivity-Division-Regions / SNAP / Playback / Trigger-Gate (+ Voices / Retrig with Poly) / Fade In / Fade Out. The display draws what the voices play (`Simpler::region`): flags at Start / Length (drag them), the loop with its brace and crossfade ramps, One-Shot fade ramps, slice lines (blue automatic, white manual), dimmed outside the flags, Cmd + wheel zoom. Filter / LFO / A-D-S-R / Volume bottom strip (Transpose instead of A-D-S-R in 1-Shot / Slice). Controls exposes modulation/pitch; Warp is disabled. No capture of the reference's modes yet: the One-Shot / Slicing rows follow the manual (DEV-4c). |
| Redux (DEV-20) | Quantization illustration, Rate/Jitter/Bits/Shape, pre/post filters and post octave, DC shift and Dry/Wet. |
| Tuner (DEV-20) | Live monophonic pitch, needle/target/strobe and history, Hz/cents, adjustable reference and right-click spelling. |
| Gate / Auto Pan (DEV-20) | Transfer/gain-reduction and paired LFO displays with actual detector, timing, floor, flip/listen/lookahead and stereo rate/phase controls respectively. |
| EQ Eight (UI-42) | Live 12 capture match: selected band's Freq / Gain / Q column, ±15 dB display (10 Hz–22 kHz, decade grid, cyan response, amber handles of the enabled bands), eight band cells (type chooser with band glyphs, on switch + number), Mode / Edit L-R or M-S / Adapt. Q / Scale / Output column. Drag a handle (Alt: Q), double-click switches a band off. DEV-2b adds a gray live output spectrum behind the response and an active analyzer toggle; the output level is the chain meter after the device (UI-55). |
| EQ Three (UI-42) | Live 12 capture match: band LEDs (not metered yet), GainLow/Mid/Hi with a dB law (-inf at the bottom, 0 dB at 0.9), L/M/H kills, FreqLow / FreqHi, 24 / 48 slope. |
| Compressor (UI-42) | Live 12 capture match: foldable sidechain strip (section folded by default; its filter is disabled until DEV-7b), Ratio (Expand shows the expansion ratio) / Attack / Release / Auto, display with Thresh and Out values, GR / Output legend, activity view (input, gain reduction, output history; drag the threshold line) and transfer view, Knee / Look. / Env. footer, Makeup, Peak / RMS / Expand, Dry/Wet. |
| Channel EQ (DEV-30) | `ChannelEq` matched to a 2000 x 231 capture of the reference's Channel EQ in the "Ac Piano Upright" rack (body 198 px from y 29, `captureBody`, 290 wide): display 20 Hz–20 kHz / ±12 dB grid with the live response curve over the output spectrum, the HP 80 Hz key (amber, high-pass glyph) and the amber Mid frequency readout (drag / wheel / double-click) with its pointer to the Mid knob; Low / Mid / High and Output knobs (dB-linear travel, a 16° slot at 0 dB, 9.5-unit triangle). Knobs following a rack macro are drawn as the reference does (grey ring, light triangle, green dot, `Knob::mapped`) and ignore hand edits. |
| Auto Filter (UI-43b) | `AutoFilter2` matched to a 914 × 382 capture (a retina zoom: body 342 px, `ReferenceFace` `captureBody`): folded sidechain strip (disabled, DEV-9c), large Freq knob (DJ knob for type DJ), type chooser with filter glyphs, 12 / 24 (a Morph box for type Morph), Res; response display (20 Hz–20 kHz, 196 px per decade, ±24 dB, the processor's own stage laws) with an amber frequency / resonance handle; a display bar for the last-touched modulator (LFO: Phase, stereo phase, offset, quantisation drawn disabled; Envelope: Attack / Release, Hold / S&H disabled); LFO Amt, Rate (Hz / time / synced by Mode), Wave glyphs, Morph (disabled), Mode; Envelope; Drive, circuit (SVF / DFM / MS2 / PRD), Clip, Output, Dry/Wet. |
| Limiter (UI-42) | Live 12 capture match: Input / Maximize, Release / Auto, Ceiling meters (per-channel output) and GR meters on the written 0–24 scale with the draggable ceiling pointer, ceiling value and Link, stepped Lookahead, Mode and Routing. Maximize, Soft Clip, True Peak and M/S are disabled (DEV-12c). |
| Wavetable (DEV-28b) | `InstrumentVector` import matched to a 2000 x 561 capture (197-px body, `ReferenceFace`): Sub, oscillator display over the installed reference library (category / table, stacked frames, gain / position bars, effect mode and FX, Semi / Det), both filters with routed response and handles, the modulation matrix (Matrix / MIDI / MPE, editable cells, add rows) and the global column; Osc / matrix tabs and the expand switch in the title. The expanded envelope / LFO view opens to the right of the body. docs/WAVETABLE.md. |
| Tension (DEV-28j) | `StringStudio` (`TensionFace.cpp`) matched to 1370 × 420 retina captures of the reference's two tabs (device x 18..1342, 34-px title, 342-px body from y 54, `captureBody`) and 24 variants with every parameter at k / 11. String / Filter-Global title tabs (the plain `CurrentTab`, saved with the set; the amber dot follows the filter switch) and the inert A \| A \| S letters. String: Exciter (type chooser renames the knobs: Bow Force / Friction with Damping greyed, hammers Mass, Plectrum Protrusion), Damper (Gated), Vel / Key boxes (cyan triangle when not neutral), Fix.Pos, the dark String / Vibrato strip (drag values), Termination, Pickup, Body (type / size, bipolar Str/Body). Filter / Global: filter display (response per type, formant U O A E I axis, cutoff / resonance handle), filter envelope (log-time stages, A / D+S / R handles), F. LFO (shape glyphs, Hz / sync), Pressure / Slide targets and amounts, Pitch Bend, keyboard, Portamento, Unison. Sections that are off draw their knobs grey and boxes unfilled, as the reference does. Value texts come from `tensionValueText` over the processor's laws. Small knob: 39-px ring open at the lower right, value 14 px right of the centre. |
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

A capture at another zoom passes its body height as `captureBody` (Auto Filter: 342); text sizes and panel corners keep
their physical size, so `kText` stays the 239-px value.

Matching a new device: get a capture of the reference device in the default light theme, measure boxes in its pixels (a
zoomed crop with a pixel ruler helps), write the face, render `ui_snapshot` with `--scale 239/<body height at scale 1>` and put
the capture over the render to review. Add the "reference faces" test
cases for reachability and gestures. ROADMAP UI-43 lists the existing devices still waiting for a capture; DEV-25–29 the
devices Sonora does not have yet.

## EQ Eight spectrum (DEV-2b, 2026-10-26)

Measured from a 1008 × 392 reference crop: spectrum inside the existing plot. The narrow stereo output rails
first drawn at x685/689 (face width 684) were removed the same day: every device's output level is now the chain meter
in the gap after it (UI-55), so the face is back to 675 capture pixels. The screenshot remains
outside the repository; all rendering is authored Skia geometry and theme colors.

The display reads **post-filter, post-output-gain, decoded L/R** audio. Each channel gets a 4096-point Hann FFT;
the filled envelope takes the stronger channel in each of 256 log-frequency cells (20 Hz–20 kHz, -90..0 dBFS).
This avoids cancellation of antiphase stereo. The ±15 dB labels still describe the **filter response**, not spectrum level.
`DeviceSpectrum` still tracks L/R peaks with hold (tested in `EQ display:`), but nothing draws them now.
Bypass/stopped processing fades the display to silence. The analyzer switch hides the spectrum and stops FFT work. Its state is local to the face
and is not a saved/automatable audio parameter. Audition and expanded view remain UI-42b.

`dsp/AudioDisplayTap` is a fixed 8192-frame stereo SPSC queue plus atomic peaks. The callback only captures samples
and peaks; overflow drops samples rather than waiting. A visible face renews a 250 ms capture lease; hidden/destroyed
faces stop renewing it. `dsp/DeviceSpectrum` drains and analyzes on the message thread, at the face's ~30 Hz tick.
Silence avoids FFTs/repaints; returning after a long gap or reprepare discards stale data. The EQ response path is cached
by size, selected set, sample rate and all parameter values, so spectrum animation never redesigns unchanged filters.

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
