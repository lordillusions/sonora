#include "devices/native/WavetableDownsampler.h"

#include <juce_dsp/juce_dsp.h>

#include <cmath>
#include <new>

namespace sonora::devices
{

bool WavetableDownsampler::prepare(int rateFactor)
{
    if (rateFactor != 1 && rateFactor != 2) return false;
    std::array<float, poleCount> next{};
    if (rateFactor == 2)
    {
        try
        {
            // Public Valenzuela/Constantinides halfband design. No copied
            // coefficient array: this width/order yields the observed poles.
            const auto design = juce::dsp::FilterDesign<double>::designIIRLowpassHalfBandPolyphaseAllpassMethod(0.0375, -120);
            if (design.alpha.size() != static_cast<int>(poleCount)) return false;
            for (size_t i = 0; i < poleCount; ++i)
            {
                const double value = design.alpha[static_cast<int>(i)];
                if (! std::isfinite(value) || value <= 0 || value >= 1) return false;
                next[i] = static_cast<float>(value);
            }
        }
        catch (const std::bad_alloc&) { return false; }
    }
    alpha_ = next;
    factor_ = rateFactor;
    reset();
    return true;
}

void WavetableDownsampler::reset() noexcept
{
    lanes_ = {};
}

void WavetableDownsampler::processFrame(const Frame& first, const Frame& second, Frame& output) noexcept
{
    if (factor_ == 1)
    {
        output = first;
        return;
    }
    for (size_t lane = 0; lane < laneCount; ++lane)
    {
        auto& state = lanes_[lane];
        float a = first[lane], b = second[lane];
        if (! std::isfinite(a) || ! std::isfinite(b))
        {
            state = {};
            output[lane] = 0;
            continue;
        }
        for (size_t stage = 0; stage < poleCount / 2; ++stage)
        {
            auto& left = state.first[stage];
            auto& right = state.second[stage];
            // Standard first-order allpass difference equation. Pair-reversed
            // paths correspond to selecting the second sample of a 2x pair.
            const float nextA = std::fma(alpha_[stage * 2 + 1], a - left.output, left.input);
            const float nextB = std::fma(alpha_[stage * 2], b - right.output, right.input);
            left = {a, nextA}; right = {b, nextB};
            a = nextA; b = nextB;
        }
        const float filtered = a * 0.5f + b * 0.5f;
        if (! std::isfinite(filtered))
        {
            state = {};
            output[lane] = 0;
            continue;
        }
        output[lane] = state.pipeline[state.cursor];
        state.pipeline[state.cursor] = filtered;
        state.cursor ^= 1;
    }
}

} // namespace sonora::devices
