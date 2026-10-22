import test from 'node:test';
import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { once } from 'node:events';
import { createInterface } from 'node:readline';
import { promises as fs } from 'node:fs';
import { fileURLToPath } from 'node:url';
import net from 'node:net';

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
    assert.equal(list.result.tools.length, 13);
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
    assert.equal(list.result.tools.length, 13);
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
