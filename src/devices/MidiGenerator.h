#pragma once
// PLG-25a: base class of Sonora's pattern generators (Sonora Phrase PLG-25, Sonora Pulse PLG-26). A MIDI effect: held
// notes come in, generated notes (and CC, pitch bend) go out. The subclass only says WHAT plays over a span of song
// beats; this class does everything about WHEN:
//
//   - Song-grid timing from HostInfo. A generator is a function of the song position (TRANCE_SUITE.md "Decisions" 3):
//     generate(from, to, sink) is called for consecutive spans [from, to) of song beats and writes the events whose beat
//     lies in the span. Spans are cut at every incoming MIDI event (so the held notes are right at each moment), at block
//     ends and at the loop end. A position jump (start, locate, loop wrap: also a loop end inside a block, read from
//     HostInfo::looping/loopEnd) ends every sounding note and calls located(beat) before the next span, so a subclass with
//     history (a scale walk) can rebuild it there.
//   - A free clock while the transport is stopped: it starts at beat 0 when runsWhenStopped() becomes true (by default:
//     a note is held) and generate() runs on it until playback starts.
//   - The held-note table (sorted by pitch, up to kMaxHeld) with latch (latchOn()): released keys stay held until a new
//     gesture (a key pressed with none physically down) replaces them. Input notes are consumed, other MIDI passes.
//   - Sounding notes and their note-offs in fixed arrays: every note-on gets its note-off; a note-on of a pitch that is
//     still sounding on that channel ends it first (retrigger); stop, bypass, locate and loop wrap end everything (and
//     centre a bent channel).
//   - The 64 KB MIDI cap per block: past it the block's output is dropped and every sounding note ended.
//   - Offline rendering (renderNotes) of exactly what the device plays, on a clone: "To Clip" and "Drag MIDI".
//
// Audio thread: process(), generate(), located(), blockStarted() and heldChanged() must not allocate or lock. The Sink
// and every table are fixed-size. Subclasses that hold non-parameter state publish it through PatternSlot and take it in
// blockStarted().

#include "core/Model.h"
#include "devices/NativeDevice.h"

#include <array>
#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace sonora::devices
{
class MidiGenerator : public NativeDevice
{
public:
    static constexpr int kMaxHeld = 32;
    static constexpr int kMaxSounding = 256;
    static constexpr int kMaxEvents = 2048;  // events one generate() call can write; more are dropped (and counted)
    static constexpr int kByteLimit = 64000; // generated MIDI bytes per block (like the other MIDI effects)

    // What generate() writes into. Beats are song beats (or the free clock's beats); an event whose beat is outside the
    // span being generated is dropped, so a strum or humanize offset must be decided by the span its own onset lies in.
    class Sink
    {
    public:
        void note(double beat, double lengthBeats, int pitch, int velocity, int channel = 0);
        void cc(double beat, int number, int value, int channel = 0);
        void bend(double beat, int value14, int channel = 0); // 0..16383, 8192 = centre
        double from() const { return from_; }
        double to() const { return to_; }
        int size() const { return count_; }
        bool full() const { return count_ >= kMaxEvents; }

    private:
        friend class MidiGenerator;
        struct Event
        {
            double beat = 0, length = 0;
            int order = 0;
            uint8_t kind = 0, channel = 0, data1 = 0, data2 = 0;
            uint16_t value14 = 8192;
        };
        void push(const Event&);
        std::array<Event, kMaxEvents> events_{};
        int count_ = 0, dropped_ = 0;
        double from_ = 0, to_ = 0;
    };

    struct HeldNote
    {
        uint8_t pitch = 0, velocity = 0, channel = 0;
        bool physical = false; // key down (false: kept by latch)
        uint32_t order = 0;    // arrival counter
    };

    explicit MidiGenerator(std::vector<ParameterInfo> parameters);

    void prepare(double sampleRate, int maxBlockSize) override;
    void reset() override;
    void setHostInfo(const HostInfo& info) override { host_ = info; }
    void process(juce::AudioBuffer<float>&, juce::MidiBuffer&) final;

    // ---- non-parameter state (message thread). A device with state returns its format ("sonora-phrase-v1"); the app saves
    // it as model::NativeContent (DeviceFactory captureDeviceState / createNative) and the VST3 inside its state.
    virtual std::string stateFormat() const { return {}; }
    virtual std::vector<uint8_t> saveState() const { return {}; }
    virtual bool loadState(const uint8_t* data, size_t size, std::string& error);
    // A new instance of the same device with default settings. cloneForRender() copies parameters and state into it.
    virtual std::unique_ptr<MidiGenerator> createEmpty() const = 0;
    std::unique_ptr<MidiGenerator> cloneForRender() const;

    // What the device plays over song beats [fromBeat, toBeat) at `tempo` with `input` held (notes in song beats; empty
    // for a generator that needs no input), rendered on a clone with the transport playing: exact beats, not samples.
    // Notes still sounding at toBeat end there. Message thread; the running instance is not touched.
    std::vector<model::MidiNote> renderNotes(const std::vector<model::MidiNote>& input, double fromBeat, double toBeat,
                                             double tempo, double sampleRate = 48000.0, int blockSize = 512) const;

    // ---- To Clip (message thread, see ui/GeneratorClipUi.h)
    // Whether the notes held while it plays are its input (true: a clip's own notes are rendered as held notes; Phrase in
    // Progression mode returns false, so writing into a clip replaces the clip's notes without reading them).
    virtual bool listensToInput() const { return true; }
    // The length of a new clip written with no clip selected (Phrase: one progression or form; Pulse: one cycle).
    virtual double clipLengthBeats() const { return 16.0; }

    // ---- faces (any thread): the position at the end of the last block (song beats, or the free clock) and whether
    // the transport was playing.
    double playBeat() const { return playBeat_.load(std::memory_order_relaxed); }
    bool transportPlaying() const { return playing_.load(std::memory_order_relaxed); }
    // ---- tests: events generate() wrote that did not fit the sink or the block's MIDI cap (cumulative).
    int droppedEvents() const { return dropped_.load(std::memory_order_relaxed); }

protected:
    // Audio thread. Write the events whose beat is in [fromBeat, toBeat) into sink.
    virtual void generate(double fromBeat, double toBeat, Sink& sink) = 0;
    // Audio thread. The position jumped to `beat` (start of playback, locate, loop wrap, free clock start): the next span
    // starts there. Rebuild state with history here.
    virtual void located(double beat) { (void) beat; }
    // Audio thread, first thing in every process() (also while bypassed): take PatternSlot updates here.
    virtual void blockStarted() {}
    // Audio thread: the held notes changed (a key, a release, latch).
    virtual void heldChanged() {}
    virtual bool latchOn() const { return false; }
    // Stopped transport: whether the free clock runs (default: while a note is held).
    virtual bool runsWhenStopped() const { return heldCount_ > 0; }

    // The beat generation has reached (inside heldChanged(): the moment of the key; inside generate(): the span's start).
    double currentBeat() const { return cursorBeat_; }
    // Ends every sounding note now (from heldChanged() or located(); a chord change that must not ring on).
    void releaseSounding();

    int heldCount() const { return heldCount_; }
    const HeldNote& held(int index) const { return held_[(size_t) index]; } // sorted by pitch, then channel
    const HostInfo& host() const { return host_; }
    double sampleRate() const { return sampleRate_; }

private:
    struct Sounding
    {
        double startBeat = 0, offBeat = 0;
        uint8_t pitch = 0, channel = 0;
        int record = -1; // renderNotes: index into the recorded notes
    };

    void runTo(int sample);
    void window(double from, double to);
    void flushOffs(double limit, bool inclusive);
    void endNote(int index, int sample, double beat);
    void allNotesOff(int sample, double beat);
    void emitNote(const Sink::Event&);
    void emit(const uint8_t* data, int size, int sample, bool force = false);
    int sampleOf(double beat) const;
    void handleInput(const juce::MidiMessageMetadata&, int sample);
    void addHeld(int pitch, int velocity, int channel);
    void removeHeld(int index);
    void dropLatched();
    void startFreeClock(int sample);

    HostInfo host_;
    double sampleRate_ = 44100.0, samplesPerBeat_ = 22050.0;
    juce::MidiBuffer scratch_;
    int bytes_ = 0;
    bool overflow_ = false;

    Sink sink_;
    std::array<HeldNote, kMaxHeld> held_{};
    int heldCount_ = 0;
    uint32_t arrivals_ = 0;
    std::array<Sounding, kMaxSounding> sounding_{};
    int numSounding_ = 0;
    std::array<bool, 16> bent_{};

    // block / clock state (audio thread)
    int blockLength_ = 0, cursorSample_ = 0, segmentSample_ = 0, wrapSample_ = -1;
    double segmentBeat_ = 0, cursorBeat_ = 0, expectedBeat_ = 0, freeBeat_ = 0;
    bool wrapped_ = false, wasPlaying_ = false, freeRunning_ = false, wasBypassed_ = false;
    std::array<std::array<bool, 128>, 16> passedThrough_{};

    std::atomic<double> playBeat_{0.0};
    std::atomic<bool> playing_{false};
    std::atomic<int> dropped_{0};
    std::vector<model::MidiNote>* record_ = nullptr; // renderNotes only (message thread, on a clone)
};
} // namespace sonora::devices
