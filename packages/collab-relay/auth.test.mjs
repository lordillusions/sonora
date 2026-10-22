// COL-10: email magic-link sign-in. Mail goes to an outbox folder or a fake Resend API; the clock is injected.
import test from 'node:test';
import assert from 'node:assert/strict';
import http from 'node:http';
import { promises as fs } from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { AccountStore } from './accounts.mjs';
import { configFromEnv } from './relay.mjs';
import { addAccount, api, start, tempStore, ticket } from './test-helpers.mjs';

const publicUrl = 'http://localhost:4100';

async function setup(overrides = {}) {
  const outbox = await fs.mkdtemp(path.join(os.tmpdir(), 'sonora-outbox-'));
  const file = await tempStore();
  const clock = { t: Date.parse('2026-10-17T12:00:00Z') };
  const { relay, base } = await start({ mode: 'cloud', accountsFile: file, publicUrl, now: () => clock.t,
    mail: { provider: 'outbox', from: 'Sonora <no-reply@sonora.test>', outbox }, ...overrides });
  return { relay, base, file, outbox, clock, advance: ms => { clock.t += ms; } };
}

async function mails(outbox) {
  const files = (await fs.readdir(outbox).catch(() => [])).sort();
  return Promise.all(files.map(async name => JSON.parse(await fs.readFile(path.join(outbox, name), 'utf8'))));
}
const linkToken = mail => /\/v1\/auth\/verify\?token=([A-Za-z0-9_-]+)/.exec(mail.text)[1];

async function html(base, method, route, form) {
  const response = await fetch(base + route, {
    method, ...(form ? { body: new URLSearchParams(form), headers: { 'content-type': 'application/x-www-form-urlencoded' } } : {}),
  });
  return { status: response.status, text: await response.text(), headers: response.headers };
}

// Starts a login, confirms it through the email link and returns the approved poll result.
async function signIn(ctx, email, deviceName) {
  const started = (await api(ctx.base, 'POST', '/v1/auth/start', { body: { email, deviceName } })).body;
  const mail = (await mails(ctx.outbox)).at(-1);
  const confirmed = await html(ctx.base, 'POST', '/v1/auth/verify', { token: linkToken(mail), code: started.code });
  assert.equal(confirmed.status, 200);
  ctx.advance(1000);
  return (await api(ctx.base, 'POST', '/v1/auth/poll', { body: { loginId: started.loginId, pollSecret: started.pollSecret } })).body;
}

test('sign-in: a new email confirms with the on-screen code, gets a device token once, and can sign in more devices', async () => {
  const ctx = await setup();
  const { base, outbox, file } = ctx;
  try {
    assert.equal((await api(base, 'GET', '/v1/health')).body.signIn, true);
    const started = await api(base, 'POST', '/v1/auth/start', { body: { email: '  Ana@Example.COM ', deviceName: 'MacBook <Pro>' } });
    assert.equal(started.status, 202);
    assert.match(started.body.code, /^\d{6}$/);
    assert.equal(started.body.pollIntervalSeconds, 2);
    const [mail] = await mails(outbox);
    assert.equal(mail.to, 'ana@example.com');
    assert.equal(mail.subject, 'Sign in to Sonora');
    assert.ok(mail.text.startsWith('Open this link') && mail.text.includes(`${publicUrl}/v1/auth/verify?token=`));
    assert.ok(!mail.text.includes(started.body.code) && !mail.html.includes(started.body.code), 'the code is only on the screen');
    assert.ok(mail.html.includes('MacBook &lt;Pro&gt;'));

    const poll = () => api(base, 'POST', '/v1/auth/poll', { body: { loginId: started.body.loginId, pollSecret: started.body.pollSecret } });
    assert.deepEqual((await poll()).body, { status: 'pending' });
    assert.equal((await poll()).body.error.code, 'slow_down');
    ctx.advance(1000);

    const token = linkToken(mail);
    const view = await html(base, 'GET', `/v1/auth/verify?token=${token}`);
    assert.equal(view.status, 200);
    assert.ok(view.text.includes('ana@example.com') && view.text.includes('MacBook &lt;Pro&gt;') && !view.text.includes(started.body.code));
    assert.match(view.headers.get('content-security-policy'), /default-src 'none'/);
    assert.equal(view.headers.get('referrer-policy'), 'no-referrer');
    assert.equal((await html(base, 'GET', `/v1/auth/verify?token=${token}`)).status, 200, 'link scanners consume nothing');

    const wrong = await html(base, 'POST', '/v1/auth/verify', { token, code: '000000' === started.body.code ? '111111' : '000000' });
    assert.equal(wrong.status, 400);
    assert.ok(wrong.text.includes('4 attempts left'));
    const spaced = `${started.body.code.slice(0, 3)} ${started.body.code.slice(3)}`;
    const ok = await html(base, 'POST', '/v1/auth/verify', { token, code: spaced });
    assert.equal(ok.status, 200);
    assert.ok(ok.text.includes('signed in'));
    assert.equal((await html(base, 'POST', '/v1/auth/verify', { token, code: started.body.code })).status, 410, 'the link works once');

    const approved = (await poll()).body;
    assert.equal(approved.status, 'approved');
    assert.equal(approved.email, 'ana@example.com');
    assert.equal(approved.plan, 'none');
    ctx.advance(1000);
    assert.equal((await poll()).status, 410, 'the token is handed out exactly once');

    const account = (await api(base, 'GET', '/v1/account', { token: approved.token })).body;
    assert.equal(account.accountId, approved.accountId);
    assert.deepEqual(account.devices.map(device => device.name), ['MacBook <Pro>']);
    assert.equal((await api(base, 'POST', '/v1/sessions', { token: approved.token, body: { protocol: 1, guestTicketSha256: ticket().hash } })).status, 402);

    const second = await signIn(ctx, 'ana@example.com', 'Studio iMac');
    assert.equal(second.accountId, approved.accountId);
    assert.equal((await api(base, 'GET', '/v1/account', { token: second.token })).body.devices.length, 2);
    assert.equal((await api(base, 'GET', '/v1/account', { token: approved.token })).status, 200, 'other devices stay signed in');

    assert.equal((await api(base, 'POST', '/v1/auth/logout', { token: approved.token })).status, 204);
    assert.equal((await api(base, 'GET', '/v1/account', { token: approved.token })).status, 401);
    assert.equal((await api(base, 'GET', '/v1/account', { token: second.token })).status, 200);
    const raw = await fs.readFile(file, 'utf8');
    assert.ok(!raw.includes(approved.token) && !raw.includes(second.token) && !raw.includes(started.body.code));
  } finally { await ctx.relay.close(); }
});

test('sign-in: wrong codes, expiry, foreign poll secrets, rate limits and HTML injection are contained', async () => {
  const ctx = await setup({ signInsPerEmailPerHour: 5 });
  const { base, outbox } = ctx;
  try {
    assert.equal((await api(base, 'POST', '/v1/auth/start', { body: { email: 'not-an-email' } })).body.error.code, 'invalid_email');
    assert.equal((await api(base, 'POST', '/v1/auth/start', { body: { email: 'a@b.co', extra: 1 } })).status, 400);

    const started = (await api(base, 'POST', '/v1/auth/start', { body: { email: 'bo@example.com', deviceName: '<script>x</script>' } })).body;
    const token = linkToken((await mails(outbox)).at(-1));
    assert.ok((await html(base, 'GET', `/v1/auth/verify?token=${token}`)).text.includes('&lt;script&gt;x&lt;/script&gt;'));
    const wrongCode = started.code === '999999' ? '888888' : '999999';
    for (let i = 0; i < 4; ++i) assert.equal((await html(base, 'POST', '/v1/auth/verify', { token, code: wrongCode })).status, 400);
    assert.ok((await html(base, 'POST', '/v1/auth/verify', { token, code: wrongCode })).text.includes('Too many wrong codes'));
    assert.equal((await html(base, 'POST', '/v1/auth/verify', { token, code: started.code })).status, 410);
    assert.equal((await api(base, 'POST', '/v1/auth/poll', { body: { loginId: started.loginId, pollSecret: started.pollSecret } })).status, 410);

    const other = (await api(base, 'POST', '/v1/auth/start', { body: { email: 'bo@example.com' } })).body;
    assert.equal((await api(base, 'POST', '/v1/auth/poll', { body: { loginId: other.loginId, pollSecret: 'someone-else' } })).status, 410);
    const otherToken = linkToken((await mails(outbox)).at(-1));
    ctx.advance(16 * 60 * 1000);
    assert.equal((await html(base, 'GET', `/v1/auth/verify?token=${otherToken}`)).status, 410);
    assert.equal((await api(base, 'POST', '/v1/auth/poll', { body: { loginId: other.loginId, pollSecret: other.pollSecret } })).status, 410);
    assert.equal((await html(base, 'GET', '/v1/auth/verify?token=made-up')).status, 410);

    for (let i = 0; i < 3; ++i) assert.equal((await api(base, 'POST', '/v1/auth/start', { body: { email: 'bo@example.com' } })).status, 202);
    assert.equal((await api(base, 'POST', '/v1/auth/start', { body: { email: 'BO@example.com' } })).body.error.code, 'too_many_attempts');
    assert.equal((await api(base, 'POST', '/v1/auth/start', { body: { email: 'cy@example.com' } })).status, 202, 'limits are per address');
  } finally { await ctx.relay.close(); }
});

test('sign-in: self-hosted relays only admit emails the admin added; revoked and admin tokens behave safely', async () => {
  const outbox = await fs.mkdtemp(path.join(os.tmpdir(), 'sonora-outbox-'));
  const file = await tempStore();
  const member = await addAccount(file, { plan: 'selfhost', email: 'member@studio.test' });
  await addAccount(file, { plan: 'selfhost', email: 'gone@studio.test', revoked: true });
  const ctx = { outbox, clock: { t: Date.now() } };
  ctx.advance = ms => { ctx.clock.t += ms; };
  Object.assign(ctx, await start({ mode: 'selfhost', accountsFile: file, publicUrl, now: () => ctx.clock.t,
    mail: { provider: 'outbox', from: 'relay@studio.test', outbox } }));
  try {
    const stranger = await api(ctx.base, 'POST', '/v1/auth/start', { body: { email: 'stranger@example.com' } });
    assert.equal(stranger.status, 202, 'same answer, so membership is not revealed');
    assert.equal((await api(ctx.base, 'POST', '/v1/auth/start', { body: { email: 'gone@studio.test' } })).status, 202);
    assert.equal((await mails(outbox)).length, 0);

    const approved = await signIn(ctx, 'Member@Studio.test', 'Desk');
    assert.equal(approved.accountId, member.id);
    assert.equal(approved.plan, 'selfhost');
    assert.equal((await api(ctx.base, 'POST', '/v1/sessions', { token: approved.token, body: { protocol: 1, guestTicketSha256: ticket().hash } })).status, 201);
    assert.equal((await api(ctx.base, 'POST', '/v1/auth/logout', { token: member.token })).body.error.code, 'admin_token');
    assert.equal(new AccountStore(file).findById(member.id).devices.length, 1);
  } finally { await ctx.relay.close(); }
});

test('sign-in: Resend delivery, provider failure and disabled sign-in', async () => {
  const received = [];
  let fail = false;
  const resend = http.createServer(async (request, response) => {
    let body = '';
    for await (const chunk of request) body += chunk;
    received.push({ path: request.url, auth: request.headers.authorization, body: JSON.parse(body) });
    response.writeHead(fail ? 500 : 200, { 'content-type': 'application/json' });
    response.end(fail ? '{}' : '{"id":"email_1"}');
  });
  await new Promise(resolve => resend.listen(0, '127.0.0.1', resolve));
  const file = await tempStore();
  const cloud = await start({ mode: 'cloud', accountsFile: file, publicUrl: 'https://relay.sonora.test',
    mail: { provider: 'resend', from: 'Sonora <login@sonora.test>', apiKey: 're_test', apiBase: `http://127.0.0.1:${resend.address().port}` } });
  const open = await start({ mode: 'open', publicUrl, mail: { provider: 'outbox', from: 'x@y.z', outbox: os.tmpdir() } });
  try {
    assert.equal((await api(cloud.base, 'POST', '/v1/auth/start', { body: { email: 'dee@example.com' } })).status, 202);
    assert.equal(received[0].path, '/emails');
    assert.equal(received[0].auth, 'Bearer re_test');
    assert.deepEqual(received[0].body.to, ['dee@example.com']);
    assert.equal(received[0].body.from, 'Sonora <login@sonora.test>');
    assert.ok(received[0].body.text.includes('https://relay.sonora.test/v1/auth/verify?token='));

    fail = true;
    const failed = await api(cloud.base, 'POST', '/v1/auth/start', { body: { email: 'eve@example.com' } });
    assert.equal(failed.status, 502);
    assert.equal(failed.body.error.code, 'mail_unavailable');

    assert.equal((await api(open.base, 'GET', '/v1/health')).body.signIn, false);
    assert.equal((await api(open.base, 'POST', '/v1/auth/start', { body: { email: 'a@b.co' } })).body.error.code, 'sign_in_disabled');
    assert.equal((await html(open.base, 'GET', '/v1/auth/verify?token=x')).status, 404);
  } finally { await cloud.relay.close(); await open.relay.close(); await new Promise(resolve => resend.close(resolve)); }
});

test('sign-in config: mail provider settings and public URL are validated', () => {
  const base = { SONORA_RELAY_ACCOUNTS: '/srv/a.json' };
  assert.deepEqual(configFromEnv({ ...base, SONORA_MAIL_PROVIDER: 'resend', SONORA_MAIL_FROM: 'a@b.co', RESEND_API_KEY: 're_x',
                                   SONORA_RELAY_PUBLIC_URL: 'https://relay.example' }).mail,
                   { provider: 'resend', from: 'a@b.co', apiKey: 're_x' });
  assert.throws(() => configFromEnv({ ...base, SONORA_MAIL_PROVIDER: 'resend', SONORA_MAIL_FROM: 'a@b.co' }), /RESEND_API_KEY/);
  assert.throws(() => configFromEnv({ ...base, SONORA_MAIL_PROVIDER: 'outbox', SONORA_MAIL_FROM: 'a@b.co' }), /SONORA_MAIL_OUTBOX/);
  assert.throws(() => configFromEnv({ ...base, SONORA_MAIL_PROVIDER: 'resend', RESEND_API_KEY: 're_x' }), /SONORA_MAIL_FROM/);
  assert.throws(() => configFromEnv({ ...base, SONORA_MAIL_PROVIDER: 'pigeon', SONORA_MAIL_FROM: 'a@b.co' }), /Unknown/);
  assert.throws(() => configFromEnv({ ...base, SONORA_RELAY_PUBLIC_URL: 'http://relay.example' }), /https/);
  assert.equal(configFromEnv({ ...base, SONORA_RELAY_PUBLIC_URL: 'http://localhost:8787' }).publicUrl, 'http://localhost:8787');
});
