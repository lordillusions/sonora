#pragma once
// Sonora Stutter as a VST3 audio effect (PLG-18): the same Stutter device and face the app uses, wrapped in a JUCE
// AudioProcessor. Every control is a host parameter (times on a log scale), so the Hold switch, the mode and the pattern
// steps can be automated in any host. MIDI input (where the host routes notes into an effect) triggers the modes like in
// the app; the host transport drives the pattern, the quantize grid and the synced slice lengths. No latency.

#include "devices/native/Stutter.h"
#include "plugin/PluginCommon.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace sonora::plugin
{
class StutterPlugin final : public juce::AudioProcessor, public ParameterOwner, private juce::Timer
{
public:
    StutterPlugin();
    ~StutterPlugin() override;

    // ---- juce::AudioProcessor
    const juce::String getName() const override { return "Sonora Stutter"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
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

    devices::Stutter& device() { return device_; }
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

    devices::Stutter device_;
    std::vector<juce::AudioProcessorParameter*> hostParameters_;
    std::unique_ptr<std::atomic<float>[]> reported_;
    juce::AudioBuffer<float> stereo_;
    double sampleRate_ = 44100.0;
};
} // namespace sonora::plugin
