#pragma once
// Sonora FM: independent four-oscillator phase-modulation synthesizer (DEV-13).
#include "devices/NativeDevice.h"
#include <array>

namespace sonora::devices
{
class FmSynth final : public NativeDevice
{
public:
    enum Param { Algorithm, Volume, Transpose, BendRange, Feedback, Pan, OscStart };
    enum OscParam { Ratio, Fine, Level, Wave, Attack, Decay, Sustain, Release, OscParams };
    static constexpr int oscParam(int oscillator, OscParam p) { return OscStart + oscillator * OscParams + p; }
    FmSynth();
    std::string name() const override { return "Sonora FM"; }
    bool isInstrument() const override { return true; }
    void prepare(double sampleRate, int) override;
    void reset() override;
    void process(juce::AudioBuffer<float>& audio, juce::MidiBuffer& midi) override;
    std::vector<int> displayParameters() const override;

private:
    struct Envelope { int stage = 0; float level = 0, releaseStep = 0; };
    struct Voice
    {
        int note = -1, channel = 0;
        bool keyDown = false, released = false;
        float velocity = 0, feedback = 0;
        uint64_t age = 0;
        std::array<double, 4> phase{};
        std::array<Envelope, 4> env{};
    };
    void event(const juce::MidiMessage& message);
    void render(juce::AudioBuffer<float>& audio, int from, int to);
    std::array<Voice, 32> voices_{};
    std::array<float, 16> bends_{};
    std::array<bool, 16> pedals_{};
    double sampleRate_ = 44100;
    uint64_t age_ = 0;
};
} // namespace sonora::devices
