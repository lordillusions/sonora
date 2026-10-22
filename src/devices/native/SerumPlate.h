#pragma once
// Native equivalent of the Serum 2 FX Reverb "Plate" algorithm (the default reverb type, DEV-18d).
// Structure (as in the reference, constants confirmed black-box): the stereo input is summed to mono, passes one
// 15 ms all-pass, a two-pole low-pass (FreqB), and a two-pole high-pass (Freq), then feeds five parallel feedback combs per channel
// (prime lengths from a 42.6..54.8 ms table, damped through a one-pole, read at a Size dependent integer delay), a series of six
// all-passes (g = 0.68) per channel and a width mixer. Nothing here is vendor code.
//
// Allocation happens in prepare(); everything else is audio-thread safe.

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

#include "devices/native/SerumReverbCommon.h"

namespace sonora::devices::serum_plate
{

// Smallest prime not below `n` (the reference sizes every buffer this way); n < 4 is returned unchanged.
inline int nextPrime(int n) noexcept
{
    n = std::max(n, 2);
    for (;; ++n)
    {
        bool prime = true;
        for (int d = 2; d * d <= n; ++d)
            if (n % d == 0) { prime = false; break; }
        if (prime) return n;
    }
}

// Two cascaded bilinear one-poles with the same coefficient: out = (in + in_prev) b - out_prev a.
class TwoPole
{
public:
    void setCoefficient(float c) noexcept { a_ = c - 1.0f; b_ = c * 0.5f; }
    void reset() noexcept { previousInput_ = firstOutput_ = secondOutput_ = 0.0f; }
    float process(float x) noexcept
    {
        const float first = (previousInput_ + x) * b_ - firstOutput_ * a_;
        const float second = (first + firstOutput_) * b_ - secondOutput_ * a_;
        firstOutput_ = first;
        secondOutput_ = second;
        previousInput_ = x;
        return second;
    }

private:
    float a_ = 0, b_ = 0, previousInput_ = 0, firstOutput_ = 0, secondOutput_ = 0;
};

// Feedback all-pass of fixed length: w = x + g d, out = d - g w (d is the oldest sample).
class Allpass
{
public:
    void allocate(int length, float gain)
    {
        buffer_.assign(static_cast<size_t>(length), 0.0f);
        length_ = length;
        gain_ = gain;
        write_ = 0;
    }
    void reset() noexcept { std::fill(buffer_.begin(), buffer_.end(), 0.0f); write_ = 0; }
    float process(float input) noexcept
    {
        const float delayed = buffer_[static_cast<size_t>(write_)];
        const float fed = input + delayed * gain_;
        buffer_[static_cast<size_t>(write_)] = fed;
        if (++write_ >= length_) write_ = 0;
        return delayed - fed * gain_;
    }
    // The state-sharing variant used for the input diffuser: also returns the oldest sample and fed value.
    float processRaw(float input, float& delayed, float& fed) noexcept
    {
        delayed = buffer_[static_cast<size_t>(write_)];
        fed = input + delayed * gain_;
        buffer_[static_cast<size_t>(write_)] = fed;
        if (++write_ >= length_) write_ = 0;
        return gain_;
    }

private:
    std::vector<float> buffer_;
    int length_ = 2, write_ = 0;
    float gain_ = 0.68f;
};

// Feedback comb with a Size dependent integer read delay and a one-pole in the feedback path (the raw delayed sample is the comb output).
class Comb
{
public:
    void allocate(int length)
    {
        buffer_.assign(static_cast<size_t>(length), 0.0f);
        length_ = length;
        write_ = 0;
        damped_ = 0.0f;
    }
    void reset() noexcept { std::fill(buffer_.begin(), buffer_.end(), 0.0f); write_ = 0; damped_ = 0.0f; }
    // `offset` is 0.395 (left) / 0.398 (right); `size` the 1 - (1 - s)^6 curve; `damping` the one-pole coefficient.
    float process(float input, float size, float offset, float damping) noexcept
    {
        const int delay = static_cast<int>(static_cast<float>(length_ - 2) * (size * 0.582f + offset) + 1.0f);
        int index = write_ - delay;
        if (index < 0) index += length_;
        const float delayed = buffer_[static_cast<size_t>(index)];
        damped_ = delayed + damping * (damped_ - delayed);
        buffer_[static_cast<size_t>(write_)] = input + size * 0.97f * damped_;
        if (++write_ >= length_) write_ = 0;
        return delayed;
    }

private:
    std::vector<float> buffer_;
    int length_ = 8, write_ = 0;
    float damped_ = 0.0f;
};

struct Parameters
{
    float size = 0.35f;     // 0..1 (reverb time and comb length)
    float highPass = 0.0f;  // 0..1 (the reference's "Freq")
    float damping = 0.25f;  // 0..1 (the reference's "Feedback")
    float lowPass = 0.35f;  // 0..1 (the reference's "FreqB")
    float width = 0.2f;     // 0..1
    float preDelaySeconds = 0.0f;
};

class Plate
{
public:
    void prepare(double sampleRate)
    {
        sampleRate_ = sampleRate;
        constexpr std::array<float, 11> table{42.60164260864258f, 45.372501373291016f, 48.323577880859375f, 51.46659469604492f, 54.81403732299805f,
                                              58.37919998168945f, 62.17625427246094f, 66.22026062011719f, 70.52729797363281f, 75.11446380615234f, 80.0f};
        const float rate = static_cast<float>(sampleRate);
        for (size_t k = 0; k < 5; ++k)
        {
            const float even = (k & 1) == 0 ? 1.005f : 1.0f, odd = (k & 1) == 0 ? 1.0f : 1.005f;
            leftComb_[k].allocate(nextPrime(static_cast<int>(table[k] * rate * 0.001f * even)));
            rightComb_[k].allocate(nextPrime(static_cast<int>(table[k] * rate * 0.001f * odd)));
        }
        for (size_t k = 0; k < 6; ++k)
        {
            leftAllpass_[k].allocate(nextPrime(static_cast<int>(rate * 0.00021f * table[4 + k])), 0.68f);
            rightAllpass_[k].allocate(nextPrime(static_cast<int>(rate * 0.00022000002f * table[4 + k])), 0.68f);
        }
        diffuser_.allocate(nextPrime(static_cast<int>(rate * 0.015000001f)), 0.68f);
        const int preLength = std::max(64, static_cast<int>(sampleRate * 1.049));
        preLeft_.allocate(preLength);
        preRight_.allocate(preLength);
        maximumPreDelay_ = static_cast<double>(preLength) - 2.0;
        // The filter coefficients are written for 44.1 kHz and scale with 44.1 kHz / max(fs, 44.1 kHz).
        rateScale_ = 44100.0f / std::max(rate, 44100.0f);
        output_.prepare(sampleRate);
        setParameters(parameters_);
        reset();
    }

    void reset() noexcept
    {
        for (auto& c : leftComb_) c.reset();
        for (auto& c : rightComb_) c.reset();
        for (auto& a : leftAllpass_) a.reset();
        for (auto& a : rightAllpass_) a.reset();
        diffuser_.reset();
        lowPass_.reset();
        highPass_.reset();
        preLeft_.reset();
        preRight_.reset();
        noise_ = 0.0f;
        output_.reset();
    }

    void setParameters(const Parameters& p) noexcept
    {
        parameters_ = p;
        const float size = std::clamp(p.size, 0.0f, 1.0f);
        float inverse = 1.0f - size;
        inverse = inverse - inverse * size;
        sizeCurve_ = 1.0f - inverse * inverse * inverse;
        damping_ = std::clamp(p.damping, 0.0f, 1.0f) * std::clamp(p.damping, 0.0f, 1.0f) * 0.95f;
        width_ = std::clamp(p.width, 0.0f, 1.0f);
        lowPass_.setCoefficient(coefficient(1.0f - std::clamp(p.lowPass, 0.0f, 1.0f)));
        highPass_.setCoefficient(coefficient(std::clamp(p.highPass, 0.0f, 1.0f) * 0.95f + 0.05f));
        preDelaySamples_ = std::clamp(static_cast<double>(p.preDelaySeconds) * sampleRate_, 0.0, maximumPreDelay_);
    }

    void setMix(float wet01, float level) noexcept { output_.setMix(wet01, level); }

    void process(float inLeft, float inRight, float& outLeft, float& outRight) noexcept
    {
        const float preL = preLeft_.process(inLeft, preDelaySamples_);
        const float preR = preRight_.process(inRight, preDelaySamples_);
        // The reference adds an alternating 1e-18 offset to the mono sum (denormal protection).
        const float previous = noise_;
        noise_ = 1e-18f - previous;
        const float mono = preR + preL + (1e-18f - previous);
        float delayed = 0, fed = 0;
        const float gain = diffuser_.processRaw(mono, delayed, fed);
        float x = std::min((mono + (delayed - fed * gain) * 0.2f) * 0.2f, 1.0f);
        if (x <= -1.0f) x = -1.0f;
        float y = std::min(lowPass_.process(x), 1.0f);
        if (y <= -1.0f) y = -1.0f;
        const float u = y - highPass_.process(y);

        float leftSum = 0, rightSum = 0;
        for (size_t k = 0; k < 5; ++k)
        {
            leftSum += leftComb_[k].process(u, sizeCurve_, 0.395f, damping_);
            rightSum += rightComb_[k].process(u, sizeCurve_, 0.398f, damping_);
        }
        float left = leftSum, right = rightSum;
        for (auto& a : leftAllpass_) left = a.process(left);
        for (auto& a : rightAllpass_) right = a.process(right);
        const float direct = width_ * 0.5f + 0.5f, cross = 0.5f - width_ * 0.5f;
        output_.process(inLeft, inRight, cross * right + left * direct, cross * left + right * direct, outLeft, outRight);
    }

private:
    // c = (-1.6 m + 3.6) m with m = (44.1 kHz scale * p)^2: the bilinear one-pole coefficient 2 g / (1 + g) of a cutoff growing with p.
    float coefficient(float p) const noexcept
    {
        float m = rateScale_ * p;
        m = m * rateScale_ * p;
        return static_cast<float>((m * -1.6 + 3.6) * m);
    }

    double sampleRate_ = 48000.0, maximumPreDelay_ = 1000.0, preDelaySamples_ = 0.0;
    Parameters parameters_{};
    std::array<Comb, 5> leftComb_{}, rightComb_{};
    std::array<Allpass, 6> leftAllpass_{}, rightAllpass_{};
    Allpass diffuser_;
    TwoPole lowPass_, highPass_;
    serum_reverb::PreDelayLine preLeft_, preRight_;
    serum_reverb::OutputStage output_;
    float rateScale_ = 0.91875f, sizeCurve_ = 0.0f, damping_ = 0.0f, width_ = 0.2f, noise_ = 0.0f;
};

} // namespace sonora::devices::serum_plate
