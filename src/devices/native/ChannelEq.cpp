#include "devices/native/ChannelEq.h"

#include "dsp/Dsp.h"

#include <algorithm>
#include <cmath>
#include <complex>

namespace sonora::devices
{

namespace
{
enum Smoothed { LowDb, MidDb, MidOctave, HighDb, OutputDb, HighpassMix };

// The high cut's low-pass (DEV-30b, measured at 44.1 kHz): fixed z-plane zeros, poles measured at three cuts.
constexpr double kLowpassZero1 = -0.4815504571, kLowpassZero2 = -0.8749352488;
struct LowpassAnchor { double db, hz, q; };
constexpr std::array<LowpassAnchor, 3> kLowpassAnchors{{{-3.0, 32792.54, 0.58654}, {-9.0, 14765.52, 0.64542},
                                                        {-14.8945, 10066.65, 0.65232}}};
// The fixed top-end droop every setting has (q-flat), as a z-plane biquad.
constexpr BiquadCoeffs kDroop{0.9802817855804391, 1.7720220809638105, 0.7985110377680459, 1.7704457741798683,
                              0.7803691301324274};

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

BiquadCoeffs inverse(const BiquadCoeffs& c)
{
    return {1.0 / c.b0, c.a1 / c.b0, c.a2 / c.b0, c.b1 / c.b0, c.b2 / c.b0};
}

// Orfanidis, "Digital Parametric Equalizer Design With Prescribed Nyquist-Frequency Gain" (JAES 1997), reference gain
// 1: peak gain g at hz, bandwidth bwHz between the gb points, gain g1 at Nyquist (1 <= g1 < gb < g).
BiquadCoeffs orfanidisPeak(double sampleRate, double hz, double bwHz, double g, double gb, double g1)
{
    const double w0 = 2.0 * M_PI * hz / sampleRate, dw = 2.0 * M_PI * bwHz / sampleRate;
    const double F = std::abs(g * g - gb * gb), G00 = std::abs(g * g - 1.0), F00 = std::abs(gb * gb - 1.0);
    const double F01 = std::abs(gb * gb - g1), G01 = std::abs(g * g - g1), G11 = std::abs(g * g - g1 * g1);
    const double F11 = std::abs(gb * gb - g1 * g1);
    const double t = std::tan(w0 / 2.0);
    const double W2 = std::sqrt(G11 / G00) * t * t;
    const double DW = (1.0 + std::sqrt(F00 / F11) * W2) * std::tan(dw / 2.0);
    const double C = F11 * DW * DW - 2.0 * W2 * (F01 - std::sqrt(F00 * F11));
    const double D = 2.0 * W2 * (G01 - std::sqrt(G00 * G11));
    const double A = std::sqrt((C + D) / F), B = std::sqrt((g * g * C + gb * gb * D) / F);
    const double den = 1.0 + W2 + A;
    return {(g1 + W2 + B) / den, -2.0 * (g1 - W2) / den, (g1 - B + W2) / den, -2.0 * (1.0 - W2) / den, (1.0 + W2 - A) / den};
}

// The Mid band: an analog Q 0.5 peak's shape without the bilinear cramping near Nyquist (laws in ChannelEq.h).
BiquadCoeffs midPeak(double sampleRate, double hz, double gainDb)
{
    if (std::abs(gainDb) < 1e-3) return {};
    hz = std::clamp(hz, 10.0, sampleRate * 0.45);
    const double q = ChannelEq::kMidQ, cut = std::abs(gainDb);
    const double g = std::pow(10.0, cut / 20.0), A = std::pow(10.0, cut / 40.0);
    // The analog peak (s^2 + s A/Q + 1) / (s^2 + s / (A Q) + 1) at Nyquist.
    const double o = sampleRate * 0.5 / hz, re = 1.0 - o * o;
    const double analogNyquist = std::sqrt((re * re + o * o * A * A / (q * q)) / (re * re + o * o / (A * A * q * q)));
    const double g1 = std::pow(analogNyquist, ChannelEq::kMidNyquistFraction);
    // Bandwidth: the bilinear-warped edges of the analog half-dB points, widened towards Nyquist.
    const double t = std::tan(M_PI * hz / sampleRate), r = std::sqrt(1.0 + 1.0 / (4.0 * q * q)), d = 1.0 / (2.0 * q);
    const double x = hz / sampleRate;
    const double bw = sampleRate / M_PI * (std::atan(t * (r + d)) - std::atan(t * (r - d))) * (1.0 + ChannelEq::kMidBandwidthWarp * x * x);
    const auto boost = orfanidisPeak(sampleRate, hz, bw, g, std::sqrt(g), g1);
    return gainDb > 0 ? boost : inverse(boost);
}

// The cut low-pass: poles from their analog equivalent through the (unwarped) bilinear transform, DC gain 1.
BiquadCoeffs cutLowpass(double sampleRate, ChannelEq::LowpassPoles poles)
{
    if (poles.hz <= 0) return {};
    const double k = 2.0 * sampleRate, w = 2.0 * M_PI * poles.hz;
    const double c1 = k / (w * poles.q), c2 = k * k / (w * w);
    const double a0 = 1.0 + c1 + c2;
    BiquadCoeffs c;
    c.a1 = (2.0 - 2.0 * c2) / a0;
    c.a2 = (1.0 - c1 + c2) / a0;
    const double norm = (1.0 + c.a1 + c.a2) / ((1.0 - kLowpassZero1) * (1.0 - kLowpassZero2));
    c.b0 = norm;
    c.b1 = -(kLowpassZero1 + kLowpassZero2) * norm;
    c.b2 = kLowpassZero1 * kLowpassZero2 * norm;
    return c;
}

float gainDb(float linear) { return dsp::gainToDb(std::max(linear, 1e-6f)); }
} // namespace

double ChannelEq::lowShelfHz(double lowDb)
{
    return kLowShelfHzAtZeroGain - kLowShelfHzPerGain * std::pow(10.0, lowDb / 20.0);
}

ChannelEq::LowpassPoles ChannelEq::lowpassPoles(double highDb, double sampleRate)
{
    if (highDb >= 0.0) return {};
    // 0 dB: the poles sit on the zeros (transparent). Their analog equivalents at this rate.
    const double s1 = 2.0 * sampleRate * (1.0 - kLowpassZero1) / (1.0 + kLowpassZero1);
    const double s2 = 2.0 * sampleRate * (1.0 - kLowpassZero2) / (1.0 + kLowpassZero2);
    LowpassAnchor previous{0.0, std::sqrt(s1 * s2) / (2.0 * M_PI), std::sqrt(s1 * s2) / (s1 + s2)};
    for (const auto& next : kLowpassAnchors)
    {
        if (highDb >= next.db)
        {
            const double f = (previous.db - highDb) / (previous.db - next.db);
            return {std::exp((1.0 - f) * std::log(previous.hz) + f * std::log(next.hz)), (1.0 - f) * previous.q + f * next.q};
        }
        previous = next;
    }
    return {previous.hz, previous.q};
}

ChannelEq::Biquads ChannelEq::designBiquads(const Settings& s, double sampleRate)
{
    using T = BiquadCoeffs::Type;
    Biquads c;
    c.highpass = firstOrderHighpass(sampleRate, kHighpassHz);
    c.stages[Low] = BiquadCoeffs::make(T::LowShelf, sampleRate, lowShelfHz(s.lowDb), kLowShelfQ, s.lowDb);
    c.stages[Mid] = midPeak(sampleRate, s.midHz, s.midDb);
    c.stages[High] = BiquadCoeffs::make(T::HighShelf, sampleRate, std::min(kHighShelfHz, sampleRate * 0.49), kHighShelfQ, s.highDb);
    c.stages[Lowpass] = cutLowpass(sampleRate, lowpassPoles(s.highDb, sampleRate));
    c.stages[Droop] = kDroop;
    return c;
}

ChannelEq::Coeffs ChannelEq::design(const Settings& s, double sampleRate)
{
    const auto b = designBiquads(s, sampleRate);
    Coeffs c;
    c.highpass = b.highpass;
    for (size_t i = 0; i < NumStages; ++i)
        c.svf[i] = SvfCoeffs::fromBiquad(b.stages[i]);
    return c;
}

double ChannelEq::response(const Settings& s, double hz, double sampleRate)
{
    const auto z1 = std::polar(1.0, -2.0 * M_PI * hz / sampleRate);
    const auto b = designBiquads(s, sampleRate);
    std::complex<double> h = std::pow(10.0, s.outputDb / 20.0);
    if (s.highpass) h *= at(b.highpass, z1);
    for (const auto& stage : b.stages) h *= at(stage, z1);
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

ChannelEq::Settings ChannelEq::smoothedSettings() const
{
    Settings s;
    s.lowDb = current_[LowDb];
    s.midDb = current_[MidDb];
    s.midHz = std::exp2(current_[MidOctave]);
    s.highDb = current_[HighDb];
    s.outputDb = current_[OutputDb];
    return s;
}

void ChannelEq::retarget(bool snap)
{
    const auto s = settings();
    target_ = {s.lowDb, s.midDb, std::log2(std::max(s.midHz, 1.0)), s.highDb, s.outputDb, s.highpass ? 1.0 : 0.0};
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
    coeffs_ = design(smoothedSettings(), sampleRate_);
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
                coeffs_ = design(smoothedSettings(), sampleRate_);
        }
        const int run = std::min(n - i, kStep - phase_);
        const float hpMix = (float) current_[HighpassMix];
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
                for (size_t k = 0; k < NumStages; ++k)
                    x = st[k].process(coeffs_.svf[k], x);
                d[j] = x * out;
            }
        }
        i += run;
        phase_ = (phase_ + run) % kStep;
    }
    displayTap_.push(l, r != nullptr ? r : l, n);
}

} // namespace sonora::devices
