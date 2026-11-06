#include "devices/native/ChannelEq.h"

#include "dsp/Dsp.h"

#include <algorithm>
#include <cmath>
#include <complex>

namespace sonora::devices
{

namespace
{
enum Smoothed { LowDb, MidDb, MidOctave, HighDb, OutputDb, HighpassMix, LowpassMix };

// First-order bilinear high-pass (pre-warped at the corner), as a biquad with b2 = a2 = 0.
BiquadCoeffs firstOrderHighpass(double sampleRate, double hz)
{
    const double k = std::tan(M_PI * std::min(hz, sampleRate * 0.49) / sampleRate);
    BiquadCoeffs c;
    c.b0 = 1.0 / (1.0 + k);
    c.b1 = -c.b0;
    c.b2 = 0.0;
    c.a1 = (k - 1.0) / (k + 1.0);
    c.a2 = 0.0;
    return c;
}

std::complex<double> at(const BiquadCoeffs& c, std::complex<double> z1)
{
    const auto z2 = z1 * z1;
    return (c.b0 + c.b1 * z1 + c.b2 * z2) / (1.0 + c.a1 * z1 + c.a2 * z2);
}

float gainDb(float linear) { return dsp::gainToDb(std::max(linear, 1e-6f)); }
} // namespace

double ChannelEq::lowpassHz(double highDb)
{
    if (highDb >= 0.0) return 0.0;
    const double depth = std::clamp(-highDb / 15.0, 0.0, 1.0);
    return kLowpassOpenHz * std::pow(kLowpassClosedHz / kLowpassOpenHz, depth);
}

ChannelEq::Coeffs ChannelEq::design(const Settings& s, double sampleRate)
{
    using T = BiquadCoeffs::Type;
    const double nyquistSafe = sampleRate * 0.49;
    Coeffs c;
    c.highpass = firstOrderHighpass(sampleRate, kHighpassHz);
    const BiquadCoeffs stages[NumStages] = {
        BiquadCoeffs::make(T::LowShelf, sampleRate, kLowShelfHz, kLowShelfQ, s.lowDb),
        BiquadCoeffs::make(T::Peak, sampleRate, s.midHz, kMidQ, s.midDb),
        BiquadCoeffs::make(T::HighShelf, sampleRate, std::min(kHighShelfHz, nyquistSafe), kHighShelfQ, s.highDb),
        BiquadCoeffs::make(T::LowPass, sampleRate, std::min(std::max(lowpassHz(s.highDb), kLowpassClosedHz), nyquistSafe),
                           M_SQRT1_2, 0.0),
    };
    for (int i = 0; i < NumStages; ++i)
        c.svf[(size_t) i] = SvfCoeffs::fromBiquad(stages[i]);
    return c;
}

double ChannelEq::response(const Settings& s, double hz, double sampleRate)
{
    using T = BiquadCoeffs::Type;
    const auto z1 = std::polar(1.0, -2.0 * M_PI * hz / sampleRate);
    const double nyquistSafe = sampleRate * 0.49;
    std::complex<double> h = std::pow(10.0, s.outputDb / 20.0);
    if (s.highpass) h *= at(firstOrderHighpass(sampleRate, kHighpassHz), z1);
    h *= at(BiquadCoeffs::make(T::LowShelf, sampleRate, kLowShelfHz, kLowShelfQ, s.lowDb), z1);
    h *= at(BiquadCoeffs::make(T::Peak, sampleRate, s.midHz, kMidQ, s.midDb), z1);
    h *= at(BiquadCoeffs::make(T::HighShelf, sampleRate, std::min(kHighShelfHz, nyquistSafe), kHighShelfQ, s.highDb), z1);
    if (const double lp = lowpassHz(s.highDb); lp > 0)
        h *= at(BiquadCoeffs::make(T::LowPass, sampleRate, std::min(lp, nyquistSafe), M_SQRT1_2, 0.0), z1);
    return std::abs(h);
}

ChannelEq::ChannelEq() : NativeDevice([] {
    using E = ChannelEq;
    std::vector<ParameterInfo> p(E::NumParams);
    p[E::HighpassOn] = makeParam("HighpassOn", "HP 80 Hz", 0.0f, 1.0f, 0.0f, "", true);
    p[E::LowShelfGain] = makeParam("LowShelfGain", "Low", 0.18f, 5.6f, 1.0f, "lin");
    p[E::MidGain] = makeParam("MidGain", "Mid", 0.25f, 4.0f, 1.0f, "lin");
    p[E::MidFrequency] = makeParam("MidFrequency", "Mid Freq", 120.0f, 7500.0f, 1500.0f, "Hz");
    p[E::HighShelfGain] = makeParam("HighShelfGain", "High", 0.18f, 5.6f, 1.0f, "lin");
    p[E::Gain] = makeParam("Gain", "Output", 0.25f, 4.0f, 1.0f, "lin");
    return p;
}())
{
    retarget(true);
    coeffs_ = design(settings(), sampleRate_);
}

ChannelEq::Settings ChannelEq::settings() const
{
    Settings s;
    s.highpass = value(HighpassOn) > 0.5f;
    s.lowDb = gainDb(value(LowShelfGain));
    s.midDb = gainDb(value(MidGain));
    s.midHz = value(MidFrequency);
    s.highDb = gainDb(value(HighShelfGain));
    s.outputDb = gainDb(value(Gain));
    return s;
}

void ChannelEq::retarget(bool snap)
{
    const auto s = settings();
    target_ = {s.lowDb, s.midDb, std::log2(std::max(s.midHz, 1.0)), s.highDb, s.outputDb, s.highpass ? 1.0 : 0.0,
               s.highDb < 0 ? 1.0 : 0.0};
    if (snap) current_ = target_;
}

void ChannelEq::prepare(double sampleRate, int)
{
    sampleRate_ = sampleRate > 0 ? sampleRate : 48000.0;
    // One-pole smoothing per 16-sample step with a ~20 ms time constant.
    smoothing_ = 1.0 - std::exp(-(double) kStep / (0.02 * sampleRate_));
    displayTap_.prepare(sampleRate_);
    parametersChanged.store(false, std::memory_order_relaxed);
    retarget(true);
    reset();
}

void ChannelEq::reset()
{
    for (auto& channel : state_)
        for (auto& s : channel) s.reset();
    for (auto& s : highpassState_) s.reset();
    current_ = target_;
    Settings s;
    s.lowDb = current_[LowDb]; s.midDb = current_[MidDb]; s.midHz = std::exp2(current_[MidOctave]);
    s.highDb = current_[HighDb]; s.outputDb = current_[OutputDb];
    coeffs_ = design(s, sampleRate_);
    phase_ = 0;
    displayTap_.reset();
}

void ChannelEq::process(juce::AudioBuffer<float>& audio, juce::MidiBuffer&)
{
    if (isBypassed())
        return;
    if (parametersChanged.exchange(false, std::memory_order_acquire))
        retarget(false);
    const int n = audio.getNumSamples();
    float* l = audio.getWritePointer(0);
    float* r = audio.getNumChannels() > 1 ? audio.getWritePointer(1) : nullptr;
    int i = 0;
    while (i < n)
    {
        if (phase_ == 0) // start of a step: move the smoothed settings and redesign when they moved
        {
            bool moved = false;
            for (size_t k = 0; k < current_.size(); ++k)
            {
                const double d = target_[k] - current_[k];
                if (juce::exactlyEqual(d, 0.0)) continue;
                current_[k] = std::abs(d) < 1e-4 ? target_[k] : current_[k] + d * smoothing_;
                moved = true;
            }
            if (moved)
            {
                Settings s;
                s.lowDb = current_[LowDb]; s.midDb = current_[MidDb]; s.midHz = std::exp2(current_[MidOctave]);
                s.highDb = current_[HighDb]; s.outputDb = current_[OutputDb];
                coeffs_ = design(s, sampleRate_);
            }
        }
        const int run = std::min(n - i, kStep - phase_);
        const float hpMix = (float) current_[HighpassMix], lpMix = (float) current_[LowpassMix];
        const float out = dsp::dbToGain((float) current_[OutputDb]);
        for (int ch = 0; ch < (r != nullptr ? 2 : 1); ++ch)
        {
            float* d = ch == 0 ? l : r;
            auto& st = state_[(size_t) ch];
            auto& hp = highpassState_[(size_t) ch];
            for (int j = i; j < i + run; ++j)
            {
                float x = d[j];
                const float filtered = hp.process(coeffs_.highpass, x);
                x += hpMix * (filtered - x);
                x = st[Low].process(coeffs_.svf[Low], x);
                x = st[Mid].process(coeffs_.svf[Mid], x);
                x = st[High].process(coeffs_.svf[High], x);
                const float lp = st[Lowpass].process(coeffs_.svf[Lowpass], x);
                x += lpMix * (lp - x);
                d[j] = x * out;
            }
        }
        i += run;
        phase_ = (phase_ + run) % kStep;
    }
    displayTap_.push(l, r != nullptr ? r : l, n);
}

} // namespace sonora::devices
