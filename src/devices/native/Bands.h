#pragma once
// Three-band devices (ROADMAP DEV-11 / DEV-10):
//   EqThree            (.als "FilterEQ3")          3-band DJ-style EQ with band kills, 24 / 48 dB slopes
//   MultibandDynamics  (.als "MultibandDynamics")  3-band up/down compressor/expander
// MultibandDynamics uses phase-aligned Linkwitz-Riley crossovers (flat sum). EqThree follows the
// reference's measured Butterworth tree instead (DEV-22b), which is not flat at
// neutral settings. Not modelled: sidechain input, Live's exact multiband detector and knee shapes,
// the MultibandDynamics band solo/"time" display.

#include "devices/NativeDevice.h"
#include "devices/native/Biquad.h"
#include "dsp/Dsp.h"

#include <array>

namespace sonora::devices
{

// Splits one channel into low / mid / high. `steep` uses LR8 crossovers (48 dB/oct).
class ThreeBandSplit
{
public:
    void setup(double sampleRate, float lowMid, float midHigh, bool steep)
    {
        lo_.setup(sampleRate, lowMid, steep);
        hi_.setup(sampleRate, midHigh, steep);
        ap_.setup(sampleRate, midHigh, steep);
    }
    void reset()
    {
        lo_.reset();
        hi_.reset();
        ap_.reset();
    }
    // Writes the three band signals. The low band also passes the upper crossover (low + high leg
    // = all-pass) so that the bands sum flat when nothing is changed.
    void process(float x, float& low, float& mid, float& high)
    {
        float lowLeg, highLeg;
        lo_.process(x, lowLeg, highLeg);
        hi_.process(highLeg, mid, high);
        float apLow, apHigh;
        ap_.process(lowLeg, apLow, apHigh);
        low = apLow + apHigh;
    }

private:
    dsp::Crossover lo_, hi_, ap_;
};

// EQ Three's band split, measured against the reference (null < -57 dB on its factory presets):
// low = LP(lo); rest = HP(lo); mid = LP(hi)(rest); high = HP(hi)(rest). Every filter is a digital
// Butterworth (bilinear, pre-warped) of order 4 ("24 dB") or 8 ("48 dB"), as a cascade of RBJ biquads.
// The responses are -3 dB at each crossover, so neutral gains give a +3 dB bump at both frequencies.
class EqThreeSplit
{
public:
    static constexpr int kMaxStages = 4;
    void setup(double sampleRate, double lowFreq, double highFreq, bool steep);
    void reset();
    void process(float x, float& low, float& mid, float& high);
    // Linear magnitude of gl * low + gm * mid + gh * high at `freq` (tests, displays).
    double response(double freq, double sampleRate, float gl, float gm, float gh) const;

private:
    struct Chain
    {
        BiquadCoeffs c[kMaxStages]; // response(); the audio runs the state-variable form (crossover moves: DEV-24)
        SvfCoeffs v[kMaxStages];
        SvfState s[kMaxStages];
    };
    static float run(Chain& chain, int stages, float x);
    int stages_ = 2;
    Chain lowLp_, lowHp_, highLp_, highHp_;
};

class EqThree final : public NativeDevice
{
public:
    enum Param
    {
        GainLow,  // linear, 1 == 0 dB, max 2 == +6 dB; the minimum (-70 dB) is a kill, as in the reference
        GainMid,
        GainHigh,
        FreqLow,  // Hz
        FreqHigh, // Hz
        LowOn,    // false == band killed
        MidOn,
        HighOn,
        Slope,    // 0 = 24 dB/oct, 1 = 48 dB/oct
        NumParams
    };
    EqThree();
    std::string name() const override { return "EQ Three"; }
    void prepare(double sampleRate, int maxBlockSize) override;
    void process(juce::AudioBuffer<float>& audio, juce::MidiBuffer& midi) override;
    void reset() override;

private:
    double sampleRate_ = 44100.0;
    std::array<EqThreeSplit, 2> split_;
    float lastLo_ = -1, lastHi_ = -1, lastSlope_ = -1;
};

class MultibandDynamics final : public NativeDevice
{
public:
    enum Band { Low, Mid, High };
    // Per-band parameter block, repeated for Low / Mid / High.
    enum BandParam
    {
        BandActive,
        BandInputGain,   // dB
        BandGain,        // dB
        AboveThreshold,  // dB
        BelowThreshold,  // dB
        AboveRatio,      // -1..: slope above = 1 + v (negative compresses, positive expands)
        BelowRatio,      // -3..1: gain below = v x (below - level) dB (positive = upward compression, negative = expansion)
        BandAttack,      // ms
        BandRelease,     // ms
        PerBand
    };
    enum Global
    {
        SplitLowMid = PerBand * 3,
        SplitMidHigh,
        SoftKnee,
        EnvelopeIsPeak,
        OutputGain, // dB
        GlobalAmount,
        GlobalTime,
        NumParams
    };
    static int bandParam(int band, BandParam p) { return band * PerBand + p; }

    MultibandDynamics();
    std::string name() const override { return "Multiband Dynamics"; }
    void prepare(double sampleRate, int maxBlockSize) override;
    void process(juce::AudioBuffer<float>& audio, juce::MidiBuffer& midi) override;
    void reset() override;
    // Gain change of a band in dB at its current settings for a steady level (for tests / meters).
    static float bandGainDb(float levelDb, float below, float above, float vBelow, float vAbove, float knee);
    float bandMeterDb(int band) const { return meters_[(size_t) band].load(std::memory_order_relaxed); }

private:
    struct BandState
    {
        float power = 0.0f; // RMS detector: smoothed mean square
        float env = 0.0f;   // detector envelope (linear), attack / release applied
    };
    double sampleRate_ = 44100.0;
    std::array<ThreeBandSplit, 2> split_;
    std::array<std::array<BandState, 3>, 1> state_{};
    std::array<std::atomic<float>, 3> meters_{};
    float lastLowMid_ = -1, lastMidHigh_ = -1;
};

} // namespace sonora::devices
