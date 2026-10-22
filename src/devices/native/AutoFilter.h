#pragma once
// Auto Filter (.als "AutoFilter" / "AutoFilter2") — ROADMAP DEV-9.
//
// State-variable filter (low-pass, high-pass, band-pass, notch, or a morph between them) with
// 12 or 24 dB slope, resonance, and an LFO on the cutoff (free-running in Hz or tempo synced).
// Cutoff is stored by Live as a MIDI-note-like pitch (0..135): frequency = 440 * 2^((n - 69) / 12).
// Not modelled: the envelope follower (its amount is not among the stored parameters we
// recognise), the circuit models (OSR / MS2 / SMP / PRD), Drive, the sidechain input.

#include "devices/NativeDevice.h"
#include "devices/native/Biquad.h"
#include "dsp/Dsp.h"
#include "dsp/ModLfo.h"

#include <array>

namespace sonora::devices
{

class AutoFilter final : public NativeDevice
{
public:
    enum Param
    {
        FilterType,   // 0 LP, 1 HP, 2 BP, 3 Notch, 4 Morph
        Slope,        // 0 = 12 dB, 1 = 24 dB
        Cutoff,       // pitch 0..135
        Resonance,    // 0..1.25
        Morph,        // 0..1 (type Morph)
        LfoAmount,    // 0..1
        LfoShape,     // 0 sine, 1 triangle, 2 saw down, 3 saw up, 4 square, 5 random
        LfoFrequency, // Hz
        LfoRateType,  // 0 Hz, 1 tempo synced
        LfoBeatRate,  // index into kBeatRates
        LfoPhase,     // degrees
        LfoOn,
        NumParams
    };
    static float pitchToHz(float pitch) { return 440.0f * std::pow(2.0f, (pitch - 69.0f) / 12.0f); }
    // Beats per LFO cycle for LfoBeatRate index (16 bars .. 1/32, as in Live's list order).
    static double beatsPerCycle(int index);

    AutoFilter();
    std::string name() const override { return "Auto Filter"; }
    void prepare(double sampleRate, int maxBlockSize) override;
    void process(juce::AudioBuffer<float>& audio, juce::MidiBuffer& midi) override;
    void reset() override;
    void setHostInfo(const HostInfo& info) override
    {
        tempo_.store((float) info.tempo, std::memory_order_relaxed);
        beats_.store(info.beats, std::memory_order_relaxed);
        playing_.store(info.playing, std::memory_order_relaxed);
    }

private:
    double sampleRate_ = 44100.0;
    std::atomic<float> tempo_{120.0f};
    std::atomic<double> beats_{0.0};
    std::atomic<bool> playing_{false};
    dsp::Svf filter_[2][2]; // [channel][stage]
    dsp::Lfo lfo_[2];
    int updateCounter_ = 0;
};

// Auto Filter 2 (.als "AutoFilter2", Live 12.4's parameter set) — DEV-22b. Measured against the reference:
//   Clean filter = Simpler's measured Clean law: RBJ stages, damping d = (1 - r) / (1 + 3 r) (Q = 1 / 2d, r = 1 gives
//   Q ~35), 24 dB adds a stage with damping (1 + d) / 2; LP / HP / BP (stage peak gain Q) / Notch; Morph 0.5 = BP.
//   Output = linear gain, Dry/Wet linear. LFO and envelope depth: 7.6 octaves per unit (sine LFO starts at phase 0).
// Approximations (not yet measured exactly): DJ control mapping (HP/LP corner 20 Hz * 2^(9.2 |c|)), Comb (normalised
// feedback comb, delay 1 / (2 f), feedback 0.5 (1 + Res)), Vowel (band-pass), synced LFO rates, circuits other than
// Clean (Clean is used; Drive = 26 dB x Drive of input gain, measured), envelope detector shape.
class AutoFilter2 final : public NativeDevice
{
public:
    enum Param
    {
        Frequency, Resonance, Morph, FilterType, Slope, Circuit, Drive, DjControl,
        LfoAmount, LfoWaveform, LfoTimeMode, LfoFrequency, LfoTime, LfoSyncedRate,
        EnvAmount, EnvAttack, EnvRelease, Output, SoftClip, DryWet,
        NumParams
    };
    static constexpr float kOctavesPerUnit = 7.6f;
    AutoFilter2();
    std::string name() const override { return "Auto Filter"; }
    void prepare(double sampleRate, int maxBlockSize) override;
    void process(juce::AudioBuffer<float>& audio, juce::MidiBuffer& midi) override;
    void reset() override;
    void setHostInfo(const HostInfo& info) override
    {
        tempo_.store((float) info.tempo, std::memory_order_relaxed);
        beats_.store(info.beats, std::memory_order_relaxed);
        playing_.store(info.playing, std::memory_order_relaxed);
    }
    // Damping of the resonant stage for resonance r (measured law, shared with Simpler's Clean circuit).
    static float damping(float r) { return std::max((1.0f - r) / (1.0f + 3.0f * r), 0.0144f); }

private:
    void setupFilters(float hz, int type);
    double sampleRate_ = 44100.0;
    std::atomic<float> tempo_{120.0f};
    std::atomic<double> beats_{0.0};
    std::atomic<bool> playing_{false};
    // RBJ stages run in state-variable form: the cutoff moves every 16 frames (LFO, envelope, automation) — DEV-24.
    std::array<SvfCoeffs, 2> lp_{}, hp_{}, bp_{}, notch_{};
    std::array<std::array<std::array<SvfState, 2>, 4>, 2> state_{}; // [channel][LP HP BP Notch][stage]
    dsp::FractionalDelay comb_[2];
    float combOut_[2] = {0, 0};
    dsp::ModLfo lfo_;
    float env_ = 0.0f, lfoValue_ = 0.0f;
    int updateCounter_ = 0;
};

} // namespace sonora::devices
