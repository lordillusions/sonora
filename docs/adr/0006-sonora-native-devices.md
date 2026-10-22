# ADR 0006 — Independent Sonora devices and explicit state identities

Status: accepted, 2026-10-14 (DEV-13–18, PLG-9).

Sonora needs native FM synthesis, multisampling, drum/instrument racks and an LFOTool-style
effect, plus vendor/category organization in the browser. Similar musical functions do not imply that
another product's algorithm enums, parameter units or saved state are interchangeable.

Sonora FM, Sonora LFO and Sonora Velocity use independent DSP and the explicit `SonoraFm`, `SonoraLfo`
and `SonoraVelocity` XML tags. Their parameters and curve points save/reload in Sonora. These extensions
are not accepted as native devices by other hosts. We do not label a Sonora FM patch as Operator or pass
its eight-algorithm enum to an Operator loader. Operator import requires a separate tested adapter
(DEV-13b); the existing unsupported-instrument fallback remains until that adapter exists.

The shared sampling engine already models the documented `MultiSampler` sample map, pitch, filter and
amplitude paths, so Sonora Sampler uses those existing format paths. Instrument/Drum/Audio Effect Racks
use the existing documented rack model/processor. Authoring these containers is tested through Sonora
save/reload; reference-app acceptance of the new output remains a separate check.

Device structure edits operate on synchronized model copies and immutable prepared processors. A new
top-level device ID rebuilds the edited processor while retaining the rest of the track. Undo keeps the
old ID and state. Zone and branch XML fragments retain unparsed metadata when authored containers change.
Message-thread state capture traverses the same per-child pointer mapping used by the rack editor,
including null pointers for unsupported devices. Audio callbacks never inspect the editable model.

Browser roles/categories come from the core device catalog. Hosted entries retain their scanned plugin
identifier, vendor and format; grouping/search never use the label as plugin identity. Browsing reads the
scanner cache and does not instantiate plugins. AU insertion/state authoring remains UI-7b/PLG-4.

Addendum (DEV-16b): Sonora Curve LFO (`SonoraCurveLfo`) implements the feature set the LFOTool manual documents
(graphs, routing, filter module, MIDI behaviours, CC output) as independent code and data. No behaviour was taken from
LFOTool's code, graphics or presets; numeric curves and units are our own and not
claimed to match. Original `.fxp` presets are not imported or copied (DEV-16c).

Addendum (DEV-18): `SonoraWavetable` is an independent native instrument informed by the reference's
features, state layout and parameter scaling. Generated waves and oscillator/filter/warp/modulation DSP are Sonora's own.
The bounded native Serum state decoder preserves sparse/unknown fields and original bytes; decoding does
not authorize automatic replacement or imply a compatible native patch. Real-world patches still need
unsupported synthesis/effect/modulation features. A separate tested adapter and controlled audio comparisons
are required before compatibility is claimed (DEV-18d).

Feature expansion and comparisons stay explicit in DEV-13b/14b/15b/16b/18d. No proprietary implementation,
preset, sample or graphic is included. Public functional references:
[instrument manual](https://www.ableton.com/en/manual/live-instrument-reference/),
[rack manual](https://www.ableton.com/en/manual/instrument-drum-and-effect-racks/),
[LFOTool product description](https://www.xferrecords.com/products/lfo-tool).
