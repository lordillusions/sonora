# ADR 0009 — Collaborative sessions

Status: accepted, 2026-10-16. Full design: [`../COLLAB_DESIGN.md`](../COLLAB_DESIGN.md).

Remote co-editing uses **host-authoritative operations**. The host's `ControlService` sequences typed, serializable
intents from every participant (local UI, peers, MCP), validates them against its model, commits them through normal
history with host-allocated identities and broadcasts the committed op. Each replica applies the same ops in order.
A CRDT and whole-screen streaming were rejected (see the design doc, section 2).

Each participant renders audio locally with its own plugins. The host may additionally stream its Main output
("listen-along") to a guest who lacks a plugin. Missing plugins keep their opaque state unchanged.

Decisions:
1. Connectivity: LAN (Bonjour, no server) first, then a self-hosted signalling + TURN relay.
2. Up to four participants in a star around the host; tested with two.
3. Free editing with object-level revisions, fine-grained (per note) conflicts and soft-lock presence badges;
   optional host-set hard track locks. Per-user guarded undo.
4. Local rendering plus optional listen-along.
5. The host decides per session whether guests may keep a copy.
6. Co-editing, presence and synced transport first (COL-1…COL-6); voice (COL-7) and camera PiP (COL-8) afterwards.
7. COL-1 (serializable `model::Op` and stable identities, including runtime note IDs) is shared with AGT-2/AGT-7 and
   precedes any networking.

Transport security: WebRTC through libdatachannel (MPL-2.0), DTLS/SRTP end to end, invite secret never sent to the
signalling server, HMAC-authenticated signalling and a short safety code derived from both certificate fingerprints.
Collab is a separate grant from the same-user local automation socket (ADR 0007).

## Hosted relay service (2026-10-16)

The official hosted relay is a **paid service**; there is no paywall inside the app.

- Context: Sonora is AGPL-3.0 (required by JUCE's GPL licence). Anyone may rebuild or share the app with a client-side
  check removed, so an in-app gate can't be enforced.
- **Free and open**: the collab protocol, the app feature, LAN/Bonjour sessions with no server, and self-hosting the
  open-source relay (`packages/collab-relay`, also AGPL).
- **Paid ("Sonora Cloud" subscription)**: the official hosted signalling and TURN relay that make "send an invite link
  and it works" possible, plus optional extras (session history/audit log, cloud project backup/versions, a persistent
  shared sample cache, higher limits such as four participants or listen-along streaming, support).
- **Only the host needs a subscription.** Guests join hosted sessions free.
- **Entitlement is checked only by the server**, when an invite or session is created on the hosted relay. The app
  holds no secret or check that matters. Accounts identify the paying host. The relay still never sees project data,
  voice or video, and end-to-end encryption and the safety code are unchanged.
- **Payment provider: Stripe** (2026-10-17). Sales tax is the relay operator's responsibility; Stripe Tax is an optional
  switch. Stripe Checkout and the Customer Portal keep card data out of Sonora entirely; signed webhooks update the
  account store (see `../collab/PROTOCOL.md`).
- **Host sign-in: email magic link** (2026-10-17). The app shows a 6-digit code, the email carries
  only a link, and the link's page requires that code plus a button press before a per-device token goes to the
  polling app. That blocks login-phishing via someone else's click and mail-scanner prefetch. Mail is sent through
  Resend by default (swappable adapter). Self-hosted relays only admit admin-added emails.
- Product names must not use "Ableton" or "Live". The hosted service also needs terms of service, a privacy policy,
  payment and tax handling, and must meet the AGPL obligations.
