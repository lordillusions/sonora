#pragma once
// Limiter (.als "Limiter") — ROADMAP DEV-12.
//
// Look-ahead peak limiter: the audio is delayed by the look-ahead time while the gain needed to
// stay under the ceiling is computed ahead of it, smoothed over the look-ahead window (so the gain
// is already down when the peak arrives) and released with a one-pole follower. Reports its
// look-ahead as latency (plugin delay compensation). Not modelled: Live's "Maximize" / stereo-link
// variants and its exact release shapes.

#include "devices/NativeDevice.h"
#include "dsp/Dsp.h"
#include "engine/DelayLine.h"

#include <array>
#include <atomic>

namespace sonora::devices
{

class Limiter final : public NativeDevice
{
public:
    enum Param
    {
        Gain,        // input gain, dB
        Ceiling,     // dBFS
        Release,     // ms
        AutoRelease,
        LinkChannels,
        Lookahead,   // 0 = 1.5 ms, 1 = 3 ms, 2 = 6 ms
        NumParams
    };

    Limiter();
    std::string name() const override { return "Limiter"; }
    void prepare(double sampleRate, int maxBlockSize) override;
    void process(juce::AudioBuffer<float>& audio, juce::MidiBuffer& midi) override;
    void reset() override;
    int latencySamples() const override { return look_; }
    float gainReductionDb() const { return grMeter_.load(std::memory_order_relaxed); }
    // Per channel (0 left, 1 right), last block, for the face's meters (UI-42). Any thread.
    float gainReductionDb(int channel) const { return grChannel_[(size_t) (channel & 1)].load(std::memory_order_relaxed); }
    float outputPeakDb(int channel) const { return outChannel_[(size_t) (channel & 1)].load(std::memory_order_relaxed); }

private:
    struct Channel
    {
        std::vector<float> window;  // required gain, ring of look_ + 1
        std::vector<float> average; // box filter over the window minimum, ring of look_
        double sum = 0.0;
        int wpos = 0, apos = 0;
        float releaseGain = 1.0f;
    };

    double sampleRate_ = 44100.0;
    int look_ = 0;
    engine::DelayLine delay_;
    std::array<Channel, 2> ch_;
    std::vector<float> gains_[2]; // per-sample gain of the current block (sized in prepare)
    std::atomic<float> grMeter_{0.0f};
    std::array<std::atomic<float>, 2> grChannel_{}, outChannel_{};
};

} // namespace sonora::devices
