// COL-10: email magic-link sign-in for hosts (docs/collab/PROTOCOL.md "Sign-in").
//
// The app starts a login and shows a 6-digit code; the email carries only a link. The link's page asks for the code
// and a button press, so (1) someone who starts a login for another person's address can't use that person's click,
// because the victim never sees the attacker's code, and (2) mail scanners that prefetch links consume nothing.
// The app polls with a secret only it holds and receives a per-device token exactly once. Pending logins live in
// memory only (a restart means "start again").
import { randomInt, timingSafeEqual } from 'node:crypto';
import { addDevice, newId, newSecret, normalizeEmail, sameHash, sha256 } from './accounts.mjs';
import { MailError } from './mail.mjs';

export class AuthError extends Error {
  constructor(status, code, message) { super(message); this.status = status; this.code = code; }
}

const escape = text => String(text).replace(/[&<>"']/g, ch => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' })[ch]);

function page(title, body) {
  return `<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>${escape(title)}</title><style>
:root{color-scheme:light dark}body{font:16px/1.5 -apple-system,system-ui,sans-serif;max-width:28rem;margin:4rem auto;padding:0 1rem}
input{font:inherit;font-size:1.5rem;letter-spacing:.3em;width:8ch;padding:.3rem .5rem}button{font:inherit;padding:.5rem 1rem;margin-top:1rem}
.error{color:#c62828}
</style></head><body><h1>${escape(title)}</h1>${body}</body></html>`;
}

export function createAuth({ config, store, enqueueWrite, now, mailer }) {
  const enabled = !!(config.publicUrl && mailer && config.mode !== 'open');
  const ttlMs = config.loginTtlMs ?? 15 * 60 * 1000;
  const maxCodeAttempts = 5;
  const logins = new Map(); // loginId -> pending login
  const byLink = new Map(); // sha256(link token) -> loginId
  const hits = new Map();   // "email:<addr>" / "ip:<addr>" -> timestamps within the last hour

  const forget = (loginId) => {
    const login = logins.get(loginId);
    if (login) byLink.delete(login.linkSha256);
    logins.delete(loginId);
  };
  const sweep = setInterval(() => {
    const time = now();
    for (const [loginId, login] of logins) if (login.expiresAt <= time) forget(loginId);
    for (const [key, times] of hits) {
      const recent = times.filter(t => time - t < 3600000);
      if (recent.length) hits.set(key, recent); else hits.delete(key);
    }
  }, 60000);
  sweep.unref();

  function limited(key, perHour) {
    const time = now();
    const recent = (hits.get(key) ?? []).filter(t => time - t < 3600000);
    if (recent.length >= perHour) { hits.set(key, recent); return true; }
    recent.push(time);
    hits.set(key, recent);
    return false;
  }

  function requireEnabled() {
    if (!enabled) throw new AuthError(404, 'sign_in_disabled', 'Email sign-in is not configured on this relay');
  }

  function live(loginId) {
    const login = typeof loginId === 'string' ? logins.get(loginId) : undefined;
    if (!login) return null;
    if (login.expiresAt <= now()) { forget(loginId); return null; }
    return login;
  }

  function loginForLink(token) {
    const loginId = typeof token === 'string' && token.length <= 256 ? byLink.get(sha256(token)) : undefined;
    const login = live(loginId);
    return login && !login.approved ? login : null;
  }

  async function start(body, ip) {
    requireEnabled();
    if (!body || typeof body !== 'object' || Array.isArray(body)) throw new AuthError(400, 'invalid_request', 'Body must be an object');
    for (const key of Object.keys(body)) if (!['email', 'deviceName'].includes(key)) throw new AuthError(400, 'invalid_request', `Unknown field: ${key}`);
    const email = normalizeEmail(body.email);
    if (!email) throw new AuthError(400, 'invalid_email', 'Enter a valid email address');
    const device = typeof body.deviceName === 'string' ? body.deviceName.replace(/[\u0000-\u001f\u007f]/g, '').trim().slice(0, 64) : '';
    if (limited(`ip:${ip}`, config.signInsPerIpPerHour ?? 20) || limited(`email:${email}`, config.signInsPerEmailPerHour ?? 5))
      throw new AuthError(429, 'too_many_attempts', 'Too many sign-in emails; try again later');

    const loginId = newId('l'), pollSecret = newSecret(), link = newSecret();
    const code = String(randomInt(0, 1000000)).padStart(6, '0');
    const existing = store.findByEmail(email);
    // Self-hosted relays don't let strangers create accounts (every account may host there). The response is the
    // same either way, so it doesn't reveal which addresses have accounts.
    const deliverable = existing ? !existing.revoked : config.mode === 'cloud';
    const login = { id: loginId, email, device: device || 'Sonora', code, pollSha256: sha256(pollSecret), linkSha256: sha256(link),
                    expiresAt: now() + ttlMs, attempts: 0, lastPoll: 0, approved: null };
    logins.set(loginId, login);
    byLink.set(login.linkSha256, loginId);
    if (deliverable) {
      const url = `${config.publicUrl.replace(/\/+$/, '')}/v1/auth/verify?token=${link}`;
      const minutes = Math.round(ttlMs / 60000);
      const text = `Open this link to sign in to Sonora Cloud on "${login.device}":\n\n${url}\n\n`
        + `The page asks for the 6-digit code shown in Sonora. The link works once and expires in ${minutes} minutes.\n\n`
        + `If you didn't try to sign in, ignore this email. Nobody can sign in without the code on your screen.\n`;
      const html = `<p>Open this link to sign in to Sonora Cloud on “${escape(login.device)}”:</p><p><a href="${escape(url)}">Sign in to Sonora</a></p>`
        + `<p>The page asks for the 6-digit code shown in Sonora. The link works once and expires in ${minutes} minutes.</p>`
        + `<p>If you didn't try to sign in, ignore this email. Nobody can sign in without the code on your screen.</p>`;
      try { await mailer.send({ to: email, subject: 'Sign in to Sonora', text, html }); }
      catch (error) {
        forget(loginId);
        if (error instanceof MailError) throw new AuthError(502, 'mail_unavailable', 'The sign-in email could not be sent; try again later');
        throw error;
      }
    }
    return { loginId, pollSecret, code, expiresAt: new Date(login.expiresAt).toISOString(), pollIntervalSeconds: 2 };
  }

  function verifyPage(token) {
    if (!enabled) return { status: 404, html: page('Sign-in unavailable', '<p>Email sign-in is not configured on this server.</p>') };
    const login = loginForLink(token);
    if (!login) return { status: 410, html: page('Link expired', '<p>This sign-in link has expired or was already used. Start again in Sonora.</p>') };
    return { status: 200, html: form(login, token) };
  }

  function form(login, token, error = '') {
    return page('Sign in to Sonora', `<p>Signing in <strong>${escape(login.email)}</strong> on “${escape(login.device)}”.</p>
${error ? `<p class="error">${escape(error)}</p>` : ''}
<form method="post" action="verify"><input type="hidden" name="token" value="${escape(token)}">
<label>Code shown in Sonora<br><input name="code" inputmode="numeric" autocomplete="one-time-code" pattern="[0-9 ]*" maxlength="7" required autofocus></label><br>
<button type="submit">Sign in</button></form>
<p>Didn't start this? Close this page; nothing happens without the code.</p>`);
  }

  async function verifySubmit(fields) {
    if (!enabled) return { status: 404, html: page('Sign-in unavailable', '<p>Email sign-in is not configured on this server.</p>') };
    const token = fields.get('token') ?? '';
    const login = loginForLink(token);
    if (!login) return { status: 410, html: page('Link expired', '<p>This sign-in link has expired or was already used. Start again in Sonora.</p>') };
    const entered = String(fields.get('code') ?? '').replace(/\s+/g, '');
    const matches = entered.length === 6 && timingSafeEqual(Buffer.from(entered), Buffer.from(login.code));
    if (!matches) {
      login.attempts += 1;
      if (login.attempts >= maxCodeAttempts) {
        forget(login.id);
        return { status: 400, html: page('Too many wrong codes', '<p>This sign-in was cancelled. Start again in Sonora.</p>') };
      }
      return { status: 400, html: form(login, token, `That code doesn't match. ${maxCodeAttempts - login.attempts} attempts left.`) };
    }
    byLink.delete(login.linkSha256); // single use
    const created = config.mode === 'cloud';
    login.approved = await enqueueWrite(() => store.update(data => {
      let account = data.accounts.find(entry => entry.email === login.email);
      if (account?.revoked) return null;
      if (!account) {
        if (!created) return null;
        account = { id: newId('a'), email: login.email, name: '', plan: 'none', activeUntil: null, createdAt: new Date(now()).toISOString() };
        data.accounts.push(account);
      }
      const deviceToken = addDevice(account, login.device, now());
      return { accountId: account.id, email: account.email, plan: account.plan, token: deviceToken };
    }));
    if (!login.approved) {
      forget(login.id);
      return { status: 403, html: page('Sign-in not possible', '<p>This account can\'t sign in here. Contact the server\'s administrator.</p>') };
    }
    return { status: 200, html: page('Signed in', `<p>You're signed in on “${escape(login.device)}”. You can close this page and return to Sonora.</p>`) };
  }

  function poll(body) {
    requireEnabled();
    const login = live(body?.loginId);
    if (!login || !sameHash(login.pollSha256, sha256(String(body?.pollSecret ?? ''))))
      throw new AuthError(410, 'expired', 'This sign-in expired or was cancelled; start again');
    const time = now();
    if (time - login.lastPoll < 1000) throw new AuthError(429, 'slow_down', 'Poll at most once per second');
    login.lastPoll = time;
    if (!login.approved) return { status: 202, body: { status: 'pending' } };
    logins.delete(body.loginId); // the token is handed out exactly once
    return { status: 200, body: { status: 'approved', ...login.approved } };
  }

  async function logout(account, token) {
    const hash = sha256(token);
    if (!(account.devices ?? []).some(device => sameHash(device.tokenSha256, hash)))
      throw new AuthError(409, 'admin_token', 'This is an administrator-issued token; ask the relay admin to rotate it');
    await enqueueWrite(() => store.update(data => {
      const entry = data.accounts.find(candidate => candidate.id === account.id);
      if (entry) entry.devices = (entry.devices ?? []).filter(device => !sameHash(device.tokenSha256, hash));
    }));
  }

  function describe(account) {
    return { accountId: account.id, email: account.email ?? null, plan: account.plan, activeUntil: account.activeUntil ?? null,
             subscription: account.stripeStatus ?? null,
             devices: (account.devices ?? []).map(({ id, name, createdAt }) => ({ id, name, createdAt })) };
  }

  return { enabled, start, verifyPage, verifySubmit, poll, logout, describe, close: () => clearInterval(sweep) };
}
