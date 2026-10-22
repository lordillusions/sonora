# ADR 0001 — C++20 + JUCE 8 for audio, MIDI and plugin hosting

**Status:** accepted (2026-10-13)

## Context
Needs: CoreAudio I/O, CoreMIDI, hosting VST3 (and AU) with editor windows, a real-time engine.
Writing those directly is months of work; Live itself is C++.

## Decision
C++20, CMake + Ninja, **JUCE 8.0.16** (pinned) for audio devices, MIDI, plugin hosting, windows and
event loop. JUCE modules are built once into `sonora_juce`.

## Consequences
- JUCE is **AGPLv3** (or commercial). Sonora is therefore AGPL-3.0 if distributed.
- JUCE's GUI classes are used only for windows/menus/dialogs and plugin editors; the main UI is Skia (ADR 0002).
- Upgrading JUCE: change the tag in `scripts/bootstrap.sh`, rebuild, run all verification tools.
