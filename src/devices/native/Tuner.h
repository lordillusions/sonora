#pragma once
#include "devices/NativeDevice.h"
#include "dsp/Dsp.h"
namespace sonora::devices
{
// DEV-20: bounded, decimated YIN analysis. Audio remains bit-for-bit unchanged.
class Tuner final : public NativeDevice
{
public:
    enum Param { Reference, DisplayHz, ViewMode, Strobe, Spelling, NumParams };
    Tuner();
    std::string name() const override { return "Tuner"; }
    void prepare(double, int) override;
    void reset() override;
    void process(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    float frequency() const { return frequency_.load(std::memory_order_relaxed); }
    float confidence() const { return confidence_.load(std::memory_order_relaxed); }
private:
    void analyse();
    static constexpr int kWindow = 2048, kMaxLag = 512;
    std::array<float, kWindow> ring_{};
    std::array<float, kMaxLag + 1> differences_{}, rawDifferences_{};
    int write_ = 0, filled_ = 0, hop_ = 0, decimation_ = 1, countdown_ = 0;
    double analysisRate_ = 8000;
    dsp::Svf lowpass_;
    std::atomic<float> frequency_{0}, confidence_{0};
};
}
