#pragma once
// Wraps a hosted juce::AudioPluginInstance as a DeviceProcessor.

#include "devices/DeviceProcessor.h"
#include "devices/MidiReleaseFilter.h"
#include "core/Model.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <atomic>
#include <memory>
#include <string>
#include <unordered_map>

namespace sonora::plugins
{

// Reads the instance's current state into `info` in the layout the .als keeps for `kind` (VST3 preset chunks, VST2 bank
// as <VstPreset>, AU ClassInfo as an XML plist). False when the instance's format does not match `kind` or it gave nothing.
// Message thread.
bool capturePluginInfo(juce::AudioPluginInstance& instance, model::DeviceKind kind, model::PluginInfo& info);

// PLG-16b: some chunk plug-ins (LFOTool, VST2 and AU, measured on this Mac) hand the host an all-zero chunk even right
// after loading a real one. That is no report, not a state: true when `captured` is such a chunk and `stored` (the
// state the set had) is a real one, so capture keeps `stored`. VST2: the chunk itself; AU: the "vstdata" FXP's chunk.
bool pluginChunkUnreported(model::DeviceKind kind, const std::vector<uint8_t>& captured, const std::vector<uint8_t>& stored);

class PluginDevice final : public devices::DeviceProcessor, private juce::AudioProcessorListener
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
    bool hostParameter(int index, int& id, std::string& name) const override;
    uint32_t handEditTotal() const override { return handTotal_.load(std::memory_order_acquire); }
    HandEdit handEdit(int index) const override;
    bool takeHandEdit(int index) override;

    bool hasEditor() const override;
    juce::AudioPluginInstance& instance() { return *instance_; }
    // Message thread. Save suspends the audio callback around it; the autosave (PRJ-4) does not, which the plug-in
    // formats allow (VST3 and AU state is read on the UI thread while processing runs; JUCE takes no process lock).
    bool captureState(model::Device& device) const;
    // PRJ-4: moves whenever the plug-in reports a parameter edit or any other change of its state (any thread). A copy of
    // its state taken when this read N is still current while it reads N.
    uint64_t stateSerial() const
    {
        return handTotal_.load(std::memory_order_acquire) + stateChanges_.load(std::memory_order_acquire);
    }

private:
    // UI-65: edits the plugin reports from its own window (performEdit and gestures); any thread.
    void audioProcessorParameterChanged(juce::AudioProcessor*, int index, float value) override;
    void audioProcessorChanged(juce::AudioProcessor*, const ChangeDetails&) override
    {
        stateChanges_.fetch_add(1, std::memory_order_release);
    }
    void audioProcessorParameterChangeGestureBegin(juce::AudioProcessor*, int index) override;
    void audioProcessorParameterChangeGestureEnd(juce::AudioProcessor*, int index) override;

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
    // UI-65: per parameter, written by the listener callbacks. `automationSeen_` belongs to the audio thread.
    struct HandSlot
    {
        std::atomic<uint32_t> serial{0};
        std::atomic<float> value{0.0f}, before{0.0f};
        std::atomic<bool> gesture{false};
    };
    std::unique_ptr<HandSlot[]> hand_;
    std::unique_ptr<uint32_t[]> automationSeen_;
    std::vector<int> hostIds_;       // per parameter: the plugin's own id, INT_MIN when it is not automatable
    std::vector<std::string> names_; // full names
    std::atomic<uint32_t> handTotal_{0};
    std::atomic<uint64_t> stateChanges_{0};
};

} // namespace sonora::plugins
