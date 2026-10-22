# Native Delay and Reverb (DEV-8c, DEV-6c)

Sonora's `Delay` and `Reverb` (`src/devices/native/{Delay,Reverb}.*`) follow Live 12's devices **parameter for parameter**.
The DSP is Sonora's own and is **not** sample-identical to Live's: Live's algorithms are proprietary, no Live code is used
or copied, and no Live audio could be rendered for comparison (see "Not verified").

## Evidence used

| Source | What it gave |
|---|---|
| `Live.app/Contents/App-Resources/Schema/12.0_12402.txt` (`<Delay>`, `<Reverb>`) | The complete element list: every parameter id the device stores. Both devices now carry **all** of them (33 each), so authored devices are complete and re-saved sets keep every field. |
| `Core Library/Defaults/Audio Effects/{Delay,Reverb}.adv` (read only) | Live's own default value of every parameter. `tests/test_delay_reverb.cpp` pins them. |
| 65 real-world sets (171 Delay, 299 Reverb instances; `scan.py`-style walk of the XML) | Observed value ranges, which enum values occur, and semantics (below). Ranges below say "observed" when taken from here. |
| Live 12 manual, Audio Effect Reference (Delay, Reverb) | Behaviour and enum names: Repitch/Fade/Jump, LFO modes and waves, Equal-Loudness, Freeze/Flat/Cut, Spin, Shape, Smoothing, Density, Stereo 0..120. |

## Facts established from real files (and bugs they exposed in the earlier implementation)

- **`DelayLine_SyncedSixteenthL/R` is a zero-based index** (default file: L = 2, R = 3 → 3 and 4 sixteenths). The old code read it
  as 1..16 sixteenths, so every imported synced Delay was one sixteenth short.
- **`DelayLine_OffsetL/R` is a fraction** (±0.33 = ±33 %); real sets store e.g. `0.026`. The old code treated it as percent.
- **Link (measured against the reference, DEV-22b):** sets with Link on may store different L and R times, but
  the reference *plays* the left side's settings on both lines. The UI still applies an edit to both sides
  (`Delay::setParameterLinked`).
- **Free times are `DelayLine_TimeL/R` in seconds** (measured: a probe with TimeL 0.2 s / SimpleDelayTime 100 ms repeats at
  200 ms; the Ambient Spaces presets' 4.9-22.9 ms combs). `DelayLine_SimpleDelayTimeL/R` (ms) is stored but not played.
  `DelayLine_PingPongDelayTimeL/R` is 1.0 in all 171 instances; `DelayLine_CompatibilityMode` is 0..2; both are kept but unused.
- `DryWetMode` default is 1 (manual: "Dry/Wet – Equal-Loudness"); older sets store 0 (Classic).
- **Reverb `MixDirect` is the dry/wet amount, 1.0 = fully wet**: all 31 reverbs on return tracks store exactly 1.0 (settles the
  DEV-6b question). Default is 0.55.
- Real Reverb values exceeded the old parameter ranges and were clamped on load (`ShelfLoFreq` up to 11 962 Hz, `ShelfHiGain`
  down to 0.2, `DecayTime` up to 24 264 ms). Ranges were widened. The old UI also printed Stereo as "10000 %".
- The old Reverb ignored Spin, Shape/DiffuseDelay, AllPassGain/Size, FlatOn, CutOn, SizeSmoothing, RoomType (Density),
  HighFilterType and StereoSeparationOnDrySignal, and muted the whole reverb input while frozen.

## Delay

Parameters (id → Sonora label, range, default). `*` = range taken from observed values or assumed, not from Live.

| Group | Parameters |
|---|---|
| Time | `DelayLine_SyncL/R` (default on), `DelayLine_SyncedSixteenthL/R` index 0..15 (2 / 3), `DelayLine_SimpleDelayTimeL/R` 1..5000 ms (100), `DelayLine_OffsetL/R` ±0.33 (0), `DelayLine_Link` (on), `DelayLine_PingPong` (off), `DelayLine_SmoothingMode` Repitch/Fade/Jump (Repitch) |
| Filter | `Filter_On` (on), `Filter_Frequency` 20..18000 Hz* (1000), `Filter_Bandwidth` 0.5..9 octaves* (8) |
| LFO | `Modulation_TimeMode` Rate/Time/Synced/Triplet/Dotted/16th, `Modulation_Frequency` 0.01..40 Hz* (0.5), `Modulation_Time` 0.025..100 s* (1), `Modulation_SyncedRate` 0.25..64* (4), `Modulation_Sixteenth` 1..64* (16), `Modulation_Waveform` Sine/Triangle/Ramp Up/Ramp Down/Square/S&H/Wander (1), `Modulation_Morph`, `Modulation_AmountTime`, `Modulation_AmountFilter` (all 0..1) |
| Output | `Feedback` 0..1 (0.5), `Freeze`, `DryWet` (0.5), `DryWetMode` Classic/Equal Loud (Equal Loud), `EcoProcessing` |

Behaviour implemented:
- Each side is synced (index + 1 sixteenths at the host tempo) or free (ms); offset multiplies the time by `1 + offset`.
- The band-pass is **in front of the delay line, inside the feedback loop**, so even the first echo is filtered (the manual:
  "band-pass filter before the delay"). Width is octaves → Q = 1 / (2 sinh(ln2/2 · width)); gain is 0 dB at the centre.
- Repitch glides the delay (1-pole, 70 ms, at most 0.5 sample per sample, i.e. a tape-style pitch change); Fade cross-fades two
  taps over 60 ms; Jump switches at once.
- Ping Pong feeds the (mono-summed) input to the left line only and cross-feeds the two lines; Freeze loops the buffer at 100 %
  feedback, bypasses the filter and ignores the input.
- LFO: seven waves, Morph, free (Hz / seconds) or tempo synced (note denominator, triplet ×2/3, dotted ×1.5, or N sixteenths);
  synced modes lock to the song position while the transport plays. It modulates the delay time and the filter frequency.
- Dry/Wet Classic = linear cross-fade; Equal Loud = constant power (sin/cos). Eco updates the filter every 32 instead of 8 samples.

## Reverb

Sections follow Live's device: Input Processing (Low Cut, High Cut, Freq, Width, Predelay), Early Reflections (Spin, Rate,
Amount, Shape), Diffusion Network (High/Low shelves + High type, Chorus, Decay, Freeze/Flat/Cut, Size, Smooth, Diffusion, Scale,
Density, Stereo), Output (Reflect, Diffuse, Stereo-on-dry, Dry/Wet). Ids and defaults: `tests/test_delay_reverb.cpp`.
Ranges taken from observed values: `RoomSize` 0.22..500, `DecayTime` 200..60000 ms, `StereoSeparation` 0..120, `BandFreq` 50..18000,
`BandWidth` 0.5..9, `ShelfHiGain` / `ShelfLoGain` 0.2..1, `MixReflect` / `MixDiffuse` 0..2 (linear, shown in dB); others are assumed
wide enough to hold every observed value.

Behaviour implemented:
- Pre-delay → low/high cut at `BandFreq / 2^(w/2)` and `BandFreq · 2^(w/2)` → stereo early reflections (8 taps per side; Spin
  modulates the tap times with opposite sign left/right; Shape shortens the reflection decay and delays the diffuse onset by up to
  40 ms) → 1..4 series all-pass diffusers (Density, gain = Diffusion, length ∝ Scale · Size) → 8-line FDN with Hadamard mixing.
- RT60 = Decay: per-line gain `10^(-3·len/RT60)`; damping per pass by the High shelf (1-pole split, HF gain = `ShelfHiGain`) or, for
  the "Cut" type, a steeper 2-pole low-pass, and the Low shelf; Chorus modulates the line lengths by ±`SizeModDepth` %.
- Freeze: loop gain 0.99998; Flat bypasses the shelves while frozen; Cut stops the input reaching the network (otherwise the input
  keeps adding, through a loop gain of 0.9998 so it stays bounded). Early reflections keep following the live input.
- Size Smoothing: None applies a new Size at once, Slow / Fast glide the line lengths (400 ms / 60 ms time constants).
- Stereo is a mid/side width on the wet signal (and on the dry signal when `StereoSeparationOnDrySignal` is on); Dry/Wet is a
  linear cross-fade by `MixDirect`.

## Sonora's own numeric choices (not documented by Live, not measured)

Repitch glide speed, Fade length, LFO depth scaling (time: `amount · 0.5 · min(time, 20 ms)`; filter: `amount · 2` octaves),
Morph definitions, Synced-rate meaning (`period = 4 / rate` beats), the `HighFilterType` labels (Shelf / Cut) and `RoomType` labels
(Sparse / Low / Mid / High) with their mapping (1..4 diffusers), Spin depth (`amount · 0.05 ms`), early-reflection tap pattern and
decay law, Shape law, all-pass lengths, FDN line lengths, Stereo/dry-wet laws, Eco meaning, and the ranges marked `*`.

## Not verified

- No Live audio was rendered or compared. Access to the running Live 12 was declined for this session, so neither the
  Live Object Model (exact min/max/units via `DeviceParameter`) nor an Export Audio/Video of test signals could be used.
- Authored Delay/Reverb devices have not been opened in Live (they contain every schema element, but Live was not available).
- No listening test, no hardware/native-window gestures.

## How to close the gap (needs Live running and driven)

1. A small own Live control-surface script can dump every parameter's `min`, `max`, `default_value`, `value_items` and
   `str_for_value()` curve for both devices → exact ranges, units and enum labels.
2. A probe set with an impulse / noise / sine clip through each device at chosen settings, exported with File > Export
   Audio/Video, gives reference renders; compare decay time, early-reflection timing, filter responses and LFO depth with
   `engine_probe --render` and fix the "own numeric choices" above one at a time.
