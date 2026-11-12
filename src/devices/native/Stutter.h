#pragma once
// Sonora Stutter (PLG-18, reworked by PLG-23): a granular voice stutter. It records its input all the time and, while it
// is engaged, replays what was just sung as a stream of grains instead of the input: stretched slower and slower until it
// stands still, rewound, shattered into fragments, frozen into a pad, spread into a wide cloud, sunk into a resonant
// tunnel, or retriggered as a classic stutter roll.
//
// One knob plays it: Motion, from Slow (long, swelling grains that barely move) to Fast (short, tight grains racing). Each
// Mode gives Motion its own character; while Macro is on, Motion drives the grain controls (Grain, Shape, Every, Speed,
// Width, Jitter, Space) and the face shows them moving. Turning one of them by hand switches Macro off and keeps the
// values it had (detachMacro), so the sound does not jump.
//
// The grains: every `Every` ms (or a note value when Sync is on, on the transport's grid while it plays) a grain of
// `Grain` ms starts reading the recording at the read head, with a fade in/out set by Shape (0: hard gate, 1: it swells
// in and out). The read head starts where the stutter was engaged and moves through the recording at Speed (1 = real
// time, 0.25 = four times slower, 0 = frozen, negative = backwards), inside the Capture window (the last beat, bar, two or
// four bars before the engage point, or the whole recording); it wraps around at the window's edges. Pitch transposes the
// grains, Width pans odd grains left and even ones right, Jitter scatters their position and timing. Tone filters the
// grains (left: darker, right: thinner), Space adds a diffuse room that rings on after the stutter stops, Mix keeps some of
// the dry voice under it, Level sets the stutter's volume (the dry input passes untouched while it is idle).
//
// It is engaged by the Hold control (latched, automatable), the face's hold pad (momentary) or MIDI notes: the white keys
// of any octave play a mode while held (C Swell, D Rewind, E Shatter, F Freeze, G Spread, A Tunnel, B Retrigger), black
// keys the selected mode. Every new note re-anchors the read head at that moment. No latency.
//
// Display data (the face): the input and output waveforms of the last seconds, the grains (when, where they read, which
// side), the read head, the capture window and the settings that play, written by the audio thread into atomics.

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
    enum Mode { Swell, Rewind, Shatter, Freeze, Spread, Tunnel, Retrigger, kModes };
    enum Param
    {
        ModeParam, Motion, Macro, Hold, Capture, Sync, Grain, Shape, Every, Speed, Pitch, Width, Jitter, Tone, Space, Mix,
        Output, NumParams
    };
    static constexpr const char* kModeNames[kModes] = {"Swell", "Rewind", "Shatter", "Freeze", "Spread", "Tunnel", "Retrigger"};
    static constexpr const char* kModeKeys[kModes] = {"C", "D", "E", "F", "G", "A", "B"};
    static constexpr int kCaptures = 5;
    static constexpr const char* kCaptureNames[kCaptures] = {"1 Beat", "1 Bar", "2 Bars", "4 Bars", "All"};
    static constexpr double kCaptureBeats[kCaptures] = {1.0, 4.0, 8.0, 16.0, 0.0}; // 0: the whole recording
    static constexpr double kHistorySeconds = 20.0;
    static constexpr int kMaxGrains = 48;
    static constexpr int kPressCurrent = 100; // press(): engage with the selected mode

    // What the grains do now: from Motion (Macro on) or from the knobs.
    struct Settings
    {
        float grainMs = 120, shape = 0.6f, everyMs = 60, speed = 0.25f, width = 0.3f, jitter = 0.1f, space = 0.15f;
        float pitch = 0;            // semitones
        float detune = 0;           // Spread: odd grains down, even grains up (semitones)
        float scatter = 0;          // Shatter: chance of a reversed or octave-shifted fragment
        float tunnelMs = 0, tunnelFeedback = 0; // Tunnel: the resonant tube (0 ms: off)
        float toneShift = 0;        // added to Tone
        bool reverseGrains = false; // Rewind: every grain plays backwards
        bool fixedSlice = false;    // Retrigger: grains repeat the slice before the engage point
        double everyBeats = 0;      // > 0: on the tempo grid (Sync, Retrigger)
    };
    // The settings Motion gives a mode (Sync: grid-locked note values at this tempo). Pure; for the face and tests.
    static Settings macroSettings(int mode, float motion, double tempo, bool sync);
    // Snaps a time to the nearest note value at a tempo (1/64 .. 1 bar, triplets too); returns its length in beats.
    static double snapToNote(double ms, double tempo);
    static const char* noteName(double beats);

    Stutter();
    std::string name() const override { return "Sonora Stutter"; }
    void prepare(double sampleRate, int maxBlockSize) override;
    void reset() override;
    void process(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void setHostInfo(const HostInfo& info) override { host_ = info; }
    std::vector<UiGroup> uiGroups() const override;

    // The settings playing now (or that would play): Macro or knobs, with Sync applied. Any thread.
    Settings settings() const;
    double tempo() const { return host_.tempo > 0 ? host_.tempo : 120.0; }
    // Message thread: copies what Motion drives into the knobs and switches Macro off.
    void detachMacro();
    // Momentary engage from the face or tests: a mode, kPressCurrent, or -1 to release. Read by the audio thread.
    void press(int mode) { pressed_.store(mode, std::memory_order_relaxed); }
    int pressedMode() const { return pressed_.load(std::memory_order_relaxed); }

    // ---- display (message thread reads; audio thread writes)
    static constexpr int kBins = 384;
    static constexpr double kInputSeconds = 12.0, kOutputSeconds = 4.0;
    static constexpr int kGrainLog = 64;
    struct GrainMark
    {
        int64_t at = 0;     // absolute sample where it started
        int64_t read = 0;   // absolute recording sample it read from (its start)
        int length = 0;     // samples
        float pan = 0;
        bool reversed = false;
    };
    struct View
    {
        bool engaged = false;
        int mode = 0;
        int64_t now = 0, anchor = 0, head = 0, windowStart = 0;
        double sampleRate = 44100;
        int inputBins = 0, outputBins = 0;        // bins filled so far (up to kBins)
        int64_t inputNewest = 0, outputNewest = 0; // absolute sample of the newest bin's end
        std::array<float, kBins> inMin{}, inMax{}, outMin{}, outMax{}; // oldest first
        std::vector<GrainMark> grains;            // newest last
        Settings playing;
        float level = 0;                          // output peak, recent
    };
    void readView(View&) const;

private:
    struct GrainVoice
    {
        bool active = false;
        double read = 0, step = 1; // recording position, samples per output sample (negative: backwards)
        int pos = 0, length = 1, fade = 1;
        float gainL = 1, gainR = 1, amp = 1;
    };
    struct Room // a small stereo feedback-delay network (Space)
    {
        std::array<std::vector<float>, 4> lines;
        std::array<int, 4> length{}, index{};
        std::array<float, 4> damp{};
        void prepare(double sampleRate);
        void clear();
        void process(float inL, float inR, float& outL, float& outR);
    };
    struct Tube // a resonant feedback comb per side (Tunnel)
    {
        std::vector<float> line[2];
        int index = 0;
        float damp[2]{};
        void prepare(double sampleRate);
        void clear();
        void process(float& l, float& r, float delaySamples, float feedback);
    };

    void engage(int64_t at);
    void spawnGrain(const Settings&, int64_t now);
    float random01();
    void readRecording(double index, float& l, float& r) const;
    int64_t captureSamples() const;
    void publishBins(float inL, float inR, float outL, float outR);

    double sampleRate_ = 44100.0;
    HostInfo host_;
    std::vector<float> history_[2];
    int64_t written_ = 0;
    int historyMask_ = 0;
    std::array<GrainVoice, kMaxGrains> grains_{};
    bool engaged_ = false;
    int playingMode_ = 0;
    int64_t anchor_ = 0, windowStart_ = 0;
    double head_ = 0;
    double untilGrain_ = 0;
    double lastGridIndex_ = -1;
    float engage_ = 0;
    float toneState_[2][2]{};
    Room room_;
    Tube tube_;
    uint32_t noise_ = 0x9e3779b9u;
    int midiNote_ = -1, midiMode_ = -1;
    bool midiFresh_ = false;
    int lastPressed_ = -1;
    std::atomic<int> pressed_{-1};

    // display
    int inBinSamples_ = 1, outBinSamples_ = 1, inCount_ = 0, outCount_ = 0;
    float inLo_ = 0, inHi_ = 0, outLo_ = 0, outHi_ = 0;
    std::array<std::atomic<float>, kBins> inMin_{}, inMax_{}, outMin_{}, outMax_{};
    std::atomic<int> inWrite_{0}, outWrite_{0}, inFilled_{0}, outFilled_{0};
    std::atomic<int64_t> inNewest_{0}, outNewest_{0}, now_{0}, anchorShown_{0}, headShown_{0}, windowShown_{0};
    std::atomic<bool> engagedShown_{false};
    std::atomic<int> modeShown_{0};
    std::atomic<float> level_{0};
    struct GrainLogEntry
    {
        std::atomic<int64_t> at{0}, read{0};
        std::atomic<int> length{0};
        std::atomic<float> pan{0};
        std::atomic<bool> reversed{false};
    };
    std::array<GrainLogEntry, kGrainLog> grainLog_{};
    std::atomic<uint32_t> grainLogNext_{0};
    int grainParity_ = 0;
};
} // namespace sonora::devices
