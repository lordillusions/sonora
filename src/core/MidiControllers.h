#pragma once
// MIDI Ctrl envelope targets (ENG-9d), verified with own Live 12 controller envelopes.
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace sonora::model
{
inline constexpr int midiControllerTargetCount = 131;
inline bool isMidiController(int index) { return index >= 0 && index < 130; }
inline int midiControllerIndex(const uint8_t* data, int size)
{
    if (size >= 3 && (data[0] & 0xf0) == 0xb0) return (data[1] & 0x7f) + 2;
    if (size >= 3 && (data[0] & 0xf0) == 0xe0) return 0;
    if (size >= 2 && (data[0] & 0xf0) == 0xd0) return 1;
    return -1;
}
inline int midiControllerValue(int index, const uint8_t* data)
{
    if (index == 0) return (data[1] & 0x7f) + ((data[2] & 0x7f) << 7) - 8192;
    return data[index == 1 ? 1 : 2] & 0x7f;
}
inline int midiControllerDefault(int index)
{
    if (index == 9) return 100; // CC 7 volume
    if (index == 12) return 64; // CC 10 pan
    if (index == 13) return 127; // CC 11 expression
    return 0;
}
inline int quantiseMidiController(int index, double value)
{
    if (! std::isfinite(value)) return midiControllerDefault(index);
    return (int) std::lround(std::clamp(value, index == 0 ? -8192.0 : 0.0, index == 0 ? 8191.0 : 127.0));
}
} // namespace sonora::model
