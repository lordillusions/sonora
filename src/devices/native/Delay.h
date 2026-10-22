#pragma once
// Delay (.als "Delay") — ROADMAP DEV-8 / DEV-8c.
//
// Parameter set, ids, units, enum orders and defaults follow Live 12's Delay (schema 12.0_12402 plus the values
// Live itself writes in its default device file and in real sets; see docs/NATIVE_DELAY_REVERB.md).
//
//  * Two delay lines. Each is tempo synced (SyncedSixteenth index 0..15 == 1..16 sixteenths) or free (1 ms .. 5 s),
//    with a per-line offset of +-33 % of the time (swing).
//  * Link only links the *editing* of the two sides (the UI moves both); the stored times stay independent, exactly
//    like in Live sets, so the engine never forces R = L.
//  * Band-pass filter in front of the delay line, inside the feedback loop (every repeat is filtered, also the first).
//  * LFO (Rate / Time / Synced / Triplet / Dotted / Sixteenth, seven waves + Morph) modulating delay time and filter.
//  * Time-change smoothing: Repitch (tape style glide), Fade (cross-fade of two taps), Jump (immediate).
//  * Freeze loops the buffer; Ping Pong sends the input to the left line and cross-feeds the lines.
//  * Dry/Wet in Classic (linear) or Equal-Loudness (equal-power) law; Eco lowers the control-rate update.
//
// Sonora's own DSP: the numeric laws that Live does not document (glide speed, fade length, modulation depth
// scaling, Morph) are documented in docs/NATIVE_DELAY_REVERB.md and are not measured against Live audio.

#include "devices/NativeDevice.h"
#include "devices/native/Biquad.h"
#include "dsp/Dsp.h"
#include "dsp/ModLfo.h"

namespace sonora::devices
{

class Delay final : public NativeDevice
{
public:
    enum Param
    {
        SmoothingMode,       // DelayLine_SmoothingMode: 0 Repitch, 1 Fade, 2 Jump
        Link,                // DelayLine_Link (editing only)
        PingPong,            // DelayLine_PingPong
        SyncL, SyncR,        // DelayLine_SyncL/R
        TimeL, TimeR,        // DelayLine_TimeL/R, seconds when not synced (measured: the reference plays these, DEV-22b)
        SyncedL, SyncedR,    // DelayLine_SyncedSixteenthL/R, index 0..15 (value + 1 sixteenths)
        OffsetL, OffsetR,    // DelayLine_OffsetL/R, fraction of the time (-0.33 .. 0.33)
        Feedback,            // 0..1
        Freeze,
        FilterOn,
        FilterFrequency,     // Hz
        FilterBandwidth,     // 0.5..9 (octaves)
        ModTimeMode,         // Modulation_TimeMode: Rate, Time, Synced, Triplet, Dotted, Sixteenth
        ModFrequency,        // Hz (Rate)
        ModTime,             // seconds per cycle (Time)
        ModSyncedRate,       // note denominator (Synced / Triplet / Dotted): 4 == quarter note
        ModSixteenth,        // sixteenths per cycle (Sixteenth)
        ModWaveform,         // Sine, Triangle, Ramp Up, Ramp Down, Square, S&H, Wander
        ModMorph,
        ModAmountTime,       // 0..1
        ModAmountFilter,     // 0..1
        DryWet,
        DryWetMode,          // 0 Classic, 1 Equal Loudness
        EcoProcessing,
        // Stored by Live but not used by the engine; kept so authored and re-saved devices stay complete.
        LegacyTimeL, LegacyTimeR,           // DelayLine_SimpleDelayTimeL/R (ms; not what the reference plays)
        PingPongTimeL, PingPongTimeR,       // DelayLine_PingPongDelayTimeL/R
        CompatibilityMode,                  // DelayLine_CompatibilityMode
        NumParams
    };

    Delay();
    std::string name() const override { return "Delay"; }
    void prepare(double sampleRate, int maxBlockSize) override;
    void process(juce::AudioBuffer<float>& audio, juce::MidiBuffer& midi) override;
    void reset() override;
    void setHostInfo(const HostInfo& info) override
    {
        tempo_ = std::max(20.0, info.tempo);
        beats_ = info.beats;
        playing_ = info.playing;
    }
    std::vector<UiGroup> uiGroups() const override;
    bool parameterShown(int index) const override;
    // Editing helper for the UI: with Link on, a change to one side is applied to the other side too
    // (toggles copied, sync index / offset shifted by the same amount, free times scaled by the same ratio).
    // The stored values stay independent, so sets whose sides differ keep sounding the same.
    void setParameterLinked(int index, float value);

private:
    double modulationPeriodBeats() const; // 0 when the LFO is not tempo synced
    float targetSamples(int channel, double sixteenthSamples) const;

    double sampleRate_ = 44100.0;
    double tempo_ = 120.0, beats_ = 0.0;
    bool playing_ = false;

    dsp::FractionalDelay line_[2];
    SvfCoeffs highPass_, lowPass_;         // measured filter: Butterworth HP at f / 2^(w/2) and LP at f * 2^(w/2)
    SvfState filterState_[2][2];           // state-variable form: the LFO / automation move the band (DEV-24)
    dsp::ModLfo lfo_;

    bool initialised_ = false;
    int lastMode_ = -1;
    float current_[2] = {0, 0};  // Repitch: gliding delay in samples
    float timeA_[2] = {0, 0};    // Fade: outgoing tap
    float timeB_[2] = {0, 0};    // Fade: incoming tap
    float fadeMix_[2] = {1, 1};
    float feedbackNow_ = 0.5f, inputNow_ = 1.0f, wetNow_ = 0.5f; // smoothed control values
};

} // namespace sonora::devices
