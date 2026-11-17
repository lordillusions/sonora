#pragma once
// Sonora Seraph as a VST3 instrument (PLG-27): the same Seraph device and face the app uses. The state is the parameters,
// the custom gate pattern and the name of the preset last chosen, so a project reopens exactly as it was.

#include "devices/native/Seraph.h"
#include "plugin/PluginCommon.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace sonora::plugin
{
class SeraphPlugin final : public juce::AudioProcessor, public ParameterOwner, private juce::Timer
{
public:
    SeraphPlugin();
    ~SeraphPlugin() override;

    // ---- juce::AudioProcessor
    const juce::String getName() const override { return "Sonora Seraph"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 20.0; } // the Endless space
    bool isBusesLayoutSupported(const BusesLayout&) const override;
    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using juce::AudioProcessor::processBlock;
    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override;
    // The factory presets as the host's programs.
    int getNumPrograms() override { return (int) devices::Seraph::presets().size(); }
    int getCurrentProgram() override { return std::max(0, device_.matchingPreset()); }
    void setCurrentProgram(int index) override;
    const juce::String getProgramName(int index) override;
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    devices::Seraph& device() { return device_; }
    bool isSynth() const { return true; }
    std::vector<std::string> buildLog; // load notes, newest last

    // ---- ParameterOwner
    float parameterValue(int index) const override { return device_.getParameter(index); }
    void setParameterValue(int index, float plainValue) override;
    juce::AudioProcessorParameter* hostParameter(int index) const { return hostParameters_[(size_t) index]; }

    // Editor edits write the device directly: tells the host about every parameter that changed since the last call and
    // about a changed gate pattern or preset (the timer runs it 20 times a second).
    void reportEditorChanges();

private:
    void timerCallback() override { reportEditorChanges(); }

    devices::Seraph device_;
    std::vector<juce::AudioProcessorParameter*> hostParameters_;
    std::unique_ptr<std::atomic<float>[]> reported_;
    std::string reportedContent_;
    juce::AudioBuffer<float> stereo_;
    double sampleRate_ = 44100.0;
};
} // namespace sonora::plugin
