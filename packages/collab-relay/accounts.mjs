// COL-10: host accounts of the collab relay (docs/collab/PROTOCOL.md). The store keeps only SHA-256 hashes of host
// tokens. A billing integration later updates `plan` / `activeUntil`; nothing else needs to change.
import { createHash, randomBytes, timingSafeEqual } from 'node:crypto';
import { promises as fs, readFileSync, statSync } from 'node:fs';
import path from 'node:path';

export const plans = ['cloud', 'selfhost', 'none'];
export const maxDevicesPerAccount = 10;
export const sha256 = text => createHash('sha256').update(text).digest('hex');
export const newSecret = () => randomBytes(32).toString('base64url');
export const newId = prefix => `${prefix}_${randomBytes(12).toString('base64url')}`;

export function sameHash(hexA, hexB) {
  if (typeof hexA !== 'string' || typeof hexB !== 'string' || hexA.length !== 64 || hexB.length !== 64) return false;
  return timingSafeEqual(Buffer.from(hexA, 'hex'), Buffer.from(hexB, 'hex'));
}

const isHash = value => /^[0-9a-f]{64}$/.test(value ?? '');

// Lowercased, trimmed address, or null when it isn't a plausible single address.
export function normalizeEmail(email) {
  if (typeof email !== 'string') return null;
  const clean = email.trim().toLowerCase();
  return clean.length <= 254 && /^[^\s@<>(),;:"\[\]]+@[^\s@<>(),;:"\[\]]+\.[^\s@<>(),;:"\[\]]+$/.test(clean) ? clean : null;
}

// An account authenticates with an admin-issued token (`tokenSha256`) and/or per-device sign-in tokens (`devices`).
// Signed-out accounts may have neither; they sign in again by email.
function validate(data) {
  if (!data || data.version !== 1 || !Array.isArray(data.accounts)) throw new Error('Account store must be {"version":1,"accounts":[...]}');
  for (const account of data.accounts) {
    const devicesOk = account.devices === undefined
      || (Array.isArray(account.devices) && account.devices.every(device => isHash(device?.tokenSha256)));
    if (typeof account.id !== 'string' || (account.tokenSha256 !== undefined && !isHash(account.tokenSha256)) || !devicesOk
        || !plans.includes(account.plan) || (account.email !== undefined && normalizeEmail(account.email) !== account.email))
      throw new Error(`Invalid account entry ${JSON.stringify(account.id)}`);
  }
  return data;
}

// Adds a sign-in token for one device, dropping the oldest beyond the cap. Returns the raw token (shown once).
export function addDevice(account, name, now = Date.now()) {
  const token = newSecret();
  const devices = account.devices ?? (account.devices = []);
  devices.push({ id: newId('d'), name: String(name ?? '').slice(0, 64), tokenSha256: sha256(token), createdAt: new Date(now).toISOString() });
  if (devices.length > maxDevicesPerAccount) devices.splice(0, devices.length - maxDevicesPerAccount);
  return token;
}

const tokenMatches = (account, hash) =>
  sameHash(account.tokenSha256 ?? '', hash) || (account.devices ?? []).some(device => sameHash(device.tokenSha256, hash));

export class AccountStore {
  constructor(file) {
    this.file = file;
    this.signature = null;
    this.data = { version: 1, accounts: [] };
  }

  // Cheap enough per request: one stat, and a re-read only when the file was replaced or changed.
  load() {
    if (!this.file) return this.data;
    let stat;
    try { stat = statSync(this.file); } catch (error) {
      if (error.code !== 'ENOENT') throw error;
      this.signature = null;
      this.data = { version: 1, accounts: [] };
      return this.data;
    }
    const signature = `${stat.ino}:${stat.mtimeMs}:${stat.size}`;
    if (signature !== this.signature) {
      this.data = validate(JSON.parse(readFileSync(this.file, 'utf8')));
      this.signature = signature;
    }
    return this.data;
  }

  accounts() { return this.load().accounts; }

  findByToken(token) {
    if (typeof token !== 'string' || token.length < 16 || token.length > 256) return null;
    const hash = sha256(token);
    return this.accounts().find(account => tokenMatches(account, hash)) ?? null;
  }

  findById(id) { return this.accounts().find(account => account.id === id) ?? null; }

  findByEmail(email) {
    const clean = normalizeEmail(email);
    return clean ? this.accounts().find(account => account.email === clean) ?? null : null;
  }

  // Read-modify-write under an exclusive lock file shared by every writer process (relay webhooks, admin CLI).
  // `change` receives a fresh copy of the store and returns a result; the copy is saved afterwards.
  async update(change, { timeoutMs = 5000, staleMs = 30000 } = {}) {
    const lock = `${this.file}.lock`;
    const started = Date.now();
    for (;;) {
      try {
        const handle = await fs.open(lock, 'wx', 0o600);
        await handle.close();
        break;
      } catch (error) {
        if (error.code !== 'EEXIST') throw error;
        const age = await fs.stat(lock).then(stat => Date.now() - stat.mtimeMs, () => 0);
        if (age > staleMs) { await fs.rm(lock, { force: true }); continue; } // a crashed writer
        if (Date.now() - started > timeoutMs) throw new Error('Account store is locked by another writer');
        await new Promise(resolve => setTimeout(resolve, 10));
      }
    }
    try {
      this.signature = null;
      const data = structuredClone(this.load());
      const result = await change(data);
      await this.save(data);
      return result;
    } finally {
      await fs.rm(lock, { force: true });
    }
  }

  // Atomic replace with owner-only permissions; readers never see a partial file.
  async save(data) {
    validate(data);
    const temporary = path.join(path.dirname(this.file), `.${path.basename(this.file)}.${process.pid}.tmp`);
    await fs.writeFile(temporary, `${JSON.stringify(data, null, 2)}\n`, { mode: 0o600 });
    await fs.rename(temporary, this.file);
    this.signature = null;
  }
}

// Who may host on a relay in `mode`. Guests never need an account.
export function entitlement(account, mode, now = Date.now()) {
  if (!account || account.revoked) return { ok: false, code: 'unauthorized' };
  const maxParticipants = Number.isInteger(account.maxParticipants) ? account.maxParticipants : 4;
  if (mode === 'selfhost' || account.plan === 'selfhost') return { ok: true, maxParticipants };
  const activeUntil = Date.parse(account.activeUntil ?? '');
  if (account.plan === 'cloud' && Number.isFinite(activeUntil) && activeUntil > now) return { ok: true, maxParticipants };
  return { ok: false, code: 'subscription_required' };
}
