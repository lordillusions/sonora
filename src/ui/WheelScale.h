#pragma once
// Scroll-wheel sensitivity shared by every view. JUCE reports a trackpad scroll as pixels * 0.5/256 (about 0.002 per pixel) and
// a classic mouse-wheel notch as roughly 0.04 .. 0.12, so views multiply by kWheelPixels to move content one-to-one with the
// fingers, and zoom with an exponential of the delta (about +25 % per mouse notch, a few percent per trackpad event).

#include <cmath>

namespace sonora::ui
{

constexpr float kWheelPixels = 512.0f;
constexpr double kWheelZoomPerUnit = 2.2;

inline double wheelZoomFactor(double delta) { return std::exp(delta * kWheelZoomPerUnit); }

} // namespace sonora::ui
