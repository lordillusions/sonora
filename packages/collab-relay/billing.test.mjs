// COL-10: Stripe billing against a local fake Stripe API and correctly signed webhooks (no network, no real keys).
import test from 'node:test';
import assert from 'node:assert/strict';
import http from 'node:http';
import { execFile } from 'node:child_process';
import { promisify } from 'node:util';
import { fileURLToPath } from 'node:url';
import { AccountStore } from './accounts.mjs';
import { periodEnd, signStripePayload, verifyStripeSignature } from './billing.mjs';
import { configFromEnv } from './relay.mjs';
import { addAccount, api, start, tempStore, ticket } from './test-helpers.mjs';

const webhookSecret = 'whsec_test_secret';
const run = promisify(execFile);
const admin = fileURLToPath(new URL('./admin.mjs', import.meta.url));
const nowSeconds = () => Math.floor(Date.now() / 1000);
let sequence = 0;

async function fakeStripe({ fail = false } = {}) {
  const requests = [];
  const server = http.createServer(async (request, response) => {
    let body = '';
    for await (const chunk of request) body += chunk;
    requests.push({ path: request.url, auth: request.headers.authorization, form: Object.fromEntries(new URLSearchParams(body)) });
    if (fail) { response.writeHead(400, { 'content-type': 'application/json' }); return response.end('{"error":{"message":"bad"}}'); }
    response.writeHead(200, { 'content-type': 'application/json' });
    response.end(JSON.stringify(request.url.startsWith('/v1/checkout')
      ? { id: 'cs_test_1', url: 'https://checkout.stripe.test/c/cs_test_1' }
      : { id: 'bps_test_1', url: 'https://billing.stripe.test/p/bps_test_1' }));
  });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  return { requests, base: `http://127.0.0.1:${server.address().port}`, close: () => new Promise(resolve => server.close(resolve)) };
}

function stripeConfig(apiBase) {
  return { secretKey: 'sk_test_fake', webhookSecret, priceId: 'price_cloud_monthly', apiBase,
           successUrl: 'https://sonora.example/billing/done', cancelUrl: 'https://sonora.example/billing/cancel' };
}

function event(type, object, created = nowSeconds()) {
  return { id: `evt_${++sequence}`, object: 'event', type, created, data: { object } };
}

function subscription(accountId, status, { id = 'sub_1', customer = 'cus_1', end = nowSeconds() + 30 * 86400, legacy = false } = {}) {
  return { id, object: 'subscription', customer, status, metadata: accountId ? { sonora_account: accountId } : {},
           ...(legacy ? { current_period_end: end } : { items: { data: [{ id: 'si_1', current_period_end: end }] } }) };
}

async function deliver(base, payload, { secret = webhookSecret, timestamp = nowSeconds(), header, raw } = {}) {
  const body = raw ?? JSON.stringify(payload);
  const response = await fetch(`${base}/v1/billing/webhook`, {
    method: 'POST', body,
    headers: { 'content-type': 'application/json', 'stripe-signature': header ?? signStripePayload(body, secret, timestamp) },
  });
  return { status: response.status, body: await response.json() };
}

const account = (file, id) => new AccountStore(file).findById(id);
const hostSession = (base, token) => api(base, 'POST', '/v1/sessions', { token, body: { protocol: 1, guestTicketSha256: ticket().hash } });

test('billing: checkout, webhooks and the portal turn a plan-less account into a host and back', async () => {
  const stripe = await fakeStripe();
  const file = await tempStore();
  const host = await addAccount(file, { plan: 'none' });
  const { relay, base } = await start({ mode: 'cloud', accountsFile: file, stripe: stripeConfig(stripe.base) });
  try {
    assert.equal((await api(base, 'GET', '/v1/health')).body.billing, true);
    assert.equal((await api(base, 'POST', '/v1/billing/checkout')).status, 401);
    assert.equal((await hostSession(base, host.token)).status, 402);

    const checkout = await api(base, 'POST', '/v1/billing/checkout', { token: host.token });
    assert.equal(checkout.status, 200);
    assert.equal(checkout.body.url, 'https://checkout.stripe.test/c/cs_test_1');
    const sent = stripe.requests[0];
    assert.equal(sent.path, '/v1/checkout/sessions');
    assert.equal(sent.auth, 'Bearer sk_test_fake');
    assert.equal(sent.form.mode, 'subscription');
    assert.equal(sent.form['line_items[0][price]'], 'price_cloud_monthly');
    assert.equal(sent.form.client_reference_id, host.id);
    assert.equal(sent.form['subscription_data[metadata][sonora_account]'], host.id);
    assert.equal(sent.form.success_url, 'https://sonora.example/billing/done');
    assert.equal(sent.form.customer, undefined);
    assert.equal(sent.form['automatic_tax[enabled]'], undefined);
    assert.equal((await api(base, 'POST', '/v1/billing/portal', { token: host.token })).body.error.code, 'no_subscription');

    // Stripe may deliver the subscription before the checkout completion; metadata links either order.
    const end = nowSeconds() + 30 * 86400;
    assert.equal((await deliver(base, event('customer.subscription.created', subscription(host.id, 'active', { end })))).body.outcome, 'granted');
    assert.equal((await deliver(base, event('checkout.session.completed',
      { id: 'cs_test_1', object: 'checkout.session', client_reference_id: host.id, customer: 'cus_1', subscription: 'sub_1' }))).body.outcome, 'linked');
    const granted = account(file, host.id);
    assert.equal(granted.plan, 'cloud');
    assert.equal(granted.stripeCustomerId, 'cus_1');
    assert.equal(granted.stripeSubscriptionId, 'sub_1');
    assert.equal(Date.parse(granted.activeUntil), (end + 24 * 3600) * 1000);
    assert.equal((await hostSession(base, host.token)).status, 201);

    assert.equal((await api(base, 'POST', '/v1/billing/portal', { token: host.token })).body.url, 'https://billing.stripe.test/p/bps_test_1');
    assert.equal(stripe.requests[1].form.customer, 'cus_1');
    assert.equal(stripe.requests[1].form.return_url, 'https://sonora.example/billing/done');
    await api(base, 'POST', '/v1/billing/checkout', { token: host.token });
    assert.equal(stripe.requests[2].form.customer, 'cus_1');

    // A failed renewal keeps access only for the grace period, never for the whole unpaid period.
    const failedAt = nowSeconds() + 5;
    assert.equal((await deliver(base, event('customer.subscription.updated', subscription(host.id, 'past_due', { end: end + 30 * 86400 }), failedAt))).body.outcome, 'past_due');
    assert.ok(Date.parse(account(file, host.id).activeUntil) <= (failedAt + 3 * 86400) * 1000);
    assert.equal((await hostSession(base, host.token)).status, 201);

    assert.equal((await deliver(base, event('customer.subscription.deleted', subscription(host.id, 'canceled'), failedAt + 10))).body.outcome, 'revoked');
    assert.equal(account(file, host.id).plan, 'none');
    assert.equal((await hostSession(base, host.token)).status, 402);
  } finally { await relay.close(); await stripe.close(); }
});

test('billing: webhooks are authenticated, idempotent, ordered and never touch staff or unrelated accounts', async () => {
  const file = await tempStore();
  const host = await addAccount(file, { plan: 'none' });
  const staff = await addAccount(file, { plan: 'selfhost' });
  const { relay, base } = await start({ mode: 'cloud', accountsFile: file, stripe: stripeConfig('http://127.0.0.1:9') });
  try {
    const created = event('customer.subscription.created', subscription(host.id, 'active'));
    assert.equal((await deliver(base, created, { secret: 'whsec_wrong' })).status, 400);
    assert.equal((await deliver(base, created, { timestamp: nowSeconds() - 600 })).status, 400);
    assert.equal((await deliver(base, created, { header: 'v1=deadbeef' })).status, 400);
    assert.equal(account(file, host.id).plan, 'none');
    // During secret rotation Stripe sends several v1 signatures; one valid one is enough.
    const body = JSON.stringify(created), t = nowSeconds();
    const rotated = `${signStripePayload(body, 'whsec_old', t)},${signStripePayload(body, webhookSecret, t).split(',')[1]}`;
    assert.equal((await deliver(base, null, { raw: body, header: rotated })).body.outcome, 'granted');
    assert.equal((await deliver(base, created)).body.outcome, 'duplicate');

    const later = nowSeconds() + 100;
    assert.equal((await deliver(base, event('customer.subscription.updated', subscription(host.id, 'canceled'), later))).body.outcome, 'revoked');
    assert.equal((await deliver(base, event('customer.subscription.updated', subscription(host.id, 'active'), later - 50))).body.outcome, 'stale');
    assert.equal(account(file, host.id).plan, 'none');

    assert.equal((await deliver(base, event('customer.subscription.created', subscription(staff.id, 'canceled', { customer: 'cus_staff' })))).body.outcome, 'selfhost_unchanged');
    assert.equal(account(file, staff.id).plan, 'selfhost');
    assert.equal((await deliver(base, event('customer.subscription.created', subscription('a_nobody', 'active', { customer: 'cus_x' })))).body.outcome, 'unknown_account');
    assert.equal((await deliver(base, event('invoice.paid', { id: 'in_1', customer: 'cus_1' }))).body.outcome, 'ignored');

    const fresh = await addAccount(file, { plan: 'none' });
    assert.equal((await deliver(base, event('customer.subscription.created', subscription(fresh.id, 'incomplete', { id: 'sub_9', customer: 'cus_9' })))).body.outcome, 'pending');
    assert.equal(account(file, fresh.id).plan, 'none');
    assert.equal((await deliver(base, event('customer.subscription.updated', subscription(fresh.id, 'active', { id: 'sub_9', customer: 'cus_9' })))).body.outcome, 'granted');
    // Cancelling some other subscription of the same customer doesn't revoke the active one.
    assert.equal((await deliver(base, event('customer.subscription.deleted', subscription(fresh.id, 'canceled', { id: 'sub_old', customer: 'cus_9' })))).body.outcome, 'other_subscription');
    assert.equal(account(file, fresh.id).plan, 'cloud');

    const raw = '{not json';
    assert.equal((await deliver(base, null, { raw })).status, 400);
    assert.equal((await deliver(base, { id: 'evt_x', type: 'x' })).status, 400);
  } finally { await relay.close(); }
});

test('billing: concurrent webhooks and admin CLI processes never lose an update', async () => {
  const file = await tempStore();
  const accounts = [];
  for (let i = 0; i < 12; ++i) accounts.push(await addAccount(file, { plan: 'none' }));
  const { relay, base } = await start({ mode: 'cloud', accountsFile: file, stripe: stripeConfig('http://127.0.0.1:9') });
  try {
    // Webhooks inside the relay and admin CLI processes write the same file at the same time.
    const [results, added] = await Promise.all([
      Promise.all(accounts.map((entry, i) =>
        deliver(base, event('customer.subscription.created', subscription(entry.id, 'active', { id: `sub_${i}`, customer: `cus_${i}` }))))),
      Promise.all([0, 1, 2, 3].map(i => run(process.execPath, [admin, 'add', '--store', file, '--name', `cli-${i}`, '--plan', 'selfhost']))),
    ]);
    assert.ok(results.every(result => result.body.outcome === 'granted'));
    const stored = new AccountStore(file).accounts();
    assert.ok(accounts.every(entry => stored.find(a => a.id === entry.id).plan === 'cloud'));
    assert.ok(added.every(({ stdout }) => stored.some(a => a.id === JSON.parse(stdout).id)));
    assert.equal(stored.length, 16);
    assert.equal(new AccountStore(file).load().processedStripeEvents.length, 12);
  } finally { await relay.close(); }
});

test('billing: provider failures, disabled billing and accounts that need no subscription are reported clearly', async () => {
  const failing = await fakeStripe({ fail: true });
  const file = await tempStore();
  const host = await addAccount(file, { plan: 'none' });
  const staff = await addAccount(file, { plan: 'selfhost' });
  const cloud = await start({ mode: 'cloud', accountsFile: file, stripe: { ...stripeConfig(failing.base), automaticTax: true } });
  const self = await start({ mode: 'selfhost', accountsFile: file, stripe: stripeConfig(failing.base) });
  try {
    const failed = await api(cloud.base, 'POST', '/v1/billing/checkout', { token: host.token });
    assert.equal(failed.status, 502);
    assert.equal(failed.body.error.code, 'billing_unavailable');
    assert.equal(failing.requests[0].form['automatic_tax[enabled]'], 'true');
    assert.equal((await api(cloud.base, 'POST', '/v1/billing/checkout', { token: staff.token })).body.error.code, 'not_needed');

    assert.equal((await api(self.base, 'GET', '/v1/health')).body.billing, false);
    assert.equal((await api(self.base, 'POST', '/v1/billing/checkout', { token: host.token })).body.error.code, 'billing_disabled');
    assert.equal((await deliver(self.base, event('customer.subscription.created', subscription(host.id, 'active')))).status, 404);
  } finally { await cloud.relay.close(); await self.relay.close(); await failing.close(); }
});

test('billing helpers: signature checks, both subscription shapes and all-or-nothing environment config', () => {
  const body = Buffer.from('{"id":"evt_1"}'), t = nowSeconds();
  assert.ok(verifyStripeSignature(body, signStripePayload(body, 's', t), 's', Date.now()));
  assert.ok(!verifyStripeSignature(body, signStripePayload(body, 's', t), 's', Date.now() + 301000));
  assert.ok(!verifyStripeSignature(Buffer.from('{"id":"evt_2"}'), signStripePayload(body, 's', t), 's', Date.now()));
  assert.ok(!verifyStripeSignature(body, undefined, 's', Date.now()));
  assert.equal(periodEnd({ current_period_end: 100 }), 100);
  assert.equal(periodEnd({ items: { data: [{ current_period_end: 100 }, { current_period_end: 200 }] } }), 200);
  assert.equal(periodEnd({ items: { data: [] } }), null);

  const env = { STRIPE_SECRET_KEY: 'sk', STRIPE_WEBHOOK_SECRET: 'wh', STRIPE_PRICE_ID: 'price', SONORA_BILLING_SUCCESS_URL: 'https://a/ok',
                SONORA_BILLING_CANCEL_URL: 'https://a/no', SONORA_STRIPE_AUTOMATIC_TAX: '1' };
  const config = configFromEnv(env);
  assert.equal(config.stripe.priceId, 'price');
  assert.equal(config.stripe.returnUrl, 'https://a/ok');
  assert.equal(config.stripe.automaticTax, true);
  assert.throws(() => configFromEnv({ STRIPE_SECRET_KEY: 'sk' }), /missing STRIPE_WEBHOOK_SECRET/);
});
