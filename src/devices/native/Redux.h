#pragma once
#include "devices/NativeDevice.h"
#include "devices/native/Biquad.h"
#include "dsp/Dsp.h"

#include <array>

namespace sonora::devices
{
// DEV-20 / DEV-22b: Redux 2 (.als "Redux2"), laws measured against the reference:
//   pre filter (optional): 16th-order Butterworth low-pass at Rate / 2
//   sample & hold in continuous time: samples the (cubic-interpolated) input at k / Rate (+ jitter) and outputs
//     band-limited steps, so the response is the zero-order-hold sinc without host-rate aliasing of the steps
//   quantiser: mu-law (mu = 2^(8 Shape) - 1) companded, step 2^(1 - Bits), DC Shift = mid-rise levels
//   post filter (optional): 16th-order Butterworth low-pass at Rate / 2 * 2^PostFilterValue
//   DC blocker (one pole, tau 31.8 ms), then linear dry/wet. Like the reference (which suspends silent devices), the
//   state resets once the input has been silent for 512 samples and the output has decayed.
// The band-limited steps use a linear-phase windowed-sinc kernel; the device reports the reference's 64 samples of
// latency (measured: its output leads a silent-to-sound onset by 64 samples).
class Redux final : public NativeDevice
{
public:
    enum Param { Rate, Jitter, Bits, Shape, Dc, PreFilter, PostFilter, PostOctave, DryWet, NumParams };
    static constexpr int kHalfKernel = 16, kPhases = 512, kStages = 8, kLatency = 64, kRing = kLatency + kHalfKernel + 2;
    Redux();
    std::string name() const override { return "Redux"; }
    void prepare(double, int) override;
    void reset() override;
    void process(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    int latencySamples() const override { return kLatency; }
    // Quantiser law (for tests): value after Bits / Shape / DC Shift.
    static float quantize(float x, float bits, float shape, bool dcShift);
    // Without jitter the sampling grid is locked to the song position: instants at kGridOffsetSeconds + k / Rate
    // (measured: the reference's grid phase is identical across exports at 240 / 300 / 7433 / 8000 Hz).
    static constexpr double kGridOffsetSeconds = 24223.75 / 44100.0;
    void setHostInfo(const HostInfo& info) override
    {
        tempo_.store(info.tempo, std::memory_order_relaxed);
        beats_.store(info.beats, std::memory_order_relaxed);
        playing_.store(info.playing, std::memory_order_relaxed);
    }

private:
    struct Channel
    {
        std::array<SvfState, kStages> pre, post; // state-variable form: Rate / Post moves must not click (DEV-24)
        std::array<float, 4> history{};                       // last four (pre-filtered) input samples, oldest first
        std::array<float, kRing> residual{};                  // band-limited step corrections for the next outputs
        std::array<float, kRing> heldDelay{}, dryDelay{};     // naive held value and dry input (all rings share pos)
        float held = 0, dcX = 0, dcY = 0;
        double nextSample = 0; // next sampling instant, in samples relative to history[2] (the interpolation interval start)
        uint32_t random = 1;
        int pos = 0, silent = 512; // starts suspended: the reference wakes a device when sound arrives
    };
    void setupFilters();
    double sampleRate_ = 44100;
    float lastRate_ = -1, lastPost_ = -100;
    std::atomic<double> tempo_{120.0}, beats_{0.0};
    std::atomic<bool> playing_{false};
    std::array<SvfCoeffs, kStages> preCoeffs_{}, postCoeffs_{};
    std::array<Channel, 2> channels_;
    // Integrated windowed-sinc minus unit step, sampled at kPhases sub-sample offsets over +-kHalfKernel samples.
    static const std::array<float, (2 * kHalfKernel) * kPhases + 1>& residualTable();
};
}
