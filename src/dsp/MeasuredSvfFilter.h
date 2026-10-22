#pragma once
// DEV-18i: the Serum 2 "normal" voice filter family (Low / High / Band / Notch, 6..24 dB) as an own implementation of
// behaviour measured black-box on the reference (complex response at every harmonic of a 32.7 Hz saw, fitted
// per type across resonance). TPT state-variable sections:
//   damping k = a (1 - r^(1/4)) for the resonant section (r = resonance 0..1, self-oscillating at 1);
//   24 dB types add a second section with k2 = 1 + b (k / 2)^2;
//   output gain g0 (1 + gc r var) / (1 + gc r) (Var restores that fraction of the resonance level loss; notch and 6 dB: g0);
//   a fixed high pole fp (the measured top-octave droop) and a cutoff scale c (6 dB types only differ from 1).
// Coefficients are set at control rate with setup(); process() is allocation-free.
#include "dsp/Dsp.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>

namespace sonora::dsp
{
class MeasuredSvfFilter
{
public:
    enum Shape { Low6, Low12, Low18, Low24, High6, High12, High18, High24, Band12, Band24, Notch12, Notch24, ShapeCount };
    struct Profile { float a, b, g0, fp, c, gc; };
    static const Profile& profile(int shape)
    {
        // Joint least-squares fits at cutoff 0.5 (425 Hz), resonance 0/25/50/75 %; rms log error 0.001 (Low 24) .. 0.12 (Band 24).
        static constexpr std::array<Profile, ShapeCount> profiles{{
            {2.0f, 0.0f, 0.9899f, 27965, 1.0554f, 0}, {1.9699f, 0.0f, 0.9780f, 58654, 1.0010f, 4.037f},
            {2.0459f, 0.0f, 1.0295f, 19839, 1.0025f, 4.120f}, {2.0110f, 1.0210f, 1.0176f, 30673, 1.0021f, 4.109f},
            {2.0f, 0.0f, 0.9566f, 63471, 0.9589f, 0}, {1.9231f, 0.0f, 0.9621f, 63353, 1.0009f, 4.060f},
            {1.9620f, 0.0f, 0.9670f, 28995, 1.0009f, 4.065f}, {1.9601f, 1.0615f, 0.9739f, 28982, 1.0020f, 4.107f},
            {1.8902f, 0.0f, 0.9426f, 0, 1.0009f, 4.051f}, {1.8549f, 1.0631f, 0.8838f, 0, 1.0001f, 3.865f},
            {1.9170f, 0.0f, 0.9552f, 63392, 1.0f, 0}, {1.7587f, 1.1522f, 0.9219f, 39968, 0.9988f, 0}}};
        return profiles[static_cast<size_t>(std::clamp(shape, 0, ShapeCount - 1))];
    }
    static int order(int shape) { const int s = std::clamp(shape, 0, ShapeCount - 1); return s >= Band12 ? (s % 2 == 0 ? 2 : 4) : s % 4 + 1; }
    static bool high(int shape) { return shape >= High6 && shape <= High24; }

    void setup(double sampleRate, float cutoffHz, float resonance, float var, int shape)
    {
        shape_ = std::clamp(shape, 0, ShapeCount - 1);
        const auto& p = profile(shape_);
        const float r = std::clamp(resonance, 0.0f, 1.0f);
        const float fc = std::clamp(cutoffHz * p.c, 10.0f, static_cast<float>(sampleRate * 0.45));
        const float g = std::tan(static_cast<float>(kPi) * fc / static_cast<float>(sampleRate));
        const float k = std::max(0.002f, p.a * (1 - std::sqrt(std::sqrt(r))));
        first_.set(g, order(shape_) == 1 ? 1.0f : k);
        second_.set(g, 1 + p.b * (k * 0.5f) * (k * 0.5f));
        poleG_ = g / (1 + g);
        const bool compensated = p.gc > 0 && order(shape_) > 1 && shape_ != Notch12 && shape_ != Notch24;
        gain_ = compensated ? p.g0 * (1 + p.gc * r * std::clamp(var, 0.0f, 1.0f)) / (1 + p.gc * r) : p.g0;
        highPole_ = p.fp > 0 && p.fp < sampleRate * 0.45;
        if (highPole_) { const float h = std::tan(static_cast<float>(kPi) * p.fp / static_cast<float>(sampleRate)); highG_ = h / (1 + h); }
    }
    void reset() { first_.reset(); second_.reset(); pole_ = high_ = 0; }
    float process(float x)
    {
        float y = 0;
        const int n = order(shape_);
        if (n == 1)
        {
            const float v = poleG_ * (x - pole_), low = v + pole_; pole_ = low + v;
            y = high(shape_) ? x - low : low;
        }
        else
        {
            y = section(first_, x);
            if (n == 3)
            {
                const float v = poleG_ * (y - pole_), low = v + pole_; pole_ = low + v;
                y = high(shape_) ? y - low : low;
            }
            else if (n == 4) y = section(second_, y);
        }
        y *= gain_;
        if (highPole_) { const float v = highG_ * (y - high_), low = v + high_; high_ = low + v; y = low; }
        return std::isfinite(y) ? y : (reset(), 0.0f);
    }
    // Analog magnitude of the same model (editor curves).
    static float magnitude(int shape, float hz, float cutoffHz, float resonance, float var)
    {
        const auto& p = profile(shape);
        const float r = std::clamp(resonance, 0.0f, 1.0f);
        const std::complex<float> s(0, hz / std::max(1.0f, cutoffHz * p.c));
        const float k = std::max(0.002f, p.a * (1 - std::sqrt(std::sqrt(r))));
        const auto two = [&](float damping) {
            const auto den = s * s + damping * s + 1.0f;
            return shape >= Notch12 ? (s * s + 1.0f) / den : shape >= Band12 ? s / den : high(shape) ? s * s / den : 1.0f / den;
        };
        const auto one = [&] { return high(shape) ? s / (1.0f + s) : 1.0f / (1.0f + s); };
        const int n = order(shape);
        const auto h = n == 1 ? one() : n == 2 ? two(k) : n == 3 ? two(k) * one() : two(k) * two(1 + p.b * (k * 0.5f) * (k * 0.5f));
        const bool compensated = p.gc > 0 && n > 1 && shape != Notch12 && shape != Notch24;
        return std::abs(h) * (compensated ? p.g0 * (1 + p.gc * r * std::clamp(var, 0.0f, 1.0f)) / (1 + p.gc * r) : p.g0);
    }

private:
    struct Section
    {
        float k = 1, a1 = 0, a2 = 0, a3 = 0, ic1 = 0, ic2 = 0;
        void set(float g, float damping) { k = damping; a1 = 1 / (1 + g * (g + k)); a2 = g * a1; a3 = g * a2; }
        void reset() { ic1 = ic2 = 0; }
    };
    float section(Section& s, float x) const
    {
        const float v3 = x - s.ic2, v1 = s.a1 * s.ic1 + s.a2 * v3, v2 = s.ic2 + s.a2 * s.ic1 + s.a3 * v3;
        s.ic1 = 2 * v1 - s.ic1; s.ic2 = 2 * v2 - s.ic2;
        if (shape_ >= Notch12) return x - s.k * v1;
        if (shape_ >= Band12) return v1;
        return high(shape_) ? x - s.k * v1 - v2 : v2;
    }
    Section first_, second_;
    int shape_ = Low12;
    float poleG_ = 0, pole_ = 0, gain_ = 1, highG_ = 0, high_ = 0;
    bool highPole_ = false;
};
} // namespace sonora::dsp
