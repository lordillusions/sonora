# Collab relay protocol v1

Status: implemented in `packages/collab-relay` (COL-3 server side, COL-10 entitlement foundation), 2026-10-17.
The app-side client (libdatachannel WebSocket + WebRTC) is not written yet. Design context: `../COLLAB_DESIGN.md`
sections 8 and 15, decisions in `../adr/0009-collab-sessions.md`.

The relay **only** introduces peers. It forwards opaque signalling envelopes (WebRTC offers/answers/ICE candidates)
between the participants of one session and issues short-lived TURN credentials. It never receives project data,
voice, video, the invite secret or the safety code. Ops, assets and media travel peer-to-peer over DTLS/SRTP, or as
ciphertext through TURN.

## Identities and secrets

| Item | Who creates it | Who sees it | Purpose |
|---|---|---|---|
| Host token | Email sign-in (one per device) or the relay admin (`admin.mjs`) | Host app; the relay stores only its SHA-256 | Identifies the account that may host; entitlement is checked against it |
| Session ID | Relay | Everyone in the invite | Names the session |
| Guest ticket | Host app (32 random bytes, base64url) | Host, guests; the relay stores only its SHA-256 | Lets a guest join without an account; stops strangers who only guess a session ID |
| Invite secret | Host app | Host, guests only; **never sent to the relay** (URL fragment) | HMAC over signalling `data` and the safety code (peer-verified) |

Invite link shape (app-defined, shown for reference): `sonora://join/<sessionId>?relay=<origin>&ticket=<ticket>#<secret>`.

## HTTP API

All bodies are JSON, at most 16 KiB. Errors are `{"error":{"code","message"}}`.

| Method and path | Auth | Body | Success | Errors |
|---|---|---|---|---|
| `GET /v1/health` | none | | `200 {"ok":true,"protocol":1,"mode":"cloud"\|"selfhost"\|"open"}` | |
| `POST /v1/sessions` | `Authorization: Bearer <hostToken>` (not needed in `open` mode) | `{"protocol":1,"guestTicketSha256":"<64 hex>","maxParticipants"?:2..N}` | `201 {"sessionId","expiresAt","maxParticipants","signalPath":"/v1/signal"}` | `401 unauthorized`, `402 subscription_required`, `400 invalid_request`, `409 protocol_mismatch`, `429 too_many_sessions` |
| `DELETE /v1/sessions/<id>` | Bearer host token of the session's account | | `204`, closes every socket with code 4001 | `401`, `404 not_found` |

`maxParticipants` counts the host and is capped by the account's plan (default 4). A session expires after 12 hours.
It also ends if no host is connected for 10 minutes: after creation if the host never connects, or after the host's
connection closes. In `open` mode the `201` response also contains `hostKey`, which replaces the account token for
this session's host `hello` and `DELETE`. An account may own at most 3 live
sessions.

**Entitlement** is checked only here and when a host socket (re)connects: `plan` must be `cloud` with
`activeUntil` in the future, or `selfhost`. A modified client can't bypass it, because without a session the relay
neither forwards signalling nor issues TURN credentials. Guests never need an account.

## Sign-in (email magic link)

Enabled when the relay has a public URL and a mail provider (not in `open` mode); `GET /v1/health` reports `signIn`.

1. App → `POST /v1/auth/start {"email","deviceName"?}` → `202 {"loginId","pollSecret","code","expiresAt","pollIntervalSeconds":2}`.
   The app shows `code` (6 digits) and keeps `pollSecret`. Errors: `400 invalid_email`, `429 too_many_attempts`
   (5 per address and 20 per IP per hour), `502 mail_unavailable`, `404 sign_in_disabled`.
2. The relay emails a link `<publicUrl>/v1/auth/verify?token=…`. The email doesn't contain the code.
3. `GET` on the link shows a confirmation page (consumes nothing, so mail scanners are harmless). The user types the
   code from Sonora and presses Sign in (`POST /v1/auth/verify`, form `token`, `code`). Five wrong codes cancel the
   login; the link works once and expires after 15 minutes. The page is served with a strict CSP, `no-referrer` and
   `no-store`.
4. App → `POST /v1/auth/poll {"loginId","pollSecret"}`, at most once per second: `202 {"status":"pending"}`, then once
   `200 {"status":"approved","accountId","email","plan","token"}`. Unknown, expired, cancelled or already collected
   logins answer `410 expired`; polling faster than 1/s answers `429 slow_down`.

Why the typed code: someone who starts a login for another person's address only sees their *own* code, so the
victim's click can't sign the attacker in. Pending logins live in memory, so a relay restart means "start again".

| Mode | Unknown email | Known email |
|---|---|---|
| `cloud` | Account created on approval with `plan: "none"` (must subscribe to host) | Device token added |
| `selfhost` | Same `202` answer, but no email is sent (only admin-added emails can sign in) | Device token added |

Revoked accounts get the same `202` and no email. Each sign-in adds a device token (up to 10 per account, oldest
dropped), so other devices stay signed in.

| Method and path | Auth | Success |
|---|---|---|
| `GET /v1/account` | Bearer token | `200 {"accountId","email","plan","activeUntil","subscription","devices":[{"id","name","createdAt"}]}` |
| `POST /v1/auth/logout` | Bearer device token | `204`; that device's token stops working. An admin-issued token answers `409 admin_token` |

## Billing (Stripe, `cloud` mode only)

Billing uses Stripe. Sales tax is the relay operator's responsibility; Stripe Tax can be switched on (`SONORA_STRIPE_AUTOMATIC_TAX=1`).
`GET /v1/health` reports `"billing": true` when it is configured, so the app knows whether to offer it.

| Method and path | Auth | Success | Errors |
|---|---|---|---|
| `POST /v1/billing/checkout` | Bearer host token (any plan) | `200 {"url"}`: a Stripe Checkout page for the subscription; the app opens it in the browser | `401`, `404 billing_disabled`, `409 not_needed` (`selfhost` account), `502 billing_unavailable` |
| `POST /v1/billing/portal` | Bearer host token | `200 {"url"}`: the Stripe Customer Portal (change card, cancel, invoices) | `401`, `404`, `409 no_subscription`, `502` |
| `POST /v1/billing/webhook` | `Stripe-Signature` (HMAC-SHA256, 5 min tolerance, any of several `v1` values) | `200 {"received":true,"outcome"}` | `400 invalid_signature` / `invalid_request`, `404 billing_disabled` |

Checkout sets `client_reference_id` and `subscription_data.metadata.sonora_account` to the account ID, so webhook
events map to the account in any delivery order. Handled events and their effect on the account:

| Event | Effect |
|---|---|
| `checkout.session.completed` | Stores `stripeCustomerId` / `stripeSubscriptionId` |
| `customer.subscription.created` / `updated`, status `active` or `trialing` | `plan: "cloud"`, `activeUntil` = period end + 24 h slack |
| status `past_due` | Access continues for at most 3 days after the event, never for the unpaid period |
| status `canceled`, `unpaid`, `incomplete_expired`, `paused`, or `customer.subscription.deleted` | `plan: "none"` immediately |
| status `incomplete` | No change until the first payment succeeds |

Period end is read from the subscription (API versions before 2025-03-31) or from its items (later versions).
Duplicate event IDs are ignored, events older than the last applied one for that account are ignored, `selfhost`
accounts are never changed, and an event for a different, inactive subscription of the same customer doesn't revoke
the active one. Writes are serialized in the relay and protected by a lock file shared with `admin.mjs`.

## WebSocket signalling: `GET /v1/signal?session=<sessionId>`

Text frames only, each one JSON object of at most 64 KiB. The first frame must arrive within 5 s:

```json
{"type":"hello","protocol":1,"role":"host","token":"<hostToken>"}
{"type":"hello","protocol":1,"role":"guest","ticket":"<guestTicket>","name":"Ana"}
```

(Credentials go in the first frame, not the URL, so they never appear in access logs.) The relay answers:

```json
{"type":"welcome","peerId":"p3","role":"guest","peers":[{"peerId":"p1","role":"host","name":"Host"}],
 "turn":{"urls":["turn:turn.example:3478"],"username":"1760702400:s_x.p3","credential":"…","ttlSeconds":3600}}
```

`turn` is omitted when the relay has no TURN configured. Then:

| Client → relay | Relay → client |
|---|---|
| `{"type":"signal","to":"p1","data":{…opaque…}}` | to p1: `{"type":"signal","from":"p3","data":{…}}` |
| `{"type":"kick","peer":"p3"}` (host only) | to p3: close 4003; others: `peer-left` |
| `{"type":"ping"}` | `{"type":"pong"}` |
| | `{"type":"peer-joined","peer":{"peerId","role","name"}}` |
| | `{"type":"peer-left","peerId":"p3"}` |
| | `{"type":"error","code","message"}` (non-fatal) |

`data` is opaque to the relay. Peers authenticate it with the invite secret (HMAC) and verify the safety code;
unauthenticated `data` must be ignored by the app. At most one host connection per session: a reconnecting host
replaces the previous socket. Peer IDs are not reused within a session.

### Close codes

Before closing with a 4xxx code the relay sends one last frame, `{"type":"closing","code":4401,"reason":"Authentication failed"}`,
because some WebSocket clients (libdatachannel, used by the app) don't expose close codes.

| Code | Meaning |
|---|---|
| 1009 | frame larger than 64 KiB (WebSocket "message too big") |
| 4000 | protocol violation (bad JSON, binary frame, unknown type, missing or late hello, protocol mismatch, rate exceeded) |
| 4001 | session ended (deleted or expired) |
| 4002 | session full |
| 4003 | kicked by the host |
| 4004 | replaced by a newer host connection |
| 4401 | authentication failed (token, ticket or unknown session) |
| 4402 | subscription required (host entitlement no longer active) |

## Limits

- 50 frames/s per socket (token bucket, burst 100). Exceeding it closes with 4000.
- 20 failed authentications per IP per minute, then further attempts are rejected (HTTP 429 / close 4401).
- Names: at most 64 characters, control characters removed.

## TURN credentials

The relay follows coturn's `use-auth-secret` REST scheme: `username = "<unixExpiry>:<sessionId>.<peerId>"`,
`credential = base64(HMAC-SHA1(turnSecret, username))`, TTL 1 hour by default. Configure coturn with the same
`static-auth-secret`.

## Modes

| Mode | Who may host | Use |
|---|---|---|
| `cloud` | accounts whose plan is `cloud` and still active, or `selfhost` | Official Sonora Cloud relay (paid, host only) |
| `selfhost` | any account in the store (admin-issued keys, no expiry needed) | A studio or user running their own relay |
| `open` | anyone (no token) | Development and trusted private networks only |

Billing writes only `plan`, `activeUntil` and the `stripe*` fields of the account store; sign-in adds accounts and
`devices` entries.
