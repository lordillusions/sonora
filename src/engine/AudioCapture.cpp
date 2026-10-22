#include "engine/AudioCapture.h"

#include <algorithm>
#include <cmath>

namespace sonora::engine
{

void AudioCapture::prepare(const std::vector<bool>& active, double sampleRate)
{
    const std::lock_guard lock(mutex_);
    ready_.store(false, std::memory_order_release);
    slotOfPhysical_.fill(-1);
    int slots = 0;
    for (size_t i = 0; i < active.size() && i < slotOfPhysical_.size() && slots < kMaxChannels; ++i)
        if (active[i]) slotOfPhysical_[i] = slots++;
    capacity_ = sampleRate > 0 ? (int) std::ceil(sampleRate * kSeconds) : 0;
    sampleRate_ = sampleRate;
    rings_.assign((size_t) (capacity_ > 0 ? slots : 0), std::vector<float>((size_t) capacity_, 0.0f));
    written_.store(0, std::memory_order_release);
    ready_.store(! rings_.empty(), std::memory_order_release);
}

void AudioCapture::write(const float* const* physical, int numPhysical, int numSamples) noexcept
{
    if (! ready_.load(std::memory_order_acquire) || numSamples <= 0 || capacity_ <= 0) return;
    const int64_t written = written_.load(std::memory_order_relaxed);
    const int count = std::min(numSamples, capacity_);
    const int skip = numSamples - count; // a block longer than the ring keeps only its end
    const int start = (int) ((written + skip) % capacity_);
    const int first = std::min(count, capacity_ - start);
    for (int channel = 0; channel < std::min(numPhysical, (int) slotOfPhysical_.size()); ++channel)
    {
        const int slot = slotOfPhysical_[(size_t) channel];
        if (slot < 0) continue;
        float* ring = rings_[(size_t) slot].data();
        const float* in = physical != nullptr ? physical[channel] : nullptr;
        if (in == nullptr)
        {
            std::fill_n(ring + start, first, 0.0f);
            std::fill_n(ring, count - first, 0.0f);
            continue;
        }
        std::copy_n(in + skip, first, ring + start);
        std::copy_n(in + skip + first, count - first, ring);
    }
    written_.store(written + numSamples, std::memory_order_release);
}

bool AudioCapture::keeps(int channel) const
{
    const std::lock_guard lock(mutex_);
    return channel >= 0 && channel < (int) slotOfPhysical_.size() && slotOfPhysical_[(size_t) channel] >= 0 && ready_.load();
}

bool AudioCapture::read(int channel, bool stereo, juce::AudioBuffer<float>& out, double& sampleRate) const
{
    const std::lock_guard lock(mutex_);
    if (! ready_.load(std::memory_order_acquire) || channel < 0 || channel >= (int) slotOfPhysical_.size()) return false;
    const int left = slotOfPhysical_[(size_t) channel];
    const int right = stereo && channel + 1 < (int) slotOfPhysical_.size() ? slotOfPhysical_[(size_t) channel + 1] : left;
    if (left < 0 || right < 0) return false;
    const int64_t before = written_.load(std::memory_order_acquire);
    const int frames = (int) std::min<int64_t>(before, capacity_);
    if (frames <= 0) return false;
    out.setSize(2, frames, false, false, true);
    const int start = (int) ((before - frames) % capacity_);
    const int first = std::min(frames, capacity_ - start);
    for (int ch = 0; ch < 2; ++ch)
    {
        const float* ring = rings_[(size_t) (ch == 0 ? left : right)].data();
        std::copy_n(ring + start, first, out.getWritePointer(ch));
        std::copy_n(ring, frames - first, out.getWritePointer(ch) + first);
    }
    // Samples the writer replaced during the copy are the oldest ones: drop them.
    const int64_t overwritten = written_.load(std::memory_order_acquire) - before - (capacity_ - frames);
    if (overwritten > 0)
    {
        const int drop = (int) std::min<int64_t>(overwritten, frames);
        juce::AudioBuffer<float> trimmed(2, frames - drop);
        for (int ch = 0; ch < 2; ++ch) trimmed.copyFrom(ch, 0, out, ch, drop, frames - drop);
        out = std::move(trimmed);
    }
    sampleRate = sampleRate_;
    return out.getNumSamples() > 0;
}

} // namespace sonora::engine
