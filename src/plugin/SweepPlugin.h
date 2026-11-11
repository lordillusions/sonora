#pragma once
// Sonora Sweep as a VST3 instrument (PLG-19): the same Sweep device and face the app uses. A note plays one sweep of
// Length at the host tempo. The Sample source's file is loaded from the editor ("Load Sample...") and its path travels in
// the state (with the parameters), so a project reopens with it.

#include "devices/native/Sweep.h"
#include "plugin/PluginCommon.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace sonora::plugin
{
class SweepPlugin final : public juce::AudioProcessor, public ParameterOwner, private juce::Timer
{
public:
    SweepPlugin();
    ~SweepPlugin() override;

    // ---- juce::AudioProcessor
    const juce::String getName() const override { return "Sonora Sweep"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.1; }
    bool isBusesLayoutSupported(const BusesLayout&) const override;
    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using juce::AudioProcessor::processBlock;
    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override;
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    devices::Sweep& device() { return device_; }
    bool isSynth() const { return true; }
    // Message thread: loads `path` as the Sample source (Source switches to Sample); false + a note on failure.
    bool loadSample(const juce::File& file);
    std::vector<std::string> buildLog; // load notes, newest last

    // ---- ParameterOwner
    float parameterValue(int index) const override { return device_.getParameter(index); }
    void setParameterValue(int index, float plainValue) override;
    juce::AudioProcessorParameter* hostParameter(int index) const { return hostParameters_[(size_t) index]; }

    // Editor edits write the device directly: tells the host about every parameter that changed since the last call
    // (the timer runs it 20 times a second).
    void reportEditorChanges();

private:
    void timerCallback() override { reportEditorChanges(); }

    devices::Sweep device_;
    std::vector<juce::AudioProcessorParameter*> hostParameters_;
    std::unique_ptr<std::atomic<float>[]> reported_;
    juce::AudioBuffer<float> stereo_;
    double sampleRate_ = 44100.0;
};
} // namespace sonora::plugin
