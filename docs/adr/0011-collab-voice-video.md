# ADR 0011 — Voice and camera in collab sessions

Status: accepted 2026-11-05 (owner answers in the COL-9a/COL-7 session). Extends ADR 0009.

## Context

Collab sessions carry edits, samples, presence and the transport over three WebRTC data channels per link (star around
the host). The owner asked for chat, a mic conversation and optionally video, each switchable in the sharing settings.
Chat fits the Ops data channel (COL-9a). Voice and video need a codec, a transport that does not wait behind sample
downloads, and a decision on what may cost relay bandwidth.

## Decisions

1. **Media tracks over SRTP, not data channels.** libdatachannel's media support is turned on (libsrtp from its own
   `deps/`, on the same OpenSSL). Each link gets one audio track in the guest's first offer. RTP has its own queue: the
   COL-3c measurements showed that SCTP's shared queue lets a busy channel starve the others, so voice on a data channel
   would stall during sample transfers. Older peers built without media answer the track as removed and keep working.
2. **libopus from `scripts/bootstrap.sh`** (pinned 1.5.2 tarball, SHA-256 checked, static), the same codec on every OS,
   not Apple's AudioToolbox Opus.
3. **Voice on macOS, Windows and Linux; camera on macOS only** (AVFoundation + VideoToolbox), drawn disabled elsewhere.
   The mic is an input of the app's JUCE audio device, so capture is portable.
4. **TURN:** voice may go through the hosted relay's TURN (about 24 kbit/s per person and direction). Video is refused
   over TURN until COL-10 accounts and billing exist; it says why.
5. Optional features are announced in the hello (`+features=` suffix of the version text) so the wire protocol stays 3
   and 0.1.9 peers keep joining; feature messages go only to peers that announced them.
6. Voice is mixed after Main on the live callback only: never exported, frozen, bounced or recorded.

## Consequences

- One more third-party library (BSD-3) whose licence ships with the app; libsrtp (BSD-3) joins libdatachannel's.
- The host forwards every guest's voice to the others (N-1 upstreams per guest at most 3), no mixing on the host.
- Encoding, decoding and resampling run on a worker thread; packets move on the message thread every 10 ms, so a stalled
  UI thread longer than the 60 ms jitter buffer is heard as a gap (COL-7b: move media off the message thread).
