#pragma once
// Native equivalent of the Serum 2 FX Reverb "Hall" algorithm (DEV-18d). Topology, delay law, decay law, filter shapes, output stage and the
// start-up state were measured against the reference: the output agrees with the reference to
// 0.1-0.5 % over seconds of noise for every control. Nothing here is vendor code.
//
// Signal flow per channel (left shown; right mirrors it with the roles of the two tank lines swapped):
//   dry -> pre-delay -> (+ the other channel's tank output) -> high shelf -> low shelf -> 3 modulated all-passes (g = 0.625)
//   -> tank delay with an exponential decay gain -> clipped to +-1 -> the other channel's input.
// The wet output is the all-pass chain output. The all-pass delays are given in milliseconds and scale with Size on an exponential law;
// the (short) tank delays are given in samples. One LFO modulates the all-pass and tank delays with staggered phases.
//
// Allocation happens in prepare(); everything else is audio-thread safe.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

#include "devices/native/SerumReverbCommon.h"

namespace sonora::devices::serum_hall
{

// Shelf built from a TPT state-variable filter (k = 10/3). `u` (0..1) sets the corner and the cut depth together:
// low shelf corner 90 + 270 u Hz, high shelf corner 3330 - 1000 u^2 Hz; the shelf gain is 0.25^(u^2) per band edge (squared at the far end).
class Shelf
{
public:
    void design(bool highShelf, float u, double sampleRate) noexcept
    {
        constexpr float k = 3.3333333f;
        const float corner = highShelf ? u * u * -1000.0f + 3330.0f : u * 270.0f + 90.0f;
        const float gain = std::exp(u * u * -1.3815511f);
        float g = std::tan(static_cast<float>(3.14159265358979323846 * corner / sampleRate));
        if (highShelf)
        {
            g *= std::sqrt(gain);
            c0_ = gain * gain;
            c1_ = k * gain - k * gain * gain;
            c2_ = 1.0f - gain * gain;
        }
        else
        {
            g /= std::sqrt(gain);
            c0_ = 1.0f;
            c1_ = k * gain - k;
            c2_ = gain * gain - 1.0f;
        }
        g_ = g;
        a1_ = 1.0f / ((g + k) * g + 1.0f);
    }
    void reset() noexcept { s1_ = s2_ = 0.0f; }
    float process(float x) noexcept
    {
        const float v1 = (s1_ + g_ * (x - s2_)) * a1_;
        const float v2 = s2_ + g_ * v1;
        s1_ = 2.0f * v1 - s1_;
        s2_ = 2.0f * v2 - s2_;
        return c0_ * x + c1_ * v1 + c2_ * v2;
    }

private:
    float s1_ = 0, s2_ = 0, g_ = 0, a1_ = 1, c0_ = 1, c1_ = 0, c2_ = 0;
};

// Circular buffer read through a first-order all-pass fractional delay, wrapped into a Schroeder all-pass (g = 0.625).
class ModulatedAllpass
{
public:
    void allocate(int length)
    {
        buffer_.assign(static_cast<size_t>(length), 0.0f);
        length_ = length;
        write_ = 0;
        last_ = 0.0f;
    }
    void reset() noexcept
    {
        std::fill(buffer_.begin(), buffer_.end(), 0.0f);
        write_ = 0;
        last_ = 0.0f;
    }
    float process(float input, float delaySamples) noexcept
    {
        delaySamples = std::clamp(delaySamples, 1.0f, static_cast<float>(length_ - 2));
        const int whole = static_cast<int>(delaySamples);
        int index = write_ - whole;
        if (index < 0) index += length_;
        const int base = index < 1 ? length_ : 0;
        const float eta = (1.0f - delaySamples) + static_cast<float>(whole);
        float delayed = buffer_[static_cast<size_t>(index + base - 1)] + eta * (buffer_[static_cast<size_t>(index)] - last_);
        if (! std::isfinite(delayed)) delayed = 0.0f;
        last_ = delayed;
        const float fed = input + delayed * 0.625f;
        buffer_[static_cast<size_t>(write_)] = fed;
        if (++write_ >= length_) write_ = 0;
        return delayed - fed * 0.625f;
    }

private:
    std::vector<float> buffer_;
    int length_ = 4, write_ = 0;
    float last_ = 0.0f;
};

// Linear-interpolating delay (read, then write); the delay is in samples.
class TankLine
{
public:
    void allocate(int length)
    {
        buffer_.assign(static_cast<size_t>(length), 0.0f);
        length_ = length;
        write_ = 0;
    }
    void reset() noexcept
    {
        std::fill(buffer_.begin(), buffer_.end(), 0.0f);
        write_ = 0;
    }
    float process(float input, float delaySamples) noexcept
    {
        delaySamples = std::clamp(delaySamples, 0.0f, static_cast<float>(length_ - 2));
        float position = static_cast<float>(write_) - delaySamples;
        if (position < 0.0f) position += static_cast<float>(length_);
        const int whole = std::clamp(static_cast<int>(position), 0, length_ - 1);
        const int next = whole + 1 >= length_ ? 0 : whole + 1;
        const float low = buffer_[static_cast<size_t>(whole)];
        const float out = low + (buffer_[static_cast<size_t>(next)] - low) * (position - static_cast<float>(whole));
        buffer_[static_cast<size_t>(write_)] = input;
        if (++write_ >= length_) write_ = 0;
        return out;
    }

private:
    std::vector<float> buffer_;
    int length_ = 4, write_ = 0;
};

struct Parameters
{
    float size = 0.35f;  // 0..1; every all-pass and tank delay is scaled by 32^size
    float decay = 0.35f; // 0..1; tank loss per delay unit = 0.44 * exp(-6.9315 decay - 3.9856)
    float low = 0.0f;    // 0..1; low-shelf cut (the reference's "Freq")
    float high = 0.35f;  // 0..1; high-shelf cut (the reference's "FreqB")
    float rate = 0.25f;  // 0..1; LFO rate = 2 * rate Hz (the reference's "Feedback")
    float depth = 0.2f;  // 0..1; modulation depth (the reference's "Width")
    float preDelaySeconds = 0.0f;
};

class Hall
{
public:
    void prepare(double sampleRate)
    {
        sampleRate_ = sampleRate;
        const int length = std::max(64, static_cast<int>(sampleRate * 1.049));
        for (auto& line : leftChain_) line.allocate(length);
        for (auto& line : rightChain_) line.allocate(length);
        tankLeft_.allocate(length);
        tankRight_.allocate(length);
        preLeft_.allocate(length);
        preRight_.allocate(length);
        maximumPreDelay_ = static_cast<double>(length) - 2.0;
        smoothing_ = static_cast<float>(44100.0 / (sampleRate * 3000.0 + 44100.0));
        output_.prepare(sampleRate);
        setParameters(parameters_);
        reset();
    }

    // Clears every buffer and returns to the reference's start-up state: the delay glides begin from the delays of its default Size (35 %),
    // copied in order into [tank-left, tank-right, left all-passes 1..3, right all-passes 1..3] (so the tank delays begin at the first two
    // all-pass lengths and every all-pass at its neighbour's length), then follow the current Size with a one-pole of about 68 ms. The LFO
    // starts at phase 0 and the output gains fade in from silence over about a millisecond.
    void reset() noexcept
    {
        for (auto& line : leftChain_) line.reset();
        for (auto& line : rightChain_) line.reset();
        tankLeft_.reset();
        tankRight_.reset();
        preLeft_.reset();
        preRight_.reset();
        for (auto& shelf : shelves_) shelf.reset();
        feedLeft_ = feedRight_ = 0.0f;
        phase_ = 0.0f;
        const auto defaults = unitsFor(0.35f);
        tankDelayLeft_ = defaults[0];
        tankDelayRight_ = defaults[1];
        stageLeft_ = {defaults[2], defaults[3], defaults[4]};
        stageRight_ = {defaults[5], defaults[6], defaults[7]};
        output_.reset();
    }

    void setParameters(const Parameters& p) noexcept
    {
        parameters_ = p;
        const auto units = unitsFor(p.size);
        targetLeft_ = {units[0], units[1], units[2]};
        tankTargetLeft_ = units[3];
        targetRight_ = {units[4], units[5], units[6]};
        tankTargetRight_ = units[7];
        decayRate_ = 0.44000003f * std::exp(std::clamp(p.decay, 0.0f, 1.0f) * -6.931472f - 3.9855964f);
        phaseStep_ = (std::clamp(p.rate, 0.0f, 1.0f) * 2.0f) / static_cast<float>(sampleRate_);
        depth2_ = std::clamp(p.depth, 0.0f, 1.0f) * 2.0f;
        const float low = std::clamp(p.low, 0.0f, 1.0f), high = std::clamp(p.high, 0.0f, 1.0f);
        shelves_[0].design(true, high, sampleRate_);
        shelves_[1].design(false, low, sampleRate_);
        shelves_[2].design(true, high, sampleRate_);
        shelves_[3].design(false, low, sampleRate_);
        preDelaySamples_ = std::clamp(static_cast<double>(p.preDelaySeconds) * sampleRate_, 0.0, maximumPreDelay_);
    }

    // `wet01` is the Wet knob (0..1), `level` the linear output gain (4 LevelOut^2); see serum_reverb::OutputStage.
    void setMix(float wet01, float level) noexcept { output_.setMix(wet01, level); }

    // One stereo sample in, one out: wet and dry mixed and scaled.
    void process(float inLeft, float inRight, float& outLeft, float& outRight) noexcept
    {
        const float preL = preLeft_.process(inLeft, preDelaySamples_);
        const float preR = preRight_.process(inRight, preDelaySamples_);
        phase_ += phaseStep_;
        if (phase_ >= 1.0f) phase_ -= 1.0f;
        float left = shelves_[1].process(shelves_[0].process(feedRight_ + preL));
        float right = shelves_[3].process(shelves_[2].process(feedLeft_ + preR));
        constexpr std::array<float, 3> offsets{0.0f, 0.071428575f, 0.14285715f};
        const float samplesPerUnit = static_cast<float>(sampleRate_ * 0.001);
        for (size_t stage = 0; stage < 3; ++stage)
        {
            const float wobble = std::sin((phase_ - offsets[stage]) * 6.2831855f) * depth2_;
            stageLeft_[stage] += smoothing_ * ((targetLeft_[stage] - stageLeft_[stage]) + wobble);
            left = leftChain_[stage].process(left, stageLeft_[stage] * samplesPerUnit);
            stageRight_[stage] += smoothing_ * (targetRight_[stage] - (wobble + stageRight_[stage]));
            right = rightChain_[stage].process(right, stageRight_[stage] * samplesPerUnit);
        }
        const float wobble = std::sin(phase_ * 6.2831855f - 1.3463969f) * depth2_;
        tankDelayLeft_ += (tankTargetLeft_ - tankDelayLeft_) * smoothing_;
        tankDelayRight_ += (tankTargetRight_ - tankDelayRight_) * smoothing_;
        const float lengthLeft = tankDelayLeft_ + wobble, lengthRight = tankDelayRight_ - wobble;
        const float gainLeft = std::exp(-6.907755278982137f * decayRate_ * lengthLeft);
        const float gainRight = std::exp(-6.907755278982137f * decayRate_ * lengthRight);
        feedLeft_ = std::clamp(tankLeft_.process(left, lengthLeft) * gainLeft, -1.0f, 1.0f);
        feedRight_ = std::clamp(tankRight_.process(right, lengthRight) * gainRight, -1.0f, 1.0f);
        if (! std::isfinite(feedLeft_)) feedLeft_ = 0.0f;
        if (! std::isfinite(feedRight_)) feedRight_ = 0.0f;
        output_.process(inLeft, inRight, left, right, outLeft, outRight);
    }

private:
    // The eight delay lengths for a Size: three all-passes and the tank, left then right. Units are milliseconds, except that the tank
    // delays are applied as sample counts.
    static std::array<float, 8> unitsFor(float rawSize) noexcept
    {
        const float size = std::clamp(rawSize, 0.0f, 1.0f);
        const float exponent = (30.0f - 60.0f * size) * 0.057762265f;
        constexpr std::array<float, 8> offsets{3.5234983f, 3.7545474f, 3.9855964f, 4.389932f, 3.4079738f, 3.6390228f, 3.870072f, 4.274408f};
        std::array<float, 8> units{};
        for (size_t i = 0; i < units.size(); ++i) units[i] = 1000.0f / (440.0f * std::exp(exponent - offsets[i]));
        return units;
    }

    double sampleRate_ = 48000.0, maximumPreDelay_ = 1000.0, preDelaySamples_ = 0.0;
    Parameters parameters_{};
    std::array<ModulatedAllpass, 3> leftChain_{}, rightChain_{};
    TankLine tankLeft_, tankRight_;
    serum_reverb::PreDelayLine preLeft_, preRight_;
    serum_reverb::OutputStage output_;
    std::array<Shelf, 4> shelves_{};
    std::array<float, 3> targetLeft_{}, targetRight_{}, stageLeft_{}, stageRight_{};
    float tankTargetLeft_ = 0, tankTargetRight_ = 0, tankDelayLeft_ = 0, tankDelayRight_ = 0;
    float feedLeft_ = 0, feedRight_ = 0, phase_ = 0, phaseStep_ = 0, depth2_ = 0, decayRate_ = 0, smoothing_ = 3e-4f;
};

} // namespace sonora::devices::serum_hall
