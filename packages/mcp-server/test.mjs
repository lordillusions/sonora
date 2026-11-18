import test from 'node:test';
import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { once } from 'node:events';
import { createInterface } from 'node:readline';
import { promises as fs } from 'node:fs';
import { fileURLToPath } from 'node:url';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';

const server = fileURLToPath(new URL('./server.mjs', import.meta.url));
const probe = process.env.SONORA_CONTROL_PROBE ?? fileURLToPath(new URL('../../build/tools/control_probe', import.meta.url));
const metadata = { 'io.modelcontextprotocol/protocolVersion': '2026-07-28',
  'io.modelcontextprotocol/clientInfo': { name: 'sonora-test', version: '1' },
  'io.modelcontextprotocol/clientCapabilities': {} };
function client(modern = false) {
  const child = spawn(process.execPath, [server], { stdio: ['pipe', 'pipe', 'pipe'] });
  let id = 0, diagnostics = '';
  child.stderr.on('data', chunk => { diagnostics += chunk; });
  const pending = new Map();
  createInterface({ input: child.stdout }).on('line', line => {
    const message = JSON.parse(line);
    if (message.id && pending.has(message.id)) { pending.get(message.id)(message); pending.delete(message.id); }
  });
  return {
    child,
    request(method, params = {}) {
      const requestId = ++id;
      return new Promise((resolve, reject) => {
        const timer = setTimeout(() => { pending.delete(requestId); reject(new Error(`Timeout ${method}: ${diagnostics}`)); }, 130000);
        pending.set(requestId, value => { clearTimeout(timer); resolve(value); });
        child.stdin.write(JSON.stringify({ jsonrpc: '2.0', id: requestId, method,
          params: modern ? { ...params, _meta: metadata } : params }) + '\n');
      });
    },
    async tool(name, args = {}) {
      const response = await this.request('tools/call', { name, arguments: args });
      assert.ok(!response.error, JSON.stringify(response));
      return response.result.structuredContent;
    },
    async close() { if (child.exitCode !== null) return; const exit = once(child, 'exit'); child.stdin.end(); await exit; }
  };
}
async function initialize(c) {
  const result = await c.request('initialize', { protocolVersion: '2025-11-25', capabilities: {}, clientInfo: { name: 'sonora-test', version: '1' } });
  assert.ok(result.result, JSON.stringify(result));
  c.child.stdin.write(JSON.stringify({ jsonrpc: '2.0', method: 'notifications/initialized' }) + '\n');
}
test('MCP: legacy initialize exposes validated tools and requires explicit attachment', async () => {
  const c = client();
  try {
    await initialize(c);
    const list = await c.request('tools/list');
    assert.equal(list.result.tools.length, 31);
    const guide = await c.request('resources/read', { uri: 'sonora://docs/control-v1' });
    assert.match(guide.result.contents[0].text, /Sonora control API/);
    assert.equal((await c.tool('sonora_project_inspect')).ok, false);
    const invalid = await c.request('tools/call', { name: 'sonora_serum_supersaw', arguments: { root: 'C' } });
    assert.ok(invalid.error || invalid.result?.isError);
  } finally { await c.close(); }
});
test('MCP: modern discover and per-request metadata work through official SDK', async () => {
  const c = client(true);
  try {
    const discovery = await c.request('server/discover');
    assert.ok(discovery.result, JSON.stringify(discovery));
    const list = await c.request('tools/list');
    assert.equal(list.result.tools.length, 31);
  } finally { await c.close(); }
});
test('MCP: real Serum workflow over authenticated IPC, retry and guarded undo', { timeout: 150000 }, async () => {
  await fs.access(probe);
  const native = spawn(probe, ['--serve', '--verify-edits-on-exit'], { stdio: ['ignore', 'pipe', 'pipe'] });
  let nativeErrors = '', nativeOutput = '';
  native.stdout.on('data', bytes => { nativeOutput += bytes; });
  native.stderr.on('data', bytes => { nativeErrors += bytes; });
  const registry = await new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error(`Probe startup timeout: ${nativeErrors}`)), 10000);
    createInterface({ input: native.stdout }).once('line', line => { clearTimeout(timer); resolve(line); });
    native.once('error', reject);
  });
  const entry = JSON.parse(await fs.readFile(registry, 'utf8'));
  const c = client();
  try {
    await initialize(c);
    const discovered = await c.tool('sonora_instances_list');
    assert.ok(discovered.instances.some(item => item.instanceId === entry.instanceId));
    let current = await c.tool('sonora_session_attach', { instanceId: entry.instanceId, documentId: entry.documentId });
    assert.equal(current.ok, true);
    const args = { expectedRevision: current.revision, idempotencyKey: 'mcp-serum-test', root: 'D#', mode: 'minor', bars: 4, startBeat: 0 };
    const receipt = await c.tool('sonora_serum_supersaw', args);
    assert.equal(receipt.ok, true, JSON.stringify(receipt));
    const retry = await c.tool('sonora_serum_supersaw', args);
    assert.equal(retry.revision, receipt.revision);
    current = await c.tool('sonora_project_inspect');
    assert.equal(current.data.tracks.length, 5);
    const created = current.data.tracks.find(track => track.trackId === receipt.data.trackId);
    const parameters = await c.tool('sonora_parameters_get', { trackId: created.trackId, deviceId: created.devices[0].deviceId });
    assert.equal(parameters.ok, true);
    assert.ok(parameters.data.length > 500);
    const unison = parameters.data.find(parameter => parameter.parameterId === '1000024');
    assert.equal(unison.name, 'A Unison');
    assert.ok(Math.abs(unison.normalized - 0.4) < 0.0001);
    const clip = await c.tool('sonora_clip_get', { trackId: receipt.data.trackId, clipId: receipt.data.clipId });
    assert.equal(clip.data.notes.length, 12);
    assert.equal(clip.data.loopEnd, 16);
    const rejected = await new Promise((resolve, reject) => {
      const socket = net.createConnection(entry.socket); socket.setEncoding('utf8');
      let text = ''; socket.on('error', reject);
      socket.on('connect', () => socket.write(JSON.stringify({ token: 'wrong', request: { apiVersion: '1.0', operation: 'project.inspect' } }) + '\n'));
      socket.on('data', data => { text += data; if (text.includes('\n')) { socket.end(); resolve(JSON.parse(text)); } });
    });
    assert.equal(rejected.error.code, 'unauthorized');
    const undo = await c.tool('sonora_history_undo', { expectedRevision: current.revision, idempotencyKey: 'mcp-undo', undoToken: receipt.data.undoToken });
    assert.equal(undo.ok, true, JSON.stringify(undo));
    assert.equal((await c.tool('sonora_project_inspect')).data.tracks.length, 4);
    // Start a second composition so the original workflow's guarded undo remains independently verified.
    current = await c.tool('sonora_project_inspect');
    const second = await c.tool('sonora_serum_supersaw', { ...args, expectedRevision: current.revision, idempotencyKey: 'second-composition' });
    assert.equal(second.ok, true, JSON.stringify(second));
    const location = { trackId: second.data.trackId, clipId: second.data.clipId };
    const noteArgs = { expectedRevision: second.revision, idempotencyKey: 'general-notes', commands: [
      { type: 'notes.update', ...location, updates: [{ noteIndex: 0, velocity: 62.5 }] },
      { type: 'notes.add', ...location, notes: [{ pitch: 75, startBeat: .5, durationBeats: 1.25, velocity: 77 }] },
      { type: 'notes.delete', ...location, noteIndices: [1] }
    ] };
    const noteReceipt = await c.tool('sonora_edits_apply', noteArgs);
    assert.equal(noteReceipt.ok, true, JSON.stringify(noteReceipt));
    assert.equal((await c.tool('sonora_edits_apply', noteArgs)).revision, noteReceipt.revision);
    const page = await c.tool('sonora_clip_get', { ...location, offset: 2, limit: 1 });
    assert.equal(page.data.notes[0].pitch, 75); assert.equal(page.data.notes[0].noteIndex, 2); assert.equal(page.data.hasMore, true);
    const noteUndo = await c.tool('sonora_history_undo', { expectedRevision: noteReceipt.revision, idempotencyKey: 'notes-undo', undoToken: noteReceipt.data.undoToken });
    assert.equal(noteUndo.ok, true, JSON.stringify(noteUndo));
    assert.equal((await c.tool('sonora_clip_get', location)).data.notes[0].velocity, 88);
    const noteAgain = await c.tool('sonora_edits_apply', { ...noteArgs, expectedRevision: noteUndo.revision, idempotencyKey: 'notes-again' });
    assert.equal(noteAgain.ok, true);
    current = await c.tool('sonora_project_inspect');
    const device = current.data.tracks.find(track => track.trackId === location.trackId).devices[0];
    const parameterArgs = { trackId: location.trackId, deviceId: device.deviceId, expectedRevision: current.revision,
      idempotencyKey: 'general-parameters', parameters: [{ parameterId: '0', name: 'Main Vol', normalized: .27 }] };
    const invalidName = await c.tool('sonora_parameters_set', { ...parameterArgs, idempotencyKey: 'wrong-name',
      parameters: [{ parameterId: '0', name: 'Wrong Name', normalized: .27 }] });
    assert.equal(invalidName.ok, false); assert.equal(invalidName.error.code, 'parameter_mismatch');
    assert.equal((await c.tool('sonora_project_inspect')).revision, current.revision);
    const parameterReceipt = await c.tool('sonora_parameters_set', parameterArgs);
    assert.equal(parameterReceipt.ok, true, JSON.stringify(parameterReceipt));
    assert.equal(parameterReceipt.data.stateReloadVerified, true); assert.equal(parameterReceipt.data.liveReadbackVerified, true); assert.notEqual(parameterReceipt.data.deviceId, device.deviceId);
    assert.equal((await c.tool('sonora_parameters_set', parameterArgs)).revision, parameterReceipt.revision);
    let values = await c.tool('sonora_parameters_get', { trackId: location.trackId, deviceId: parameterReceipt.data.deviceId });
    assert.ok(Math.abs(values.data.find(p => p.parameterId === '0').normalized - .27) < .0001);
    const parameterUndo = await c.tool('sonora_history_undo', { expectedRevision: parameterReceipt.revision,
      idempotencyKey: 'parameters-undo', undoToken: parameterReceipt.data.undoToken });
    assert.equal(parameterUndo.ok, true, JSON.stringify(parameterUndo));
    values = await c.tool('sonora_parameters_get', { trackId: location.trackId, deviceId: device.deviceId });
    assert.ok(Math.abs(values.data.find(p => p.parameterId === '0').normalized - .35) < .0001);
    const parameterAgain = await c.tool('sonora_parameters_set', { ...parameterArgs, expectedRevision: parameterUndo.revision,
      idempotencyKey: 'parameters-again' });
    assert.equal(parameterAgain.ok, true, JSON.stringify(parameterAgain));

  } finally {
    await c.close();
    if (native.exitCode === null) { const exit = once(native, 'exit'); native.kill('SIGTERM'); await exit; }
  }
  assert.equal(native.exitCode, 0, nativeErrors);
  assert.match(nativeOutput, /PASS: MCP MIDI and VST3 edits save\/reload verified/);
  await assert.rejects(fs.access(registry));
});

test('MCP: discover insert edit remove native and generic VST3 devices with guarded undo and persistence', { timeout: 150000 }, async () => {
  const native = spawn(probe, ['--serve', '--verify-devices-on-exit'], { stdio: ['ignore', 'pipe', 'pipe'] });
  let output = '', errors = '';
  native.stdout.on('data', b => { output += b; }); native.stderr.on('data', b => { errors += b; });
  const registry = await new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error(`Startup timeout: ${errors}`)), 10000);
    createInterface({ input: native.stdout }).once('line', line => { clearTimeout(timer); resolve(line); });
    native.once('error', reject);
  });
  const entry = JSON.parse(await fs.readFile(registry, 'utf8')); const c = client();
  try {
    await initialize(c);
    let current = await c.tool('sonora_session_attach', { instanceId: entry.instanceId, documentId: entry.documentId });
    const trackId = current.data.tracks[0].trackId;
    const catalog = await c.tool('sonora_devices_catalog', { filter: 'Utility', kind: 'native' });
    assert.equal(catalog.data.devices.length, 1);
    const insertArgs = { trackId, catalogId: catalog.data.devices[0].catalogId, expectedRevision: current.revision, idempotencyKey: 'insert-utility' };
    const inserted = await c.tool('sonora_devices_insert', insertArgs); assert.equal(inserted.ok, true, JSON.stringify(inserted));
    assert.equal((await c.tool('sonora_devices_insert', insertArgs)).data.deviceId, inserted.data.deviceId);
    let values = await c.tool('sonora_parameters_get', { trackId, deviceId: inserted.data.deviceId });
    const gain = values.data.find(p => p.parameterId === 'Gain'); assert.equal(gain.unit, 'lin'); assert.equal(gain.normalization, 'linear');
    const changed = await c.tool('sonora_parameters_set', { trackId, deviceId: inserted.data.deviceId, expectedRevision: inserted.revision,
      idempotencyKey: 'native-gain', parameters: [{ parameterId: 'Gain', name: 'Gain', value: .3 }] });
    assert.equal(changed.ok, true, JSON.stringify(changed)); assert.equal(changed.data.liveReadbackVerified, true);
    const nativeId = changed.data.deviceId;
    const stale = await c.tool('sonora_devices_remove', { trackId, deviceId: nativeId, expectedRevision: inserted.revision, idempotencyKey: 'stale-remove' });
    assert.equal(stale.error.code, 'stale_revision');
    const synths = await c.tool('sonora_devices_catalog', { filter: 'Serum 2', kind: 'vst3' });
    const serum = synths.data.devices.find(d => d.name === 'Serum 2' && d.role === 'instrument'); assert.ok(serum);
    const synth = await c.tool('sonora_devices_insert', { trackId, catalogId: serum.catalogId, expectedRevision: changed.revision, idempotencyKey: 'generic-serum-insert' });
    assert.equal(synth.ok, true, JSON.stringify(synth));
    const patch = await c.tool('sonora_parameters_set', { trackId, deviceId: synth.data.deviceId, expectedRevision: synth.revision,
      idempotencyKey: 'generic-serum-volume', parameters: [{ parameterId: '0', name: 'Main Vol', normalized: .27 }] });
    assert.equal(patch.ok, true, JSON.stringify(patch));
    const conflict = await c.tool('sonora_devices_insert', { trackId, catalogId: 'native:SonoraFm', expectedRevision: patch.revision, idempotencyKey: 'never-replace-instrument' });
    assert.equal(conflict.error.code, 'instrument_conflict');
    const removed = await c.tool('sonora_devices_remove', { trackId, deviceId: nativeId, expectedRevision: patch.revision, idempotencyKey: 'remove-native' });
    assert.equal(removed.ok, true, JSON.stringify(removed));
    const restored = await c.tool('sonora_history_undo', { expectedRevision: removed.revision, idempotencyKey: 'restore-native', undoToken: removed.data.undoToken });
    assert.equal(restored.ok, true, JSON.stringify(restored));
    values = await c.tool('sonora_parameters_get', { trackId, deviceId: nativeId }); assert.ok(Math.abs(values.data.find(p => p.parameterId === 'Gain').value - .3) < 1e-5);
    const removedPlugin = await c.tool('sonora_devices_remove', { trackId, deviceId: patch.data.deviceId, expectedRevision: restored.revision, idempotencyKey: 'remove-vst3' });
    assert.equal(removedPlugin.ok, true, JSON.stringify(removedPlugin));
    assert.equal((await c.tool('sonora_history_undo', { expectedRevision: removedPlugin.revision, idempotencyKey: 'restore-vst3', undoToken: removedPlugin.data.undoToken })).ok, true);
    current = await c.tool('sonora_project_inspect'); assert.equal(current.data.tracks[0].devices.length, 2);
  } finally {
    await c.close(); if (native.exitCode === null) { const exit = once(native, 'exit'); native.kill('SIGTERM'); await exit; }
  }
  assert.equal(native.exitCode, 0, errors); assert.match(output, /PASS: MCP native and generic VST3 insertion/);
  await assert.rejects(fs.access(registry));
});

test('MCP: Arrangement locators add, update, delete, undo and persist through edits.apply', { timeout: 60000 }, async () => {
  const native = spawn(probe, ['--serve', '--verify-locators-on-exit'], { stdio: ['ignore', 'pipe', 'pipe'] });
  let output = '', errors = '';
  native.stdout.on('data', b => { output += b; }); native.stderr.on('data', b => { errors += b; });
  const registry = await new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error(`Startup timeout: ${errors}`)), 10000);
    createInterface({ input: native.stdout }).once('line', line => { clearTimeout(timer); resolve(line); });
    native.once('error', reject);
  });
  const entry = JSON.parse(await fs.readFile(registry, 'utf8')); const c = client();
  try {
    await initialize(c);
    let current = await c.tool('sonora_session_attach', { instanceId: entry.instanceId, documentId: entry.documentId });
    assert.deepEqual(current.data.locators, []);
    const invalid = await c.request('tools/call', { name: 'sonora_edits_apply', arguments: { expectedRevision: current.revision,
      idempotencyKey: 'bad-colour', commands: [{ type: 'locator.add', beat: 4, color: 70 }] } });
    assert.ok(invalid.error || invalid.result?.isError);
    const added = await c.tool('sonora_edits_apply', { expectedRevision: current.revision, idempotencyKey: 'add-locators', commands: [
      { type: 'locator.add', beat: 16, name: 'Drop', color: 14 }, { type: 'locator.add', beat: 0, name: 'Intro' }] });
    assert.equal(added.ok, true, JSON.stringify(added));
    const [drop, intro] = added.data.locatorIds;
    current = await c.tool('sonora_project_inspect');
    assert.deepEqual(current.data.locators.map(l => [l.name, l.beat, l.color]), [['Intro', 0, -1], ['Drop', 16, 14]]);
    const taken = await c.tool('sonora_edits_apply', { expectedRevision: current.revision, idempotencyKey: 'taken-beat',
      commands: [{ type: 'locator.update', locatorId: intro, beat: 16 }] });
    assert.equal(taken.error.code, 'invalid_request');
    const changed = await c.tool('sonora_edits_apply', { expectedRevision: current.revision, idempotencyKey: 'edit-locators', commands: [
      { type: 'locator.update', locatorId: drop, name: 'Chorus', beat: 32, color: 22, songStart: true },
      { type: 'locator.delete', locatorId: intro }] });
    assert.equal(changed.ok, true, JSON.stringify(changed));
    assert.equal((await c.tool('sonora_project_inspect')).data.locators.length, 1);
    const undone = await c.tool('sonora_history_undo', { expectedRevision: changed.revision, idempotencyKey: 'undo-locators',
      undoToken: changed.data.undoToken });
    assert.equal(undone.ok, true, JSON.stringify(undone));
    const again = await c.tool('sonora_edits_apply', { expectedRevision: undone.revision, idempotencyKey: 'edit-again', commands: [
      { type: 'locator.update', locatorId: drop, name: 'Chorus', color: 22 }] });
    assert.equal(again.ok, true, JSON.stringify(again));
  } finally {
    await c.close(); if (native.exitCode === null) { const exit = once(native, 'exit'); native.kill('SIGTERM'); await exit; }
  }
  assert.equal(native.exitCode, 0, errors); assert.match(output, /PASS: locators saved and reloaded/);
  await assert.rejects(fs.access(registry));
});

test('MCP: groups, clip create/copy and volume/filter automation through edits.apply persist (AGT-7d)', { timeout: 60000 }, async () => {
  const native = spawn(probe, ['--serve', '--verify-agent-edits-on-exit'], { stdio: ['ignore', 'pipe', 'pipe'] });
  let output = '', errors = '';
  native.stdout.on('data', b => { output += b; }); native.stderr.on('data', b => { errors += b; });
  const registry = await new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error(`Startup timeout: ${errors}`)), 10000);
    createInterface({ input: native.stdout }).once('line', line => { clearTimeout(timer); resolve(line); });
    native.once('error', reject);
  });
  const entry = JSON.parse(await fs.readFile(registry, 'utf8')); const c = client();
  try {
    await initialize(c);
    let current = await c.tool('sonora_session_attach', { instanceId: entry.instanceId, documentId: entry.documentId });
    const tracks = current.data.tracks;
    assert.ok(tracks.every(track => ['audio', 'midi', 'group'].includes(track.kind) && Number.isInteger(track.groupId)));
    const [midi, other] = tracks.filter(track => track.kind === 'midi').map(track => track.trackId);
    const audio = tracks.find(track => track.kind === 'audio').trackId;
    const folded = await c.tool('sonora_edits_apply', { expectedRevision: current.revision, idempotencyKey: 'fold-all',
      commands: [{ type: 'track.set_folded', folded: true }] });
    assert.equal(folded.ok, true, JSON.stringify(folded));
    const invalid = await c.request('tools/call', { name: 'sonora_edits_apply', arguments: { expectedRevision: folded.revision,
      idempotencyKey: 'both-places', commands: [{ type: 'clip.create', trackId: midi, startBeat: 0, scene: 0, lengthBeats: 4 }] } });
    assert.ok(invalid.error || invalid.result?.isError);
    const created = await c.tool('sonora_edits_apply', { expectedRevision: folded.revision, idempotencyKey: 'create', commands: [
      { type: 'clip.create', trackId: midi, startBeat: 32, lengthBeats: 8, name: 'Fill',
        notes: [{ pitch: 60, startBeat: 0, durationBeats: 1, velocity: 100 }, { pitch: 67, startBeat: 4, durationBeats: 1, velocity: 100 }] }] });
    assert.equal(created.ok, true, JSON.stringify(created));
    const clipId = created.data.clips[0].clipId;
    const copied = await c.tool('sonora_edits_apply', { expectedRevision: created.revision, idempotencyKey: 'copy', commands: [
      { type: 'clip.copy', trackId: midi, clipId, toTrackId: other, destinationBeat: 64 }] });
    assert.equal(copied.ok, true, JSON.stringify(copied));
    const occupied = await c.tool('sonora_edits_apply', { expectedRevision: copied.revision, idempotencyKey: 'occupied', commands: [
      { type: 'clip.create', trackId: other, startBeat: 66, lengthBeats: 2 }] });
    assert.equal(occupied.error.code, 'region_occupied');
    const filter = await c.tool('sonora_devices_insert', { expectedRevision: copied.revision, idempotencyKey: 'filter',
      trackId: audio, catalogId: 'native:AutoFilter2' });
    assert.equal(filter.ok, true, JSON.stringify(filter));
    const automated = await c.tool('sonora_edits_apply', { expectedRevision: filter.revision, idempotencyKey: 'automate', commands: [
      { type: 'automation.set', trackId: audio, target: 'volume', points: [{ beat: 16, gainDb: 0 }, { beat: 32, gainDb: -80 }] },
      { type: 'automation.set', trackId: audio, target: 'device', deviceId: filter.data.deviceId, parameterId: 'Filter_Frequency',
        rangeStartBeat: 16, rangeEndBeat: 48, points: [{ beat: 16, value: 20000 }, { beat: 44, value: 40 }] }] });
    assert.equal(automated.ok, true, JSON.stringify(automated));
    current = await c.tool('sonora_project_inspect');
    const lanes = current.data.tracks.find(track => track.trackId === audio).automation;
    assert.deepEqual(lanes.map(lane => lane.target).sort(), ['device', 'volume']);
    assert.equal(lanes.find(lane => lane.target === 'device').parameterId, 'Filter_Frequency');
  } finally {
    await c.close(); if (native.exitCode === null) { const exit = once(native, 'exit'); native.kill('SIGTERM'); await exit; }
  }
  assert.equal(native.exitCode, 0, errors); assert.match(output, /PASS: agent clip create\/copy and volume\/filter automation saved and reloaded/);
  await assert.rejects(fs.access(registry));
});

test('MCP: complete control coverage, status, mixer, transport, structure, automation, history, import and save (AGT-7e)', { timeout: 90000 }, async () => {
  const native = spawn(probe, ['--serve'], { stdio: ['ignore', 'pipe', 'pipe'] });
  let output = '', errors = '';
  native.stdout.on('data', b => { output += b; }); native.stderr.on('data', b => { errors += b; });
  const registry = await new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error(`Startup timeout: ${errors}`)), 10000);
    createInterface({ input: native.stdout }).once('line', line => { clearTimeout(timer); resolve(line); });
    native.once('error', reject);
  });
  const entry = JSON.parse(await fs.readFile(registry, 'utf8')); const c = client();
  const wav = path.join(os.tmpdir(), `sonora-mcp-import-${process.pid}.wav`);
  try {
    await initialize(c);
    // Annotations: queries are read-only, live actions are not destructive, save overwrites a file.
    const tools = Object.fromEntries((await c.request('tools/list')).result.tools.map(tool => [tool.name, tool]));
    for (const name of ['sonora_status_get', 'sonora_automation_get']) assert.equal(tools[name].annotations.readOnlyHint, true, name);
    for (const name of ['sonora_mixer_set', 'sonora_transport_set', 'sonora_session_launch', 'sonora_recording_set', 'sonora_devices_set_enabled', 'sonora_plugins_scan']) {
      assert.equal(tools[name].annotations.readOnlyHint, false, name); assert.equal(tools[name].annotations.destructiveHint, false, name);
    }
    assert.equal(tools.sonora_project_save.annotations.destructiveHint, true);
    for (const name of ['sonora_history_step', 'sonora_clips_import_audio', 'sonora_devices_move']) assert.equal(tools[name].annotations.destructiveHint, true, name);
    // Live actions take no revision: the strict schema refuses one.
    assert.equal(Object.hasOwn(tools.sonora_mixer_set.inputSchema.properties, 'expectedRevision'), false);
    assert.equal(Object.hasOwn(tools.sonora_history_step.inputSchema.properties, 'expectedRevision'), true);

    let current = await c.tool('sonora_session_attach', { instanceId: entry.instanceId, documentId: entry.documentId });
    const caps = await c.tool('sonora_capabilities_get');
    for (const operation of ['status.get', 'automation.get', 'history.step', 'devices.move', 'clips.import_audio', 'mixer.set', 'transport.set', 'project.save'])
      assert.ok(caps.data.operations.includes(operation), operation);
    assert.ok(caps.data.editTypes.includes('track.create') && caps.data.editTypes.includes('clip.update'));
    assert.ok(!caps.data.operations.includes('recording.set'), 'the headless engine has no recording hook');
    assert.ok(current.data.mainTrack && Number.isInteger(current.data.mainTrack.trackId));
    assert.ok(current.data.returnTracks.length >= 1 && Array.isArray(current.data.returnTracks[0].mixer.sends));
    assert.deepEqual(current.data.timeSignature, { numerator: 4, denominator: 4 });
    const first = current.data.tracks[0], returnTrack = current.data.returnTracks[0], main = current.data.mainTrack;

    const status = await c.tool('sonora_status_get');
    assert.equal(status.ok, true); assert.equal(status.data.liveStatus, true); assert.equal(status.data.transport.metronome, false);

    // Live mixer: tracks, returns and the main track; absolute values and a receipt with the previous ones.
    const mixed = await c.tool('sonora_mixer_set', { trackId: first.trackId, gainDb: -6, pan: -0.5, muted: true });
    assert.equal(mixed.ok, true, JSON.stringify(mixed)); assert.equal(mixed.data.undoable, false); assert.equal(mixed.status, 'applied');
    assert.equal((await c.tool('sonora_mixer_set', { trackId: returnTrack.trackId, gainDb: -12 })).ok, true);
    assert.equal((await c.tool('sonora_mixer_set', { trackId: main.trackId, gainDb: -3 })).ok, true);
    const sent = await c.tool('sonora_mixer_set', { trackId: first.trackId, sends: [{ sendIndex: 0, gainDb: -20 }] });
    assert.equal(sent.ok, true, JSON.stringify(sent));
    assert.equal((await c.tool('sonora_mixer_set', { trackId: first.trackId })).ok, false);
    assert.equal((await c.tool('sonora_mixer_set', { trackId: 424242, pan: 0 })).error.code, 'target_replaced');
    current = await c.tool('sonora_project_inspect');
    const after = current.data.tracks[0].mixer;
    assert.ok(Math.abs(after.pan + 0.5) < 1e-6 && after.muted === true);
    assert.ok(Math.abs(after.gainLinear - Math.pow(10, -6 / 20)) < 1e-4);
    assert.ok(Math.abs(after.sends[0].gainLinear - 0.1) < 1e-4);
    assert.ok(Math.abs(current.data.mainTrack.mixer.gainLinear - Math.pow(10, -3 / 20)) < 1e-4);
    assert.ok(Math.abs(current.data.returnTracks[0].mixer.gainLinear - Math.pow(10, -12 / 20)) < 1e-4);

    // Transport (the headless engine honours the metronome flag and the position request).
    assert.equal((await c.tool('sonora_transport_set', { metronome: true })).ok, true);
    assert.equal((await c.tool('sonora_status_get')).data.transport.metronome, true);
    const rewind = await c.request('tools/call', { name: 'sonora_transport_set', arguments: { action: 'rewind' } });
    assert.ok(rewind.error || rewind.result?.isError, 'an unknown action is refused by the schema');

    // Structure, song and automation edits as one undo step; IDs come back in the receipt.
    const created = await c.tool('sonora_edits_apply', { expectedRevision: current.revision, idempotencyKey: 'cover-structure', commands: [
      { type: 'track.create', kind: 'midi', name: 'Agent Lead', color: 9, afterTrackId: first.trackId },
      { type: 'track.create', kind: 'return', name: 'Agent Hall' },
      { type: 'time_signature.set', numerator: 3, denominator: 4 },
      { type: 'automation.set', trackId: main.trackId, target: 'tempo', points: [{ beat: 0, value: 100 }, { beat: 32, value: 140 }] }] });
    assert.equal(created.ok, true, JSON.stringify(created));
    assert.equal(created.data.tracks.length, 2);
    current = await c.tool('sonora_project_inspect');
    assert.equal(current.data.tracks[1].trackId, created.data.tracks[0].trackId); assert.equal(current.data.tracks[1].name, 'Agent Lead');
    assert.equal(current.data.returnTracks.at(-1).name, 'Agent Hall');
    assert.deepEqual(current.data.timeSignature, { numerator: 3, denominator: 4 });
    const lane = await c.tool('sonora_automation_get', { trackId: main.trackId, target: 'tempo' });
    assert.equal(lane.ok, true, JSON.stringify(lane)); assert.equal(lane.data.pointCount, 2); assert.equal(lane.data.points[1].value, 140);
    assert.equal((await c.tool('sonora_automation_get', { trackId: main.trackId, target: 'tempo', offset: 1, limit: 1 })).data.points[0].beat, 32);

    // General history: only the labelled step is undone, then redone.
    const labels = (await c.tool('sonora_status_get')).data.history;
    assert.equal(labels.canUndo, true);
    const wrong = await c.tool('sonora_history_step', { expectedRevision: current.revision, idempotencyKey: 'step-wrong', direction: 'undo', expectedLabel: 'Not the label' });
    assert.equal(wrong.error.code, 'undo_conflict');
    const undone = await c.tool('sonora_history_step', { expectedRevision: current.revision, idempotencyKey: 'step-undo', direction: 'undo', expectedLabel: labels.undoLabel });
    assert.equal(undone.ok, true, JSON.stringify(undone));
    assert.equal((await c.tool('sonora_project_inspect')).data.tracks.length, current.data.tracks.length - 1);
    const redone = await c.tool('sonora_history_step', { expectedRevision: undone.revision, idempotencyKey: 'step-redo', direction: 'redo', expectedLabel: labels.undoLabel });
    assert.equal(redone.ok, true, JSON.stringify(redone));

    // Audio import: a one-second 44.1 kHz mono WAV becomes a clip on a new audio track.
    const frames = 44100, data = Buffer.alloc(frames * 2);
    for (let i = 0; i < frames; i++) data.writeInt16LE(Math.round(Math.sin(i / 20) * 8000), i * 2);
    const header = Buffer.alloc(44);
    header.write('RIFF', 0); header.writeUInt32LE(36 + data.length, 4); header.write('WAVEfmt ', 8); header.writeUInt32LE(16, 16);
    header.writeUInt16LE(1, 20); header.writeUInt16LE(1, 22); header.writeUInt32LE(44100, 24); header.writeUInt32LE(88200, 28);
    header.writeUInt16LE(2, 32); header.writeUInt16LE(16, 34); header.write('data', 36); header.writeUInt32LE(data.length, 40);
    await fs.writeFile(wav, Buffer.concat([header, data]));
    current = await c.tool('sonora_project_inspect');
    assert.equal((await c.tool('sonora_clips_import_audio', { expectedRevision: current.revision, idempotencyKey: 'import-relative', paths: ['relative.wav'] })).error.code, 'invalid_request');
    const imported = await c.tool('sonora_clips_import_audio', { expectedRevision: current.revision, idempotencyKey: 'import-wav', paths: [wav], startBeat: 8 });
    assert.equal(imported.ok, true, JSON.stringify(imported));
    current = await c.tool('sonora_project_inspect');
    const withAudio = current.data.tracks.find(track => track.clips.some(clip => clip.kind === 'audio'));
    assert.ok(withAudio, 'an audio clip exists after the import');
    assert.equal(withAudio.clips.find(clip => clip.kind === 'audio').sampleFile, path.basename(wav));

    // Save in place (the headless engine saves to its working file).
    const saved = await c.tool('sonora_project_save');
    assert.equal(saved.ok, true, JSON.stringify(saved)); assert.equal(saved.data.saved, true);
    assert.equal((await c.tool('sonora_status_get')).data.dirty, false);
  } finally {
    await fs.rm(wav, { force: true });
    await c.close(); if (native.exitCode === null) { const exit = once(native, 'exit'); native.kill('SIGTERM'); await exit; }
  }
  assert.equal(native.exitCode, 0, errors);
  await assert.rejects(fs.access(registry));
});
