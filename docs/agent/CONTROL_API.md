# Sonora control API version 1.0

This is the implemented control surface (AGT-7e, MCP 0.6): inspection of the whole project, song/track/clip/take-lane edits, MIDI edits, native/rack/VST3 parameters (rack children included), device insertion/removal/move on tracks, returns and the main track, and live actions (mixer, transport, launching, recording, save, save a copy, audio export, plug-in scan, device switch and editor, view, media jobs, audio import). All calls execute on the message thread; JSON decoding and socket I/O use a
worker. The audio callback receives the existing graph/clip updates and atomics. The API is headless-capable through
`sonora_control`; the app supplies adapters to its live engine/history. The broader plan is in `../AGENT_DESIGN.md`.

The request envelope and operation argument shapes are in `schemas/control/v1/request.schema.json`. The native
service also validates values at execution; the MCP adapter publishes tool schemas for clients. Inspect reports both
`gainLinear` (live where available) and `authoredGainLinear` (stored model value).

## Envelope and identity

```json
{
  "apiVersion": "1.0",
  "operation": "edits.apply",
  "target": { "instanceId": "from-inspect", "documentId": "from-inspect" },
  "expectedRevision": "opaque-string-from-inspect",
  "idempotencyKey": "unique-run-step-1",
  "arguments": { "commands": [{ "type": "tempo.set", "bpm": 128 }] }
}
```

Application and document IDs are fresh UUIDs per launch/install. Track/device IDs and clip UIDs are numeric and valid
only in the explicitly targeted document. Names are display labels. Reopening the same file invalidates earlier handles.
The revision is an opaque string. In the live app it includes history and a conservative fingerprint of current mixer
and top-level processor values, preventing undo/write across detected manual changes. Automation or plugin self-updates
can also invalidate this first revision implementation; re-inspect rather than force the edit.

Mutations require exact target, revision and an idempotency key. An identical retry returns the retained original receipt,
even after revision advances; a changed request with the same key fails. Up to 256 mutation receipts are retained per
document without eviction/replay. Application restart loses the journal; never retry uncertain writes against a new
launch. Idempotency is not crash-proof exactly-once persistence.

## Operations

Every operation is one of three classes. `capabilities.get` lists what the running host implements (generated from the same
tables the dispatcher uses; a live action appears only when the host supplies it).

* **Queries**: target (`instanceId`, `documentId`) only, no revision, no idempotency key. `project.inspect` and
  `capabilities.get` need no target.
* **Mutations**: target + `expectedRevision` + `idempotencyKey`; one undo step, receipts, `undoToken`, `saved:false`,
  `audioVerified:false`.
* **Live actions**: target only. No `expectedRevision` or key (sending one is `invalid_request`), because playback and
  automation move the revision fingerprint constantly. Values are absolute, so repeating a request is harmless. They are not
  undoable: the result says `"undoable": false` and returns the previous values. Status is `applied`.

| Operation | Class | Arguments | Behavior |
| --- | --- | --- | --- |
| `project.inspect` | query | none | See "Inspection". |
| `capabilities.get` | query | none | Operations, edit types, live actions actually available, limits. |
| `status.get` | query | none | Transport, recording, export, plug-in scan, busy text, dirty flag, path, undo/redo labels. |
| `clips.get` | query | `trackId`, `clipId`, optional `scene` OR `laneId`, `offset`, `limit` | Paged notes with revision-scoped indices, total, `hasMore`, loop bounds. Default limit 4096. |
| `automation.get` | query | like `automation.set` addressing + `offset`, `limit` (1-1024, default 512) | All breakpoints of one lane, paged. Tracks, returns and the main track. |
| `devices.parameters` | query | `trackId`, `deviceId` | Native/rack/VST3 parameters; `deviceId` may be a device nested at any depth in a rack (read only). Tracks, returns, main. |
| `devices.catalog` | query | optional `filter`, `kind: native/vst3/vst2/au`, `offset`, `limit` | Insertable native devices and scanned VST3, VST2 (`vst2:`) and Audio Unit (`au:`) plug-ins, role and version. Default 64, maximum 128. |
| `edits.apply` | mutation | `commands`, 1-32 | Validate against a staged model, then commit one undoable batch (edit types below). |
| `devices.parameters.set` | mutation | `trackId`, `deviceId`, `parameters` (1-128 exact ID/name pairs with `normalized` OR `value`) | Top-level native/rack/VST3 devices on tracks, returns and main: capture live before-state; prepare and reload-verify isolated state; commit. Returns the NEW `deviceId`. VST3 accepts normalized only. |
| `devices.insert` | mutation | `trackId`, exact `catalogId`, optional `position`, optional `replaceInstrument` | Insert on a track, return or main (audio effects only on returns/main). `replaceInstrument:true` swaps an existing instrument in one undo step (default is `instrument_conflict`). |
| `devices.remove` | mutation | `trackId`, `deviceId` | Remove a top-level device, retaining its captured live state for undo. |
| `devices.move` | mutation | `trackId`, `deviceId`, `position` (reorder an audio effect) XOR `toTrackId` | One undo step. Moving to another track places the device like a browser drop and returns the NEW `deviceId`. |
| `clips.import_audio` | mutation | `paths` (1-16 absolute existing audio files), optional `trackId` (audio track), `startBeat` OR `scene` | Mirrors the sample drop: new audio track without `trackId`; tempo detected as the app does; one undo step; receipt lists `tracks` and `clips`. Reads files only. |
| `history.step` | mutation | `direction: undo/redo`, `expectedLabel` | Undo/redo the application history, only when `expectedLabel` equals the label `status.get` reports (`undo_conflict` otherwise). |
| `history.undo` | mutation | `undoToken` | Undo only if the agent edit is still the latest unchanged state. |
| `workflow.serum_supersaw` | mutation | `root: "D#"`, `mode: "minor"`, `bars: 4`, `startBeat` | Prepare the qualified real Serum patch and insert a new MIDI track with 12 notes over 16 beats. |
| `mixer.set` | live | `trackId` (track, return or main), `gainDb?` -70..6, `pan?` -1..1, `muted?`, `solo?`, `armed?`, `sends?` [{`sendIndex`, `gainDb` -70..0}] | Writes the live mixer exactly as the UI does. `automation_conflict` when the control has an envelope. |
| `devices.set_enabled` | live | `trackId`, `deviceId`, `enabled` | The device power button (top-level device, tracks/returns/main). |
| `transport.set` | live | `action?` play/stop/continue/return_to_start, `positionBeats?`, `metronome?` | Mirrors the Play/Stop/Continue/Return menu handlers. Asynchronous: read `status.get`. |
| `session.launch` | live | `action` clip/scene/stop_track/stop_all/back_to_arrangement, `trackId?`, `scene?` | Launches at the clip's launch quantisation. |
| `recording.set` | live | `mode` arrangement/session, `recording` | Asynchronous: poll `status.get` `recording`. |
| `project.save` | live | none | Saves in place; `untitled_document` when a dialog would be needed (and for collaboration guests). Receipt `saved:true`, `path`. |
| `plugins.scan` | live | `includeAudioUnits?` | Starts the app's scan; asynchronous (`status.get` `pluginScan.running`). Loads third-party code in-process. |
| `project.save_copy` | live | `path` (absolute `.als`, parent exists, must not exist) | Writes a copy (samples copied in) like Save a Copy; the open project keeps its path and unsaved state. |
| `project.export_audio` | live | `destination`, `target` main/track/stems, `trackId?` (target track), `startBeat`, `endBeat`, `format` wav/aiff/flac, `bitDepth` 16/24/32 (32 = WAV float), `normalize?`, `mono?`, `dither?` (default on), `tailSeconds?` XOR `autoTail?` | Through `beginExport`. `destination` must not exist, its parent must, the extension must match the format; for `stems` it is a NEW folder. Asynchronous: `status.get` `export`. Refused while recording; edits are `busy` while it runs. |
| `project.export_cancel` | live | none | Cancels the running export (`invalid_request` when none runs). |
| `view.set` | live | `mainView?` session/arrangement, `detail?` clip/devices/hidden, `selectTrackId?`, `selectClip? {trackId, clipId, scene?}`, `browserVisible?`, `mixerVisible?`, `followPlayhead?` | The View menu's own `ui` fields. The receipt's `previous` has the earlier values. Does not touch the project. |
| `devices.open_editor` | live | `trackId`, `deviceId` | Opens the editor window of a hosted top-level plug-in (VST3/VST2/AU). Native devices have no window (`unsupported_device`). |
| `media.run` | live | `action` + `trackId` (+ `clipId`, `scene`, `mode`) | freeze, unfreeze, bounce_in_place, bounce_new_track (track jobs); reverse, crop, toggle_warp, audio_to_midi (`mode` melody/harmony/drums/slices), strip_silence (audio clips). Asynchronous: `status.get` `mediaJob`. A successful job is one normal history step (`history.undoLabel`). |

Inspection/capability discovery does not require an envelope target. Other queries and live actions do. A local socket is already bound
to the enabled document, and changing documents revokes that endpoint. The MCP adapter additionally checks returned identities.

Supported edit types are `tempo.set {bpm: 20..999}`, `track.set_gain {trackId, gainDb: -90..6}`,
`clip.duplicate {trackId, clipId, destinationBeat: 0..1000000}`, the note commands below, the locator commands
(see "Arrangement locators"), the group, clip and automation commands of "Groups, clips and automation (AGT-7d)" and the
song, track, clip and take-lane commands of "Structure and song edits (AGT-7e)".
Gain is absolute dB, not a relative change. An agent asked
to lower a track by 3 dB must read its current gain and calculate the absolute target. Gain updates participate in
history, live engine controls and save state; tracks with authored volume automation are rejected for this operation.

Musical times are quarter-note beats, with half-open clip regions. Four 4/4 bars are 16 beats. Source audio seconds and
sample offsets are separate concepts; no source-audio editing is exposed yet. The Serum recipe keeps project tempo and
requires 4/4. It assumes D# natural minor when a built-in request says only D# scale; the receipt states that assumption.

Implementation (COL-1, ADR 0009): every `edits.apply` command decodes to a `model::Op` (`src/core/Op.h`, wire form
`core/OpJson.h`), the same serializable intent collab peers will exchange, and `opToCommand` builds the undoable command.
The public note API still uses `noteIndex`; the service translates indices to per-clip note keys against the staged
song. A `trackId` that no longer exists reports `target_replaced` for every edit type (previously some returned
`invalid_request`). The op vocabulary's `track.rename`, `track.set_color` and `loop.set` are exposed as edits since AGT-7e.

### Groups, clips and automation (AGT-7d)

**Groups.** Every inspected track has `kind` (`audio`, `midi`, `group`) and `groupId` (its parent group, -1 at top level).
A group track holds no clips of its own: its content is on the tracks in its `childTrackIds` (nested groups appear there
too). `folded` tells whether the group is closed in the views. `track.set_folded {trackIds?, folded}` opens or closes
groups; omitting `trackIds` applies it to every group (collapse all / expand all). Only group IDs are accepted.

**New and copied clips.** `clip.create {trackId, startBeat | scene, lengthBeats: 1/64..65536, name?, notes?}` makes a
MIDI clip on a MIDI track, in the Arrangement at `startBeat` or in the empty Session slot `scene`. Notes use the same
fields as `notes.add` and clip-relative beats. `clip.copy {trackId, clipId, toTrackId?, destinationBeat}` copies an
Arrangement clip (audio or MIDI) to a track of the same kind, which is how an empty audio track gets content.
`clip.move {trackId, clipId, toTrackId?, startBeat}` and `clip.delete {trackId, clipId, scene? | laneId?}` are exposed too.
`clip.create` and `clip.copy` refuse an Arrangement region that any clip already overlaps (`region_occupied`); they never
trim other clips. The receipt lists every clip the batch created in `data.clips` (`trackId`, `clipId`, beats or `scene`).
Audio clips cannot be created from files through this API.

**Automation.** Inspect lists each track's lanes in `automation[]`: `targetId`, `target` (`volume`, `pan`, `activator`,
`send` with `sendIndex`, `device` with `deviceId`/`parameterId`, `tempo`, `midi`), `pointCount`, `stepped` and up to
64 `points` (song beats; the pre-roll value is omitted). Return tracks list theirs under `returnTracks[]`, whose order is
the `sendIndex` order.

`automation.set {trackId, target, sendIndex?, deviceId?, parameterId?, points, rangeStartBeat?, rangeEndBeat?}` writes the
track envelope of one target. `target` is `volume`, `pan`, `activator` (track on/off), `tempo` (main track only), `send` (+`sendIndex`) or `device`
(+`deviceId` and `parameterId` from `devices.parameters`, or `"on"` for the device switch, also inside racks). `targetId`
from inspect may replace `target`. Each point has a song `beat` and either `value` or `gainDb`:

- volume and sends: `value` is linear gain (1 = 0 dB; volume up to 1.995 = +6 dB, sends up to 1), or use `gainDb`
  (-70 dB and below is silence);
- pan -1..1; activator and device on/off 0 or 1;
- native device parameters: the descriptor's `minimum..maximum` in its own units (Hz for a filter frequency); switches and
  choices automate as stepped lanes;
- VST3 parameters: normalized 0..1, and only parameters the set already has an automation target for.

Without a range the lane becomes exactly the given points (the value before the first point holds the first point's
value). With `rangeStartBeat`/`rangeEndBeat` only points inside that span are replaced: points outside are kept and the
lane's previous value is pinned at both edges unless a point sits exactly there, so the lane outside the range is
unchanged. A new lane starts from the target's current manual value. `automation.clear {trackId, target...}` removes a
lane. Automated volume makes `track.set_gain` return `automation_conflict`; automated device parameters are likewise
rejected by `devices.parameters.set`.

Example: filter the kick out over bars 65–72 of a 4/4 song (beats 256–288) with a native Auto Filter, then open it again:

```json
{"commands":[{"type":"automation.set","trackId":19,"target":"device","deviceId":31,"parameterId":"Filter_Frequency",
  "rangeStartBeat":256,"rangeEndBeat":288,
  "points":[{"beat":256,"value":20000},{"beat":287.9,"value":40},{"beat":288,"value":20000}]}]}
```

If the track has no filter yet, insert one first with `devices.insert` (`native:AutoFilter2`). A volume fade-out uses
`"target":"volume"` with `gainDb` points.

### Arrangement locators (UI-3b)

`project.inspect` lists `locators` (sorted by time): `locatorId` (the file's Locator `Id`, stable across edits and saves of
the same document), `name`, `beat` (quarter notes), `color` (palette index 0–69, -1 = default locator colour; a Sonora
extension saved as `<SonoraLocators>`), `songStart` and `infoText`. `edits.apply` accepts:

- `locator.add {beat: 0..1000000, name?: ≤256 bytes, color?: -1..69}` — unnamed locators are numbered like the Set
  button's ("1", "2"…). Only one locator per beat: adding onto an existing one is `invalid_request`. The receipt's
  `data.locatorIds` lists the new IDs in creation order.
- `locator.update {locatorId, beat?, name?, infoText?: ≤4096 bytes, color?, songStart?}` — at least one field. Moving onto
  another locator's beat is rejected; `songStart: true` clears it on every other locator.
- `locator.delete {locatorId}`.

An unknown `locatorId` is `target_replaced`. A batch is one undo step like any other edit. The Arrangement's Set/Del button,
ruler gestures and menus build the same `model::Op`s through `ui/Locators.cpp`.

All edits are rejected while recording/draining or during export/device state work. Dry-run prepare/commit, durable
jobs, nested plugin writes and remote access remain future work (see "Not exposed").
A missing device currently returns an empty parameter list rather than implying a writable target.

### Native instruments, effects and rack controls

The same generic parameter operations now cover every implemented native catalogue device, including top-level rack
macros and chain selector. Native descriptors use exact parameter paths and full names, `kind: native`, `value`,
`minimum`, `maximum`, `defaultValue`, `unit`, `choices`, `steps`, `writable`, `shownInCurrentMode` and `normalization: linear`. The `valueDomain`
is `processorPlain`: values follow the processor's actual descriptor scale. Unit labels are hints; percentage-like
controls can use ratios (for example Width defaults to 1 with range 0–4). Do not assume a raw value of 100 means 100%.
Normalized values map as `minimum + normalized * (maximum - minimum)`, independently of the UI knob's drag law.
For Hz/dB controls, sending `value` avoids inventing a normalized conversion. Toggles/enums must use a valid discrete
value; invalid batches commit nothing. VST3 descriptors report `kind: vst3` and accept `normalized` only.

Preparation captures live native values, preserves authored manual bases for automated parameters/bypass, edits an
isolated processor and verifies its model round-trip. Commit replaces that processor with a fresh ID; live readback
is checked and a failed commit rolls back. Undo restores the captured live before-state. Parameter-target automation
in track, Arrangement, Session or take envelopes is rejected. Rack writes conservatively reject any authored descendant
automation because whole-rack reconstruction can affect mapped controls. Native undo also checks current parameter,
bypass, content and child state, so intervening changes are not silently overwritten.
Live hosted children in a rack must provide capturable state; an unsuccessful child capture fails the operation rather
than retaining stale bytes. Unsupported children already skipped by the graph retain their original opaque XML/state.

This exposes registered scalar controls, including settings that a native implementation only retains. State readback
does not establish DSP parity or audible effect for every parameter. Waveform/table imports, source media, sample-zone
editing, custom curves/content, routing and individual nested-device targets are separate operations and remain outside
this API. Empty sampler/Resynth devices need content before they can sound; catalogue `contentRequired` states that.

### Device catalogue, insertion and removal

Discover identities through `devices.catalog`, then pass an exact `catalogId` (for example `native:StereoGain`) to
`devices.insert`. VST3 IDs come from the current scan cache, not guessed names or arbitrary file paths. Plugins must be
installed/scanned; no per-plugin code integration is required. Rescanning is the live action `plugins.scan`; there is no installer. Native catalogue
entries require an implemented factory; unimplemented placeholders are not advertised. VST2 (`vst2:`) and Audio Unit (`au:`)
entries take the same model/state capture a browser drop does; they have no isolated parameter round-trip, so
`devices.parameters.set` stays limited to native, rack and VST3 devices.

Instruments/MIDI effects require a MIDI track. Defaults put MIDI effects first, the instrument next, audio effects last.
Explicit positions must retain that order. Adding a second instrument fails with `instrument_conflict`; pass
`replaceInstrument: true` (one undo step, the old instrument is kept with its live state for undo), remove it first, or
choose an empty MIDI track. Create tracks with `track.create`. Return tracks and the main track hold audio effects only. Agent insertion is limited
to 64 top-level devices per track. Native defaults and verified initialized VST3 state are authored as ordinary devices;
the app then checks that the actual processor exists. Inserts, parameter batches and removals are separate transactions.

Removal captures current live state before deleting the device. Undo restores its ID, position and state even after
save; redo removes it again. Source XML/unknown content and source media ownership remain with the existing model/writer.
Plugin loading/state work is still in-process and may interrupt voices or block on a faulty third-party plugin.

MCP 0.6 (AGT-7e) adds the tools of the table above (`sonora_status_get`, `sonora_automation_get`, `sonora_mixer_set`,
`sonora_transport_set`, `sonora_session_launch`, `sonora_recording_set`, `sonora_clips_import_audio`, `sonora_devices_move`,
`sonora_devices_set_enabled`, `sonora_plugins_scan`, `sonora_project_save`, `sonora_history_step`) and extends
`sonora_edits_apply`, `sonora_devices_catalog` and `sonora_devices_insert`. MCP 0.5 added the AGT-7d group, clip and automation commands to `sonora_edits_apply`; MCP 0.4 (locator commands) also exposes `sonora_devices_catalog`, `sonora_devices_insert`, `sonora_devices_remove` and the extended
`sonora_parameters_get/set`. The configured native LLM has the identical operations through its document-bound adapter.
Reload the MCP bridge and use the rebuilt application to load the new tool/service versions.

### General MIDI notes

The `edits.apply` batch also accepts the following commands. Each uses `trackId`, `clipId` and optionally **either**
`scene` (zero-based Session slot) **or** `laneId` (stable take-lane ID). Omitting both targets Arrangement. The UID must
match the exact container; replacing a Session slot never redirects an old handle. `project.inspect` exposes
`clips`, `sessionClips` and `takeClips` on each regular track.

- `notes.add {notes}` appends 1–4096 new notes.
- `notes.update {updates}` changes 1–4096 existing notes. Each update has a `noteIndex` and at least one changed field.
- `notes.delete {noteIndices}` deletes 1–4096 distinct indices.
- `notes.replace {notes}` replaces the whole note array (0–4096 entries); an empty array intentionally clears it.

A new note requires `pitch` (integer 0–127), `startBeat` (clip-content beats, not Arrangement time), `durationBeats`
(positive, at least 0.000001) and `velocity` (0–127, fractional allowed). Optional fields are `releaseVelocity` (default 64),
`probability` (default 1, 0–1), `enabled` (default true), and `velocityDeviation` (default 0, -127–127).
Times and note ends are bounded to 1000000 beats. Edits do not resize the clip or change its loop; notes outside the
playable range remain stored. A clip may contain at most 65536 notes after an edit.

Read `clips.get` first. `offset` is zero-based and `limit` is 1–4096. Each returned note includes `noteIndex`, persisted
`noteId` (possibly -1 for unsaved new notes), and all editable fields. **Use noteIndex, not noteId, for edits.** Indices
are valid only for the inspected revision/current staged array. Each command sorts by start/pitch; later commands in a
batch address that resulting array. Duplicate indices in one command fail. Updates preserve stored note IDs and other
fields; replacement creates new identities. Per-note expression editing is not exposed. Empty updates, audio clips,
missing targets, malformed fields and an invalid later command reject the whole batch without a partial edit.

Example `edits.apply` arguments (substitute current document handles and the normal guarded envelope):

```json
{"commands":[{"type":"notes.add","trackId":6,"clipId":1000000,
  "notes":[{"pitch":75,"startBeat":0.5,"durationBeats":1.25,"velocity":77}]}]}
```

### General hosted VST3 controls

Use `devices.parameters` to discover full `parameterId`/`name` pairs and normalized values. Send the exact pair; no
name fallback, fuzzy matching or numeric-index assumption is used. Generic writes support top-level actual VST3
instances on regular tracks (any plugin/version with working state support), distinct from the version-qualified Serum recipe.
Only controls with `writable:true` are supported: automatable, non-meter parameters. UI-only buttons, unexposed controls,
file/preset dialogs and non-automatable parameters are outside this operation. Duplicate IDs and values outside 0–1 fail.
Discrete values must match the plugin's normalized grid; no silent snapping is promised.

The service captures the **live** component/controller state under callback exclusion, then releases the callback lock.
It opens an isolated copy of that exact plugin, validates the entire batch before writing, flushes silent blocks,
captures opaque state and opens another isolated copy to verify persistence/readback (tolerance 0.0001).
Preparation failure or a revision change commits nothing. Authored automation targeting a requested parameter in the
track, Arrangement, Session or take envelopes is rejected. Other opaque plugin content is retained in captured state.

Before commit, a second capture also checks for opaque editor changes during preparation. After commit, actual live
parameter readback and state capture must succeed; failure undoes the transaction and returns `plugin_commit_failed`
(or `outcome_unknown` if rollback fails). Identical retries retain that failure receipt.

A successful commit uses existing device replacement history: it rebuilds that processor, closes plugin editors and
can interrupt its sounding voices. Other processors are retained. The receipt reports `previousDeviceId`, NEW `deviceId`,
actual normalized/display values, `processorRebuilt:true`, `stateReloadVerified:true` and `liveReadbackVerified:true`. Re-inspect/use the new ID for
later requests; undo restores the old ID and captured live state, redo restores the edited state. This is a coarse patch
edit, not realtime parameter streaming or automation recording. Plugin constructors/state methods still run in-process;
a buggy plugin can block/crash the host. A verified isolated state is not a universal plugin compatibility guarantee.

Example shared request arguments (use the normal target/revision/idempotency envelope):

```json
{
  "trackId": 6,
  "deviceId": 10,
  "parameters": [{"parameterId": "0", "name": "Main Vol", "normalized": 0.27}]
}
```

MCP exposes this as `sonora_parameters_set`. MIDI commands use the existing `sonora_edits_apply`; clip pagination and
location selectors use `sonora_clip_get`. Notes and plugin batches are separate transactions, each with its own undo token.
Whole-state undo also compares captured component/controller bytes with the actual committed plugin state, so GUI-only
changes cannot be silently overwritten. Plugin-generated volatile state can conservatively refuse an undo; inspect and
use normal application history deliberately if appropriate. External MCP clients and the configured native LLM translate
natural language to these typed operations. The legacy deterministic `submit()` parser remains a developer/probe helper;
it is not a fallback behind the native prompt. See [NATIVE_LLM.md](NATIVE_LLM.md) for its bounded context adapters.

## Inspection (AGT-7e)

`project.inspect` keeps every earlier field and adds:

* Song: `timeSignature {numerator, denominator}`, `loop {enabled, startBeat, lengthBeats}`, `punch {in, out}`,
  `scale {root, name, inKey}`, `quantization` (0-13, the launch quantisation index), `scenes[] {scene, name, tempo?}`,
  `path`, `untitled`, `dirty`, `history {canUndo, undoLabel, canRedo, redoLabel}`.
* `mainTrack {trackId, name, color, mixer, devices, automation}`. **`mainTrack.trackId` addresses the main track** in
  `mixer.set`, `automation.*`, `devices.*` and `devices.set_enabled`. A set stores no ID for it, so Sonora reports the file's
  ID when there is one and the reserved value 999999999 otherwise. Tracks, returns and main share one ID space.
* Per track: `color`, `mixer {gainLinear, authoredGainLinear, pan, muted, solo, armed, sends[{sendIndex, returnTrackId, gainLinear}]}`
  (live engine values; the model values where the host has no live mixer), `monitoring` (`in`/`auto`/`off`), `frozen`,
  `deactivated`, `takeLanes[] {laneId, name, audition, clipCount}`. `returnTracks[]` gain `mixer`, `devices` and `color`.
* Per clip (Arrangement, Session and take clips): `kind` (`midi`/`audio`), `muted`, `color`, `looping` and, for audio, `sampleFile`.
* Per device: `enabled`, `kind` (`native`, `rack`, `vst3`, `vst2`, `au`, `max`), `role` (`instrument`, `midiEffect`,
  `audioEffect`) and nested `chains[] {index, name, devices[]}` (recursive, depth 4, 64 chains, 64 devices per list;
  `chainsTruncated` marks a cut).

Per-container caps are unchanged (512 clips per container, 64 automation points per lane in the summary). `automation.get`
pages one lane completely. The native assistant's inspect summary stays bounded: without a focused `trackId` it omits nested
chains and long lists.

`status.get` returns `busy`, `dirty`, `path`, `untitled`, `history`, `tempo`, and from the host `transport`
(`playing`, `countingIn`, `positionBeats`, `tempo`, `metronome`, `loop`), `recording` (`recording`, `requested`, `mode`),
`export` (`phase` idle/running/done/failed/cancelled, `running`, `progress`, `files`, `error`: what `finishExport` records),
`mediaJob` (`state` idle/running/done/failed/finished_without_change, `action`, `message` = the app's status line, `undoLabel`, `seconds`) and
`pluginScan` (`running`). `liveStatus:false` means the host supplies no live status. A media job's `done` means the history moved
(the job's edit is one step); `finished_without_change` means it ended and the app's message (for example "Cannot reverse: ...") says why.

## Structure and song edits (AGT-7e)

All of these are `edits.apply` commands: strictly validated (unknown fields rejected), undoable as one step with the rest of
the batch, and a failing command commits nothing. Track, clip and lane IDs resolve against the state after the earlier
commands of the same batch. Before a batch that deletes, duplicates, moves or groups tracks the application copies live plug-in
and mixer state into the model (as the UI does). Unknown IDs are `target_replaced`.

* Song: `time_signature.set {numerator 1-99, denominator 1|2|4|8|16}`, `loop.set {on, startBeat, lengthBeats}`,
  `punch.set {in?, out?}`, `scale.set {root 0-11, name 0-34}` (index into the scale table: 0 Major, 1 Minor...),
  `quantization.set {value 0-13}` (0 none, 1 8 bars, 2 4 bars, 3 2 bars, 4 1 bar, 5 1/2, 6 1/2T, 7 1/4, 8 1/4T, 9 1/8, 10 1/8T,
  11 1/16, 12 1/16T, 13 1/32). Setting a value the song already has succeeds as a no-op.
* Tracks: `track.create {kind: midi|audio|return, name?, color? 0-69, afterTrackId?}` (a new MIDI/audio track goes right
  after that track and joins its group, like Insert Track; returns are appended, at most 12; the receipt's `data.tracks[]`
  lists `{trackId, kind, name}`), `track.delete {trackIds}` (a group takes its children; `data.deletedTrackIds`; main cannot be
  deleted), `track.duplicate {trackIds, withAutomation?}` (`data.tracks[]`), `track.move {trackIds, beforeTrackId?, groupId?}`
  (`groupId` -1 is top level and defaults to the parent of `beforeTrackId`), `track.group {trackIds, name?}` (the group's ID is
  in `data.tracks[0]`), `track.ungroup {trackId}`, `track.rename {trackId, name}`, `track.set_color {trackIds, color}`,
  `track.set_hidden {trackIds, hidden}`, `track.set_deactivated {trackIds, deactivated}`,
  `track.set_monitoring {trackId, mode: in|auto|off}` (audio and MIDI tracks).
* Clips: `clip.update {trackId, clipId, scene? | laneId?, name?, color?, muted?, startBeat?, endBeat?, loop? {enabled?, startBeat?, endBeat?},
  launchMode? trigger|gate|toggle|repeat, launchQuantization? 0-14, legato?, gainDb? -70..24, pitchCoarse? -48..48, pitchFine? -50..50}`.
  `startBeat`/`endBeat` trim the edges of an Arrangement clip (content stays in place; `region_occupied` if another clip is
  in the way; use `clip.move` to move). Launch settings apply to Session clips; the gain/pitch fields to audio clips
  (`unsupported_clip` on MIDI). `clip.split {trackIds, beat, endBeat?}`, `region.duplicate` and `region.delete`
  `{firstTrackId, lastTrackId, startBeat, endBeat}` (the track range follows the display order), `clip.duplicate_linked
  {trackId, clipId}` and `clip.unlink {trackId, clipId, scene? | laneId?}` (MIDI), `clip.copy_to_scene {trackId, clipId, scene?
  (source slot; omit for an Arrangement clip), toScene, toTrackId?}` into an empty Session slot of the same track kind
  (Arrangement *audio* clips are refused: their Session form is not verified; MIDI clips and Session clips of either kind work).
* Take lanes: `take_lane.rename {trackId, laneId, name}`, `take_lane.move {trackId, laneId, delta}`, `take_lane.delete`,
  `take_lane.duplicate`, `take_lane.audition {trackId, laneId?}` (omit `laneId` to return to the main lane) and
  `take_lane.comp {trackId, laneId, startBeat, endBeat}`.
* Automation on the main track and its tempo lane (`target: "tempo"`, 20-999 BPM) works like any other lane.

## Live actions (AGT-7e)

Live actions write the running session the way the UI does and never go through history. The mixer values are the engine
controls (the set takes them on save, and the application marks the project edited by itself); `solo` is additive (other
tracks are untouched); `armed` uses the UI's arm path and fails with `recording_conflict` while that track records.
`devices.set_enabled` is the device power switch. `transport.set`, `session.launch` and `recording.set` start work that
completes later (launch quantisation, input permission): read `status.get`. `project.save` writes in place with the backup
copy the UI makes; it never opens a dialog, so an untitled project or a collaboration guest gets `untitled_document`.
`plugins.scan` runs the application's own scan (which loads third-party plug-ins in the app process).

`clips.import_audio` is a mutation, not a live action: it is one undo step, runs on the message thread (decoding and tempo
analysis of up to 16 files) and only reads the given files.

## Not exposed

Absent on purpose, with the reason; none is faked:

* `media.run` stem separation (needs an external tool and opens alerts) and Resynth (opens "clip too long / too many notes" offer prompts): left out on purpose.
* `view.set` has no automation-lane toggle (the menu command toggles blind); `devices.open_editor` opens hosted plug-ins only.
* Arrangement *audio* clips into a Session slot (`clip.copy_to_scene`): the UI has no such path to reuse and the Session form of loop/warp markers is not verified.
* Scene insert, delete and rename: the model has no such commands (scenes grow only when a clip is placed in a new slot).
* Nested hosted plug-in (VST3/VST2/AU) parameter writes inside a rack, and VST2/AU parameter writes at any level.
* Sample-zone, wavetable and other content authoring; routing; Max for Live patches; preset browsing.
* Save As, Open/New project, quitting, audio/MIDI device settings: they need dialogs or change what the user sees and hears globally.

## Nested rack devices (AGT-7e)

`devices.parameters` accepts the `deviceId` of a device nested at any depth in a rack. `devices.parameters.set` accepts a nested
*native* device too: the child is edited on the isolated rack processor, the whole top-level rack is captured, verified through a
state reload and committed (one undo step; undo restores the old rack with its ID). The receipt has `previousDeviceId` and
`deviceId` (the rack's old and NEW IDs), `nestedDeviceId` (the child, whose ID does not change) and the written parameters. Any
authored automation on a descendant of the rack is `automation_conflict`, as for a rack macro write. Nested VST3/VST2/AU
children are refused (`unsupported_device`).

## Testing the app hooks (Debug builds)

`SONORA_CONTROL_AUTOTEST=1` (compiled into Debug builds only, `#if JUCE_DEBUG`) grants the local control grant, what "Allow MCP
access to this project" does, once the set passed on the command line has installed. `packages/mcp-server/app-smoke.mjs` uses it:
it writes a disposable set (`control_probe --write-set`) and a WAV into a temp folder, starts the Debug app with
`open -n .../Sonora.app --env SONORA_DATA_FOLDER=SonoraMcpTest --env SONORA_CONTROL_AUTOTEST=1 --stderr <log> --args <set.als>`
(LaunchServices, never a direct exec; the data folder keeps it apart from a running Sonora), drives it through the real `server.mjs`
(the bridge reads `SONORA_MCP_REGISTRY_DIR` instead of the `Sonora`/`Sonora Nightly` registries) and checks every effect with a
follow-up read, `als_dump` of the saved files and the exported WAV's peak. It quits only the instance it started and deletes the
data folder and temp files. It does not run `recording.set` with `recording:true`, arming (permission dialogs) or `plugins.scan`.

## Results and recovery

Results contain `ok`, `status`, `instanceId`, `documentId`, `revision` and `data`; failures also contain `error.code`
and `error.message`. Commits return `data.undoToken`, `saved: false`, and `audioVerified: false`. The Serum receipt includes
actual normalized/display parameter values, plugin version/profile, assumptions, and created track/clip IDs.

`committed` means history/model and app preparation completed. It does not claim an audio block was heard, a file was
saved, or a subjective timbre was accepted. The standalone end-to-end probe separately reloads and renders the real
plugin to verify audio. Native playback/save remain normal user actions.

Common failures: `capacity`, `region_occupied`, `incompatible_track`, `instrument_conflict`, `untitled_document`, `save_failed`, `track_frozen`, `stale_instance`, `stale_document`, `stale_revision`, `busy`, `recording_conflict`, `automation_conflict`,
`idempotency_conflict`, `undo_conflict`, `missing_plugin`, `unsupported_plugin_version`, `plugin_profile_mismatch`,
`plugin_write_failed`, `plugin_commit_failed`, `parameter_mismatch`, `parameter_read_only`, `unsupported_device`, `unsupported_clip`, `target_replaced`, `invalid_request`, `unsupported_operation` and `unsupported_prompt`.

For `outcome_unknown` or a broken socket, reconnect to the same instance/document and reconcile the identical operation
key or inspect state. Do not invent a new key and repeat a potentially committed mutation. Undo after intervening human
edits is deliberately refused; this version does not generate targeted inverse edits. Undo does not delete external files.

## Local transport and authorization

Enable local automation in Sonora before connecting. Registry metadata is under
`~/Library/Application Support/Sonora/Automation/instances/` (`Sonora Nightly/...` for nightly builds; the MCP server
reads both, see UPDATES.md). Socket and credential files live in a private
`/tmp/sonora-control-<uid>/` directory; files are owner-only. A message is one bounded UTF-8 JSON line:
`{"token":"private credential","request":{...}}`. There is one response per connection. Never print credentials.
The endpoint verifies the OS peer user and credential, bounds message size/depth, and marshals execution to the message thread.

This is a same-user local grant, not a sandbox against malicious software already running as that user. There is no shell,
arbitrary code or network upload. File access is limited to: reading the audio files named in `clips.import_audio` (existing,
absolute paths that the app's audio-file filter accepts), and writing the project's own file through `project.save` (with the
app's backup copy). No operation takes a destination path for writing. `plugins.scan` loads plug-in binaries that are already
installed. The grant is disabled on project switch/quit.
