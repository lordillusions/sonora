#pragma once
#include "devices/NativeDevice.h"
#include <array>
namespace sonora::devices
{
// DEV-21: channel-aware note transforms. Engine/rack MIDI buffers reserve 147 KiB in prepare;
// generated output is capped at 64 KiB, including release/panic room.
class MidiNoteEffects final : public NativeDevice
{
public:
    enum class Kind { Pitch, Chord, Scale, Random, Velocity };
    explicit MidiNoteEffects(Kind);
    std::string name() const override;
    void prepare(double, int) override;
    void reset() override;
    void process(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    std::vector<int> displayParameters() const override;
private:
    struct Held { std::array<int, 7> notes{}; int count = 0; bool seen = false; };
    std::array<std::array<Held, 128>, 16> held_{};
    std::array<std::array<int, 128>, 16> references_{};
    juce::MidiBuffer scratch_;
    Kind kind_;
    uint32_t random_ = 1;
    uint32_t alternate_ = 0;
    int bytes_ = 0;
    bool overflow_ = false;
    float random();
    void emit(const void*, int size, int sample);
    void release(int channel, int note, int velocity, int sample);
};
}
