# ADR 0007 — Shared application control

Status: accepted for the first implementation slice, 2026-10-15.

Native workflows and external agents call `sonora_control`, a headless-capable layer above the engine and below the app.
The app supplies message-thread adapters for model history, live state and plugin preparation. The control API never
calls widgets or the audio callback directly. Mutations require a document/revision target and an idempotency key.

The first slice supports inspection, tempo/gain/clip-duplication batches, guarded undo, hosted parameter inspection and
one qualified Serum composition recipe. It stages pure-model edits, then commits through the existing history. Plugin
preparation occurs before song mutation; filesystem export, arbitrary plugin state editing and general job transactions
are outside this first contract. Existing tolerant project loading is retained.

By design, reasoning comes from external MCP clients first. The native prompt contains
explicit built-in workflows; it does not pretend to be a general model conversation. No provider credential is needed.
The MCP bridge uses the official SDK over stdio and an opt-in, document-scoped Unix socket into the running app.

The initial permission is an owner-only, same-user local automation grant for the current project. It is revoked on
project switch or quit. Per-client pairing, per-operation scopes, remote transport, durable jobs and full command coverage
remain roadmap work. Protocol discovery/negotiation belongs to the SDK; application identity remains in Sonora.

See `../agent/CONTROL_API.md` for the implemented contract and `../AGENT_DESIGN.md` for the broader plan.

## General editing extension (2026-10-16)

Note operations reuse existing ClipLocation/setNotes history for Arrangement, Session and takes. Queries return
revision-scoped array indices; writes preserve existing note IDs except explicit whole-array replacement.
Generic VST3 writes are separate from pure-model batches: capture live opaque state, edit an isolated exact plugin,
reopen/verify that state, then use device replacement history with a fresh device ID. This deliberately favors
restorable patch edits over realtime gestures. The receipt exposes processor rebuild and replacement identity;
authored automation conflicts are rejected. See CONTROL_API.md for bounds, fields and unexposed controls.

## Native controls and device lifecycle (2026-10-17)

AGT-7c reuses that preparation/history boundary for native/rack parameters, plus catalogue, insertion and removal.
`DeviceControl` binds the same adapters in the app and probes. Registered descriptors provide exact paths/names,
processor plain values/ranges/choices and explicitly linear normalization. No per-device agent shim is required.
Native factory/model round-trip and actual live readback are verified; guarded undo compares native values/content
and child state as well as hosted opaque state. Automated bases are preserved; target automation and rack descendant
automation reject conflicting writes. Every replacement returns its new processor identity.

Catalogue identities come from implemented native entries and the scan cache. Ordered insertion never silently replaces
an instrument; removal retains captured live state through undo/save. Media/content authoring and nested targeting remain
separate future APIs. MCP and the subsequently requested optional configured native LLM use these same operations;
the original built-in parser remains a probe helper. See `../agent/NATIVE_LLM.md` and CONTROL_API for current scope.
