#pragma once
// Fixed-length stereo delay used for plugin delay compensation (ENG-12).
// prepare() allocates; everything else is audio-thread safe (no allocation, no locks).

#include <juce_audio_basics/juce_audio_basics.h>

#include <vector>

namespace sonora::engine
{

class DelayLine
{
public:
    // `delaySamples` == 0 makes every call a plain add/no-op and allocates nothing.
    void prepare(int delaySamples)
    {
        delay_ = std::max(0, delaySamples);
        pos_ = 0;
        for (auto& r : ring_)
            r.assign(delay_ > 0 ? (size_t) delay_ : 0, 0.0f);
    }

    int delay() const { return delay_; }
    void reset()
    {
        for (auto& r : ring_)
            std::fill(r.begin(), r.end(), 0.0f);
    }

    // dst += gain * (src delayed by `delay` samples)
    void addDelayed(const juce::AudioBuffer<float>& src, juce::AudioBuffer<float>& dst, int n, float gain)
    {
        if (delay_ == 0)
        {
            for (int ch = 0; ch < 2; ++ch)
                dst.addFrom(ch, 0, src, ch, 0, n, gain);
            return;
        }
        for (int ch = 0; ch < 2; ++ch)
        {
            auto& ring = ring_[(size_t) ch];
            const float* in = src.getReadPointer(ch);
            float* out = dst.getWritePointer(ch);
            size_t p = pos_;
            for (int i = 0; i < n; ++i)
            {
                out[i] += ring[p] * gain; // oldest sample == delay_ samples ago
                ring[p] = in[i];
                if (++p == ring.size())
                    p = 0;
            }
        }
        pos_ = (pos_ + (size_t) n) % (size_t) delay_;
    }

    // Feeds silence (the source is muted) so no stale audio comes out when it returns.
    void pushSilence(int n)
    {
        if (delay_ == 0)
            return;
        for (auto& ring : ring_)
        {
            size_t p = pos_;
            for (int i = 0; i < n; ++i)
            {
                ring[p] = 0.0f;
                if (++p == ring.size())
                    p = 0;
            }
        }
        pos_ = (pos_ + (size_t) n) % (size_t) delay_;
    }

    // In-place: buffer = buffer delayed by `delay` samples.
    void processInPlace(juce::AudioBuffer<float>& buf, int n)
    {
        if (delay_ == 0)
            return;
        for (int ch = 0; ch < 2; ++ch)
        {
            auto& ring = ring_[(size_t) ch];
            float* io = buf.getWritePointer(ch);
            size_t p = pos_;
            for (int i = 0; i < n; ++i)
            {
                const float old = ring[p];
                ring[p] = io[i];
                io[i] = old;
                if (++p == ring.size())
                    p = 0;
            }
        }
        pos_ = (pos_ + (size_t) n) % (size_t) delay_;
    }

private:
    int delay_ = 0;
    size_t pos_ = 0;
    std::vector<float> ring_[2];
};

} // namespace sonora::engine
