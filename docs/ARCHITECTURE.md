# Architecture

## 1. Layers

```
            +-----------------------------------------------------------+
  app       | Main, MainWindow (menus), AppController (commands)        |
            +-----------------------------------------------------------+
  ui/gfx    | MainLayout -> views -> widgets   |  SkiaCanvasComponent   |
            | (pure Skia drawing, own widgets) |  MetalRenderer (GPU)   |
            +-----------------------------------------------------------+
  collab    | CollabSession  Sequencer/Client  Objects  Net (WebRTC)    |
            +-----------------------------------------------------------+
  engine    | AudioEngine  EngineGraph  ClipPlayers  Transport  Samples |
  devices   | DeviceProcessor  <- native devices, RackProcessor,        |
  plugins   |                     PluginDevice (VST3/AU via JUCE)       |
  midi      | MidiManager (inputs, MIDI learn)                          |
            +-----------------------------------------------------------+
  core/io   | model::Song (plain structs)  <-  AlsReader (.als)         |
            +-----------------------------------------------------------+
  3rd party | JUCE 8 (audio I/O, MIDI, plugin hosting, windows)  Skia   |
```

CMake targets mirror this: `sonora_core`, `sonora_engine`, `sonora_collab`, `sonora_ui`, `Sonora` (app).

**Collab sessions (COL-2..6, ADR 0009/0010).** `src/collab` sits on core/io/engine and below app; `ui` never includes it
(the views read plain `UiContext::collab` data). The Song is replicated as keyed objects encoded by `core/ModelCodec`;
`AppController::applyEdit` hands every finished local edit to the session, which shares the changed objects; remote
objects come back through `AppController::applyCollabObjects` and the usual `afterEdit` refresh. Live values (mixer,
device parameters) are polled from the engine at 30 Hz on the message thread and applied to the same atomics the UI uses.
Network callbacks (libdatachannel threads) only queue events; `CollabSession::poll()` handles them on the message thread.
Nothing touches the audio thread except `Transport::playNow/stopNow/seekNow` requests.
All JUCE modules are compiled once into `sonora_juce` (`cmake/SonoraJuce.cmake`).

Application updates (COM-4) live only in `app/`: `Updater.h` is platform-neutral, `UpdaterSparkle.mm` embeds Sparkle 2
(`third_party/sparkle`, copied into `Contents/Frameworks`), `UpdaterStub.cpp` serves other platforms. The updater asks the
session (playing/recording/rendering/unsaved) before it restarts; it never runs on or waits for the audio thread.
See [UPDATES.md](UPDATES.md).

**Platforms (COM-7).** macOS arm64 is the reference. Code that differs per OS sits behind small seams: header-only
`src/platform/CNumeric.h` and `Paths.h` (any layer), `sonora_platform` (`HttpClient`, `SecretStore`; used by the agent),
and per-OS source files picked in `src/CMakeLists.txt` (`MetalLayerView.mm` vs `MetalRendererNone.cpp`,
`AssistantDock.mm` vs `AssistantDockPortable.cpp`, ...). Linux and Windows render the Skia canvas on the CPU. Keep
`#if __APPLE__`/`_WIN32` inside those files. Seam table, build commands and per-platform gaps: [PLATFORMS.md](PLATFORMS.md).

## 2. Data flow when opening a set

```
File > Open / argv / browser double-click
  -> AppController::openFile            (message thread)
  -> io::loadAls                         (thread pool: gunzip + XML DOM + model)
  -> AppController::installSong          (message thread)
       close plugin windows; ui.modelChanged() with null pointers
       AudioEngine::loadSong(song)
         buildGraph(): per track -> PreparedClip[] + DeviceProcessor chain
                      (DeviceFactory: native / rack / plugin (instantiate + restore state) / placeholder)
         graph->prepare(sampleRate, blockSize)
         pending_.exchange(graph)        -> audio thread picks it up at next block
       ui.song/graph/controls = new; ui.modelChanged()  (views rebuild synchronously)
```

`model::Song` is edited only on the message thread through undoable `EditCommand`s. The audio thread never reads that mutable model. Live controls live in:
- `Transport` (atomics): play/stop/position/tempo/loop.
- `ControlBank` → `TrackControls` (atomics): volume, pan, sends, activator, solo, arm, monitor mode, packed input route, meters.
- `DeviceProcessor` parameters (atomics inside each device).

`AlsWriter` re-reads and patches the original set, preserving unknown XML. The app copies live mixer/native
values and hosted VST3 state to the model before saving. `devices::captureDeviceState` recurses through
rack per-model-child processor pointers (including null unsupported entries). Capture runs on the
message thread with JUCE's callback lock held. Native-window verification of nested hosted state remains ALS-10d.
Clip, Session and automation edits are built into a `ClipSet` on the message
thread and handed over with `ClipSwap`; vector swaps occur at the block boundary, retired data is deleted on
the message thread. `TrackNode::sessionAvailability` is a separate message-thread-only copy used by the
launcher/UI, so they never inspect vectors being swapped by audio.

Recording uses `RecordingManager` with grouped Arrangement capture and one `Recorder` per Session track:
immutable armed-track membership and packed source/channel selection for each take,
fixed-capacity note/controller storage and SPSC audio rings. MIDI controller events are raw-byte decoded
without message ownership; captured steps and descriptor IDs are materialized only after worker drain.
Immutable prepared controller lanes produce MIDI at sample positions, with a 65,536-byte output guard
and reserved routed-input/release room. Overflow is published atomically for the UI; full dense-note/rack
capacity and worst-case controller CPU auditing remain ENG-14. Mono sources write mono WAV, stereo/resampling write stereo.
`MidiManager` publishes a bounded `InputBlock` (1024 events, 64 KiB raw data) with source-port tokens; each
track filters port/channel into prepared buffers. No long-message ownership is constructed in the callback.
Per-sample prepared monitoring masks implement In/Auto/Off; Auto is inhibited over clip playback intervals
but active during replacement recording. External input is recorded before effects even with Monitor Off;
resampling reads completed Main audio before metronome and browser preview. Empty Audio tracks get authored
clip templates when take length is known; MIDI still needs a retained template. Device input pointers are
normalized back to physical channel indices, including sparse active channel sets. The recording worker does WAV I/O and publishes Finished after the audio thread releases
the take and the rings are drained. The app commits all clips as one undoable edit. `Recorder::captureRange` intersects each playback segment with its immutable punch region; the same
sample interval controls replacement playback/monitoring and capture. Pre-count/punch MIDI held-state uses
fixed arrays and is promoted to notes at the left edge, with closure at punch-out. Session takes ignore
Arrangement punch bounds. Takes end at explicit stop/seek, non-loop punch-out, capacity failure or device stop. Arrangement loops continue across wraps; Session recording remains a separate flow. Browser preview is mixed after the main output using decoded `SampleSlot`s and its own
pending/retired pointer exchange. Neither recording nor preview performs file I/O in the audio callback.

### Session capture and elapsed clock (ENG-9d)

`RecordingManager` publishes immutable banks of independently prepared Session recorders. Each bank owns
shared child lifetimes, but audio only exchanges raw bank pointers and accesses references; ownership
counts and bank destruction stay on the message thread. Every child has a reserved 1024-segment range
array, separate start/stop state, worker, input route and preview. `prepareBlock` consumes schedules before
track rendering, and capture uses those exact per-track ranges after Main processing. Message queries use
the message-owned registry; rendering uses the audio-owned bank. Finished children can be collected/edited
while others keep capturing. Target guards retain a scene through drain/result collection. Per-track stop
intent controls eventual relaunch independently. Collected previews survive bank updates until the model
swap preserves their UID and clock; retirement removes them only after audio publishes preview completion.
A lifecycle lock serialises preparation/retirement with device-stop notification, never audio rendering.
Failed unpublished groups are drained/discarded without reaching audio or leaving empty WAVs.

`Recorder::SessionPlan` snapshots armed membership, exact scene/expected UID, quantisation and Arrangement
loop bounds on the message thread. `prepareCaptureRange` consumes start/stop atomics once per playback
segment before track rendering; capture and monitoring use the same prepared interval. Stopped start may
be immediate independently of the later quantised stop. Session recording ignores Arrangement punch/wrap
closure. Existing MIDI targets continue playing with Auto input enabled; empty targets suppress old playback
at capture-in. Fixed storage/rings remain bounded; result merging and history commands run after drain.
Existing MIDI overdubs use an audio-owned `OverdubPreview` for ongoing playback. The message thread
prepares a clone of each exact scene/UID, reserves the original notes plus 65,536 captured notes and
parallel eligibility arrays, and allocates a shared 131,072-node controller step pool. Atomic pending/
retired bank exchange transfers ownership at block boundaries; the engine timer deletes retired banks.
The message thread never reads mutable preview notes or steps. Routed input is available before clip
rendering; completed notes replay on later passes, and touched controller passes commit at loop wraps.
Silent passes preserve earlier controller patterns. Live input remains a separate device path.
Preview playback continues through worker drain until the prepared recorded clip replaces the same UID.
That handover preserves Session elapsed time and existing voices; target replacement, stop and seek still
use normal release. The completed model edit remains one undo command. Preview output is bounded to
65,536 bytes; overflow releases voices on all channels and resets pedals/bend. Held notes are published
only once their endpoint is known. In-flight clip visuals, nuanced note overlap/wrap behaviour and empty-slot
capture handover remain open. Edits/history/launches that change a recording target are rejected through
worker drain. Stopping one track ends only its take; Stop All leaves new results stopped. Scene changes
and Back to Arrangement still require finishing Session capture.

`TrackNode::sessionElapsed` is an audio-owned unfolded clock reset at Session launch/stop, independent of
Arrangement loop position. `TrackControls::sessionElapsed` publishes progress for UI/envelope evaluation.
Queued actions use a shared sample-boundary helper; Arrangement loop boundaries prevent a longer global
quantisation from starving inside a shorter loop. The independent clock is verified against an own Live
Session recording; loop-boundary quantisation fallback is an implementation inference needing broader
reference timing checks. Audio never reads mutable model slot vectors or merges recorded MIDI notes.

Held Repeat launches publish a slot and the original quantisation with the queued launch. The audio-owned
`TrackNode::repeatRemaining` counts unfolded beats; `renderTrackClips` splits segments at every launch/retrigger
boundary, releases old voices/controllers and resets the audio stretch state. Fractional sample overshoot
is carried into the next interval, avoiding cumulative drift across blocks and Arrangement wraps. Mouse-up
clears the held slot without stopping the last pass. Stop/scene/other clip/Back to Arrangement cancel holding.
Recording or take audition also cancels holding before rendering, preserving the take's clock and voices.
Scene launches resolve explicit clip quantisation on the message thread; empty slots use global timing and
also cancel queued launches. No model data, timer, allocation or lock is added to callback scheduling.

`ControlTarget::SessionClip` resolves runtime UIDs on the message thread via `midi::sessionClipLocation`, so moves/history
retain the target while replacement clips cannot inherit it. Sidecar serialization uses `Song::savedClips`' current-file
track ID/slot rather than unsaved model positions; loading explicitly rebinds to newly allocated UIDs. Missing saved
identities remain disabled. Computer keys are another source on `Mapping`, coexist with MIDI, and stay per set.
`applySessionClipControl` shares mode/quantisation/velocity resolution and recording/drain guards between both inputs.
`MidiManager` retains note velocity, consumes mapped releases, edge-detects momentary CCs, and retains remote hold sources
through remapping/Remote disable until physical release. Non-clip note buttons keep their existing press-only behavior.
The widget tree supports virtual mapping hit targets for painted clip cells. Root owns computer-key holds, suppresses
OS repeat and releases them through JUCE key-state/focus events and view/document/modal transitions; text/modifier input
keeps priority. None of this routing runs in the audio callback.

Launch gain publishes with the queued slot and becomes audio-owned `TrackNode::sessionLaunchGain` only at the boundary.
Repeats/clip swaps retain it; ordinary mouse/key/scene launches restore unity. Audio scales only the rendered clip segment
before physical input merges. MIDI renderers, including overdub previews, scale note-on velocity while retaining note-off
velocity/controllers. Fully silent MIDI launches skip notes, avoiding accidental zero-velocity note-offs. Stored notes,
recorded input and monitored input are unchanged. No extra callback buffers, allocations or locks were added.

### Take-lane storage

`Track::takeLanes` contains independent clips with fresh UIDs and retained lane fragments; it is prepared
separately from the engine's main ClipSet. `appendTakeLanes` allocates monotonic lane IDs and clips,
retains source fragments across history, and sets `takeLanesNeedPatch` on apply and undo. The writer
rebuilds edited containers from immutable original metadata, preserving unknown non-clip events and
checking templates before output. Original sample directories are stored on each take clip for Save As.
Prepared take lanes hold their own clips/envelope bindings. T audition is exclusive per track; the selected
lane ID is published atomically at a clip swap boundary. Main/Session vectors stay intact during take-only
swaps, so inactive take edits do not release main voices. Pending main edits force a full preparation when
coalesced with a take edit. Active retired envelope bindings restore their base values before storage swap.
Audition temporarily overrides Session rendering while retaining its launcher state. Recorder sizes pass metadata
(4096 entries) and tempo marks (65536 entries) before capture. Both sides of every wrap are captured;
held MIDI is closed/promoted at pass boundaries. Audio shares one continuous WAV, with a source-seconds
→ unfolded-beats map prepared from tempo changes. Recorded clips default Warp On/Beats. On the message
thread, chronological passes become independent clips and lanes. `recordArrangementPasses` stages
successive replacements to retain pre-roll and previous remainder after a final partial pass, then commits
main and lane arrays as one edit. TakeId links copied comp sections to their source; IDs and recorder
TakeCounter advance monotonically across undo. Single Arrangement captures use this path too.
The independent summary `Result::clips` remains for legacy callers/Session; app prefers `Result::passes`.

### Editing, saving and browser drops

`model::ClipLocation` selects an explicit Arrangement/Session/take container plus track/UID and exact
scene or stable lane ID. UiContext retains compatible track/UID/scene fields plus a separate selected lane ID;
its location helper routes `findClip`/`modifyClip`/`setNotes` and shared editors to all three containers.
Gesture edits retain the original location. Commands check the Session slot UID and take lane identity. This keeps edits away from a replacement
clip in the same slot. Clip edits still publish immutable ClipSets without rebuilding devices. `takeLanesOnly` commands replace
only prepared take data; comp commands replace main clips. Range comp preserves source offsets/TakeId,
clips to source intersections, replaces main overlaps and retains their surviving sides as one history entry. MIDI
probability is a deterministic hash of prepared clip seed, note identity/content and loop occurrence,
so no generator state/locks/allocations are needed and note-on/off decisions agree across blocks.
Start markers before loop start now play a MIDI lead-in once before repeating the loop region.

Clip UIDs come from one atomic allocator shared by readers and edits. Each clip retains immutable source
XML shared by undo snapshots. `Song::savedClips` holds current-file locations separately; saves update this
map without rewriting command snapshots. The writer validates a replacement plan before touching the DOM,
then clones retained fragments and preserves unknown metadata. `stripSourceEnvelopes` and
`resetSourceMarkers` prevent fresh content from inheriting old envelopes/end markers on later saves.
The Clip tab's `ClipEnvelopeEditor` uses the same explicit locations for linked MIDI controller edits.
It keeps a message-thread gesture copy, and cancels if the source envelope/location changed before commit.
`setClipMidiEnvelope` retains other lanes, Float fractions and duplicate ordering; fresh lanes get a
beat-zero default anchor. Descriptor allocation is deferred until command apply (after recording guards),
then composed with the clip replacement as one stable-ID undo. Take-only edits retain their preparation
scope unless descriptors must be allocated. Draw gestures restore original left/right limits outside
painted cells and serialize jumps as paired Float points. Audio only sees the resulting immutable ClipSet.

File/device sample origins survive undo and Save As; app saves anchor existing project-relative references
when relocating, without copying media. Backup failure aborts replacement. `EditHistory` assigns unique
state IDs and records the saved ID so divergent branches cannot appear clean by having the same depth.

Whole-track duplication (UI-30) captures live mixer/device state, patches a fresh source DOM in memory, then clones
complete track elements. Global automation/pointee IDs and internal group/routing references are rebound before parsing
the clones with fresh runtime clip UIDs; retained XML includes unsupported metadata. One `insertTracks` command owns the
clones across undo/save/redo. Track reconciliation removes known Audio/MIDI/Group/Return elements absent from the model,
while retaining unknown track kinds. No temporary save or mutation of the source set is used to prepare the copy.

Piano-key audition publishes a monophonic atomic `TrackControls::previewPitch`; the audio-owned previous pitch generates
bounded note-off/on events in the preallocated device MIDI buffer. It bypasses arm/input-monitor masks and recording
input, with no callback allocation, lock or logging. The widget retains its control bank until release so focus/document
changes cannot send a note-off to a replacement track. Device/graph rebuilds use the existing pointer invalidation rules.

Browser rows carry sample paths/native tags/scanned plugin identifiers. Root owns the drag, routes release
through target widgets, paints its label and cancels on Escape. The app probes sample metadata on a worker,
checks the document generation on completion, and creates one undoable clip/device insertion. New native
devices/audio clips/VST3 wrappers use Sonora-authored `AlsTemplates`, not reference-app presets.

Device insertion prepares new processors on the message thread, briefly suspends the callback, moves
existing processor ownership into the new order and rebuilds clip/automation/PDC bindings. Existing
controls, processor state and transport survive. Before device mutation the app closes plugin editors and
invalidates UI pointers; only afterward does it rebuild views. Open Set completion is deferred during
plugin construction/state capture. Audio-device changes use the same pointer invalidation and reprepare
existing graph instances with the callback suspended instead of reloading stale model state. Queued reprepare
checks lifetime and whether preparation is still needed; synchronous preparation before Record prevents a
stale queued callback from aborting the new take. Microphone requests track document generation and engine
readiness; denied automatic requests are not repeated until an explicit input edit/arm/Record action.

## 3. Threads

| Thread | Runs | May |
|---|---|---|
| Message (main) | UI, commands, graph building, plugin instantiation/editors, deleting retired graphs | allocate, lock, log |
| Audio (CoreAudio) | `AudioEngine::audioDeviceIOCallbackWithContext` → clips → devices → mixer | **only** atomics + preallocated buffers |
| MIDI input (CoreMIDI) | `MidiManager::handleIncomingMidiMessage` | push to lock-free FIFO; `callAsync` for mapped CCs |
| Thread pool (2) | `.als` parsing, project discovery, plugin scanning, first-Warp tempo analysis | no UI access; post results with `callAsync` |
| Collab network (libdatachannel) | WebSocket signalling, ICE/DTLS, data-channel framing | queue events under a mutex; never touch the model |
| Collab hashing (1) | SHA-256 of shared samples | results collected by `CollabSession::poll()` |

First-Warp analysis (`AppWarp`, UI-41) captures the clip, document generation and history version on the message thread,
then decodes through SampleCache and estimates the sample's tempo on a worker (`engine/TempoAnalysis`, UI-47b: spectral-flux
onset envelope of at most 90 s from the middle, comb tempogram + subdivision coverage, whole-beat length prior for
loops; 1-45 ms per file in Release, also -O2 in Debug). Sample drops (UI-47) run the same estimator. Repeated
requests for the same clip coalesce; at most eight are pending. Completion rejects replaced documents, intervening edits
and busy/recording state, then commits through ordinary clip history. Known/manual tempos and authored multi-anchor
grids bypass detection. Neither file decoding nor tempo estimation runs on the audio callback.

### Graph hand-over (no locks, no frees on the audio thread)

```
message:  pending_.exchange(newGraph)            // replaces a not-yet-picked graph (deleted right away)
audio:    if retired_ == null && pending_ != null:
              retired_ = current_; current_ = pending_   // old one parked, never freed here
message:  Timer (10 Hz): delete retired_.exchange(null)
```
`AudioEngine::latestGraph()` (message thread) always returns the newest graph; only *older* graphs
are deleted, so UI pointers into the latest graph stay valid until the next `loadSong` or device-chain mutation.
That is why `installSong` and device-edit preparation invalidate UI pointers and call `modelChanged()` first.

### Monitor tap and offline render

- `MasterMonitor::push()` is called at the end of `renderBlock` with the Main output (before the click and the browser preview). It is
  wait-free (one atomic load while disabled, a copy into a 128k-frame ring while enabled). The "Sonora monitor" thread drains the
  ring, runs `dsp::LoudnessMeter / StereoMeter / SpectrumAnalyzer / Scope`, and publishes a `MonitorSnapshot` under a mutex that
  only the UI and that thread take. Nothing here allocates on the audio thread.
- Export: `AudioEngine::beginOffline()` (message thread) removes the device callback and consumes pending transport requests;
  a worker calls `renderOffline()` which drives `renderBlock` with exact block sizes until the caller says stop; `endOffline()`
  restores transport/loop/metronome and re-adds the callback. While `offlineStemSlot_` is set, `renderBlock` also copies each requested
  track's delay-compensated output into a stem buffer (same code path, no extra processing). The graph must not be edited
  meanwhile: the Export dialog is modal and `MainWindow` ignores menu commands while `AppController::exporting()`.

### Per-block processing (`AudioEngine.cpp`)

1. Swap graph if pending. Pop live MIDI from `MidiManager`.
2. `Transport::beginBlock(samples, samplesPerBeat)` → play/stop/seek/count-in requests. Count-in advances its
   own negative beat counter while holding timeline position and returns the first playback sample offset.
   Split post-count playback at every loop wrap into a fixed array (up to 1024 segments); MIDI clock Start
   uses that offset. Exact boundaries wrap and fractional-sample overshoot is retained to prevent clock drift.
   Record ranges are computed per segment before monitoring; capture consumes those same intervals after Main.
3. Tracks in `trackOrder` (children before their group): consume clip launches, render permitted clips, build
   monitoring masks → filter/add selected MIDI or physical audio input →
   devices → volume/pan → meter → sum into parent group or main → post-fader sends to returns.
4. Returns → main. Main devices + volume → recording capture (Main resampling / pre-effect external input)
   → output; metronome and browser preview are mixed afterward.
5. `Transport::advanceTo(endPos)`.

Clip renderers (`ClipPlayers.cpp`) are **stateless** functions of (clip, beat range) — seeking and looping
need no special handling, and they are unit-tested.

## 4. Devices

`DeviceProcessor` (src/devices/DeviceProcessor.h) is the only interface the engine knows.

- `NativeDevice` base: parameters as an array of atomics, loaded from `.als` by matching
  `ParameterInfo::id` with the flattened `.als` path (e.g. `Bands.3/ParameterA/Freq`).
  Non-finite parameter writes are rejected. Sonora FM uses 32 fixed voices/four oscillator envelopes;
  Sonora LFO reads atomic shape/point controls and applies sample-position volume/pan modulation.
  Sonora Velocity edits only MIDI note-on velocity bytes in-place. None allocates in process().
  Sonora Delay / Reverb carry Live's complete schema parameter lists (`docs/NATIVE_DELAY_REVERB.md`); their process() is allocation free
  (pre-sized `FractionalDelay` lines, `dsp::ModLfo`, smoothed controls). Devices with many controls describe a sectioned panel with
  `DeviceProcessor::uiGroups()` / `parameterShown()`; `DeviceBox` lays it out (captioned groups, knobs, toggles, cycle buttons).
  Sonora Curve LFO stores each of its 12 graphs as atomics (count, warp, x/y/tension per point); `process()` snapshots a
  graph lazily per block on the stack (no allocation), evaluates every destination per sample, updates the filter every
  16 samples and appends at most 32 CC events per block to the pre-sized MIDI buffer after the loop. Editors write graphs
  with `setGraph()` (point count last) and read the playhead through `displayPosition()`; `CurveGraph::eval` tolerates
  half-written edits.
  Sonora Wavetable (`SonoraWavetable`) has 32 fixed voices, three oscillators with up to 16 unison voices,
  immutable generated/mipmapped tables, four envelopes, ten LFOs, dual filters and a 64-slot matrix.
  It snapshots its 540 atomic controls once per block and updates derived voice controls on a persistent
  16-sample clock. Sample-offset MIDI splits do not reset that clock. Tables are built during construction,
  never in the callback. The DSP source is optimized in Debug too, to keep heavy unison usable.
  `WavetableEditor` provides all controls through seven sections and instance/page selectors.
- `RackProcessor`: chains in parallel; Drum Racks route notes by pad.
  Each chain keeps processor pointers parallel to model children, including unsupported nulls. Bounded
  per-chain MIDI overflow emits channel-wide All Sound Off; choke groups also stop one-shot samples.
- `VectorSynth`, `Impulse`, `Echo` (DEV-19) are compact Sonora-only devices. Vector owns generated immutable mipmaps
  and sixteen fixed voices; Impulse owns immutable sparse sample slots and thirty-two fixed one-shot voices; Echo
  allocates fractional delay rings in prepare. UI reads immutable samples/atomic parameters and sends Impulse audition
  through an atomic request. None allocates, locks, logs or reads editable model state during processing.
- `PluginDevice`: wraps `juce::AudioPluginInstance`.
- `Resynth` (RESYN-1): worker-only `engine/ResynthAnalysis` produces a bounded plain `core/ResynthModel`.
  `engine/ResynthEdits` creates an editable MIDI phrase and native instrument as one track edit. CC119 selects
  each captured articulation at the note sample, including repeated pitches; continuous source pitch stays in
  the model. The processor owns immutable harmonic mip tables and model/reference data, eight fixed mono/legato/poly
  voices and per-channel controller arrays. No ownership or memory allocation changes in process().
  Existing `NativeContent` embeds the checksummed `sonora-resynth-v2` payload (v1 remains readable), making Save As/Collect independent
  of the original file. `AppResynth` uses guarded/cancellable media jobs; its expanded editor uses atomic controls
  and structural model replacement for trimming/removing reference PCM. Spectral edits use bounded direct
  additive synthesis; default spectra retain the immutable table path. Synth never reads reference PCM. See `docs/RESYNTH.md` for scope and remaining qualification.
- `PlaceholderSynth`: sine synth used when an instrument cannot be created (so MIDI tracks are audible).

`DeviceFactory::createDevice` decides which one to build. `TrackNode::processorForModelDevice`
keeps a 1:1 mapping to `model::Track::devices` (nullptr = unsupported) for the UI.

`nativeDeviceModel` creates authored defaults from the same processor parameter catalog. Device structure
edits copy synchronized state, assign a new top-level device ID (forcing just that processor to rebuild),
allocate unique child/automation identities and use `replaceDevice` history. Sample metadata is read on
workers; completion validates document/device identity before editing. Audio never reads editable zones.
`SamplePart` and rack `Chain` retain XML fragments so edited containers preserve unparsed fields.
`samplePartsEdited`/`chainsEdited` author only the changed containers; untouched sets retain identical XML.

## 5. Plugins

- Formats: VST3 (primary), AU (compiled in). VST2 is **not** hostable (SDK not licensable): VST2
  references are matched to the VST3 of the same name with default state (PLG-5).
- State restore: VST3 `.als` state = raw `IComponent`/`IEditController` blobs → `Vst3PresetBuilder`
  wraps them as an in-memory `.vstpreset` → `getVST3Client()->setPreset()`. **Verified** with Serum 2
  and Vital.
- Scanning: out-of-process helper with timeout/blacklist, cached in
  `~/Library/Application Support/Sonora/PluginCache.xml`. See PLG-3.
- Per-user files (settings, plug-in cache, mappings, agent settings, untitled projects, automation registry) all resolve
  through `core::appDataDirectory()` (`src/core/AppDataFolder.h`): `Application Support/Sonora`, or `Sonora Nightly` in
  nightly builds (COM-4c, UPDATES.md).
- Read-only native state inspection: `plugins/SerumState.*` decodes observed version-9 Serum 2
  `XferJson`/Zstandard/CBOR states on the load/message thread with explicit memory/node/depth budgets.
  Original bytes, sparse defaults and unknown values are retained; a decoded state is not a native patch.
  Zstandard is pinned and statically built from source. `plugin_probe --inspect-file` isolates plugin
  execution with a watchdog; `--decode-serum-state` uses the native decoder without loading Serum.

## 6. Rendering (look & feel)

Live 12 draws its whole UI itself with **Skia**. Sonora does the same:

- `ui::Widget` tree (own toolkit, ~300 lines), painted with `SkCanvas`.
- `gfx::SkiaCanvasComponent` is the single JUCE component of the main window. It hosts a
  CAMetalLayer view (`MetalRenderer`, Skia Ganesh/Metal) and forwards JUCE input to `ui::RootWidget`.
  Keyboard dispatch gives the visible focused widget and its ancestors priority; root modal/map handling
  retains priority. macOS Command shortcuts fall back to JUCE key codes when text characters are absent.
  `SONORA_RASTER=1` forces the CPU path (Skia → juce::Image).
- Frame pacing: `juce::VBlankAttachment`; widget `tick()` at ~30 Hz for animation (playhead, meters);
  a frame is rendered only when the root is dirty. `root.layout()` runs before each paint so views
  rebuild for the current song before drawing.
- Colours come from `ui::Theme` tokens named like Live's `.ask` theme keys, so a user's Live theme
  can be imported 1:1 (Options > Load Live Theme).
- `DeviceFace` (UI-33) owns dedicated FM/sample/dynamics/Vector/Impulse/Echo compact faces. Parameter widgets use
  the existing DeviceBox MIDI/macro tagging callback; structural sample edits use `UiContext::editDevice` and may
  synchronously destroy the face. Rack macro/pad/chain editing remains in DeviceBox/NativeDevicePanel.
- Off-screen rendering for tests/tools: `gfx::renderToPng`.

Plugin editor windows are normal JUCE `DocumentWindow`s (native plugin views can't live inside Skia).

## 7. Adding things — recipes

### Application control and Agent Control

`sonora_control` (`src/control/`) depends on core/engine and builds with UI off. `ControlService` validates versioned
JSON requests, explicit instance/document/revision targets, staged edit batches, idempotency receipts and guarded undo.
`AppControl.cpp` supplies message-thread adapters to `AppController::applyEdit`, history and live controls. Gain commands
carry a separate `gainTracks` publication list so gain-only edits do not rebuild clips or release notes. Hosted queries
use full names and host IDs through `describeHostedParameters`, independently of the generic painted device UI.

Agent Control is an AppKit sibling of the Skia canvas, hosted by the main window (ADR 0008). Its default MCP view has no
composer. The optional native LLM reveals one only after explicit provider/endpoint/model/Keychain setup. `sonora_agent`
depends on `sonora_control` and also builds without Skia. Its C++ worker owns bounded provider conversation framing and
ephemeral NSURLSession HTTP; every tool call marshals to the message thread and checks its original document binding.
Responses, Chat Completions and Anthropic Messages share the API schema and execution boundary. Provider failure and
cancellation do not roll back committed edits or block normal app use. This first integration is in-process; bundled
helper isolation, live-model qualification and usage/cost UI remain AGT-6 work. See `agent/NATIVE_LLM.md`.

The qualified Serum recipe prepares a fresh real VST3 instance and captures restorable
state before adding a new MIDI track. JUCE's VST3 processor-state route synchronizes host parameter caches after preset
restore; the direct VST3-client preset method alone leaves those caches stale in JUCE 8.0.16.

General MIDI edits stage existing `ClipLocation`/`setNotes` commands (including Session and takes) and publish ordinary
immutable clip swaps. Generic VST3 writes capture actual live state under callback exclusion, then prepare and reload-
verify isolated instances outside that lock (`control/HostedParameters`). They commit through device replacement
history with a fresh device ID and captured-live undo; they are coarse patch edits, not realtime parameter streams.
No parameter preparation or serialization runs in the audio callback. See `agent/CONTROL_API.md` for exact semantics.

`control/DeviceControl` now supplies the same production adapters to the app and offline probes: implemented native/
rack descriptors, scanned VST3 catalogue, initialized insertion, captured-live removal and native parameter preparation.
Native writes use exact processor paths/names and plain units or explicit linear normalization. They round-trip through
the factory/model before replacement and preserve authored automated bases. Native/rack/content/child state comparisons
extend guarded undo beyond opaque VST3 bytes. Track/order/instrument conflicts are checked by ControlService; no GUI
coordinates or per-plugin adapters are involved. AppControl wraps capture/preparation against nested device/document
edits. Existing core insert/remove/replace history and engine graph publication own mutations. This slice does not add
the prepared-device operations to the collaboration Op vocabulary (COL-1) or expose nested targets/media authoring.

Local automation is disabled by default. `LocalEndpoint` provides bounded, same-user authenticated Unix IPC and dispatches
requests to the message thread. It is revoked on document replacement. The external stdio MCP bridge uses the same service;
all socket/JSON work stays outside the audio callback. See `agent/CONTROL_API.md` for current scope and limitations.

### Serializable operations (COL-1)

`model::Op` (`core/Op.h`) is a plain, versioned intent addressed by TrackId, clip UID plus container (`ClipRef`) and
per-clip note key. `opToCommand(song, op)` validates against the current song and returns the existing `EditCommand`;
nothing else in history/engine/writer changes, and indices are resolved locally on each replica. `MidiNote::key` is a
runtime identity (never saved) that is unique within a clip: `assignNoteKeys` gives unkeyed or duplicated notes keys
above the clip's highest, deterministically. The reader, `modifyClip`, `insertClip` and `setSessionClip` assign keys;
`keyedNotes(clip)` shows the keys the next note edit will store. Clip UIDs come from a process-wide allocator, so
`recordOp` captures the UIDs an op allocates (build plus first apply) through a thread-local `ClipUidScope`, and
`replayOp` hands back exactly those values on a replica, reporting a mismatch instead of diverging. `core/OpJson`
is the strict wire form; `opMessageToString` keeps doubles exact. Control `edits.apply` already runs through ops.
Routing UI call sites through ops, more op types and the join snapshot's identity map are COL-1/COL-2 work.

**A native device**: subclass `NativeDevice` in `src/devices/native/`, parameter ids = `.als` paths
(check with `als_dump --params`), register in `DeviceFactory::createNative`, flip `implemented` in
`DeviceCatalog.cpp`, add a DSP test in `tests/test_devices.cpp`, add a DeviceBox body in
`DetailView.cpp` (or rely on the generic knob layout).

**A new `.als` field**: add it to `core/Model.h`, read it in `AlsReader.cpp` (helpers `at/str/num/integer/boolean`),
extend `tests/fixtures/minimal_set.xml` + `tests/test_als.cpp`, run `als_dump --summary` on the real sets.

**A view/widget**: subclass `ui::Widget`; build children in a `rebuild()` keyed on `ctx().song`;
use theme tokens (never hard-coded colours except meter colours); check with `ui_snapshot`.


### UI-34 track structure gestures / UI-35 note expression

RootWidget owns an optional message-thread track-drag payload. Session/Arrangement return local drop targets;
Root validates the tree plan, paints the insertion/group frame and commits a single `moveTracks` command on release.
The model command retains order/parent/default-routing state by track ID, expands selected groups and rejects cycles
or non-contiguous subtree results. Existing structure-change capture/remap/rebuild preserves live device/mixer state.

`Clip::noteExpressions` is separate from fixed-size `MidiNote`; realtime recording/overdub never copies expression
vectors. Preparation copies lanes and allocates member channels outside audio. Rendering reads immutable points,
quantises pitch/pressure/slide changes and writes raw bounded MIDI bytes. The existing overflow path releases the track;
stop/seek/loop paths now release all 16 note channels. Per-clip allocation, fixed bend range and linear interpolation
remain qualifications in UI-35b. MpeEditor stages a full message-thread gesture and guards observed note/expression data
before committing. The writer rebases expression XML on immutable source metadata before overlaying edited lanes,
which preserves unknown original data and handles Save -> Undo -> Save of newly authored lanes.

### Rendered media editing (UI-38)

`AppMediaEdits` takes a message-thread model/device-state snapshot, shows the export modal and marks the shared
editing backend busy. Consolidation/analysis use worker-side `ClipRender` and `AudioToMidi`; Freeze/Bounce detach the
audio callback and use the existing graph via pre-mixer offline stems. Only after success does one EditCommand commit.
Project changes/Finder drops are refused while processing. Generated files remain for history and are project media.

Freeze retains the original Track and owns separate cached Arrangement/Session clips. Graph construction substitutes
`frozenPlaybackTrack` (Audio/no devices), retains mixer/send automation and uses the cache's tempo markers. Missing
cache files fall back to originals. The XML extension and clip refs are documented in ALS_FORMAT. Offline blocks
copy into preallocated stem buffers; new analysis/render/file/model allocations occur outside the audio callback.


### Native expansion and compact device layout (DEV-20/21 / UI-39)

Plain Remoteable/user settings are explicitly marked in native descriptors and round-trip through `Device::plain`;
Timeable values continue through `Parameter::path`/Manual/automation. Tuner keeps its analysis ring, YIN difference
arrays and atomic results inside the processor; the UI reads atomics and keeps its own display history. Analysis and
MIDI-transform kernels retain Debug symbols with normal floating-point semantics and `-O2`, like the existing synth
kernels, to keep bounded callback work practical. No worker signaling, allocations, locks, logging or strings were added
to native processing. MIDI note effects prepare their scratch buffer, cap output at 64 KiB and copy into the engine/rack
MIDI buffers reserved in `prepare`. Other callers must reserve the same output capacity before processing generated MIDI.
Held source/channel-to-output mappings retain note releases and pressure routing after parameter/bypass changes; overflow
clears the working output and emits channel panic from fixed storage.

`MainLayout` chooses the stock Devices height independently of the saved clip/editor height and disables that divider.
The explicit docked Sonora Wavetable dashboard (including the selected rack child) is an expanded editor and keeps the
existing resizable layout. Resynth uses a fixed 1080 x 368 content area and 404-high dock with the divider disabled;
its device frame retains its height when horizontal scrolling appears, without changing saved heights for other editors.
Mode/placement changes invalidate this decision. Structural sample readouts commit through
`editDevice` once on release; callbacks can destroy the face, so button/gesture dispatch must avoid post-commit access.
