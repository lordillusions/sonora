#pragma once
// Wraps a hosted juce::AudioPluginInstance as a DeviceProcessor.

#include "devices/DeviceProcessor.h"
#include "devices/MidiReleaseFilter.h"
#include "core/Model.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <memory>
#include <string>
#include <unordered_map>

namespace sonora::plugins
{

class PluginDevice final : public devices::DeviceProcessor
{
public:
    explicit PluginDevice(std::unique_ptr<juce::AudioPluginInstance> instance);
    ~PluginDevice() override;

    std::string name() const override;
    bool isInstrument() const override;
    bool isHostedPlugin() const override { return true; }
    void prepare(double sampleRate, int maxBlockSize) override;
    void process(juce::AudioBuffer<float>& audio, juce::MidiBuffer& midi) override;
    void reset() override;
    void setHostInfo(const devices::HostInfo& info) override { playing_ = info.playing; playHead_.info = info; }
    int latencySamples() const override;

    const std::vector<devices::ParameterInfo>& parameters() const override { return infos_; }
    int findParameterByHostId(int id, const std::string& name) const override;
    float getParameter(int index) const override;
    void setParameter(int index, float value) override;

    bool hasEditor() const override;
    juce::AudioPluginInstance& instance() { return *instance_; }
    bool captureState(model::Device& device) const; // message thread, with audio callback suspended

private:
    std::unique_ptr<juce::AudioPluginInstance> instance_;
    std::vector<devices::ParameterInfo> infos_;
    bool instrument_ = false; // cached: getPluginDescription() allocates
    juce::AudioBuffer<float> io_; // plugin may want more than 2 channels
    // ENG-14d: while playing, the engine's All Notes Off bursts reach the plugin as note-offs for its held notes.
    devices::MidiReleaseFilter releases_;
    bool playing_ = false; // audio thread, from setHostInfo()
    // PLG-10: the host transport as the plugin sees it. Without a play head a VST3 plugin is told nothing (stopped, no tempo,
    // no position), which breaks tempo-synced effects and assistants that learn while the song plays (Ozone's Master
    // Assistant). Written and read on the audio thread only, inside process().
    struct PlayHead final : juce::AudioPlayHead
    {
        devices::HostInfo info;
        juce::Optional<PositionInfo> getPosition() const override;
    } playHead_;
    // Host parameter id / name -> index, built on first lookup (message thread only). Plugins like Serum 2 expose
    // thousands of parameters and the engine resolves every automation target, so a linear scan cost seconds on load.
    mutable std::unordered_map<std::string, int> idToIndex_, nameToIndex_;
    mutable bool lookupBuilt_ = false;
};

} // namespace sonora::plugins
