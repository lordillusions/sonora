#include "PreparedSampleKernel.h"
#include "generated/SampleKernelMinimax1025.inc"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace sonora::dsp
{
namespace
{
double besselI0(double value)
{
    double sum = 1, term = 1;
    const double squared = value * value * 0.25;
    for (int order = 1; order <= 64; ++order)
    {
        term *= squared / static_cast<double>(order * order);
        sum += term;
        if (term < sum * 1e-16) break;
    }
    return sum;
}

// Standard uniform-grid cubic spline, with not-a-knot boundary conditions.
// Second derivatives use grid-index units, so interpolation needs no rate scale.
// This solver prepares the independently generated prototype off audio; it is
// neither a proprietary generation algorithm nor a table read from a binary.
auto minimaxSecondDerivatives()
{
    constexpr size_t count = detail::sampleKernelMinimax1025.size();
    std::array<double, count> second{}, upper{}, right{};
    for (size_t index = 1; index + 1 < count; ++index)
        right[index] = 6 * (detail::sampleKernelMinimax1025[index - 1]
            - 2 * detail::sampleKernelMinimax1025[index] + detail::sampleKernelMinimax1025[index + 1]);
    second[1] = right[1] / 6;
    for (size_t index = 2; index + 2 < count; ++index)
    {
        const double diagonal = 4 - upper[index - 1];
        upper[index] = 1 / diagonal;
        second[index] = (right[index] - second[index - 1]) / diagonal;
    }
    second[count - 2] = right[count - 2] / 6;
    for (size_t index = count - 3; index >= 2; --index)
        second[index] -= upper[index] * second[index + 1];
    second[0] = 2 * second[1] - second[2];
    second[count - 1] = 2 * second[count - 2] - second[count - 3];
    return second;
}

double minimaxWeight(double distance, const std::array<double, 1025>& second)
{
    // Zero the support endpoints to keep a16-tap periodic lookup continuous
    // when its integer sample index advances. This is an own support convention.
    if (std::abs(distance) >= 8) return 0;
    const double coordinate = (distance + 8) * 64;
    const auto first = std::min<size_t>(1023, static_cast<size_t>(coordinate));
    const double right = coordinate - static_cast<double>(first), left = 1 - right;
    return left * detail::sampleKernelMinimax1025[first] + right * detail::sampleKernelMinimax1025[first + 1]
        + ((left * left * left - left) * second[first]
           + (right * right * right - right) * second[first + 1]) / 6;
}
}

const PreparedSampleKernel PreparedSampleKernel::unityKaiser_(SampleKernelProfile::UnityKaiser);
const PreparedSampleKernel PreparedSampleKernel::minimaxApprox_(SampleKernelProfile::MinimaxApprox);

PreparedSampleKernel::PreparedSampleKernel(SampleKernelProfile profile) noexcept : profile_(profile)
{
    if (profile == SampleKernelProfile::MinimaxApprox)
    {
        const auto second = minimaxSecondDerivatives();
        for (int phase = 0; phase <= phases; ++phase)
            for (int tap = 0; tap < taps; ++tap)
                coefficients_[static_cast<size_t>(phase * taps + tap)] = static_cast<float>(
                    minimaxWeight(static_cast<double>(phase) / phases - static_cast<double>(tap - 7), second));
        return;
    }
    // Original independently authored Kaiser beta8/cutoff1.125, per-phase unity
    // DC. Keep the calculation/order intact for existing noise playback.
    constexpr double beta = 8, cutoff = 1.125, radius = 8;
    const double denominator = besselI0(beta);
    for (int phase = 0; phase <= phases; ++phase)
    {
        const double fraction = static_cast<double>(phase) / phases;
        std::array<double, taps> weights{};
        double sum = 0;
        for (int tap = 0; tap < taps; ++tap)
        {
            const double distance = fraction - static_cast<double>(tap - 7);
            if (std::abs(distance) >= radius) continue;
            const double argument = std::numbers::pi * cutoff * distance;
            const double sinc = std::abs(argument) < 1e-12 ? 1 : std::sin(argument) / argument;
            const double window = besselI0(beta * std::sqrt(std::max(0.0, 1 - distance * distance / (radius * radius)))) / denominator;
            weights[static_cast<size_t>(tap)] = cutoff * sinc * window;
            sum += weights[static_cast<size_t>(tap)];
        }
        for (int tap = 0; tap < taps; ++tap)
            coefficients_[static_cast<size_t>(phase * taps + tap)] = static_cast<float>(weights[static_cast<size_t>(tap)] / sum);
    }
}

const PreparedSampleKernel* PreparedSampleKernel::forProfile(SampleKernelProfile profile) noexcept
{
    switch (profile)
    {
        case SampleKernelProfile::UnityKaiser: return &unityKaiser_;
        case SampleKernelProfile::MinimaxApprox: return &minimaxApprox_;
    }
    return nullptr;
}

} // namespace sonora::dsp
