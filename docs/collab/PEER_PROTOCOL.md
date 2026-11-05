# Collab peer protocol v1

Status: implemented, 2026-10-20 (`src/collab/`); chat and optional features 2026-11-04 (COL-9a); voice and camera 2026-11-05 (COL-7/8). The relay side is [PROTOCOL.md](PROTOCOL.md); decisions in
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
| 21 | policy | host → guests with chat | version, host features, allow chat / voice / video, (participant, features) for everyone (COL-9) |
| 22 | chat | guest → host → guests with chat | version, author, id, client id, host time, history flag, text (COL-9) |
| 23 | media-state | any → host → guests with voice | version, author, mic on, camera on (COL-7/8) |

## Features (COL-9)

The protocol number stays 3, so Sonora 0.1.9 and newer builds share sessions. A hello announces optional features as a
suffix of its app version text: `"0.1.10 +features=1"` (decimal bits: 1 chat, 2 voice when libopus is built in,
4 video on macOS). No released host reads the version text, and the suffix keeps it within its 64 bytes (the version is
shortened, never the tag). A hello without the suffix, or with one that is not a number, has no features.

Older decoders drop the link on an unknown message type, so messages 21 and later go only to peers that announced the
feature: the host relays chat only to guests with chat, and a guest sends chat only after the host's `policy` said the
host has it. A host that never sends `policy` is older: the guest's chat says so and the session goes on.

Extension messages (21 and later) start with a version byte (1 today; 0 is invalid). A later version only appends
fields: a reader decodes the fields it knows, skips the rest of the frame and passes the message on as its own
version. Inside its version a message is as strict as the others.

## Chat (COL-9)

Ops channel, so lines keep their order and are never lost on a live link. A guest sends `chat` with its own
`client id`; the host checks it and broadcasts it to every guest with chat, the sender included, stamped with the
author, the host's next `id` and the host's clock. The sender matches its line by client id and marks it delivered. A
refused line comes back to its sender only, with `id` 0 and the reason as text: chat is off, more than 20 lines in 10 s
from one person, or nothing left after cleaning.

Text is cleaned on both sides (`collab::cleanChatText`): valid UTF-8 only, no control characters except line breaks
(CR LF and CR become LF, tabs become spaces, at most two blank lines in a row), no bidirectional overrides (U+202A–202E,
U+2066–2069), trimmed, at most 1000 code points (4000 bytes on the wire). The host's `policy` says whether chat is
allowed; the host's switch is saved as the default for its next sessions. A joining guest gets the last 100 lines as
history (`history` set); a reconnecting guest skips history lines it already has (by id). Each replica keeps 500 lines.
Chat is never saved with the set and never logged (except by `SONORA_COLLAB_AUTOTEST` runs).

## Voice (COL-7)

Decisions (owner, 2026-11-05; [ADR 0011](../adr/0011-collab-voice-video.md)): WebRTC media over SRTP, libopus, voice on
every platform, voice allowed through TURN.

**Track.** The guest's first offer carries one audio m-line, `mid` "voice", sendrecv, Opus payload type 111, before the
data channels. The host takes it in `onTrack`. A host built without media (0.1.9) answers it as removed and the data
channels work as before; a guest without media offers none, and the host never sends it voice. `Transport::mediaReady`
says whether the link's track is open. Only RTP crosses it: no media handler, no RTCP of ours (libdatachannel drops
nothing; `decodeRtp` ignores RTCP).

**Packets.** RTP version 2, payload type 111, 20 ms mono Opus frames at 48 kHz (24 kbit/s, in-band FEC, 10 % expected
loss, complexity 5), timestamp +960 per frame, SSRC `0x534E5600 + author id` ("SNV" + the speaker). Media is
`Channel::Media` in `Transport`: whole packets, never fragmented, never on a data channel. The host accepts a packet from
a guest only when its SSRC is that guest's own, voice is allowed and the guest announced voice, and forwards it unchanged
to every other guest with voice (it is a star: guests never reach each other). Each receiver keeps one jitter buffer and
decoder per SSRC: playing starts at 60 ms buffered, a missing frame is rebuilt from the next packet's FEC or concealed,
an empty buffer conceals without moving on (the delay grows a frame), more than 300 ms queued skips ahead, 200 ms of
silence ends the talk spurt. Late packets are dropped.

**State.** `media-state` carries each person's own switches; the host stamps the author, relays it to guests with voice
and sends everyone's to a joining guest; a reconnecting guest repeats its own after the host's `policy`. The host's
`policy.voice` off stops everything: guests cannot send and the host forwards nothing. Muting someone is local: that
machine stops decoding them.

**Audio.** `engine::VoiceTap` on the live device callback, after `renderBlock`: it copies the first active input (or the
chosen one) into a 2.7 s SPSC ring while the mic is sent, and adds the decoded mix to outputs 1-2 after Main, ducking the
music to -6 dB while a voice is heard (300 ms hold, 50 ms glide; optional). Offline renders never call it: voice is
never exported, frozen, bounced or recorded. `collab::VoiceChannel`'s worker (5 ms) resamples to and from 48 kHz,
encodes and decodes; the message thread only moves packets (every 10 ms). Push to talk: hold the backquote key.

## Camera (COL-8)

macOS only (AVFoundation capture, VideoToolbox H.264); `FeatureVideo` (value 4) is announced only there.

**Track.** Since COL-8 the guest's first offer has two m-lines: "voice" and "video" (sendrecv, H.264 payload type 96,
packetization mode 1). With two tracks libdatachannel routes packets by SSRC, so both list every possible speaker's SSRC
(`voice`/`video` base + 0..15) in the offer; the host's answer inherits them.

**Packets.** H.264 constrained baseline, 480x360, 15 fps, ~350 kbit/s, a keyframe (with SPS/PPS) every 2 s, no B-frames.
RTP timestamp 90 kHz, SSRC `0x534E5800 + author`, RFC 6184 single NAL units up to 1200 bytes and FU-A fragments above
(no STAP-A); the marker ends the access unit. A receiver drops a frame with a sequence gap or no marker and then waits
for the next keyframe; it decodes at most six queued frames (older ones go, and it waits for a keyframe again).

**TURN.** Video never crosses a relayed link (owner decision until COL-10): `Transport::relayed` is true when the
selected ICE pair has a relayed candidate on either side. A guest on such a link can't send or receive video ("Camera
needs a direct connection"); the host forwards a guest's video only to guests on direct links and drops video arriving
over a relayed one. Voice is not affected.

**State and UI.** `media-state` carries the camera switch too (it now goes to peers with voice or video). Tiles appear
for people whose camera is on and whose frames arrived in the last 3 s, as picture-in-picture over the main view's top
right (this person's preview first, mirrored); the x hides them on this machine. The host's `policy.video` off stops
every camera.

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

Chat lines ≤ 1000 characters, 20 per person per 10 s; names ≤ 64 characters; 11 authors per session lifetime (clip UID ranges); 256 queued asset requests per guest; uploads
and assets ≤ 4 GiB; the host keeps 2000 commits or 512 MiB for reconnect replay and undo (100 undo steps per person).
