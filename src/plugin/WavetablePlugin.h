#pragma once
// Sonora Prism (the WavetableSynth) as a VST3 instrument (PLG-11). The same WavetableSynth and dashboard the app uses, wrapped in a JUCE
// AudioProcessor so other hosts can load it.
//
// The device lives as a model::Device (its controls, plain values and native content) inside a one-track Song, exactly
// as in a Sonora set, so the editor and the content helpers (WavetableContent.h) work unchanged. Content edits build a
// fresh WavetableSynth from that model on the message thread and swap it in (installSynth); control edits go straight to
// the running instance through atomics.
//
// Host parameters are the synth's fixed controls (indices below WavetableSynth::ParameterCount). The per-module FX
// controls depend on the current effects rack, so they are saved with the state but not published to the host.

#include "core/Model.h"
#include "devices/native/WavetableSynth.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace sonora::plugin
{

class WavetablePlugin final : public juce::AudioProcessor, private juce::Timer
{
public:
    WavetablePlugin();
    ~WavetablePlugin() override;

    // ---- juce::AudioProcessor
    const juce::String getName() const override { return "Sonora Prism"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
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

    // ---- message thread: what the editor works on
    const model::Song& song() const { return song_; }
    const model::Device& device() const { return song_.tracks.front().devices.front(); }
    devices::WavetableSynth* synth() const { return owned_.get(); }
    // A content edit (table, media, curves, effects, preset): the current control values are captured into the model
    // first, then `edit` changes it and a fresh instance replaces the running one. An edit that returns false leaves
    // everything as it was.
    bool editDevice(const std::function<bool(model::Device&)>& edit);
    // Called after an instance swap. The old instance stays alive (retired) until releaseRetired(), so an editor can
    // rebuild later, outside the gesture that caused the swap; without an editor the timer releases it.
    std::function<void()> onSynthReplaced;
    void releaseRetired() { retired_.clear(); }
    std::vector<std::string> buildLog; // load/import notes, newest last

    // Plain value of a host parameter (an index below ParameterCount).
    float parameterValue(int index) const;
    void setParameterValue(int index, float plainValue); // host automation: any thread

    // Editor edits write the synth directly: tells the host about every control that changed since the last call
    // (the timer runs it 20 times a second).
    void reportEditorChanges();

    // Serialised state: the device model (controls, plain values, native content). Exposed for tests.
    static juce::MemoryBlock encodeState(const model::Device&);
    static bool decodeState(const void* data, size_t size, model::Device& device, std::string& error);

private:
    void timerCallback() override;
    std::unique_ptr<devices::WavetableSynth> buildSynth(const model::Device&, std::vector<std::string>& warnings) const;
    void installSynth(std::unique_ptr<devices::WavetableSynth>);
    void captureState(); // running control values -> model
    void renderChunk(devices::WavetableSynth&, juce::AudioBuffer<float>&, juce::MidiBuffer&, int start, int length);

    model::Song song_;
    std::unique_ptr<devices::WavetableSynth> owned_;
    std::vector<std::unique_ptr<devices::WavetableSynth>> retired_; // never processed again (see installSynth)
    std::atomic<devices::WavetableSynth*> active_{nullptr};
    std::atomic<bool> inProcess_{false};
    double sampleRate_ = 44100.0;
    int blockSize_ = 512;
    juce::AudioBuffer<float> chunk_;
    juce::MidiBuffer chunkMidi_;
    std::vector<juce::AudioProcessorParameter*> hostParameters_;
    // Last plain value each host parameter reported (written by host automation on any thread), to notice editor edits.
    std::unique_ptr<std::atomic<float>[]> reported_;
    bool quietReport_ = false; // after an instance swap: report new values without edit gestures
};

} // namespace sonora::plugin
