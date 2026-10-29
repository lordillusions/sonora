# Sonora collab relay

Signalling and TURN-credential service for Sonora collab sessions (COL-3 server side, COL-10 subscription
foundation). Protocol: [`docs/collab/PROTOCOL.md`](../../docs/collab/PROTOCOL.md). Design and business model:
[`docs/COLLAB_DESIGN.md`](../../docs/COLLAB_DESIGN.md) sections 8 and 15, [ADR 0009](../../docs/adr/0009-collab-sessions.md).

The relay introduces the peers of a session and forwards opaque WebRTC signalling. It never receives project data,
audio, video or the invite secret. Hosting requires an entitled account (checked here, server side). Guests join
with the ticket from the invite and never need an account.

## Run

```bash
npm install
node admin.mjs add --name "Studio A" --plan selfhost        # prints the host token once
SONORA_RELAY_MODE=selfhost SONORA_RELAY_ACCOUNTS=accounts.json PORT=8787 node relay.mjs
npm test
```

| Variable | Default | Meaning |
|---|---|---|
| `SONORA_RELAY_MODE` | `cloud` | `cloud` (active `cloud` plan or `selfhost` account), `selfhost` (any account in the store), `open` (no accounts; development only) |
| `SONORA_RELAY_ACCOUNTS` | `accounts.json` | Account store (owner-only, token hashes only). Reloaded automatically when it changes |
| `HOST` / `PORT` | `127.0.0.1` / `8787` | Listen address. Put a TLS reverse proxy in front: clients must use `https`/`wss` |
| `SONORA_RELAY_TRUST_PROXY` | off | `1` takes the client address from `X-Forwarded-For` (only behind your own proxy) |
| `SONORA_TURN_SECRET`, `SONORA_TURN_URLS`, `SONORA_TURN_TTL` | off, none, 3600 | coturn `static-auth-secret`, comma-separated TURN URLs, credential lifetime (seconds) |

## Deploy on one Google Cloud VM (COL-3b)

`deploy/` runs the relay, Caddy (HTTPS with an automatic certificate) and coturn (STUN + TURN) with Docker Compose on
one e2-micro VM. You need the [gcloud CLI](https://cloud.google.com/sdk/docs/install), `gcloud auth login`, and a
project with billing enabled.

```bash
deploy/gcp.sh ip --project <project-id>           # reserves the static IP and prints it
# DNS: an A record relay.sonoradaw.com -> that IP (the project's relay; sonoradaw.com itself stays on GitHub Pages)
deploy/gcp.sh create --project <project-id> --domain relay.sonoradaw.com   # waits for DNS, then VM + stack
deploy/gcp.sh token "Your Name"                   # host access key, shown once
```

In Sonora: Collaborate > Relay: **Change...** > the URL and the access key (once per Mac). From then on, **Share over
the Internet** is one click and the invite link lands on the clipboard. Guests only paste the link; they need no key.
The app's default relay is `https://relay.sonoradaw.com` (CMake `SONORA_COLLAB_RELAY`; a fork or self-hoster sets
its own, empty = a relay on this Mac). A relay the user picked with Change... wins over the default.

- Without `--domain`, the relay is `https://<ip-with-dashes>.sslip.io` (no DNS setup). `update --domain D` moves an
  existing VM to another name (the TURN secret and accounts stay).
- Zone: `us-east1-b` by default (e2-micro free tier regions are us-east1, us-central1, us-west1; us-east1 is the closest
  to South America). `--zone southamerica-east1-b` lowers TURN latency for Brazil but is outside the free tier.
- `deploy/gcp.sh update` after changing the relay; `status`, `logs`, `ssh`, `delete`. `.env` (TURN secret) and
  `data/accounts.json` stay on the VM across updates.
- Ports: TCP 80/443 (Caddy), TCP+UDP 3478 (STUN/TURN), UDP 49160-49200 (TURN relay ports, about 20 relayed links).
- coturn never relays into private ranges or the metadata server (only its own address, for two TURN peers), and has
  no bandwidth caps on purpose: coturn enforces them by dropping packets, which collapsed transfers to ~100 KB/s in tests.

Cost (list prices, check the GCP calculator): the e2-micro VM and its 30 GB standard disk are in the free tier; the
static IPv4 is about US$ 3–4 per month; internet egress on the Standard network tier has a monthly free allowance and
then costs per GB. Signalling is tiny; TURN only carries sessions whose peers can't connect directly (often 10–20%).
Set a billing budget alert on the project.

Local check of the same stack (Docker Desktop): see docs/collab/TESTING.md "Self-hosted relay stack".

## Sign-in (email magic link)

| Variable | Meaning |
|---|---|
| `SONORA_RELAY_PUBLIC_URL` | Public `https://` URL of the relay (http only for localhost), used in sign-in links |
| `SONORA_MAIL_PROVIDER` | `resend` (production) or `outbox` (development: writes each email as a JSON file, sends nothing) |
| `SONORA_MAIL_FROM` | Sender, e.g. `Sonora <login@your-domain>` (the domain must be verified with the provider) |
| `RESEND_API_KEY` / `SONORA_MAIL_OUTBOX` | Resend API key / outbox folder |

On a self-hosted relay, add members with `node admin.mjs add --plan selfhost --email them@studio.example`. Only
those addresses receive sign-in emails. Another mail provider is a small adapter in `mail.mjs`.

## Billing (Stripe)

Only in `cloud` mode. Set **all** of these, or none (the relay refuses to start with a partial set):

| Variable | Meaning |
|---|---|
| `STRIPE_SECRET_KEY` | Secret key (`sk_test_…` while testing, `sk_live_…` in production) |
| `STRIPE_WEBHOOK_SECRET` | Signing secret of the webhook endpoint (`whsec_…`) |
| `STRIPE_PRICE_ID` | The recurring price of the Sonora Cloud plan (`price_…`) |
| `SONORA_BILLING_SUCCESS_URL` / `SONORA_BILLING_CANCEL_URL` | Pages Checkout returns to |
| `SONORA_BILLING_RETURN_URL` | Optional; where the Customer Portal returns (default: success URL) |
| `SONORA_STRIPE_AUTOMATIC_TAX` | Optional `1` to enable Stripe Tax on Checkout (sales tax is otherwise the relay operator's job) |
| `STRIPE_API_VERSION` / `STRIPE_API_BASE` | Optional pin of the API version / alternative endpoint (tests) |

Stripe dashboard setup (test mode first):
1. Create a product "Sonora Cloud" with a recurring price, and copy its price ID.
2. Activate the Customer Portal (Settings → Billing → Customer portal) and allow cancelling and updating payment methods.
3. Add a webhook endpoint `https://<relay>/v1/billing/webhook` for `checkout.session.completed`,
   `customer.subscription.created`, `customer.subscription.updated` and `customer.subscription.deleted`, and copy its signing secret.
4. Local testing: `stripe listen --forward-to localhost:8787/v1/billing/webhook` prints a temporary `whsec_…`.

The relay must be a single process: webhook writes are serialized in memory, and a lock file covers the admin CLI.

## Accounts

`node admin.mjs add|set|rotate|revoke|list` (see the header of `admin.mjs`). A `cloud` plan needs an expiry
(`--days` / `--until`); `selfhost` never expires. Stripe webhooks update `plan` and `activeUntil` in the same
store; `list` also shows the last Stripe status.

## Not done yet

- The app-side client (libdatachannel WebSocket + WebRTC) and LAN/Bonjour mode (COL-3).
- The app side of sign-in and subscribing (screens, Keychain storage of the device token) (COL-10).
- A real email delivery check with a verified sending domain (tests use an outbox and a fake Resend API).
- A real Stripe test-mode run: the integration is verified only against a local fake API and locally signed events.
- A first deployment on a real VM and a two-Mac test over a WAN (COL-3b); monitoring and multi-instance session sharing.
