#include "devices/native/EqEight.h"

namespace sonora::devices
{

namespace
{
const std::vector<std::string> kTypeNames{"Low Cut 48", "Low Cut 12", "Low Shelf", "Bell",
                                          "Notch",      "High Shelf", "High Cut 12", "High Cut 48"};

// Live's default band layout for a fresh EQ Eight.
constexpr float kDefaultFreq[EqEight::kBands]{30, 100, 250, 500, 1000, 2500, 5000, 18000};

// Butterworth Q values for an 8th-order (4 x biquad) response.
constexpr double kButterworthQ8[EqEight::kStages]{0.50979558, 0.60134489, 0.89997622, 2.56291545};

void addBands(std::vector<ParameterInfo>& p, const char* set)
{
    for (int b = 0; b < EqEight::kBands; ++b)
    {
        const std::string base = "Bands." + std::to_string(b) + "/Parameter" + set + "/";
        const std::string label = std::string(set) + std::to_string(b + 1) + " ";
        const float defType = b == 0 ? 1.0f : b == EqEight::kBands - 1 ? 6.0f : 3.0f;
        p.push_back(makeParam(base + "IsOn", label + "On", 0, 1, b == 0 || b == 7 ? 0.0f : 1.0f, "", true));
        p.push_back(makeParam(base + "Mode", label + "Type", 0, 7, defType, "", false, kTypeNames));
        p.push_back(makeParam(base + "Freq", label + "Freq", 10, 22000, kDefaultFreq[b], "Hz"));
        p.push_back(makeParam(base + "Gain", label + "Gain", -15, 15, 0, "dB"));
        p.push_back(makeParam(base + "Q", label + "Q", 0.1f, 18, 0.71f, ""));
    }
}

std::vector<ParameterInfo> makeInfos()
{
    std::vector<ParameterInfo> p;
    addBands(p, "A");
    p.push_back(makeParam("GlobalGain", "Gain", -12, 12, 0, "dB"));
    p.push_back(makeParam("Scale", "Scale", -1.5f, 1.5f, 1, "%")); // -150 % .. 150 %
    p.push_back(makeParam("Mode", "Mode", 0, 2, 0, "", false, {"Stereo", "Left/Right", "Mid/Side"}));
    p.push_back(makeParam("AdaptiveQ", "Adaptive Q", 0, 1, 0, "", true));
    p.push_back(makeParam("Precision", "Precision", 0, 3, 0));
    p.push_back(makeParam("AdaptiveQFactor", "Adaptive Q Amount", 1.0f, 2.0f, 1.12f));
    addBands(p, "B");
    return p;
}
} // namespace

EqEight::EqEight() : NativeDevice(makeInfos()) {}

void EqEight::prepare(double sampleRate, int)
{
    sampleRate_ = sampleRate;
    outGain_.reset(sampleRate, 0.02);
    reset();
    parametersChanged.store(true);
}

void EqEight::reset()
{
    for (auto* set : {&bands_, &bandsB_})
        for (auto& b : *set)
            for (auto& ch : b.state)
                for (auto& s : ch)
                    s.reset();
}

void EqEight::design(Band& band, FilterType type, double sr, double freq, double q, double gainDb)
{
    using T = BiquadCoeffs::Type;
    switch (type)
    {
        case FilterType::LowCut12:
        case FilterType::HighCut12:
            band.coeffs[0] = BiquadCoeffs::make(type == FilterType::LowCut12 ? T::HighPass : T::LowPass, sr, freq, q, 0);
            band.activeStages = 1;
            break;
        case FilterType::LowCut48:
        case FilterType::HighCut48:
            // Approximation: Butterworth cascade, user Q scales the resonance of every stage.
            for (int s = 0; s < kStages; ++s)
                band.coeffs[(size_t) s] = BiquadCoeffs::make(type == FilterType::LowCut48 ? T::HighPass : T::LowPass, sr,
                                                             freq, kButterworthQ8[s] * (q / 0.7071), 0);
            band.activeStages = kStages;
            break;
        case FilterType::LowShelf: band.coeffs[0] = BiquadCoeffs::make(T::LowShelf, sr, freq, q, gainDb); band.activeStages = 1; break;
        case FilterType::Bell: band.coeffs[0] = BiquadCoeffs::make(T::Peak, sr, freq, q, gainDb); band.activeStages = 1; break;
        case FilterType::Notch: band.coeffs[0] = BiquadCoeffs::make(T::Notch, sr, freq, q, 0); band.activeStages = 1; break;
        case FilterType::HighShelf: band.coeffs[0] = BiquadCoeffs::make(T::HighShelf, sr, freq, q, gainDb); band.activeStages = 1; break;
    }
}

// Adaptive Q: the more a band boosts or cuts, the narrower it gets (a guess at Live's curve).
double EqEight::effectiveQ(bool setB, int band) const
{
    double q = value(bandParamSet(setB, band, Q));
    if (value(kAdaptiveQ) > 0.5f)
        q *= std::pow((double) value(kAdaptiveQFactor), std::fabs((double) value(bandParamSet(setB, band, GainDb))) / 3.0);
    return q;
}

void EqEight::designBand(Band& out, bool setB, int band) const
{
    if (value(bandParamSet(setB, band, On)) < 0.5f)
    {
        out.activeStages = 0;
        return;
    }
    design(out, (FilterType) (int) std::lround(value(bandParamSet(setB, band, Type))), sampleRate_,
           value(bandParamSet(setB, band, Freq)), effectiveQ(setB, band),
           value(bandParamSet(setB, band, GainDb)) * value(kScale));
}

void EqEight::updateCoefficients()
{
    for (int b = 0; b < kBands; ++b)
    {
        for (bool setB : {false, true})
        {
            auto& band = (setB ? bandsB_ : bands_)[(size_t) b];
            designBand(band, setB, b);
            for (int s = 0; s < band.activeStages; ++s)
                band.svf[(size_t) s] = SvfCoeffs::fromBiquad(band.coeffs[(size_t) s]);
        }
    }
    outGain_.setTargetValue(juce::Decibels::decibelsToGain(value(kGlobalGain)));
}

void EqEight::process(juce::AudioBuffer<float>& audio, juce::MidiBuffer&)
{
    if (isBypassed())
        return;
    if (parametersChanged.exchange(false, std::memory_order_acquire))
        updateCoefficients(); // allocation-free

    const int n = audio.getNumSamples();
    const int mode = (int) std::lround(value(kMode));
    float* l = audio.getWritePointer(0);
    float* r = audio.getWritePointer(1);

    if (mode == 2) // Mid/Side: encode, filter, decode
        for (int i = 0; i < n; ++i)
        {
            const float m = 0.5f * (l[i] + r[i]), sd = 0.5f * (l[i] - r[i]);
            l[i] = m;
            r[i] = sd;
        }

    // Channel 0 always uses set A; channel 1 uses set A in Stereo mode and set B otherwise.
    for (int ch = 0; ch < 2; ++ch)
    {
        float* d = ch == 0 ? l : r;
        auto& set = (ch == 1 && mode != 0) ? bandsB_ : bands_;
        for (auto& band : set)
            for (int s = 0; s < band.activeStages; ++s)
            {
                auto& st = band.state[(size_t) ch][(size_t) s];
                const auto& c = band.svf[(size_t) s];
                for (int i = 0; i < n; ++i)
                    d[i] = st.process(c, d[i]);
            }
    }

    if (mode == 2)
        for (int i = 0; i < n; ++i)
        {
            const float m = l[i], sd = r[i];
            l[i] = m + sd;
            r[i] = m - sd;
        }
    outGain_.applyGain(audio, n);
}

double EqEight::responseAt(double freq, bool setB) const
{
    // Designs throw-away filters from the current parameter values so the UI never reads
    // state owned by the audio thread.
    double mag = juce::Decibels::decibelsToGain((double) value(kGlobalGain));
    for (int b = 0; b < kBands; ++b)
    {
        Band band;
        designBand(band, setB, b);
        for (int s = 0; s < band.activeStages; ++s)
            mag *= band.coeffs[(size_t) s].magnitudeAt(freq, sampleRate_);
    }
    return mag;
}

} // namespace sonora::devices
