#pragma once
// Instrument / Audio Effect / Drum Rack (ROADMAP DEV-5).
//
// Each chain runs on a copy of the input and the results are summed (chain volume / pan applied).
//   - Drum Racks route each note only to the pad whose receivingNote matches, transposed to the
//     pad's sendingNote; pads in the same choke group cut each other off.
//   - Instrument / Effect Racks: a chain only gets notes inside its key and velocity zones and is
//     only active while the chain selector is inside its selector range.
//   - 16 macros are exposed as parameters ("MacroControls.N", 0..127). Parameters of the devices
//     in the chains that are mapped to a macro follow it (MacroLink).
// Not yet: zone / selector crossfades, return chains, MIDI Effect Racks (need MIDI effects).

#include "devices/DeviceProcessor.h"
#include "engine/DelayLine.h"

#include <array>
#include <memory>

namespace sonora::devices
{

class RackProcessor final : public DeviceProcessor
{
public:
    static constexpr int kNumMacros = 16;

    struct Chain
    {
        std::vector<std::unique_ptr<DeviceProcessor>> devices;
        std::vector<DeviceProcessor*> processorForModelDevice; // includes unsupported children as nullptr
        float volume = 1.0f;
        float pan = 0.0f;
        bool active = true;
        int receivingNote = -1;
        int sendingNote = 60;
        int chokeGroup = 0;
        int keyMin = 0, keyMax = 127;
        int velMin = 1, velMax = 127;
        int selMin = 0, selMax = 127;
        engine::DelayLine alignDelay; // plugin delay compensation between chains (ENG-12)
        int latency = 0;
        bool midiOverflow = false;
    };

    // "parameter `paramIndex` of `target` follows macro `macro`, from `min` to `max` (its own units)".
    struct MacroLink
    {
        int macro = 0;
        DeviceProcessor* target = nullptr;
        int paramIndex = 0;
        float min = 0.0f, max = 1.0f;
    };

    RackProcessor(std::string displayName, bool instrument, bool isDrumRack, std::vector<Chain> chains);

    // Macro setup (message thread, before the rack is used).
    void setMacros(const std::vector<std::string>& names, const std::array<float, kNumMacros>& defaults,
                   std::vector<MacroLink> links, int numVisible);

    std::string name() const override { return name_; }
    bool isInstrument() const override { return instrument_; }
    void prepare(double sampleRate, int maxBlockSize) override;
    void process(juce::AudioBuffer<float>& audio, juce::MidiBuffer& midi) override;
    void reset() override;
    int latencySamples() const override { return latency_; }
    void setHostInfo(const HostInfo& info) override;

    // parameters(): 16 macros, then the chain selector.
    const std::vector<ParameterInfo>& parameters() const override { return infos_; }
    std::vector<int> displayParameters() const override;
    float getParameter(int index) const override;
    void setParameter(int index, float value) override;
    static constexpr int kChainSelector = kNumMacros;

    const std::vector<Chain>& chains() const { return chains_; }

private:
    void applyMacro(int macro);
    bool chainSelected(const Chain& c, float selector) const;

    std::string name_;
    bool instrument_;
    bool drumRack_;
    int latency_ = 0; // slowest chain; shorter chains are delayed to match
    int numVisibleMacros_ = 8;
    std::vector<Chain> chains_;
    std::vector<ParameterInfo> infos_;
    std::array<std::atomic<float>, kNumMacros + 1> values_{};
    std::vector<MacroLink> links_;
    juce::AudioBuffer<float> scratch_, sum_;
    std::vector<juce::MidiBuffer> chainMidi_; // one per chain, built before the chains are processed
};

} // namespace sonora::devices
