# ADR 0005 — MIDI controllers via MIDI learn, not Live's control-surface scripts

**Status:** accepted (2026-10-13)

Live's controller support is 149 proprietary Python packages running against the Live Object Model.
Re-implementing that API is months of work and the scripts can't be redistributed. Sonora supports
controllers through: MIDI input to armed tracks, MIDI learn (CC → mixer/transport/device parameter),
and later importing the MIDI mappings Live saves in `.als` (MIDI-3). A generic Push/APC-style
"surface" layer can be designed later on top of the same `ControlTarget` mechanism.
