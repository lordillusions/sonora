#include "devices/native/Bands.h"

#include <complex>

namespace sonora::devices
{

// ----------------------------------------------------------------------------- EQ Three

void EqThreeSplit::setup(double sampleRate, double lowFreq, double highFreq, bool steep)
{
    stages_ = steep ? 4 : 2;
    const int order = 2 * stages_;
    for (int k = 0; k < stages_; ++k)
    {
        // Butterworth pole pair k: Q = 1 / (2 cos((2k + 1) pi / 2N)).
        const double q = 1.0 / (2.0 * std::cos((2.0 * k + 1.0) * M_PI / (2.0 * order)));
        lowLp_.c[k] = BiquadCoeffs::make(BiquadCoeffs::Type::LowPass, sampleRate, lowFreq, q, 0.0);
        lowHp_.c[k] = BiquadCoeffs::make(BiquadCoeffs::Type::HighPass, sampleRate, lowFreq, q, 0.0);
        highLp_.c[k] = BiquadCoeffs::make(BiquadCoeffs::Type::LowPass, sampleRate, highFreq, q, 0.0);
        highHp_.c[k] = BiquadCoeffs::make(BiquadCoeffs::Type::HighPass, sampleRate, highFreq, q, 0.0);
        for (auto* chain : {&lowLp_, &lowHp_, &highLp_, &highHp_})
            chain->v[k] = SvfCoeffs::fromBiquad(chain->c[k]);
    }
}

void EqThreeSplit::reset()
{
    for (auto* chain : {&lowLp_, &lowHp_, &highLp_, &highHp_})
        for (auto& state : chain->s)
            state.reset();
}

float EqThreeSplit::run(Chain& chain, int stages, float x)
{
    for (int k = 0; k < stages; ++k)
        x = chain.s[k].process(chain.v[k], x);
    return x;
}

void EqThreeSplit::process(float x, float& low, float& mid, float& high)
{
    low = run(lowLp_, stages_, x);
    const float rest = run(lowHp_, stages_, x);
    mid = run(highLp_, stages_, rest);
    high = run(highHp_, stages_, rest);
}

double EqThreeSplit::response(double freq, double sampleRate, float gl, float gm, float gh) const
{
    using C = std::complex<double>;
    const C z1 = std::polar(1.0, -2.0 * M_PI * freq / sampleRate), z2 = z1 * z1;
    const auto chain = [&](const Chain& ch) {
        C h(1.0, 0.0);
        for (int k = 0; k < stages_; ++k)
        {
            const auto& c = ch.c[k];
            h *= (c.b0 + c.b1 * z1 + c.b2 * z2) / (1.0 + c.a1 * z1 + c.a2 * z2);
        }
        return h;
    };
    const C rest = chain(lowHp_);
    return std::abs((double) gl * chain(lowLp_) + (double) gm * rest * chain(highLp_) + (double) gh * rest * chain(highHp_));
}

EqThree::EqThree() : NativeDevice([] {
    using E = EqThree;
    std::vector<ParameterInfo> p(E::NumParams);
    p[E::GainLow] = makeParam("GainLo", "Low", 0.0f, 2.0f, 1.0f, "lin");
    p[E::GainMid] = makeParam("GainMid", "Mid", 0.0f, 2.0f, 1.0f, "lin");
    p[E::GainHigh] = makeParam("GainHi", "High", 0.0f, 2.0f, 1.0f, "lin");
    p[E::FreqLow] = makeParam("FreqLo", "FreqLo", 50.0f, 500.0f, 250.0f, "Hz");
    p[E::FreqHigh] = makeParam("FreqHi", "FreqHi", 500.0f, 18000.0f, 2500.0f, "Hz");
    p[E::LowOn] = makeParam("LowOn", "Low On", 0.0f, 1.0f, 1.0f, "", true);
    p[E::MidOn] = makeParam("MidOn", "Mid On", 0.0f, 1.0f, 1.0f, "", true);
    p[E::HighOn] = makeParam("HighOn", "High On", 0.0f, 1.0f, 1.0f, "", true);
    p[E::Slope] = makeParam("Slope", "48 dB", 0.0f, 1.0f, 0.0f, "", true);
    return p;
}())
{
}

void EqThree::prepare(double sampleRate, int)
{
    sampleRate_ = sampleRate;
    lastLo_ = lastHi_ = lastSlope_ = -1;
    reset();
}

void EqThree::reset()
{
    for (auto& s : split_)
    {
        s.reset();
    }
}

void EqThree::process(juce::AudioBuffer<float>& audio, juce::MidiBuffer&)
{
    if (isBypassed())
        return;
    const float lo = value(FreqLow), hi = value(FreqHigh), slope = value(Slope);
    if (std::fabs(lo - lastLo_) > 1e-3f || std::fabs(hi - lastHi_) > 1e-3f || std::fabs(slope - lastSlope_) > 1e-3f)
    {
        lastLo_ = lo;
        lastHi_ = hi;
        lastSlope_ = slope;
        for (auto& s : split_)
        {
            s.setup(sampleRate_, lo, hi, slope > 0.5f);
        }
    }
    // The gain knobs' minimum (-70 dB, stored as 0.000316) is "-inf" in the reference: a kill, like the band switch.
    const auto bandGain = [this](int gain, int on) { return value(on) > 0.5f && value(gain) > 0.000317f ? value(gain) : 0.0f; };
    const float gl = bandGain(GainLow, LowOn), gm = bandGain(GainMid, MidOn), gh = bandGain(GainHigh, HighOn);
    const int n = audio.getNumSamples();
    for (int ch = 0; ch < 2; ++ch)
    {
        float* d = audio.getWritePointer(ch);
        auto& split = split_[(size_t) ch];
        for (int i = 0; i < n; ++i)
        {
            float l, m, h;
            split.process(d[i], l, m, h);
            d[i] = l * gl + m * gm + h * gh;
        }
    }
}

// ----------------------------------------------------------------------------- Multiband

namespace
{
const char* kBandSuffix[3] = {"Low", "Mid", "High"};

float hinge(float d, float knee)
{
    if (knee <= 0.0f)
        return std::max(d, 0.0f);
    if (d <= -0.5f * knee)
        return 0.0f;
    if (d >= 0.5f * knee)
        return d;
    const float x = d + 0.5f * knee;
    return x * x / (2.0f * knee);
}
} // namespace

float MultibandDynamics::bandGainDb(float levelDb, float below, float above, float vBelow, float vAbove, float knee)
{
    const float gAbove = vAbove * hinge(levelDb - above, knee);       // slope above = 1 + vAbove
    // Measured (DEV-22b): positive Below ratios raise quiet signals (1 = infinite upward compression, "Flatline"),
    // negative ones expand downwards (-3 = 1:4); the stored range is -3 .. 1.
    // The below-threshold action fades out between -54 and -58 dB (measured at a -50 dB threshold: 86 % at -54.3,
    // 7 % at -57.5, none at -60), so silence and noise are never boosted. Absolute vs threshold-relative: unknown.
    const float floorWeight = std::clamp((levelDb + 58.0f) / 4.0f, 0.0f, 1.0f);
    const float gBelow = vBelow * hinge(below - levelDb, knee) * floorWeight;
    return gAbove + gBelow;
}

MultibandDynamics::MultibandDynamics() : NativeDevice([] {
    using M = MultibandDynamics;
    std::vector<ParameterInfo> p(M::NumParams);
    for (int b = 0; b < 3; ++b)
    {
        const std::string s = kBandSuffix[b];
        p[(size_t) M::bandParam(b, M::BandActive)] = makeParam("Active" + s, s + " On", 0.0f, 1.0f, 1.0f, "", true);
        p[(size_t) M::bandParam(b, M::BandInputGain)] = makeParam("InputGain" + s, s + " In", -30.0f, 30.0f, 0.0f, "dB");
        p[(size_t) M::bandParam(b, M::BandGain)] = makeParam("Gain" + s, s + " Out", -30.0f, 30.0f, 0.0f, "dB");
        p[(size_t) M::bandParam(b, M::AboveThreshold)] = makeParam("AboveThreshold" + s, s + " Above", -80.0f, 0.0f, -20.0f, "dB");
        p[(size_t) M::bandParam(b, M::BelowThreshold)] = makeParam("BelowThreshold" + s, s + " Below", -80.0f, 0.0f, -60.0f, "dB");
        p[(size_t) M::bandParam(b, M::AboveRatio)] = makeParam("AboveRatio" + s, s + " Ratio+", -1.0f, 1.0f, 0.0f);
        p[(size_t) M::bandParam(b, M::BelowRatio)] = makeParam("BelowRatio" + s, s + " Ratio-", -3.0f, 1.0f, 0.0f);
        p[(size_t) M::bandParam(b, M::BandAttack)] = makeParam("Attack" + s, s + " Att", 0.1f, 500.0f, 10.0f, "ms");
        p[(size_t) M::bandParam(b, M::BandRelease)] = makeParam("Release" + s, s + " Rel", 1.0f, 3000.0f, 100.0f, "ms");
    }
    p[M::SplitLowMid] = makeParam("SplitLowMid", "Low/Mid", 50.0f, 18000.0f, 120.0f, "Hz");
    p[M::SplitMidHigh] = makeParam("SplitMidHigh", "Mid/High", 50.0f, 18000.0f, 2500.0f, "Hz");
    p[M::SoftKnee] = makeParam("SoftKnee", "Soft Knee", 0.0f, 1.0f, 1.0f, "", true);
    p[M::EnvelopeIsPeak] = makeParam("EnvelopeIsPeak", "Peak", 0.0f, 1.0f, 0.0f, "", true);
    p[M::OutputGain] = makeParam("OutputGain", "Output", -30.0f, 30.0f, 0.0f, "dB");
    p[M::GlobalAmount] = makeParam("GlobalAmount", "Amount", 0.0f, 2.0f, 1.0f, "%");
    p[M::GlobalTime] = makeParam("GlobalTime", "Time", 0.1f, 4.0f, 1.0f);
    return p;
}())
{
}

void MultibandDynamics::prepare(double sampleRate, int)
{
    sampleRate_ = sampleRate;
    lastLowMid_ = lastMidHigh_ = -1;
    reset();
}

void MultibandDynamics::reset()
{
    for (auto& s : split_)
    {
        s.reset();
    }
    for (auto& st : state_[0])
        st = BandState{};
}

void MultibandDynamics::process(juce::AudioBuffer<float>& audio, juce::MidiBuffer&)
{
    if (isBypassed())
        return;
    const float lm = value(SplitLowMid), mh = std::max(value(SplitMidHigh), value(SplitLowMid) * 1.05f);
    if (std::fabs(lm - lastLowMid_) > 1e-3f || std::fabs(mh - lastMidHigh_) > 1e-3f)
    {
        lastLowMid_ = lm;
        lastMidHigh_ = mh;
        for (auto& s : split_)
        {
            s.setup(sampleRate_, lm, mh, false);
        }
    }

    const float knee = value(SoftKnee) > 0.5f ? 6.0f : 0.0f;
    const bool peak = value(EnvelopeIsPeak) > 0.5f;
    const float amount = value(GlobalAmount);
    const float timeScale = value(GlobalTime);
    const float outGain = dsp::dbToGain(value(OutputGain));
    const float rmsCoef = dsp::timeToCoef(1.0f, sampleRate_); // fitted on the reference's timing cases and presets

    struct Cfg
    {
        bool active;
        float in, out, above, below, vAbove, vBelow, atk, rel;
    } cfg[3];
    for (int b = 0; b < 3; ++b)
    {
        cfg[b] = {value(bandParam(b, BandActive)) > 0.5f,
                  dsp::dbToGain(value(bandParam(b, BandInputGain))),
                  dsp::dbToGain(value(bandParam(b, BandGain))),
                  value(bandParam(b, AboveThreshold)),
                  value(bandParam(b, BelowThreshold)),
                  value(bandParam(b, AboveRatio)),
                  value(bandParam(b, BelowRatio)),
                  dsp::timeToCoef(value(bandParam(b, BandAttack)) * timeScale, sampleRate_),
                  dsp::timeToCoef(value(bandParam(b, BandRelease)) * timeScale, sampleRate_)};
    }

    const int n = audio.getNumSamples();
    float* d[2] = {audio.getWritePointer(0), audio.getWritePointer(1)};
    std::array<float, 3> minGain{0.0f, 0.0f, 0.0f};
    for (int i = 0; i < n; ++i)
    {
        float bands[2][3];
        for (int ch = 0; ch < 2; ++ch)
            split_[(size_t) ch].process(d[ch][i], bands[ch][0], bands[ch][1], bands[ch][2]);

        float outL = 0.0f, outR = 0.0f;
        for (int b = 0; b < 3; ++b)
        {
            const auto& c = cfg[b];
            float l = bands[0][b] * c.in, r = bands[1][b] * c.in;
            if (c.active)
            {
                // detector: linked stereo level, attack/release on the envelope
                auto& st = state_[0][(size_t) b];
                float target;
                if (peak)
                {
                    target = std::max(std::fabs(l), std::fabs(r));
                }
                else
                {
                    st.power += (0.5f * (l * l + r * r) - st.power) * rmsCoef; // 20 ms mean square
                    // Sine-calibrated RMS (measured: the reference's RMS mode reads a sine at its peak, DEV-22b).
                    target = std::sqrt(std::max(2.0f * st.power, 0.0f));
                }
                st.env += (target - st.env) * (target > st.env ? c.atk : c.rel);
                const float gainDb = bandGainDb(dsp::gainToDb(st.env), c.below, c.above, c.vBelow, c.vAbove, knee) * amount;
                minGain[(size_t) b] = std::min(minGain[(size_t) b], gainDb);
                const float g = dsp::dbToGain(gainDb);
                l *= g;
                r *= g;
            }
            outL += l * c.out;
            outR += r * c.out;
        }
        d[0][i] = outL * outGain;
        d[1][i] = outR * outGain;
    }
    for (size_t b = 0; b < 3; ++b)
        meters_[b].store(minGain[b], std::memory_order_relaxed);
}

} // namespace sonora::devices
