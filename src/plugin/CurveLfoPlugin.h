#pragma once
// Sonora Curve LFO as a VST3 audio effect (PLG-12): the same CurveLfo device and face the app uses, wrapped in a JUCE
// AudioProcessor so other hosts can load it.
//
// Every control, the twelve graphs included, is a CurveLfo parameter (atomics), so there is one instance for the
// plug-in's lifetime: the editor, host automation and a restored state all write it directly.
//
// Host parameters are the controls below CurveLfo::SelectedGraph (timing, routes, filter, MIDI). The graph points and
// warps are too many to automate; they are saved with the state, and an edit to them marks the host's project changed.
//
// MIDI: notes drive graph selection, retrigger and gate as in the app. The output carries only the cutoff curve as CC
// (CC Out); input notes are not passed through.

#include "devices/native/CurveLfo.h"
#include "plugin/PluginCommon.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace sonora::plugin
{

class CurveLfoPlugin final : public juce::AudioProcessor, public ParameterOwner, private juce::Timer
{
public:
    static constexpr int kHostParameters = devices::CurveLfo::SelectedGraph;

    CurveLfoPlugin();
    ~CurveLfoPlugin() override;

    // ---- juce::AudioProcessor
    const juce::String getName() const override { return "Sonora Curve LFO"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return true; }
    double getTailLengthSeconds() const override { return 0.0; }
    bool isBusesLayoutSupported(const BusesLayout&) const override;
    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
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

    devices::CurveLfo& lfo() { return lfo_; }
    const devices::CurveLfo& lfo() const { return lfo_; }
    std::vector<std::string> buildLog; // load notes, newest last

    // ---- ParameterOwner: plain values of the host parameters
    float parameterValue(int index) const override;
    void setParameterValue(int index, float plainValue) override;

    // Editor edits write the device directly: tells the host about every host parameter that changed since the last
    // call, and that the project changed when a graph did (the timer runs it 20 times a second).
    void reportEditorChanges();

    // Serialised state: every control by its id (unknown ids are skipped, missing ones keep their defaults). Exposed
    // for tests. decodeState applies to `lfo` only when the whole state could be read.
    static juce::MemoryBlock encodeState(const devices::CurveLfo&);
    static bool decodeState(const void* data, size_t size, devices::CurveLfo& lfo, std::string& error);

private:
    void timerCallback() override;
    void renderChunk(juce::AudioBuffer<float>&, const juce::MidiBuffer& input, int start, int length, const devices::HostInfo&);

    devices::CurveLfo lfo_;
    double sampleRate_ = 44100.0;
    int blockSize_ = 512;
    juce::MidiBuffer chunkMidi_, outputMidi_;
    std::vector<juce::AudioProcessorParameter*> hostParameters_;
    // Last plain value each host parameter reported (written by host automation on any thread), to notice editor edits.
    std::unique_ptr<std::atomic<float>[]> reported_;
    std::vector<float> graphValues_; // controls from SelectedGraph on, as last reported
};

} // namespace sonora::plugin
