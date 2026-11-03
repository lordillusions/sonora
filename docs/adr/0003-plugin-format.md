# ADR 0003 — VST3 first

**Status:** accepted (2026-10-13)

## Decision
VST3 is the supported third-party format: SDK under MIT since 3.8, cross-platform, and Live stores
raw VST3 state in `.als` that we can restore exactly (verified). AU hosting is compiled in (free on
macOS) but secondary. VST2 is disabled (`JUCE_PLUGINHOST_VST=0`): the SDK can no longer be licensed.

## Consequences
The real-world test sets use VST2 in 110 places. Those load the VST3 build of the same plugin with **default**
settings until PLG-5 (state migration) is solved per plugin.

## Update 2026-11-04 (PLG-15): VST2 and Audio Units hosted too
VST2 and AU support were requested. VST3 stays the first choice for new work, but:
- **VST2** is hosted through **FST** ("Free Studio Technologies", git.iem.at/zmoelnig/FST, GPL-3.0-or-later, compatible
  with Sonora's AGPL-3.0): headers written without Steinberg's SDK. `scripts/bootstrap.sh` fetches a pinned commit into
  `third_party/fst` and applies `src/plugins/vst2/fst-host-layout.patch` (three structs get the sizes hosts allocate and
  plug-ins clear; FST's flexible arrays made JUCE overrun the heap and the stack). `src/plugins/vst2/pluginterfaces/vst2.x/`
  maps the include path JUCE expects onto FST. Without the patched headers CMake builds with `JUCE_PLUGINHOST_VST=0`.
  Steinberg's SDK is still never used. "VST" is Steinberg's trademark: product text says "VST2 plug-ins" only to name
  what is loaded.
- **Audio Units** (macOS) load with Live's `<AuPreset><Buffer>` (the ClassInfo XML plist) and save it back in that form.
- A set's VST2 reference prefers the installed VST2 (full state); the VST3 build with Live's parameter values (PLG-5) is
  the fallback.
