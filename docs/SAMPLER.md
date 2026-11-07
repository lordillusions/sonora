# Sonora Sampler (`MultiSampler`) — parameters, laws, round trip (DEV-14b)

Sonora plays the reference's Sampler with `devices::Sampler` (`src/devices/native/Sampler.{h,cpp}`). The reference's
Sampler is its Simpler engine with every control exposed (the XML of both devices has the same classes:
`SimplerPlayer`, `SimplerPitch`, `SimplerFilter`, ...), so the Sampler reuses Simpler's measured laws (DEV-22:
playback kernel, filter circuits, envelope slopes, LFO shapes, velocity / key / pan laws) through
`Simpler::parameterInfos()`, `simplerFilterStages()`, `Simpler::readSample()` / `saturate()` and the envelope constants,
and adds everything only the Sampler has.

Sources used: the reference manual (Live 12, chapter "Sampler"), the schema
(`App-Resources/Schema/12.0_12402.txt`: `MultiSampler`, `SimplerPlayer`, `SimplerAuxLfo`...), the 76 factory Sampler
presets and the default preset `Defaults/Instruments/Sampler.adv` (read at run time, never copied), 34 more Sampler
devices inside factory racks (matrix rows), and the captures in `/private/tmp/Sonora Parity/SamplerCaptures`
(Lovely Keys, every tab). **There are no reference recordings of the Sampler yet**: every law below that is not
inherited from Simpler is marked *unverified* and carries `TODO(DEV-14b): law unverified` in the code.

## Parameter model

`Sampler::parameters()` = Simpler's list (indices `0 .. Simpler::NumParams - 1`, same XML paths, so a face can index
both devices with `Simpler::Param`) followed by the Sampler-only parameters (`Sampler::Param`). Every id is the XML
path below `<MultiSampler>`; a face binds a control by that path. "Plain" parameters are leaves without `<Manual>`
(written as `Value="..."`); the reader collects every numeric / boolean plain leaf of a `MultiSampler`
(`collectPlainLeaves` in `AlsReader.cpp`) into `Device::plain`, the writer puts them back in place.

Sections whose `Slot/Value` the reference leaves empty while switched off (Osc, Shaper, Aux, LFO 1 / 2 / 3, pitch
envelope) read as the defaults below; those are the values the captures show when the section is switched on
(`tab-*-on.png`). A new device equals `Defaults/Instruments/Sampler.adv` for every parameter that file holds (test
`sampler: a new device has the reference's default Sampler values`).

Status: **R** = read, **P** = played, **W** = written back (unchanged or edited), **V** = law verified (measured on the
reference's Simpler, DEV-22, or pinned by the reference's own data), **U** = law unverified.

### Player / zones (`Player/...`, `Player/MultiSampleMap/...`)

| XML path | Sonora | Status |
|---|---|---|
| `Player/MultiSampleMap/SampleParts/MultiSamplePart` | `model::SamplePart` (below) | R P W |
| `.../KeyRange/{Min,Max,CrossfadeMin,CrossfadeMax}` | zone keys; full level from CrossfadeMin to CrossfadeMax, fades towards Min / Max (`Sampler::zoneFade`) | R P W; fade law U (a fade over n keys steps 1/(n+1) so adjacent factory zones sum to 1) |
| `.../VelocityRange/{Min,Max,CrossfadeMin,CrossfadeMax}` | same for velocity | R P W, U |
| `.../SelectorRange/{Min,Max,CrossfadeMin,CrossfadeMax}` | Sample Select zone against `Player/SampleSelector` (+ matrix "Sample Selector" x 127) at note-on | R P W, U |
| `.../RootKey`, `.../Detune` (cents) | zone pitch | R P W, V |
| `.../TuneScale` (%) | key tracking: (note - root) x TuneScale / 100 (0 % in drum kits) | R P W, U |
| `.../Volume` (linear), `.../Panorama` | zone gain, equal-power pan (+3 dB at the sides, Simpler law) before the filter | R P W, V |
| `.../Link` | Sample Start = sustain loop start (manual) | R P W |
| `.../IsActive`, `.../Solo` | inactive zones never play; with any zone soloed only soloed zones play | R P W |
| `.../SampleStart`, `.../SampleEnd` | region; SampleEnd is the last frame played | R P W |
| `.../SustainLoop/{Start,End,Mode,Crossfade,Detune}` | Mode 0 off, 1 forward, 2 back-and-forth; forward loops crossfade into the audio before the loop start (constant power or linear by `Player/UseConstPowCrossfade`); Detune (cents) while looping | R P W; crossfade shape V (Simpler), detune U |
| `.../ReleaseLoop/{Start,End,Mode,Crossfade,Detune}` | Mode **3 = OFF** (sustain loop keeps looping in the release; the reference's default), **0 = ->\|** (plays on to the sample end), **1 = forward release loop** Start .. sample end, **2 = back-and-forth** | R P W; enum order from the factory zones (219 + 187 zones use 3; Lovely Keys' 0 shows "->\|" in the capture), U |
| `.../Name`, `SampleRef`, slicing, warp properties, `Selection` | kept from the file (ALS-15b) | R W |
| `Player/MultiSampleMap/LayerCrossfade` | Zone Fade Mode: 0 Lin, 1 Pow (constant power) | R P W, mapping U |
| `Player/MultiSampleMap/RoundRobin`, `RoundRobinMode` | RR on; 0 Forward, 1 Backward, 2 Other (random, never twice), 3 Random; zones sharing a key range take turns (one counter per key range) | R P W, U |
| `Player/MultiSampleMap/RoundRobinResetPeriod` | 0 never, then 1/16, 1/8, 1/4, 1/2, 1, 2, 4 bars (while the song plays) | R P W, enum U |
| `Player/MultiSampleMap/RoundRobinRandomSeed` | seeds Random / Other (kept as a double in `plain`, not a float parameter) | R P W |
| `Player/MultiSampleMap/LoadInRam` | RAM flag (Sonora always holds samples in memory) | R W |
| `Player/Reverse` | plays from the region end backwards, through the sustain loop, to the start | R P W |
| `Player/Snap` | start / end / loop points move to the nearest sign change of the left channel (manual) | R P W, U |
| `Player/SampleSelector` | 0..127 | R P W |
| `Player/InterpolationMode` | 2 Good / 3 Best: Simpler's measured kernel; 1 Normal: cubic; 0: linear (Lovely Keys' 2 shows "Good") | R P W, U |
| `Player/UseConstPowCrossfade` | loop crossfade shape | R P W, V |
| `Player/LoopModulators/*` | Simpler's region controls (always defaults in a Sampler) | R W |

### Modulation oscillator (`Player/SubOsc/IsOn`, `Player/SubOsc/Slot/Value/SimplerSubOsc/...`)

| XML path | Sonora | Status |
|---|---|---|
| `Mode` | 0 FM (linear FM of the playback rate: x (1 + osc x level)), 1 AM (gain x (1 + osc x level)) | R P W, U |
| `Type` | 21 waves: Sine, Sine 4 Bit, Sine 8 Bit, Saw 3/4/6/8/16/32/64 (additive), Saw D, Square 3..64, Square D, Triangle, Noise | R P W, U |
| `Volume` (linear, -70 dB floor), `VolumeVelScale` | level x (1 - Vol<Vel x (1 - velocity)) | R P W, U |
| `IsFixedFreq`, `FreqFixed`, `FreqFixedMul` | fixed Hz x {0.001, 0.01, 0.1, 1, 10, 100, 1000}[Mul] (default 100 Hz x index 3) | R P W, U |
| `TuneCoarse`, `TuneFine` | ratio = Coarse (1..48) or 0.5 / 0.25 / 0.125 for 0 / -1 / -2, x (1 + Fine / 1000); follows the played pitch and the pitch envelope | R P W, U |
| `Envelope/*` (14 fields, see Envelopes) | the oscillator's own envelope with loop modes | R P W |
| `Envelope/CurrentOverlay`, `ScrollPosition` | view state | R W |

### Pitch (`Pitch/...`, `Globals/...`)

| XML path | Sonora | Status |
|---|---|---|
| `Pitch/TransposeKey`, `Pitch/TransposeFine` | Transp, Detune | R P W, V |
| `Pitch/Envelope/IsOn`, `.../SimplerPitchEnvelope/Amount` | P.Env x Amount (st); also moves the oscillator | R P W, V (Simpler) |
| `.../SimplerPitchEnvelope/*` (14 fields) | levels, times, slopes, loop, Time<Vel | R P W |
| `Pitch/PitchLfoAmount` | LFO 1 Pitch (Simpler law: +-12 log2(1 + amount / 2) st) | R P W, V |
| `Globals/SpreadAmount` | left down / right up by Spread / 10 cents (Simpler) | R P W, V |
| `Globals/KeyZoneShift` | zone selection uses note - shift | R P W, V |
| `Globals/PortamentoMode`, `PortamentoTime` | 1 Glide: mono, legato notes glide; 2 Portamento: polyphonic, every new note glides from the last note; linear in semitones over Time (Simpler) | R P W; Portamento U |
| `Globals/PitchBendRange` (plain) | channel-1 pitch wheel range (st) | R P W |
| `Globals/MpePitchBendRange` (plain) | Note PB: pitch wheel on MPE member channels 2..16 (per note), default 48 | R P W |

### Filter / Shaper / Global (`Filter/...`, `Shaper/...`, `VolumeAndPan/...`, `Globals/...`)

| XML path | Sonora | Status |
|---|---|---|
| `Filter/IsOn`, `.../SimplerFilter/{Type,CircuitLpHp,CircuitBpNoMo,Slope,Freq,Res,X,Drive}` | Simpler's filter (Clean / OSR / MS2 / SMP / PRD, 12 / 24 dB, Morph, drive into the circuit) per voice and per channel | R P W, V (DEV-22) |
| `.../SimplerFilter/{ModByPitch,ModByVelocity,ModByLfo}` | Key, Vel, LFO 1 -> cutoff | R P W, V |
| `.../SimplerFilter/Envelope/{IsOn,Amount}` + 14 fields | F.Env | R P W |
| `.../SimplerFilter/{LegacyType,LegacyQ}` | the pre-Live-10 filter (LegacyMode false): kept | R W |
| `Shaper/IsOn`, `.../SimplerShaper/{Type,Amount,Structure}` | Soft (tanh) / Hard (clip) / Sine (fold) / 4 Bit (16 steps) after 0..+24 dB drive (Amount 0..100); Structure false = shaper -> filter (triangle up), true = filter -> shaper | R P W, U |
| `VolumeAndPan/Volume`, `VolumeVelScale`, `VolumeKeyScale` | Simpler laws (0.4 dB / velocity step around 70, 0.5 dB / semitone) | R P W, V |
| `VolumeAndPan/Panorama`, `PanoramaKeyScale`, `PanoramaRnd` | Simpler laws, after the filter | R P W, V |
| `VolumeAndPan/VolumeLfoAmount`, `PanoramaLfoAmount` | LFO 1 Vol / Pan (Simpler laws) | R P W, V |
| `VolumeAndPan/Envelope/*` (14 fields) | volume envelope: Initial (`AttackLevel`) -> Peak (`DecayLevel`) -> Sustain -> silence; levels at the -70 dB floor are silence; the voice ends below -60 dB | R P W |
| `VolumeAndPan/OneShotEnvelope/*` | Simpler One-Shot fades (unused by the Sampler) | R W |
| `Globals/EnvScale/EnvTime` | Time: every envelope x 2^(2 x Time / 100) | R P W, U |
| `Globals/EnvScale/EnvTimeKeyScale` | Time<Key: x 2^(amount / 100 x octaves from C3) (negative = shorter high notes, as the factory keys use it) | R P W, U |
| `Globals/EnvScale/EnvTimeIncludeAttack` | whether Time / Time<Key scale the attacks | R P W |
| `Globals/NumVoices`, `RetriggerMode` | Simpler's voice table (index 9 = 12 voices, as captured), Retrigger | R P W, V |
| `Globals/{NumVoicesEnvTimeControl,ModulationResolution,ScrollPosition,IsSimpler,PlaybackMode,LegacyMode}` | kept; a new device writes IsSimpler false | R W |

### Envelopes (volume, filter, pitch, aux, oscillator: 14 fields each)

`AttackTime`, `AttackLevel` (Initial), `AttackSlope`, `DecayTime`, `DecayLevel` (Peak), `DecaySlope`, `SustainLevel`,
`ReleaseTime`, `ReleaseLevel` (End; the volume envelope always ends silent), `ReleaseSlope`, `LoopMode`, `LoopTime`,
`RepeatTime`, `TimeVelScale`. All R P W.

- Segments: Initial -> Peak over Attack, Peak -> Sustain over Decay, held, then -> End over Release (manual). Slopes:
  Simpler's measured law (V): |slope| < 1 blends linear with exp(-9.115 x); +1 approaches the target dB-linearly to
  a -84 dB floor (pitch: higher floor).
- `LoopMode` 0 None, 1 Loop (after the decay, back to Initial over Loop Time, then the attack again; manual), 2 Beat /
  3 Sync (restart the attack from the current level every `RepeatTime` while held; Sync waits for the song grid), 4
  Trigger (note-off ignored until the decay ends, then the release — Simpler measured the release after the decay).
  Loop / Beat V (Simpler), Sync grid U.
- `RepeatTime` enum: Simpler's table (index 3 = 1/16 measured, the rest U).
- `TimeVelScale` (Time<Vel, %): times x 2^(-2 x amount / 100 x velocity) — U.
- `CurrentOverlay` / `ScrollPosition`: view state, R W.

### Modulation (`AuxEnv/...`, `Lfo/...`, `AuxLfos.0/...` = LFO 2, `AuxLfos.1/...` = LFO 3)

| XML path | Sonora | Status |
|---|---|---|
| `AuxEnv/IsOn`, `.../SimplerAuxEnvelope/*` (14 fields) | Aux envelope (levels -1..1) | R P W |
| `.../SimplerAuxEnvelope/ModDst/ModConnections.{0,1}/{Connection,Amount}` (plain) | A / B destinations and amounts | R P W |
| `Lfo/IsOn`, `.../SimplerLfo/{Type,Frequency,RateType,BeatRate,Attack,Retrigger,Offset,FrequencyKeyScale}` | LFO 1 (Simpler: shapes, S&H, smoothing, attack fade, Offset = start phase, Key law) | R P W, V |
| `.../SimplerLfo/{StereoMode,Spin,Phase,Smooth,Width}` | not on LFO 1's face: kept | R W |
| `AuxLfos.N/IsOn`, `.../SimplerAuxLfo/{Type,Frequency,RateType,BeatRate,Attack,Retrigger,Offset,FrequencyKeyScale}` | LFO 2 / 3, as LFO 1; S&H is a plain -1..1 hold | R P W; S&H U |
| `.../SimplerAuxLfo/StereoMode`, `Spin`, `Phase` | 0 Phase: the right channel starts Phase degrees later; 1 Spin: the right channel runs up to 50 % faster (rate x (1 + Spin / 2)) (manual) | R P W, U |
| `.../SimplerAuxLfo/{Smooth,Width}` | kept (Smooth is always 0.5 in the factory content; the smoothing is Simpler's fixed 0.6 ms) | R W |
| `.../SimplerAuxLfo/ModDst/ModConnections.{0,1}/{Connection,Amount}` (plain) | A / B | R P W |
| `BeatRate` | Simpler's 17 measured entries (1/64 .. 1.5 bars), then 2, 3, 4, 6, 8 bars (U) | R P W |

### MIDI (`KeyDst`, `VelDst`, `RelVelDst`, `MidiCtrl.0 .. 5`)

Each row is a `SimplerModDst`: `ModConnections.{0,1}/{Connection,Amount}` (plain, -100..100 %); `MidiCtrl.N/Feedback`
is kept (always 0 in the factory content). Rows (identified from the factory "MPE" presets' info texts: "Slide is routed
to filter frequency and Shaper amount" = MidiCtrl.4 -> 12 / 11, "Pressure ... to filter resonance and LFO 1 pan" =
MidiCtrl.0 -> 14 / 23, "Mod Wheel ... to LFO 1 volume" = MidiCtrl.2 -> 22; MidiCtrl.1 = Pitch Bend from a preset that
sets the bend range to 0 and routes the wheel; MidiCtrl.3 / 5 by elimination):

| Row | Source value (U) |
|---|---|
| `KeyDst` | (note - 60) / 64 |
| `VelDst` | velocity 0..1 |
| `RelVelDst` | release velocity 0..1, after note-off |
| `MidiCtrl.0` Pressure | channel pressure (channel 1) or the note's own pressure (poly aftertouch / member-channel pressure), 0..1 |
| `MidiCtrl.1` Pitch Bend | channel-1 pitch wheel -1..1 (the pitch itself bends by `PitchBendRange` too) |
| `MidiCtrl.2` Mod Wheel | CC 1, 0..1 |
| `MidiCtrl.3` Foot Ctrl | CC 4, 0..1 |
| `MidiCtrl.4` Slide | CC 74 (per note on member channels) 0..1 |
| `MidiCtrl.5` Note PB | member-channel pitch wheel -1..1 (the note bends by `MpePitchBendRange` too) |

### Matrix destinations (`Connection` values)

Order from the reference's destination name table; pinned by the Lovely Keys capture (4 = Loop Length, 18 = Volume),
the MPE preset info texts (11, 12, 14, 22, 23) and plausible factory uses (21 from an aux envelope on a "wobble" bass,
7 at 0.7 % as LFO drift). Amount 100 % x source 1 gives (all U):

| # | Destination | Effect |
|---|---|---|
| 0 | Off | |
| 1 | Sample Selector | +127 (note-on) |
| 2 | Sample Offset | start + region length (note-on, positive only) |
| 3 / 4 / 5 | Loop Start / Loop Length / Release Loop | loop start + region, loop length x 2, release start + region (note-on) |
| 6 / 7 / 8 | Pitch / Pitch Sample / Pitch Osc | +-12 st (sample and oscillator / sample / oscillator) |
| 9 | Pitch Env | envelope amount +-48 st |
| 10 | Volume Osc | oscillator level +1 (linear) |
| 11 | Shaper Amt | +100 |
| 12 / 13 / 14 | Filter Freq / Filter Q (Legacy) / Filter Res | +-60 st / resonance +1 / resonance +1 |
| 15 / 16 / 17 | Filter Morph / Filter Drive / Filter Env | +1 / +24 dB / +-72 st |
| 18 | Volume | gain x (1 + m), 0 .. 2 |
| 19 | Pan | +1 |
| 20 | Time | envelope times x 2^(2 m) (note-on) |
| 21 / 22 / 23 / 24 / 25 | LFO 1 Rate / Vol / Pan / Filter / Pitch | rate x 2^(4 m); amounts +1 (filter +24) |
| 26 / 27 / 28 | LFO 2 Rate / Amt A / Amt B | rate x 2^(4 m); A / B route amounts x (1 + m) |
| 29 / 30 / 31 | LFO 3 Rate / Amt A / Amt B | same |

LFO 2 / 3 are stereo: their routes feed separate left / right sums; Volume, Pitch, Filter (per-channel coefficients)
use each channel's sum, the other destinations the left one (Pan the mean).

### View settings (`ViewSettings/*`, `SimplerSlicing/PlaybackMode`)

Kept in `plain` and written back (the face may read `ViewSettings/SelectedPage` 0 Sample .. 4 MIDI and
`ZoneEditorVisible`).

## Signal flow per voice

Zones selected at note-on (key after Zone Shift, velocity, selector; crossfade gains; round robin) are layers of one
voice (up to 8). Each layer reads its sample with the chosen interpolation at
`2^((note - root) x TuneScale / 100 + detunes + pitch modulation) x sample rate ratio`, loops / releases as above and is
panned by its zone; the layers' sum goes through the Shaper (pre), the filter (per channel), the Shaper (post), then
volume envelope x level x LFO 1 volume x matrix volume x AM, and the voice pan (with LFO 1 / matrix pan). Glide shifts
the pitch linearly in semitones. Everything runs on the audio thread without allocation, locks or logging.

## Verification

- `sonora_tests sampler` (tests/test_sampler.cpp): parameter ids, zone fade law, zone selection (key / velocity /
  selector / solo / inactive / Zone Shift), round robin modes, envelope levels / loop / trigger, sustain loop forward
  and back-and-forth with every Release Mode, Reverse, Link, Tune Scale, Pitch Bend and Note PB ranges, Portamento,
  LFO 2 stereo tremolo, Mod Wheel -> Filter Freq, Velocity -> Volume, AM, shaper / oscillator laws, all-extremes
  finiteness, the zone XML round trip, the reference's default values, and **every factory Sampler preset**: loaded
  without clamping a single value, saved in a set and read back with equal parameters, plain leaves and zones.
- `parity_probe make / render` with `tools/parity_probe/suites/sampler_laws.txt` (13 law cases on the default Sampler
  + every factory preset with its library samples): all 89 stems play, finite, -36 .. +16 dBFS peaks (no reference
  stems to compare yet).

## Open (DEV-14b)

- Reference stems for the unverified laws (open `Parity.als` of the suite in the reference, export, `parity_probe
  compare`): matrix scaling per destination, source ranges, FM / AM depth, oscillator waves, shaper curves and drive,
  Time / Time<Key / Time<Vel, zone fade steps, round robin reset enum, interpolation Normal vs Good, Spin.
- Loop crossfades for back-and-forth loops and release loops played in Reverse; matrix destinations that act at
  note-on only (Sample Offset, loop points, Time) may follow continuously in the reference.
- A Sonora-saved Sampler with edited Osc / Aux / LFO sections opened in the reference (slots are written with
  `Id="0"` and only the fields Sonora has).
