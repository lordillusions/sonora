#pragma once
// DEV-18i: the Serum 2 comb / flanger / phaser voice filter types as an own implementation of behaviour measured black-box on
// the reference (complex response at every harmonic of a 32.7 Hz saw across cutoff, resonance and Var).
// One topology covers them all:
//   w = P F u,  u = x + s r w (fed back),  out = x / (2 (1 + r)) + s b w,  b = r / 2 (comb) or 1 / 2 (flanger, phaser),
//   s = +1 for the "+" (P) and -1 for the "-" (N) variants, r = resonance 0..1 (linear: the response does not change with level).
// - Comb / Flange: P = a delay of exactly 1 / cutoff.
// - Phase 12/24/36/48: P = 2/4/6/8 first-order allpasses at about cutoff / 2 (ratio table below; it rises near the top),
//   and the feedback passes one more sample of the reference's 96 kHz engine (a fractional delay here).
// - FlangePhase 12: the delay followed by 2 allpasses.
// - L6 / H6 / HL6: F = one-pole low-pass / high-pass / both. L6 and H6 sit at cutoff x 2^(21.33 (Var - 0.5)) (Var 0 removes the
//   L6 wet path entirely); HL6 has its low-pass at cutoff x 2^(10.67 Var) and its high-pass at cutoff / 2^(10.67 Var). L6 is a
//   plain (not bilinear) one-pole, g = 1 - exp(-2 pi f / fs), H6 a bilinear one with 0.25 more 96 kHz samples; HL6 fits best as
//   bilinear one-poles. The low-pass paths carry one more 96 kHz sample. Var has no other effect on these types (measured: none at all on the plain ones). The phaser variants keep the
//   same filters without the extra delays and match less well.
// The delay line is owned by the caller (sized with lineLength() before audio runs) and passed to process(); the filter keeps
// how much of it has been written since reset(), so a recycled voice never hears the previous note.
#include "dsp/LadderFilter.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <iterator>

namespace sonora::dsp
{
class MeasuredCombFilter
{
public:
    enum Shape { CombP, CombN, CombL6P, CombL6N, CombH6P, CombH6N, CombHL6P, CombHL6N,
                 FlangeP, FlangeN, FlangeL6P, FlangeL6N, FlangeH6P, FlangeH6N, FlangeHL6P, FlangeHL6N,
                 Phase12P, Phase12N, Phase24P, Phase24N, Phase36P, Phase36N, Phase48P, Phase48N,
                 Phase48L6P, Phase48L6N, Phase48H6P, Phase48H6N, Phase48HL6P, Phase48HL6N,
                 FlangePhase12HL6P, FlangePhase12HL6N, ShapeCount };
    enum Family { Comb, Flange, Phase, FlangePhase };
    enum LoopFilter { NoFilter, LowPass, HighPass, Band };
    struct Kind { Family family; int stages; LoopFilter filter; float sign; };
    static constexpr double kMinimumHz = 16, kReferenceRate = 96000;

    static Kind kind(int shape)
    {
        const int s = std::clamp(shape, 0, ShapeCount - 1);
        const float sign = s % 2 == 0 ? 1.0f : -1.0f;
        if (s <= FlangeHL6N)
        {
            static constexpr LoopFilter filters[]{NoFilter, LowPass, HighPass, Band};
            return {s < FlangeP ? Comb : Flange, 0, filters[(s % 8) / 2], sign};
        }
        if (s <= Phase48N) return {Phase, 2 * ((s - Phase12P) / 2 + 1), NoFilter, sign};
        if (s <= Phase48HL6N)
        {
            static constexpr LoopFilter filters[]{LowPass, HighPass, Band};
            return {Phase, 8, filters[(s - Phase48L6P) / 2], sign};
        }
        return {FlangePhase, 2, Band, sign};
    }
    static constexpr bool usesDelayLine(int shape) { return shape < Phase12P || shape > Phase48HL6N; }
    // Power-of-two delay line length for a sample rate (delays down to kMinimumHz).
    static int lineLength(double sampleRate)
    {
        int length = 64;
        while (length < sampleRate / kMinimumHz + 8) length *= 2;
        return length;
    }
    // Measured allpass corner: (reference rate / pi) atan(pi cutoff / (2 x 96 kHz) x ratio(u)), u = the cutoff's control position.
    static double allpassCorner(double cutoffHz)
    {
        static constexpr double position[]{0.0, 0.05, 0.1, 0.2, 0.25, 0.3, 0.4, 0.45, 0.5, 0.55, 0.6, 0.65, 0.7, 0.75, 0.775, 0.8,
                                           0.825, 0.85, 0.875, 0.9, 0.925, 0.95, 0.975, 1.0};
        static constexpr double ratio[]{1.0156, 1.0108, 1.0075, 1.0044, 1.0038, 1.0036, 1.0044, 1.0056, 1.0077, 1.0110, 1.0162, 1.0242,
                                        1.0367, 1.0566, 1.0708, 1.0892, 1.1133, 1.1453, 1.1890, 1.2503, 1.3400, 1.4791, 1.7151, 2.1843};
        constexpr int count = static_cast<int>(std::size(position));
        const double u = std::clamp((std::log2(std::max(cutoffHz, 1.0) / 440) + 5.75) / 11.4, 0.0, 1.0);
        double scale = ratio[count - 1];
        for (int i = 1; i < count; ++i)
            if (u <= position[i]) { scale = ratio[i - 1] + (ratio[i] - ratio[i - 1]) * (u - position[i - 1]) / (position[i] - position[i - 1]); break; }
        return kReferenceRate / 3.14159265358979323846 * std::atan(3.14159265358979323846 * cutoffHz / (2 * kReferenceRate) * scale);
    }
    static void loopCorners(const Kind& k, double cutoffHz, float var, double& low, double& high)
    {
        const double v = std::clamp(static_cast<double>(var), 0.0, 1.0);
        low = k.filter == Band ? cutoffHz * std::exp2(10.667 * v) : cutoffHz * std::exp2(21.333 * (v - 0.5));
        high = k.filter == Band ? cutoffHz * std::exp2(-10.667 * v) : low;
    }

    void setup(double sampleRate, float cutoffHz, float resonance, float var, int shape, int lineLength = 0)
    {
        shape_ = std::clamp(shape, 0, ShapeCount - 1);
        kind_ = kind(shape_);
        const double limit = std::min(42000.0, sampleRate * 0.45);
        const double fc = std::clamp(static_cast<double>(cutoffHz), lineLength > 8 ? sampleRate / (lineLength - 8) : kMinimumHz, sampleRate * 0.45);
        const auto pole = [&](double hz) { const double g = std::tan(3.14159265358979323846 * std::clamp(hz, 1.0, limit) / sampleRate); return g / (1 + g); };
        r_ = std::clamp(resonance, 0.0f, 1.0f);
        dry_ = 0.5f / (1 + r_);
        wet_ = kind_.sign * (kind_.family == Comb ? 0.5f * r_ : 0.5f);
        allpass_ = static_cast<float>(pole(allpassCorner(fc)));
        double low = 0, high = 0;
        loopCorners(kind_, fc, var, low, high);
        lowG_ = static_cast<float>(kind_.filter == LowPass ? 1 - std::exp(-2 * 3.14159265358979323846 * std::clamp(low, 1.0, limit) / sampleRate)
                                                            : pole(low));
        highG_ = static_cast<float>(pole(high));
        const double extra = sampleRate / kReferenceRate; // one sample of the reference engine
        const double filterDelay = kind_.filter == LowPass || kind_.filter == Band ? 1.0 : kind_.filter == HighPass ? 0.25 : 0.0;
        delay_ = static_cast<float>(sampleRate / fc + filterDelay * extra);
        // Phaser loop delay: Thiran allpass for the fraction below one sample, a short history beyond.
        loopDelay_ = static_cast<float>(extra);
        const float fraction = std::min(1.0f, loopDelay_);
        thiran_ = (1 - fraction) / (1 + fraction);
    }
    void reset()
    {
        stage_ = {}; low_ = high_ = 0; written_ = 0; write_ = 0; thiranState_ = 0; history_ = {}; historyAt_ = 0;
    }
    // `line`/`mask`: the caller's delay line (power-of-two length, mask = length - 1); unused by the phaser types.
    float process(float x, float* line, int mask)
    {
        float w = 0;
        if (usesDelayLine(shape_))
        {
            if (line != nullptr) w = processPath(read(line, mask));
            const float sum = LadderFilter::inputClip(x + kind_.sign * r_ * w);
            if (line != nullptr)
            {
                line[write_ & mask] = sum;
                write_ = (write_ + 1) & mask;
                written_ = std::min(written_ + 1, mask + 1);
            }
        }
        else
        {
            // u = x + s r t, t = loop delay of w; w = A u + B (all sections are first order and linear in their input).
            float a = 1, b = 0;
            pathResponse(a, b);
            float t = 0, u = 0;
            if (loopDelay_ < 1.0f)
            {
                const float delayedB = thiran_ * b + thiranState_;
                u = (x + kind_.sign * r_ * delayedB) / (1 - kind_.sign * r_ * thiran_ * a);
                u = LadderFilter::inputClip(u);
                w = processPath(u);
                t = thiran_ * w + thiranState_;
                thiranState_ = w - thiran_ * t;
            }
            else
            {
                const float d = std::min(loopDelay_, static_cast<float>(history_.size() - 2));
                const int whole = static_cast<int>(d);
                const float frac = d - static_cast<float>(whole);
                const auto past = [&](int n) { return history_[static_cast<size_t>((historyAt_ - n) & static_cast<int>(history_.size() - 1))]; };
                t = past(whole - 1) * (1 - frac) + past(whole) * frac; // past(0) is the previous sample's w
                u = LadderFilter::inputClip(x + kind_.sign * r_ * t);
                w = processPath(u);
            }
            historyAt_ = (historyAt_ + 1) & static_cast<int>(history_.size() - 1);
            history_[static_cast<size_t>(historyAt_)] = w;
        }
        const float out = dry_ * x + wet_ * w;
        return std::isfinite(out) ? out : (reset(), 0.0f);
    }
    // Magnitude of the measured model at the reference rate (editor curves).
    static float magnitude(int shape, float hz, float cutoffHz, float resonance, float var)
    {
        const Kind k = kind(shape);
        const double r = std::clamp(static_cast<double>(resonance), 0.0, 1.0), fs = kReferenceRate;
        const double fc = std::clamp(static_cast<double>(cutoffHz), kMinimumHz, 22000.0);
        const std::complex<double> z1 = std::polar(1.0, -2 * 3.14159265358979323846 * hz / fs);
        const auto onePole = [&](double corner) {
            const double g = std::tan(3.14159265358979323846 * std::clamp(corner, 1.0, 42000.0) / fs);
            return g * (1.0 + z1) / ((1 + g) + (g - 1) * z1);
        };
        const auto plainPole = [&](double corner) {
            const double g = 1 - std::exp(-2 * 3.14159265358979323846 * std::clamp(corner, 1.0, 42000.0) / fs);
            return g / (1.0 - (1 - g) * z1);
        };
        std::complex<double> p = 1.0, loop = 1.0;
        if (k.family != Phase) p *= std::polar(1.0, -2 * 3.14159265358979323846 * hz / fc);
        if (k.stages > 0)
        {
            const double t = std::tan(3.14159265358979323846 * allpassCorner(fc) / fs), a = (t - 1) / (t + 1);
            p *= std::pow((a + z1) / (1.0 + a * z1), k.stages);
        }
        if (k.family == Phase) loop = z1;
        double low = 0, high = 0;
        loopCorners(k, fc, var, low, high);
        if (k.filter == LowPass || k.filter == Band) p *= (k.filter == LowPass ? plainPole(low) : onePole(low)) * (k.family == Phase ? 1.0 : z1);
        if (k.filter == HighPass || k.filter == Band) p *= (1.0 - onePole(high)) * (k.family != Phase && k.filter == HighPass ? std::pow(z1, 0.25) : 1.0);
        const auto w = p / (1.0 - k.sign * r * p * loop);
        return static_cast<float>(std::abs(0.5 / (1 + r) + k.sign * (k.family == Comb ? 0.5 * r : 0.5) * w));
    }

private:
    // Allpass stages then the loop filter, advancing their states.
    float processPath(float in)
    {
        for (int i = 0; i < kind_.stages; ++i)
        {
            auto& s = stage_[static_cast<size_t>(i)];
            const float v = allpass_ * (in - s), low = v + s; s = low + v;
            in = 2 * low - in;
        }
        if (kind_.filter == LowPass) { low_ += lowG_ * (in - low_); in = low_; } // plain one-pole
        else if (kind_.filter == Band) { const float v = lowG_ * (in - low_), low = v + low_; low_ = low + v; in = low; }
        if (kind_.filter == HighPass || kind_.filter == Band)
        {
            const float v = highG_ * (in - high_), low = v + high_; high_ = low + v; in -= low;
        }
        return in;
    }
    // The same path as an affine map of its input (output = a in + b) without advancing the states.
    void pathResponse(float& a, float& b) const
    {
        for (int i = 0; i < kind_.stages; ++i)
        {
            const float s = stage_[static_cast<size_t>(i)];
            a *= 2 * allpass_ - 1; b = (2 * allpass_ - 1) * b + 2 * (1 - allpass_) * s;
        }
        // Plain and TPT one-poles share the instantaneous form g in + (1 - g) state.
        if (kind_.filter == LowPass || kind_.filter == Band) { a *= lowG_; b = lowG_ * b + (1 - lowG_) * low_; }
        if (kind_.filter == HighPass || kind_.filter == Band) { a *= 1 - highG_; b = (1 - highG_) * (b - high_); }
    }
    // Catmull-Rom read `delay_` samples back; samples not written since reset() read as silence.
    float read(const float* line, int mask) const
    {
        const float d = std::clamp(delay_, 1.0f, static_cast<float>(mask - 4));
        const int whole = static_cast<int>(d);
        const float t = d - static_cast<float>(whole);
        const auto at = [&](int back) { return back <= written_ ? line[(write_ - back) & mask] : 0.0f; };
        const float y0 = at(whole - 1 < 1 ? 1 : whole - 1), y1 = at(whole), y2 = at(whole + 1), y3 = at(whole + 2);
        return y1 + 0.5f * t * (y2 - y0 + t * (2 * y0 - 5 * y1 + 4 * y2 - y3 + t * (3 * (y1 - y2) + y3 - y0)));
    }

    Kind kind_{Comb, 0, NoFilter, 1};
    int shape_ = CombP, write_ = 0, written_ = 0, historyAt_ = 0;
    float r_ = 0, dry_ = 0.5f, wet_ = 0, allpass_ = 0, lowG_ = 0, highG_ = 0, delay_ = 1, loopDelay_ = 0.5f, thiran_ = 1.0f / 3;
    float low_ = 0, high_ = 0, thiranState_ = 0;
    std::array<float, 8> stage_{};
    std::array<float, 8> history_{};
};
} // namespace sonora::dsp
