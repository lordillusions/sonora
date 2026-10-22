#pragma once
// DEV-21b: Arpeggiator. Turns held notes into a rhythmic pattern: 18 styles, synced (song grid) or free (ms) rate,
// swing groove, gate, hold/latch, pattern offset, repeats, Note/Beat retrigger, chromatic or in-scale transposition
// passes and a velocity ramp. Parameter ids are the reference app's MidiArpeggiator element names, so imported
// sets keep their settings and saved sets reopen there.
//
// Timing: while the transport plays and Sync is on, steps sit on the song's beat grid (a note pressed between steps
// waits for the next one). Stopped, or with Sync off, a free clock starts at the first note. Audio thread: fixed
// arrays only, generated MIDI is capped like the other MIDI effects.

#include "devices/NativeDevice.h"

#include <array>

namespace sonora::devices
{

class Arpeggiator final : public NativeDevice
{
public:
    enum Param
    {
        Style, Groove, Hold, Offset, Sync, SyncedRate, FreeRate, Gate, Retrigger, RetriggerInterval, Repeats,
        TransposeMode, TransposeKey, TransposeSteps, TransposeDistance, VelocityOn, VelocityRetrigger, VelocityDecay,
        VelocityTarget, NumParams
    };
    enum StyleId
    {
        Up, Down, UpDown, DownUp, UpAndDown, DownAndUp, Converge, Diverge, ConAndDiverge, PinkyUp, PinkyUpDown, ThumbUp,
        ThumbUpDown, PlayOrder, ChordTrigger, Random, RandomOther, RandomOnce, NumStyles
    };
    static constexpr int kMaxNotes = 64;
    static constexpr int kMaxPattern = 4 * kMaxNotes;

    Arpeggiator();
    std::string name() const override { return "Arpeggiator"; }
    void prepare(double sampleRate, int maxBlockSize) override;
    void reset() override;
    void setHostInfo(const HostInfo& info) override { host_ = info; }
    void process(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    std::vector<UiGroup> uiGroups() const override;
    bool parameterShown(int index) const override;

    // Length of one step / one retrigger interval in beats for a SyncedRate / RetriggerInterval index.
    static double syncedRateBeats(int index);
    static double retriggerIntervalBeats(int index);
    // The pattern for `count` notes sorted low to high: indices into that order, played left to right. `arrival`
    // gives each sorted note's rank in playing order (Play Order). Random styles use `seed`. Returns the length.
    static int buildPattern(int style, int count, const int* arrival, uint32_t& seed, int* out);
    // Swing: maps a straight beat position to its grooved position (monotonic; 0 = no groove).
    static double grooveWarp(double beat, int groove);

private:
    struct Note
    {
        uint8_t pitch = 0, velocity = 0, channel = 0;
        bool physical = false; // key still down (false = latched by Hold)
        uint32_t order = 0;    // arrival counter, for Play Order
    };
    struct Sounding
    {
        uint8_t pitch = 0, channel = 0;
        int64_t offAt = 0; // device sample clock
    };

    void handleEvent(const juce::MidiMessageMetadata& meta);
    void addNote(int pitch, int velocity, int channel);
    void removeNote(int index);
    void dropLatched();
    void notesChanged() { patternDirty_ = true; }
    void runUntil(int64_t limit); // note-offs and steps strictly before `limit` (device clock)
    void fireStep(int64_t at);
    void noteOff(int index, int64_t at);
    void emit(const uint8_t* data, int size, int64_t at);
    void rebuildPattern();
    int64_t nextGridStep(); // device-clock sample of the next grid step at or after the cursor, or INT64_MAX
    double stepSamples() const;

    HostInfo host_;
    double sampleRate_ = 44100.0;
    juce::MidiBuffer scratch_;
    int bytes_ = 0;
    bool overflow_ = false;

    std::array<Note, kMaxNotes> notes_{}; // sorted by pitch, then channel
    int count_ = 0;
    uint32_t arrivals_ = 0;
    std::array<Sounding, kMaxPattern> sounding_{};
    int numSounding_ = 0;
    std::array<int, kMaxPattern> pattern_{};
    int patternLength_ = 0;
    bool patternDirty_ = true;
    uint32_t seed_ = 0x5eed1234u;

    // Clocks. `clock_` counts device samples; the block being processed spans [blockStart_, blockStart_ + blockLength_).
    int64_t clock_ = 0, blockStart_ = 0;
    int blockLength_ = 0;
    bool gridMode_ = false;
    double blockBeat_ = 0, samplesPerBeat_ = 22050;
    double expectedBeat_ = -1e30;        // continuity check of the song position
    double windowBeat_ = 0;              // grid steps from here on belong to this block
    double pendingBeat_ = 0, pendingNominal_ = 0; // the step nextGridStep() returned
    double lastStepBeat_ = -1e30;        // grooved beat of the last grid step (grid mode)
    double lastStepNominal_ = -1e30;     // straight beat of the last grid step (Beat retrigger)
    int64_t gridCursor_ = 0;             // next grid step index to consider
    int64_t nextFree_ = -1;              // device clock of the next free-running step, -1 = idle
    int64_t lastStep_ = -1;              // device clock of the last step
    int64_t velocityStart_ = 0;
    int64_t stepCounter_ = 0;
    bool retriggerPending_ = false;
    bool wasBypassed_ = false;
    std::array<std::array<bool, 128>, 16> passedThrough_{}; // notes forwarded unchanged while bypassed
};

} // namespace sonora::devices
