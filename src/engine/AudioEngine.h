#pragma once
// Owns the audio device and runs the EngineGraph on the audio thread.
//
// Graph hand-over (lock-free, see docs/ARCHITECTURE.md "Threads"):
//   message thread: buildGraph() -> pending_.exchange(newGraph)
//   audio thread:   if retired_ slot is empty, take pending_ and move current to retired_
//   message thread: timer deletes whatever is in retired_
// This guarantees the audio thread never sees a deleted graph and never frees memory.

#include "core/Model.h"
#include "engine/AudioCapture.h"
#include "engine/EngineGraph.h"
#include "engine/MasterMonitor.h"
#include "engine/Metronome.h"
#include "engine/RecordingManager.h"
#include "engine/SampleCache.h"
#include "engine/Transport.h"
#include "midi/InputBlock.h"

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_events/juce_events.h>

#include <array>
#include <atomic>
#include <functional>
#include <memory>

namespace sonora::plugins
{
class PluginHost;
}
namespace sonora::midi
{
class MidiManager;
class MidiClockOut;
}

namespace sonora::engine
{

class AudioEngine final : private juce::AudioIODeviceCallback, private juce::Timer
{
public:
    AudioEngine(plugins::PluginHost& host, midi::MidiManager& midi);
    ~AudioEngine() override;

    // Opens the default output (and input when available). Returns an error string or "".
    juce::String initialiseAudio(const juce::XmlElement* savedState = nullptr);
    juce::AudioDeviceManager& deviceManager() { return deviceManager_; }

    // Builds and installs a graph for the song. Message thread. Returns the build log.
    std::vector<std::string> loadSong(const model::Song& song);
    // UI-42: starts `device`'s plug-in ahead of loadSong(), which then uses that instance (message thread). False when
    // nothing was started: not a plug-in, not installed, or it failed (loadSong() reports those).
    bool preloadPlugin(const model::Device& device);

    // Prepares and installs an already built graph (loadSong() uses this; tests inject custom graphs).
    void installGraph(std::unique_ptr<EngineGraph> graph);

    // Optional MIDI clock/transport output (MIDI-6). Not owned; must outlive the engine's callbacks.
    void setClockOut(midi::MidiClockOut* clock) { clockOut_ = clock; }

    // Replaces arrangement/session clips and arrangement automation of one track while audio runs (message thread). `trackIndex` is the
    // ControlBank / node index; `song` is the edited model. Cheap: only that track's clips are rebuilt.
    void updateTrackClips(int trackIndex, const model::Song& song, bool takesOnly = false);
    // Message thread: prepare inserted processors, briefly pause the device callback to edit the
    // chain, retain existing instances/state, refresh bindings and latency, then resume.
    std::vector<std::string> updateTrackDevices(int trackIndex, const model::Song& song);
    void reprepareGraph(); // message thread; retains instances, controls and transport on device changes
    bool graphPreparedForDevice() const
    { return latest_ && juce::approximatelyEqual(latest_->preparedSampleRate, sampleRate_) && latest_->preparedBlockSize >= blockSize_; }
    std::function<void()> beforeReprepare, afterReprepare; // UI drops processor pointers during prepare

    void previewSample(const std::string& path); // message thread; empty path stops audition
    bool previewPlaying() const { return previewPlaying_.load(); }

    // ---- offline render (Export). beginOffline()/endOffline() on the message thread; renderOffline() on one worker
    // thread in between. The device callback is removed meanwhile, so nothing is heard and nothing else may touch
    // the graph (the UI shows a modal dialog; edits are blocked by the caller).
    struct OfflineBlock
    {
        int numFrames = 0;
        const float* mainL = nullptr;
        const float* mainR = nullptr;
        const std::vector<std::array<const float*, 2>>* stems = nullptr; // one entry per requested stem node (post-fader, delay compensated)
    };
    bool beginOffline();
    void endOffline();
    bool offlineActive() const { return offline_; }
    // Renders from `startBeat` in blocks. `nextBlockSize(positionBeats, samplesPerBeat, maxBlock)` returns how many frames to
    // render next (<= maxBlock, 0 ends the render); `onBlock` receives the audio. `stemNodes` are node indices to capture individually.
    void renderOffline(double startBeat, const std::vector<int>& stemNodes,
                       const std::function<int(double positionBeats, double samplesPerBeat, int maxBlock)>& nextBlockSize,
                       const std::function<void(const OfflineBlock&)>& onBlock, bool preMixer = false, int sessionScene = -1);

    RecordingManager& recorder() { return recorder_; }
    juce::String enableRecordingInputs(int requiredChannels = 2);
    int inputChannelCount() const;

    Transport& transport() { return transport_; }
    AudioCapture& audioCapture() { return audioCapture_; } // TRK-4: last minute of the interface inputs
    const AudioCapture& audioCapture() const { return audioCapture_; }
    MasterMonitor& monitor() { return monitor_; } // loudness / spectrum / scope of the Main output
    std::shared_ptr<ControlBank> controls() const { return controls_; }

    // The most recently built graph. Message thread only. Safe to dereference until the
    // next loadSong(): only *older* graphs are ever deleted.
    EngineGraph* latestGraph() const { return latest_; }

    // The audio callback body. Called by the device on the audio thread; tests and offline
    // tools may call it directly when no audio device is open (never concurrently with a device).
    void renderBlock(float* const* outputs, int numOutputs, int numSamples,
                     const float* const* inputs = nullptr, int numInputs = 0);

    // Samples still decoding in the background / files that could not be read (ENG-8).
    int pendingSampleLoads() const { return samples_.pendingLoads(); }
    std::vector<std::string> failedSamples() const { return samples_.failedFiles(); }
    SampleCache& sampleCache() { return samples_; }

    double sampleRate() const { return sampleRate_; }
    int blockSize() const { return blockSize_; }
    double cpuUsage() const { return deviceManager_.getCpuUsage(); }

    bool placeholderInstruments = true;
    // Diagnostics (off by default): after every device check the track buffer for NaN / inf / huge values
    // and call `onBadAudio(trackName, deviceName, inMagnitude, outMagnitude)` per occurrence. Used by tools/engine_probe.
    bool debugCheckDevices = false;
    std::function<void(const std::string& track, const std::string& device, float inMagnitude, float outMagnitude)> onBadAudio;

private:
    std::shared_ptr<std::atomic<bool>> lifetime_ = std::make_shared<std::atomic<bool>>(true);
    void queueReprepare(); // device callbacks may outlive shutdown in the message queue
    bool changingDevices_ = false, reprepareQueued_ = false; // message thread only
    // juce::AudioIODeviceCallback
    void audioDeviceIOCallbackWithContext(const float* const* inputs, int numInputs, float* const* outputs,
                                          int numOutputs, int numSamples,
                                          const juce::AudioIODeviceCallbackContext& context) override;
    void audioDeviceAboutToStart(juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;

    // juce::Timer — garbage collects retired graphs.
    void timerCallback() override;

    void renderPreview(float* const* outputs, int numOutputs, int numSamples);

    // Clip envelopes (ALS-9) of one track at the block start position.
    void applyClipEnvelopes(TrackNode& node, TrackControls& ctl, double beat);
    static void applyPendingClips(TrackNode& node, TrackControls& controls);
    static void copyTap(juce::AudioBuffer<float>& tap, const juce::AudioBuffer<float>& src, int numSamples);
    void feedSidechains(EngineGraph& graph, TrackNode& node, int numSamples);
    void processNode(TrackNode& node, const TrackControls& ctl, int numSamples, const devices::HostInfo& host);
    void renderClips(TrackNode& node, const BlockSegment* segs, int numSegs, double tempo);
    // Arrangement + session clips of one track, including quantised launches / stops (ENG-6).
    void renderTrackClips(TrackNode& node, TrackControls& ctl, const BlockSegment* segs, int numSegs, double tempo,
                          const Transport::BlockStart& bs, int numSamples, const Recorder::CaptureRange* ranges = nullptr);

    plugins::PluginHost& host_;
    midi::MidiManager& midi_;
    juce::AudioDeviceManager deviceManager_;
    SampleCache samples_;
    juce::ThreadPool loaderPool_{2}; // declared after samples_: destroyed (jobs drained) first
    Transport transport_;

    std::atomic<EngineGraph*> pending_{nullptr};
    std::atomic<EngineGraph*> retired_{nullptr};
    EngineGraph* current_ = nullptr; // audio thread only
    std::shared_ptr<ControlBank> controls_;
    EngineGraph* latest_ = nullptr;
    std::unique_ptr<model::Song> lastSong_; // to rebuild on sample-rate change

    double sampleRate_ = 44100.0;
    std::array<int, 64> inputPhysicalChannels_{}; // callback index -> physical index, filled in aboutToStart
    int numPhysicalInputs_ = 0;
    int blockSize_ = 512;
    juce::MidiBuffer liveMidi_;
    midi::InputBlock inputMidiBlock_;
    Metronome metronome_;
    MasterMonitor monitor_;
    AudioCapture audioCapture_;
    RecordingManager recorder_;
    struct Preview
    {
        std::shared_ptr<SampleSlot> sample;
        double position = 0;
    };
    std::atomic<Preview*> pendingPreview_{nullptr}, retiredPreview_{nullptr};
    Preview* currentPreview_ = nullptr; // audio thread; freed after callback removal
    std::atomic<bool> previewPlaying_{false};
    std::atomic<midi::MidiClockOut*> clockOut_{nullptr};

    // Offline render state (see beginOffline). The audio path only looks at offlineStemSlot_, which is null in real time.
    struct OfflineSaved
    {
        double position = 0, tempo = 120, loopStart = 0, loopLength = 4;
        bool loopOn = false, metronome = false, deviceWasRunning = false;
    } offlineSaved_;
    struct OfflineNodeState
    {
        float volume = 1, pan = 0;
        bool active = true, solo = false;
        std::array<float, TrackControls::kMaxSends> sends{};
        std::vector<std::vector<float>> parameters;
    };
    std::vector<OfflineNodeState> offlineNodes_;
    bool offlinePreMixer_ = false; // set before worker render, never changed concurrently with audio
    bool offline_ = false; // message thread
    std::vector<int> offlineStemSlotStorage_;
    std::vector<juce::AudioBuffer<float>> offlineStemBuffers_;
    const std::vector<int>* offlineStemSlot_ = nullptr; // node index -> stem slot or -1; set only while renderOffline runs
};

} // namespace sonora::engine
