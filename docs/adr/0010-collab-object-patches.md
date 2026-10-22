# ADR 0010 — Collab edits as replicated object patches

Status: accepted, 2026-10-20. Refines ADR 0009 (host-authoritative operations). Implementation: `src/collab/`,
`src/core/ModelCodec.*`; protocol in [`../collab/PEER_PROTOCOL.md`](../collab/PEER_PROTOCOL.md).

## Context

ADR 0009 and COLLAB_DESIGN section 3 planned to route each of the ~88 UI `applyEdit` call sites through a typed
`model::Op` (COL-1) before any networking. After the first slice (12 op types) that plan meant rewriting every edit
path, recording, media edits and device editing before two people could share a set, and every new edit added later
would need its own op, codec and replay test or it would silently not sync.

## Decision

A collab session replicates the Song as **keyed objects** and shares **object states**, not per-edit intents:

- Objects: `song` header, `tracks` structure, and per track `t/<id>` header, `d/<id>` device chain, `o/<id>` clip order,
  plus one `c/<uid>` per Arrangement or Session clip. Addressing is by stable identity only.
- `core/ModelCodec` encodes every Model.h field exactly (doubles bit-exact). Its *identity form* leaves out live values
  (mixer levels, device parameter values, bypass, opaque plugin state), which a separate live channel syncs.
- Every local edit still runs through the existing `applyEdit` / `EditCommand` path. Afterwards the replica compares the
  identity form of every object with its cache and shares the changed objects' full form. Any edit, including ones
  written later, syncs without extra code.
- The host stays authoritative (ADR 0009): its Sequencer commits object patches in order, with a revision per object.
  A guest's patch commits only when the objects it changed are unchanged since it read them; otherwise it is rejected
  and the guest restores the confirmed state. Two people editing different clips or tracks never conflict.
- Undo is per person: the inverse patch of that person's latest commit, refused when someone changed those objects since.
- Guest patches are validated (decodable, consistent structure, finite values, bounded sizes) and may only reference files
  already in the session (path allow-list), so a peer can't make the host read arbitrary files.
- Clip UIDs come from per-participant ranges; song-level allocators (device and pointee ids, track ids) live in the
  song header/structure objects, so concurrent allocations conflict instead of colliding.

Typed ops (COL-1) remain for the control API and MCP, where intent matters; they are no longer a prerequisite for collab.

## Consequences

- Conflict granularity is the object: notes of one clip edited concurrently conflict (ADR 0009 hoped for per-note
  merges). In practice edits commit within one round trip, so this is rare; per-note merging can be added for clip
  objects later.
- Each edit costs a scan of the whole song's identity forms. The first version built every identity string per edit:
  29.1 ms Debug / 13.7 ms Release on a 47-track set (1058 objects). Since COL-2b the scan feeds the same visit() calls
  into a 64-bit hash (`model::HashWriter`, no allocation) and full forms are built only for changed objects: 6.9 ms Debug
  / 1.9 ms Release on that set, 11.7 -> 7.2 ms / 4.5 -> 1.4 ms on a 4011-object set (`collab_probe ... --edits 0`,
  median of 21 scans). The cost is now the values themselves (long automation envelopes dominate). A 64-bit collision
  would hide one edit (probability ~2^-64 per changed object); each hash step is a bijection, so a single changed value
  is always detected.
- `ModelCodec.cpp` pins struct sizes on macOS arm64, so adding a Model.h field without updating the codec fails to
  compile there.
- Live values that change without an edit (a fader move, a plugin knob) travel on the live channel; a structural edit
  never resets them on peers (live values of surviving devices are merged from the receiver).
