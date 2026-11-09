#pragma once
// UI-75: where an automation value sits on its lane, as the reference draws it: the control's position, not the raw
// value. Volume and sends follow the fader law (0 dB near the top), frequencies and times a log scale, the rest
// (pan, plug-in parameters 0..1, switches, choices) are linear. Breakpoints interpolate in this position space, so a
// drawn line is straight on the lane and the engine plays what the lane shows.

#include "core/FaderCurve.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

namespace sonora::model
{
enum class AutomationLaw : uint8_t
{
    Linear,
    Fader, // linear gain on the volume fader's curve (min/max unused)
    Log,   // min > 0
};

// A native parameter's law from its unit and range (DeviceFace draws the same knobs logarithmic).
inline AutomationLaw automationLawFor(const std::string& unit, double min, double max, bool stepped)
{
    if (stepped || min <= 0 || max <= min) return AutomationLaw::Linear;
    return unit == "Hz" || unit == "ms" ? AutomationLaw::Log : AutomationLaw::Linear;
}

// Value -> lane position 0..1.
inline double automationPosition(AutomationLaw law, double value, double min, double max)
{
    switch (law)
    {
        case AutomationLaw::Fader: return faderGainToPosition((float) value);
        case AutomationLaw::Log:
            return std::clamp(std::log(std::max(value, min) / min) / std::log(max / min), 0.0, 1.0);
        case AutomationLaw::Linear: break;
    }
    return std::clamp((value - min) / std::max(1e-12, max - min), 0.0, 1.0);
}

// Lane position 0..1 -> value.
inline double automationValue(AutomationLaw law, double position, double min, double max)
{
    position = std::clamp(position, 0.0, 1.0);
    switch (law)
    {
        case AutomationLaw::Fader: return faderPositionToGain((float) position);
        case AutomationLaw::Log: return min * std::pow(max / min, position);
        case AutomationLaw::Linear: break;
    }
    return min + position * (max - min);
}
} // namespace sonora::model
