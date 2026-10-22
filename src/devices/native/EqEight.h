#pragma once
// EQ Eight (.als tag "Eq8").
//
// Band filter types, in the order of the <Mode> enum stored in .als:
//   0 LowCut48, 1 LowCut12, 2 LowShelf, 3 Bell, 4 Notch, 5 HighShelf, 6 HighCut12, 7 HighCut48
// Implemented: Stereo / Left-Right / Mid-Side modes (band set A processes the left or mid channel,
// set B the right or side channel), 8 bands, Global Gain, Scale, Adaptive Q.
// Not yet: oversampling ("Precision"), the spectrum analyzer (ROADMAP DEV-2b). Curves are RBJ
// approximations, not Live-identical; Adaptive Q is a guess at Live's curve.

#include "devices/NativeDevice.h"
#include "devices/native/Biquad.h"

#include <array>

namespace sonora::devices
{

class EqEight final : public NativeDevice
{
public:
    static constexpr int kBands = 8;
    static constexpr int kStages = 4; // 48 dB/oct = 4 cascaded biquads

    enum class FilterType
    {
        LowCut48,
        LowCut12,
        LowShelf,
        Bell,
        Notch,
        HighShelf,
        HighCut12,
        HighCut48
    };

    // Parameter index layout: 5 per band, then globals.
    enum BandParam
    {
        On,
        Type,
        Freq,
        GainDb,
        Q,
        PerBand
    };
    static int bandParam(int band, BandParam p) { return band * PerBand + p; }
    static constexpr int kGlobalGain = kBands * PerBand;
    static constexpr int kScale = kGlobalGain + 1;
    static constexpr int kMode = kScale + 1;       // 0 Stereo, 1 Left/Right, 2 Mid/Side
    static constexpr int kAdaptiveQ = kMode + 1;   // on/off
    static constexpr int kPrecision = kAdaptiveQ + 1; // stored, not used yet
    static constexpr int kAdaptiveQFactor = kPrecision + 1;
    static constexpr int kSetB = kAdaptiveQFactor + 1; // second band set (right / side channel)
    static int bandParamB(int band, BandParam p) { return kSetB + band * PerBand + p; }
    // Parameter index of a band in set A (false) or B (true).
    static int bandParamSet(bool setB, int band, BandParam p) { return setB ? bandParamB(band, p) : bandParam(band, p); }

    EqEight();

    std::string name() const override { return "EQ Eight"; }
    void prepare(double sampleRate, int maxBlockSize) override;
    void process(juce::AudioBuffer<float>& audio, juce::MidiBuffer& midi) override;
    void reset() override;

    // Combined magnitude (linear) of all enabled bands at `freq`. Message thread (UI curve).
    // `setB` selects the second band set (right / side channel).
    double responseAt(double freq, bool setB = false) const;

private:
    struct Band
    {
        std::array<BiquadCoeffs, kStages> coeffs; // response (UI curve); the audio runs the state-variable form below
        std::array<SvfCoeffs, kStages> svf;       // band moves (automation, knobs) must not click: DEV-24
        int activeStages = 0;
        std::array<std::array<SvfState, kStages>, 2> state; // [channel][stage]
    };

    static void design(Band& band, FilterType type, double sr, double freq, double q, double gainDb);
    double effectiveQ(bool setB, int band) const;
    void designBand(Band& out, bool setB, int band) const;
    void updateCoefficients();

    double sampleRate_ = 44100.0;
    std::array<Band, kBands> bands_{};  // set A
    std::array<Band, kBands> bandsB_{}; // set B
    juce::SmoothedValue<float> outGain_;
};

} // namespace sonora::devices
