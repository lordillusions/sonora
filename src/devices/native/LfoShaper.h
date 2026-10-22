#pragma once
// Sonora LFO: tempo/Hz volume and pan shaping with a drawable envelope (DEV-16).
#include "devices/NativeDevice.h"

namespace sonora::devices
{
class LfoShaper final : public NativeDevice
{
public:
    enum Param { Shape, Sync, Rate, Hertz, Depth, Phase, StereoPhase, PanAmount, Smooth, Mix, Trigger, PointStart };
    static constexpr int kPoints = 17;
    LfoShaper();
    std::string name() const override { return "Sonora LFO"; }
    void prepare(double sampleRate, int) override;
    void reset() override;
    void setHostInfo(const HostInfo& info) override { host_ = info; }
    void process(juce::AudioBuffer<float>& audio, juce::MidiBuffer& midi) override;
    float levelAt(double phase) const;
    std::vector<int> displayParameters() const override;

private:
    HostInfo host_;
    double sampleRate_ = 44100, phase_ = 0;
    float smoothedL_ = 1, smoothedR_ = 1;
    bool triggered_ = false;
};
} // namespace sonora::devices
