// Real-app smoke run for the Sonora control surface (AGT-7e). NOT part of `npm test`.
//
//   node packages/mcp-server/app-smoke.mjs [--build build-mcp]
//
// It writes a disposable set and a short WAV into a temp folder, launches the DEBUG app through LaunchServices with its own data
// folder (SONORA_DATA_FOLDER=SonoraMcpTest, so a Sonora you have open is never touched) and SONORA_CONTROL_AUTOTEST=1 (Debug builds
// only: grants the local control grant once the set is installed), then drives it through the real server.mjs (stdio MCP) and checks
// every effect with a follow-up read. The saved files are read back with build/tools/als_dump. It quits only the instance it started
// (by pid) and removes the data folder and temp files.
//
// Deliberately NOT exercised: recording.set with recording:true and arming tracks (microphone permission dialogs) and plugins.scan
// (loads every installed third-party plug-in). Sound is played for about a second at low level (transport.set play).
import { spawn, spawnSync } from 'node:child_process';
import { once } from 'node:events';
import { createInterface } from 'node:readline';
import { promises as fs } from 'node:fs';
import { fileURLToPath } from 'node:url';
import assert from 'node:assert/strict';
import os from 'node:os';
import path from 'node:path';

const repo = fileURLToPath(new URL('../../', import.meta.url));
const argBuild = process.argv.indexOf('--build');
const build = path.resolve(repo, argBuild >= 0 ? process.argv[argBuild + 1] : 'build-mcp');
const app = path.join(build, 'src/Sonora_artefacts/Debug/Sonora.app');
const probe = path.join(build, 'tools/control_probe'), alsDump = path.join(build, 'tools/als_dump');
const dataFolder = 'SonoraMcpTest';
const registryDir = path.join(os.homedir(), 'Library/Application Support', dataFolder, 'Automation/instances');
const work = await fs.mkdtemp(path.join(os.tmpdir(), 'sonora-app-smoke-'));
const setPath = path.join(work, 'Smoke.als'), wavPath = path.join(work, 'tone.wav'), log = path.join(work, 'app.log');
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
let passed = 0;
const check = (condition, message) => { assert.ok(condition, message); passed++; console.log(`ok   ${message}`); };

function mcpClient() {
  const child = spawn(process.execPath, [fileURLToPath(new URL('./server.mjs', import.meta.url))],
    { stdio: ['pipe', 'pipe', 'pipe'], env: { ...process.env, SONORA_MCP_REGISTRY_DIR: registryDir } });
  let id = 0; const pending = new Map();
  createInterface({ input: child.stdout }).on('line', line => {
    const message = JSON.parse(line);
    if (message.id && pending.has(message.id)) { pending.get(message.id)(message); pending.delete(message.id); }
  });
  const request = (method, params = {}) => new Promise((resolve, reject) => {
    const requestId = ++id; const timer = setTimeout(() => reject(new Error(`timeout ${method}`)), 130000);
    pending.set(requestId, value => { clearTimeout(timer); resolve(value); });
    child.stdin.write(JSON.stringify({ jsonrpc: '2.0', id: requestId, method, params }) + '\n');
  });
  return {
    child, request,
    async tool(name, args = {}) {
      const response = await request('tools/call', { name, arguments: args });
      assert.ok(!response.error, JSON.stringify(response));
      return response.result.structuredContent;
    },
    async init() {
      await request('initialize', { protocolVersion: '2025-11-25', capabilities: {}, clientInfo: { name: 'app-smoke', version: '1' } });
      child.stdin.write(JSON.stringify({ jsonrpc: '2.0', method: 'notifications/initialized' }) + '\n');
    }
  };
}
function wav(seconds = 2) {
  const frames = 44100 * seconds, data = Buffer.alloc(frames * 2);
  // 0.5 s of tone, silence, 0.5 s of tone: strip_silence has something to strip.
  for (let i = 0; i < frames; i++) { const tone = i < 22050 || i >= frames - 22050; data.writeInt16LE(tone ? Math.round(Math.sin(i * 2 * Math.PI * 440 / 44100) * 12000) : 0, i * 2); }
  const header = Buffer.alloc(44);
  header.write('RIFF', 0); header.writeUInt32LE(36 + data.length, 4); header.write('WAVEfmt ', 8); header.writeUInt32LE(16, 16);
  header.writeUInt16LE(1, 20); header.writeUInt16LE(1, 22); header.writeUInt32LE(44100, 24); header.writeUInt32LE(88200, 28);
  header.writeUInt16LE(2, 32); header.writeUInt16LE(16, 34); header.write('data', 36); header.writeUInt32LE(data.length, 40);
  return Buffer.concat([header, data]);
}
async function peakOf(file) { // 16/24-bit PCM WAV (RIFF) peak, 0..1
  const bytes = await fs.readFile(file); let offset = 12, peak = 0;
  while (offset + 8 <= bytes.length) {
    const tag = bytes.toString('ascii', offset, offset + 4), size = bytes.readUInt32LE(offset + 4);
    if (tag === 'fmt ') var bits = bytes.readUInt16LE(offset + 22);
    if (tag === 'data') {
      const step = bits / 8;
      for (let i = offset + 8; i + step <= offset + 8 + size; i += step)
        peak = Math.max(peak, Math.abs(step === 2 ? bytes.readInt16LE(i) / 32768 : bytes.readIntLE(i, 3) / 8388608));
      return peak;
    }
    offset += 8 + size + (size & 1);
  }
  return 0;
}
const dump = file => spawnSync(alsDump, [file], { encoding: 'utf8' }).stdout;

let pid, c;
try {
  await fs.access(app); await fs.access(probe); await fs.access(alsDump);
  const made = spawnSync(probe, ['--write-set', setPath]); assert.equal(made.status, 0, 'control_probe --write-set');
  await fs.writeFile(wavPath, wav());
  await fs.rm(path.join(os.homedir(), 'Library/Application Support', dataFolder), { recursive: true, force: true });
  const launched = spawnSync('open', ['-n', app, '--env', `SONORA_DATA_FOLDER=${dataFolder}`, '--env', 'SONORA_CONTROL_AUTOTEST=1', '--stderr', log, '--args', setPath]);
  assert.equal(launched.status, 0, String(launched.stderr));
  c = mcpClient(); await c.init();
  let entry;
  for (let i = 0; i < 120 && !entry; i++) {
    await sleep(500);
    entry = (await c.tool('sonora_instances_list')).instances?.[0];
  }
  assert.ok(entry, `the app never registered a control grant; see ${log}`);
  pid = entry.pid;
  let current = await c.tool('sonora_session_attach', { instanceId: entry.instanceId, documentId: entry.documentId });
  check(current.ok && current.data.tracks.length === 4 && current.data.untitled === false, 'attached to the running Debug app; the disposable set has 4 tracks');
  const caps = await c.tool('sonora_capabilities_get');
  for (const op of ['mixer.set', 'transport.set', 'session.launch', 'project.save', 'project.save_copy', 'project.export_audio', 'view.set', 'media.run', 'clips.import_audio', 'devices.set_enabled'])
    assert.ok(caps.data.operations.includes(op), `capability ${op}`);
  check(!caps.data.operations.includes('plugins.scan') === false, 'capabilities list the live actions the app supplies');

  const edit = async (commands, label) => {
    current = await c.tool('sonora_project_inspect');
    const result = await c.tool('sonora_edits_apply', { expectedRevision: current.revision, idempotencyKey: `smoke-${label}-${Date.now()}`, commands });
    assert.equal(result.ok, true, `${label}: ${JSON.stringify(result)}`);
    return result;
  };
  const made3 = await edit([{ type: 'track.create', kind: 'midi', name: 'Smoke Lead' }, { type: 'track.create', kind: 'audio', name: 'Smoke Audio' },
    { type: 'track.create', kind: 'return', name: 'Smoke Verb' }], 'tracks');
  current = await c.tool('sonora_project_inspect');
  const lead = made3.data.tracks[0].trackId, audio = made3.data.tracks[1].trackId, verb = made3.data.tracks[2].trackId;
  check(current.data.tracks.some(t => t.trackId === lead && t.name === 'Smoke Lead' && t.kind === 'midi')
    && current.data.tracks.some(t => t.trackId === audio && t.kind === 'audio') && current.data.returnTracks.some(t => t.trackId === verb && t.name === 'Smoke Verb'),
    'track.create: MIDI, audio and return tracks exist with their names');

  current = await c.tool('sonora_project_inspect');
  const inst = await c.tool('sonora_devices_insert', { expectedRevision: current.revision, idempotencyKey: 'smoke-inst', trackId: lead, catalogId: 'native:SonoraFm' });
  assert.equal(inst.ok, true, JSON.stringify(inst));
  current = await c.tool('sonora_project_inspect');
  const fx = await c.tool('sonora_devices_insert', { expectedRevision: current.revision, idempotencyKey: 'smoke-fx', trackId: lead, catalogId: 'native:StereoGain' });
  assert.equal(fx.ok, true, JSON.stringify(fx));
  current = await c.tool('sonora_project_inspect');
  const leadNow = current.data.tracks.find(t => t.trackId === lead);
  check(leadNow.devices.length === 2 && leadNow.devices[0].role === 'instrument' && leadNow.devices[1].role === 'audioEffect', 'devices.insert: a native instrument then a native effect on the MIDI track');

  await edit([{ type: 'clip.create', trackId: lead, startBeat: 0, lengthBeats: 8, name: 'Smoke Clip',
    notes: [{ pitch: 60, startBeat: 0, durationBeats: 2, velocity: 90 }, { pitch: 64, startBeat: 2, durationBeats: 2, velocity: 90 }, { pitch: 67, startBeat: 4, durationBeats: 4, velocity: 90 }] },
    { type: 'clip.create', trackId: lead, scene: 0, lengthBeats: 4, name: 'Smoke Scene', notes: [{ pitch: 72, startBeat: 0, durationBeats: 1, velocity: 80 }] },
    { type: 'automation.set', trackId: lead, target: 'volume', points: [{ beat: 0, gainDb: 0 }, { beat: 8, gainDb: -6 }] }], 'clip');
  current = await c.tool('sonora_project_inspect');
  const leadClips = current.data.tracks.find(t => t.trackId === lead);
  check(leadClips.clips.some(x => x.name === 'Smoke Clip' && x.noteCount === 3) && leadClips.sessionClips.length === 1 && leadClips.automation.some(a => a.target === 'volume' && a.pointCount === 2),
    'clip.create (Arrangement + Session) and automation.set are in the project');
  const lane = await c.tool('sonora_automation_get', { trackId: lead, target: 'volume' });
  check(lane.ok && lane.data.pointCount === 2, 'automation.get reads the lane back');

  // Live mixer on track, return and main, read back through inspect (the live engine values).
  assert.equal((await c.tool('sonora_mixer_set', { trackId: audio, gainDb: -6, pan: -0.5, muted: true, solo: false,
    sends: [{ sendIndex: 0, gainDb: -12 }] })).ok, true);
  assert.equal((await c.tool('sonora_mixer_set', { trackId: verb, gainDb: -9, pan: 0.25 })).ok, true);
  assert.equal((await c.tool('sonora_mixer_set', { trackId: current.data.mainTrack.trackId, gainDb: -18 })).ok, true);
  assert.equal((await c.tool('sonora_mixer_set', { trackId: lead, solo: true })).ok, true);
  current = await c.tool('sonora_project_inspect');
  const a = current.data.tracks.find(t => t.trackId === audio).mixer, r = current.data.returnTracks.find(t => t.trackId === verb).mixer;
  check(Math.abs(a.gainLinear - 10 ** (-6 / 20)) < 1e-3 && Math.abs(a.pan + 0.5) < 1e-6 && a.muted && Math.abs(a.sends[0].gainLinear - 10 ** (-12 / 20)) < 1e-3
    && Math.abs(r.gainLinear - 10 ** (-9 / 20)) < 1e-3 && Math.abs(r.pan - 0.25) < 1e-6 && Math.abs(current.data.mainTrack.mixer.gainLinear - 10 ** (-18 / 20)) < 1e-3
    && current.data.tracks.find(t => t.trackId === lead).mixer.solo === true, 'mixer.set: gain/pan/mute/solo/sends on track, return and main show in inspect');
  const afterMixer = await c.tool('sonora_status_get');
  check(afterMixer.data.dirty === true, 'a mixer move marks the document edited');
  // A guarded mutation sent with the revision of a live action must be accepted (markHandEdited is called before the receipt).
  const live = await c.tool('sonora_mixer_set', { trackId: lead, solo: false });
  const guarded = await c.tool('sonora_edits_apply', { expectedRevision: live.revision, idempotencyKey: `smoke-live-revision-${Date.now()}`, commands: [{ type: 'tempo.set', bpm: 126 }] });
  check(guarded.ok === true, 'a mutation sent with the revision returned by a live action is accepted');

  const toggled = await c.tool('sonora_devices_set_enabled', { trackId: lead, deviceId: leadNow.devices[1].deviceId, enabled: false });
  current = await c.tool('sonora_project_inspect');
  check(toggled.ok && current.data.tracks.find(t => t.trackId === lead).devices[1].enabled === false, 'devices.set_enabled switches the effect off');

  // Transport on the real audio device.
  await c.tool('sonora_transport_set', { positionBeats: 0 });
  await c.tool('sonora_transport_set', { action: 'play', metronome: false });
  await sleep(1200);
  const playing = await c.tool('sonora_status_get');
  await sleep(600);
  const later = await c.tool('sonora_status_get');
  check(playing.data.transport.playing === true && later.data.transport.positionBeats > playing.data.transport.positionBeats && later.data.transport.positionBeats > 0.5, 'transport.set play: status shows playing and an advancing position');
  await c.tool('sonora_transport_set', { action: 'stop' });
  await sleep(500);
  check((await c.tool('sonora_status_get')).data.transport.playing === false, 'transport.set stop');

  const launch = await c.tool('sonora_session_launch', { action: 'clip', trackId: lead, scene: 0 });
  await sleep(1200);
  const stopAll = await c.tool('sonora_session_launch', { action: 'stop_all' });
  check(launch.ok && stopAll.ok, 'session.launch clip and stop_all are accepted by the app');
  await c.tool('sonora_transport_set', { action: 'stop' });

  current = await c.tool('sonora_project_inspect');
  const imported = await c.tool('sonora_clips_import_audio', { expectedRevision: current.revision, idempotencyKey: 'smoke-import', paths: [wavPath], trackId: audio, startBeat: 0 });
  assert.equal(imported.ok, true, JSON.stringify(imported));
  current = await c.tool('sonora_project_inspect');
  const audioTrack = current.data.tracks.find(t => t.trackId === audio);
  check(audioTrack.clips.some(x => x.kind === 'audio' && x.sampleFile === 'tone.wav'), 'clips.import_audio put the WAV on the audio track');

  const waitJob = async (label) => {
    for (let i = 0; i < 240; i++) {
      await sleep(500);
      const job = (await c.tool('sonora_status_get')).data.mediaJob;
      if (job && ['done', 'failed', 'finished_without_change'].includes(job.state)) return job;
    }
    throw new Error(`media job ${label} did not finish`);
  };
  const tracksBefore = (await c.tool('sonora_project_inspect')).data.tracks.length;
  assert.equal((await c.tool('sonora_media_run', { action: 'freeze', trackId: lead })).ok, true);
  let job = await waitJob('freeze');
  current = await c.tool('sonora_project_inspect');
  check(job.state === 'done' && current.data.tracks.find(t => t.trackId === lead).frozen === true, `media.run freeze: ${job.state}, "${job.undoLabel}", track is frozen`);
  assert.equal((await c.tool('sonora_media_run', { action: 'unfreeze', trackId: lead })).ok, true);
  await sleep(800);
  current = await c.tool('sonora_project_inspect');
  check(current.data.tracks.find(t => t.trackId === lead).frozen === false, 'media.run unfreeze');
  assert.equal((await c.tool('sonora_media_run', { action: 'bounce_new_track', trackId: lead })).ok, true);
  job = await waitJob('bounce');
  current = await c.tool('sonora_project_inspect');
  check(job.state === 'done' && current.data.tracks.length === tracksBefore + 1, `media.run bounce_new_track: ${job.state}, a new track exists`);
  for (const [action, extra] of [['reverse', {}], ['toggle_warp', {}], ['crop', {}], ['audio_to_midi', { mode: 'melody' }], ['strip_silence', {}]]) {
    current = await c.tool('sonora_project_inspect');
    const clip = current.data.tracks.find(t => t.trackId === audio).clips.find(x => x.kind === 'audio');
    const started = await c.tool('sonora_media_run', { action, trackId: audio, clipId: clip.clipId, ...extra });
    assert.equal(started.ok, true, `${action}: ${JSON.stringify(started)}`);
    const result = await waitJob(action);
    console.log(`     media.run ${action}: ${result.state} (${result.message}; undo: ${result.undoLabel})`);
    check(result.state === 'done', `media.run ${action} finished as one history step`);
  }
  const refused = await c.tool('sonora_media_run', { action: 'reverse', trackId: lead, clipId: leadClips.clips[0].clipId });
  check(refused.ok === false && refused.error.code === 'unsupported_clip', 'media.run reverse on a MIDI clip is refused by validation');

  const view = await c.tool('sonora_view_set', { mainView: 'session', detail: 'devices', browserVisible: false, selectTrackId: audio });
  const view2 = await c.tool('sonora_view_set', { mainView: 'arrangement' });
  check(view.ok && view2.data.previous.mainView === 'session' && view2.data.previous.browserVisible === false, 'view.set changed the window (the second call reports the first one as previous)');

  // General history.
  const labels = (await c.tool('sonora_status_get')).data.history;
  current = await c.tool('sonora_project_inspect');
  const undone = await c.tool('sonora_history_step', { expectedRevision: current.revision, idempotencyKey: 'smoke-undo', direction: 'undo', expectedLabel: labels.undoLabel });
  assert.equal(undone.ok, true, JSON.stringify(undone));
  const clipsOnAudio = async () => (await c.tool('sonora_project_inspect')).data.tracks.find(t => t.trackId === audio).clips.length;
  const afterUndo = (await c.tool('sonora_status_get')).data.history;
  const fragmentsUndone = await clipsOnAudio();
  check(afterUndo.redoLabel === labels.undoLabel, `history.step undo took back "${labels.undoLabel}" (redo label matches)`);
  const redone = await c.tool('sonora_history_step', { expectedRevision: (await c.tool('sonora_project_inspect')).revision, idempotencyKey: 'smoke-redo', direction: 'redo', expectedLabel: labels.undoLabel });
  const afterRedo = (await c.tool('sonora_status_get')).data.history;
  check(redone.ok && afterRedo.undoLabel === labels.undoLabel && await clipsOnAudio() > fragmentsUndone, 'history.step redo re-applied it (the stripped clip is in fragments again)');

  // Save and read the file back.
  const saved = await c.tool('sonora_project_save');
  assert.equal(saved.ok, true, JSON.stringify(saved));
  const text = dump(setPath);
  check(text.includes('Smoke Lead') && text.includes('Smoke Audio') && text.includes('Smoke Verb') && /Smoke Audio\s+color=\d+\s+group=-1\s+vol=0\.50\d pan=-0\.50 OFF/.test(text),
    'project.save: als_dump shows the new tracks and the live mixer values (gain, pan, mute) in the file');
  const params = spawnSync(alsDump, [setPath, '--notes'], { encoding: 'utf8' }).stdout;
  check(/Smoke Clip/.test(params) && /SonoraFm|Sonora FM|StereoGain|Utility/i.test(spawnSync(alsDump, [setPath, '--params'], { encoding: 'utf8' }).stdout), 'the saved file holds the clip and the devices');
  const copyPath = path.join(work, 'Copy.als');
  const copy = await c.tool('sonora_project_save_copy', { path: copyPath });
  check(copy.ok && dump(copyPath).includes('Smoke Lead') && (await c.tool('sonora_project_inspect')).data.path === setPath, 'project.save_copy wrote a loadable copy and the open project kept its own path');
  check((await c.tool('sonora_project_save_copy', { path: copyPath })).error.code === 'destination_exists', 'save_copy never overwrites');

  // Export a few bars.
  const out = path.join(work, 'mix.wav');
  const exported = await c.tool('sonora_project_export_audio', { destination: out, target: 'main', startBeat: 0, endBeat: 16, format: 'wav', bitDepth: 16, tailSeconds: 0 });
  assert.equal(exported.ok, true, JSON.stringify(exported));
  let state;
  for (let i = 0; i < 240; i++) { await sleep(500); state = (await c.tool('sonora_status_get')).data.export; if (state.phase !== 'running') break; }
  check(state.phase === 'done' && state.files.length === 1 && state.files[0].endsWith('mix.wav'), 'project.export_audio finished: status.get export phase done with the file');
  const peak = await peakOf(out);
  check(peak > 0.001, `the exported WAV is not silent (peak ${peak.toFixed(4)})`);
  const second = await c.tool('sonora_project_export_audio', { destination: out, target: 'main', startBeat: 0, endBeat: 16, format: 'wav', bitDepth: 16 });
  check(second.ok === false && second.error.code === 'destination_exists', 'export refuses an existing destination');
  const long = await c.tool('sonora_project_export_audio', { destination: path.join(work, 'long.wav'), target: 'main', startBeat: 0, endBeat: 6000, format: 'wav', bitDepth: 16 });
  assert.equal(long.ok, true, JSON.stringify(long));
  await sleep(700);
  const cancelled = await c.tool('sonora_project_export_cancel');
  for (let i = 0; i < 120; i++) { await sleep(500); state = (await c.tool('sonora_status_get')).data.export; if (state.phase !== 'running') break; }
  check(cancelled.ok && state.phase === 'cancelled', `project.export_cancel stopped a long export (phase ${state.phase})`);
  const cancelIdle = await c.tool('sonora_project_export_cancel');
  check(cancelIdle.ok === false, 'export_cancel with nothing running is refused');
  console.log(`\nAll ${passed} checks passed. Not run in the real app: recording.set (recording:true), arming tracks, plugins.scan.`);
} finally {
  if (c) { c.child.stdin.end(); }
  if (pid) {
    try { process.kill(pid, 'SIGTERM'); } catch { /* already gone */ }
    for (let i = 0; i < 20; i++) { await sleep(500); try { process.kill(pid, 0); } catch { pid = undefined; break; } }
    if (pid) { try { process.kill(pid, 'SIGKILL'); } catch { /* gone */ } }
  }
  await fs.rm(path.join(os.homedir(), 'Library/Application Support', dataFolder), { recursive: true, force: true });
  if (!process.argv.includes('--keep')) await fs.rm(work, { recursive: true, force: true });
  else console.log(`kept ${work}`);
}
