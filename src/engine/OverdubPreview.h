#pragma once
// ENG-9f: audio-owned, bounded Session note/controller overdub playback. Storage is
// prepared before publication; the callback only changes reserved arrays and pooled step nodes.
#include "engine/ClipPlayers.h"
#include <array>

namespace sonora::engine
{
class OverdubPreview
{
public:
    static constexpr int maxRecordedNotes = 65536;
    OverdubPreview(int track, int scene, const model::Clip&);
    int track() const { return track_; }
    int scene() const { return scene_; }
    int uid() const { return uid_; }
    bool started() const { return started_; }
    bool stopped() const { return stopped_; }
    bool changed() const { return changed_; }
    void finish(double elapsed, int sample, juce::MidiBuffer& out);
    // `capture` intersects this segment in output sample space; null means playback only.
    bool render(const BlockSegment&, const juce::MidiBuffer& input, int channel, const BlockSegment* capture,
                bool stopAfterCapture, bool stopNow, juce::MidiBuffer&, bool refresh, bool* controllerOverflow, float velocityScale = 1.0f);
private:
    int appendNote(int key, int velocity, double elapsed);
    void closeNote(int index, double elapsed, int velocity);
    void stop(double elapsed, int sample, juce::MidiBuffer&);
    struct ControllerNode { double phase = 0; int next = -1, value = 0; bool original = false; };
    struct ControllerState
    {
        int head = -1, pending = -1, tail = -1, cursor = -1, originalLane = -1;
        int input = -32768, last = -32768;
        double first = 0;
        bool reading = false;
    };
    static constexpr int maxControllerNodes = 2 * 65536;
    int newControllerNode(double phase, int value, bool original);
    void captureController(int index, int value, double phase);
    void commitControllers(double end);
    void renderControllers(const BlockSegment&, const juce::MidiBuffer&, int channel, const BlockSegment* capture,
                           bool stopAfterCapture, bool stopNow, juce::MidiBuffer&, bool refresh, bool* overflow);
    int controllerValue(int index, double phase);
    std::vector<ControllerNode> controllerNodes_; // one shared fixed pool; pending + committed steps
    std::vector<PreparedMidiController> originalControllers_;
    std::array<ControllerState, 130> controllers_{};
    std::array<int, 130> controllerIndices_{};
    std::array<bool, 130> controllerPresent_{};
    int numControllerIndices_ = 0, freeController_ = 0;
    double previousPhase_ = 0;
    bool controllerStarted_ = false, controllersStopped_ = false, controllerEnded_ = false;
    PreparedClip clip_;
    int track_, scene_, uid_;
    size_t originalNotes_;
    std::vector<double> earliestOn_, disabledFrom_; // same capacity and index space as clip_.notes
    std::vector<double> inputStarts_;
    std::array<int, 16 * 128> held_{};
    std::array<uint8_t, 16 * 128> inputHeld_{};
    bool started_ = false, stopped_ = false, changed_ = false, full_ = false;
};
} // namespace sonora::engine
