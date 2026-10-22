#pragma once
// Arrangement recording (ENG-9). The audio thread writes preallocated MIDI notes and stereo
// samples into SPSC rings; this worker owns all file I/O. Completion is published only after
// the audio thread has released the take, so the message thread can safely build edit commands.

#include "core/Model.h"
#include "engine/EngineGraph.h"
#include "engine/OverdubPreview.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <array>
#include <atomic>
#include <memory>
#include <vector>

namespace sonora::engine
{

class Recorder final : private juce::Thread
{
public:
    Recorder();
    ~Recorder() override;

    // Message thread. Armed tracks are fixed for the duration of a take.
    struct SessionTarget { int track = -1, scene = -1, uid = -1; }; // uid >= 0 overdubs that exact MIDI clip
    struct SessionPlan { std::vector<SessionTarget> targets; double quant = 0; bool immediateStart = false; bool looping = false; double loopStart = 0, loopEnd = 0; };
    bool start(const model::Song& song, const ControlBank& controls, const juce::File& directory,
               double sampleRate, std::string& error, bool arrangement = true, const SessionPlan* session = nullptr);
    void requestSessionStop() { sessionStopRequested_.store(true, std::memory_order_release); }
    int sessionPhase() const { return sessionPhase_.load(std::memory_order_relaxed); } // 0 idle, 1 queued, 2 capture, 3 queued stop
    bool sessionRecording() const { return active() && sessionScheduled_; }
    int sessionScene(int track) const;
    bool overdubsTrack(int track) const;
    struct CaptureRange;
    void collectRetiredPreviews() { collectPreviews(); } // message thread
    void beginPreviewBlock(const EngineGraph&); // audio thread; source identity/lifetime reconciliation
    bool hasOverdubPreview(int track, int scene, uint32_t uid); // audio only
    bool renderOverdub(int track, int scene, const PreparedClip&, const BlockSegment&, const juce::MidiBuffer&,
                       const CaptureRange*, juce::MidiBuffer&, bool refresh, bool* overflow, float velocityScale = 1.0f); // audio only
    bool commitOverdub(int track, int scene, const PreparedClip* replacement, double elapsed, juce::MidiBuffer&); // after swap/clear
    void beginSessionContent(int track, double elapsed); // audio, first captured sample
    void requestStop() { stopRequested_.store(true, std::memory_order_release); }
    bool active() const { return state_.load(std::memory_order_acquire) == State::Capturing; }
    bool finished() const { return state_.load(std::memory_order_acquire) == State::Finished; }
    bool busy() const { return state_.load(std::memory_order_acquire) != State::Idle; }
    bool previewSettled() const { return ! pendingPreview_.load(std::memory_order_acquire) && ! previewLive_.load(std::memory_order_acquire); } // message thread retirement
    struct Result
    {
        std::vector<std::pair<int, model::Clip>> clips;
        std::vector<std::pair<int, std::vector<model::Clip>>> passes; // chronological per-track Arrangement passes
        struct SessionClip { int track = -1, scene = -1, uid = -1; model::Clip clip; bool launchAfterRecording = true; };
        std::vector<SessionClip> session;
        std::string warning;
    };
    Result takeResult(); // message thread, only after finished()
    struct CaptureRange { int offset = 0, count = 0; double beat = 0; bool punchEnd = false; };
    CaptureRange prepareCaptureRange(const BlockSegment&, bool playing, bool jumped); // once per segment, before rendering
    // Audio thread, after Main processing for resampling. MIDI is live input, never clip playback.
    void capture(const float* const* inputs, int numInputs, const juce::MidiBuffer& midi,
                 double beat, double samplesPerBeat, int numSamples, bool playing, bool jumped, bool loopEnd,
                 const EngineGraph* graph = nullptr, int sampleOffset = 0, const CaptureRange* preparedRange = nullptr);
    CaptureRange captureRange(double beat, double samplesPerBeat, int numSamples, bool playing, bool jumped,
                              int sampleOffset = 0) const; // audio thread, same range used by monitoring and capture
    bool capturesAudioTrack(int track) const;
    bool capturesTrack(int track) const;
    bool capturesCurrentBlock(bool playing, bool jumped) const // audio thread only
    { return active() && ! stopRequested_.load(std::memory_order_relaxed) && playing && (! jumped || ! started_); }
    // After the audio callback has been removed, e.g. device disconnect / shutdown.
    void deviceStopped();
    void discardUnpublished(); // message thread, a prepared recorder that was never handed to audio

private:
    struct PreviewBank
    {
        std::vector<std::unique_ptr<OverdubPreview>> clips;
        std::vector<int> channels;
        std::vector<uint8_t> disabled;
    };
    void collectPreviews(); // message thread
    void updatePreviewBank(); // audio thread
    std::atomic<PreviewBank*> pendingPreview_{nullptr}, retiredPreview_{nullptr};
    PreviewBank* preview_ = nullptr; // audio owned until retired, never inspected by message thread
    std::atomic<bool> discardPreview_{false};
    std::atomic<bool> previewLive_{false};
    enum class State { Idle, Capturing, Draining, Finished };
    static constexpr int kMaxNotes = 65536, kMaxControllers = 65536;
    static constexpr int kAudioRing = 262144;
    static constexpr int kMaxPasses = 4096, kMaxTempoMarks = 65536;
    struct Pass { double timeline = 0, source = 0, length = 0; int64_t frame = 0, frames = 0; };
    std::vector<Pass> passes_; // sized before capture, shared pass boundaries across armed tracks
    std::vector<model::WarpMarker> tempoMarks_; // source seconds -> unfolded beats
    int numPasses_ = 0, numTempoMarks_ = 0;
    int64_t totalFrames_ = 0;
    double previousSamplesPerBeat_ = 0;
    bool arrangement_ = true, repeating_ = false, newPass_ = true;
    struct CapturedController { double time = 0; int16_t value = 0; uint8_t index = 0; };
    struct Take
    {
        int track = -1, scene = -1, expectedUid = -1;
        bool contentStarted = false;
        double contentStart = 0;
        std::optional<model::Clip> overdub;
        uint64_t inputRoute = 0;
        model::Clip clip;
        std::array<int, 16 * 128> held{}; // note index per channel/pitch; -1 when released
        std::array<uint8_t, 16 * 128> inputHeld{}; // notes held through count-in / punch-in
        int numNotes = 0, numControllers = 0;
        std::array<int, model::midiControllerTargetCount> inputControllers{}; // -32768 when unseen
        std::vector<CapturedController> controllers; // sized before capture, never grows in callback
        std::vector<model::MidiNote> notes; // sized in start(); capture never grows it
        std::unique_ptr<juce::AbstractFifo> fifo;
        juce::AudioBuffer<float> ring;
        std::unique_ptr<juce::AudioFormatWriter> writer;
        int64_t frames = 0;
    };
    void mergeControllers(Take&, model::Clip&, bool overdub); // message thread only
    void run() override;
    void endCapture(); // audio thread, or message thread after callback removal
    void drain(Take& take); // worker only

    bool sessionScheduled_ = false, sessionStarted_ = false, sessionStopping_ = false, sessionEnded_ = false;
    SessionPlan sessionPlan_;
    std::atomic<bool> sessionStopRequested_{false};
    std::atomic<int> sessionPhase_{0};
    std::vector<std::unique_ptr<Take>> takes_;
    std::atomic<State> state_{State::Idle};
    std::atomic<bool> stopRequested_{false}, failed_{false};
    double sampleRate_ = 44100, startBeat_ = 0, lengthBeats_ = 0;
    bool started_ = false; // audio thread
    double punchStart_ = 0, punchEnd_ = 0; // immutable during capture; infinity disables the boundary
    std::atomic<bool> discontinuity_{false};
};

} // namespace sonora::engine
