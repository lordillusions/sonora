#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <numbers>

namespace sonora::dsp
{
// RESYN-1: identical four-band analysis and synthesis filters. No allocation or ownership.
inline std::array<float, 3> resynthNoiseAlpha(double rate)
{
    std::array<float, 3> alpha{};
    constexpr std::array<double, 3> cutoff{300, 1500, 6000};
    for (size_t i = 0; i < alpha.size(); ++i) alpha[i] = (float) (1 - std::exp(-2 * std::numbers::pi * std::min(cutoff[i], rate * 0.45) / rate));
    return alpha;
}
inline std::array<float, 4> resynthNoiseBands(float sample, std::array<float, 3>& state, const std::array<float, 3>& alpha)
{
    for (size_t i = 0; i < state.size(); ++i) state[i] += alpha[i] * (sample - state[i]);
    return {state[0], state[1] - state[0], state[2] - state[1], sample - state[2]};
}
inline std::array<double, 4> resynthNoiseResponse(double frequency, double rate)
{
    const auto alpha = resynthNoiseAlpha(rate);
    const auto z = std::polar(1.0, -2 * std::numbers::pi * frequency / rate);
    std::array<std::complex<double>, 3> lowpass{};
    for (size_t i = 0; i < lowpass.size(); ++i) lowpass[i] = (double) alpha[i] / (1.0 - (1.0 - alpha[i]) * z);
    return {std::norm(lowpass[0]), std::norm(lowpass[1] - lowpass[0]), std::norm(lowpass[2] - lowpass[1]), std::norm(1.0 - lowpass[2])};
}
inline std::array<float, 4> resynthNoiseNormalization(double rate)
{
    std::array<double, 4> energy{};
    for (int i = 0; i < 2048; ++i)
    {
        const auto response = resynthNoiseResponse((i + 0.5) * rate / 4096, rate);
        for (size_t band = 0; band < energy.size(); ++band) energy[band] += response[band] / (3 * 2048);
    }
    std::array<float, 4> normalization{};
    for (size_t i = 0; i < energy.size(); ++i) normalization[i] = (float) (1 / std::sqrt(std::max(1e-9, energy[i])));
    return normalization;
}
}
