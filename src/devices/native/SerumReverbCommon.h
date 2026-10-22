#pragma once
// Shared pieces of the Serum 2 FX Reverb engines: the fractional pre-delay and the wet/dry/level output
// stage, which are identical for every reverb type. Allocation happens in allocate(); everything else is audio-thread safe.

#include <algorithm>
#include <cmath>
#include <vector>

namespace sonora::devices::serum_reverb
{

// Pre-delay: the write index leads, so the effective delay is `delaySamples + 1`.
class PreDelayLine
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
    float process(float input, double delaySamples) noexcept
    {
        double position = static_cast<double>(write_) - delaySamples;
        if (position < 0.0) position += static_cast<double>(length_);
        const int whole = std::clamp(static_cast<int>(position), 0, length_ - 1);
        const int next = whole + 1 < length_ ? whole + 1 : 0;
        const float low = buffer_[static_cast<size_t>(whole)];
        const float out = static_cast<float>(static_cast<double>(low) + static_cast<double>(buffer_[static_cast<size_t>(next)] - low) * (position - whole));
        if (++write_ >= length_) write_ = 0;
        buffer_[static_cast<size_t>(write_)] = input;
        return out;
    }
    int length() const noexcept { return length_; }

private:
    std::vector<float> buffer_;
    int length_ = 4, write_ = 0;
};

// Wet gain q = 0.5 - 0.5 cos(pi Wet), dry gain 1 - q^2, level 4 LevelOut^2 on the whole output. Every gain follows two cascaded one-pole smoothers
// whose coefficient grows with the distance between the two states (min(2 |a - b| + 2t / (1 + t), 1), t = tan(4 pi / fs)) and starts from silence,
// which is a fade-in of about a millisecond; the float smoothers stall ~1.5e-4 short of their target exactly like the reference.
class OutputStage
{
public:
    void prepare(double sampleRate) noexcept
    {
        const float t = std::tan(static_cast<float>(12.566370614359172 / sampleRate));
        base_ = (t + t) / (t + 1.0f);
        setMix(mixSetting_, levelTarget_);
        reset();
    }
    void reset() noexcept { wetA_ = wetB_ = dryA_ = dryB_ = levelA_ = levelB_ = 0.0f; }
    // `wet01` is the Wet knob (0..1); `level` the linear output gain (4 LevelOut^2).
    void setMix(float wet01, float level) noexcept
    {
        mixSetting_ = std::clamp(wet01, 0.0f, 1.0f);
        const float q = 0.5f - 0.5f * std::cos(mixSetting_ * 3.14159265358979f);
        wetTarget_ = q;
        dryTarget_ = 1.0f - q * q;
        levelTarget_ = level;
    }
    void process(float dryLeft, float dryRight, float wetLeft, float wetRight, float& outLeft, float& outRight) noexcept
    {
        const float levelCoef = std::min(std::abs(levelA_ - levelB_) * 2.0f + base_, 1.0f);
        levelA_ += (levelTarget_ - levelA_) * levelCoef;
        levelB_ += (levelA_ - levelB_) * levelCoef;
        const float wetCoef = std::min(std::abs(wetA_ - wetB_) * 2.0f + base_, 1.0f);
        const float dryCoef = std::min(std::abs(dryA_ - dryB_) * 2.0f + base_, 1.0f);
        wetA_ += (wetTarget_ - wetA_) * wetCoef;
        dryA_ += (dryTarget_ - dryA_) * dryCoef;
        wetB_ += (wetA_ - wetB_) * wetCoef;
        dryB_ += (dryA_ - dryB_) * dryCoef;
        outLeft = levelB_ * (wetB_ * wetLeft + dryB_ * dryLeft);
        outRight = levelB_ * (wetB_ * wetRight + dryB_ * dryRight);
    }

private:
    float base_ = 5e-4f, mixSetting_ = 0.2f, wetTarget_ = 0.2f, dryTarget_ = 0.96f, levelTarget_ = 1.0f;
    float wetA_ = 0, wetB_ = 0, dryA_ = 0, dryB_ = 0, levelA_ = 0, levelB_ = 0;
};

} // namespace sonora::devices::serum_reverb
