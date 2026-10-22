#include "dsp/Loudness.h"

#include "dsp/Dsp.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace sonora::dsp
{

namespace
{
constexpr double kAbsoluteGate = -70.0;

double lufsToPower(double lufs) { return std::pow(10.0, (lufs + 0.691) / 10.0); }

double meanPower(const std::vector<float>& powers, double minimum)
{
    double sum = 0;
    size_t n = 0;
    for (float p : powers)
        if (p > minimum)
        {
            sum += p;
            ++n;
        }
    return n > 0 ? sum / (double) n : 0.0;
}
} // namespace

float LoudnessMeter::powerToLufs(double power)
{
    return power > 1e-12 ? (float) (-0.691 + 10.0 * std::log10(power)) : kSilenceDb;
}

void LoudnessMeter::prepare(double sampleRate)
{
    sampleRate_ = std::max(8000.0, sampleRate);
    hopSamples_ = std::max(1, (int) std::lround(sampleRate_ * 0.1));
    design();
    reset();
}

void LoudnessMeter::design()
{
    // Stage 1: high shelf (head acoustics), stage 2: RLB high-pass. Coefficients for any rate (BS.1770-4 / libebur128).
    {
        const double f0 = 1681.974450955533, g = 3.999843853973347, q = 0.7071752369554196;
        const double k = std::tan(kPi * f0 / sampleRate_);
        const double vh = std::pow(10.0, g / 20.0), vb = std::pow(vh, 0.4996667741545416);
        const double a0 = 1.0 + k / q + k * k;
        Biquad s;
        s.b0 = (vh + vb * k / q + k * k) / a0;
        s.b1 = 2.0 * (k * k - vh) / a0;
        s.b2 = (vh - vb * k / q + k * k) / a0;
        s.a1 = 2.0 * (k * k - 1.0) / a0;
        s.a2 = (1.0 - k / q + k * k) / a0;
        for (auto& channel : filters_)
            channel[0] = s;
    }
    {
        const double f0 = 38.13547087602444, q = 0.5003270373238773;
        const double k = std::tan(kPi * f0 / sampleRate_);
        const double a0 = 1.0 + k / q + k * k;
        Biquad s;
        s.b0 = 1.0;
        s.b1 = -2.0;
        s.b2 = 1.0;
        s.a1 = 2.0 * (k * k - 1.0) / a0;
        s.a2 = (1.0 - k / q + k * k) / a0;
        for (auto& channel : filters_)
            channel[1] = s;
    }

    // 4x interpolation filter: Hann-windowed sinc, 48 taps (12 per phase), unity DC gain in every phase.
    std::array<double, kTaps> h{};
    const double centre = (kTaps - 1) * 0.5;
    for (int n = 0; n < kTaps; ++n)
    {
        const double x = ((double) n - centre) / kPhases;
        const double sinc = std::abs(x) < 1e-9 ? 1.0 : std::sin(kPi * x) / (kPi * x);
        const double window = 0.5 - 0.5 * std::cos(2.0 * kPi * (double) (n + 1) / (kTaps + 1));
        h[(size_t) n] = sinc * window;
    }
    for (int p = 0; p < kPhases; ++p)
    {
        double sum = 0;
        for (int k = 0; k < kPerPhase; ++k)
            sum += h[(size_t) (p + k * kPhases)];
        for (int k = 0; k < kPerPhase; ++k)
            firPhases_[(size_t) (p * kPerPhase + k)] = (float) (h[(size_t) (p + k * kPhases)] / (sum != 0.0 ? sum : 1.0));
    }
}

void LoudnessMeter::reset()
{
    for (auto& channel : filters_)
        for (auto& stage : channel)
            stage.z1 = stage.z2 = 0.0;
    recent_.fill(0.0);
    recentCount_ = recentPos_ = hopFill_ = 0;
    hopSum_ = 0.0;
    blockPowers_.clear();
    shortTermPowers_.clear();
    if (! realtime_)
    {
        blockPowers_.reserve(4096);
        shortTermPowers_.reserve(4096);
    }
    histogramPower_.fill(0.0);
    histogramCount_.fill(0);
    hopSumChannel_.fill(0.0);
    for (auto& channel : recentChannel_)
        channel.fill(0.0);
    hops_ = 0;
    if (realtime_)
        recentCount_ = (int) recent_.size(); // windows start full of silence
    momentary_ = shortTerm_ = momentaryMax_ = kSilenceDb;
    momentaryChannel_.fill(kSilenceDb);
    samplePeak_ = truePeak_ = 0.0f;
    for (auto& channel : history_)
        channel.fill(0.0f);
    historyPos_ = 0;
}

void LoudnessMeter::trackTruePeak(int channel, float x)
{
    auto& hist = history_[(size_t) channel];
    hist[(size_t) historyPos_] = x;
    for (int p = 0; p < kPhases; ++p)
    {
        float y = 0.0f;
        const float* taps = firPhases_.data() + p * kPerPhase;
        for (int k = 0; k < kPerPhase; ++k)
            y += taps[k] * hist[(size_t) ((historyPos_ - k + kPerPhase) % kPerPhase)];
        truePeak_ = std::max(truePeak_, std::abs(y));
    }
}

void LoudnessMeter::process(const float* left, const float* right, int numSamples)
{
    for (int i = 0; i < numSamples; ++i)
    {
        const float l = left[i], r = right != nullptr ? right[i] : left[i];
        samplePeak_ = std::max({samplePeak_, std::abs(l), std::abs(r)});
        trackTruePeak(0, l);
        trackTruePeak(1, r);
        historyPos_ = (historyPos_ + 1) % kPerPhase;

        const double wl = filters_[0][1].run(filters_[0][0].run((double) l));
        const double wr = filters_[1][1].run(filters_[1][0].run((double) r));
        hopSumChannel_[0] += wl * wl;
        hopSumChannel_[1] += wr * wr;
        hopSum_ += wl * wl + wr * wr; // channel weights are 1.0 for L and R
        if (++hopFill_ >= hopSamples_)
            closeBlock();
    }
}

void LoudnessMeter::closeBlock()
{
    recent_[(size_t) recentPos_] = hopSum_ / (double) hopSamples_;
    recentPos_ = (recentPos_ + 1) % (int) recent_.size();
    recentCount_ = std::min(recentCount_ + 1, (int) recent_.size());
    hopSum_ = 0.0;
    hopFill_ = 0;
    ++hops_;
    for (size_t ch = 0; ch < 2; ++ch)
    {
        auto& window = recentChannel_[ch];
        window[(size_t) (hops_ % window.size())] = hopSumChannel_[ch] / (double) hopSamples_;
        hopSumChannel_[ch] = 0.0;
        if (recentCount_ >= 4)
            momentaryChannel_[ch] = powerToLufs((window[0] + window[1] + window[2] + window[3]) / 4.0);
    }

    auto meanOfLast = [this](int count) {
        double sum = 0;
        for (int k = 1; k <= count; ++k)
            sum += recent_[(size_t) ((recentPos_ - k + (int) recent_.size()) % (int) recent_.size())];
        return sum / count;
    };
    if (recentCount_ >= 4)
    {
        const double power = meanOfLast(4);
        momentary_ = powerToLufs(power);
        momentaryMax_ = std::max(momentaryMax_, momentary_);
        if (realtime_)
        {
            if (momentary_ > (float) kAbsoluteGate)
            {
                const int bin = std::min(kHistogramBins - 1, (int) ((momentary_ - (float) kAbsoluteGate) / kHistogramStep));
                histogramPower_[(size_t) bin] += power;
                ++histogramCount_[(size_t) bin];
            }
        }
        else
            blockPowers_.push_back((float) power);
    }
    if (recentCount_ >= 30)
    {
        const double power = meanOfLast(30);
        shortTerm_ = powerToLufs(power);
        if (! realtime_)
            shortTermPowers_.push_back((float) power);
    }
}

float LoudnessMeter::integrated() const
{
    if (realtime_)
    {
        // Same two gates as below, applied per 0.05 LU bin (a bin straddling the relative gate counts as above it).
        double sum = 0;
        uint64_t count = 0;
        for (int bin = 0; bin < kHistogramBins; ++bin)
        {
            sum += histogramPower_[(size_t) bin];
            count += histogramCount_[(size_t) bin];
        }
        if (count == 0)
            return kSilenceDb;
        const float relativeLufs = powerToLufs(sum / (double) count * 0.1);
        const int first = std::clamp((int) ((relativeLufs - (float) kAbsoluteGate) / kHistogramStep), 0, kHistogramBins - 1);
        double gatedSum = 0;
        uint64_t gatedCount = 0;
        for (int bin = first; bin < kHistogramBins; ++bin)
        {
            gatedSum += histogramPower_[(size_t) bin];
            gatedCount += histogramCount_[(size_t) bin];
        }
        return gatedCount > 0 ? powerToLufs(gatedSum / (double) gatedCount) : kSilenceDb;
    }
    const double absolute = lufsToPower(kAbsoluteGate);
    const double ungated = meanPower(blockPowers_, absolute);
    if (ungated <= 0.0)
        return kSilenceDb;
    const double relative = ungated * 0.1; // -10 LU
    const double gated = meanPower(blockPowers_, std::max(absolute, relative));
    return gated > 0.0 ? powerToLufs(gated) : kSilenceDb;
}

float LoudnessMeter::loudnessRange() const
{
    if (realtime_)
        return 0.0f;
    const double absolute = lufsToPower(kAbsoluteGate);
    const double ungated = meanPower(shortTermPowers_, absolute);
    if (ungated <= 0.0)
        return 0.0f;
    const double relative = ungated * 0.01; // -20 LU
    std::vector<float> kept;
    for (float p : shortTermPowers_)
        if (p > absolute && p > relative)
            kept.push_back(powerToLufs(p));
    if (kept.size() < 2)
        return 0.0f;
    std::sort(kept.begin(), kept.end());
    auto percentile = [&kept](double q) {
        const double pos = q * (double) (kept.size() - 1);
        const size_t lo = (size_t) pos;
        const size_t hi = std::min(kept.size() - 1, lo + 1);
        return kept[lo] + (kept[hi] - kept[lo]) * (float) (pos - (double) lo);
    };
    return std::max(0.0f, percentile(0.95) - percentile(0.10));
}

float LoudnessMeter::samplePeakDb() const
{
    return samplePeak_ > 1e-10f ? 20.0f * std::log10(samplePeak_) : kSilenceDb;
}

float LoudnessMeter::truePeakDb() const
{
    return truePeak_ > 1e-10f ? 20.0f * std::log10(std::max(truePeak_, samplePeak_)) : kSilenceDb;
}

} // namespace sonora::dsp
