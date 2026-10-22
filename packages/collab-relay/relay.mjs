// COL-3 / COL-10: Sonora collab relay (docs/collab/PROTOCOL.md). Introduces the peers of one session, forwards opaque
// signalling envelopes and issues short-lived TURN credentials. It never sees project data, media or the invite
// secret. Hosting a session requires an entitled account (checked here, server side); guests need only a ticket.
import http from 'node:http';
import { createHmac, randomBytes } from 'node:crypto';
import { pathToFileURL } from 'node:url';
import { WebSocketServer } from 'ws';
import { AccountStore, entitlement, newId, sameHash, sha256 } from './accounts.mjs';
import { AuthError, createAuth } from './auth.mjs';
import { BillingError, createBilling } from './billing.mjs';
import { createMailer } from './mail.mjs';

export const protocolVersion = 1;
const modes = ['cloud', 'selfhost', 'open'];
const defaults = {
  mode: 'cloud',
  accountsFile: null,
  sessionTtlMs: 12 * 60 * 60 * 1000,
  hostGraceMs: 10 * 60 * 1000,
  helloTimeoutMs: 5000,
  maxFrameBytes: 64 * 1024,
  maxBodyBytes: 16 * 1024,
  maxWebhookBytes: 512 * 1024,
  frameRate: 50,
  frameBurst: 100,
  maxSessionsPerAccount: 3,
  authFailuresPerMinute: 20,
  trustProxy: false,
  turn: null, // { secret, urls: [...], ttlSeconds }
  stripe: null, // billing.mjs; only used in cloud mode
  publicUrl: null, // origin (and optional path prefix) used in sign-in links
  mail: null, // mail.mjs: { provider: 'resend' | 'outbox', from, apiKey | outbox }
  now: () => Date.now(),
};

class HttpError extends Error {
  constructor(status, code, message) { super(message); this.status = status; this.code = code; }
}

function sanitizeName(name, fallback) {
  if (typeof name !== 'string') return fallback;
  const clean = name.replace(/[\u0000-\u001f\u007f]/g, '').trim().slice(0, 64);
  return clean || fallback;
}

export function turnCredentials(turn, sessionId, peerId, now) {
  const ttlSeconds = turn.ttlSeconds ?? 3600;
  const username = `${Math.floor(now / 1000) + ttlSeconds}:${sessionId}.${peerId}`;
  const credential = createHmac('sha1', turn.secret).update(username).digest('base64');
  return { urls: turn.urls, username, credential, ttlSeconds };
}

export function createRelay(options = {}) {
  const config = { ...defaults, ...options };
  if (!modes.includes(config.mode)) throw new Error(`Unknown relay mode: ${config.mode}`);
  if (config.mode !== 'open' && !config.accountsFile) throw new Error(`Mode ${config.mode} needs an accounts file`);
  const store = new AccountStore(config.accountsFile);
  const sessions = new Map();
  const failures = new Map(); // ip -> { count, since }
  let writeChain = Promise.resolve();
  const enqueueWrite = task => {
    const result = writeChain.then(task);
    writeChain = result.catch(() => {});
    return result;
  };
  const billing = createBilling({ config: config.mode === 'cloud' ? config : { ...config, stripe: null }, store, enqueueWrite, now: config.now });
  const auth = createAuth({ config, store, enqueueWrite, now: config.now, mailer: createMailer(config.mail) });

  const clientIp = request => {
    const forwarded = config.trustProxy ? String(request.headers['x-forwarded-for'] ?? '').split(',')[0].trim() : '';
    return forwarded || request.socket.remoteAddress || 'unknown';
  };
  const lockedOut = ip => {
    const entry = failures.get(ip);
    if (!entry || config.now() - entry.since > 60000) return false;
    return entry.count >= config.authFailuresPerMinute;
  };
  const recordFailure = ip => {
    const now = config.now(), entry = failures.get(ip);
    if (!entry || now - entry.since > 60000) failures.set(ip, { count: 1, since: now });
    else entry.count += 1;
  };
  const sweep = setInterval(() => {
    const now = config.now();
    for (const [ip, entry] of failures) if (now - entry.since > 60000) failures.delete(ip);
  }, 60000);
  sweep.unref();

  const send = (ws, message) => { if (ws.readyState === ws.OPEN) ws.send(JSON.stringify(message)); };
  // WebSocket libraries in some clients (libdatachannel) hide close codes, so the reason travels as a last frame too.
  const closeWith = (ws, code, reason) => {
    send(ws, { type: 'closing', code, reason });
    ws.close(code, reason);
  };
  const broadcast = (session, message, except) => {
    for (const [peerId, peer] of session.peers) if (peerId !== except) send(peer.ws, message);
  };

  function endSession(session, code, reason) {
    if (!sessions.has(session.id)) return;
    sessions.delete(session.id);
    clearTimeout(session.expiry);
    clearTimeout(session.hostGrace);
    for (const peer of session.peers.values()) closeWith(peer.ws, code, reason);
    session.peers.clear();
  }

  // Host identity: the creating account (entitlement re-checked), or the per-session host key in open mode.
  function authorizeHost(session, token) {
    if (config.mode === 'open') return sameHash(session.hostKeySha256, sha256(String(token ?? ''))) ? { ok: true } : { ok: false, code: 'unauthorized' };
    const account = store.findByToken(token);
    if (!account || account.id !== session.accountId) return { ok: false, code: 'unauthorized' };
    return entitlement(account, config.mode, config.now());
  }

  function bearer(request) {
    const match = /^Bearer\s+(\S+)$/i.exec(String(request.headers.authorization ?? ''));
    return match ? match[1] : null;
  }

  async function readRaw(request, limit) {
    let size = 0;
    const chunks = [];
    for await (const chunk of request) {
      size += chunk.length;
      if (size > limit) throw new HttpError(413, 'invalid_request', 'Body too large');
      chunks.push(chunk);
    }
    return Buffer.concat(chunks);
  }
  async function readJson(request) {
    const raw = await readRaw(request, config.maxBodyBytes);
    try { return JSON.parse(raw.toString('utf8') || '{}'); }
    catch { throw new HttpError(400, 'invalid_request', 'Body must be JSON'); }
  }

  // Account and billing endpoints act for the account behind the bearer token, whatever its plan.
  function billingAccount(request, ip) {
    if (lockedOut(ip)) throw new HttpError(429, 'too_many_attempts', 'Too many failed authentications; retry later');
    const account = store.findByToken(bearer(request));
    if (!account || account.revoked) { recordFailure(ip); throw new HttpError(401, 'unauthorized', 'A valid host token is required'); }
    return account;
  }

  async function createSession(request, ip) {
    if (lockedOut(ip)) throw new HttpError(429, 'too_many_attempts', 'Too many failed authentications; retry later');
    const body = await readJson(request);
    if (!body || typeof body !== 'object' || Array.isArray(body)) throw new HttpError(400, 'invalid_request', 'Body must be an object');
    for (const key of Object.keys(body))
      if (!['protocol', 'guestTicketSha256', 'maxParticipants'].includes(key)) throw new HttpError(400, 'invalid_request', `Unknown field: ${key}`);
    if (body.protocol !== protocolVersion) throw new HttpError(409, 'protocol_mismatch', `This relay speaks protocol ${protocolVersion}`);
    if (typeof body.guestTicketSha256 !== 'string' || !/^[0-9a-f]{64}$/.test(body.guestTicketSha256))
      throw new HttpError(400, 'invalid_request', 'guestTicketSha256 must be 64 lowercase hex characters');

    let account = null, cap = 4;
    if (config.mode !== 'open') {
      account = store.findByToken(bearer(request));
      if (!account) { recordFailure(ip); throw new HttpError(401, 'unauthorized', 'A valid host token is required'); }
      const allowed = entitlement(account, config.mode, config.now());
      if (allowed.code === 'unauthorized') { recordFailure(ip); throw new HttpError(401, 'unauthorized', 'A valid host token is required'); }
      if (!allowed.ok) throw new HttpError(402, 'subscription_required', 'Hosting on this relay requires an active subscription');
      cap = allowed.maxParticipants;
      const live = [...sessions.values()].filter(session => session.accountId === account.id).length;
      if (live >= config.maxSessionsPerAccount) throw new HttpError(429, 'too_many_sessions', 'End a session before starting another');
    }
    const maxParticipants = body.maxParticipants ?? cap;
    if (!Number.isInteger(maxParticipants) || maxParticipants < 2 || maxParticipants > cap)
      throw new HttpError(400, 'invalid_request', `maxParticipants must be 2..${cap}`);

    const id = newId('s');
    const hostKey = config.mode === 'open' ? randomBytes(32).toString('base64url') : null;
    const session = {
      id, accountId: account?.id ?? null, maxParticipants, ticketSha256: body.guestTicketSha256,
      hostKeySha256: hostKey ? sha256(hostKey) : null, expiresAt: config.now() + config.sessionTtlMs,
      peers: new Map(), nextPeer: 0, expiry: null, hostGrace: null,
    };
    session.expiry = setTimeout(() => endSession(session, 4001, 'Session expired'), config.sessionTtlMs);
    session.expiry.unref();
    // A host that never connects behaves like one that left.
    session.hostGrace = setTimeout(() => endSession(session, 4001, 'Host did not return'), config.hostGraceMs);
    session.hostGrace.unref();
    sessions.set(id, session);
    return { status: 201, body: { sessionId: id, expiresAt: new Date(session.expiresAt).toISOString(), maxParticipants,
                                  signalPath: '/v1/signal', ...(hostKey ? { hostKey } : {}) } };
  }

  function deleteSession(request, id, ip) {
    if (lockedOut(ip)) throw new HttpError(429, 'too_many_attempts', 'Too many failed authentications; retry later');
    const session = sessions.get(id);
    const token = bearer(request);
    const owner = session && (config.mode === 'open' ? sameHash(session.hostKeySha256, sha256(String(token ?? '')))
                                                      : store.findByToken(token)?.id === session.accountId);
    if (!session || !owner) {
      recordFailure(ip);
      throw new HttpError(404, 'not_found', 'No such session for this host');
    }
    endSession(session, 4001, 'Session ended by host');
    return { status: 204 };
  }

  const server = http.createServer(async (request, response) => {
    const reply = (status, body) => {
      response.writeHead(status, body === undefined ? {} : { 'content-type': 'application/json', 'cache-control': 'no-store' });
      response.end(body === undefined ? undefined : JSON.stringify(body));
    };
    const replyHtml = ({ status, html }) => {
      response.writeHead(status, { 'content-type': 'text/html; charset=utf-8', 'cache-control': 'no-store', 'referrer-policy': 'no-referrer',
        'x-content-type-options': 'nosniff',
        'content-security-policy': "default-src 'none'; style-src 'unsafe-inline'; form-action 'self'; frame-ancestors 'none'; base-uri 'none'" });
      response.end(html);
    };
    try {
      const url = new URL(request.url, 'http://relay');
      const ip = clientIp(request);
      if (request.method === 'GET' && url.pathname === '/v1/health')
        return reply(200, { ok: true, protocol: protocolVersion, mode: config.mode, billing: billing.enabled, signIn: auth.enabled });
      if (request.method === 'POST' && url.pathname === '/v1/auth/start') {
        const started = await auth.start(await readJson(request), ip);
        return reply(202, started);
      }
      if (request.method === 'POST' && url.pathname === '/v1/auth/poll') {
        const result = auth.poll(await readJson(request));
        return reply(result.status, result.body);
      }
      if (request.method === 'GET' && url.pathname === '/v1/auth/verify') return replyHtml(auth.verifyPage(url.searchParams.get('token')));
      if (request.method === 'POST' && url.pathname === '/v1/auth/verify')
        return replyHtml(await auth.verifySubmit(new URLSearchParams((await readRaw(request, config.maxBodyBytes)).toString('utf8'))));
      if (request.method === 'POST' && url.pathname === '/v1/auth/logout') {
        const account = billingAccount(request, ip);
        await auth.logout(account, bearer(request));
        return reply(204);
      }
      if (request.method === 'GET' && url.pathname === '/v1/account') return reply(200, auth.describe(billingAccount(request, ip)));
      if (request.method === 'POST' && url.pathname === '/v1/billing/webhook') {
        const outcome = await billing.webhook(await readRaw(request, config.maxWebhookBytes), request.headers['stripe-signature']);
        return reply(200, { received: true, outcome });
      }
      if (request.method === 'POST' && url.pathname === '/v1/billing/checkout') {
        const account = billingAccount(request, ip);
        await readRaw(request, config.maxBodyBytes);
        return reply(200, await billing.checkout(account));
      }
      if (request.method === 'POST' && url.pathname === '/v1/billing/portal') {
        const account = billingAccount(request, ip);
        await readRaw(request, config.maxBodyBytes);
        return reply(200, await billing.portal(account));
      }
      if (request.method === 'POST' && url.pathname === '/v1/sessions') {
        const result = await createSession(request, ip);
        return reply(result.status, result.body);
      }
      const match = /^\/v1\/sessions\/([A-Za-z0-9_-]{1,64})$/.exec(url.pathname);
      if (request.method === 'DELETE' && match) return reply(deleteSession(request, match[1], ip).status);
      throw new HttpError(404, 'not_found', 'Unknown endpoint');
    } catch (error) {
      if (error instanceof HttpError || error instanceof BillingError || error instanceof AuthError)
        return reply(error.status, { error: { code: error.code, message: error.message } });
      return reply(500, { error: { code: 'internal', message: 'Internal error' } });
    }
  });

  const wss = new WebSocketServer({ noServer: true, maxPayload: config.maxFrameBytes });
  server.on('upgrade', (request, socket, head) => {
    const url = new URL(request.url, 'http://relay');
    if (url.pathname !== '/v1/signal' || !/^[A-Za-z0-9_-]{1,64}$/.test(url.searchParams.get('session') ?? '')) {
      socket.end('HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\n');
      return;
    }
    wss.handleUpgrade(request, socket, head, ws => connect(ws, url.searchParams.get('session'), clientIp(request)));
  });

  function connect(ws, sessionId, ip) {
    const state = { session: null, peerId: null, tokens: config.frameBurst, refilled: config.now() };
    const helloTimer = setTimeout(() => closeWith(ws, 4000, 'Expected hello'), config.helloTimeoutMs);
    const violation = reason => closeWith(ws, 4000, reason);

    const allowFrame = () => {
      const now = config.now();
      state.tokens = Math.min(config.frameBurst, state.tokens + ((now - state.refilled) / 1000) * config.frameRate);
      state.refilled = now;
      if (state.tokens < 1) return false;
      state.tokens -= 1;
      return true;
    };

    function hello(message) {
      clearTimeout(helloTimer);
      if (message.type !== 'hello') return violation('Expected hello');
      if (message.protocol !== protocolVersion) return violation('Protocol mismatch');
      const session = sessions.get(sessionId);
      if (lockedOut(ip)) return closeWith(ws, 4401, 'Too many failed authentications');
      if (!session) { recordFailure(ip); return closeWith(ws, 4401, 'Unknown session'); }
      let role;
      if (message.role === 'host') {
        const allowed = authorizeHost(session, message.token);
        if (!allowed.ok) {
          if (allowed.code === 'subscription_required') return closeWith(ws, 4402, 'Subscription required');
          recordFailure(ip);
          return closeWith(ws, 4401, 'Authentication failed');
        }
        role = 'host';
      } else if (message.role === 'guest') {
        if (typeof message.ticket !== 'string' || message.ticket.length > 256 || !sameHash(session.ticketSha256, sha256(message.ticket))) {
          recordFailure(ip);
          return closeWith(ws, 4401, 'Authentication failed');
        }
        role = 'guest';
      } else return violation('Unknown role');

      const previousHost = role === 'host' ? [...session.peers].find(([, peer]) => peer.role === 'host') : null;
      if (!previousHost && session.peers.size >= session.maxParticipants) return closeWith(ws, 4002, 'Session full');
      if (previousHost) {
        session.peers.delete(previousHost[0]);
        closeWith(previousHost[1].ws, 4004, 'Replaced by a newer host connection');
        broadcast(session, { type: 'peer-left', peerId: previousHost[0] });
      }
      const peerId = `p${++session.nextPeer}`;
      const name = sanitizeName(message.name, role === 'host' ? 'Host' : `Guest ${session.nextPeer}`);
      const peers = [...session.peers].map(([id, peer]) => ({ peerId: id, role: peer.role, name: peer.name }));
      session.peers.set(peerId, { ws, role, name });
      state.session = session;
      state.peerId = peerId;
      if (role === 'host') clearTimeout(session.hostGrace);
      send(ws, { type: 'welcome', peerId, role, peers,
                 ...(config.turn ? { turn: turnCredentials(config.turn, session.id, peerId, config.now()) } : {}) });
      broadcast(session, { type: 'peer-joined', peer: { peerId, role, name } }, peerId);
    }

    ws.on('message', (raw, isBinary) => {
      if (isBinary) return violation('Text frames only');
      if (!allowFrame()) return violation('Rate exceeded');
      let message;
      try { message = JSON.parse(raw.toString('utf8')); } catch { return violation('Invalid JSON'); }
      if (!message || typeof message !== 'object' || Array.isArray(message)) return violation('Expected an object');
      if (!state.peerId) return hello(message);
      const { session, peerId } = state;
      if (!sessions.has(session.id) || session.peers.get(peerId)?.ws !== ws) return;
      if (message.type === 'ping') return send(ws, { type: 'pong' });
      if (message.type === 'signal') {
        const target = typeof message.to === 'string' ? session.peers.get(message.to) : null;
        if (!message.data || typeof message.data !== 'object' || Array.isArray(message.data)) return violation('signal needs object data');
        if (!target || message.to === peerId) return send(ws, { type: 'error', code: 'unknown_peer', message: 'No such peer in this session' });
        return send(target.ws, { type: 'signal', from: peerId, data: message.data });
      }
      if (message.type === 'kick') {
        if (session.peers.get(peerId).role !== 'host') return send(ws, { type: 'error', code: 'forbidden', message: 'Only the host can remove participants' });
        const target = session.peers.get(message.peer);
        if (!target || target.role === 'host') return send(ws, { type: 'error', code: 'unknown_peer', message: 'No such guest in this session' });
        session.peers.delete(message.peer);
        closeWith(target.ws, 4003, 'Removed by the host');
        return broadcast(session, { type: 'peer-left', peerId: message.peer });
      }
      return violation('Unknown message type');
    });

    ws.on('close', () => {
      clearTimeout(helloTimer);
      const { session, peerId } = state;
      if (!session || session.peers.get(peerId)?.ws !== ws) return; // replaced, kicked or session ended
      const { role } = session.peers.get(peerId);
      session.peers.delete(peerId);
      broadcast(session, { type: 'peer-left', peerId });
      if (role === 'host' && sessions.has(session.id)) {
        session.hostGrace = setTimeout(() => endSession(session, 4001, 'Host did not return'), config.hostGraceMs);
        session.hostGrace.unref();
      }
    });
    ws.on('error', () => {});
  }

  return {
    server,
    sessions,
    signIn: auth.enabled,
    async close() {
      clearInterval(sweep);
      auth.close();
      for (const session of [...sessions.values()]) endSession(session, 1001, 'Relay shutting down');
      wss.close();
      server.closeIdleConnections?.();
      await new Promise(resolve => server.close(resolve));
      await writeChain;
    },
  };
}

// Configuration from the environment; secrets are never logged.
export function configFromEnv(env = process.env) {
  const turn = env.SONORA_TURN_SECRET
    ? { secret: env.SONORA_TURN_SECRET, urls: String(env.SONORA_TURN_URLS ?? '').split(',').map(s => s.trim()).filter(Boolean),
        ttlSeconds: Number(env.SONORA_TURN_TTL ?? 3600) }
    : null;
  if (turn && (turn.urls.length === 0 || !Number.isInteger(turn.ttlSeconds) || turn.ttlSeconds < 60))
    throw new Error('SONORA_TURN_URLS must list TURN URLs and SONORA_TURN_TTL must be >= 60 seconds');
  const stripeKeys = ['STRIPE_SECRET_KEY', 'STRIPE_WEBHOOK_SECRET', 'STRIPE_PRICE_ID', 'SONORA_BILLING_SUCCESS_URL', 'SONORA_BILLING_CANCEL_URL'];
  const present = stripeKeys.filter(key => env[key]);
  if (present.length && present.length !== stripeKeys.length)
    throw new Error(`Billing needs all of ${stripeKeys.join(', ')}; missing ${stripeKeys.filter(key => !env[key]).join(', ')}`);
  const stripe = present.length
    ? { secretKey: env.STRIPE_SECRET_KEY, webhookSecret: env.STRIPE_WEBHOOK_SECRET, priceId: env.STRIPE_PRICE_ID,
        successUrl: env.SONORA_BILLING_SUCCESS_URL, cancelUrl: env.SONORA_BILLING_CANCEL_URL,
        returnUrl: env.SONORA_BILLING_RETURN_URL ?? env.SONORA_BILLING_SUCCESS_URL,
        apiVersion: env.STRIPE_API_VERSION ?? null, automaticTax: env.SONORA_STRIPE_AUTOMATIC_TAX === '1',
        ...(env.STRIPE_API_BASE ? { apiBase: env.STRIPE_API_BASE } : {}) }
    : null;
  let mail = null;
  if (env.SONORA_MAIL_PROVIDER) {
    if (!env.SONORA_MAIL_FROM) throw new Error('SONORA_MAIL_FROM is required with SONORA_MAIL_PROVIDER');
    if (env.SONORA_MAIL_PROVIDER === 'resend') {
      if (!env.RESEND_API_KEY) throw new Error('RESEND_API_KEY is required for the resend mail provider');
      mail = { provider: 'resend', from: env.SONORA_MAIL_FROM, apiKey: env.RESEND_API_KEY, ...(env.RESEND_API_BASE ? { apiBase: env.RESEND_API_BASE } : {}) };
    } else if (env.SONORA_MAIL_PROVIDER === 'outbox') {
      if (!env.SONORA_MAIL_OUTBOX) throw new Error('SONORA_MAIL_OUTBOX is required for the outbox mail provider');
      mail = { provider: 'outbox', from: env.SONORA_MAIL_FROM, outbox: env.SONORA_MAIL_OUTBOX };
    } else throw new Error(`Unknown SONORA_MAIL_PROVIDER: ${env.SONORA_MAIL_PROVIDER}`);
  }
  const publicUrl = env.SONORA_RELAY_PUBLIC_URL ?? null;
  if (publicUrl && !/^https:\/\/[^\s]+$|^http:\/\/(localhost|127\.0\.0\.1)(:\d+)?(\/[^\s]*)?$/.test(publicUrl))
    throw new Error('SONORA_RELAY_PUBLIC_URL must be https (http only for localhost)');
  return {
    stripe,
    mail,
    publicUrl,
    mode: env.SONORA_RELAY_MODE ?? 'cloud',
    accountsFile: env.SONORA_RELAY_ACCOUNTS ?? (env.SONORA_RELAY_MODE === 'open' ? null : 'accounts.json'),
    trustProxy: env.SONORA_RELAY_TRUST_PROXY === '1',
    turn,
  };
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  const config = configFromEnv();
  const relay = createRelay(config);
  const port = Number(process.env.PORT ?? 8787), host = process.env.HOST ?? '127.0.0.1';
  relay.server.listen(port, host, () =>
    console.error(`[sonora-relay] protocol ${protocolVersion}, mode ${config.mode}, listening on ${host}:${port}, TURN ${config.turn ? 'on' : 'off'}, billing ${config.stripe && config.mode === 'cloud' ? 'on' : 'off'}, sign-in ${relay.signIn ? 'on' : 'off'}`));
  for (const signal of ['SIGINT', 'SIGTERM']) process.on(signal, () => relay.close().then(() => process.exit(0)));
}
