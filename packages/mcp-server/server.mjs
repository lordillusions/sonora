// MCP-1: stdio adapter over Sonora's opt-in, document-scoped local control service.
import { McpServer } from '@modelcontextprotocol/server';
import { serveStdio } from '@modelcontextprotocol/server/stdio';
import * as z from 'zod/v4';
import { promises as fs } from 'node:fs';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';

// One registry per app data folder (COM-4c): the stable app and "Sonora Nightly" never share data.
const registryDirectories = ['Sonora', 'Sonora Nightly']
  .map(folder => path.join(os.homedir(), 'Library/Application Support', folder, 'Automation/instances'));
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
const command = z.discriminatedUnion('type', [
  z.object({ type: z.literal('tempo.set'), bpm: z.number().min(20).max(999) }).strict(),
  z.object({ type: z.literal('track.set_gain'), trackId: id, gainDb: z.number().min(-90).max(6) }).strict(),
  z.object({ type: z.literal('clip.duplicate'), trackId: id, clipId: id, destinationBeat: z.number().min(0).max(1000000) }).strict(),
  z.object({ type: z.literal('notes.add'), ...location, notes: z.array(note).min(1).max(4096) }).strict(),
  z.object({ type: z.literal('notes.replace'), ...location, notes: z.array(note).max(4096) }).strict(),
  z.object({ type: z.literal('notes.update'), ...location, updates: z.array(update).min(1).max(4096) }).strict(),
  z.object({ type: z.literal('notes.delete'), ...location, noteIndices: z.array(id).min(1).max(4096) }).strict()
]);
function createServer() {
  const server = new McpServer({ name: 'sonora', version: '0.3.0' });
  server.registerResource('control-guide', 'sonora://docs/control-v1', { mimeType: 'text/markdown' }, async uri => ({
    contents: [{ uri: uri.href, mimeType: 'text/markdown', text: await fs.readFile(new URL('../../docs/agent/CONTROL_API.md', import.meta.url), 'utf8') }]
  }));
  function tool(name, description, shape, handler, readOnly = false) {
    server.registerTool(name, {
      description, inputSchema: z.object(shape).strict(), outputSchema: z.object({ ok: z.boolean() }).passthrough(),
      annotations: { readOnlyHint: readOnly, destructiveHint: !readOnly, openWorldHint: false }
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
  tool('sonora_project_inspect', 'Read revision, tracks, Arrangement/Session/take clip handles and top-level devices before editing.', {}, () => call('project.inspect'), true);
  tool('sonora_capabilities_get', 'Read implemented operations and explicit limitations.', {}, () => call('capabilities.get'), true);
  tool('sonora_clip_get', 'Read MIDI notes with revision-scoped noteIndex handles and paging. Default Arrangement; scene selects Session, laneId selects take (never both). Times are clip-content quarter-note beats.', {
    ...location, offset: id.optional(), limit: z.number().int().min(1).max(4096).optional()
  }, args => call('clips.get', args), true);
  tool('sonora_parameters_get', 'Inspect native/rack/VST3 parameter IDs, full names and writable metadata. Native parameters include value/unit/minimum/maximum/choices and linear normalization.', {
    trackId: z.number().int().nonnegative(), deviceId: z.number().int().nonnegative()
  }, args => call('devices.parameters', args), true);
  tool('sonora_parameters_set', 'Set 1–128 exact native/rack/VST3 parameter ID/name pairs. Supply exactly one of normalized (0–1) or native value (advertised units); VST3 accepts normalized only. Requires writable descriptors; rejects authored automation. Prepares and reload-verifies opaque state before one undoable commit. Rebuilds the affected processor (may interrupt voices/close editor); use the returned NEW deviceId. Does not save the project.', {
    ...mutation, trackId: id, deviceId: id, parameters: z.array(parameterWrite).min(1).max(128)
  }, ({ expectedRevision, idempotencyKey, ...args }) => call('devices.parameters.set', args, { expectedRevision, idempotencyKey }));
  tool('sonora_edits_apply', 'Apply a staged batch as one undo step: tempo, gain, clip duplicate or notes.add/update/delete/replace. Inspect notes first; noteIndex refers to current sorted notes and changes after each command. scene/laneId select Session/take. Replace is whole-content replacement (empty clears); add/update preserve other notes. Automated gain is rejected.', {
    ...mutation, commands: z.array(command).min(1).max(32)
  }, ({ commands, ...guard }) => call('edits.apply', { commands }, guard));
  tool('sonora_devices_catalog', 'List insertable implemented native devices and scanned VST3 plugins. Select exact catalogId, no file paths. No installation or scanning.', {
    filter: z.string().min(1).max(256).optional(), kind: z.enum(['native', 'vst3']).optional(), offset: id.optional(), limit: z.number().int().min(1).max(128).optional()
  }, args => call('devices.catalog', args), true);
  tool('sonora_devices_insert', 'Insert a catalogId on an existing track. Optional zero-based position; defaults to MIDI effects, instrument, audio effects order. MIDI devices require MIDI tracks; existing instruments are never silently replaced. Returns a new device ID and one undo token.', {
    ...mutation, trackId: id, catalogId: z.string().min(1).max(1024), position: z.number().int().min(0).max(64).optional()
  }, ({ expectedRevision, idempotencyKey, ...args }) => call('devices.insert', args, { expectedRevision, idempotencyKey }));
  tool('sonora_devices_remove', 'Remove an exact top-level device after capturing live state for undo. Does not uninstall plugins or delete source media; can interrupt voices.', {
    ...mutation, trackId: id, deviceId: id
  }, ({ expectedRevision, idempotencyKey, ...args }) => call('devices.remove', args, { expectedRevision, idempotencyKey }));
  tool('sonora_serum_supersaw', 'Create a NEW MIDI track with the real Serum 2 VST3 and a four-bar D# natural minor supersaw progression: D#m, B, F#, C#. Requires 4/4 and scanned Serum 2.0.24. Keeps project tempo; no save/export; returns actual settings and guarded undo token. Declare the minor assumption if the user omitted mode.', {
    ...mutation, root: z.literal('D#'), mode: z.literal('minor'), bars: z.literal(4), startBeat: z.number().min(0).max(1000000).default(0)
  }, ({ expectedRevision, idempotencyKey, ...args }) => call('workflow.serum_supersaw', args, { expectedRevision, idempotencyKey }));
  tool('sonora_history_undo', 'Undo only the latest unchanged agent transaction; refuses intervening human edits. Use returned undoToken.', {
    ...mutation, undoToken: z.string().min(1)
  }, ({ undoToken, ...guard }) => call('history.undo', { undoToken }, guard));
  return server;
}

serveStdio(createServer, { legacy: 'serve', onerror: error => process.stderr.write(`sonora-mcp: ${error.message}\n`) });
