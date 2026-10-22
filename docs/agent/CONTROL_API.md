# Sonora control API version 1.0

This is the implemented control surface (including MIDI edits, native/rack/VST3 parameters and device insertion/removal). All calls execute on the message thread; JSON decoding and socket I/O use a
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

| Operation | Arguments | Behavior |
| --- | --- | --- |
| `project.inspect` | Empty | App/document IDs, revision, busy state, tempo, regular tracks, Arrangement, Session and take clip handles, and top-level devices. |
| `capabilities.get` | Empty | Actual operations, reasoning-client mode and limitations. |
| `clips.get` | `trackId`, `clipId`, optional `scene` OR `laneId`, `offset`, `limit` | Paged notes with revision-scoped indices, total count, `hasMore` and loop bounds. Default limit 4096. |
| `devices.parameters` | `trackId`, `deviceId` | Top-level native/rack/VST3 parameters with exact IDs/names, values, unit/range/choice metadata and `writable`. |
| `devices.parameters.set` | `trackId`, `deviceId`, `parameters` (1–128 exact ID/name pairs with **either** `normalized` **or** native `value`) | Capture live before-state; prepare and reload-verify isolated state; commit one undo step. Returns NEW `deviceId`. VST3 accepts normalized only. |
| `devices.catalog` | Optional `filter`, `kind: native/vst3`, `offset`, `limit` | Insertable implemented native devices and scanned VST3 identities, role and version. Default 64, maximum 128 entries; returns total/hasMore. |
| `devices.insert` | `trackId`, exact `catalogId`, optional zero-based `position` | Insert a device on an existing regular track, one undo step; returns `deviceId`. No silent instrument replacement. |
| `devices.remove` | `trackId`, `deviceId` | Remove a top-level device, retaining its captured live state for undo. No uninstall or source-media deletion. |
| `edits.apply` | `commands` array, 1–32 entries | Validate against a staged model, then commit one undoable batch. |
| `workflow.serum_supersaw` | `root: "D#"`, `mode: "minor"`, `bars: 4`, `startBeat` | Prepare the qualified real Serum patch and insert a new MIDI track with 12 notes over 16 beats. |
| `history.undo` | `undoToken` from a commit | Undo only if the agent edit is still the latest unchanged state. |

Inspection/capability discovery does not require an envelope target. Other queries do. A local socket is already bound
to the enabled document, and changing documents revokes that endpoint. The MCP adapter additionally checks returned identities.

Supported edit types are `tempo.set {bpm: 20..999}`, `track.set_gain {trackId, gainDb: -90..6}` and
`clip.duplicate {trackId, clipId, destinationBeat: 0..1000000}`. Gain is absolute dB, not a relative change. An agent asked
to lower a track by 3 dB must read its current gain and calculate the absolute target. Gain updates participate in
history, live engine controls and save state; tracks with authored volume automation are rejected for this operation.

Musical times are quarter-note beats, with half-open clip regions. Four 4/4 bars are 16 beats. Source audio seconds and
sample offsets are separate concepts; no source-audio editing is exposed yet. The Serum recipe keeps project tempo and
requires 4/4. It assumes D# natural minor when a built-in request says only D# scale; the receipt states that assumption.

Implementation (COL-1, ADR 0009): every `edits.apply` command decodes to a `model::Op` (`src/core/Op.h`, wire form
`core/OpJson.h`), the same serializable intent collab peers will exchange, and `opToCommand` builds the undoable command.
The public note API still uses `noteIndex`; the service translates indices to per-clip note keys against the staged
song. A `trackId` that no longer exists reports `target_replaced` for every edit type (previously some returned
`invalid_request`). The op vocabulary also contains `track.rename`, `track.set_color`, `loop.set`, `clip.move` and
`clip.delete`; they are not exposed through this API until schemas/MCP tools are added (AGT-7).

All edits are rejected while recording/draining or during export/device state work. Track CRUD beyond the recipe,
save/export tools, dry-run prepare/commit, durable jobs, nested plugin targets and remote access remain future work.
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
installed/scanned; no per-plugin code integration is required. There is no agent install/scan operation. Native catalogue
entries require an implemented factory; unimplemented placeholders and AU/VST2 insertion are not advertised.

Instruments/MIDI effects require a MIDI track. Defaults put MIDI effects first, the instrument next, audio effects last.
Explicit positions must retain that order. Adding a second instrument fails with `instrument_conflict`; remove the old
instrument deliberately first, or choose an empty MIDI track. No track creation is implied. Agent insertion is limited
to 64 top-level devices per track. Native defaults and verified initialized VST3 state are authored as ordinary devices;
the app then checks that the actual processor exists. Inserts, parameter batches and removals are separate transactions.

Removal captures current live state before deleting the device. Undo restores its ID, position and state even after
save; redo removes it again. Source XML/unknown content and source media ownership remain with the existing model/writer.
Plugin loading/state work is still in-process and may interrupt voices or block on a faulty third-party plugin.

MCP 0.3 exposes `sonora_devices_catalog`, `sonora_devices_insert`, `sonora_devices_remove` and the extended
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

## Results and recovery

Results contain `ok`, `status`, `instanceId`, `documentId`, `revision` and `data`; failures also contain `error.code`
and `error.message`. Commits return `data.undoToken`, `saved: false`, and `audioVerified: false`. The Serum receipt includes
actual normalized/display parameter values, plugin version/profile, assumptions, and created track/clip IDs.

`committed` means history/model and app preparation completed. It does not claim an audio block was heard, a file was
saved, or a subjective timbre was accepted. The standalone end-to-end probe separately reloads and renders the real
plugin to verify audio. Native playback/save remain normal user actions.

Common failures: `stale_instance`, `stale_document`, `stale_revision`, `busy`, `recording_conflict`, `automation_conflict`,
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
arbitrary code, network upload or general filesystem-write operation. The grant is disabled on project switch/quit.
