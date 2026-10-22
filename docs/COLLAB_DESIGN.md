# Collab sessions — design plan

Status: **accepted**, 2026-10-16 (all section 13 recommendations). Recorded in ADR 0009; tasks are the
`COL-*` items in `ROADMAP.md` (section 12).

Goal: two or more producers work on the same Sonora set at the same time over a secure internet (or LAN) connection.
They see each other's cursors and edits live, hear the same thing, talk by voice, and optionally see each other on
camera, much like co-editing a shared Office 365 document.

---

## Implementation status (2026-10-20)

Co-editing works between real Sonora instances, on the local network and through the relay: share and join, join
snapshot with sample transfer, edits from everyone (any edit, via object patches, [ADR 0010](adr/0010-collab-object-patches.md)
instead of the per-call-site ops of section 3), per-object conflict rejection, per-person undo, roles, presence cursors,
live mixer/device values, plugin editor state, shared transport with clock sync, guest sample drops, reconnect, and a
guest's "Save a Copy". Protocol: [collab/PEER_PROTOCOL.md](collab/PEER_PROTOCOL.md); how to test:
[collab/TESTING.md](collab/TESTING.md). Not yet: voice and camera (sections 9), follow mode and gesture ghosts
(section 1.5), per-note merges inside one clip, listen-along, LAN without a link via Bonjour is implemented but
approval-based joins rely on the safety code alone, Sonora Cloud screens (COL-10), COL-9 hardening.

## 1. What the user sees

1. **Host** opens a set and chooses *File > Share Session…*. Sonora runs Collect All and Save if needed, then shows an
   invite link/code and an access level (Edit, or Listen & Comment).
2. **Guest** chooses *File > Join Session…* and pastes the invite. Sonora shows a **compatibility report** before
   anything opens: missing plugins, plugin version differences, Sonora protocol version, and the download size of
   any missing samples.
3. The guest downloads only the files they don't already have (content-addressed cache), then the set opens in a
   **collab document** window. Its title bar shows participants as coloured avatars.
4. Both users compare a **4-word safety code** over voice once ("amber-tiger-river-nine"). This proves no server sits
   in the middle (section 8).
5. While working:
   - Each participant has a colour. Remote **mouse cursors** appear in Arrangement, Session and piano roll, positioned
     by beat/track/pitch, so they land on the same musical spot whatever each user's zoom and window size.
   - Remote **selections** (clip/time ranges, notes) appear as coloured outlines. A clip someone has open in the
     detail view carries their avatar badge.
   - While someone drags a clip, resizes a note or turns a knob, the others see a live **ghost preview**. The edit
     commits when they release.
   - **Follow mode**: click an avatar to follow that person's view (scroll, zoom, open clip).
   - **Shared transport**: play/stop/seek/loop are synced, and playback starts at the same moment on both sides.
     A toggle lets a user audition locally without moving the others.
   - **Voice** channel with mute, push-to-talk, a talkback volume and an option to dip the music while someone speaks.
     Voice is never recorded into the set or exported.
   - Optional **camera PiP**: small floating tiles that you can drag and collapse.
6. Dragging a sample into a track works for everyone. The others see the clip immediately with a "downloading" stripe,
   and it becomes audible as soon as the file arrives.
7. When the host ends the session, guests choose whether to keep a local copy (if the host allows it). The host's
   project stays the copy of record.

---

## 2. Core architecture decision: host-authoritative operations

| Option | Verdict |
|---|---|
| Stream the host's screen and audio to the guest | Rejected as the main mode. Guests can't edit locally and audio quality suffers. Kept as a fallback for audio only ("listen-along", section 7). |
| CRDT (Automerge/Yjs-style) over the whole model | Rejected for now. `.als` round-trip fidelity, opaque plugin state, device ID allocation and the existing staged-validation command service don't map cleanly to CRDT merges. |
| **Host-authoritative, operation-based (chosen)** | The host's `ControlService` is the single sequencer. Guests send typed *intents*. The host validates them against the current model, commits them through normal history, assigns a sequence number and broadcasts the committed op. Every participant then applies the same ops in the same order. |

This reuses what already exists. ADR 0007 already requires native workflows, MCP and the native LLM to share one typed,
validated command service with targets, revisions and idempotency keys. A collab peer becomes one more client of that
service, like an MCP agent. The host checks every edit, so a guest can't push malformed state.

```
 Guest A UI ──intent──┐                                     ┌──committed op #n──► Guest A model/engine
 Guest B UI ──intent──┼──► Host ControlService (sequencer) ─┼──committed op #n──► Guest B model/engine
 Host UI ─────intent──┘    validate · commit · log          └── applied locally ──► Host model/engine
```

Each participant **renders audio locally** from its own copy of the model, with its own plugins. Nothing streams audio
for playback, so quality is full and bandwidth is tiny (ops are a few hundred bytes).

---

## 3. Prerequisite: serializable operations (the biggest piece of work)

Today `model::EditCommand` (`src/core/Edit.h`) holds `std::function` apply/revert closures. Most factories address
tracks by **index**. Closures can't be sent over a network, and indices differ as soon as two people insert tracks.
88 `applyEdit` call sites in `src/ui`/`src/app` build about 45 kinds of command.

Work (overlaps AGT-2/AGT-7; doing it helps MCP too):

- **`model::Op`**: a plain, versioned, serializable intent such as `clip.move {trackId, clipUid, toTrackId, beat}`,
  `notes.update {trackId, clipUid, location, updates[{noteId, …}]}` or `device.insert {trackId, position, deviceSpec}`.
  `opToCommand(const Song&, const Op&) -> EditCommand` produces the existing undoable command, so history, the engine
  refresh flags and the writer stay unchanged.
- **Stable identity everywhere**: ops address `TrackId`, clip UID, `ClipLocation` (scene/lane ID), device ID and a new
  **runtime note ID** (today `noteId` is `-1` for new notes, and the control API uses revision-scoped `noteIndex`).
  The host allocates every new ID (track, clip UID, device ID, note ID, automation pointee) inside the committed op, so
  replicas never allocate IDs themselves and can't diverge.
- **Route every UI edit through `Op`** (UI → Op → host → command). Solo use runs the same path with a local sequencer,
  so there is one code path to test.
- Extend `schemas/control/v1` with the full vocabulary. MCP gets complete coverage as a side effect.

**Done when**: an automated test replays a recorded op log onto a fresh copy of the set, and the result is
byte-identical to the original (`AlsWriter` output and a model hash).

**Progress (2026-10-16)**: first slice implemented. `core/Op.*` has 12 op types and `core/OpJson.*` is the wire codec.
Notes carry runtime keys that are unique per clip. Clip UIDs are recorded by the host and replayed on replicas.
Control `edits.apply` runs through ops, and `tests/test_op.cpp` shows a replica fed only wire messages converging
byte-for-byte with the host through undo/redo. Finding: note keys are assigned deterministically per clip, so they
need no transport, but clip UIDs come from a process-wide allocator and must travel with the committed op.

---

## 4. Consistency, conflicts and undo

- **Object-level revisions** instead of one global revision. Every track, clip, device and note carries the sequence
  number of the op that last changed it. An intent includes the revisions it read. The host commits if the touched
  objects are unchanged, even if other parts of the song changed in the meantime. Two people editing different clips
  never conflict.
- **Fine-grained notes**: with stable note IDs, A editing notes 1–4 and B editing notes 7–9 of the same clip both
  succeed. Only an edit to the *same* note conflicts. The later edit is rejected with "Bob changed this note", the
  guest's view snaps to the committed state, and a toast explains why.
- **Soft locks (presence, not hard locks)**: opening a clip or device editor shows "Ana is editing" badges, so people
  naturally avoid collisions. The host can turn on **hard track locks** ("this track is mine") per track.
- **Optimistic gesture previews**: during a drag, the local UI shows the result immediately, and others get ephemeral
  ghost updates. Only the release sends an intent. If the host rejects it, the local preview rolls back.
- **Per-user undo**: Cmd+Z undoes only *your own* last op, and only if the objects it touched haven't been changed by
  someone else since. Otherwise it's refused with an explanation (same rule as the control API's guarded undo).
  Undo is sent as an op too ("revert op #n"), so all replicas stay identical.
- **Recording**: whoever records captures locally, and the finished take is committed as one op with its new audio
  asset. Recording locks only that person's armed tracks.

---

## 5. Project and asset sync

- **Join snapshot**: the host serializes the current model with the existing `AlsWriter` into memory, plus an **asset
  manifest** of every referenced sample/media file: SHA-256 hash, size, duration and project-relative path. The
  guest's per-user cache `~/Library/Application Support/Sonora/CollabCache/<hash>` is checked first, so a second join
  to the same project downloads almost nothing.
- **Transfer**: chunked (64 KiB), resumable and hash-verified, over a dedicated reliable data channel with
  backpressure, so big downloads don't delay ops or cursors. Progress appears in the status bar.
- **Missing asset at open**: the clip loads as a pending slot (existing ENG-8 async `SampleSlot` pattern) and becomes
  audible when the file arrives. Loading never fails (golden rule 3).
- **Mid-session sample drop**: the dropping client hashes the file on a worker and sends `clip.insertAudio {assetHash,
  meta, …}`. The host commits the op immediately, so everyone sees the clip at once, and pulls the bytes from the
  dropper. Peers fetch from the host (or directly from the dropper in a mesh). Files land in the project as
  `Samples/Collab/<name>-<hash8>.<ext>`, so a later Collect All and Save on the host already contains them.
- **Path safety**: peers only ever request assets *by hash from the current manifest*. No remote request can name a
  filesystem path. Incoming file names are sanitized, and files are only written inside the project/cache folders.
- **Late join / reconnect**: the host keeps the op log since the last snapshot. A reconnecting guest sends its last
  sequence number and gets the missing ops replayed. If the gap is too large, it gets a fresh snapshot.

---

## 6. Plugins

- **Compatibility report at join** compares `PluginInfo` (VST3 UID, name, vendor, version) against the guest's plugin
  cache.
  - *Same plugin and version*: normal.
  - *Version differs*: warning, state is still restored.
  - *Missing*: the guest gets a placeholder that **keeps the opaque state byte-for-byte**, so nothing is lost if the
    guest edits around it. That track plays silent for them, or plays the host's audio through listen-along
    (section 7). The device shows "Missing on Ana's machine" to everyone.
- **Parameter changes** (knob, MIDI-mapped CC or automation lane on a plugin or native device) travel on a separate
  **live-parameter channel**: coalesced to ~30 Hz, last writer wins, with the value applied through the same atomic
  path the UI uses. Gesture begin/end produces **one** history op containing the final value, so dragging a knob isn't
  hundreds of undo steps. This needs the mixer/native-parameter history that AGT-2 already lists.
- **Changes made inside a plugin's own GUI** (load a wavetable, edit inside Serum): VST3 reports some of them as
  parameter changes, but not all. On editor close and on a 2-second idle timer, Sonora captures the plugin state and
  hashes it. If the hash changed, it sends a `device.state.replace` op (bounded size, compressed). Receivers restore
  the state with the existing `Vst3PresetBuilder`/`setPreset` route. Suggested rule: one person at a time has a given
  plugin's GUI open in edit mode, and the others see "Ana is in Serum (track 3)".
- **Plugin trust**: restoring state received from a peer runs plugin code. That's acceptable between invited
  collaborators, but the size limits and the existing `outcome_unknown`/crash-isolation caveats still apply.

---

## 7. Transport, live MIDI and controllers

- **Clock sync**: an NTP-style ping exchange over the data channel estimates each guest's clock offset and round-trip
  time against the host (median of the last N, refreshed every few seconds).
- **Shared play**: Play is scheduled as "start at host time T + 150 ms, beat B". Each engine starts sample-exactly at
  that local time through the existing `Transport` request path, so starts agree within a few milliseconds. Seek, loop
  and tempo changes work the same way. Live sync drift is corrected by re-anchoring at loop wraps or every 8 bars.
- **Local audition toggle**: break out of shared transport to listen alone, then rejoin.
- **MIDI controllers**: each person keeps their **own** hardware and mappings, since controllers differ between
  machines. What syncs is the *result*: a mapped knob turn becomes a parameter change on the live-parameter channel, so
  the other side sees and hears the knob move. Sharing a mapping sidecar is optional ("use host's mappings").
- **Live playing**: notes played into an armed track are streamed as timestamped MIDI events. The receiver plays them
  through that track's instrument with a small fixed jitter buffer (~40–80 ms), so you hear your partner playing in
  near real time. True zero-latency joint performance over the internet is not possible. The design is honest about
  that latency and doesn't try to hide it.
- **Listen-along (optional)**: the host can stream its **Main output** as high-quality stereo Opus (128–256 kb/s) to a
  guest. That's for a guest who lacks a plugin, or who wants to hear exactly the host's master. The guest's local
  engine is muted while listening along.

---

## 8. Security

- **Transport**: WebRTC (ICE + DTLS 1.2/1.3 + SRTP) through **libdatachannel** (C++17, MPL-2.0, compatible with
  Sonora's AGPL). Media and data are encrypted end-to-end between peers. A TURN relay only forwards ciphertext.
- **Accounts and subscription (hosted relay only)**: the host signs in to the official relay, which checks the
  subscription when it creates an invite (section 15). Guests need no account beyond the invite. LAN and self-hosted
  relays need no account. Account identity is separate from the per-session invite secret and safety code.
- **Signalling**: a tiny rendezvous service exchanges connection offers. It never sees project data, voice or video.
  Each invite contains a random 128-bit secret that never reaches the server. Signalling messages are authenticated
  with it (HMAC), so a stranger who guesses the session ID can't join.
- **Man-in-the-middle check**: both apps derive a short safety code from the two DTLS certificate fingerprints and the
  invite secret. Users compare it once by voice. Until they confirm, the UI shows "unverified" (same idea as Signal or
  WhatsApp safety numbers).
- **Roles**: Host / Editor / Listener. The host can change roles, lock tracks, kick participants, and allow or deny
  "keep a copy". The host validates every intent: guests can't call file, export, plugin-scan or local-control
  operations.
- **Input hardening**: all network messages are bounded in size, nesting depth and rate, and validated on worker
  threads. Nothing from the network runs on the audio thread except preallocated ring reads.
- **Separation from local automation**: the local MCP Unix socket stays local and same-user only. Collab is a
  separate, explicit grant that ends when the session ends or the document changes.
- **Privacy**: camera and mic are off until the user turns them on. macOS camera/mic permission prompts and visible
  "on air" indicators are shown, and nothing is recorded.

---

## 9. Voice and video

**Voice**
- The mic comes from a chosen input of the *current* audio device. The audio callback copies those samples into a
  preallocated SPSC ring (wait-free, no allocation). A collab worker thread resamples to 48 kHz, applies a noise gate
  and AGC, encodes with **Opus** (BSD), and sends it on a WebRTC audio track.
- Remote voices are decoded on the worker into a per-participant jitter-buffered ring. The audio callback mixes them
  **after Main**, the same way the metronome and browser preview are mixed today. So voice is never in Main, exports,
  recordings or meters.
- Controls: per-person volume, mute, push-to-talk key, and "dip music while talking".
- Echo: Apple's voice-processing echo cancellation needs its own audio unit, which conflicts with a pro audio device.
  For the first version, **headphones are recommended** and we add a simple adaptive echo canceller later (COL-9).

**Video PiP**
- AVFoundation camera capture, VideoToolbox hardware H.264 encode at 320×240–640×480 and 15–30 fps, adaptive bitrate,
  over a WebRTC video track (`src/collab/media/*.mm`).
- Display: decoded `CVPixelBuffer` frames are uploaded as Skia images and drawn by a floating `PipWidget` in the root
  widget overlay. It stays inside the Skia toolkit (ADR 0004), works in `ui_snapshot` with a synthetic frame, and has
  no native-view stacking problems over Metal.
- Needs an `NSCameraUsageDescription` entry in the app's Info.plist.

**Recommendation**: voice and video are valuable, but people can use FaceTime or Discord on day one. Ship editing and
sync first (COL-1…6), then voice (COL-7), then video (COL-8).

---

## 10. Network budget (2 people)

| Channel | Reliability | Typical rate |
|---|---|---|
| Ops / intents | reliable, ordered | < 5 kb/s bursts |
| Presence (cursors, selections, ghosts) | unreliable, unordered, 20–30 Hz | ~5–10 kb/s |
| Live parameters / live MIDI | unreliable for values (latest wins), reliable for gesture end and notes | < 20 kb/s |
| Assets | reliable, separate channel, backpressured | as fast as the link allows |
| Voice | SRTP Opus | 24–32 kb/s per person |
| Video | SRTP H.264 | 300–800 kb/s per person |
| Listen-along (optional) | SRTP Opus stereo | 128–256 kb/s |

A star topology (everyone connects to the host) is fine for 2–4 people. More would need an SFU and is out of scope.

---

## 11. Code layout and threads

```
src/core/      Op.h/.cpp            serializable intents, opToCommand, stable-ID allocation (no JUCE GUI, no audio)
src/control/   (extended)           sequencer mode, object revisions, per-user undo, op log
src/collab/    CollabSession        roles, lifecycle, snapshot/join, reconnect
               Presence             ephemeral cursor/selection/ghost model (message thread)
               AssetSync            manifest, hashing worker, chunked transfer, cache
               ClockSync            offset/RTT estimation, scheduled transport
               transport/           libdatachannel wrapper, signalling client, invite/SAS crypto
               media/               Opus voice, VideoToolbox video (.mm), lock-free rings to/from the engine
src/ui/        views/CollabPanel    participants, roles, safety code, mic/cam controls
               PresenceOverlay      remote cursors/selections in Arrangement, Session, PianoRoll
               widgets/PipWidget    floating video tiles
src/app/       AppCollab.cpp        menus, wiring, document lifetime
packages/collab-relay/              small signalling server (Node), plus coturn config for TURN
tools/collab_probe                  headless host+guest pair for automated checks
```

Layering stays as ARCHITECTURE requires: `collab` depends on core/control/engine. Only `ui`/`app` depend on `collab`.
Threads:

| Thread | Collab work |
|---|---|
| Audio | mic → SPSC ring; remote voice/listen-along rings → mix after Main; scheduled transport via existing atomics. **Nothing else.** |
| Message | apply committed ops (existing `applyEdit`), presence model, UI |
| Collab network (libdatachannel) | sockets, DTLS, framing, validation, size/rate limits; posts to the message thread |
| Collab media worker | Opus encode/decode, resampling, jitter buffers, video encode/decode |
| Thread pool | hashing, snapshot serialization, asset file I/O |

---

## 12. Phases (proposed roadmap items)

| ID | Size | Scope | Done when |
|---|---|---|---|
| **COL-0** | S | ADR 0009 "Collaborative sessions", protocol spec (`docs/collab/PROTOCOL.md`), threat model | The decisions in section 13 are approved |
| **COL-1** | L | Serializable `Op` layer + stable IDs (track/clip/device/**note**), every UI edit routed through ops, host allocates IDs. Shares work with AGT-2/AGT-7 | Op-log replay gives a byte-identical `.als`, and all existing tests pass through the op path |
| **COL-2** | L | Collab core with an **in-memory transport**: sequencer, object revisions, conflict rejection, per-user undo, snapshot + op log, reconnect replay | Fuzz test: 2–3 simulated clients, random concurrent ops with latency/loss/reordering, replicas always end identical (model hash + writer output) |
| **COL-3** | M–L | Real transport: libdatachannel in `bootstrap.sh`, signalling relay, invites, HMAC, safety code, roles; **LAN mode via Bonjour, no server** | `collab_probe` host+guest over localhost and two Macs; wrong secret, tampered offers and oversized messages are rejected |
| **COL-4** | M | Asset/plugin sync: manifest, content cache, resumable transfer, compatibility report, mid-session sample drop, missing-plugin placeholder with preserved state | Join a real-world set on a second machine and get identical render (`plugin_probe --render` hash per track where plugins match) |
| **COL-5** | M | Presence UI: cursors, selections, ghosts, editing badges, follow mode, participant panel | `ui_snapshot` with synthetic participants, plus a two-machine manual check |
| **COL-6** | M–L | Clock sync + shared transport, live-parameter channel with gesture history, plugin-GUI state diff, live MIDI streaming | Measured start offset < 5 ms on LAN and < 20 ms on WAN; knob/CC moves mirror; GUI-made plugin change converges |
| **COL-7** | M | Voice: mic ring, Opus, post-Main mix, PTT, ducking; optional listen-along | Audio-thread allocation check passes; voice is absent from export/recording; 2-machine call test |
| **COL-8** | M | Camera PiP: capture, VideoToolbox, Skia tiles | Two-machine call; camera permission and indicator verified |
| **COL-10** | M–L | Sonora Cloud: accounts/sign-in on the hosted relay, host-only subscription check at invite creation, billing (payment provider), account UI in the app (sign in, plan status), self-host mode with no account | Unsubscribed hosts can't create hosted invites, guests join without paying, LAN/self-hosted sessions still work without an account, and a modified client can't bypass the check |
| **COL-9** | L | Hardening: host leaving/handoff, echo cancellation, text chat and comments pinned to beats, guest "keep a copy", rate limits, session audit log | Disconnect/kill tests on both sides keep the host project intact |

**Suggested MVP** = COL-0 … COL-5 plus the shared-transport part of COL-6: real co-editing with presence and synced
playback. Voice and video come after that.

---

## 13. Decisions (2026-10-16: all recommendations accepted)

1. **Connection infrastructure**: (a) LAN-only first (no server), (b) self-hosted signalling + TURN
   (a small VPS), or (c) a hosted TURN provider. Recommended: LAN mode in COL-3, then (b).
2. **Max participants**: 2 (simplest) or up to 4 (star topology, same design). Recommended: design for 4, test with 2.
3. **Edit model**: free editing with fine-grained conflicts and soft-lock badges (recommended), or Office-style
   exclusive locks per clip/track.
4. **Audio model**: local rendering on each side (recommended, requires matching plugins) plus optional listen-along
   streaming of the host's Main. OK?
5. **Copies**: may guests keep a copy of the project when the session ends? Default: host decides per session.
6. **Priority**: confirm voice/video come after co-editing (people can use FaceTime/Discord meanwhile).
7. **COL-1 overlap**: COL-1 also completes much of AGT-2/AGT-7 (full typed command coverage for MCP). OK to do it as
   one shared effort before any networking?

8. **Hosted relay** (decided later the same day): free protocol, LAN sessions and self-hosted relay; a paid hosted
   relay subscription ("Sonora Cloud") that only the host needs. Details in section 15 and ADR 0009.

## 14. Risks

- **COL-1 is a refactor of all 88 edit call sites.** It's the riskiest step, and replay tests guard it.
- **Plugin determinism**: two machines with the same plugin can still sound slightly different (random seeds, CPU
  features, versions). The UI promises "same project", not "bit-identical audio". Listen-along covers critical
  listening.
- **Plugin GUI state changes** that aren't reported as parameters are only caught by the state-hash polling.
  Large states (sample-based instruments) may take seconds to sync.
- **NAT traversal** fails without TURN on some networks (corporate or carrier NAT). That needs a relay for reliability.
- **Echo** without headphones until COL-9.
- Sonora versions must match on the protocol major version. Older peers are refused with a clear message.
- **Running the hosted service**: uptime, relay bandwidth, payments/taxes, terms and privacy policy, account
  security, and the AGPL obligations.

## 15. Hosted relay (Sonora Cloud)

| | Free | Sonora Cloud (subscription) |
|---|---|---|
| Collab feature in the app, open protocol | ✓ | ✓ |
| LAN sessions (Bonjour, no server) | ✓ | ✓ |
| Self-hosted relay (`packages/collab-relay`) | ✓ | ✓ |
| Official hosted signalling + TURN ("send a link, it works") | Join as a guest only | ✓ Host sessions |
| Session history / audit log, cloud backup and versions, shared sample cache | – | ✓ (optional extras) |
| Up to 4 participants, listen-along streaming | LAN/self-hosted only | ✓ |

- **Why a service, not an in-app gate**: Sonora is AGPL-3.0, so a check inside the client can be removed by anyone who
  rebuilds it. A server-side check on the hosted relay can't be bypassed.
- **Host-only subscription**: guests join hosted sessions free.
- **Enforcement point**: the relay checks the host's account entitlement when an invite/session is created and when
  TURN credentials are issued (short-lived, per session). Peers' encrypted media/data never pass through account
  systems.
- **Sign-in**: email magic link with an on-screen code. Implemented in
  `packages/collab-relay/auth.mjs`; per-device tokens, so other devices stay signed in.
- **Payments**: Stripe Checkout and Customer Portal (sales tax is the relay operator's responsibility). Implemented in
  `packages/collab-relay/billing.mjs`; protocol in `docs/collab/PROTOCOL.md`.
- **Work item**: COL-10 (section 12). It depends on COL-3's hosted relay and can ship after the free LAN MVP.
