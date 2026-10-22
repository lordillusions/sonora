#pragma once

#include "engine/Recorder.h"

namespace sonora::engine
{

// ENG-9d: Arrangement keeps one grouped take. Session captures own one immutable recorder per
// track. Message-thread banks own lifetimes; audio only exchanges pointers and fills reserved ranges.
class RecordingManager final
{
public:
    using SessionPlan = Recorder::SessionPlan;
    using CaptureRange = Recorder::CaptureRange;
    using Result = Recorder::Result;
    static constexpr int maxSegments = 1024;

    ~RecordingManager();
    bool start(const model::Song&, const ControlBank&, const juce::File&, double sampleRate,
               std::string& error, bool arrangement = true, const SessionPlan* = nullptr); // message
    bool active() const;
    bool busy() const;
    bool finished() const;
    bool sessionRecording() const;
    int sessionScene(int track) const;
    int sessionPhase(int track = -1) const; // 4 = draining, independent of the other tracks
    bool capturesTrack(int track) const; // message-thread target protection, includes draining
    Result takeResult(); // collects finished tracks while other tracks continue
    void requestSessionStop(int track = -1, bool launchAfterRecording = true); // message
    void requestStop(); // message or audio: atomic stop flags only
    void collectRetiredPreviews(); // message
    void deviceStopped(); // after callback removal

    void beginBlock(); // audio, before any recorder access
    void prepareBlock(const BlockSegment*, int count, bool playing, bool jumped); // audio
    const CaptureRange* rangesForTrack(int track) const; // audio, valid until next prepareBlock
    bool audioCapturesTrack(int track) const;
    bool audioSessionRecording(int track) const;
    bool overdubsTrack(int track) const; // audio
    void beginSessionContent(int track, double elapsed); // audio
    void beginPreviewBlock(const EngineGraph&);
    bool hasOverdubPreview(int track, int scene, uint32_t uid);
    bool renderOverdub(int track, int scene, const PreparedClip&, const BlockSegment&, const juce::MidiBuffer&,
                       const CaptureRange*, juce::MidiBuffer&, bool refresh, bool* overflow, float velocityScale = 1.0f);
    bool commitOverdub(int track, int scene, const PreparedClip*, double elapsed, juce::MidiBuffer&);
    void capture(const float* const*, int numInputs, const juce::MidiBuffer&, double beat, double samplesPerBeat,
                 int numSamples, bool playing, bool jumped, bool loopEnd, const EngineGraph* = nullptr,
                 int sampleOffset = 0, const CaptureRange* = nullptr);

private:
    struct SessionTake
    {
        Recorder recorder;
        int track = -1, scene = -1;
        bool collected = false; // message thread only
        bool launchAfterRecording = true; // message thread only
        std::array<CaptureRange, maxSegments> ranges{}; // audio only
    };
    struct Bank { std::vector<std::shared_ptr<SessionTake>> takes; };
    void publish(); // message, immutable bank; shared ownership is never changed by audio
    SessionTake* audioTake(int track) const;
    Recorder arrangement_;
    std::vector<std::shared_ptr<SessionTake>> sessions_; // message only
    std::atomic<Bank*> pending_{nullptr}, retired_{nullptr};
    Bank* audio_ = nullptr;
    std::array<CaptureRange, maxSegments> arrangementRanges_{};
    int numRanges_ = 0; // audio only
    std::array<int, maxSegments> segmentOffsets_{};
    std::atomic<bool> stopAllRequested_{false};
    juce::CriticalSection lifecycle_; // start/retirement vs device-stop notification; never taken by renderBlock
};

} // namespace sonora::engine
