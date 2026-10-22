// COL-10: outgoing email for magic-link sign-in. `resend` sends through Resend's HTTP API; `outbox` writes one JSON
// file per message (development and tests; it never sends). Another provider is one more adapter with `send()`.
import { promises as fs } from 'node:fs';
import path from 'node:path';
import { randomBytes } from 'node:crypto';

export class MailError extends Error {}

export function createMailer(mail) {
  if (!mail) return null;
  if (mail.provider === 'outbox') {
    return {
      async send(message) {
        await fs.mkdir(mail.outbox, { recursive: true, mode: 0o700 });
        const file = path.join(mail.outbox, `${Date.now()}-${randomBytes(4).toString('hex')}.json`);
        await fs.writeFile(file, JSON.stringify({ from: mail.from, ...message }, null, 2), { mode: 0o600 });
      },
    };
  }
  if (mail.provider === 'resend') {
    return {
      async send({ to, subject, text, html }) {
        let response;
        try {
          response = await fetch(`${mail.apiBase ?? 'https://api.resend.com'}/emails`, {
            method: 'POST',
            headers: { authorization: `Bearer ${mail.apiKey}`, 'content-type': 'application/json' },
            body: JSON.stringify({ from: mail.from, to: [to], subject, text, html }),
            signal: AbortSignal.timeout(15000),
          });
        } catch { throw new MailError('The email provider could not be reached'); }
        if (!response.ok) throw new MailError('The email provider rejected the message');
      },
    };
  }
  throw new Error(`Unknown mail provider: ${mail.provider}`);
}
