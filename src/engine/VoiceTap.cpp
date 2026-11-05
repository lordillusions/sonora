#include "engine/VoiceTap.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace sonora::engine
{

SpscFloatRing::SpscFloatRing(size_t capacityPow2) : buffer_(capacityPow2), mask_(capacityPow2 - 1) {}

size_t SpscFloatRing::write(const float* data, size_t n) noexcept
{
    const size_t head = head_.load(std::memory_order_relaxed);
    const size_t tail = tail_.load(std::memory_order_acquire);
    n = std::min(n, buffer_.size() - (head - tail));
    for (size_t i = 0; i < n;)
    {
        const size_t at = (head + i) & mask_;
        const size_t run = std::min(n - i, buffer_.size() - at);
        std::memcpy(buffer_.data() + at, data + i, run * sizeof(float));
        i += run;
    }
    head_.store(head + n, std::memory_order_release);
    return n;
}

size_t SpscFloatRing::read(float* data, size_t n) noexcept
{
    const size_t tail = tail_.load(std::memory_order_relaxed);
    const size_t head = head_.load(std::memory_order_acquire);
    n = std::min(n, head - tail);
    for (size_t i = 0; i < n;)
    {
        const size_t at = (tail + i) & mask_;
        const size_t run = std::min(n - i, buffer_.size() - at);
        std::memcpy(data + i, buffer_.data() + at, run * sizeof(float));
        i += run;
    }
    tail_.store(tail + n, std::memory_order_release);
    return n;
}

size_t SpscFloatRing::available() const noexcept
{
    return head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire);
}

void SpscFloatRing::clearFromReader() noexcept { tail_.store(head_.load(std::memory_order_acquire), std::memory_order_release); }

VoiceTap::VoiceTap() : capture_(1u << 17), playback_(1u << 16) {}

void VoiceTap::process(const float* const* physical, int numPhysical, float* const* outputs, int numOutputs, int numSamples,
                       double sampleRate) noexcept
{
    if (numSamples <= 0 || sampleRate <= 0.0) return;
    sampleRate_.store(sampleRate, std::memory_order_relaxed);
    if (! active.load(std::memory_order_relaxed))
    {
        holdLeft_ = 0;
        duck_ = 1.0f;
        wasPlaying_ = false;
        micPeak_.store(0.0f, std::memory_order_relaxed);
        return;
    }

    // ---- the microphone
    int channel = micChannel.load(std::memory_order_relaxed);
    if (channel < 0 && physical != nullptr)
        for (int i = 0; i < numPhysical && channel < 0; ++i)
            if (physical[i] != nullptr) channel = i;
    const float* mic = physical != nullptr && channel >= 0 && channel < numPhysical ? physical[channel] : nullptr;
    if (const float hz = testToneHz.load(std::memory_order_relaxed); hz > 0.0f)
    {
        hasInput_.store(true, std::memory_order_relaxed);
        const double step = 2.0 * 3.14159265358979323846 * hz / sampleRate;
        for (int offset = 0; offset < numSamples; offset += maxChunk)
        {
            const int n = std::min(maxChunk, numSamples - offset);
            for (int i = 0; i < n; ++i)
            {
                tone_[(size_t) i] = 0.25f * (float) std::sin(tonePhase_);
                tonePhase_ += step;
            }
            if (tonePhase_ > 1.0e6) tonePhase_ = std::fmod(tonePhase_, 2.0 * 3.14159265358979323846);
            micPeak_.store(0.25f, std::memory_order_relaxed);
            if (capturing.load(std::memory_order_relaxed)) capture_.write(tone_.data(), (size_t) n);
        }
        mic = nullptr;
    }
    else hasInput_.store(mic != nullptr, std::memory_order_relaxed);
    if (mic != nullptr)
    {
        for (int i = 0; i < numSamples; ++i) peakHold_ = std::max(peakHold_, std::abs(mic[i]));
        peakSamples_ += numSamples;
        if (peakSamples_ >= (int) (sampleRate * 0.05))
        {
            micPeak_.store(peakHold_, std::memory_order_relaxed);
            peakHold_ = 0.0f;
            peakSamples_ = 0;
        }
        if (capturing.load(std::memory_order_relaxed)) capture_.write(mic, (size_t) numSamples);
    }

    // ---- the other people's voices, after Main
    if (numOutputs <= 0 || outputs == nullptr) return;
    float* left = outputs[0];
    float* right = numOutputs > 1 ? outputs[1] : nullptr;
    const float gain = voiceGain.load(std::memory_order_relaxed);
    const float duckTo = std::clamp(duckGain.load(std::memory_order_relaxed), 0.0f, 1.0f);
    const int hold = (int) (0.3 * sampleRate);              // the duck lets go 300 ms after the last voice
    const float glide = 1.0f / (0.05f * (float) sampleRate); // 50 ms gain glide
    for (int offset = 0; offset < numSamples; offset += maxChunk)
    {
        const int n = std::min(maxChunk, numSamples - offset);
        const size_t got = playback_.read(voice_.data(), (size_t) n);
        if (got == 0 && ! wasPlaying_ && duck_ >= 1.0f) continue; // nothing to add, nothing ducked
        if (got < (size_t) n)
        {
            if (wasPlaying_ && got == 0) underruns_.fetch_add(1, std::memory_order_relaxed);
            std::fill(voice_.begin() + (std::ptrdiff_t) got, voice_.begin() + n, 0.0f);
        }
        wasPlaying_ = got > 0;
        for (int i = 0; i < n; ++i)
        {
            const float v = voice_[(size_t) i];
            if (std::abs(v) > 0.003f) holdLeft_ = hold; // about -50 dBFS
            else if (holdLeft_ > 0) --holdLeft_;
            const float target = holdLeft_ > 0 ? duckTo : 1.0f;
            duck_ += (target - duck_) * glide;
            const float add = v * gain;
            if (left != nullptr) left[offset + i] = left[offset + i] * duck_ + add;
            if (right != nullptr) right[offset + i] = right[offset + i] * duck_ + add;
        }
        if (duck_ > 0.9999f && holdLeft_ == 0) duck_ = 1.0f;
    }
}

} // namespace sonora::engine
