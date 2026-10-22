#pragma once

#include "devices/NativeDevice.h"
#include "devices/native/WavetableData.h"
#include "dsp/Dsp.h"

namespace sonora::devices
{
// DEV-19: compact, independent two-oscillator wavetable instrument.
class VectorSynth final : public NativeDevice
{
public:
    enum Param { Position1, Position2, Level1, Level2, Tune1, Tune2, Detune1, Detune2,
                 FilterOn, Cutoff, Resonance, FilterType, Attack, Decay, Sustain, Release,
                 LfoRate, LfoAmount, Volume, Pan, SubLevel, Transpose, NumParams };
    VectorSynth();
    std::string name() const override { return "Sonora Vector"; }
    bool isInstrument() const override { return true; }
    void prepare(double sampleRate, int) override;
    void reset() override;
    void process(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    float waveform(float position, double phase) const { return table_->sample(position, phase, 0); }
private:
    struct Voice
    {
        int note = -1, channel = 0, stage = 0;
        bool held = false;
        float velocity = 0, envelope = 0, releaseStep = 0;
        uint64_t age = 0;
        double phase[2]{}, subPhase = 0;
        dsp::Svf filter;
    };
    void event(const uint8_t* data, int size);
    void render(juce::AudioBuffer<float>&, int start, int end);
    std::shared_ptr<const WavetableData> table_;
    std::array<Voice, 16> voices_{};
    std::array<float, 16> bends_{};
    std::array<bool, 16> pedals_{};
    double sampleRate_ = 44100, lfoPhase_ = 0;
    uint64_t age_ = 0;
};
}
