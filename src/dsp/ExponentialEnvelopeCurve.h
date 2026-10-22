#pragma once
// DEV-18d: independent fit to controlled interior Serum envelope progress curves.
// Endpoint tension clamping is a finite Sonora choice, not vendor code.
#include <algorithm>
#include <cmath>

namespace sonora::dsp
{
struct ExponentialEnvelopeCurve
{
    float coefficient = 0, scale = 1;
    static ExponentialEnvelopeCurve fromTension(float tension)
    {
        if (! std::isfinite(tension)) tension = 0;
        const double u = std::clamp((static_cast<double>(tension) + 1) * 0.5, 0.0, 1.0);
        const double m = u < 0.5 ? 4 * u * u * u : 1 - 4 * (1 - u) * (1 - u) * (1 - u);
        const double k = m <= 0 ? 64 : m >= 1 ? -64 : std::clamp(2 * std::log((1 - m) / m), -64.0, 64.0);
        if (std::abs(k) < 1e-5) return {};
        return {static_cast<float>(k), static_cast<float>(1 / std::expm1(k))};
    }
    float eval(float phase) const
    {
        if (! std::isfinite(phase) || phase <= 0) return 0;
        if (phase >= 1) return 1;
        if (std::abs(coefficient) < 1e-5f) return phase;
        return std::clamp(std::expm1(coefficient * phase) * scale, 0.0f, 1.0f);
    }
};
} // namespace sonora::dsp
