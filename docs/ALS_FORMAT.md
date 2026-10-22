# The `.als` format (as used by Sonora)

`.als` = **gzip-compressed XML**. Root element `<Ableton MajorVersion="5" MinorVersion="12.0_12120" Creator="Ableton Live 12.1.5">`.
Almost every scalar is stored as `<Tag Value="..."/>`. Booleans are `"true"/"false"`.

**Authoritative structure reference:** Live ships its own schema for every version at
`/Applications/Ableton Live 12 Suite.app/Contents/App-Resources/Schema/<version>_<n>.txt`
(203 files; newest `12.0_12402.txt`, 412 classes). Each class lists child elements with their
`Class`. Use it to find field names. (Do not copy those files into the repo.)

Notation below: paths relative to the element in bold. ✅ = read by `AlsReader.cpp`.

## LiveSet
```
Ableton/LiveSet
  Tracks/{AudioTrack|MidiTrack|GroupTrack|ReturnTrack}*        ✅ (display order)
  MainTrack            (Live 12; Live ≤ 11: MasterTrack)        ✅ both
  Scenes/Scene*        Name, Color, IsTempoEnabled, Tempo        ✅
  Locators/Locators/Locator*  Time, Name                         ✅
  Transport            LoopOn, LoopStart, LoopLength, CurrentTime, PunchIn, PunchOut ✅
```
- Tempo: `MainTrack/DeviceChain/Mixer/Tempo/Manual` ✅
- Time signature: `MainTrack/DeviceChain/Mixer/TimeSignature/Manual` — one integer:
  `value = (numerator-1) + 99*log2(denominator)` (201 = 4/4) ✅ `TimeUtils.h`
  The control-bar editor patches this manual value and a matching single pre-roll signature automation point, with undo.
  Multi-point signature automation is retained but not played or authored (ENG-11b); this editor changes the global meter.

## Track (`AudioTrack`, `MidiTrack`, `GroupTrack`, `ReturnTrack`, `MainTrack`)

Whole-track duplication materializes current edits into a disposable DOM, copies the full track element, and remaps
automation/modulation/Pointee/controller target IDs plus internal group and `/Track.<id>` routing references. Device,
clip and take fragments retain unknown data; runtime clip UIDs are fresh. Reconciliation removes known track elements
that undo removed from the model, including after saving a duplicated track, and preserves unrecognized track kinds.

| Field | Path | Notes |
|---|---|---|
| id | attribute `Id` | unique among tracks; used by `TrackGroupId` ✅ |
| name | `Name/UserName` or `Name/EffectiveName` ✅ | Empty UserName sets `nameIsAutomatic`; EffectiveName is the cached name. Display ordinals follow current track order, including groups. Explicit names stay literal; leading `#` templates substitute the ordinal (repeated `#` pads with zeroes). Writer retains empty UserName for automatic names; untouched XML round-trips unchanged. |
| colour | `Color` ✅ | index into 70-colour palette (`core/LiveColors.cpp`) |
| group | `TrackGroupId` ✅ | -1 = top level |
| folded | `TrackUnfolded` ✅ | group tracks |
| volume | `DeviceChain/Mixer/Volume/Manual` ✅ | **linear gain**, 1 = 0 dB, max 1.99526 (+6 dB), 0.0003162 = -70 dB = "-inf" |
| pan | `DeviceChain/Mixer/Pan/Manual` ✅ | -1..1 |
| activator | `DeviceChain/Mixer/Speaker/Manual` ✅ | false = muted |
| solo | `DeviceChain/Mixer/SoloSink` ✅ | |
| sends | `DeviceChain/Mixer/Sends/TrackSendHolder*/Send/Manual` ✅ | linear, one per return, in return order |
| automation target ids | `Volume/AutomationTarget@Id`, `Pan/AutomationTarget@Id` ✅ | matched by envelopes |
| devices | `DeviceChain/DeviceChain/Devices/*` ✅ | |
| session clips | `DeviceChain/MainSequencer/ClipSlotList/ClipSlot*/ClipSlot/Value/{MidiClip|AudioClip}` ✅ | one slot per scene |
| arrangement clips (MIDI) | `DeviceChain/MainSequencer/ClipTimeable/ArrangerAutomation/Events/MidiClip*` ✅ | |
| arrangement clips (audio) | `DeviceChain/MainSequencer/Sample/ArrangerAutomation/Events/AudioClip*` ✅ | |
| automation | `AutomationEnvelopes/Envelopes/AutomationEnvelope*` ✅ parsed and played (ENG-11) | `EnvelopeTarget/PointeeId` + `Automation/Events/FloatEvent@Time,@Value` |

### Input routing and monitoring (ENG-9b)

`DeviceChain/{AudioInputRouting|MidiInputRouting}/{Target,UpperDisplayString,LowerDisplayString}` are retained
and patched for the active track kind. `MainSequencer/MonitoringEnum`: 0 In, 1 Auto, 2 Off. Presence/edited
flags retain missing original elements and remove newly authored fields after Save→Undo→Save. Other routing
children (including MPE) pass through unchanged. Unsupported intertrack routes are retained and logged.

Observed audio targets: `AudioIn/External/M0` = mono channel 1, M1 = channel 2;
`AudioIn/External/S0` = stereo 1/2; `AudioIn/None` and `AudioIn/Main` = No Input and Resampling.
Higher S values currently use ordinal stereo pairs, preferring numeric LowerDisplayString such as 3/4;
this convention still needs a multichannel reference check. Physical input index is limited to 0..63.
MIDI: `MidiIn/External.All/-1` = all ports/channels; `MidiIn/External.<identifier>/<channel>` uses
file channels 0..15 (runtime 1..16), -1 all. Specific JUCE persistent identifiers need hardware/Live
interoperability verification; missing ports are displayed offline. Port tokens are runtime-only hashes.

The generated recording probe opened in Live 12.4.6 with external mono channel 2, Monitor Off and a
48 kHz/24-bit/1-channel authored clip. This verifies that output, not every hardware or routing variant.

### Punch flags and count-in (ENG-9b)

Transport `PunchIn`/`PunchOut` are booleans; the region uses LoopStart/LoopLength even with LoopOn false.
Presence/edit flags preserve untouched/missing XML and undo newly authored fields across repeated saves.
Count-in is a Sonora application preference (None/1/2/4 bars), not an added LiveSet scalar. The schema lists
Metronome/CountInDurationInBars on the metronome device; changing Live's count-in None→1 Bar and saving our
probe did not serialize that device or field into the set. Live's own saved punch booleans are true, but
Live 12.4.6 resets both UI toggles on reopening even its own saved copy; Sonora currently reloads the fields.

A Sonora native count-in/punch resampling take opened in Live as 44.1 kHz/24-bit/2-channel audio at beats
1→2. The tool's mono 48 kHz/24000-frame punch take opened too. Neither proves physical-input timing parity.

### Session recording reference (ENG-9d)

Own `/tmp/Sonora Verification Project/Session Recording Reference.als`, recorded with Live 12.4.6's computer
MIDI keyboard and Session Record: scene 3 gets a looping MIDI clip with `CurrentStart/LoopStart/StartRelative`
zero and `CurrentEnd/LoopEnd` 29, while the Arrangement loop remains beats 1→2. A second Session overdub adds
pitch 67 while retaining pitches 60/64, their NoteIds 1/2, the clip ID and 29-beat loop; new NoteId is 3.
This establishes independent Session elapsed time and retained clip identity; it does not prove every
quantisation mode or same-pitch overlap rule. Sonora's synthetic scene-2 overdub keeps its four-beat loop and
opens in Live with the original C3/E3/G3 notes plus added D4. The native eight-bar stereo WAV clip also opens
with Loop On, Warp On/Beats and 44.1 kHz/24-bit/2-channel sample information. Controller capture/playback/save is now implemented for linked envelopes (ENG-9e); MIDI Ctrl targets use the descriptors below.

### MIDI controller envelopes (ENG-9e)

Own Controller Envelope Reference in Live 12.4.6 establishes
`MidiTrack/DeviceChain/MainSequencer/MidiControllers/ControllerTargets.N@Id` → clip
`Envelopes/Envelopes/ClipEnvelope/EnvelopeTarget/PointeeId@Value`. Target **0** is signed pitch bend
(-8192..8191), **1** channel pressure, **CC n** uses target **n+2** (observed CC1→3, CC7→9, CC10→12,
CC11→13, CC64→66). Descriptor 130 has no verified meaning and is preserved. General CC indexing beyond
those observed follows that mapping; channel-mode CC interoperability remains unverified.

These are **FloatEvent**, including pressure and sustain. Duplicate points at the same `Time` express
jumps; stable order matters. Between distinct times Float values interpolate. Default preroll values
at -63072000 are 0 except CC7=100, CC10=64, CC11=127, observed by drawing the own reference. Fresh
ClipEnvelope metadata includes empty LoopSlot/Value, ScrollerTimePreserver and AutomationTransformViewState.
The authoring probe's 131 missing descriptors are newly allocated with LockEnvelope=0; its linked bend
and sustain lanes open under the correct chooser in Live. Unknown metadata and untouched events remain.

Recorder serializes captured transitions as paired Float points and leaves physical note-offs intact
when sustain is held. Partial overdubs preserve original left limits and restore old values outside the
capture interval. Playback converts values to channel-1 MIDI at sample positions; MPE/poly pressure,
independent LoopSlot timing, bezier controls and controller-start reset-policy parity are not established.
`recording_probe --controllers` creates separate own outputs; Native Controller Final reopens in Live
with all seven envelopes intact after Sonora native delete/save/undo/redo and duplication checks.

### Controller editor authoring (UI-5b)

`setClipMidiEnvelope` preserves Float values and duplicate ordering for existing lanes. It authors
MIDI semantic targets through the track descriptor table as part of one undo. Fresh lanes retain the
preroll default and a beat-zero default anchor when their first edited point is later than zero. This
prevents interpolation from the distant -63072000 sentinel. Existing lanes are not given new anchors.
Grid drawing creates paired boundary Float points, preserving the old left limit before the painted
interval and its original right-edge value afterwards; unchanged adjacent cells need no extra points.
Clip Envelope Native Final retains the original six other envelopes and all notes; its CC64 interval
1.5→2 has value 126, then restores the old 2.95348835 at 2. It opens under CC64 in Live 12.4.6 with that
shape. Clip Envelope Breakpoints adds CC2 through the same UI after corrected double-click
routing, with default 0 at beat 0 and value 90 at beat 2. The exact file opens in Live and its native
chooser recognises 2: Breath Controller; a fresh curve screenshot remains unverified because the
computer-use capture returned an older Notes view despite updated accessibility state. These are own
disposable verification sets.

### Continuous Session overdub references (ENG-9f)

Own Independent Session Final (ENG-9g) records separate MIDI tracks through staggered start/stop plans.
Track 0/scene 3 saves a two-beat loop while track 1/scene 0 keeps capturing to three beats; the latter has
channel-2 note/pedal input authored through typed track-local descriptors. Second-track save/undo/reload/
redo leaves the first track intact. The exact final file opens in Live 12.4.6, whose native accessibility
reports Loop On with lengths 0.2.0 and 0.3.0. This checks authored XML/timing metadata, not physical input
timing, Live's recording gestures or listening parity.

Own Overdub Playback Reference, recorded through the temporary `midi_input_probe` CoreMIDI source in
Live 12.4.6, retains CC1 110→15, pedal 127→0 and bend 4096→0 over later passes without input. A five-beat
controller gesture in a four-beat loop also retains both transitions when the receiving pass ends later
in phase than it starts. Empty passes do not flatten the previously recorded pattern.

Own Controller Wrap Reference isolates a two-beat loop. The first recording has CC1 110 at phase
0.414195 and 15 at 1.421089. A later overdub sends 64 near the previous pass end and 32 at phase
0.502585 in the receiving pass. The saved envelope retains the original 110 and new 32; the previous
pass's 64 and later original 15 disappear. This supports overwriting from the first touched point to
the end of each touched pass, while leaving silent passes intact. It does not establish every overlap,
held-note, unlinked-envelope or curve rule. Sonora's model merge and bounded ongoing playback have a
matching wrap regression. Native Realtime Overdub, captured in Sonora via the same ordinary CoreMIDI
input path, reopens in Live with its note and three linked envelopes after native save/undo/redo.

## Clips
Common (`MidiClip`/`AudioClip`): attribute `Time` (timeline position), `CurrentStart`, `CurrentEnd`
(timeline start/end in beats), `Loop/{LoopStart,LoopEnd,StartRelative,LoopOn}`, `Name`, `Color`,
`Disabled`, `Envelopes/Envelopes` (clip envelopes, ALS-9) ✅.

### Clip timing (implemented in `TimeUtils::clipContentPosition`, tested)
Content position at timeline beat `t`:
```
pos = LoopStart + StartRelative + (t - CurrentStart)
if LoopOn and pos >= LoopEnd:  pos = LoopStart + fmod(pos - LoopStart, LoopEnd - LoopStart)
```
**Units of the loop fields depend on warping (verified on real sets):**
- MIDI clips and **warped** audio clips: beats.
- **Unwarped** audio clips: **seconds** (`LoopEnd` == file length in seconds). `CurrentStart/End` stay in beats.

### MIDI notes (Live 11+)
```
MidiClip/Notes/KeyTracks/KeyTrack*/
    MidiKey Value=pitch
    Notes/MidiNoteEvent* Time Duration Velocity VelocityDeviation OffVelocity Probability IsEnabled NoteId
```
`Time` is relative to the clip content start (beats). Velocity is a float 0..127. ✅
Live 9/10 used a different layout (ALS-11).

### Audio clips
`IsWarped`, `WarpMode` (0 Beats, 1 Tones, 2 Texture, 3 Re-Pitch, 4 Complex, 6 Complex Pro),
`WarpMarkers/WarpMarker@SecTime,@BeatTime`, `SampleVolume` (linear), `PitchCoarse`, `PitchFine`,
`SampleRef/FileRef/{Path,RelativePath,RelativePathType}`, `SampleRef/DefaultDuration` (samples),
`SampleRef/DefaultSampleRate` ✅.
Optional Sonora-owned `SonoraTempoAnalyzed Value="true"` retains a detected or manually confirmed source tempo across
Warp Off/save/reload/On. Imported warped clips are treated as having an authored tempo without adding this field to
untouched files. First detection/manual tempo edits retain their grid in ordinary WarpMarkers; no external analysis data
or vendor content is embedded. A first-Warp tempo estimate uses detected rhythmic attacks and falls back to set tempo
when no steady pulse is found (UI-41).
Sample resolution (`resolveSamplePath`): retained source project folder + `RelativePath` first, then absolute `Path`.
The retained origin survives Save As/undo. App Save As anchors resolvable project-relative refs to absolute
paths when moving the set; it references existing media rather than copying it.
Paths may be stale or Windows paths from other machines.

Usage in the real-world test sets: warped Beats ≈ 2,000 clips, Complex 188, Complex Pro 14, Re-Pitch 124,
unwarped ≈ 8,000.

## Devices
Element name = device type. Every device has `On/Manual` (enabled) and `UserName`.
Parameters are `<Name><Manual Value=…/><AutomationTarget Id=…/>…</Name>`; Sonora flattens every
`.../Manual` under a native device into `Parameter{path, value}` (e.g. `Bands.3/ParameterA/Freq`).

### Native devices implemented
| Device | Tag | Parameter ids (path) | Units |
|---|---|---|---|
| Utility | `StereoGain` | `Gain` (linear), `Balance` (-1..1), `StereoWidth` (0..4, 1 = 100 %), `ChannelMode` (0 L, 1 Stereo, 2 R, 3 Swap), `Mono`, `Mute`, `PhaseInvertL/R`, `BassMono`, `BassMonoFrequency`, `DcFilter`, `MidSideBalance` | |
| EQ Eight | `Eq8` | `Bands.N/ParameterA/{IsOn,Mode,Freq,Gain,Q}` (ParameterB = right/side), `GlobalGain` (dB), `Scale`, `Mode` (stereo/LR/MS), `AdaptiveQ`, `Precision` | Freq Hz, Gain dB |

EQ Eight band `Mode`: 0 LowCut48, 1 LowCut12, 2 LowShelf, 3 Bell, 4 Notch, 5 HighShelf, 6 HighCut12, 7 HighCut48
(consistent with defaults found in real sets: band 1 cut, 2–7 bell, 8 high cut).

### Racks
`InstrumentGroupDevice`, `AudioEffectGroupDevice`, `MidiEffectGroupDevice`, `DrumGroupDevice`:
`Branches/{InstrumentBranch|AudioEffectBranch|DrumBranch|MidiEffectBranch}*` each with a nested
`.../Devices`, `MixerDevice/{Volume,Panorama,Speaker}/Manual` ✅.
Drum pads: `BranchInfo/ReceivingNote` — **inferred** encoding `128 - MIDI note` (92 = C1 = 36), and
`BranchInfo/SendingNote` (default 60) ✅ (ALS-8 to confirm).

### Plugins
```
PluginDevice/PluginDesc/VstPluginInfo     PlugName, Path, UniqueId, Category, Preset/VstPreset/Buffer (hex)
PluginDevice/PluginDesc/Vst3PluginInfo    Name, Uid/Fields.0..3 (int32 x4 = 16-byte class id), DeviceType,
                                          Preset/Vst3Preset/{ProcessorState, ControllerState} (hex)
AuPluginDevice/PluginDesc/AuPluginInfo    Name, Manufacturer, ComponentType/SubType/Manufacturer (FourCC ints),
                                          Preset/AuPreset/Buffer (hex)
PluginDevice/ParameterList/PluginFloatParameter*  ParameterId, ParameterName, ParameterValue/Manual (automation, PLG-7)
```
Hex blobs contain whitespace/newlines; `decodeHexBlob` skips non-hex chars.
VST3 class id string = `%08X%08X%08X%08X` of the four fields. VST3 state is restored by building a
`.vstpreset` (header `VST3`, version 1, class id, chunk list `Comp`/`Cont`) — **verified working**.
Usage in the real-world test sets: VST2 110 instances, VST3 39, AU 19.

### Max for Live
`MxDeviceAudioEffect`, `MxDeviceInstrument`, `MxDeviceMidiEffect` — skipped (needs Max runtime), except patches with
a native replacement (DEV-23). The patch is `PatchSlot/Value/MxPatchRef/FileRef/RelativePath` (or `Path`); its file
name without `.amxd` is `Device::maxPatch` and `model::maxForLiveNativeName()` decides. Live parameters, when the patch
has any, are `ParameterList/ParameterList/MxD{Float,Int,Enum}Parameter` with `Name` and a value; `BlobSlot/.../MxDBlob/Blob`
is hex JSON of the patch's own UI state (Swiss Army Meter: `Expand`, `Side Menu`, `devicewidth` ...; no parameters).
The element is retained and saved back unchanged apart from `On`. Browser inserts author the schema's MxDevice
children (PatchSlot with `RelativePathType` 3 to `Presets/Audio Effects/Max Audio Effect/Imported/<patch>.amxd` and the
absolute path of a copy found on disk; empty parameter/file-drop/id lists, blob and routables) — not yet opened in Live.

## Writer and authored insertion templates (ALS-10c / UI-7)

The writer patches the loaded XML, retaining unknown fields. Clip history keeps immutable original fragments
and stable UIDs; `Song::savedClips` records current file locations separately. Device history retains source
fragments and VST3 component/controller state. New native devices, VST3 wrappers and first audio clips use
Sonora-authored templates based on field structure. Missing templates/backups abort the save without
replacing its destination. Untouched files still round-trip to identical XML (65/65).

Live 12.4.6 opened a disposable file saved through Sonora with inserted Vital, enabled Utility and a stereo
sample in both Arrangement and Session. Format requirements discovered in that probe:
- `Vst3PluginInfo` and `Vst3Preset` are **Slot members with `Id="0"`**; UID fields serialize as signed int32.
- `SampleRef/DefaultSampleRate` uses an integer value, not a decimal string.
- `WarpMarkers` require anchors even with `IsWarped=false`; new unwarped samples have neutral endpoints.
- Native toggle manuals and `On/Manual` use `true`/`false`. New automation targets use unique pointee IDs.
- Fresh/recorded content clears inherited envelopes, onsets and source end markers; undo restores the
  original fragment/warp data. UI-10c edits loop/start values and warp-marker lists/modes; unchanged
  marker lists preserve their original XML. The writer compares probability, release velocity and
  velocity deviation as well as note position/length/pitch/velocity before deciding notes are unchanged.

Portable-reference rewriting is enabled by the app; tools default it off so untouched XML comparison is
meaningful. AU insertion, authored VST3 parameter automation descriptors and nested rack plugin state
capture remain follow-ups. Opening this probe does not verify every possible edited set or DSP parity.

The disposable `als_dump --resave-editor SOURCE COPY` probe patches loop/start settings and note
probability in Arrangement/Session, and creates a three-anchor warped audio map. It requires a separate
destination. `/tmp/Sonora Verification Project/Clip Editor Probe.als` opened in Live 12.4.6 without a
format error; Live displayed the edited start/loop and Beats warp anchors. Local Sonora scale highlighting
is UI state; clip/global scale enum import/save remains UI-10d.

## Take lanes (ALS-14 / ENG-9b)

Track/TakeLanes/TakeLanes/TakeLane has Id, LomId, Height, IsContentSelectedInDocument,
ClipAutomation/Events/{AudioClip|MidiClip}, Name, Annotation, Audition and ArrangementClipsListWrapper.
The outer container has AreTakeLanesFolded. The model reads lane/clip metadata and retains immutable
fragments; untouched lanes remain untouched. Append allocates fresh stable lane/clip identities; both
apply and undo request a container patch, allowing Save→Undo→Save to remove authored containers.
Unknown container/lane/event fields survive patches. Clip templates validate before destination write;
Save As relocates each take clip using its retained sample origin, including same-name impostors.

Own native Live reference: `/tmp/Sonora Verification Project/Loop Reference.als`, 15 passes, one shared
338432-frame/44.1-kHz stereo WAV (silence from internal resampling, not physical input). The first take
is timeline 0→2, content LoopStart/End 0→2; second is timeline 1→2, content 2→3; final partial take is
1→1.3483901515, content 15→15.3483901515. All are warped, non-looping clips; TakeId increments 1..15.
Main lane contains copied sections, including the final partial pass and surviving earlier sections.
TakeId is now a typed clip field; recorded comp copies retain the same ID as their lane source. Per-track
nextTakeId is derived from all main/Session/lane clips. MainSequencer/Recorder/TakeCounter advances once
per committed recording group (this is the recording/file counter, not the per-pass TakeId allocator).
Generated recording warp maps cover the continuous WAV and preserve changes in samples-per-beat. Audition
is stored and now routed through prepared take-lane playback. Lane editing rebuilds only the edited container
while retaining unknown XML. Copied comp sections retain TakeId and adjusted source offsets; duplicated
lanes have fresh lane IDs, clip UIDs and source TakeIds. Renaming/reordering/deleting does not change copied
main material. Current folding visibility is local; the original AreTakeLanesFolded XML stays untouched.

`als_dump --resave-takes SOURCE COPY` authors a disposable named lane from an Arrangement clip.
The copy opened in Live and displayed "Sonora retained take" with Cmd+Opt+U.
The source is never overwritten by this mode.

Continuous capture proof: `recording_probe SOURCE COPY --loop` records our own 440/660 Hz input for
108000 frames at 48 kHz; loop region beats 1→2, pre-roll 0→1, four passes including a final half pass.
The main comp has three sections: pre-roll from pass 1, final half from pass 4, previous remainder from
pass 3. All refer to one WAV. Final defaults are Warp On, Beats, non-looping clips, content loop bounds
in unfolded beats. Recording can continue after loop/punch boundaries; explicit stop ends the group.
Own final native `/tmp/Sonora Verification Project/Native Loop Final.als`: four prior lanes plus 70 new
passes (74 total), one additional stereo 44.1-kHz/24-bit WAV of 1555968 frames, three main sections.
Live opened the exact final file and displayed all 74 lanes; no physical-input or audio-parity proof is implied.

Own UI-15 verification file: `/tmp/Sonora Verification Project/Take Editing Probe.als`. Native comping
replaced beats 1→2 with source TakeId 2/content beats 2→3. Final lane order/IDs are Take 1/0, Take 3/2,
Chosen Take/1, Take 4/3, Chosen Take Copy/4. Only Chosen Take has PitchCoarse 2; the two main clips
and duplicate have 0. All audition flags ended false. Live 12.4.6 opened it, exposed all five names in
that order, and showed +2 in the selected source take's Sample/Pitch controls. Source WAV is our own
mono 660-Hz/48-kHz/24-bit recording; no reference application's audio/assets are shipped.


## Authored empty sets (Sonora "New Project")

`io/als/AlsNewSet.cpp` writes an empty Live Set from the documented element names (schema classes in Live's
`Schema/12.0_*.txt`). Live 12.4.6 is strict about a few things that are easy to get wrong — all learnt by opening probe files
and bisecting hybrids of Live's own default sets against ours (Live logs `Loading document ...` then `Begin ExchangeDocument`
and dies with a null dereference when the structure is inconsistent; recoverable errors are shown in a dialog):
- **Integer-typed nodes** must not be printed as `1.000000000000` ("Unexpected value for int node"). Print numbers without padding.
- **Slot counts**: every Midi/Audio track's `MainSequencer/ClipSlotList` *and* `FreezeSequencer/ClipSlotList` need exactly one
  `ClipSlot` per scene ("Slot count mismatch"). Return tracks keep an empty `FreezeSequencer` list; the Main track stores
  `FreezeSequencer/AudioSequencer` with one slot per scene.
- **`SendsPre`** needs one `<SendPreBool Id=n Value=false/>` per return track, and each track's `Sends` one `TrackSendHolder`
  per return (send range 0.000316..1). A missing entry crashes Live on load.
- A `PreHearTrack` follows `MainTrack`; Main's `AutomationEnvelopes` carry the initial Tempo (`FloatEvent`) and TimeSignature
  (`EnumEvent` 201) at time -63072000. Automation/modulation target IDs must be unique across the document;
  `NextPointeeId` is one past the largest.
- A MIDI clip template (`midiClipTemplate()`) with an empty `KeyTracks` list, `NoteIdGenerator`/`ProbabilityGroupIdGenerator`
  and a `TimeSignature` list opens in Live and is filled by the normal writer path.

### Region duplication (UI-31)

Arrangement region copies retain each clip's immutable source XML and clone metadata, trimming source intersections
and destination fragments while allocating fresh clip identities. Content offsets advance in beats for MIDI/warped
audio and seconds for unwarped audio. Existing clip envelopes remain in content time. Track automation is copied as
piecewise linear/stepped values with equal-time jumps at destination boundaries; pre-roll-only manual envelopes are
left untouched. Source XML outside known edited fields still passes through the existing writer. UI-31 does not
parse or author fade-curve skew/slope: those fields remain retained opaque data, while Sonora draws/plays its own fixed
concave fade curve. Exact imported fade-shape and automation bezier evaluation remain separate follow-ups.

### Moving Arrangement regions (UI-32)

A transfer stages all source tracks before clearing any source/destination interval, including overlapping horizontal
and vertical moves. Full moved clips retain their runtime UIDs; copies and split fragments allocate new ones. A
cross-track move marks its clip XML as a clone from the original track, keeping unknown metadata and content-time
envelopes. Save/refresh/undo/save/redo is covered for partial multi-track and whole cross-track moves. Destinations
replace the full interval including gaps, while outside fragments keep their original seconds/beats offsets.

Authored track envelopes use explicit equal-time boundary jumps and a held value in the cleared source interval.
Pre-roll-only manual values and unresolved negative target IDs are unchanged. Vertical moves rebind known mixer/send,
existing MIDI-controller and matching device/parameter targets; a missing mapping rejects the complete operation.
No target IDs are invented at drag time. Group roll-up/take-lane region editing, bezier interpolation and new target
authoring remain separate tasks.


## Per-note expression foundation (UI-35)

The installed 12.x schema lists `Notes/PerNoteEventStore/EventLists/PerNoteEventList` with NoteId/CC attributes,
Events/PerNoteEvent with TimeOffset/Value and CurveControl1X/Y/2X/Y. Sonora retains these lists keyed to existing
MidiNoteEvent NoteId, separate from notes so realtime capture keeps a fixed-size note value. The MPE editor assigns a
local NoteId before authoring a new curve. Modified lists preserve other source lists/unknown children; immutable-source
rebasing makes saved added lanes disappear after undo, then reappear on redo. Unedited files retain their original XML.

**Observed in installed bundled MIDI-clip XML** (350 `.alc` files inspected read-only): CC **-2** carries signed
pitch values (observed -32..49), CC **-1** pressure and CC **74** slide (both observed 0..127). Lists are keyed by NoteId;
TimeOffset is note-relative. Sonora adopts these encodings, emits pitch at a ±48-semitone member bend range and raw
0..127 pressure/slide. Field/control names agree with the installed schema. The inspection retained only structural/
numeric observations; no bundled clip, note pattern, preset, sample or other asset was copied into the repository.
No nonempty PerNoteEventStore was found in the scanned real-world project corpus. Self-save/reload/history and emitted
MIDI bytes are tested; opening/editing the authored result in the reference app remains UI-35b. Unknown CC lanes are
retained without playback. Existing curve controls persist, but interpolation currently joins points linearly.

### Sonora fade shapes and frozen-render cache (UI-38)

Audio clips retain FadeInCurveSkew/FadeOutCurveSkew unchanged on import and save. UI edits bound skew to ±0.95;
DSP/UI use Sonora's monotonic rational skew followed by the existing curved gain function. Imported slope leaves
remain opaque and preserved; the curve law is independently implemented and not calibrated to the vendor's law.

A frozen track retains all original clips, devices and automation. Optional `SonoraFreeze Version="1"` contains
`Render Scene="-1"` for Arrangement and numbered Session renders, each holding an authored AudioClip with standard
SampleRef/FileRef/warp/loop/envelope data. Readers without this extension still see the original editable track.
Sonora substitutes cached audio/no devices in the engine, retains live mixer/send automation, and falls back to the
original when a cache file cannot be found. Save As/Collect include cached sample references. Unfreeze removes the
extension; history retains generated files for subsequent redo. Bounce may replace MidiTrack/GroupTrack by AudioTrack
under the same track ID; writer reconciliation now checks type as well as order/ID.


### Native plain settings (DEV-20 / UI-39)

`ParameterInfo::plainValue` marks Remoteable/user value leaves rather than Timeable `Manual` containers. Native model
creation and capture store those descriptors in `Device::plain`; loading reads them there, with a compatibility fallback
to older parameter containers. The reader retains Globals leaves and Tuner's four known display leaves. The writer patches
edited plain values in place, preserving existing boolean spelling and unchanged numeric text. Authoring handles both root
and nested paths (a root setting must not become a duplicate parent/child). This fixes saved Simpler playback mode/voice
settings and Tuner view/strobe/Hz/spelling persistence. Unknown XML remains retained rather than reinterpreted.

Redux2, Tuner, Gate, AutoPan and the five Midi* note transforms use independently written DSP and the shipped declarative
schema paths. Original ranges/algorithms and advanced settings still need compatibility qualification.
The legacy Redux tag and AutoPan2 retain their unsupported status.

## Sonora Resynth extension (RESYN-1)

`SonoraResynth` is a Sonora-only native instrument; its scalar controls use ordinary authored native
parameter containers. Existing `<SonoraState Format="sonora-resynth-v2">` stores the analysis in the hex
`ProcessorState` payload. It is self-contained, including an optional mono reference for Reference/Hybrid audition,
so it requires no new FileRef collection or external model-file path rules. Other readers do not implement it.

Payloads use little-endian integers/floats, magic `0x53525331`, version 2, a bounded zero-terminated UTF-8 name,
sample rate/channel, duration frame count, stored PCM count (zero for model-only patches), note count, optional mono float samples, and note records containing start/end/root/
confidence/sustain and bounded harmonic/noise frames. A trailing FNV-1a 32-bit checksum detects corruption;
it is not an authenticity mechanism. Version 1 omits the separate stored PCM count; the decoder retains
compatibility and uses its source count for both duration and PCM size. Version 2 retains timing and the
complete spectral data when reference PCM is removed. New scalar controls append to the original fourteen
indices; `Harmonic.1` through `Harmonic.96` store individual gains. Decoding bounds total bytes/counts and validates finite ranges/order
before compiling tables; unsupported/corrupt content leaves a silent device with a warning.

Generated phrase tracks use ordinary MIDI notes and a MIDI CC119 clip envelope (`midiController` index 121)
to select the exact captured example at each note. The envelope's target IDs are freshly allocated and survive
save/reopen. Values 1–64 select examples and 0 clears the override. Continuous source pitch is retained inside
the model, rather than authored as MIDI bend data. State, MIDI and native behavior are documented in
[RESYNTH.md](RESYNTH.md).

## Sonora track state, linked clips and inserted returns (TRK-1/2/5)

Verified 2026-10-17 against the reference app 12.4.6 (sets opened through `open -b`, Log.txt + window title, no crash report):
unknown `Sonora*` elements as the last child of a track element, a clip element or `LiveSet` are ignored silently. Saving
from the reference app drops them, so hidden/deactivated tracks come back as ordinary tracks and linked clips as independent ones.

- `<SonoraTrack Version="1" Hidden="true" Deactivated="true" />` (last child of MidiTrack/AudioTrack/GroupTrack/ReturnTrack;
  written only when a flag is set, removed otherwise). The mixer, devices and clips stay exactly as authored.
- `<SonoraAlias Group="N" />` (last child of MidiClip): clips sharing N share their notes in Sonora.
- Insert Return Track appends an authored `ReturnTrack`, one `TrackSendHolder` (Manual -inf, fresh AutomationTarget and
  ModulationTarget ids, `EnabledByUser` true on tracks and false on returns) per track, and a `SendPreBool Value="false"`.
  Returns only receive holders when their existing holders already match the return count (Sonora's own new sets author none).
  **13 returns crash the reference app while loading** (12 open), so Sonora caps returns at 12.

