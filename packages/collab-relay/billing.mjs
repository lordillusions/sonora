// COL-10: Stripe billing for Sonora Cloud (ADR 0009). Hosts subscribe through Stripe Checkout and manage the
// subscription in the Stripe Customer Portal. Signed webhooks set `plan` / `activeUntil` in the account store,
// which is the only thing the relay's entitlement check reads. Sales tax is the relay operator's responsibility; Stripe Tax
// can be switched on with `automaticTax`.
import { createHmac, timingSafeEqual } from 'node:crypto';

export class BillingError extends Error {
  constructor(status, code, message) { super(message); this.status = status; this.code = code; }
}

const grantStatuses = new Set(['active', 'trialing']);
const revokeStatuses = new Set(['canceled', 'unpaid', 'incomplete_expired', 'paused']);
const maxProcessedEvents = 1000;

// Stripe-Signature: t=<unix>,v1=<hex HMAC-SHA256(secret, "<t>.<raw body>")>[,v1=...] (several during secret rotation).
export function verifyStripeSignature(rawBody, header, secret, nowMs, toleranceSeconds = 300) {
  if (typeof header !== 'string' || !secret) return false;
  let timestamp = null;
  const signatures = [];
  for (const part of header.split(',')) {
    const [key, value] = part.split('=', 2).map(text => text?.trim());
    if (key === 't' && /^\d+$/.test(value ?? '')) timestamp = Number(value);
    if (key === 'v1' && /^[0-9a-f]{64}$/.test(value ?? '')) signatures.push(value);
  }
  if (timestamp === null || !signatures.length || Math.abs(nowMs / 1000 - timestamp) > toleranceSeconds) return false;
  const expected = Buffer.from(createHmac('sha256', secret).update(`${timestamp}.`).update(rawBody).digest('hex'), 'hex');
  return signatures.some(signature => timingSafeEqual(Buffer.from(signature, 'hex'), expected));
}

export function signStripePayload(rawBody, secret, timestamp) { // tests and local tooling
  return `t=${timestamp},v1=${createHmac('sha256', secret).update(`${timestamp}.`).update(rawBody).digest('hex')}`;
}

// API versions before 2025-03-31 carry the period on the subscription, later ones on its items.
export function periodEnd(subscription) {
  if (Number.isFinite(subscription.current_period_end)) return subscription.current_period_end;
  const ends = (subscription.items?.data ?? []).map(item => item.current_period_end).filter(Number.isFinite);
  return ends.length ? Math.max(...ends) : null;
}

export function createBilling({ config, store, enqueueWrite, now }) {
  const stripe = config.stripe;
  const enabled = !!(stripe?.secretKey && stripe?.webhookSecret && stripe?.priceId && stripe?.successUrl && stripe?.cancelUrl);

  async function call(path, params) {
    const body = new URLSearchParams();
    for (const [key, value] of Object.entries(params)) if (value !== undefined && value !== null) body.append(key, String(value));
    let response;
    try {
      response = await fetch(`${stripe.apiBase ?? 'https://api.stripe.com'}${path}`, {
        method: 'POST', body,
        headers: { authorization: `Bearer ${stripe.secretKey}`, 'content-type': 'application/x-www-form-urlencoded',
                   ...(stripe.apiVersion ? { 'stripe-version': stripe.apiVersion } : {}) },
        signal: AbortSignal.timeout(15000),
      });
    } catch { throw new BillingError(502, 'billing_unavailable', 'The payment provider could not be reached'); }
    const result = await response.json().catch(() => ({}));
    if (!response.ok || typeof result.url !== 'string') throw new BillingError(502, 'billing_unavailable', 'The payment provider rejected the request');
    return result;
  }

  function requireEnabled() {
    if (!enabled) throw new BillingError(404, 'billing_disabled', 'Billing is not configured on this relay');
  }

  async function checkout(account) {
    requireEnabled();
    if (account.plan === 'selfhost') throw new BillingError(409, 'not_needed', 'This account does not need a subscription');
    const session = await call('/v1/checkout/sessions', {
      mode: 'subscription',
      'line_items[0][price]': stripe.priceId,
      'line_items[0][quantity]': 1,
      client_reference_id: account.id,
      'metadata[sonora_account]': account.id,
      'subscription_data[metadata][sonora_account]': account.id,
      customer: account.stripeCustomerId,
      success_url: stripe.successUrl,
      cancel_url: stripe.cancelUrl,
      ...(stripe.automaticTax ? { 'automatic_tax[enabled]': 'true' } : {}),
    });
    return { url: session.url };
  }

  async function portal(account) {
    requireEnabled();
    if (!account.stripeCustomerId) throw new BillingError(409, 'no_subscription', 'Subscribe first; there is nothing to manage yet');
    const session = await call('/v1/billing_portal/sessions', { customer: account.stripeCustomerId, return_url: stripe.returnUrl ?? stripe.successUrl });
    return { url: session.url };
  }

  // Returns a short outcome string for tests/logs; never throws for well-signed events it doesn't handle.
  function applyEvent(data, event) {
    const processed = data.processedStripeEvents ?? (data.processedStripeEvents = []);
    if (processed.includes(event.id)) return 'duplicate';
    const object = event.data?.object ?? {};
    const findAccount = () => {
      const accountId = object.metadata?.sonora_account ?? object.client_reference_id;
      return data.accounts.find(account => account.id === accountId)
          ?? (object.customer ? data.accounts.find(account => account.stripeCustomerId === object.customer) : undefined);
    };
    let outcome = 'ignored';
    if (event.type === 'checkout.session.completed') {
      const account = findAccount();
      if (account && typeof object.customer === 'string') {
        account.stripeCustomerId = object.customer;
        if (typeof object.subscription === 'string') account.stripeSubscriptionId = object.subscription;
        outcome = 'linked';
      } else outcome = 'unknown_account';
    } else if (/^customer\.subscription\.(created|updated|deleted)$/.test(event.type)) {
      const account = findAccount();
      if (!account) outcome = 'unknown_account';
      else if (account.plan === 'selfhost') outcome = 'selfhost_unchanged';
      else if ((account.stripeEventCreated ?? 0) > event.created) outcome = 'stale';
      else if (account.stripeSubscriptionId && account.stripeSubscriptionId !== object.id && !grantStatuses.has(object.status)) outcome = 'other_subscription';
      else {
        const status = event.type.endsWith('deleted') ? 'canceled' : object.status;
        const end = periodEnd(object);
        if (typeof object.customer === 'string') account.stripeCustomerId = object.customer;
        account.stripeEventCreated = event.created;
        account.stripeStatus = status;
        if (grantStatuses.has(status) && end) {
          account.stripeSubscriptionId = object.id;
          account.plan = 'cloud';
          account.activeUntil = new Date((end + (stripe.renewalSlackHours ?? 24) * 3600) * 1000).toISOString();
          outcome = 'granted';
        } else if (status === 'past_due') {
          // Stripe is retrying the payment: keep access for a short grace period, never for the whole new period.
          const graceEnd = (event.created + (stripe.pastDueGraceDays ?? 3) * 86400) * 1000;
          const current = Date.parse(account.activeUntil ?? '') || 0;
          account.activeUntil = new Date(Math.min(Math.max(current, now()), graceEnd)).toISOString();
          outcome = 'past_due';
        } else if (revokeStatuses.has(status)) {
          account.plan = 'none';
          account.activeUntil = new Date(now()).toISOString();
          outcome = 'revoked';
        } else outcome = 'pending'; // incomplete: the first payment hasn't succeeded yet
      }
    }
    processed.push(event.id);
    if (processed.length > maxProcessedEvents) processed.splice(0, processed.length - maxProcessedEvents);
    return outcome;
  }

  async function webhook(rawBody, signatureHeader) {
    if (!stripe?.webhookSecret) throw new BillingError(404, 'billing_disabled', 'Billing is not configured on this relay');
    if (!verifyStripeSignature(rawBody, signatureHeader, stripe.webhookSecret, now()))
      throw new BillingError(400, 'invalid_signature', 'Webhook signature verification failed');
    let event;
    try { event = JSON.parse(rawBody.toString('utf8')); } catch { throw new BillingError(400, 'invalid_request', 'Webhook body must be JSON'); }
    if (typeof event?.id !== 'string' || typeof event.type !== 'string' || !Number.isInteger(event.created))
      throw new BillingError(400, 'invalid_request', 'Not a Stripe event');
    // Serialized in-process and locked across processes: concurrent webhooks and admin edits never lose an update.
    return enqueueWrite(() => store.update(data => applyEvent(data, event)));
  }

  return { enabled, checkout, portal, webhook };
}
