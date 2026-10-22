#pragma once

#include "devices/NativeDevice.h"
#include "dsp/Dsp.h"

namespace sonora::devices
{
// DEV-19: independently authored stereo / ping-pong / M-S feedback delay.
class Echo final : public NativeDevice
{
public:
    enum Param { Input, TimeL, TimeR, SyncL, SyncR, DivisionL, DivisionR, Feedback, Invert,
                 Mode, FilterOn, HighPass, LowPass, ModRate, ModAmount, Duck, Output, DryWet, NumParams };
    Echo();
    std::string name() const override { return "Sonora Echo"; }
    void prepare(double sampleRate, int) override;
    void reset() override;
    void process(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void setHostInfo(const HostInfo& info) override { tempo_ = std::isfinite(info.tempo) ? std::clamp(info.tempo, 20.0, 999.0) : 120; }
    double delayMilliseconds(int channel) const;
private:
    dsp::FractionalDelay delays_[2];
    dsp::Svf high_[2], low_[2];
    double sampleRate_ = 44100, phase_ = 0;
    std::atomic<double> tempo_{120};
    float detector_ = 0;
    std::array<float, 2> times_{};
};
}
