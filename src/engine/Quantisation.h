#pragma once
// Shared sample boundary for Session launches and recording (ENG-9d). Arrangement loop wraps
// are launch boundaries too, so quantisation longer than the loop cannot starve a queued action.
#include "engine/ClipPlayers.h"
#include <algorithm>
#include <cmath>

namespace sonora::engine
{
inline int quantisedSample(const BlockSegment& segment, double quant, bool looping, double loopStart, double loopEnd)
{
    if (quant <= 0) return 0;
    const double sampleBeat = 1.0 / segment.samplesPerBeat;
    double boundary = std::ceil((segment.startBeats - sampleBeat) / quant - 1e-9) * quant;
    if (looping && segment.startBeats <= loopEnd + 1e-9)
    {
        if (std::abs(segment.startBeats - loopStart) < sampleBeat + 1e-9) boundary = segment.startBeats;
        else boundary = std::min(boundary, loopEnd);
    }
    const double frames = (boundary - segment.startBeats) * segment.samplesPerBeat;
    if (frames < -1.00001 || frames > segment.numSamples + 1e-8) return -1;
    return (int) std::ceil(std::clamp(frames - 1e-8, 0.0, (double) segment.numSamples));
}
} // namespace sonora::engine
