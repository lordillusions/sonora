#pragma once

#include <algorithm>
#include <cmath>

namespace sonora::ui
{
// Linear decay must reach exact silence, otherwise a sub-threshold repaint cutoff can freeze a lit pixel.
inline float meterDecay(float peak, float previous)
{
    const float value = std::max(std::isfinite(peak) ? std::max(0.0f, peak) : 0.0f, previous * 0.85f);
    return value < 0.00001f ? 0.0f : value;
}
} // namespace sonora::ui
