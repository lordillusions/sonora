#pragma once
// DEV-18i: the Serum 2 peak / dual / morph voice filter types as an own implementation of behaviour measured black-box
// on the reference (complex response at every harmonic of a 32.7 Hz saw across cutoff, resonance and Var).
// All of them are TPT state-variable sections whose low / band / high outputs are mixed:
//   section response (n0 + n1 s + n2 s^2) / (s^2 + k s + 1), k = 2 (1 - r^(1/4)) (r = resonance 0..1, as the normal family);
//   L = (1,0,0), B = (0,1,0) (peak gain 1 / k), H = (0,0,1), N = (1,0,1), P = (1,1,1) (a bell, -6 dB at r = 0).
// - Peak 12 / 24: P (24: a second P section with k2 = 1 + 1.06 (k / 2)^2); output gain (1 + 4 r var) / (1 + 4 r)
//   (the normal family's Var law).
// - Dual types ("LH 12", "PP 12", ...): two sections in series, the first at the cutoff and the second at an absolute
//   frequency set by Var alone: MIDI note 128 x Var (330 Hz at Var 0.5 whatever the cutoff). Same damping (except the
//   notch of LN / HN / BN / PN, which ignores resonance), no level loss. LH 6 is a one-pole low-pass at the cutoff into a one-pole high-pass at that frequency.
// - Morph types ("L/B/H 12" ...): one section (24: two, k2 as above, both with the same mix) crossfading linearly
//   through three responses over Var 0 .. 0.5 .. 1; no level loss. L/P/H 24 is measured with its second section going
//   P -> -H (not H) over Var 0.5 .. 1; reproduced.
// - A fixed high pole (the top-octave droop of the reference): 60 kHz on 12 dB morph / peak types, 30 kHz otherwise.
// Coefficients are set at control rate with setup(); process() is allocation-free.
#include "dsp/Dsp.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>

namespace sonora::dsp
{
class MeasuredMultiFilter
{
public:
    enum Shape { Peak12, Peak24, LH6, LH12, LB12, LP12, LN12, HB12, HP12, HN12, BP12, BN12, PP12, PN12, NN12,
                 LBH12, LBH24, LPH12, LPH24, LNH12, LNH24, BPN12, BPN24, ShapeCount };
    struct Mix { float n0, n1, n2; };
    static constexpr bool isDual(int shape) { return shape >= LH6 && shape <= NN12; }
    static constexpr bool isMorph(int shape) { return shape >= LBH12 && shape <= BPN24; }
    static constexpr bool steep(int shape) { return shape == Peak24 || (isMorph(shape) && (shape - LBH12) % 2 == 1); }
    // Second cutoff of the dual types (Hz): MIDI note 128 x Var.
    static float dualFrequency(float var) { return 440.0f * std::exp2((128.0f * std::clamp(var, 0.0f, 1.0f) - 69.0f) / 12.0f); }
    static float damping(float resonance) { return std::max(0.002f, 2.0f * (1 - std::sqrt(std::sqrt(std::clamp(resonance, 0.0f, 1.0f))))); }
    // Damping of the second section: the notch of LN / HN / BN / PN 12 ignores resonance (measured k = 2); NN 12's does not.
    static float secondDamping(int shape, float k)
    {
        if (isDual(shape)) return (shape == LN12 || shape == HN12 || shape == BN12 || shape == PN12) ? 2.0f : k;
        return 1 + 1.06f * (k * 0.5f) * (k * 0.5f);
    }
    static float highPole(int shape) { return (shape == Peak12 || (isMorph(shape) && ! steep(shape))) ? 60000.0f : 30000.0f; }

    // Section mixes for a shape at Var (first section; `second` the second one of 24 dB morph types and dual types).
    static Mix mix(int shape, float var, bool second)
    {
        static constexpr Mix L{1, 0, 0}, B{0, 1, 0}, H{0, 0, 1}, N{1, 0, 1}, P{1, 1, 1};
        const float v = std::clamp(var, 0.0f, 1.0f);
        const auto blend = [](Mix a, Mix b, float t) { return Mix{a.n0 + (b.n0 - a.n0) * t, a.n1 + (b.n1 - a.n1) * t, a.n2 + (b.n2 - a.n2) * t}; };
        const auto morph = [&](Mix a, Mix b, Mix c) { return v <= 0.5f ? blend(a, b, 2 * v) : blend(b, c, 2 * v - 1); };
        switch (shape)
        {
            case Peak12: case Peak24: return P;
            case LH6: case LH12: return second ? H : L;
            case LB12: return second ? B : L;
            case LP12: return second ? P : L;
            case LN12: return second ? N : L;
            case HB12: return second ? B : H;
            case HP12: return second ? P : H;
            case HN12: return second ? N : H;
            case BP12: return second ? P : B;
            case BN12: return second ? N : B;
            case PP12: return P;
            case PN12: return second ? N : P;
            case NN12: return N;
            case LBH12: case LBH24: return morph(L, B, H);
            case LPH12: return morph(L, P, H);
            case LPH24: return second ? morph(L, P, Mix{0, 0, -1}) : morph(L, P, H);
            case LNH12: case LNH24: return morph(L, N, H);
            case BPN12: case BPN24: return morph(B, P, N);
            default: return L;
        }
    }

    void setup(double sampleRate, float cutoffHz, float resonance, float var, int shape)
    {
        shape_ = std::clamp(shape, 0, ShapeCount - 1);
        const auto limit = static_cast<float>(sampleRate * 0.45);
        const auto coefficient = [&](float hz) { return std::tan(static_cast<float>(kPi) * std::clamp(hz, 5.0f, limit) / static_cast<float>(sampleRate)); };
        const float k = damping(resonance);
        const float g1 = coefficient(cutoffHz);
        const float g2 = isDual(shape_) ? coefficient(dualFrequency(var)) : g1;
        first_.set(g1, k, mix(shape_, var, false));
        second_.set(g2, secondDamping(shape_, k), mix(shape_, var, true));
        poleA_ = g1 / (1 + g1); poleB_ = g2 / (1 + g2);
        const float r = std::clamp(resonance, 0.0f, 1.0f);
        gain_ = shape_ <= Peak24 ? (1 + 4 * r * std::clamp(var, 0.0f, 1.0f)) / (1 + 4 * r) : 1.0f;
        const float fp = highPole(shape_);
        highPole_ = fp < sampleRate * 0.45;
        if (highPole_) { const float h = coefficient(fp); highG_ = h / (1 + h); }
    }
    void reset() { first_.reset(); second_.reset(); stateA_ = stateB_ = high_ = 0; }
    float process(float x)
    {
        float y;
        if (shape_ == LH6)
        {
            const float v = poleA_ * (x - stateA_), low = v + stateA_; stateA_ = low + v;
            const float w = poleB_ * (low - stateB_), lowB = w + stateB_; stateB_ = lowB + w;
            y = low - lowB;
        }
        else
        {
            y = first_.process(x);
            if (shape_ != Peak12 && (isDual(shape_) || steep(shape_))) y = second_.process(y);
        }
        y *= gain_;
        if (highPole_) { const float v = highG_ * (y - high_), low = v + high_; high_ = low + v; y = low; }
        return std::isfinite(y) ? y : (reset(), 0.0f);
    }
    // Analog magnitude of the same model (editor curves).
    static float magnitude(int shape, float hz, float cutoffHz, float resonance, float var)
    {
        shape = std::clamp(shape, 0, ShapeCount - 1);
        const float k = damping(resonance);
        const auto section = [&](float corner, float damp, Mix m) {
            const std::complex<float> s(0, hz / std::max(1.0f, corner));
            return (m.n0 + m.n1 * s + m.n2 * s * s) / (s * s + damp * s + 1.0f);
        };
        const float second = isDual(shape) ? dualFrequency(var) : cutoffHz;
        std::complex<float> h;
        if (shape == LH6)
        {
            const std::complex<float> a(0, hz / std::max(1.0f, cutoffHz)), b(0, hz / std::max(1.0f, second));
            h = 1.0f / (1.0f + a) * b / (1.0f + b);
        }
        else
        {
            h = section(cutoffHz, k, mix(shape, var, false));
            if (shape != Peak12 && (isDual(shape) || steep(shape)))
                h *= section(second, secondDamping(shape, k), mix(shape, var, true));
        }
        const float r = std::clamp(resonance, 0.0f, 1.0f);
        return std::abs(h) * (shape <= Peak24 ? (1 + 4 * r * std::clamp(var, 0.0f, 1.0f)) / (1 + 4 * r) : 1.0f);
    }

private:
    struct Section
    {
        float k = 1, a1 = 0, a2 = 0, a3 = 0, ic1 = 0, ic2 = 0;
        Mix m{1, 0, 0};
        void set(float g, float damping, Mix mix) { k = damping; a1 = 1 / (1 + g * (g + k)); a2 = g * a1; a3 = g * a2; m = mix; }
        void reset() { ic1 = ic2 = 0; }
        float process(float x)
        {
            const float v3 = x - ic2, v1 = a1 * ic1 + a2 * v3, v2 = ic2 + a2 * ic1 + a3 * v3;
            ic1 = 2 * v1 - ic1; ic2 = 2 * v2 - ic2;
            return m.n0 * v2 + m.n1 * v1 + m.n2 * (x - k * v1 - v2);
        }
    };
    Section first_, second_;
    int shape_ = LBH12;
    float poleA_ = 0, poleB_ = 0, stateA_ = 0, stateB_ = 0, gain_ = 1, highG_ = 0, high_ = 0;
    bool highPole_ = false;
};
} // namespace sonora::dsp
