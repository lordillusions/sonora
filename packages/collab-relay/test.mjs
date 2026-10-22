import test from 'node:test';
import assert from 'node:assert/strict';
import { createHmac } from 'node:crypto';
import { execFile } from 'node:child_process';
import { promises as fs } from 'node:fs';
import { promisify } from 'node:util';
import { fileURLToPath } from 'node:url';
import { sha256 } from './accounts.mjs';
import { addAccount, api, start, tempStore, ticket } from './test-helpers.mjs';
import { configFromEnv, createRelay } from './relay.mjs';

const run = promisify(execFile);
const admin = fileURLToPath(new URL('./admin.mjs', import.meta.url));
const day = 86400000;

function socket(base, sessionId) {
  const ws = new WebSocket(`${base.replace('http', 'ws')}/v1/signal?session=${sessionId}`);
  const queue = [], waiters = [], closeWaiters = [];
  let closed = null;
  ws.addEventListener('message', event => {
    const message = JSON.parse(event.data);
    if (waiters.length) waiters.shift()(message); else queue.push(message);
  });
  ws.addEventListener('close', event => { closed = { code: event.code, reason: event.reason }; closeWaiters.splice(0).forEach(fn => fn(closed)); });
  ws.addEventListener('error', () => {});
  const wait = (list, ms, what) => new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error(`timeout waiting for ${what}`)), ms);
    list.push(value => { clearTimeout(timer); resolve(value); });
  });
  return {
    opened: new Promise(resolve => ws.addEventListener('open', resolve, { once: true })),
    send: message => ws.send(typeof message === 'string' ? message : JSON.stringify(message)),
    next: (ms = 2000) => queue.length ? Promise.resolve(queue.shift()) : wait(waiters, ms, 'message'),
    closed: (ms = 2000) => closed ? Promise.resolve(closed) : wait(closeWaiters, ms, 'close'),
    close: () => ws.close(),
    pending: () => queue.length,
  };
}
async function join(base, sessionId, hello) {
  const peer = socket(base, sessionId);
  await peer.opened;
  peer.send({ type: 'hello', protocol: 1, ...hello });
  return peer;
}

test('relay: cloud hosting requires an active subscription; guests and expired plans are handled server side', async () => {
  const file = await tempStore();
  const active = await addAccount(file, { plan: 'cloud', activeUntil: new Date(Date.now() + 30 * day).toISOString() });
  const expired = await addAccount(file, { plan: 'cloud', activeUntil: new Date(Date.now() - day).toISOString() });
  const lapsed = await addAccount(file, { plan: 'none' });
  const staff = await addAccount(file, { plan: 'selfhost', maxParticipants: 6 });
  const revoked = await addAccount(file, { plan: 'cloud', activeUntil: new Date(Date.now() + day).toISOString(), revoked: true });
  const { relay, base } = await start({ mode: 'cloud', accountsFile: file });
  try {
    assert.deepEqual((await api(base, 'GET', '/v1/health')).body, { ok: true, protocol: 1, mode: 'cloud', billing: false, signIn: false });
    const body = { protocol: 1, guestTicketSha256: ticket().hash };
    assert.equal((await api(base, 'POST', '/v1/sessions', { body })).status, 401);
    assert.equal((await api(base, 'POST', '/v1/sessions', { body, token: 'x'.repeat(43) })).status, 401);
    assert.equal((await api(base, 'POST', '/v1/sessions', { body, token: revoked.token })).status, 401);
    for (const account of [expired, lapsed]) {
      const denied = await api(base, 'POST', '/v1/sessions', { body, token: account.token });
      assert.equal(denied.status, 402);
      assert.equal(denied.body.error.code, 'subscription_required');
    }
    const created = await api(base, 'POST', '/v1/sessions', { body, token: active.token });
    assert.equal(created.status, 201);
    assert.match(created.body.sessionId, /^s_[A-Za-z0-9_-]+$/);
    assert.equal(created.body.maxParticipants, 4);
    assert.equal(created.body.hostKey, undefined);
    assert.equal((await api(base, 'POST', '/v1/sessions', { body: { ...body, maxParticipants: 6 }, token: staff.token })).body.maxParticipants, 6);
    assert.equal((await api(base, 'POST', '/v1/sessions', { body: { ...body, maxParticipants: 5 }, token: active.token })).status, 400);
    assert.equal((await api(base, 'POST', '/v1/sessions', { body: { ...body, protocol: 2 }, token: active.token })).status, 409);
    assert.equal((await api(base, 'POST', '/v1/sessions', { body: { ...body, guestTicketSha256: 'abc' }, token: active.token })).status, 400);
    assert.equal((await api(base, 'POST', '/v1/sessions', { body: { ...body, extra: 1 }, token: active.token })).status, 400);
    assert.equal((await api(base, 'POST', '/v1/sessions', { body: '{bad', token: active.token })).status, 400);
    assert.equal((await api(base, 'POST', '/v1/sessions', { body, token: active.token })).status, 201);
    assert.equal((await api(base, 'POST', '/v1/sessions', { body, token: active.token })).status, 201);
    assert.equal((await api(base, 'POST', '/v1/sessions', { body, token: active.token })).body.error.code, 'too_many_sessions');
    assert.equal((await api(base, 'DELETE', `/v1/sessions/${created.body.sessionId}`, { token: staff.token })).status, 404);
    assert.equal((await api(base, 'DELETE', `/v1/sessions/${created.body.sessionId}`, { token: active.token })).status, 204);
    assert.equal((await api(base, 'POST', '/v1/sessions', { body, token: active.token })).status, 201);
  } finally { await relay.close(); }
});

test('relay: host and guests exchange opaque signals; kick, replacement, capacity and delete use their close codes', async () => {
  const file = await tempStore();
  const host = await addAccount(file, { plan: 'cloud', activeUntil: new Date(Date.now() + day).toISOString() });
  const other = await addAccount(file, { plan: 'selfhost' });
  const turn = { secret: 'turn-secret', urls: ['turn:turn.example:3478'], ttlSeconds: 600 };
  const { relay, base } = await start({ mode: 'cloud', accountsFile: file, turn });
  try {
    const guestTicket = ticket();
    const { sessionId } = (await api(base, 'POST', '/v1/sessions', { token: host.token,
      body: { protocol: 1, guestTicketSha256: guestTicket.hash, maxParticipants: 3 } })).body;

    const h = await join(base, sessionId, { role: 'host', token: host.token, name: 'Host\u0007 Studio' });
    const welcome = await h.next();
    assert.equal(welcome.type, 'welcome'); assert.equal(welcome.role, 'host'); assert.deepEqual(welcome.peers, []);
    const [expiry, user] = welcome.turn.username.split(':');
    assert.equal(user, `${sessionId}.${welcome.peerId}`);
    assert.ok(Math.abs(Number(expiry) - (Date.now() / 1000 + 600)) < 5);
    assert.equal(welcome.turn.credential, createHmac('sha1', turn.secret).update(welcome.turn.username).digest('base64'));

    const g = await join(base, sessionId, { role: 'guest', ticket: guestTicket.value, name: 'Ana' });
    const guestWelcome = await g.next();
    assert.deepEqual(guestWelcome.peers, [{ peerId: welcome.peerId, role: 'host', name: 'Host Studio' }]);
    assert.deepEqual(await h.next(), { type: 'peer-joined', peer: { peerId: guestWelcome.peerId, role: 'guest', name: 'Ana' } });

    const offer = { kind: 'offer', sdp: 'v=0...', mac: 'peer-verified' };
    g.send({ type: 'signal', to: welcome.peerId, data: offer });
    assert.deepEqual(await h.next(), { type: 'signal', from: guestWelcome.peerId, data: offer });
    h.send({ type: 'signal', to: guestWelcome.peerId, data: { kind: 'answer' } });
    assert.deepEqual(await g.next(), { type: 'signal', from: welcome.peerId, data: { kind: 'answer' } });
    g.send({ type: 'signal', to: 'p999', data: {} });
    assert.equal((await g.next()).code, 'unknown_peer');
    g.send({ type: 'ping' });
    assert.deepEqual(await g.next(), { type: 'pong' });
    g.send({ type: 'kick', peer: welcome.peerId });
    assert.equal((await g.next()).code, 'forbidden');

    const wrong = await join(base, sessionId, { role: 'guest', ticket: 'nope' });
    assert.deepEqual(await wrong.next(), { type: 'closing', code: 4401, reason: 'Authentication failed' }); // for clients that hide close codes
    assert.equal((await wrong.closed()).code, 4401);
    const intruder = await join(base, sessionId, { role: 'host', token: other.token });
    assert.equal((await intruder.closed()).code, 4401);
    const unknown = await join(base, 's_doesnotexist', { role: 'guest', ticket: guestTicket.value });
    assert.equal((await unknown.closed()).code, 4401);

    const g2 = await join(base, sessionId, { role: 'guest', ticket: guestTicket.value });
    const second = await g2.next();
    await h.next(); await g.next(); // peer-joined
    const full = await join(base, sessionId, { role: 'guest', ticket: guestTicket.value });
    assert.equal((await full.closed()).code, 4002);

    h.send({ type: 'kick', peer: second.peerId });
    assert.equal((await g2.closed()).code, 4003);
    assert.deepEqual(await h.next(), { type: 'peer-left', peerId: second.peerId });
    assert.deepEqual(await g.next(), { type: 'peer-left', peerId: second.peerId });

    const h2 = await join(base, sessionId, { role: 'host', token: host.token });
    assert.equal((await h.closed()).code, 4004);
    const replaced = await h2.next();
    assert.deepEqual(replaced.peers.map(peer => peer.role), ['guest']);
    assert.deepEqual(await g.next(), { type: 'peer-left', peerId: welcome.peerId });
    assert.equal((await g.next()).type, 'peer-joined');

    assert.equal((await api(base, 'DELETE', `/v1/sessions/${sessionId}`, { token: host.token })).status, 204);
    assert.equal((await g.closed()).code, 4001);
    assert.equal((await h2.closed()).code, 4001);
    assert.equal(relay.sessions.size, 0);
  } finally { await relay.close(); }
});

test('relay: protocol violations, oversized frames, slow hello and floods close the socket', async () => {
  const file = await tempStore();
  const host = await addAccount(file, { plan: 'selfhost' });
  const { relay, base } = await start({ mode: 'selfhost', accountsFile: file, helloTimeoutMs: 150, frameRate: 5, frameBurst: 10, maxFrameBytes: 4096 });
  try {
    const guestTicket = ticket();
    const { sessionId } = (await api(base, 'POST', '/v1/sessions', { token: host.token, body: { protocol: 1, guestTicketSha256: guestTicket.hash } })).body;
    const silent = socket(base, sessionId);
    await silent.opened;
    assert.equal((await silent.closed()).code, 4000);
    const bad = socket(base, sessionId);
    await bad.opened; bad.send('{not json');
    assert.equal((await bad.closed()).code, 4000);
    const oldProtocol = socket(base, sessionId);
    await oldProtocol.opened; oldProtocol.send({ type: 'hello', protocol: 2, role: 'guest', ticket: guestTicket.value });
    assert.equal((await oldProtocol.closed()).code, 4000);

    const big = await join(base, sessionId, { role: 'guest', ticket: guestTicket.value });
    await big.next();
    big.send({ type: 'signal', to: 'p1', data: { blob: 'x'.repeat(8192) } });
    assert.equal((await big.closed()).code, 1009);

    const flood = await join(base, sessionId, { role: 'guest', ticket: guestTicket.value });
    await flood.next();
    for (let i = 0; i < 30; ++i) flood.send({ type: 'ping' });
    assert.equal((await flood.closed()).code, 4000);

    const odd = await join(base, sessionId, { role: 'guest', ticket: guestTicket.value });
    await odd.next();
    odd.send({ type: 'teleport' });
    assert.equal((await odd.closed()).code, 4000);
  } finally { await relay.close(); }
});

test('relay: a lapsed subscription blocks host reconnects; the host grace period ends abandoned sessions', async () => {
  const file = await tempStore();
  const host = await addAccount(file, { plan: 'cloud', activeUntil: new Date(Date.now() + day).toISOString() });
  const { relay, base } = await start({ mode: 'cloud', accountsFile: file, hostGraceMs: 300 });
  try {
    const guestTicket = ticket();
    const { sessionId } = (await api(base, 'POST', '/v1/sessions', { token: host.token, body: { protocol: 1, guestTicketSha256: guestTicket.hash } })).body;
    const h = await join(base, sessionId, { role: 'host', token: host.token });
    await h.next();
    const g = await join(base, sessionId, { role: 'guest', ticket: guestTicket.value });
    await g.next(); await h.next();

    // Billing marks the plan inactive (any writer of the store; here the admin CLI).
    await run(process.execPath, [admin, 'set', '--store', file, '--id', host.id, '--plan', 'none']);
    h.close();
    assert.equal((await g.next()).type, 'peer-left');
    const back = await join(base, sessionId, { role: 'host', token: host.token });
    assert.equal((await back.closed()).code, 4402);
    assert.equal((await g.closed(2000)).code, 4001); // nobody hosts it within the grace period
    assert.equal(relay.sessions.size, 0);
  } finally { await relay.close(); }
});

test('relay: repeated authentication failures from one address are throttled, even with a valid token afterwards', async () => {
  const file = await tempStore();
  const host = await addAccount(file, { plan: 'selfhost' });
  const { relay, base } = await start({ mode: 'selfhost', accountsFile: file, authFailuresPerMinute: 3 });
  try {
    const body = { protocol: 1, guestTicketSha256: ticket().hash };
    for (let i = 0; i < 3; ++i) assert.equal((await api(base, 'POST', '/v1/sessions', { body, token: `guess-${i}-${'y'.repeat(30)}` })).status, 401);
    assert.equal((await api(base, 'POST', '/v1/sessions', { body, token: host.token })).status, 429);
  } finally { await relay.close(); }
});

test('relay: open mode needs no account and protects the session with a returned host key', async () => {
  const { relay, base } = await start({ mode: 'open' });
  try {
    const guestTicket = ticket();
    const created = await api(base, 'POST', '/v1/sessions', { body: { protocol: 1, guestTicketSha256: guestTicket.hash } });
    assert.equal(created.status, 201);
    assert.equal(typeof created.body.hostKey, 'string');
    const fake = await join(base, created.body.sessionId, { role: 'host', token: 'not-the-key' });
    assert.equal((await fake.closed()).code, 4401);
    const h = await join(base, created.body.sessionId, { role: 'host', token: created.body.hostKey });
    assert.equal((await h.next()).role, 'host');
    assert.equal((await api(base, 'DELETE', `/v1/sessions/${created.body.sessionId}`, { token: 'wrong' })).status, 404);
    assert.equal((await api(base, 'DELETE', `/v1/sessions/${created.body.sessionId}`, { token: created.body.hostKey })).status, 204);
    assert.equal((await h.closed()).code, 4001);
  } finally { await relay.close(); }
});

test('relay admin: tokens are shown once, stored hashed and owner-only, and rotate/revoke take effect immediately', async () => {
  const file = await tempStore();
  const added = JSON.parse((await run(process.execPath, [admin, 'add', '--store', file, '--name', 'Studio', '--plan', 'cloud', '--days', '30'])).stdout);
  assert.match(added.id, /^a_/);
  const raw = await fs.readFile(file, 'utf8');
  assert.ok(!raw.includes(added.token));
  assert.ok(raw.includes(sha256(added.token)));
  assert.equal((await fs.stat(file)).mode & 0o777, 0o600);
  await run(process.execPath, [admin, 'set', '--store', file, '--id', added.id, '--email', ' Owner@Studio.TEST ']);
  const listed = (await run(process.execPath, [admin, 'list', '--store', file])).stdout;
  assert.ok(!listed.includes('tokenSha256') && listed.includes('"plan":"cloud"') && listed.includes('"email":"owner@studio.test"'));
  await assert.rejects(run(process.execPath, [admin, 'add', '--store', file, '--plan', 'selfhost', '--email', 'owner@studio.test']), /already uses/);
  await assert.rejects(run(process.execPath, [admin, 'add', '--store', file, '--plan', 'selfhost', '--email', 'nope']), /valid address/);
  await assert.rejects(run(process.execPath, [admin, 'add', '--store', file, '--plan', 'cloud']), /days or --until/);
  await assert.rejects(fs.stat(`${file}.lock`), { code: 'ENOENT' }); // a failed command releases the writer lock

  const { relay, base } = await start({ mode: 'cloud', accountsFile: file });
  try {
    const body = { protocol: 1, guestTicketSha256: ticket().hash };
    assert.equal((await api(base, 'POST', '/v1/sessions', { body, token: added.token })).status, 201);
    const rotated = JSON.parse((await run(process.execPath, [admin, 'rotate', '--store', file, '--id', added.id])).stdout);
    assert.equal((await api(base, 'POST', '/v1/sessions', { body, token: added.token })).status, 401);
    assert.equal((await api(base, 'POST', '/v1/sessions', { body, token: rotated.token })).status, 201);
    await run(process.execPath, [admin, 'revoke', '--store', file, '--id', added.id]);
    assert.equal((await api(base, 'POST', '/v1/sessions', { body, token: rotated.token })).status, 401);
  } finally { await relay.close(); }
});

test('relay config: environment parsing never needs secrets on the command line and validates TURN settings', () => {
  assert.deepEqual(configFromEnv({ SONORA_RELAY_MODE: 'open' }), { stripe: null, mail: null, publicUrl: null, mode: 'open', accountsFile: null, trustProxy: false, turn: null });
  const config = configFromEnv({ SONORA_RELAY_ACCOUNTS: '/srv/a.json', SONORA_TURN_SECRET: 's', SONORA_TURN_URLS: 'turn:a:3478, turns:a:5349' });
  assert.deepEqual(config.turn, { secret: 's', urls: ['turn:a:3478', 'turns:a:5349'], ttlSeconds: 3600 });
  assert.throws(() => configFromEnv({ SONORA_TURN_SECRET: 's' }), /SONORA_TURN_URLS/);
  assert.throws(() => createRelay({ mode: 'cloud' }), /accounts file/);
  assert.throws(() => createRelay({ mode: 'free-for-all' }), /Unknown relay mode/);
});
