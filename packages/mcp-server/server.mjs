// MCP-1: stdio adapter over Sonora's opt-in, document-scoped local control service.
import { McpServer } from '@modelcontextprotocol/server';
import { serveStdio } from '@modelcontextprotocol/server/stdio';
import * as z from 'zod/v4';
import { promises as fs } from 'node:fs';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';

// One registry per app data folder (COM-4c): the stable app and "Sonora Nightly" never share data.
// SONORA_MCP_REGISTRY_DIR points the bridge at one registry folder instead (an app started with SONORA_DATA_FOLDER=X keeps it in
// ~/Library/Application Support/X/Automation/instances; app-smoke.mjs uses this to stay away from a Sonora that is already running).
const registryDirectories = process.env.SONORA_MCP_REGISTRY_DIR ? [process.env.SONORA_MCP_REGISTRY_DIR]
  : ['Sonora', 'Sonora Nightly'].map(folder => path.join(os.homedir(), 'Library/Application Support', folder, 'Automation/instances'));
let attached;

async function readPrivate(file, maxBytes = 65536) {
  const stat = await fs.lstat(file);
  if (!stat.isFile() || stat.isSymbolicLink() || stat.uid !== process.getuid() || (stat.mode & 0o077) || stat.size > maxBytes)
    throw new Error('Control file must be an owner-only regular file');
  return fs.readFile(file, 'utf8');
}
async function registryFiles() {
  const files = [];
  for (const directory of registryDirectories) {
    const names = await fs.readdir(directory).catch(error => {
      if (error.code === 'ENOENT') return [];
      throw error;
    });
    files.push(...names.filter(name => /^[a-f0-9-]+\.json$/i.test(name)).map(name => path.join(directory, name)));
  }
  return files;
}
async function instances() {
  const entries = [];
  for (const file of (await registryFiles()).slice(0, 64)) {
    try {
      const entry = JSON.parse(await readPrivate(file));
      if (typeof entry.instanceId !== 'string' || typeof entry.documentId !== 'string' || !Number.isInteger(entry.pid)) continue;
      process.kill(entry.pid, 0);
      entries.push(entry);
    } catch { /* Stale/invalid entries are not live targets. */ }
  }
  return entries;
}
async function exchange(connection, request) {
  return new Promise((resolve, reject) => {
    const socket = net.createConnection(connection.socket);
    socket.setEncoding('utf8');
    let response = '', finished = false;
    const fail = error => { if (!finished) { finished = true; socket.destroy(); reject(error); } };
    socket.setTimeout(130000, () => fail(new Error('outcome_unknown: reconcile before retrying this operation')));
    socket.on('error', fail);
    socket.on('connect', () => socket.write(JSON.stringify({ token: connection.token, request }) + '\n'));
    socket.on('data', bytes => {
      response += bytes.toString('utf8');
      if (response.length > 4 * 1024 * 1024) return fail(new Error('Control response exceeds limit'));
      const end = response.indexOf('\n');
      if (end >= 0 && !finished) {
        try { const result = JSON.parse(response.slice(0, end)); finished = true; socket.end(); resolve(result); }
        catch (error) { fail(error); }
      }
    });
    socket.on('end', () => { if (!finished) fail(new Error('instance_gone: control connection ended without a receipt')); });
  });
}
async function call(operation, args = {}, mutation) {
  if (!attached) throw new Error('Attach to an explicit instance first');
  const result = await exchange(attached, {
    apiVersion: '1.0', operation, arguments: args,
    target: { instanceId: attached.instanceId, documentId: attached.documentId },
    ...(mutation ? { expectedRevision: mutation.expectedRevision, idempotencyKey: mutation.idempotencyKey } : {})
  });
  if (result.ok && (result.instanceId !== attached.instanceId || result.documentId !== attached.documentId)) {
    attached = undefined;
    throw new Error('stale_document: attachment changed; explicitly reattach');
  }
  return result;
}
const mutation = {
  expectedRevision: z.string().min(1).describe('Exact revision from a fresh inspect; never guess'),
  idempotencyKey: z.string().min(1).max(128).describe('Unique operation key; reuse identical arguments when reconciling a retry')
};
const id = z.number().int().min(0).max(1000000000);
const location = { trackId: id, clipId: id, scene: z.number().int().min(0).max(1000000).optional(), laneId: id.optional() };
const noteFields = {
  pitch: z.number().int().min(0).max(127), startBeat: z.number().min(0).max(1000000),
  durationBeats: z.number().min(0.000001).max(1000000), velocity: z.number().min(0).max(127),
  releaseVelocity: z.number().min(0).max(127).optional(), probability: z.number().min(0).max(1).optional(),
  enabled: z.boolean().optional(), velocityDeviation: z.number().min(-127).max(127).optional()
};
const note = z.object(noteFields).strict();
const update = note.partial().extend({ noteIndex: id }).strict().refine(value => Object.keys(value).length > 1, 'Specify a changed note field');
const parameterIdentity = { parameterId: z.string().min(1).max(256), name: z.string().min(1).max(256) };
const parameterWrite = z.union([
  z.object({ ...parameterIdentity, normalized: z.number().min(0).max(1) }).strict(),
  z.object({ ...parameterIdentity, value: z.number().min(-1e12).max(1e12) }).strict()
]);
// UI-3b: Arrangement locators (markers), addressed by locatorId from project.inspect.
const beat = z.number().min(0).max(1000000);
const locatorName = z.string().max(256);
const trackColor = z.number().int().min(0).max(69).describe('Palette index 0-69');
const trackIds = z.array(id).min(1).max(256).describe('Track IDs from sonora_project_inspect');
const locatorColor = z.number().int().min(-1).max(69).describe('Palette index 0–69; -1 = default locator colour');
// AGT-7d: automation targets by name; points are song beats with a value in the target's units or gainDb (volume/send).
const automationTarget = {
  trackId: id,
  target: z.enum(['volume', 'pan', 'activator', 'tempo', 'send', 'device']).optional()
    .describe('volume/pan/activator (track on), tempo (main track only), send (with sendIndex = returnTracks[] order) or device (with deviceId and parameterId)'),
  sendIndex: z.number().int().min(0).max(1000).optional(),
  deviceId: id.optional(),
  parameterId: z.string().min(1).max(1024).optional().describe('parameterId from sonora_parameters_get, or "on" for the device switch'),
  targetId: id.optional().describe('Alternative to target: an automation targetId from inspect automation[]')
};
const automationPoint = z.object({
  beat,
  value: z.number().optional().describe('Volume/send: linear gain (1 = 0 dB, max 1.995 = +6 dB for volume). Pan -1..1. Activator/on: 0 or 1. Native parameter: its descriptor minimum..maximum. VST3: normalized 0..1'),
  gainDb: z.number().min(-1000).max(6).optional().describe('Volume/send only; -70 dB or lower is silence')
}).strict().refine(value => (value.value === undefined) !== (value.gainDb === undefined), 'Give value OR gainDb');
const command = z.discriminatedUnion('type', [
  z.object({ type: z.literal('tempo.set'), bpm: z.number().min(20).max(999) }).strict(),
  z.object({ type: z.literal('track.set_gain'), trackId: id, gainDb: z.number().min(-90).max(6) }).strict(),
  z.object({ type: z.literal('clip.duplicate'), trackId: id, clipId: id, destinationBeat: z.number().min(0).max(1000000) }).strict(),
  z.object({ type: z.literal('notes.add'), ...location, notes: z.array(note).min(1).max(4096) }).strict(),
  z.object({ type: z.literal('notes.replace'), ...location, notes: z.array(note).max(4096) }).strict(),
  z.object({ type: z.literal('notes.update'), ...location, updates: z.array(update).min(1).max(4096) }).strict(),
  z.object({ type: z.literal('notes.delete'), ...location, noteIndices: z.array(id).min(1).max(4096) }).strict(),
  z.object({ type: z.literal('locator.add'), beat, name: locatorName.optional(), color: locatorColor.optional() }).strict(),
  z.object({ type: z.literal('locator.update'), locatorId: id, beat: beat.optional(), name: locatorName.optional(),
    infoText: z.string().max(4096).optional(), color: locatorColor.optional(), songStart: z.boolean().optional() }).strict()
    .refine(value => Object.keys(value).length > 2, 'Specify a changed locator field'),
  z.object({ type: z.literal('locator.delete'), locatorId: id }).strict(),
  z.object({ type: z.literal('track.set_folded'), trackIds: z.array(id).max(1024).optional().describe('Group track IDs; omit for every group'),
    folded: z.boolean() }).strict(),
  z.object({ type: z.literal('clip.create'), trackId: id, startBeat: beat.optional(), scene: z.number().int().min(0).max(999).optional(),
    lengthBeats: z.number().min(1 / 64).max(65536), name: z.string().max(256).optional(), notes: z.array(note).max(4096).optional() }).strict()
    .refine(value => (value.startBeat === undefined) !== (value.scene === undefined), 'Give startBeat (Arrangement) OR scene (Session)'),
  z.object({ type: z.literal('clip.copy'), trackId: id, clipId: id, toTrackId: id.optional(), destinationBeat: beat }).strict(),
  z.object({ type: z.literal('clip.move'), trackId: id, clipId: id, toTrackId: id.optional(), startBeat: beat }).strict(),
  z.object({ type: z.literal('clip.delete'), ...location }).strict(),
  z.object({ type: z.literal('automation.set'), ...automationTarget, points: z.array(automationPoint).max(4096),
    rangeStartBeat: beat.optional(), rangeEndBeat: beat.optional() }).strict(),
  z.object({ type: z.literal('automation.clear'), ...automationTarget }).strict(),
  // AGT-7e: song, tracks, clips, regions and take lanes.
  z.object({ type: z.literal('time_signature.set'), numerator: z.number().int().min(1).max(99), denominator: z.union([z.literal(1), z.literal(2), z.literal(4), z.literal(8), z.literal(16)]) }).strict(),
  z.object({ type: z.literal('loop.set'), on: z.boolean(), startBeat: beat, lengthBeats: z.number().min(0).max(1000000) }).strict(),
  z.object({ type: z.literal('punch.set'), in: z.boolean().optional(), out: z.boolean().optional() }).strict()
    .refine(value => value.in !== undefined || value.out !== undefined, 'Give in and/or out'),
  z.object({ type: z.literal('scale.set'), root: z.number().int().min(0).max(11).describe('Pitch class 0=C..11=B'), name: z.number().int().min(0).max(34).describe('Scale index: 0 Major, 1 Minor, ...') }).strict(),
  z.object({ type: z.literal('quantization.set'), value: z.number().int().min(0).max(13).describe('0 none, 1 8 bars, 2 4 bars, 3 2 bars, 4 1 bar, 5 1/2, 6 1/2T, 7 1/4, 8 1/4T, 9 1/8, 10 1/8T, 11 1/16, 12 1/16T, 13 1/32') }).strict(),
  z.object({ type: z.literal('track.rename'), trackId: id, name: z.string().max(256) }).strict(),
  z.object({ type: z.literal('track.set_color'), trackIds: z.array(id).max(1024), color: trackColor }).strict(),
  z.object({ type: z.literal('track.create'), kind: z.enum(['midi', 'audio', 'return']), name: z.string().min(1).max(256).optional(), color: trackColor.optional(),
    afterTrackId: id.optional().describe('midi/audio: the new track goes right after this one and joins its group; omit to append') }).strict(),
  z.object({ type: z.literal('track.delete'), trackIds: trackIds }).strict(),
  z.object({ type: z.literal('track.duplicate'), trackIds, withAutomation: z.boolean().optional() }).strict(),
  z.object({ type: z.literal('track.move'), trackIds, beforeTrackId: id.optional().describe('Insert before this track; omit for the end'),
    groupId: z.number().int().min(-1).max(1000000000).optional().describe('Parent group ID, -1 top level; defaults to the parent of beforeTrackId') }).strict(),
  z.object({ type: z.literal('track.group'), trackIds, name: z.string().min(1).max(256).optional() }).strict(),
  z.object({ type: z.literal('track.ungroup'), trackId: id }).strict(),
  z.object({ type: z.literal('track.set_hidden'), trackIds, hidden: z.boolean() }).strict(),
  z.object({ type: z.literal('track.set_deactivated'), trackIds, deactivated: z.boolean() }).strict(),
  z.object({ type: z.literal('track.set_monitoring'), trackId: id, mode: z.enum(['in', 'auto', 'off']) }).strict(),
  z.object({ type: z.literal('clip.update'), ...location, name: z.string().max(256).optional(), color: trackColor.optional(), muted: z.boolean().optional(),
    startBeat: beat.optional(), endBeat: beat.optional(),
    loop: z.object({ enabled: z.boolean().optional(), startBeat: z.number().min(-1000000).max(1000000).optional(), endBeat: z.number().min(-1000000).max(1000000).optional() }).strict()
      .refine(value => Object.keys(value).length > 0, 'loop needs a field').optional(),
    launchMode: z.enum(['trigger', 'gate', 'toggle', 'repeat']).optional(), launchQuantization: z.number().int().min(0).max(14).optional(), legato: z.boolean().optional(),
    gainDb: z.number().min(-70).max(24).optional(), pitchCoarse: z.number().int().min(-48).max(48).optional(), pitchFine: z.number().min(-50).max(50).optional() }).strict()
    .refine(value => Object.keys(value).length > 3, 'Specify at least one clip property'),
  z.object({ type: z.literal('clip.split'), trackIds, beat, endBeat: beat.optional() }).strict(),
  z.object({ type: z.literal('region.duplicate'), firstTrackId: id, lastTrackId: id, startBeat: beat, endBeat: beat }).strict(),
  z.object({ type: z.literal('region.delete'), firstTrackId: id, lastTrackId: id, startBeat: beat, endBeat: beat }).strict(),
  z.object({ type: z.literal('clip.duplicate_linked'), trackId: id, clipId: id }).strict(),
  z.object({ type: z.literal('clip.unlink'), ...location }).strict(),
  z.object({ type: z.literal('clip.copy_to_scene'), trackId: id, clipId: id, scene: z.number().int().min(0).max(1000000).optional().describe('Source Session slot; omit for an Arrangement clip'),
    toScene: z.number().int().min(0).max(999), toTrackId: id.optional() }).strict(),
  z.object({ type: z.literal('take_lane.rename'), trackId: id, laneId: id, name: z.string().max(256) }).strict(),
  z.object({ type: z.literal('take_lane.move'), trackId: id, laneId: id, delta: z.number().int().min(-64).max(64) }).strict(),
  z.object({ type: z.literal('take_lane.delete'), trackId: id, laneId: id }).strict(),
  z.object({ type: z.literal('take_lane.duplicate'), trackId: id, laneId: id }).strict(),
  z.object({ type: z.literal('take_lane.audition'), trackId: id, laneId: id.optional().describe('Omit to return to the main lane') }).strict(),
  z.object({ type: z.literal('take_lane.comp'), trackId: id, laneId: id, startBeat: beat, endBeat: beat }).strict()
]);
function createServer() {
  const server = new McpServer({ name: 'sonora', version: '0.6.0' });
  server.registerResource('control-guide', 'sonora://docs/control-v1', { mimeType: 'text/markdown' }, async uri => ({
    contents: [{ uri: uri.href, mimeType: 'text/markdown', text: await fs.readFile(new URL('../../docs/agent/CONTROL_API.md', import.meta.url), 'utf8') }]
  }));
  // destructive: an undoable edit or an overwrite (save). Live state changes (mixer, transport...) pass false.
  function tool(name, description, shape, handler, readOnly = false, destructive = !readOnly) {
    server.registerTool(name, {
      description, inputSchema: z.object(shape).strict(), outputSchema: z.object({ ok: z.boolean() }).passthrough(),
      annotations: { readOnlyHint: readOnly, destructiveHint: readOnly ? false : destructive, openWorldHint: false }
    }, async args => {
      try {
        const result = await handler(args);
        return { content: [{ type: 'text', text: JSON.stringify(result) }], structuredContent: result, isError: !result.ok };
      } catch (error) {
        const result = { ok: false, error: { code: 'connection_error', message: error.message } };
        return { content: [{ type: 'text', text: JSON.stringify(result) }], structuredContent: result, isError: true };
      }
    });
  }
  tool('sonora_instances_list', 'List running Sonora instances with local automation enabled. Select an explicit instance ID.', {}, async () => ({
    ok: true, instances: (await instances()).map(({ instanceId, documentId, pid }) => ({ instanceId, documentId, pid }))
  }), true);
  tool('sonora_session_attach', 'Attach to the exact app launch and document chosen by the user. Never silently follow another document.', {
    instanceId: z.string().min(1), documentId: z.string().min(1)
  }, async args => {
    attached = undefined;
    const entry = (await instances()).find(item => item.instanceId === args.instanceId && item.documentId === args.documentId);
    if (!entry) throw new Error('instance_gone: enable local MCP control in the intended project');
    const token = (await readPrivate(entry.credentialFile, 256)).trim();
    const candidate = { ...entry, token };
    const result = await exchange(candidate, { apiVersion: '1.0', operation: 'project.inspect' });
    if (!result.ok || result.instanceId !== args.instanceId || result.documentId !== args.documentId) throw new Error('Target changed during attach');
    attached = candidate;
    return result;
  });
  tool('sonora_project_inspect', 'Read revision, tempo, time signature, loop/punch/scale/quantization, scenes, history labels (undo/redo), dirty flag and file path; per track: kind, color, mixer (pan, muted, solo, armed, sends with returns), monitoring, frozen, deactivated, takeLanes; tracks (kind audio/midi/group; groupId = parent group; groups list childTrackIds and folded, and hold no clips themselves: their content is on the child tracks), clips with kind/muted/color/sampleFile, devices (enabled, kind native/rack/vst3/vst2/au/max, role, nested chains), returnTracks (send order, mixer, devices) and mainTrack (its trackId addresses the main track in every tool). Read (kind audio/midi/group; groupId = parent group; groups list childTrackIds and folded, and hold no clips themselves: their content is on the child tracks), Arrangement/Session/take clip handles, top-level devices, automation lanes per track, returnTracks (send order) and Arrangement locators before editing.', {}, () => call('project.inspect'), true);
  tool('sonora_capabilities_get', 'Read implemented operations and explicit limitations.', {}, () => call('capabilities.get'), true);
  tool('sonora_clip_get', 'Read MIDI notes with revision-scoped noteIndex handles and paging. Default Arrangement; scene selects Session, laneId selects take (never both). Times are clip-content quarter-note beats.', {
    ...location, offset: id.optional(), limit: z.number().int().min(1).max(4096).optional()
  }, args => call('clips.get', args), true);
  tool('sonora_parameters_get', 'Inspect native/rack/VST3 parameter IDs, full names and writable metadata. Native parameters include value/unit/minimum/maximum/choices and linear normalization.', {
    trackId: z.number().int().nonnegative(), deviceId: z.number().int().nonnegative()
  }, args => call('devices.parameters', args), true);
  tool('sonora_parameters_set', 'Set 1–128 exact native/rack/VST3 parameter ID/name pairs. deviceId may also be a native device nested in a rack (any depth): the whole top-level rack is rebuilt and committed (receipt: nestedDeviceId, NEW deviceId of the rack); nested hosted plug-ins are refused. Supply exactly one of normalized (0–1) or native value (advertised units); VST3 accepts normalized only. Requires writable descriptors; rejects authored automation. Prepares and reload-verifies opaque state before one undoable commit. Rebuilds the affected processor (may interrupt voices/close editor); use the returned NEW deviceId. Does not save the project.', {
    ...mutation, trackId: id, deviceId: id, parameters: z.array(parameterWrite).min(1).max(128)
  }, ({ expectedRevision, idempotencyKey, ...args }) => call('devices.parameters.set', args, { expectedRevision, idempotencyKey }));
  tool('sonora_edits_apply', 'Apply a staged batch (1-32 commands) as ONE undo step; a failing command commits nothing. Track/clip/take-lane commands address by trackId/clipId/laneId, resolved against the state after the earlier commands of the batch. SONG: tempo.set, time_signature.set {numerator 1-99, denominator 1|2|4|8|16}, loop.set {on, startBeat, lengthBeats}, punch.set {in?, out?}, scale.set {root 0-11, name index}, quantization.set {value 0-13}. TRACKS: track.create {kind midi|audio|return, name?, color?, afterTrackId?} (returns the new IDs in data.tracks; a new track after a track joins its group; max 12 returns), track.delete {trackIds} (groups take their children; undo restores devices), track.duplicate {trackIds, withAutomation?}, track.move {trackIds, beforeTrackId?, groupId?}, track.group {trackIds, name?} (group ID in data.tracks), track.ungroup {trackId}, track.rename, track.set_color, track.set_hidden, track.set_deactivated, track.set_monitoring {mode in|auto|off}. CLIPS: clip.update {trackId, clipId, scene? or laneId?, name, color, muted, startBeat/endBeat (trim Arrangement edges; use clip.move to move), loop {enabled,startBeat,endBeat}, launchMode/launchQuantization/legato (Session clips), gainDb/pitchCoarse/pitchFine (audio clips only)}, clip.split {trackIds, beat, endBeat?}, region.duplicate / region.delete {firstTrackId, lastTrackId, startBeat, endBeat}, clip.duplicate_linked (MIDI), clip.unlink, clip.copy_to_scene {trackId, clipId, scene? (source slot), toScene, toTrackId?} into an EMPTY slot of the same track kind (Arrangement audio clips are not supported, MIDI and Session clips are). TAKE LANES: take_lane.rename/move {delta}/delete/duplicate/audition (omit laneId for main)/comp {startBeat,endBeat}. Also: tempo, gain, group folding, clip create/copy/move/duplicate/delete, automation, notes.add/update/delete/replace or Arrangement locators (locator.add/update/delete). Inspect notes first; noteIndex refers to current sorted notes and changes after each command. scene/laneId select Session/take. Replace is whole-content replacement (empty clears); add/update preserve other notes. Automated gain is rejected: write a volume envelope with automation.set instead. track.set_folded opens/closes groups (omit trackIds for all). clip.create makes a new MIDI clip (optional notes, clip-relative beats) on a MIDI track at startBeat or in an empty Session scene; clip.copy puts an existing Arrangement clip on another track of the same kind; both need a free region and return data.clips. automation.set writes a track envelope: target volume/pan/activator/send (+sendIndex)/device (+deviceId, parameterId from sonora_parameters_get, or "on") on tracks and return tracks, plus tempo (target tempo, 20-999 BPM) and the mixer targets on the main track (trackId = mainTrack.trackId); points use song beats; without a range the lane becomes exactly the points, with rangeStartBeat/rangeEndBeat only that span is replaced and the previous value is pinned at both edges. Example kick filter-out over bars 65–72 in 4/4: target device, parameterId of the filter frequency, rangeStartBeat 256, rangeEndBeat 288, points [{beat:256,value:<open>},{beat:284,value:<closed>}]. automation.clear removes a lane. Locators use quarter-note beats and locatorId from inspect (locators[]); locator.add returns data.locatorIds; one locator per beat; songStart is exclusive; color is a palette index (-1 default).', {
    ...mutation, commands: z.array(command).min(1).max(32)
  }, ({ commands, ...guard }) => call('edits.apply', { commands }, guard));
  tool('sonora_devices_catalog', 'List insertable implemented native devices and scanned VST3, VST2 (vst2:) and Audio Unit (au:) plug-ins. Select exact catalogId, no file paths. No installation; sonora_plugins_scan refreshes the scan. VST2/AU state is captured as the browser drop does; only VST3 and native devices accept sonora_parameters_set.', {
    filter: z.string().min(1).max(256).optional(), kind: z.enum(['native', 'vst3', 'vst2', 'au']).optional(), offset: id.optional(), limit: z.number().int().min(1).max(128).optional()
  }, args => call('devices.catalog', args), true);
  tool('sonora_devices_insert', 'Insert a catalogId on a track, return track or the main track (audio effects only on returns/main; trackId from inspect). Optional zero-based position; defaults to MIDI effects, instrument, audio effects order. MIDI devices require MIDI tracks. An existing instrument is refused (instrument_conflict) unless replaceInstrument is true, which swaps it in one undo step. Returns a new device ID and one undo token.', {
    ...mutation, trackId: id, catalogId: z.string().min(1).max(1024), position: z.number().int().min(0).max(64).optional(), replaceInstrument: z.boolean().optional()
  }, ({ expectedRevision, idempotencyKey, ...args }) => call('devices.insert', args, { expectedRevision, idempotencyKey }));
  tool('sonora_devices_remove', 'Remove an exact top-level device (also on returns and the main track) after capturing live state for undo. Does not uninstall plugins or delete source media; can interrupt voices.', {
    ...mutation, trackId: id, deviceId: id
  }, ({ expectedRevision, idempotencyKey, ...args }) => call('devices.remove', args, { expectedRevision, idempotencyKey }));
  tool('sonora_serum_supersaw', 'Create a NEW MIDI track with the real Serum 2 VST3 and a four-bar D# natural minor supersaw progression: D#m, B, F#, C#. Requires 4/4 and scanned Serum 2.0.24. Keeps project tempo; no save/export; returns actual settings and guarded undo token. Declare the minor assumption if the user omitted mode.', {
    ...mutation, root: z.literal('D#'), mode: z.literal('minor'), bars: z.literal(4), startBeat: z.number().min(0).max(1000000).default(0)
  }, ({ expectedRevision, idempotencyKey, ...args }) => call('workflow.serum_supersaw', args, { expectedRevision, idempotencyKey }));
  tool('sonora_status_get', 'Read live status: transport (playing, positionBeats, tempo, metronome, loop), recording state, export and plug-in scan state, busy text, dirty flag, file path and the undo/redo labels that sonora_history_step needs. Poll this after asynchronous actions (transport, recording, scan, import).', {},
    () => call('status.get'), true);
  tool('sonora_automation_get', 'Read ALL breakpoints of one automation lane, paged (offset, limit 1-1024, default 512). Address it like automation.set: target volume/pan/activator/tempo (main track)/send+sendIndex/device+deviceId+parameterId, or targetId from inspect. Works on tracks, return tracks and the main track. Beats are song quarter notes; initialValue is the value before the first breakpoint. exists:false when the target has no lane.', {
    trackId: id, ...automationTarget, offset: id.optional(), limit: z.number().int().min(1).max(1024).optional()
  }, args => call('automation.get', args), true);
  tool('sonora_devices_move', 'Move a top-level device (one undo step). position (0-based, final index) reorders an audio effect on its track; toTrackId moves it to another track, placed like a browser drop (MIDI effects first, an instrument replaces the destination instrument, effects last) and returns the NEW deviceId. Works on tracks, returns and the main track (audio effects only).', {
    ...mutation, trackId: id, deviceId: id, position: z.number().int().min(0).max(63).optional(), toTrackId: id.optional()
  }, ({ expectedRevision, idempotencyKey, ...args }) => call('devices.move', args, { expectedRevision, idempotencyKey }));
  tool('sonora_clips_import_audio', 'Import 1-16 existing audio files (absolute paths; wav/aiff/flac/mp3/ogg as the app accepts) as clips. Without trackId a new audio track is created; with trackId the clips go on that audio track at startBeat (Arrangement, default 0) or in Session slots from scene. Tempo is detected like a sample drop, so clips start warped when a tempo is found. One undo step; the receipt lists created tracks and clips (data.tracks, data.clips). Only reads the files.', {
    ...mutation, paths: z.array(z.string().min(1).max(4096)).min(1).max(16), trackId: id.optional(), startBeat: beat.optional(), scene: z.number().int().min(0).max(999).optional()
  }, ({ expectedRevision, idempotencyKey, ...args }) => call('clips.import_audio', args, { expectedRevision, idempotencyKey }));
  tool('sonora_history_step', 'Undo or redo the application history (any step, not only agent edits). expectedLabel must equal the label status.get / inspect reports for the next undo or redo, so a human edit in between is never undone by mistake. Use sonora_history_undo with the token for your own latest edit.', {
    ...mutation, direction: z.enum(['undo', 'redo']), expectedLabel: z.string().min(1).max(256)
  }, ({ expectedRevision, idempotencyKey, ...args }) => call('history.step', args, { expectedRevision, idempotencyKey }));
  // Live actions: no expectedRevision, no idempotency key, no undo. Values are absolute, so a repeat is harmless.
  tool('sonora_mixer_set', 'LIVE, not undoable: set mixer values of a track, return or the main track (trackId from inspect; main uses mainTrack.trackId). gainDb -70..6 (-70 = silence), pan -1..1, muted, solo (additive, other tracks untouched), armed (audio/MIDI tracks only; refused while that track records), sends [{sendIndex = returnTracks[] order, gainDb -70..0}]. Fails with automation_conflict when the control has an envelope (edit it with automation.set instead). The receipt returns previous and current values. The set takes the values when saved.', {
    trackId: id, gainDb: z.number().min(-70).max(6).optional(), pan: z.number().min(-1).max(1).optional(), muted: z.boolean().optional(), solo: z.boolean().optional(),
    armed: z.boolean().optional(), sends: z.array(z.object({ sendIndex: z.number().int().min(0).max(11), gainDb: z.number().min(-70).max(0) }).strict()).min(1).max(12).optional()
  }, args => call('mixer.set', args), false, false);
  tool('sonora_transport_set', 'LIVE, not undoable: action play | stop | continue | return_to_start, positionBeats (quarter notes from the start, jump), metronome on/off. Asynchronous: read sonora_status_get for the resulting position and playing state. play honours a Song Start locator unless positionBeats is given.', {
    action: z.enum(['play', 'stop', 'continue', 'return_to_start']).optional(), positionBeats: beat.optional(), metronome: z.boolean().optional()
  }, args => call('transport.set', args), false, false);
  tool('sonora_session_launch', 'LIVE, not undoable: Session view. action clip (trackId + scene, the slot must hold a clip), scene (launches the scene row), stop_track (trackId), stop_all, back_to_arrangement. Launches start at the clip\'s launch quantisation; read sonora_status_get for transport state.', {
    action: z.enum(['clip', 'scene', 'stop_track', 'stop_all', 'back_to_arrangement']), trackId: id.optional(), scene: z.number().int().min(0).max(999).optional()
  }, args => call('session.launch', args), false, false);
  tool('sonora_recording_set', 'LIVE, asynchronous: start or stop Arrangement or Session recording on the armed tracks (arm them with sonora_mixer_set armed). Starting waits for input permission and device setup; poll sonora_status_get recording. Edits are refused while recording.', {
    mode: z.enum(['arrangement', 'session']), recording: z.boolean()
  }, args => call('recording.set', args), false, false);
  tool('sonora_devices_set_enabled', 'LIVE, not undoable: switch a top-level device on or off like its power button (trackId from inspect incl. returns/main, deviceId). Refused with automation_conflict when the switch has automation. The set takes the state when saved.', {
    trackId: id, deviceId: id, enabled: z.boolean()
  }, args => call('devices.set_enabled', args), false, false);
  tool('sonora_plugins_scan', 'LIVE, asynchronous: rescan installed plug-ins (VST3 and VST2; Audio Units when includeAudioUnits). Loads third-party plug-in code in the app process and can take minutes. Poll sonora_status_get pluginScan.running, then read sonora_devices_catalog.', {
    includeAudioUnits: z.boolean().optional()
  }, args => call('plugins.scan', args), false, false);
  tool('sonora_project_save', 'LIVE: save the project in place (overwrites its .als; a backup copy goes to the Backup folder first). Fails with untitled_document when the project has no file yet or when the user is a collaboration guest: Save As needs the application dialog. The receipt has saved:true and the path.', {},
    () => call('project.save'), false, true);
  tool('sonora_project_save_copy', 'LIVE: write a copy of the project to a NEW absolute .als path (the parent folder must exist; an existing file is never overwritten; samples are copied in). The open project keeps its own file and its unsaved state.', {
    path: z.string().min(1).max(4096)
  }, args => call('project.save_copy', args), false, true);
  tool('sonora_project_export_audio', 'LIVE, asynchronous: export audio. target main (one file), track (trackId, one file) or stems (destination is a NEW folder with one file per track). destination must not exist and its parent must; the extension must match format (wav/aiff/flac; bitDepth 16/24, 32-bit float is WAV only); startBeat/endBeat in quarter notes (end after start); optional tailSeconds (0-60) or autoTail, normalize, mono, dither (default on). Returns at once: poll sonora_status_get export {phase idle|running|done|failed|cancelled, progress 0-1, files, error}. The export owns the audio graph while it runs, so edits are refused (busy).', {
    destination: z.string().min(1).max(4096), target: z.enum(['main', 'track', 'stems']), trackId: id.optional(), startBeat: beat, endBeat: beat,
    format: z.enum(['wav', 'aiff', 'flac']), bitDepth: z.union([z.literal(16), z.literal(24), z.literal(32)]), normalize: z.boolean().optional(),
    mono: z.boolean().optional(), dither: z.boolean().optional(), tailSeconds: z.number().min(0).max(60).optional(), autoTail: z.boolean().optional()
  }, args => call('project.export_audio', args), false, false);
  tool('sonora_project_export_cancel', 'LIVE: cancel the running export; poll sonora_status_get export until the phase is cancelled.', {},
    () => call('project.export_cancel'), false, false);
  tool('sonora_view_set', 'LIVE, not undoable, does not change the project: mainView session|arrangement, detail clip|devices|hidden, selectTrackId (track or return), selectClip {trackId, clipId, scene? for a Session clip}, browserVisible, mixerVisible, followPlayhead.', {
    mainView: z.enum(['session', 'arrangement']).optional(), detail: z.enum(['clip', 'devices', 'hidden']).optional(), selectTrackId: id.optional(),
    selectClip: z.object({ trackId: id, clipId: id, scene: z.number().int().min(0).max(1000000).optional() }).strict().optional(),
    browserVisible: z.boolean().optional(), mixerVisible: z.boolean().optional(), followPlayhead: z.boolean().optional()
  }, args => call('view.set', args), false, false);
  tool('sonora_devices_open_editor', 'LIVE: open the plug-in editor window of a hosted top-level plug-in (VST3, VST2 or Audio Unit) on a track, return or the main track. Native devices have no separate window.', {
    trackId: id, deviceId: id
  }, args => call('devices.open_editor', args), false, false);
  tool('sonora_media_run', 'LIVE, asynchronous audio job without prompts. Track jobs (trackId): freeze, unfreeze, bounce_in_place, bounce_new_track. Clip jobs (trackId + clipId, scene for a Session clip, audio clips only): reverse, crop, toggle_warp, audio_to_midi (mode melody|harmony|drums|slices), strip_silence (Arrangement). Returns at once: poll sonora_status_get mediaJob {state running|done|failed|finished_without_change, message}; a successful job is one history step you can see in history.undoLabel and undo with sonora_history_step. Stem separation and Resynth are not exposed (they open prompts / need an external tool).', {
    action: z.enum(['freeze', 'unfreeze', 'bounce_in_place', 'bounce_new_track', 'reverse', 'crop', 'toggle_warp', 'audio_to_midi', 'strip_silence']),
    trackId: id, clipId: id.optional(), scene: z.number().int().min(0).max(1000000).optional(), mode: z.enum(['melody', 'harmony', 'drums', 'slices']).optional()
  }, args => call('media.run', args), false, false);
  tool('sonora_history_undo', 'Undo only the latest unchanged agent transaction; refuses intervening human edits. Use returned undoToken.', {
    ...mutation, undoToken: z.string().min(1)
  }, ({ undoToken, ...guard }) => call('history.undo', { undoToken }, guard));
  return server;
}

serveStdio(createServer, { legacy: 'serve', onerror: error => process.stderr.write(`sonora-mcp: ${error.message}\n`) });
