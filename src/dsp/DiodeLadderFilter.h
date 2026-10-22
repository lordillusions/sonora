#pragma once
// DEV-18i: Serum 2 "Ladder Acid" as an own implementation of behaviour measured black-box on the reference.
// The measured small-signal response is the classic coupled diode ladder:
//   y' = w (M y + e1 u),  M = [[-1, 1, 0, 0], [1, -2, 1, 0], [0, 1, -2, 1], [0, 0, 1, -2]]
//   (poles 0.12 / 1 / 2.35 / 3.53 w at zero feedback), u = x - k y4, w = 2 pi 0.847 cutoff, k = 18.2 resonance,
//   output y4 x -(0.99 + 0.945 k) (inverted, as the reference). Fits within ~0.3 dB down to -60 dB at cutoffs 0.4..0.75
//   and resonance 0..90 % (the reference adds ~18 us of latency, not modelled).
// Trapezoidal integration of the coupled system: (I - g M) y = s + g e1 u with g = tan(pi f / fs), solved with the
// inverse precomputed per control tick; the feedback sum is solved instantaneously and soft-clipped like the other
// measured ladders. Allocation-free.
#include "dsp/LadderFilter.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>

namespace sonora::dsp
{
class DiodeLadderFilter
{
public:
    static constexpr float kCutoffScale = 0.847f, kFeedback = 18.2f;
    static float outputGain(float feedback) { return -(0.99f + 0.945f * feedback); }

    void setup(double sampleRate, float cutoffHz, float resonance)
    {
        const double fc = std::clamp(static_cast<double>(cutoffHz) * kCutoffScale, 2.0, sampleRate * 0.45);
        const double g = std::tan(3.14159265358979323846 * fc / sampleRate);
        // A = I - g M (tridiagonal) and its inverse by Gauss-Jordan (4x4, control rate).
        double a[4][8]{};
        static constexpr double m[4][4]{{-1, 1, 0, 0}, {1, -2, 1, 0}, {0, 1, -2, 1}, {0, 0, 1, -2}};
        for (int r = 0; r < 4; ++r)
        {
            for (int c = 0; c < 4; ++c) a[r][c] = (r == c ? 1.0 : 0.0) - g * m[r][c];
            a[r][4 + r] = 1;
        }
        for (int c = 0; c < 4; ++c)
        {
            const double pivot = a[c][c];
            for (int j = 0; j < 8; ++j) a[c][j] /= pivot;
            for (int r = 0; r < 4; ++r)
                if (r != c)
                {
                    const double factor = a[r][c];
                    for (int j = 0; j < 8; ++j) a[r][j] -= factor * a[c][j];
                }
        }
        for (int r = 0; r < 4; ++r)
        {
            for (int c = 0; c < 4; ++c) p_[r][c] = a[r][4 + c];
            q_[r] = g * a[r][4];
        }
        k_ = kFeedback * std::clamp(static_cast<double>(resonance), 0.0, 1.0);
        gain_ = outputGain(static_cast<float>(k_));
    }
    void reset() { s_ = {}; }
    float process(float x)
    {
        double y[4];
        for (int r = 0; r < 4; ++r) y[r] = p_[r][0] * s_[0] + p_[r][1] * s_[1] + p_[r][2] * s_[2] + p_[r][3] * s_[3];
        // y4 = y[3] + q4 u, u = x - k y4: solve for u, then soft-clip the feedback sum.
        const double u = LadderFilter::inputClip(static_cast<float>((x - k_ * y[3]) / (1 + k_ * q_[3])));
        for (int r = 0; r < 4; ++r)
        {
            y[r] += q_[r] * u;
            s_[static_cast<size_t>(r)] = 2 * y[r] - s_[static_cast<size_t>(r)];
        }
        const float out = static_cast<float>(gain_ * y[3]);
        return std::isfinite(out) ? out : (reset(), 0.0f);
    }
    // Analog magnitude of the same model (editor curves).
    static float magnitude(float hz, float cutoffHz, float resonance)
    {
        const std::complex<double> s(0, hz / std::max(1.0, static_cast<double>(cutoffHz) * kCutoffScale));
        const double k = kFeedback * std::clamp(static_cast<double>(resonance), 0.0, 1.0);
        // Tridiagonal chain: transfer from stage-1 input to y4 by forward substitution on (s I - M) y = e1 u.
        // Solve with the Thomas algorithm for u = 1, then close the loop.
        const std::complex<double> diag[4]{s + 1.0, s + 2.0, s + 2.0, s + 2.0};
        std::complex<double> c[4], d[4];
        c[0] = -1.0 / diag[0]; d[0] = 1.0 / diag[0];
        for (int i = 1; i < 4; ++i)
        {
            const auto denominator = diag[i] + c[i - 1]; // sub-diagonal is -1
            c[i] = -1.0 / denominator;
            d[i] = d[i - 1] / denominator;
        }
        const auto open = d[3]; // y4 / u (back substitution leaves the last unknown as is)
        return static_cast<float>(std::abs(open / (1.0 + k * open)) * std::abs(outputGain(static_cast<float>(k))));
    }

private:
    double p_[4][4]{}, q_[4]{}, k_ = 0;
    float gain_ = -1;
    std::array<double, 4> s_{};
};
} // namespace sonora::dsp
