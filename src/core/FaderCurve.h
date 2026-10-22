#pragma once
// Live's volume fader law: position 0..1 <-> linear gain (+6 dB at the top, -inf at the bottom).
// Shared by the fader widget (drawing / dragging) and MIDI control (CC -> volume).
//
// The law is piecewise linear in dB. Breakpoints were read from the 0/12/24/36/48/60 scale of a
// running Live mixer (positions as a fraction of the meter height); below the first one the curve
// runs to silence at -90 dB.

#include <algorithm>
#include <cmath>

namespace sonora::model
{

inline constexpr float kFaderMaxGain = 1.99526f; // +6 dB

struct FaderPoint
{
    float pos;
    float db;
};

inline constexpr FaderPoint kFaderTable[] = {
    {0.0f, -90.0f}, {0.075f, -60.0f}, {0.19f, -48.0f}, {0.33f, -36.0f},
    {0.51f, -24.0f}, {0.70f, -12.0f}, {0.893f, 0.0f}, {1.0f, 6.0f},
};
inline constexpr int kFaderTableSize = (int) (sizeof(kFaderTable) / sizeof(kFaderTable[0]));

inline float faderGainToPosition(float gain)
{
    if (gain <= 0.000317f)
        return 0.0f;
    const float db = 20.0f * std::log10(gain);
    if (db <= kFaderTable[0].db)
        return 0.0f;
    if (db >= kFaderTable[kFaderTableSize - 1].db)
        return 1.0f;
    for (int i = 1; i < kFaderTableSize; ++i)
        if (db <= kFaderTable[i].db)
        {
            const auto& a = kFaderTable[i - 1];
            const auto& b = kFaderTable[i];
            return a.pos + (b.pos - a.pos) * (db - a.db) / (b.db - a.db);
        }
    return 1.0f;
}

inline float faderPositionToGain(float pos)
{
    pos = std::clamp(pos, 0.0f, 1.0f);
    if (pos <= 0.0f)
        return 0.0f;
    for (int i = 1; i < kFaderTableSize; ++i)
        if (pos <= kFaderTable[i].pos)
        {
            const auto& a = kFaderTable[i - 1];
            const auto& b = kFaderTable[i];
            const float db = a.db + (b.db - a.db) * (pos - a.pos) / (b.pos - a.pos);
            return std::pow(10.0f, db / 20.0f);
        }
    return kFaderMaxGain;
}

} // namespace sonora::model
