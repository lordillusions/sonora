#pragma once
#include "devices/NativeDevice.h"
#include "dsp/Dsp.h"
namespace sonora::devices
{
// Gate, measured against the reference (DEV-22b):
// - detector: mean power of L and R, one-pole envelope (2.5 ms up, 20 ms down); opens at >= Threshold + 0.75 dB,
//   closes below that - Return (dB), after Hold. (Fitted on event timing and presets: envelope error <= 0.45 dB.)
// - gain: one-pole towards 1 (open) or Floor (closed, dB; -75 == -inf), linear domain, tau = Attack when the gain
//   rises and Release when it falls (Flip mirrors both: it ducks to Floor with Attack and recovers with Release).
// - state starts closed with gain 0 (Flip: 1) and is held in that reset state during digital silence (the
//   reference suspends the device), so a note after silence fades in from 0.
class Gate final : public NativeDevice
{
public:
    enum Param { Threshold, Attack, Hold, Release, Return, Floor, Flip, Listen, Lookahead, NumParams };
    static constexpr float kDetectorUpMs = 2.5f, kDetectorDownMs = 20.0f, kThresholdOffsetDb = 0.75f;
    static constexpr int kSilenceResetSamples = 512;
    Gate();
    std::string name() const override { return "Gate"; }
    void prepare(double, int) override;
    void reset() override;
    void process(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void setSidechain(const juce::AudioBuffer<float>* source, float gain) override { sidechain_ = source; sidechainGain_ = gain; }
    int latencySamples() const override { return latency_; }
    float reduction() const { return reduction_.load(std::memory_order_relaxed); }
private:
    void resetState();
    double sampleRate_ = 44100;
    dsp::FractionalDelay delay_[2];
    const juce::AudioBuffer<float>* sidechain_ = nullptr;
    float sidechainGain_ = 1, gain_ = 0, detector_ = 0;
    int hold_ = 0, latency_ = 0, silent_ = 0;
    bool open_ = false, flipState_ = false;
    std::atomic<float> reduction_{0};
};
class AutoPan final : public NativeDevice
{
public:
    enum Param { Amount, Rate, Sync, Beat, Shape, Phase, Offset, Invert, On, NumParams };
    AutoPan();
    std::string name() const override { return "Auto Pan"; }
    void prepare(double rate, int) override { sampleRate_ = rate; reset(); }
    void reset() override { phase_ = 0; }
    void process(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void setHostInfo(const HostInfo& host) override { tempo_ = host.tempo; }
    static float waveform(double phase, int shape);
private:
    double sampleRate_ = 44100, phase_ = 0, tempo_ = 120;
};
}
