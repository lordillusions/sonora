#pragma once
// Sonora Phrase as a VST3 instrument that sends MIDI (PLG-25; TRANCE_SUITE.md "Decisions" 2): the same Phrase device and
// face the app uses. The host's MIDI goes in (held chords for MIDI In), the part comes out as MIDI (another track takes
// "MIDI From" it), and the audition voice (a small sine voice, parameter Audition, on by default here) plays it so the
// plug-in makes sound alone. The patterns and progression travel in the state with the parameters.

#include "devices/PlaceholderSynth.h"
#include "devices/native/Phrase.h"
#include "plugin/PluginCommon.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace sonora::plugin
{
class PhrasePlugin final : public juce::AudioProcessor, public ParameterOwner, private juce::Timer
{
public:
    PhrasePlugin();
    ~PhrasePlugin() override;

    // ---- juce::AudioProcessor
    const juce::String getName() const override { return "Sonora Phrase"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return true; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.2; }
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

    devices::Phrase& device() { return device_; }
    std::vector<std::string> buildLog;
    // The part over its form (or progression) as a .mid file, dragged from `source` (the editor). Message thread.
    bool dragMidi(juce::Component* source);
    std::vector<model::MidiNote> partNotes(double& lengthBeats) const;

    // ---- ParameterOwner
    float parameterValue(int index) const override { return device_.getParameter(index); }
    void setParameterValue(int index, float plainValue) override;
    void reportEditorChanges();

private:
    void timerCallback() override { reportEditorChanges(); }

    devices::Phrase device_;
    devices::PlaceholderSynth voice_{"Sonora Phrase"};
    GeneratorMidiBridge bridge_;
    std::vector<juce::AudioProcessorParameter*> hostParameters_;
    std::unique_ptr<std::atomic<float>[]> reported_;
    juce::AudioBuffer<float> stereo_;
    juce::MidiBuffer voiceMidi_;
    double sampleRate_ = 44100.0, tempo_ = 138.0;
};
} // namespace sonora::plugin
