// COL-10: manage relay host accounts. Tokens are printed once and stored only as SHA-256 hashes.
//   node admin.mjs add --name "Studio A" --plan cloud --days 30 [--email a@b.co] [--max-participants 4]
//   node admin.mjs set --id a_... --plan cloud (--days 30 | --until 2027-01-01T00:00:00Z)
//   node admin.mjs rotate --id a_...      node admin.mjs revoke --id a_...      node admin.mjs list
// Store: --store <file>, else SONORA_RELAY_ACCOUNTS, else ./accounts.json (created owner-only).
import { parseArgs } from 'node:util';
import { AccountStore, newId, newSecret, normalizeEmail, plans, sha256 } from './accounts.mjs';

const { positionals, values } = parseArgs({
  allowPositionals: true,
  options: {
    store: { type: 'string' }, id: { type: 'string' }, name: { type: 'string' }, plan: { type: 'string' }, email: { type: 'string' },
    days: { type: 'string' }, until: { type: 'string' }, 'max-participants': { type: 'string' },
  },
});
const command = positionals[0];
const store = new AccountStore(values.store ?? process.env.SONORA_RELAY_ACCOUNTS ?? 'accounts.json');
class UsageError extends Error {}
// Throws, so a lock taken by store.update() is always released before the process exits.
const fail = message => { throw new UsageError(message); };

function activeUntil() {
  if (values.until !== undefined) {
    const time = Date.parse(values.until);
    if (!Number.isFinite(time)) fail('--until must be an ISO date');
    return new Date(time).toISOString();
  }
  if (values.days !== undefined) {
    const days = Number(values.days);
    if (!Number.isFinite(days) || days <= 0) fail('--days must be positive');
    return new Date(Date.now() + days * 86400000).toISOString();
  }
  return null;
}
function plan(required) {
  if (values.plan === undefined) return required ? fail(`--plan is required (${plans.join(', ')})`) : undefined;
  if (!plans.includes(values.plan)) fail(`--plan must be one of ${plans.join(', ')}`);
  return values.plan;
}
function maxParticipants() {
  if (values['max-participants'] === undefined) return undefined;
  const count = Number(values['max-participants']);
  if (!Number.isInteger(count) || count < 2 || count > 16) fail('--max-participants must be 2..16');
  return count;
}

// An email lets the account sign in by magic link (on a self-hosted relay, only such accounts can).
function email(data, self) {
  if (values.email === undefined) return undefined;
  const clean = normalizeEmail(values.email) ?? fail('--email must be a valid address');
  if (data.accounts.some(account => account.email === clean && account !== self)) fail(`another account already uses ${clean}`);
  return clean;
}

function execute(data) {
  const find = () => data.accounts.find(account => account.id === values.id) ?? fail(`no account ${values.id}`);
  if (command === 'add') {
    const token = newSecret();
    const account = { id: newId('a'), name: values.name ?? '', plan: plan(true), activeUntil: activeUntil(),
                      tokenSha256: sha256(token), createdAt: new Date().toISOString() };
    const count = maxParticipants();
    if (count !== undefined) account.maxParticipants = count;
    if (account.plan === 'cloud' && !account.activeUntil) fail('a cloud plan needs --days or --until');
    const address = email(data, account);
    if (address) account.email = address;
    data.accounts.push(account);
    return { lines: [{ id: account.id, token, plan: account.plan, activeUntil: account.activeUntil }], note: 'Store the token now; it is not shown again.' };
  }
  if (command === 'set') {
    const account = find();
    const nextPlan = plan(false), until = activeUntil(), count = maxParticipants();
    if (nextPlan !== undefined) account.plan = nextPlan;
    if (until !== null) account.activeUntil = until;
    if (count !== undefined) account.maxParticipants = count;
    const address = email(data, account);
    if (address) account.email = address;
    return { lines: [{ id: account.id, plan: account.plan, activeUntil: account.activeUntil ?? null, email: account.email ?? null }] };
  }
  if (command === 'rotate') {
    const account = find(), token = newSecret();
    account.tokenSha256 = sha256(token);
    return { lines: [{ id: account.id, token }], note: 'The previous token no longer works.' };
  }
  if (command === 'revoke') {
    const account = find();
    account.revoked = true;
    return { lines: [{ id: account.id, revoked: true }] };
  }
  if (command === 'list')
    return { lines: data.accounts.map(({ id, name, email: address, plan: accountPlan, activeUntil: until, revoked, maxParticipants: count, stripeStatus, devices }) =>
      ({ id, name, email: address ?? null, plan: accountPlan, activeUntil: until ?? null, revoked: !!revoked, maxParticipants: count ?? 4,
         stripeStatus: stripeStatus ?? null, devices: (devices ?? []).length })) };
  return fail('usage: admin.mjs add|set|rotate|revoke|list [options]');
}

let output;
try {
  output = await store.update(execute);
} catch (error) {
  console.error(`error: ${error.message}`);
  process.exit(error instanceof UsageError ? 2 : 1);
}
for (const line of output.lines) console.log(JSON.stringify(line));
if (output.note) console.error(output.note);
