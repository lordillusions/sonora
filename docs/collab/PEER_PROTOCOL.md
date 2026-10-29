# Collab peer protocol v1

Status: implemented, 2026-10-20 (`src/collab/`). The relay side is [PROTOCOL.md](PROTOCOL.md); decisions in
[ADR 0009](../adr/0009-collab-sessions.md) and [ADR 0010](../adr/0010-collab-object-patches.md). How to try it:
[TESTING.md](TESTING.md).

## Topology and links

A session is a star: the host plus up to three guests (`Options::maxParticipants`, default 4). Each guest has one
WebRTC peer connection to the host (libdatachannel, DTLS). Each connection has three data channels:

| Channel | Label | Delivery | Carries |
|---|---|---|---|
| Ops | `ops` | reliable, ordered | hello/welcome, snapshot, intents, commits, rejects, undo, participants, transport, plugin state |
| Live | `live` | unordered, no retransmits | presence, live values, clock pings |
| Assets | `assets` | reliable, ordered | sample requests and chunks, guest uploads |

Ops and Assets messages are framed: first byte 0 = whole message (≤ 16 KiB with the byte), 1 = first part (+ u32 total),
2 = middle, 3 = last; at most 512 MiB per message. Live messages are never fragmented (larger ones are dropped). Frames
stay at 16 KiB (COL-3c): with larger SCTP messages a busy channel starved the others in libdatachannel/usrsctp (an edit
waited until every queued sample chunk had arrived; measured with `collab_probe --bench`, 20 KiB starves, 16 KiB doesn't).
Senders keep at most 64 KiB of assets in the data channel's own queue, which is first in, first out across channels.

## Meeting

**Relay** (hosted, self-hosted or a local open-mode relay): the host `POST /v1/sessions` with the SHA-256 of a random
ticket, then joins the relay's WebSocket as host. The invite is
`sonora://join/<sessionId>?relay=<origin>&ticket=<ticket>#<secret>`; the secret stays in the fragment and never reaches
the relay. Guests join the relay's WebSocket with the ticket and send the WebRTC offer to the host's relay peer id. TURN
servers come from the relay's `welcome` (STUN on the same host is added). Before closing with a 4xxx code the relay sends
a `closing` frame (libdatachannel hides close codes).

**Local network**: the host listens on TCP 47100 (or any free port) for WebSocket signalling and advertises
`_sonora-collab._tcp` with Bonjour (TXT `name`, `session`). The invite carries `lan=<ip:port>` instead of `relay`. A guest
without an invite can "Ask to Join" from the list; the host sees a join request and allows or denies it. Approved
requests use an empty secret, so for them the safety code is what proves the link.

**Signalling messages** `{"type":"signal","to"?,"data":{"p":<payload JSON>,"t":<tag>}}`, payload kinds `offer`, `answer`,
`candidate`. `t = base64url(HMAC-SHA256(secret, "sonora-signal-v1|" + len:sessionId + len:from + len:to + len:p))`, from/to
being relay peer ids (relay) or `guest`/`host` (LAN). Unauthenticated payloads are ignored; more than five close the link.

**Safety code**: four words from `HMAC-SHA256(secret, "sonora-safety-v1|" + both DTLS fingerprints, sorted)`, one byte per
word from a fixed 256-word list. People compare it once by voice.

A guest that isn't connected 30 s after starting gives up (`SONORA_COLLAB_CONNECT_TIMEOUT_MS` overrides it).

## Messages (Ops unless noted)

Binary, one type byte then fields (`core/ByteCodec.h`: varints, zigzag, little-endian exact doubles). Strict decoding:
unknown type, truncation, trailing bytes or oversize fields drop the peer.

**Packed** (protocol 2, COL-3c): a mode byte, then the fields as one byte string. Mode 0 = plain (bodies under 4 KiB,
most edits); 1 = zlib-deflated (level 1), followed by the inflated size; 2 = deduplicated, then deflated, followed by
the deduplicated size and the body size. Inflating or rebuilding must give exactly the stated size (≤ 512 MiB), or the
peer is dropped.

Mode 2 (bodies of 64 KiB and more): the body is cut into content-defined chunks (gear rolling hash, 4–64 KiB, ~20 KiB
on average) and written as entries, each a chunk's bytes or the 64-bit key of a chunk that already crossed this link in
either direction. Each side keeps, per link, the keys it sent and received (at most 256 MiB of chunks per direction,
first come; no eviction) and one byte pool per session. Both ends make the same decisions because each direction sees
the same chunks in the same order on Ops. A new link (reconnect) starts empty. Why: a track header or clip carries the
source XML it was loaded from (megabytes with plugin state), and every rename or colour change used to send all of it.
The 47-track test set: snapshot body 119 MB plain, 25 MB deflated, 16.7 MB deduplicated (+ the 8 MB .als, which is gzip
already and goes as it is; 1.3 s on the host); 51 random edits sent 220 MB as references.

| # | Message | Direction | Content |
|---|---|---|---|
| 0 | hello | guest → host | protocol 2, app version, name, rejoin author + token, last seq |
| 1 | welcome | host → guest | your author id, role, participants, clip UID base, rejoin token, snapshot follows? |
| 2 | snapshot | host → guest | seq, packed {every object (full form), revisions, document binding, asset catalog, transport}, set name, the host's saved .als bytes |
| 3 | commit | host → all | seq, author, intent id (0: host/undo/redo), kind (edit/undo/redo), label, packed {object states} |
| 4 | intent | guest → host | intent id, label, base revision per object (or "after my intent N"), packed {object states} |
| 5 | reject | host → author | intent id, code (`conflict`, `invalid`, `not_allowed`, `depends_on_rejected`), message, who changed it first |
| 6 | undo | guest → host | redo flag |
| 7 | participants | host → all | id, name, role (Host/Editor/Listener), colour, connected |
| 8 | presence (Live) | any → host → others | view, track id, beat, pitch, open clip, selected clip/tracks/time range |
| 9 | live (Live) | any → host → others | (track id, device id or -1, target, value): mixer volume/pan/active/solo/sends, device parameter, bypass |
| 10 | plugin-state | any → host → others | track id, device id, VST3 component and controller state |
| 11 | transport | guest → host, host → all | play/stop/seek, beat, host clock time to act at |
| 12 | clock (Live) | guest ↔ host | ping (guest time) / pong (guest time, host time) |
| 13 | asset-request (Assets) | guest → host | hash, offset |
| 14 | asset-chunk (Assets) | host → guest | hash, offset, total, ≤ 1 MiB bytes, or "missing" |
| 15 | asset-add | host → all | catalog entries for samples new since the snapshot |
| 16 | upload (Assets) | guest → host | upload id, file name, offset, total, bytes |
| 17 | drop-request (Assets) | guest → host | upload id, track id, beat, scene |
| 18 | role | — | reserved (roles travel in participants) |
| 19 | notice | host → guest | text for the guest's status line |
| 20 | bye | any | reason; the receiver ends the session (no reconnect) |

## Edits

Objects and the commit/reject/rebase rules are in ADR 0010 and `src/collab/Session.h`. A guest applies its own edit at
once and keeps it pending. A commit from someone else that touches a pending object makes that intent (and later ones
building on it) impossible, so the guest rolls them back immediately; the host's reject arrives afterwards and is ignored.
Receivers keep their own live values for devices and mixers that already exist (`mergeLiveValues`).

Host-side checks on every intent: author is not a Listener; one base revision per object; revisions match; objects
decode; structure is consistent (unique track ids, each placed clip exists once on a fitting track, known groups);
values finite and in range (tempo 10–999, signature, loop); bounded counts; every referenced file path already in the
song. The host re-encodes committed objects from its own song (the canonical form).

## Samples (COL-4)

The host hashes every sample its song references (SHA-256, worker thread) before sending snapshots, and new references
after each commit (`asset-add`). Guests fetch missing hashes into `<app data>/CollabCache/<hash><ext>` (up to eight
downloads at once, 64 KiB chunks, 2 MiB of requests in flight in all: enough for a ~400 ms path), verify the hash, then reload the clips and sampler zones that use them. A
guest resolves sample references only through the catalog (`io::setSamplePathOverride`), never through the host's paths.
A guest's file drop uploads the file; the host stores it as `<project>/Samples/Collab/<name>` and performs the drop,
attributed to the guest.

## Transport (COL-6)

Guests ping the host every 2 s (every 200 ms for the first six) and use the offset of the lowest-latency of the last
eight samples. Play and seek are scheduled 150 ms ahead in host time; Stop is immediate. A guest can turn shared
transport off to audition alone. A guest joining while the host plays asks for the current position and starts with it.

## Limits

Names ≤ 64 characters; 11 authors per session lifetime (clip UID ranges); 256 queued asset requests per guest; uploads
and assets ≤ 4 GiB; the host keeps 2000 commits or 512 MiB for reconnect replay and undo (100 undo steps per person).
