#pragma once
// DEV-30: Channel EQ (.als "ChannelEq"), the reference's console-style three-band EQ.
//
// Signal path (per channel): 80 Hz high-pass (switch) -> low shelf -> sweepable mid peak -> high shelf -> the high cut's
// low-pass -> a fixed top-end droop -> Output. DEV-30b measured every law on the reference's own audio (suite
// `channeleq_laws.txt`, white noise, Welch transfer functions against the source stem of the same export, 44.1 kHz;
// docs/PARITY.md "Channel EQ"). All 24 cases fit within 0.25 dB from 20 Hz to 20 kHz:
//   - HP 80 Hz: first order (bilinear, corner 80 Hz).
//   - Low: RBJ shelf, Q 0.6063, +-15 dB; "adaptive": the midpoint moves with the linear gain G, f = 204.32 - 18.446 G Hz
//     (101 Hz at +15 dB, 178 Hz at +3, 191 Hz at -3, 201 Hz at -15).
//   - Mid: peak with a prescribed Nyquist gain (Orfanidis 1997) so it does not cramp near Nyquist: analog Q 0.5
//     (bandwidth between the half-dB points = the bilinear-warped Q 0.5 edges x (1 + 1.446 (f/fs)^2)), Nyquist gain =
//     0.2236 x (dB of the analog Q 0.5 peak at fs/2). Cuts are the exact inverse of the boost.
//   - High: RBJ shelf, 2902 Hz, Q 0.6043, +-15 dB. A cut adds a second-order low-pass whose zeros sit at fixed points of
//     the z plane (-0.4816, -0.8749) and whose poles (bilinear analog equivalent) are measured at -3 / -9 / -14.9 dB
//     (32.8 kHz Q 0.587, 14.8 kHz Q 0.645, 10.1 kHz Q 0.652) and interpolated in dB (log frequency, linear Q) from the
//     transparent setting at 0 dB (poles on the zeros).
//   - Every setting (even all flat) has a fixed droop near Nyquist: -0.06 dB at 16 kHz, -0.24 at 19 kHz, -0.47 at 20 kHz.
//     It is a fixed z-plane biquad (the same fraction of Nyquist at every sample rate).
//   - Output: +-12 dB (0.25..4).
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
    static constexpr double kLowShelfQ = 0.6063, kLowShelfHzAtZeroGain = 204.32, kLowShelfHzPerGain = 18.446;
    static constexpr double kHighShelfHz = 2902.4, kHighShelfQ = 0.6043;
    static constexpr double kMidQ = 0.5, kMidBandwidthWarp = 1.446, kMidNyquistFraction = 0.2236;

    // A complete setting in display units (dB, Hz): what the filters are designed from.
    struct Settings
    {
        bool highpass = false;
        double lowDb = 0, midDb = 0, midHz = 1500, highDb = 0, outputDb = 0;
    };
    // The low shelf's midpoint for a Low setting (the shelf adapts to its gain).
    static double lowShelfHz(double lowDb);
    // The cut low-pass's poles as a bilinear analog equivalent (Hz, Q), or {0, 0} when High does not cut.
    struct LowpassPoles { double hz = 0, q = 0; };
    static LowpassPoles lowpassPoles(double highDb, double sampleRate);
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
    enum Stage { Low, Mid, High, Lowpass, Droop, NumStages };
    struct Biquads
    {
        std::array<BiquadCoeffs, NumStages> stages;
        BiquadCoeffs highpass;
    };
    static Biquads designBiquads(const Settings&, double sampleRate);
    struct Coeffs
    {
        std::array<SvfCoeffs, NumStages> svf;
        BiquadCoeffs highpass;
    };
    static Coeffs design(const Settings&, double sampleRate);
    Settings smoothedSettings() const;
    void retarget(bool snap);

    double sampleRate_ = 48000.0;
    // Smoothed controls (dB, log2 Hz, the high-pass switch mix), moved once per step towards the parameters.
    std::array<double, 6> current_{}, target_{};
    Coeffs coeffs_;
    std::array<std::array<SvfState, NumStages>, 2> state_{};
    std::array<BiquadState, 2> highpassState_{};
    int phase_ = 0; // samples into the current step
    double smoothing_ = 0.1;
    dsp::AudioDisplayTap displayTap_;
};

} // namespace sonora::devices
