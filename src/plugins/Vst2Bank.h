#pragma once
// PLG-15: converts between the VST2 state Live stores in <VstPreset> and the FXB bank JUCE's VST2 host reads and writes
// (AudioPluginInstance::setStateInformation / getStateInformation).
//
// Live's <VstPreset>: Type (a four-char code), ProgramCount, ParameterCount, ProgramNumber, PluginVersion, Buffer:
//   'FBCh' / 'FPCh': Buffer = the plug-in's own chunk (effGetChunk), untouched.
//   'FxBk' / 'FxCk': Buffer = ProgramCount programs of { char name[28]; float params[ParameterCount] (little-endian) }.
// FXB (big-endian ints and floats): "CcnK" | byteSize | fxMagic | version | fxID | fxVersion | numPrograms | ...
//   FBCh: future[128] | chunkSize | chunk.   FPCh: name[28] | chunkSize | chunk.
//   FxBk: future[128] | numPrograms * FxCk program ("CcnK" | byteSize | "FxCk" | version | fxID | fxVersion | numParams |
//   name[28] | params).   FxCk alone: one such program.

#include "core/Model.h"

#include <cstdint>
#include <vector>

namespace sonora::plugins
{

constexpr uint32_t fourCC(const char (&s)[5])
{
    return (uint32_t) (uint8_t) s[0] << 24 | (uint32_t) (uint8_t) s[1] << 16 | (uint32_t) (uint8_t) s[2] << 8 | (uint8_t) s[3];
}

// The FXB for `info` (vst2PresetType/counts/state) and the plug-in's VST2 unique id. Empty when there is nothing to load
// or the stored layout does not add up.
std::vector<uint8_t> buildVst2Bank(const model::PluginInfo& info, int32_t uniqueId);

// Reads an FXB written by JUCE back into Live's layout. Leaves `info` unchanged and returns false on anything malformed.
// Sets vst2PresetType, vst2ProgramCount, vst2PluginVersion and state; vst2ParameterCount only for parameter banks.
bool readVst2Bank(const std::vector<uint8_t>& fxb, model::PluginInfo& info);

} // namespace sonora::plugins
