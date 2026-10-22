#pragma once
// DEV-21b: Note Length. Note On trigger: notes start as played and end after Length x Gate, ignoring
// the played note-offs; Latch holds them until the next note-on. Note Off trigger: a note starts when its key is
// released, with the note-on velocity decayed (Decay, Key Scale) and blended with the release velocity; Latch triggers the released notes
// together once every key is up and holds them until the next such trigger. Parameter ids are the reference app's
// MidiNoteLength element names. Audio thread: fixed arrays, capped output.

#include "devices/NativeDevice.h"

#include <array>

namespace sonora::devices
{

class NoteLength final : public NativeDevice
{
public:
    enum Param { Trigger, Latch, Sync, SyncedLength, TimeLength, Gate, ReleaseVelocity, DecayTime, KeyScale, NumParams };
    static constexpr int kMaxNotes = 128;

    NoteLength();
    std::string name() const override { return "Note Length"; }
    void prepare(double sampleRate, int maxBlockSize) override;
    void reset() override;
    void setHostInfo(const HostInfo& info) override { tempo_ = info.tempo > 0 ? info.tempo : 120.0; }
    void process(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    std::vector<UiGroup> uiGroups() const override;
    bool parameterShown(int index) const override;

    static double syncedLengthBeats(int index); // 0 = no length, 1 = 1/64 note ... 9 = 1/4 note (ROADMAP DEV-21c)
    // Length of an output note in samples for the current settings (Key Scale does not change it).
    double lengthSamples() const;
    // Note Off trigger: the note-on velocity after `heldMs` of a `decayMs` decay at `pitch` (exponential, measured).
    static double decayedVelocity(double velocity, double heldMs, int pitch, double decayMs, double keyScale);

private:
    struct Sounding { uint8_t pitch = 0, channel = 0; int64_t offAt = 0; }; // offAt < 0: held (Latch)
    struct Held { uint8_t pitch = 0, channel = 0, velocity = 0; int64_t onAt = 0; };

    void emit(const uint8_t* data, int size, int64_t at);
    void start(int pitch, int channel, int velocity, int64_t at, bool latched);
    void stop(int index, int64_t at);
    void releaseDue(int64_t limit);
    void releaseAll(int64_t at);

    double sampleRate_ = 44100.0, tempo_ = 120.0;
    juce::MidiBuffer scratch_;
    int bytes_ = 0;
    bool overflow_ = false;
    int64_t clock_ = 0, blockStart_ = 0;
    int blockLength_ = 0;
    std::array<Sounding, kMaxNotes> sounding_{};
    int numSounding_ = 0;
    std::array<Held, kMaxNotes> held_{};     // keys down (Note Off trigger: their note-on time and velocity)
    int numHeld_ = 0;
    std::array<Held, kMaxNotes> pending_{};  // Note Off + Latch: released keys waiting for the last key to go up
    int numPending_ = 0;
    std::array<std::array<bool, 128>, 16> owned_{}; // Note On trigger: keys whose note-on started one of our notes
    bool sustain_ = false;
    bool wasBypassed_ = false;
};

} // namespace sonora::devices
