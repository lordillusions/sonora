#include "devices/native/Hush.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace sonora::devices
{
namespace
{
constexpr float kPi = 3.14159265358979f;

std::vector<ParameterInfo> hushParameters()
{
    std::vector<ParameterInfo> p;
    p.push_back(makeParam("Mode", "Mode", 0, 1, 0, {}, false, {"soft", "hard"}));
    p.push_back(makeParam("Depth", "Depth", 0, 10, 5));
    p.push_back(makeParam("Detail", "Detail", 0, 10, 5));
    p.push_back(makeParam("Attack", "Attack", 0, 10, 5));
    p.push_back(makeParam("Release", "Release", 0, 10, 3));
    p.push_back(makeParam("Stereo mode", "Stereo mode", 0, 1, 0, {}, false, {"L/R", "M/S"}));
    p.push_back(makeParam("Stereo link", "Stereo link", 0, 100, 100, "%"));
    p.push_back(makeParam("Stereo focus", "Stereo focus", 0, 1, 0.5f));
    for (const char* name : {"Detail tilt low", "Detail tilt high", "Attack tilt low", "Attack tilt high", "Release tilt low",
                             "Release tilt high"})
        p.push_back(makeParam(name, name, 0.1f, 10.0f, 1.0f, "x"));
    p.push_back(makeParam("Max cut", "Max cut", 0, Hush::kMaxCutOff, Hush::kMaxCutOff, "dB"));
    p.push_back(makeParam("Wet trim", "Wet trim", -18, 18, 0, "dB"));
    p.push_back(makeParam("Bypass", "Bypass", 0, 1, 0, {}, true));
    p.push_back(makeParam("Delta", "Delta", 0, 1, 0, {}, true));
    p.push_back(makeParam("Mix", "Mix", 0, 100, 100, "%"));
    p.push_back(makeParam("Out trim", "Out trim", -18, 18, 0, "dB"));
    p.push_back(makeParam("Quality", "Quality", 0, 2, 0, {}, false, {"normal", "high", "ultra"}));
    p.push_back(makeParam("Linear phase", "Linear phase", 0, 1, 0, {}, true));
    p.push_back(makeParam("Low latency", "Low latency", 0, 1, 0, {}, true));
    p.push_back(makeParam("Sidechain", "Sidechain", 0, 1, 0, {}, true));
    p.push_back(makeParam("Sidechain listen", "Sidechain listen", 0, 1, 0, {}, true));
    p.push_back(makeParam("Band solo", "Band solo", 0, Hush::kBands, 0, "int"));
    // The first three bands start as a low cut, a bell and a high cut, like a typical starting curve.
    const hush::Band defaults[3] = {{true, true, hush::Shape::LowCut, 100.0f, 0.0f, 0.707f, 5.0f, 0.5f},
                                    {true, true, hush::Shape::Bell, 4000.0f, 2.5f, 0.707f, 5.0f, 0.5f},
                                    {true, true, hush::Shape::HighCut, 16000.0f, 0.0f, 0.707f, 5.0f, 0.5f}};
    std::vector<std::string> shapes;
    for (int s = 0; s < hush::kShapes; ++s) shapes.push_back(hush::shapeName((hush::Shape) s));
    for (int b = 0; b < Hush::kBands; ++b)
    {
        const hush::Band d = b < 3 ? defaults[b] : hush::Band{};
        const std::string prefix = "Band " + std::to_string(b + 1) + " ";
        p.push_back(makeParam(prefix + "used", prefix + "used", 0, 1, d.used ? 1.0f : 0.0f, {}, true));
        p.push_back(makeParam(prefix + "enabled", prefix + "enabled", 0, 1, 1, {}, true));
        p.push_back(makeParam(prefix + "shape", prefix + "shape", 0, hush::kShapes - 1, (float) d.shape, {}, false, shapes));
        p.push_back(makeParam(prefix + "frequency", prefix + "frequency", 20, 20000, d.frequency, "Hz"));
        p.push_back(makeParam(prefix + "depth", prefix + "depth", -10, 10, d.depth));
        p.push_back(makeParam(prefix + "q", prefix + "q", 0.1f, 10.0f, d.q));
        p.push_back(makeParam(prefix + "slope", prefix + "slope", 0, 10, d.slope));
        p.push_back(makeParam(prefix + "focus", prefix + "focus", 0, 1, d.focus));
    }
    return p;
}

int factorFor(double sampleRate) { return sampleRate <= 50000.0 ? 1 : sampleRate <= 100000.0 ? 2 : 4; }

float lerpTable(const float (&table)[5], float x) // x in 0..10 on a 2.5 grid, log interpolation
{
    const float t = std::clamp(x, 0.0f, 10.0f) / 2.5f;
    const int i = std::min(3, (int) t);
    const float f = t - (float) i;
    return std::exp(std::log(table[i]) * (1.0f - f) + std::log(table[i + 1]) * f);
}

// How much a low (below ~450 Hz) or high (above ~2 kHz) tilt applies at `hz`.
float lowShare(float hz) { return 1.0f / (1.0f + (hz / 450.0f) * (hz / 450.0f)); }
float highShare(float hz) { return 1.0f / (1.0f + (2000.0f / hz) * (2000.0f / hz)); }
// Measured: a x10 time tilt slows by about x2.5, a x0.1 tilt speeds up about x9.
float timeTilt(float tilt, float share)
{
    const float t = std::clamp(tilt, 0.1f, 10.0f);
    return std::pow(t, (t > 1.0f ? 0.4f : 0.95f) * share);
}

// Built-in frequency emphasis of the reduction (measured on the reference with a tone in white noise, relative to
// 1 kHz): the ear's most sensitive region, 2-8 kHz, is treated about three times harder than the lows.
float emphasis(float hz)
{
    static constexpr float kHz[] = {20, 50, 100, 200, 300, 500, 700, 1000, 1400, 2000, 2800, 4000, 5600, 8000, 12000, 16000, 24000};
    static constexpr float kGain[] = {1.055f, 1.385f, 0.967f, 1.183f, 0.975f, 0.824f, 0.853f, 1.000f, 1.488f, 2.825f, 4.088f, 4.669f, 4.917f, 4.458f, 3.942f, 3.245f, 2.368f};
    constexpr int n = (int) std::size(kHz);
    if (hz <= kHz[0]) return kGain[0];
    for (int i = 1; i < n; ++i)
        if (hz <= kHz[i])
        {
            const float t = std::log(hz / kHz[i - 1]) / std::log(kHz[i] / kHz[i - 1]);
            return kGain[i - 1] + t * (kGain[i] - kGain[i - 1]);
        }
    return kGain[n - 1];
}

int orderOf(int size)
{
    int order = 0;
    while ((1 << order) < size) ++order;
    return order;
}
} // namespace

Hush::Tuning& Hush::tuning()
{
    static Tuning t = [] {
        Tuning v;
        // Calibration harness only (docs/HUSH.md): SONORA_HUSH_TUNE="softGain=0.2,knee=8".
        if (const char* env = std::getenv("SONORA_HUSH_TUNE"))
        {
            const juce::StringArray items = juce::StringArray::fromTokens(env, ",", "");
            for (const auto& item : items)
            {
                const auto key = item.upToFirstOccurrenceOf("=", false, false).trim();
                const float value = item.fromFirstOccurrenceOf("=", false, false).getFloatValue();
                float* fields[] = {&v.softGain, &v.softCurve, &v.hardGain, &v.hardCurve, &v.hardLevel, &v.hardThreshold,
                                   &v.hardThresholdSlope, &v.knee, &v.envelopeRatio, &v.floorDb, &v.floorRange, &v.excessFloor, &v.widthComp, &v.widthScale, &v.curveBoost, &v.curveCut, &v.curveSaturation, &v.temporalGain, &v.temporalTime, &v.temporalFloor, &v.temporalFloorSlope};
                const char* names[] = {"softGain", "softCurve", "hardGain", "hardCurve", "hardLevel", "hardThreshold",
                                       "hardThresholdSlope", "knee", "envelopeRatio", "floorDb", "floorRange", "excessFloor", "widthComp", "widthScale", "curveBoost", "curveCut", "curveSaturation", "temporalGain", "temporalTime", "temporalFloor", "temporalFloorSlope"};
                for (size_t i = 0; i < std::size(names); ++i)
                    if (key == names[i]) *fields[i] = value;
            }
        }
        return v;
    }();
    return t;
}

float Hush::displayFrequency(int point)
{
    return 20.0f * std::pow(1000.0f, (float) point / (float) (kDisplayPoints - 1));
}

int Hush::latencyFor(double sampleRate, bool linearPhase, bool lowLatency)
{
    if (lowLatency) return 0;
    return factorFor(sampleRate) * (linearPhase ? 3456 : 2304);
}

int Hush::currentLatency() const
{
    return latencyFor(sampleRate_, value(LinearPhase) >= 0.5f, lowLatencyActive());
}

bool Hush::lowLatencyActive() const
{
    return value(LowLatency) >= 0.5f || forceLowLatency_.load(std::memory_order_relaxed);
}

int Hush::activeQuality() const
{
    const int forced = forceQuality_.load(std::memory_order_relaxed);
    return forced >= 0 ? forced : (int) std::lround(value(Quality));
}

void Hush::setOverrides(int quality, bool lowLatency)
{
    if (forceQuality_.exchange(quality) != quality || forceLowLatency_.exchange(lowLatency) != lowLatency)
        parametersChanged.store(true, std::memory_order_release);
}

Hush::Hush() : NativeDevice(hushParameters()) {}

std::array<hush::Band, Hush::kBands> Hush::bands() const
{
    std::array<hush::Band, kBands> out;
    for (int b = 0; b < kBands; ++b)
    {
        auto& band = out[(size_t) b];
        band.used = value(bandParam(b, BandUsed)) >= 0.5f;
        band.enabled = value(bandParam(b, BandEnabled)) >= 0.5f;
        band.shape = (hush::Shape) std::clamp((int) std::lround(value(bandParam(b, BandShape))), 0, hush::kShapes - 1);
        band.frequency = value(bandParam(b, BandFrequency));
        band.depth = value(bandParam(b, BandDepth));
        band.q = value(bandParam(b, BandQ));
        band.slope = value(bandParam(b, BandSlope));
        band.focus = value(bandParam(b, BandFocus));
    }
    return out;
}

void Hush::setBand(int index, const hush::Band& band)
{
    if (index < 0 || index >= kBands) return;
    setParameter(bandParam(index, BandUsed), band.used ? 1.0f : 0.0f);
    setParameter(bandParam(index, BandEnabled), band.enabled ? 1.0f : 0.0f);
    setParameter(bandParam(index, BandShape), (float) band.shape);
    setParameter(bandParam(index, BandFrequency), band.frequency);
    setParameter(bandParam(index, BandDepth), band.depth);
    setParameter(bandParam(index, BandQ), band.q);
    setParameter(bandParam(index, BandSlope), band.slope);
    setParameter(bandParam(index, BandFocus), band.focus);
}

Hush::Config Hush::configFor(int quality, bool linear, bool lowLatency) const
{
    Config c;
    c.factor = factor_;
    c.analysisSize = 4096 * factor_;
    c.analysisWindow = 2560 * factor_;
    // Quality sets how often the spectrum is analysed; the filter is applied in short frames (512, hop 128) so the
    // analysis window leads the audio it shapes by 768 samples: that lookahead is what lets fast attacks land on time.
    c.hop = (512 * factor_) >> std::clamp(quality, 0, 2);
    c.synthHop = 128 * factor_;
    c.frame = 4 * c.synthHop;
    c.linear = linear && ! lowLatency;
    c.lowLatency = lowLatency;
    c.filterLength = c.linear ? 2304 * factor_ : 3072 * factor_;
    c.convSize = 4096 * factor_;
    c.latency = latencyFor(sampleRate_, linear, lowLatency);
    return c;
}

juce::dsp::FFT& Hush::fft(int size) { return *ffts_[(size_t) orderOf(size)]; }

void Hush::prepare(double sampleRate, int maxBlockSize)
{
    sampleRate_ = sampleRate > 0 ? sampleRate : 44100.0;
    maxBlock_ = std::max(1, maxBlockSize);
    factor_ = factorFor(sampleRate_);
    const int maxAnalysis = 4096 * factor_, maxConv = 8192 * factor_;
    bins_ = maxAnalysis / 2 + 1;
    for (int order = 8; order <= orderOf(maxConv); ++order)
        if (ffts_[(size_t) order] == nullptr) ffts_[(size_t) order] = std::make_unique<juce::dsp::FFT>(order);

    // Calibrate the engine's scaling once (JUCE's engines differ): forward unnormalised, inverse divides by N.
    {
        std::vector<float> d((size_t) maxAnalysis * 2, 0.0f);
        d[0] = 1.0f;
        fft(maxAnalysis).performRealOnlyForwardTransform(d.data(), true);
        forwardScale_ = d[0] != 0.0f ? 1.0f / d[0] : 1.0f;
        for (size_t i = 0; i < d.size(); i += 2)
        {
            d[i] = 1.0f;
            d[i + 1] = 0.0f;
        }
        fft(maxAnalysis).performRealOnlyInverseTransform(d.data());
        inverseScale_ = d[0] != 0.0f ? 1.0f / d[0] : 1.0f;
    }

    ringSize_ = 1 << orderOf(maxConv * 2 + 3456 * factor_ + maxBlock_ * 2);
    ringMask_ = ringSize_ - 1;
    for (int ch = 0; ch < 2; ++ch)
    {
        mainRing_[(size_t) ch].assign((size_t) ringSize_, 0.0f);
        detRing_[(size_t) ch].assign((size_t) ringSize_, 0.0f);
        outRing_[(size_t) ch].assign((size_t) ringSize_, 0.0f);
        for (auto* v : {&power_, &excess_, &spread_, &level_, &reduction_, &smoothed_, &gain_, &slowEnvelope_})
            (*v)[(size_t) ch].assign((size_t) bins_, 0.0f);
        spec_[(size_t) ch].assign((size_t) maxConv * 2 + 2, 0.0f);
        curveWeight_[ch].assign((size_t) bins_, 1.0f);
        curveDb_[ch].assign((size_t) bins_, 0.0f);
        llTaps_ = 512 * factor_;
        llHistory_[(size_t) ch].assign((size_t) llTaps_ * 2, 0.0f);
        llCoef_[(size_t) ch].assign((size_t) llTaps_, 0.0f);
        llPrev_[(size_t) ch].assign((size_t) llTaps_, 0.0f);
        llCoef_[(size_t) ch][0] = llPrev_[(size_t) ch][0] = 1.0f;
    }
    fftA_.assign((size_t) maxConv * 2 + 2, 0.0f);
    fftB_.assign((size_t) maxConv * 2 + 2, 0.0f);
    impulse_.assign((size_t) maxConv, 0.0f);
    prefix_.assign((size_t) bins_ + 1, 0.0);
    binHz_.resize((size_t) bins_);
    for (int k = 0; k < bins_; ++k) binHz_[(size_t) k] = std::max(1.0f, (float) (k * sampleRate_ / maxAnalysis));
    attackCoef_.assign((size_t) bins_, 1.0f);
    releaseCoef_.assign((size_t) bins_, 1.0f);
    spreadBins_.assign((size_t) bins_, 1.0f);
    widthComp_.assign((size_t) bins_, 1.0f);
    emphasis_.assign((size_t) bins_, 1.0f);
    listenRegion_.assign((size_t) bins_, 0.0f);
    analysisWindow_.resize((size_t) (2560 * factor_));
    for (size_t n = 0; n < analysisWindow_.size(); ++n)
        analysisWindow_[n] = 0.5f - 0.5f * std::cos(2.0f * kPi * ((float) n + 0.5f) / (float) analysisWindow_.size());
    synthWindow_.resize((size_t) (512 * factor_));
    for (size_t n = 0; n < synthWindow_.size(); ++n) // periodic Hann at 75 % overlap sums to 2: halved here
        synthWindow_[n] = 0.25f - 0.25f * std::cos(2.0f * kPi * (float) n / (float) synthWindow_.size());

    displayLo_.resize((size_t) kDisplayPoints);
    displayHi_.resize((size_t) kDisplayPoints);
    for (int p = 0; p < kDisplayPoints; ++p)
    {
        const float lo = displayFrequency(p) * std::pow(1000.0f, -0.5f / (float) (kDisplayPoints - 1));
        const float hi = displayFrequency(p) * std::pow(1000.0f, 0.5f / (float) (kDisplayPoints - 1));
        displayLo_[(size_t) p] = std::clamp((int) std::floor(lo * (float) maxAnalysis / (float) sampleRate_), 1, bins_ - 1);
        displayHi_[(size_t) p] = std::clamp((int) std::ceil(hi * (float) maxAnalysis / (float) sampleRate_), displayLo_[(size_t) p], bins_ - 1);
    }

    for (auto* s : {&mix_, &wetGain_, &outGain_, &bypassMix_}) s->reset(sampleRate_, 0.02);
    cfg_ = configFor(activeQuality(), value(LinearPhase) >= 0.5f, lowLatencyActive());
    latency_.store(cfg_.latency, std::memory_order_relaxed);
    parametersChanged.store(true);
    reset();
}

void Hush::reset()
{
    for (int ch = 0; ch < 2; ++ch)
    {
        std::fill(mainRing_[(size_t) ch].begin(), mainRing_[(size_t) ch].end(), 0.0f);
        std::fill(detRing_[(size_t) ch].begin(), detRing_[(size_t) ch].end(), 0.0f);
        std::fill(outRing_[(size_t) ch].begin(), outRing_[(size_t) ch].end(), 0.0f);
        std::fill(smoothed_[(size_t) ch].begin(), smoothed_[(size_t) ch].end(), 0.0f);
        std::fill(slowEnvelope_[(size_t) ch].begin(), slowEnvelope_[(size_t) ch].end(), -300.0f);
        std::fill(llHistory_[(size_t) ch].begin(), llHistory_[(size_t) ch].end(), 0.0f);
        for (auto& d : display_[(size_t) ch]) d.store(0.0f, std::memory_order_relaxed);
    }
    count_ = 0;
    primed_ = false;
    llFade_ = 0;
    mix_.setCurrentAndTargetValue(value(Mix) / 100.0f);
    wetGain_.setCurrentAndTargetValue(juce::Decibels::decibelsToGain(value(WetTrim)));
    outGain_.setCurrentAndTargetValue(juce::Decibels::decibelsToGain(value(OutTrim)));
    bypassMix_.setCurrentAndTargetValue(value(Bypass) >= 0.5f ? 1.0f : 0.0f);
}

void Hush::setSidechain(const juce::AudioBuffer<float>* audio, float gain)
{
    sidechain_ = audio;
    sidechainGain_ = gain;
}

void Hush::updateDerived()
{
    const auto bandList = bands();
    const float detail = value(Detail), attack = value(Attack), release = value(Release);
    const bool soft = value(Mode) < 0.5f;
    const float hopSeconds = (float) cfg_.hop / (float) sampleRate_;
    const float binHz = (float) (sampleRate_ / cfg_.analysisSize);
    // Measured notch width (FWHM, Hz at 1 kHz) for detail 0, 2.5, 5, 7.5, 10; it grows with f^0.55.
    static constexpr float kWidth[5] = {1128.0f, 679.0f, 362.0f, 166.0f, 76.0f};
    const float width1k = lerpTable(kWidth, detail);
    const float tauAttack = 0.055f * std::pow(10.0f, 0.29f * (attack - 5.0f));
    const float tauRelease = std::max(0.03f, 0.12f * std::pow(10.0f, 0.2f * (release - 5.0f))); // measured floor
    const float slowDetail = soft ? 1.0f + 0.08f * std::max(0.0f, detail - 5.0f) : 1.0f;
    const int listen = (int) std::lround(value(BandListen)) - 1;
    for (int k = 0; k < bins_; ++k)
    {
        const float hz = binHz_[(size_t) k], lo = lowShare(hz), hi = highShare(hz);
        const float detailTilt = std::pow(std::clamp(value(DetailTiltLow), 0.1f, 10.0f), 0.6f * lo)
                               * std::pow(std::clamp(value(DetailTiltHigh), 0.1f, 10.0f), 0.6f * hi);
        // The analysis window's main lobe (about 3 bins) widens every notch: take it out of the kernel.
        const float widthHz = tuning().widthScale * width1k * std::pow(hz / 1000.0f, 0.55f) / detailTilt;
        const float lobe = 3.0f * binHz;
        const float kernelHz = std::sqrt(std::max(widthHz * widthHz - lobe * lobe, 0.09f * widthHz * widthHz));
        spreadBins_[(size_t) k] = std::max(1.0f, kernelHz / binHz);
        // The spread dilutes a peak by its width; undo the part of that which comes from the width growing with f.
        widthComp_[(size_t) k] = std::pow(std::max(hz, 20.0f) / 1000.0f, 0.55f * tuning().widthComp);
        emphasis_[(size_t) k] = emphasis(hz);
        const float freqTime = std::pow(1000.0f / hz, 0.12f) * slowDetail;
        const float ta = tauAttack * freqTime * timeTilt(value(AttackTiltLow), lo) * timeTilt(value(AttackTiltHigh), hi);
        const float tr = tauRelease * freqTime * timeTilt(value(ReleaseTiltLow), lo) * timeTilt(value(ReleaseTiltHigh), hi);
        attackCoef_[(size_t) k] = 1.0f - std::exp(-hopSeconds / std::max(1.0e-4f, ta));
        releaseCoef_[(size_t) k] = 1.0f - std::exp(-hopSeconds / std::max(1.0e-4f, tr));
        const float focus = value(Focus);
        for (int c = 0; c < 2; ++c)
        {
            const auto point = hush::evaluate(bandList, hz, c);
            curveWeight_[c][(size_t) k] = point.gate * hush::focusAmount(focus, c);
            curveDb_[c][(size_t) k] = point.curve;
        }
        listenRegion_[(size_t) k] = listen >= 0 && listen < kBands && bandList[(size_t) listen].used
                                        ? hush::bandRegion(bandList[(size_t) listen], hz)
                                        : 0.0f;
    }
    mix_.setTargetValue(value(Mix) / 100.0f);
    wetGain_.setTargetValue(juce::Decibels::decibelsToGain(value(WetTrim)));
    outGain_.setTargetValue(juce::Decibels::decibelsToGain(value(OutTrim)));
    bypassMix_.setTargetValue(value(Bypass) >= 0.5f ? 1.0f : 0.0f);
}

namespace
{
// Mean of `in` over [k - w/2, k + w/2] per bin (w in bins), via prefix sums; twice = a triangle of base 2w.
void boxMean(const float* in, float* out, int n, const float* widths, float widthScale, std::vector<double>& prefix)
{
    prefix[0] = 0.0;
    for (int k = 0; k < n; ++k) prefix[(size_t) k + 1] = prefix[(size_t) k] + in[k];
    for (int k = 0; k < n; ++k)
    {
        const int half = std::max(1, (int) std::lround(0.5f * widths[k] * widthScale));
        const int a = std::max(0, k - half), b = std::min(n - 1, k + half);
        out[k] = (float) ((prefix[(size_t) b + 1] - prefix[(size_t) a]) / (double) (b - a + 1));
    }
}
} // namespace

void Hush::analyse(long long t)
{
    const int na = cfg_.analysisSize, wa = (int) analysisWindow_.size(), nb = na / 2 + 1;
    const bool ms = value(StereoMode) >= 0.5f;
    double windowSum = 0.0;
    for (float w : analysisWindow_) windowSum += w;
    const float norm = (float) (4.0 / (windowSum * windowSum)) * forwardScale_ * forwardScale_;
    for (int c = 0; c < 2; ++c)
    {
        std::fill(fftA_.begin(), fftA_.begin() + na * 2, 0.0f);
        for (int n = 0; n < wa; ++n)
        {
            const int idx = (int) ((t - wa + n) & ringMask_);
            const float l = detRing_[0][(size_t) idx], r = detRing_[1][(size_t) idx];
            const float x = ms ? (c == 0 ? 0.5f * (l + r) : 0.5f * (l - r)) : (c == 0 ? l : r);
            fftA_[(size_t) n] = x * analysisWindow_[(size_t) n];
        }
        fft(na).performRealOnlyForwardTransform(fftA_.data(), true);
        auto& p = power_[(size_t) c];
        for (int k = 0; k < nb; ++k)
            p[(size_t) k] = (fftA_[(size_t) k * 2] * fftA_[(size_t) k * 2] + fftA_[(size_t) k * 2 + 1] * fftA_[(size_t) k * 2 + 1]) * norm;
    }
}

void Hush::computeReduction()
{
    const auto& tune = tuning();
    const int nb = cfg_.analysisSize / 2 + 1;
    const float link = std::clamp(value(Link) / 100.0f, 0.0f, 1.0f);
    const float depth = value(Depth);
    const bool soft = value(Mode) < 0.5f;
    const float maxCut = value(MaxCut);
    const float mix = std::clamp(value(Mix) / 100.0f, 0.0f, 1.0f);
    const float gainS = tune.softGain * depth * (1.0f + tune.softCurve * depth);
    const float gainH = tune.hardGain * depth * (1.0f + tune.hardCurve * depth);
    const float threshold = tune.hardThreshold - tune.hardThresholdSlope * depth;
    // Measured: high detail cuts much deeper than its narrower width alone explains (x1.15 at 7.5, x2.2 at 10).
    const float detailValue = value(Detail);
    const float detailGain = detailValue <= 5.0f ? 1.0f
                           : detailValue <= 7.5f ? std::pow(1.15f, (detailValue - 5.0f) / 2.5f)
                                                 : 1.15f * std::pow(2.17f / 1.15f, (detailValue - 7.5f) / 2.5f);
    float* scratch = fftB_.data();
    float* scratch2 = fftB_.data() + bins_;
    for (int c = 0; c < 2; ++c)
    {
        auto& lvl = level_[(size_t) c];
        auto& ex = excess_[(size_t) c];
        float top = -300.0f;
        for (int k = 0; k < nb; ++k)
        {
            const float pc = power_[(size_t) c][(size_t) k], avg = 0.5f * (power_[0][(size_t) k] + power_[1][(size_t) k]);
            const float p = (1.0f - link) * pc + link * avg;
            lvl[(size_t) k] = 10.0f * std::log10(p + 1.0e-14f);
            if (k > 0) top = std::max(top, lvl[(size_t) k]);
        }
        const float floor = std::max(tune.floorDb, top - tune.floorRange);
        // Local envelope: twice a box mean of the dB spectrum over envelopeRatio x the spread width.
        boxMean(lvl.data(), scratch, nb, spreadBins_.data(), tune.envelopeRatio, prefix_);
        boxMean(scratch, scratch2, nb, spreadBins_.data(), tune.envelopeRatio, prefix_);
        // Soft mode's threshold also adapts over time: the envelope rising above its own recent average (transients,
        // a new tonal build-up) counts as excess too. Stationary material, tonal or noisy, shows none of it.
        auto& slow = slowEnvelope_[(size_t) c];
        // Measured on drums: barely anything at depth 5, several dB at depth 10, so the rise needed drops with depth.
        const float riseFloor = tune.temporalFloor + tune.temporalFloorSlope * (10.0f - depth);
        const float slowCoef = 1.0f - std::exp(-(float) cfg_.hop / ((float) sampleRate_ * std::max(0.01f, tune.temporalTime)));
        for (int k = 0; k < nb; ++k)
        {
            const float env = scratch2[k];
            if (! std::isfinite(slow[(size_t) k]) || slow[(size_t) k] < -200.0f) slow[(size_t) k] = env;
            const float rise = std::max(0.0f, env - slow[(size_t) k] - riseFloor);
            slow[(size_t) k] += slowCoef * (env - slow[(size_t) k]);
            ex[(size_t) k] = (k == 0 || lvl[(size_t) k] < floor)
                                 ? 0.0f
                                 : std::max(0.0f, lvl[(size_t) k] - env - tune.excessFloor) + tune.temporalGain * rise;
        }
        // Spread the excess over the detail width (area preserving: narrow peaks become notches of that width).
        boxMean(ex.data(), scratch, nb, spreadBins_.data(), 1.0f, prefix_);
        boxMean(scratch, spread_[(size_t) c].data(), nb, spreadBins_.data(), 1.0f, prefix_);
        if (! soft)
        {
            // Hard mode: the local level (power mean over the width) above an absolute threshold adds drive.
            for (int k = 0; k < nb; ++k) scratch2[k] = power_[(size_t) c][(size_t) k];
            boxMean(scratch2, scratch, nb, spreadBins_.data(), 1.0f, prefix_);
            for (int k = 0; k < nb; ++k)
                scratch2[k] = std::max(0.0f, 10.0f * std::log10(scratch[k] + 1.0e-14f) - threshold);
        }
        auto& r = reduction_[(size_t) c];
        const float knee = std::max(0.1f, tune.knee);
        for (int k = 0; k < nb; ++k)
        {
            // The excess compresses and depth scales it linearly (measured). Hard mode adds the local level above an
            // absolute threshold that drops with depth, at a fixed dB-per-dB slope.
            // The depth curve and the built-in emphasis scale the drive before it compresses (measured: the same
            // boost helps a small cut far more than a deep one); cuts and pass bands gate.
            const float gate = curveWeight_[c][(size_t) k], curve = curveDb_[c][(size_t) k];
            const float base = std::max(0.0f, spread_[(size_t) c][(size_t) k] * widthComp_[(size_t) k] * emphasis_[(size_t) k]);
            float weight = std::pow(10.0f, (curve > 0 ? tune.curveBoost : tune.curveCut) * curve / 20.0f);
            if (weight > 1.0f) weight = 1.0f + (weight - 1.0f) * tune.curveSaturation / (tune.curveSaturation + base); // boosts saturate
            const float u = base * weight * gate;
            float v = (soft ? gainS : gainH) * detailGain * knee * (std::sqrt(1.0f + 2.0f * u / knee) - 1.0f);
            if (! soft) v += tune.hardLevel * gate * emphasis_[(size_t) k] * std::max(0.0f, scratch2[k] + curve);
            r[(size_t) k] = v;
        }
    }

    float peak[2] = {0.0f, 0.0f};
    for (int c = 0; c < 2; ++c)
    {
        auto& s = smoothed_[(size_t) c];
        auto& g = gain_[(size_t) c];
        const auto& r = reduction_[(size_t) c];
        for (int k = 0; k < nb; ++k)
        {
            float v = s[(size_t) k];
            v += (r[(size_t) k] > v ? attackCoef_[(size_t) k] : releaseCoef_[(size_t) k]) * (r[(size_t) k] - v);
            s[(size_t) k] = v;
            float out = maxCut < kMaxCutOff ? v / std::pow(1.0f + std::pow(v / std::max(0.05f, maxCut), 4.0f), 0.25f)
                                            : std::min(v, 60.0f);
            g[(size_t) k] = std::pow(10.0f, -out / 20.0f);
            peak[c] = std::max(peak[c], out);
        }
        flat_[(size_t) c] = peak[c] < 0.01f;
    }

    // Reduction graph: the largest reduction (mix included) over each display point's bins.
    for (int c = 0; c < 2; ++c)
    {
        float top = 0.0f;
        for (int p = 0; p < kDisplayPoints; ++p)
        {
            float m = 1.0f;
            for (int k = displayLo_[(size_t) p]; k <= std::min(displayHi_[(size_t) p], nb - 1); ++k)
                m = std::min(m, gain_[(size_t) c][(size_t) k]);
            const float shown = -20.0f * std::log10(std::max(1.0e-6f, mix * m + 1.0f - mix));
            display_[(size_t) c][(size_t) p].store(shown, std::memory_order_relaxed);
            top = std::max(top, shown);
        }
        maxReduction_[(size_t) c].store(top, std::memory_order_relaxed);
    }
    displayRevision_.fetch_add(1, std::memory_order_relaxed);
}

void Hush::designMinimumPhase(const float* gain, float* impulse, int length)
{
    // Real cepstrum of log|G| folded onto positive quefrencies = the minimum-phase spectrum.
    const int n = cfg_.analysisSize, nb = n / 2 + 1;
    auto& d = fftA_;
    for (int k = 0; k < n; ++k)
    {
        const int b = k < nb ? k : n - k;
        d[(size_t) k * 2] = std::log(std::max(gain[b], 1.0e-5f));
        d[(size_t) k * 2 + 1] = 0.0f;
    }
    fft(n).performRealOnlyInverseTransform(d.data());
    for (int i = 0; i < n; ++i) d[(size_t) i] *= inverseScale_;
    for (int i = 1; i < n / 2; ++i) d[(size_t) i] *= 2.0f;
    for (int i = n / 2 + 1; i < 2 * n; ++i) d[(size_t) i] = 0.0f;
    fft(n).performRealOnlyForwardTransform(d.data(), true);
    for (int k = 0; k < nb; ++k)
    {
        const float re = d[(size_t) k * 2] * forwardScale_, im = d[(size_t) k * 2 + 1] * forwardScale_;
        const float mag = std::exp(std::min(re, 20.0f));
        d[(size_t) k * 2] = mag * std::cos(im);
        d[(size_t) k * 2 + 1] = mag * std::sin(im);
    }
    for (int k = nb; k < n; ++k)
    {
        d[(size_t) k * 2] = d[(size_t) (n - k) * 2];
        d[(size_t) k * 2 + 1] = -d[(size_t) (n - k) * 2 + 1];
    }
    fft(n).performRealOnlyInverseTransform(d.data());
    const int taper = std::max(1, length / 8);
    for (int i = 0; i < length; ++i)
    {
        float v = d[(size_t) i] * inverseScale_;
        if (i >= length - taper) v *= 0.5f + 0.5f * std::cos(kPi * (float) (i - (length - taper)) / (float) taper);
        impulse[i] = v;
    }
}

void Hush::designLinearPhase(const float* gain, float* impulse, int length)
{
    const int n = cfg_.analysisSize, nb = n / 2 + 1;
    auto& d = fftA_;
    for (int k = 0; k < n; ++k)
    {
        d[(size_t) k * 2] = gain[k < nb ? k : n - k];
        d[(size_t) k * 2 + 1] = 0.0f;
    }
    fft(n).performRealOnlyInverseTransform(d.data());
    const int half = length / 2;
    for (int i = 0; i < length; ++i)
    {
        const int src = ((i - half) % n + n) % n;
        const float w = 0.5f - 0.5f * std::cos(2.0f * kPi * ((float) i + 0.5f) / (float) length);
        impulse[i] = d[(size_t) src] * inverseScale_ * w;
    }
}

void Hush::spectrumOf(const float* impulse, int length, float* spectrum)
{
    const int nc = cfg_.convSize;
    std::fill(spectrum, spectrum + nc * 2, 0.0f);
    std::copy(impulse, impulse + length, spectrum);
    fft(nc).performRealOnlyForwardTransform(spectrum, true);
}

void Hush::synthesise(long long t)
{
    const int nf = cfg_.frame, nc = cfg_.convSize, nb = nc / 2 + 1, len = cfg_.filterLength;
    const bool ms = value(StereoMode) >= 0.5f;
    const long long start = t - 2304LL * factor_; // the newest frame whose output can still be finished in time
    if ((int) synthWindow_.size() < nf) return;
    std::vector<float>& ya = fftB_;
    for (int c = 0; c < 2; ++c)
    {
        const bool identity = identity_[(size_t) c];
        // Frame of the main input in this channel's domain.
        std::fill(ya.begin(), ya.begin() + nc * 2, 0.0f);
        for (int n = 0; n < nf; ++n)
        {
            const int idx = (int) ((start + n) & ringMask_);
            const float l = mainRing_[0][(size_t) idx], r = mainRing_[1][(size_t) idx];
            const float x = ms ? (c == 0 ? 0.5f * (l + r) : 0.5f * (l - r)) : (c == 0 ? l : r);
            ya[(size_t) n] = x * synthWindow_[(size_t) n];
        }
        int span = nf;
        if (identity)
        {
            if (cfg_.linear)
            {
                std::copy_backward(ya.begin(), ya.begin() + nf, ya.begin() + nf + len / 2);
                std::fill(ya.begin(), ya.begin() + len / 2, 0.0f);
                span = nf + len / 2;
            }
        }
        else
        {
            fft(nc).performRealOnlyForwardTransform(ya.data(), true);
            const float* h = spec_[(size_t) c].data();
            const float scale = forwardScale_ * forwardScale_;
            for (int k = 0; k < nb; ++k)
            {
                const float ar = ya[(size_t) k * 2], ai = ya[(size_t) k * 2 + 1], hr = h[k * 2], hi = h[k * 2 + 1];
                ya[(size_t) k * 2] = (ar * hr - ai * hi) * scale;
                ya[(size_t) k * 2 + 1] = (ar * hi + ai * hr) * scale;
            }
            for (int k = nb; k < nc; ++k)
            {
                ya[(size_t) k * 2] = ya[(size_t) (nc - k) * 2];
                ya[(size_t) k * 2 + 1] = -ya[(size_t) (nc - k) * 2 + 1];
            }
            fft(nc).performRealOnlyInverseTransform(ya.data());
            for (int i = 0; i < nc; ++i) ya[(size_t) i] *= inverseScale_;
            span = std::min(nc, nf + len - 1);
        }
        // Back to L/R and into the overlap-add accumulator.
        for (int n = 0; n < span; ++n)
        {
            const int idx = (int) ((start + n) & ringMask_);
            const float v = ya[(size_t) n];
            if (! ms) outRing_[(size_t) c][(size_t) idx] += v;
            else
            {
                outRing_[0][(size_t) idx] += v;
                outRing_[1][(size_t) idx] += c == 0 ? v : -v;
            }
        }
    }
}

void Hush::runHop(long long t)
{
    if (parametersChanged.exchange(false, std::memory_order_acq_rel))
    {
        const Config next = configFor(activeQuality(), value(LinearPhase) >= 0.5f, lowLatencyActive());
        if (! (next == cfg_))
        {
            const bool latencyChanged = next.latency != cfg_.latency;
            cfg_ = next;
            latency_.store(cfg_.latency, std::memory_order_relaxed);
            if (latencyChanged)
                for (auto& ring : outRing_) std::fill(ring.begin(), ring.end(), 0.0f);
        }
        updateDerived();
    }
    analyse(t);
    computeReduction();
    if (! cfg_.lowLatency)
    {
        const bool listening = std::lround(value(BandListen)) > 0;
        for (int c = 0; c < 2; ++c)
        {
            const float* g = gain_[(size_t) c].data();
            identity_[(size_t) c] = flat_[(size_t) c] && ! listening;
            if (listening)
            {
                // Band listen: the band's region of the delta (or the band region solo) instead of the wet signal.
                float* lg = fftB_.data();
                const bool delta = bandListenDelta.load(std::memory_order_relaxed);
                for (int k = 0; k < bins_; ++k) lg[k] = listenRegion_[(size_t) k] * (delta ? 1.0f - g[k] : 1.0f);
                g = lg;
            }
            if (identity_[(size_t) c]) continue;
            if (cfg_.linear) designLinearPhase(g, impulse_.data(), cfg_.filterLength);
            else designMinimumPhase(g, impulse_.data(), cfg_.filterLength);
            spectrumOf(impulse_.data(), cfg_.filterLength, spec_[(size_t) c].data());
        }
    }
    else
    {
        for (int c = 0; c < 2; ++c)
        {
            std::swap(llPrev_[(size_t) c], llCoef_[(size_t) c]);
            if (flat_[(size_t) c])
            {
                std::fill(llCoef_[(size_t) c].begin(), llCoef_[(size_t) c].end(), 0.0f);
                llCoef_[(size_t) c][0] = 1.0f;
            }
            else
                designMinimumPhase(gain_[(size_t) c].data(), llCoef_[(size_t) c].data(), llTaps_);
        }
        llFade_ = cfg_.hop;
    }
}

float Hush::lowLatencyFilter(int channel, float x)
{
    auto& h = llHistory_[(size_t) channel];
    h[(size_t) llPos_] = h[(size_t) (llPos_ + llTaps_)] = x;
    const float* hist = h.data() + llPos_ + 1; // the last `taps` inputs, oldest first: hist[taps - 1] = x
    const float* a = llCoef_[(size_t) channel].data();
    float y = 0.0f;
    for (int j = 0; j < llTaps_; ++j) y += a[j] * hist[llTaps_ - 1 - j];
    if (llFade_ > 0)
    {
        const float* b = llPrev_[(size_t) channel].data();
        float yo = 0.0f;
        for (int j = 0; j < llTaps_; ++j) yo += b[j] * hist[llTaps_ - 1 - j];
        const float t = (float) llFade_ / (float) cfg_.hop;
        y = y * (1.0f - t) + yo * t;
    }
    return y;
}

void Hush::process(juce::AudioBuffer<float>& audio, juce::MidiBuffer&)
{
    const int n = audio.getNumSamples();
    if (n <= 0 || mainRing_[0].empty()) return;
    if (parametersChanged.load(std::memory_order_acquire) && ! primed_) updateDerived();
    primed_ = true;
    float* io[2] = {audio.getWritePointer(0), audio.getWritePointer(std::min(1, audio.getNumChannels() - 1))};
    const bool useSidechain = value(Sidechain) >= 0.5f && sidechain_ != nullptr && sidechain_->getNumChannels() > 0
                           && sidechain_->getNumSamples() >= n;
    const bool listenSidechain = value(SidechainListen) >= 0.5f && sidechain_ != nullptr && sidechain_->getNumSamples() >= n;
    const float* sc[2] = {nullptr, nullptr};
    if (sidechain_ != nullptr && sidechain_->getNumChannels() > 0 && sidechain_->getNumSamples() >= n)
        for (int ch = 0; ch < 2; ++ch) sc[ch] = sidechain_->getReadPointer(std::min(ch, sidechain_->getNumChannels() - 1));
    const bool ms = value(StereoMode) >= 0.5f;
    const bool delta = value(Delta) >= 0.5f;
    const bool listening = std::lround(value(BandListen)) > 0;

    for (int i = 0; i < n; ++i)
    {
        const int w = (int) (count_ & ringMask_);
        const float in[2] = {io[0][i], io[1][i]};
        for (int ch = 0; ch < 2; ++ch)
        {
            mainRing_[(size_t) ch][(size_t) w] = in[ch];
            detRing_[(size_t) ch][(size_t) w] = useSidechain ? sc[ch][i] * sidechainGain_ : in[ch];
        }
        ++count_;
        float wet[2], dry[2];
        if (cfg_.lowLatency)
        {
            if (count_ % cfg_.hop == 0) runHop(count_);
            const float a = ms ? 0.5f * (in[0] + in[1]) : in[0], b = ms ? 0.5f * (in[0] - in[1]) : in[1];
            const float ya = lowLatencyFilter(0, a), yb = lowLatencyFilter(1, b);
            llPos_ = (llPos_ + 1) % llTaps_;
            if (llFade_ > 0) --llFade_;
            wet[0] = ms ? ya + yb : ya;
            wet[1] = ms ? ya - yb : yb;
            dry[0] = in[0];
            dry[1] = in[1];
            if (listening) // band listen without lookahead: show the delta of the whole curve
                for (int ch = 0; ch < 2; ++ch) wet[ch] = dry[ch] - wet[ch];
        }
        else
        {
            if (count_ % cfg_.synthHop == 0)
            {
                if (count_ % cfg_.hop == 0) runHop(count_);
                synthesise(count_);
            }
            const long long readWet = count_ - 1 - (2304 * factor_);
            const long long readDry = count_ - 1 - cfg_.latency;
            const int rw = (int) (readWet & ringMask_), rd = (int) (readDry & ringMask_);
            for (int ch = 0; ch < 2; ++ch)
            {
                wet[ch] = readWet >= 0 ? outRing_[(size_t) ch][(size_t) rw] : 0.0f;
                outRing_[(size_t) ch][(size_t) rw] = 0.0f;
                dry[ch] = readDry >= 0 ? mainRing_[(size_t) ch][(size_t) rd] : 0.0f;
            }
        }
        const float mix = mix_.getNextValue(), wetGain = wetGain_.getNextValue(), outGain = outGain_.getNextValue();
        const float bypass = bypassMix_.getNextValue();
        for (int ch = 0; ch < 2; ++ch)
        {
            float out;
            if (listenSidechain && sc[ch] != nullptr)
            {
                const long long r = count_ - 1 - cfg_.latency;
                out = r >= 0 ? detRing_[(size_t) ch][(size_t) (r & ringMask_)] : 0.0f;
            }
            else if (listening) out = wet[ch] * outGain;
            else
            {
                const float mixed = mix * wet[ch] * wetGain + (1.0f - mix) * dry[ch];
                out = (delta ? dry[ch] - mixed : mixed) * outGain;
            }
            io[ch][i] = bypass > 0.0f ? out * (1.0f - bypass) + dry[ch] * bypass : out;
        }
    }
}

} // namespace sonora::devices
