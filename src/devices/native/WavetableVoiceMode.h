#pragma once
// DEV-18d: fixed-storage mono note bookkeeping and native portamento.
// Priority, sustain fallback, scaled timing and rational curve are independent Sonora policies;
// vendor note priority/timing/curve behavior still requires controlled reference measurements.

#include <array>
#include <cstdint>
#include "dsp/ExponentialEnvelopeCurve.h"

namespace sonora::devices
{
class WavetableVoiceMode final
{
public:
    static constexpr int channels = 16, notesPerChannel = 128;
    enum class Priority { Last, Low, High };
    struct Settings
    {
        bool legato = false, portaAlways = false, portaScaled = false;
        double portaSeconds = 0; // Fixed duration; scaled mode uses this duration per 12 semitones.
        float portaCurve = 0; // -100..100: fast/linear/slow rational progress curve.
        Priority priority = Priority::Last;
        bool deduplicate = false, globalSustain = false, exponentialPortamento = false;
    };
    struct Selection
    {
        int note = -1, channel = -1, velocity = 0;
        uint64_t age = 0;
        bool keyDown = false;
        bool active() const { return note >= 0; }
    };
    struct Transition
    {
        Selection before, after;
        bool changed = false, identityChanged = false, retrigger = false, legato = false;
        bool release = false, portamento = false;
    };
    class PitchGlide final
    {
    public:
        void prepare(double sampleRate);
        void reset(double pitch = 60);
        // Finite bounded inputs only; a rejected start preserves the previous glide.
        // Pass current() as fromPitch to retarget continuously during an existing glide.
        bool start(double fromPitch, double targetPitch, double seconds, float curve, bool exponential = false);
        double next();
        double current() const { return current_; }
        double target() const { return target_; }
        bool active() const { return elapsed_ < duration_; }
        uint64_t remainingSamples() const { return duration_ - elapsed_; }
    private:
        double sampleRate_ = 44100, from_ = 60, target_ = 60, current_ = 60, curveFactor_ = 1;
        uint64_t elapsed_ = 0, duration_ = 0;
        bool exponential_ = false;
        dsp::ExponentialEnvelopeCurve exponentialCurve_;
    };

    static float observedPortamentoSeconds(float normalized); // Observed 8*u^5 storage curve.
    void prepare(double sampleRate);
    void reset();
    // Audio-thread configuration from the parent's atomic block snapshot. Existing glides retain
    // their start-time duration/shape; priority changes may emit a new selection transition.
    Transition configure(const Settings&);
    const Settings& settings() const { return settings_; }
    // Channels are 0-based. Duplicate releases pair FIFO with physically held notes of that pitch.
    // Sustained notes remain priority candidates until pedal-up. Overflow evicts the oldest
    // entry of that channel, with no growth, allocation or cross-channel eviction.
    Transition noteOn(int channel, int note, int velocity);
    Transition noteOff(int channel, int note);
    Transition sustain(int channel, bool down);
    Transition allNotesOff(int channel); // Releases physical keys, respecting sustain.
    Transition allSoundOff(int channel); // Clears keys/latches; retains the channel's pedal state.
    Transition resetControllers(int channel); // Lifts pedal and clears only released latches.
    Selection selection(int channel) const;
    Selection selected() const { return selected_; } // One global mono selection across channels.
    int heldCount(int channel) const;
    int physicalHeldCount(int channel) const;
    bool sustainDown(int channel) const;
    double nextPitch();
    double currentPitch() const { return glide_.current(); }
    bool gliding() const { return glide_.active(); }
    uint64_t glideRemainingSamples() const { return glide_.remainingSamples(); }

private:
    struct Note { int note = -1, velocity = 0; uint64_t age = 0; bool keyDown = false; };
    static bool validChannel(int channel) { return channel >= 0 && channel < channels; }
    static bool validNote(int note) { return note >= 0 && note <= 127; }
    bool preferred(const Selection& candidate, const Selection& current) const;
    Transition update();
    Transition unchanged() const;
    std::array<std::array<Note, notesPerChannel>, channels> notes_{};
    std::array<bool, channels> pedals_{};
    Settings settings_;
    Selection selected_;
    PitchGlide glide_;
    uint64_t age_ = 0;
    bool pitchHistory_ = false;
};
} // namespace sonora::devices
