#pragma once
// Sonora Stutter (PLG-18): a vocal stutter / retrigger effect. It records its input all the time into a ring buffer and,
// while it is engaged, plays a captured slice back instead of the input in one of five modes:
//
//   Repeat   - loops the slice that starts where it was engaged (the first pass is the live input being captured); each
//              repeat can be shorter (Shrink: a roll that speeds up), higher or lower (Pitch, Ramp per repeat), quieter
//              (Decay) and panned left/right in turn (Wide).
//   Reverse  - loops the slice that ended where it was engaged, backwards.
//   Freeze   - holds the sound at the engage point with overlapping grains.
//   Stop     - tape stop: the playback slows down to a halt over the slice length.
//   Gate     - chops the live input in time with the Rate.
//
// It is engaged by (any of) the Hold control (a host-automatable switch), the 16-step pattern (one bar of 1/16 steps,
// each step Off or a mode; runs while the transport plays), or MIDI notes: C1..B1 (36..47) pick a mode while held
// (C Repeat, D Reverse, E Freeze, F Stop, G Gate), C2 and above (48+) play Repeat pitched chromatically from C3 (60).
// A trigger from Hold or MIDI waits for the Quantize grid when the transport plays.
//
// Spacing is how much of each repeat sounds (the rest is silence), Smooth the fade at every edge. Tone is a filter on the
// wet signal (left: low-pass, right: high-pass). Mix blends the dry input under the wet sound while engaged; idle, the
// input passes untouched. No latency.
//
// Display data (the face): the slice being played as min/max bins, the play position inside it, the repeat count and
// the active mode/step, written by the audio thread into atomics.

#include "devices/NativeDevice.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

namespace sonora::devices
{
class Stutter final : public NativeDevice
{
public:
    enum Mode { Repeat, Reverse, Freeze, Stop, Gate, kModes };
    static constexpr int kSteps = 16;
    enum Param
    {
        ModeParam, Hold, Rate, Sync, LengthMs, Quantize, Shrink, Pitch, Ramp, Decay, Spacing, Smooth, Wide, Tone, Mix,
        Output, PatternOn, Chance,
        StepStart, // kSteps parameters: 0 = off, 1 + mode
        NumParams = StepStart + kSteps
    };
    // Rate choices and their length in beats.
    static constexpr int kRates = 11;
    static constexpr const char* kRateNames[kRates] = {"1 Bar", "1/2", "1/4", "1/4T", "1/8", "1/8T", "1/16", "1/16T", "1/32", "1/32T", "1/64"};
    static constexpr double kRateBeats[kRates] = {4.0, 2.0, 1.0, 2.0 / 3.0, 0.5, 1.0 / 3.0, 0.25, 1.0 / 6.0, 0.125, 1.0 / 12.0, 0.0625};
    static constexpr int kQuantizes = 5;
    static constexpr double kQuantizeBeats[kQuantizes] = {0.0, 0.25, 0.5, 1.0, 4.0}; // Off, 1/16, 1/8, 1/4, 1 Bar
    static constexpr const char* kModeNames[kModes] = {"Repeat", "Reverse", "Freeze", "Stop", "Gate"};
    static constexpr double kHistorySeconds = 8.0;

    Stutter();
    std::string name() const override { return "Sonora Stutter"; }
    void prepare(double sampleRate, int maxBlockSize) override;
    void reset() override;
    void process(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void setHostInfo(const HostInfo& info) override { host_ = info; }
    std::vector<UiGroup> uiGroups() const override;
    bool parameterShown(int index) const override;

    static constexpr int stepParam(int step) { return StepStart + step; }
    // The slice length in samples for the current settings and tempo.
    double sliceSamples() const;

    // ---- face (message thread reads; audio thread writes)
    static constexpr int kViewBins = 192;
    struct View
    {
        bool engaged = false;
        int mode = 0, repeat = 0, step = -1; // step: the pattern step playing (-1: pattern idle)
        float position = 0; // 0..1 inside the slice
        int bins = 0;
        std::array<float, kViewBins> min{}, max{};
        uint32_t serial = 0;
    };
    // Copies the current view; the waveform part only when it changed since `serial`.
    void readView(View& out) const;
    // Momentary engage from the face (pads) or tests: mode -1 releases. Message thread; read by the audio thread.
    void press(int mode) { pressed_.store(mode, std::memory_order_relaxed); }
    int pressedMode() const { return pressed_.load(std::memory_order_relaxed); }

private:
    struct Event
    {
        bool active = false;
        int mode = Repeat;
        int64_t start = 0;     // absolute sample index where it was engaged
        double slice = 1;      // base slice length (samples)
        double current = 1;    // this repeat's length (Shrink)
        double pos = 0;        // samples since the start of this repeat
        double read = 0;       // Stop: read position (absolute, fractional); Freeze: grain phase
        double speed = 1;      // Stop: current speed
        float pitch = 0;       // extra semitones (MIDI pitched repeat)
        bool fromPast = false; // loops the slice that ended at `start` (pitched notes) instead of the one starting there
        int repeat = 0;
        float gain = 1, pan = 0;
    };
    struct Grain
    {
        double read = 0, pos = 0, length = 1;
        bool active = false;
    };

    void begin(int mode, float pitch, bool fromPast, int64_t at);
    void startRepeat();
    float fadeAt(double pos, double length, double smooth) const;
    void readHistory(double index, float& l, float& r) const;
    void wetSample(float inL, float inR, float& outL, float& outR);
    void publishSlice();
    int patternMode(double beat, bool& fresh);

    double sampleRate_ = 44100.0;
    HostInfo host_;
    std::vector<float> history_[2];
    int64_t written_ = 0; // samples written so far (absolute index of the next one)
    int historyMask_ = 0;
    Event event_;
    Grain grains_[2];
    int grainNext_ = 0;
    uint32_t noise_ = 0x9e3779b9u;
    float engage_ = 0;          // dry/wet crossfade 0..1
    float toneState_[2][2]{};   // SVF integrators
    int midiMode_ = -1;         // held mode note, or Repeat for a pitched note
    float midiPitch_ = 0;
    int midiNote_ = -1;
    bool midiFresh_ = false;    // a note-on this sample: retrigger even when the mode stays
    bool midiPitched_ = false, manualPast_ = false, pendingPast_ = false; // C2 and up: pitched Repeat from the past
    int manualSeen_ = -1, manualActive_ = -1, pendingMode_ = -1; // manual source seen / engaged / waiting for the grid
    float manualSeenPitch_ = 0, manualPitch_ = 0, pendingPitch_ = 0;
    double pendingBeat_ = -1;
    int lastStep_ = -1, stepMode_ = -1;
    bool stepFires_ = true;
    std::atomic<int> pressed_{-1};

    std::array<std::atomic<float>, kViewBins> viewMin_{}, viewMax_{};
    std::atomic<int> viewBins_{0}, viewMode_{0}, viewRepeat_{0}, viewStep_{-1};
    std::atomic<float> viewPos_{0};
    std::atomic<bool> viewEngaged_{false};
    std::atomic<uint32_t> viewSerial_{0};
};
} // namespace sonora::devices
