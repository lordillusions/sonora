// Shared helpers for the relay test files.
import { randomBytes } from 'node:crypto';
import { promises as fs } from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { AccountStore, newId, sha256 } from './accounts.mjs';
import { createRelay } from './relay.mjs';

export async function tempStore() {
  const dir = await fs.mkdtemp(path.join(os.tmpdir(), 'sonora-relay-'));
  return path.join(dir, 'accounts.json');
}
export async function addAccount(file, fields) {
  const store = new AccountStore(file);
  const data = structuredClone(store.load());
  const token = randomBytes(32).toString('base64url');
  const account = { id: newId('a'), name: 'test', tokenSha256: sha256(token), ...fields };
  data.accounts.push(account);
  await store.save(data);
  return { ...account, token };
}
export async function start(options) {
  const relay = createRelay(options);
  await new Promise(resolve => relay.server.listen(0, '127.0.0.1', resolve));
  return { relay, base: `http://127.0.0.1:${relay.server.address().port}` };
}
export async function api(base, method, route, { token, body } = {}) {
  const response = await fetch(base + route, {
    method, headers: { ...(token ? { authorization: `Bearer ${token}` } : {}), ...(body ? { 'content-type': 'application/json' } : {}) },
    body: body === undefined ? undefined : typeof body === 'string' ? body : JSON.stringify(body),
  });
  const text = await response.text();
  return { status: response.status, body: text ? JSON.parse(text) : null };
}
export function ticket() {
  const value = randomBytes(32).toString('base64url');
  return { value, hash: sha256(value) };
}
