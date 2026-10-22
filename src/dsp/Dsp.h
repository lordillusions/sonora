#pragma once
// Small allocation-free DSP building blocks shared by the native devices (audio thread safe once
// prepared). Header-only on purpose: everything here is tiny and gets inlined.

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace sonora::dsp
{

constexpr double kPi = 3.14159265358979323846;

inline float dbToGain(float db) { return std::pow(10.0f, db / 20.0f); }
inline float gainToDb(float g) { return 20.0f * std::log10(std::max(g, 1e-9f)); }

// One-pole smoothing coefficient for a time constant (ms): y += (x - y) * coef.
inline float timeToCoef(float ms, double sampleRate)
{
    return ms <= 0.0f ? 1.0f : 1.0f - std::exp(-1.0f / (float) (ms * 0.001 * sampleRate));
}

// Fractional delay line (linear interpolation). Capacity fixed in prepare().
class FractionalDelay
{
public:
    void prepare(int maxDelaySamples)
    {
        size_ = 1;
        while (size_ < maxDelaySamples + 4)
            size_ <<= 1;
        buf_.assign((size_t) size_, 0.0f);
        mask_ = size_ - 1;
        pos_ = 0;
    }
    void reset() { std::fill(buf_.begin(), buf_.end(), 0.0f); }
    void write(float x)
    {
        buf_[(size_t) pos_] = x;
        pos_ = (pos_ + 1) & mask_;
    }
    // Sample written `delay` samples ago (delay >= 1 for feedback loops, 0 == the newest sample).
    float read(float delay) const
    {
        const float d = std::clamp(delay, 0.0f, (float) (size_ - 2));
        const int i = (int) d;
        const float f = d - (float) i;
        const float a = buf_[(size_t) ((pos_ - 1 - i) & mask_)];
        const float b = buf_[(size_t) ((pos_ - 2 - i) & mask_)];
        return a + (b - a) * f;
    }
    // Catmull-Rom cubic interpolation (delay >= 1): flatter than linear, used where repeats compound the error.
    float readCubic(float delay) const
    {
        const float d = std::clamp(delay, 1.0f, (float) (size_ - 3));
        const int i = (int) d;
        const float t = d - (float) i;
        const auto at = [this](int j) { return buf_[(size_t) ((pos_ - 1 - j) & mask_)]; };
        const float pm1 = at(i - 1), p0 = at(i), p1 = at(i + 1), p2 = at(i + 2);
        return p0 + 0.5f * t * (p1 - pm1 + t * (2.0f * pm1 - 5.0f * p0 + 4.0f * p1 - p2 + t * (3.0f * (p0 - p1) + p2 - pm1)));
    }
    int capacity() const { return size_; }

private:
    std::vector<float> buf_;
    int size_ = 0, mask_ = 0, pos_ = 0;
};

// Chamberlin-style state variable filter (stable up to ~fs/6, fine for musical filters) with
// a trapezoidal (TPT) core so that cutoff modulation is well behaved.
class Svf
{
public:
    enum class Mode { LowPass, HighPass, BandPass, Notch };

    void setup(double sampleRate, float cutoffHz, float q)
    {
        const float fc = std::clamp(cutoffHz, 10.0f, (float) (sampleRate * 0.45));
        g_ = std::tan((float) kPi * fc / (float) sampleRate);
        k_ = 1.0f / std::max(0.05f, q);
        a1_ = 1.0f / (1.0f + g_ * (g_ + k_));
        a2_ = g_ * a1_;
        a3_ = g_ * a2_;
    }
    float process(float x, Mode mode)
    {
        const float v3 = x - ic2_;
        const float v1 = a1_ * ic1_ + a2_ * v3;
        const float v2 = ic2_ + a2_ * ic1_ + a3_ * v3;
        ic1_ = 2.0f * v1 - ic1_;
        ic2_ = 2.0f * v2 - ic2_;
        switch (mode)
        {
            case Mode::LowPass: return v2;
            case Mode::BandPass: return v1;
            case Mode::HighPass: return x - k_ * v1 - v2;
            case Mode::Notch: return x - k_ * v1;
        }
        return v2;
    }
    void reset() { ic1_ = ic2_ = 0.0f; }

private:
    float g_ = 0, k_ = 1, a1_ = 0, a2_ = 0, a3_ = 0, ic1_ = 0, ic2_ = 0;
};

// Low-frequency oscillator, phase in 0..1.
class Lfo
{
public:
    enum class Shape { Sine, Triangle, SawDown, SawUp, Square, Random };
    void setPhase(double p) { phase_ = p - std::floor(p); }
    // Advances by `inc` (cycles per sample) and returns -1..1.
    float next(double inc, Shape shape)
    {
        phase_ += inc;
        if (phase_ >= 1.0)
        {
            phase_ -= std::floor(phase_);
            rnd_ = rnd_ * 1664525u + 1013904223u;
            held_ = (float) (rnd_ >> 8) / 8388608.0f - 1.0f;
        }
        const float p = (float) phase_;
        switch (shape)
        {
            case Shape::Sine: return std::sin(2.0f * (float) kPi * p);
            case Shape::Triangle: return 1.0f - 4.0f * std::fabs(p - 0.5f);
            case Shape::SawDown: return 1.0f - 2.0f * p;
            case Shape::SawUp: return 2.0f * p - 1.0f;
            case Shape::Square: return p < 0.5f ? 1.0f : -1.0f;
            case Shape::Random: return held_;
        }
        return 0.0f;
    }

private:
    double phase_ = 0.0;
    unsigned rnd_ = 12345u;
    float held_ = 0.0f;
};

// Linkwitz-Riley split per channel: LR4 (24 dB/oct, two Butterworth-2 biquads per leg) or LR8
// (48 dB/oct, Butterworth-4 squared = four biquads per leg). The low and high legs sum to an
// all-pass, so a three-band split built from them is flat.
class Crossover
{
public:
    void setup(double sampleRate, float freq, bool steep = false)
    {
        steep_ = steep;
        const double w0 = 2.0 * kPi * std::clamp((double) freq, 20.0, sampleRate * 0.45) / sampleRate;
        const double cw = std::cos(w0), sw = std::sin(w0);
        static constexpr double kQ4[2] = {0.70710678, 0.70710678};
        static constexpr double kQ8[4] = {0.54119610, 1.30656296, 0.54119610, 1.30656296};
        stages_ = steep ? 4 : 2;
        for (int i = 0; i < stages_; ++i)
        {
            const double alpha = sw / (2.0 * (steep ? kQ8[i] : kQ4[i]));
            const double a0 = 1.0 + alpha;
            auto& c = coef_[(size_t) i];
            c.a1 = (float) (-2.0 * cw / a0);
            c.a2 = (float) ((1.0 - alpha) / a0);
            c.lb0 = (float) ((1.0 - cw) / 2.0 / a0);
            c.lb1 = (float) ((1.0 - cw) / a0);
            c.hb0 = (float) ((1.0 + cw) / 2.0 / a0);
            c.hb1 = (float) (-(1.0 + cw) / a0);
        }
    }
    void process(float x, float& low, float& high)
    {
        float l = x, h = x;
        for (int i = 0; i < stages_; ++i)
        {
            const auto& c = coef_[(size_t) i];
            l = stage(lz_[i], c.lb0, c.lb1, c, l);
            h = stage(hz_[i], c.hb0, c.hb1, c, h);
        }
        low = l;
        high = h;
    }
    void reset()
    {
        for (auto& z : lz_)
            z[0] = z[1] = 0.0f;
        for (auto& z : hz_)
            z[0] = z[1] = 0.0f;
    }

private:
    struct Coef
    {
        float a1 = 0, a2 = 0, lb0 = 0, lb1 = 0, hb0 = 0, hb1 = 0;
    };
    static float stage(float* z, float b0, float b1, const Coef& c, float x)
    {
        const float y = b0 * x + z[0];
        z[0] = b1 * x - c.a1 * y + z[1];
        z[1] = b0 * x - c.a2 * y; // b2 == b0 for both the low-pass and the high-pass
        return y;
    }
    std::array<Coef, 4> coef_{};
    float lz_[4][2]{}, hz_[4][2]{};
    int stages_ = 2;
    bool steep_ = false;
};

} // namespace sonora::dsp
