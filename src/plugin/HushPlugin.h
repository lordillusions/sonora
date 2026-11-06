#pragma once
// Sonora Hush as a VST3 audio effect (PLG-14): the Hush resonance suppressor (devices/native/Hush.h) with a sidechain
// bus and every control as a host parameter. Presets, A/B, undo and the saved state are the session's
// (devices/native/HushSession.h), shared with the built-in device (PLG-14g).
//
// One Hush instance lives for the plug-in's lifetime; host automation, the editor and restored state all write its
// parameter atomics. Latency follows the linear-phase / low-latency switches (the host is told from a timer).

#include "devices/native/Hush.h"
#include "devices/native/HushSession.h"
#include "plugin/PluginCommon.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <string>
#include <vector>

namespace sonora::plugin
{
using HushSettings = devices::HushSettings;
using HushPreset = devices::HushPreset;

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
    int getNumPrograms() override { return (int) devices::HushSession::factoryPresets().size(); }
    int getCurrentProgram() override { return currentProgram_; }
    void setCurrentProgram(int index) override;
    const juce::String getProgramName(int index) override;
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    devices::Hush& hush() { return hush_; }
    const devices::Hush& hush() const { return hush_; }
    devices::HushSession& session() { return session_; }

    // ---- ParameterOwner
    float parameterValue(int index) const override { return hush_.getParameter(index); }
    void setParameterValue(int index, float plainValue) override { hush_.setParameter(index, plainValue); }
    juce::AudioProcessorParameter* hostParameter(int index) const { return hostParameters_[(size_t) index]; }

    std::vector<std::string> buildLog;

private:
    void timerCallback() override;

    devices::Hush hush_;
    devices::HushSession session_{hush_};
    std::vector<juce::AudioProcessorParameter*> hostParameters_;
    juce::AudioBuffer<float> stereo_, sidechain_;
    int currentProgram_ = 0;
};

} // namespace sonora::plugin
