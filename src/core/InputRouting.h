#pragma once
// Input routing codec (ENG-9b). Parsing/strings run on the message thread; packed decoding is realtime safe.
#include "core/Model.h"
#include <cstdint>

namespace sonora::model
{
enum class InputKind : uint8_t { Unset, AudioMono, AudioStereo, Midi, None, Resampling, Unsupported };
struct InputSelection
{
    InputKind kind = InputKind::Unsupported;
    int channel = 0; // physical audio channel (zero based); MIDI 0 all, 1..16
    std::string device; // MIDI persistent port identifier; empty means All Ins
};
InputSelection parseInput(const Track& track);
uint32_t midiPortToken(const std::string& identifier);
uint64_t packInput(const InputSelection& input);
inline InputKind inputKind(uint64_t code) { return (InputKind) (code & 0xff); }
inline int inputChannel(uint64_t code) { return (int) ((code >> 8) & 0xffff); }
inline uint32_t inputMidiPort(uint64_t code) { return (uint32_t) (code >> 32); }
void setInputSelection(Track& track, const InputSelection& input); // sets target + display strings, marks edited
} // namespace sonora::model
