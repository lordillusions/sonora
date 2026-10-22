#pragma once
// The runnable form of a Song: one node per track with prepared clips and device chains.
//
// Built on the message thread by buildGraph(), handed to the audio thread by
// AudioEngine (lock-free pointer swap), destroyed on the message thread again.
// While the audio thread owns a graph, nothing else may touch its nodes — UI talks to it
// only through TrackControls (atomics).

#include "core/Model.h"
#include "core/InputRouting.h"
#include "devices/DeviceFactory.h"
#include "devices/DeviceProcessor.h"
#include "engine/AudioClipPlayer.h"
#include "engine/Automation.h"
#include "engine/ClipPlayers.h"
#include "engine/DelayLine.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <bitset>
#include <memory>
#include <vector>

namespace sonora::engine
{

class SampleCache;

// Per-track values the UI may change or read while audio runs.
struct TrackControls
{
    std::atomic<float> volume{1.0f};
    std::atomic<float> pan{0.0f};
    std::atomic<bool> active{true}; // "track activator" (unmuted)
    std::atomic<bool> solo{false};
    std::atomic<bool> armed{false};  // receives live MIDI input
    std::atomic<int> monitoring{1}; // 0 In, 1 Auto, 2 Off
    std::atomic<uint64_t> inputRoute{0}; // packed model::InputSelection; 0 unset in standalone tests
    std::atomic<bool> midiControllerOverflow{false};
    std::atomic<int> previewPitch{-1}; // message-thread piano key audition, independent of arm/input/recording
    std::atomic<uint32_t> midiActivity{0}; // event generation for MIDI-only track activity indicators
    std::atomic<float> peakL{0.0f}, peakR{0.0f}; // written by audio thread
    // ---- session clip launching (ENG-6). Written by the message thread (ClipLauncher), consumed
    // by the audio thread at the next quantisation boundary.
    static constexpr int kNoSlot = -1;     // playingSlot: nothing playing
    static constexpr int kNothingQueued = -2;
    static constexpr int kQueuedStop = -1; // queuedSlot: stop at the boundary
    std::atomic<int> playingSlot{kNoSlot};
    std::atomic<int> queuedSlot{kNothingQueued};
    std::atomic<double> queuedQuant{0.0};  // beats; 0 == immediately
    std::atomic<float> queuedLaunchGain{1.0f}; // published with queuedSlot, affects only the new launch
    std::atomic<double> repeatQuant{0.0}; // original quantisation, including launches from stopped
    std::atomic<int> repeatSlot{kNoSlot}; // held Repeat launch button; release leaves playback running
    std::atomic<double> sessionElapsed{0.0}; // clip clock, advances independently of Arrangement wraps
    std::atomic<double> launchBeat{0.0};   // timeline beat at which the playing clip started
    std::atomic<int> auditionLane{-1}; // stable take-lane ID, published by audio at clip swap
    std::atomic<bool> arrangementOn{true}; // false once a session clip took the track over
    // Automation override (Re-Enable Automation): bit n is set while the control with key n (automationOverrideKey) is
    // held by hand instead of following its lane. The "last written / wrote" tables are audio-thread-only.
    static constexpr int kOverrideKeys = 64 + 16 * 64;
    std::array<std::atomic<uint64_t>, (kOverrideKeys + 63) / 64> automationOverrides{};
    std::atomic<bool> automationReEnable{false}; // message thread asks the audio thread to forget what the lanes wrote
    std::array<float, kOverrideKeys> automationLast{};
    std::bitset<kOverrideKeys> automationWrote;
    static constexpr int kMaxSends = 12;
    std::array<std::atomic<float>, kMaxSends> sends{}; // linear send level per return track
};

// Stable index space: [tracks..., returns..., main]. Matches Song order.
class ControlBank
{
public:
    explicit ControlBank(size_t n) : n_(n), items_(std::make_unique<TrackControls[]>(n)) {}
    size_t size() const { return n_; }
    TrackControls& operator[](size_t i) { return items_[i]; }
    const TrackControls& operator[](size_t i) const { return items_[i]; }

private:
    size_t n_;
    std::unique_ptr<TrackControls[]> items_;
};

// Per-channel gain for a track's volume (linear) and pan (-1..1). Balance-style law:
// the channel on the far side is attenuated, the near one keeps unity. (ENG-4)
struct StereoGains
{
    float left, right;
};
inline StereoGains panGains(float volume, float pan)
{
    pan = juce::jlimit(-1.0f, 1.0f, pan);
    return {volume * juce::jmin(1.0f, 1.0f - pan), volume * juce::jmin(1.0f, 1.0f + pan)};
}

// The arrangement part of a track that can be replaced while audio runs (UI-3 / UI-10): built on the
// message thread, handed to the audio thread through ClipSwap, swapped in at the start of a block.
struct PreparedTakeLane
{
    int id = -1;
    std::vector<PreparedClip> clips;
    std::vector<ClipEnvelopeBinding> envelopes;
};

struct ClipSet
{
    bool takesOnly = false;
    int auditionLane = -1;
    std::vector<PreparedTakeLane> takes;
    std::vector<PreparedClip> arrangement;
    std::vector<PreparedClip> session;
    std::vector<char> hasSessionClip;
    std::vector<ClipEnvelopeBinding> sessionEnvelopes;
    std::vector<PreparedMidiController> midiControllers;
    std::vector<AutomationBinding> automation; // arrangement lanes replaced with clip edits (UI-5)
    std::vector<ClipEnvelopeBinding> envelopes; // for the arrangement clips
};

struct ClipSwap
{
    std::atomic<ClipSet*> pending{nullptr}; // message thread -> audio thread
    std::atomic<ClipSet*> retired{nullptr}; // audio thread -> message thread (the set that was replaced)
    ~ClipSwap()
    {
        delete pending.load();
        delete retired.load();
    }
};

struct TrackNode
{
    model::TrackKind kind = model::TrackKind::Audio;
    std::string name;
    int controlIndex = 0;
    int auditionLane = -1;
    std::vector<PreparedTakeLane> takes;
    int outputNode = -1; // parent group node, -1 == main
    std::vector<PreparedClip> clips;        // arrangement clips (replaced as a whole through clipSwap)
    std::vector<PreparedClip> sessionClips; // one per scene; kind/notes empty and `hasSessionClip` false when the slot is empty
    std::vector<char> hasSessionClip; // audio thread
    std::vector<char> sessionAvailability; // message thread only: launch/UI metadata, never swapped by audio
    std::vector<std::unique_ptr<devices::DeviceProcessor>> devices;
    // Parallel to model::Track::devices: the processor built for each model device, or
    // nullptr when it could not be created (unsupported / missing). Used by the UI.
    std::vector<devices::DeviceProcessor*> processorForModelDevice;
    std::vector<PreparedMidiController> midiControllers;
    std::vector<AutomationBinding> automation; // evaluated once per block (ENG-11)
    std::vector<ClipEnvelopeBinding> clipEnvelopes;        // of the arrangement clips (replaced with them)
    std::vector<ClipEnvelopeBinding> sessionEnvelopes;     // of the session clips (fixed after the build)
    std::unique_ptr<ClipSwap> clipSwap = std::make_unique<ClipSwap>();

    juce::AudioBuffer<float> buffer;
    juce::MidiBuffer midi;
    juce::MidiBuffer inputMidi; // routed live input, before devices; also used by recording
    std::vector<uint8_t> inputMonitorMask; // preallocated per sample, follows clip/launch boundaries
    double sessionElapsed = 0; // audio-thread owned, published in TrackControls
    double repeatRemaining = 0; // beats to the next held Repeat retrigger (unfolded across Arrangement wraps)
    float sessionLaunchGain = 1.0f; // audio-owned gain/velocity scale, held through repeats and clip swaps
    bool inputMonitored = false; // audio thread: release voices when monitoring changes
    int previewPitch = -1; // audio-owned audition note; one pointer can hold one key
    bool recordingClipSuppressed = false;
    uint64_t lastInputRoute = 0;
    bool midiWasPlaying = false, refreshMidiControllers = true, midiControllerOverflow = false; // audio-owned
    bool clipsEdited = false; // audio thread: release old MIDI voices after a clip/session swap
    AudioPlayState audioState; // time-stretch state of the clip playing on this track (ENG-5)

    // Audio thread: gains applied in the previous block, so volume/pan changes ramp across
    // one block instead of stepping (ENG-4). Negative == not initialised yet (first block).
    float lastGainL = -1.0f, lastGainR = -1.0f;

    // Routing (ALS-12): a track routed to "None" does not feed its output (sends still work).
    bool outputEnabled = true;
    bool deactivated = false; // TRK-1: built without clips/devices and never audible (output and sends)
    float trackDelayMs = 0.0f;  // <TrackDelay>
    int trackDelaySamples = 0;  // derived in prepare(); positive delays the track (counts as latency for PDC)
    DelayLine trackDelay;
    // Sidechain taps other tracks listen to, filled when needed: 0 pre-FX, 1 post-FX, 2 post-mixer.
    std::array<bool, 3> needTap{false, false, false};
    std::array<juce::AudioBuffer<float>, 3> tap;
    struct SidechainLink
    {
        devices::DeviceProcessor* device = nullptr;
        int sourceNode = -1;
        int tap = 1;
        float gain = 1.0f;
    };
    std::vector<SidechainLink> sidechains;

    // Plugin delay compensation (ENG-12), filled by EngineGraph::prepare().
    int latency = 0;                    // device latency + the slowest input path, in samples
    std::vector<char> sendEnabled;      // per return: this track sends to it (decided at build time)
    DelayLine outDelay;                 // aligns this node's output with its siblings at the parent
    std::vector<DelayLine> sendDelays;  // per return, same for the send path
};

struct EngineGraph
{
    double preparedSampleRate = 0;
    int preparedBlockSize = 0;
    std::vector<TrackNode> nodes; // tracks (incl. groups), then returns, then main (last)
    std::vector<int> trackOrder;  // processing order for tracks: children before groups
    std::vector<int> returnNodes;
    int mainNode = -1;
    std::shared_ptr<ControlBank> controls;

    void prepare(double sampleRate, int maxBlockSize);
    // Called by prepare() after the devices are prepared (their latency is known then).
    void computeDelayCompensation();
};

struct GraphBuildResult
{
    std::unique_ptr<EngineGraph> graph;
    std::vector<std::string> log; // devices skipped, plugins missing, samples not found...
};

// Whether a track/group contributes to its output given the solo/activator state (ENG-7).
// A track plays when its activator is on and, if anything is soloed, it or one of its parent
// groups is soloed. Groups always pass (their children decide whether there is signal).
// Returns and main are not affected by solo here. Audio thread safe.
bool isTrackAudible(const EngineGraph& graph, const ControlBank& controls, const TrackNode& node, bool anySolo);

// Builds the replaceable clip part of one track from the model (message thread). `node` supplies the
// device processors that clip envelopes drive.
std::unique_ptr<ClipSet> buildClipSet(const model::Track& track, const TrackNode& node, const model::Song& song,
                                      SampleCache& samples, std::vector<std::string>& log, bool takesOnly = false);

GraphBuildResult buildGraph(const model::Song& song, devices::BuildContext ctx, SampleCache& samples);

} // namespace sonora::engine
