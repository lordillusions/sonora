#pragma once
// Sonora Hush as a VST3 audio effect (PLG-14): the Hush resonance suppressor (devices/native/Hush.h) with a sidechain
// bus, every control as a host parameter, presets (factory, user, and preset files in the same JSON layout), A/B
// slots and undo/redo for the editor.
//
// One Hush instance lives for the plug-in's lifetime; host automation, the editor and restored state all write its
// parameter atomics. Latency follows the linear-phase / low-latency switches (the host is told from a timer).

#include "devices/native/Hush.h"
#include "plugin/PluginCommon.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <atomic>
#include <map>
#include <string>
#include <vector>

namespace sonora::plugin
{

// Settings menu: shared by every instance in the process and kept in ~/Library/Application Support/Sonora/Hush.settings.
struct HushSettings
{
    std::atomic<bool> showTooltips{true}, renderUltra{false}, lowLatencyAll{false}, listenOnDrag{false}, listenDelta{true},
        frequencyNearCursor{false};
    std::string defaultPreset; // message thread; empty = "Factory default"
    static HushSettings& get();
    static juce::File file();
    void save() const;
};

struct HushPreset
{
    std::string name, category, description;
    std::map<std::string, float> values; // parameter id -> plain value; missing ids take their defaults
    bool factory = false;
    juce::File file; // user and imported presets
};

class HushPlugin final : public juce::AudioProcessor, public ParameterOwner, private juce::Timer
{
public:
    HushPlugin();
    ~HushPlugin() override;

    // ---- juce::AudioProcessor
    const juce::String getName() const override { return "Sonora Hush"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.1; }
    bool isBusesLayoutSupported(const BusesLayout&) const override;
    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using juce::AudioProcessor::processBlock;
    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override;
    int getNumPrograms() override { return (int) factoryPresets().size(); }
    int getCurrentProgram() override { return currentProgram_; }
    void setCurrentProgram(int index) override;
    const juce::String getProgramName(int index) override;
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    devices::Hush& hush() { return hush_; }
    const devices::Hush& hush() const { return hush_; }

    // ---- ParameterOwner
    float parameterValue(int index) const override { return hush_.getParameter(index); }
    void setParameterValue(int index, float plainValue) override { hush_.setParameter(index, plainValue); }
    // Editor edits: sets the value and tells the host (gesture begin/end around drags is the editor's job).
    void setFromEditor(int index, float plainValue);
    juce::AudioProcessorParameter* hostParameter(int index) const { return hostParameters_[(size_t) index]; }
    // Text the host and the editor show for a plain value ("4.0 kHz", "1.0x", "even", "off"), and back.
    static juce::String formatValue(const devices::ParameterInfo&, float plainValue);
    static float parseValue(const devices::ParameterInfo&, const juce::String& text, float fallback);

    // ---- presets
    static const std::vector<HushPreset>& factoryPresets();
    // Sonora's factory presets, then user presets, then the reference plug-in's own factory and user preset files when
    // it is installed (read in place, never copied), then other files of the same JSON layout.
    std::vector<HushPreset> allPresets() const;
    static std::vector<juce::File> referencePresetFolders(); // the installed reference's factory and user folders
    static std::string categoryName(int category);
    static juce::File userPresetFolder();
    static bool readPresetFile(const juce::File&, HushPreset&);
    bool savePreset(const juce::String& name, const juce::String& category, bool overwrite);
    void loadPreset(const HushPreset&);
    const std::string& presetName() const { return presetName_; }
    const std::string& presetCategory() const { return presetCategory_; }
    bool presetModified() const;
    void stepPreset(int delta); // < > buttons
    bool qualityLocked = false, lowLatencyLocked = false;

    // ---- A/B and undo (editor)
    int activeSlot() const { return activeSlot_; }
    void selectSlot(int slot);
    void copyToOtherSlot();
    void pushUndo(); // before an edit gesture
    bool undo();
    bool redo();
    bool canUndo() const { return ! undo_[(size_t) activeSlot_].empty(); }
    bool canRedo() const { return ! redo_[(size_t) activeSlot_].empty(); }

    // ---- editor state kept with the plug-in
    float editorScale = 1050.0f / 900.0f; // the reference opens at 1050 x 621
    bool sidePanelOpen = false;
    float graphRange = 9.0f; // dB shown by the reduction graph

    // State: every parameter by id, settings, the other A/B slot. Exposed for tests.
    juce::MemoryBlock encodeState() const;
    bool decodeState(const void* data, size_t size, std::string& error);
    std::vector<std::string> buildLog;

private:
    using Snapshot = std::vector<float>;
    Snapshot snapshot() const;
    void restore(const Snapshot&);
    void timerCallback() override;
    void applySettings(); // the shared settings onto this instance's device

    devices::Hush hush_;
    std::vector<juce::AudioProcessorParameter*> hostParameters_;
    juce::AudioBuffer<float> stereo_, sidechain_;
    int currentProgram_ = 0;
    std::string presetName_ = "Factory default", presetCategory_ = "misc";
    Snapshot presetValues_;
    int activeSlot_ = 0;
    std::array<Snapshot, 2> slots_;
    std::array<std::vector<Snapshot>, 2> undo_, redo_;
};

} // namespace sonora::plugin
