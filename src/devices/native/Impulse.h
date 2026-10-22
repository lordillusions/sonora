#pragma once

#include "devices/NativeDevice.h"
#include "devices/native/Simpler.h"
#include "dsp/Dsp.h"

namespace sonora::devices
{
// DEV-19: eight fixed-note, polyphonic one-shot sample slots. Own Sonora state.
class Impulse final : public NativeDevice
{
public:
    enum Param { Volume, SlotStart };
    enum SlotParam { Start, Tune, Decay, Pan, Level, FilterOn, Cutoff, Resonance, Drive, SlotParams };
    static constexpr std::array<int, 8> notes{60, 62, 64, 65, 67, 69, 71, 72};
    static constexpr int slotParam(int slot, SlotParam p) { return SlotStart + slot * SlotParams + p; }
    explicit Impulse(std::vector<Simpler::Zone> zones);
    std::string name() const override { return "Sonora Impulse"; }
    bool isInstrument() const override { return true; }
    void prepare(double sampleRate, int) override;
    void reset() override;
    void process(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    const Simpler::Zone* zone(int slot) const;
    void audition(int slot) { audition_.store(slot, std::memory_order_release); }
    unsigned activePads() const { return activePads_.load(std::memory_order_relaxed); }
private:
    struct Voice
    {
        int slot = -1, channel = 0;
        double position = 0;
        float envelope = 0, velocity = 0;
        uint64_t age = 0;
        dsp::Svf filter[2];
    };
    void start(int slot, int channel, float velocity);
    void render(juce::AudioBuffer<float>&, int from, int to);
    std::vector<Simpler::Zone> zones_;
    std::array<Voice, 32> voices_{};
    double sampleRate_ = 44100;
    uint64_t age_ = 0;
    std::atomic<int> audition_{-1};
    std::atomic<unsigned> activePads_{0};
};
}
