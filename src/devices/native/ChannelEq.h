#pragma once
// DEV-30: Channel EQ (.als "ChannelEq"), the reference's console-style three-band EQ.
//
// Signal path (per channel): 80 Hz high-pass (switch) -> low shelf -> sweepable mid peak -> high shelf (a low-pass
// joins it while High cuts) -> Output. Laws, from the reference manual and fitted to the response curve the reference
// draws for the "Ac Piano Upright" chain (HP on, Low -2 dB, High +2 dB; a 2000 x 231 capture, fit rms 0.06 dB):
//   - HP 80 Hz: first order (6 dB/oct), corner 80 Hz (the free fit lands on 80.2 Hz).
//   - Low: second-order shelf, +-15 dB (stored 0.18..5.6 linear), midpoint 178 Hz, Q 0.63.
//   - Mid: peak, +-12 dB (0.25..4), 120 Hz..7.5 kHz. Its bandwidth is not in the capture: Q 0.71 is assumed.
//   - High: second-order shelf, +-15 dB, midpoint 2.65 kHz, Q 0.62. Cutting also lowers a 12 dB/oct low-pass from
//     20 kHz (0 dB) to 8 kHz (-15 dB), logarithmically with the cut (manual); the low-pass order is assumed.
//   - Output: +-12 dB (0.25..4).
// Not modelled (no reference measurement yet, see docs/PARITY.md "Channel EQ"): the manual's "adaptive" low shelf
// (its curve changes with the gain; Sonora keeps the -2 dB shape at every gain) and the exact mid Q.
// The filters run in the state-variable form so automation and macros can move them while audio plays; every
// setting is smoothed on a fixed 16-sample grid (block-size invariant). No allocation or locks in process().

#include "devices/NativeDevice.h"
#include "devices/native/Biquad.h"
#include "dsp/AudioDisplayTap.h"

#include <array>

namespace sonora::devices
{

class ChannelEq final : public NativeDevice
{
public:
    enum Param
    {
        HighpassOn,    // 80 Hz high-pass switch
        LowShelfGain,  // linear, 0.18..5.6 (-15..+15 dB)
        MidGain,       // linear, 0.25..4 (-12..+12 dB)
        MidFrequency,  // Hz, 120..7500
        HighShelfGain, // linear, 0.18..5.6
        Gain,          // Output, linear, 0.25..4
        NumParams
    };
    static constexpr double kHighpassHz = 80.0;
    static constexpr double kLowShelfHz = 178.0, kLowShelfQ = 0.63;
    static constexpr double kHighShelfHz = 2655.0, kHighShelfQ = 0.62;
    static constexpr double kMidQ = 0.71;
    static constexpr double kLowpassOpenHz = 20000.0, kLowpassClosedHz = 8000.0;

    // A complete setting in display units (dB, Hz): what the filters are designed from.
    struct Settings
    {
        bool highpass = false;
        double lowDb = 0, midDb = 0, midHz = 1500, highDb = 0, outputDb = 0;
    };
    // The low-pass corner for a High setting, or 0 when High does not cut.
    static double lowpassHz(double highDb);
    // Magnitude of the whole device at `hz` (linear), for the face's curve and tests.
    static double response(const Settings&, double hz, double sampleRate);

    ChannelEq();
    std::string name() const override { return "Channel EQ"; }
    void prepare(double sampleRate, int maxBlockSize) override;
    void process(juce::AudioBuffer<float>& audio, juce::MidiBuffer& midi) override;
    void reset() override;

    Settings settings() const; // current parameter values (message or audio thread)
    double sampleRate() const { return sampleRate_; }
    dsp::AudioDisplayTap& displayTap() { return displayTap_; }

private:
    static constexpr int kStep = 16; // samples between coefficient updates
    enum Stage { Low, Mid, High, Lowpass, NumStages };
    struct Coeffs
    {
        std::array<SvfCoeffs, NumStages> svf;
        BiquadCoeffs highpass;
    };
    static Coeffs design(const Settings&, double sampleRate);
    void retarget(bool snap);

    double sampleRate_ = 48000.0;
    // Smoothed controls (dB, log2 Hz, switch mixes), moved once per step towards the parameters.
    std::array<double, 7> current_{}, target_{};
    Coeffs coeffs_;
    std::array<std::array<SvfState, NumStages>, 2> state_{};
    std::array<BiquadState, 2> highpassState_{};
    int phase_ = 0; // samples into the current step
    double smoothing_ = 0.1;
    dsp::AudioDisplayTap displayTap_;
};

} // namespace sonora::devices
