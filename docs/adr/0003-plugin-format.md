# ADR 0003 — VST3 first

**Status:** accepted (2026-10-13)

## Decision
VST3 is the supported third-party format: SDK under MIT since 3.8, cross-platform, and Live stores
raw VST3 state in `.als` that we can restore exactly (verified). AU hosting is compiled in (free on
macOS) but secondary. VST2 is disabled (`JUCE_PLUGINHOST_VST=0`): the SDK can no longer be licensed.

## Consequences
The real-world test sets use VST2 in 110 places. Those load the VST3 build of the same plugin with **default**
settings until PLG-5 (state migration) is solved per plugin.
