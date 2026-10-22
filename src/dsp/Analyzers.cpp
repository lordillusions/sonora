#include "dsp/Analyzers.h"

#include "dsp/Dsp.h"

#include <algorithm>
#include <cmath>

namespace sonora::dsp
{

// ---------------------------------------------------------------- StereoMeter

void StereoMeter::prepare(double sampleRate)
{
    decay_ = std::exp(-1.0 / (0.3 * std::max(8000.0, sampleRate)));
    decimate_ = std::max(1, (int) std::lround(sampleRate / 12000.0));
    reset();
}

void StereoMeter::reset()
{
    ll_ = rr_ = lr_ = mm_ = ss_ = 0.0;
    points_.fill({0.0f, 0.0f});
    pointPos_ = decimateCount_ = 0;
}

void StereoMeter::process(const float* left, const float* right, int numSamples)
{
    constexpr float kRoot2 = 0.70710678f;
    for (int i = 0; i < numSamples; ++i)
    {
        const double l = left[i], r = right != nullptr ? right[i] : left[i];
        ll_ = ll_ * decay_ + l * l;
        rr_ = rr_ * decay_ + r * r;
        lr_ = lr_ * decay_ + l * r;
        const double m = (l + r) * 0.5, s = (l - r) * 0.5;
        mm_ = mm_ * decay_ + m * m;
        ss_ = ss_ * decay_ + s * s;
        if (++decimateCount_ >= decimate_)
        {
            decimateCount_ = 0;
            points_[(size_t) pointPos_] = {(float) (l - r) * kRoot2, (float) (l + r) * kRoot2};
            pointPos_ = (pointPos_ + 1) % kGoniometerPoints;
        }
    }
}

float StereoMeter::correlation() const
{
    const double denom = std::sqrt(ll_ * rr_);
    return denom < 1e-9 ? 0.0f : (float) std::clamp(lr_ / denom, -1.0, 1.0);
}

float StereoMeter::balance() const
{
    const double l = std::sqrt(ll_), r = std::sqrt(rr_);
    return l + r < 1e-6 ? 0.0f : (float) ((r - l) / (r + l));
}

float StereoMeter::width() const
{
    return mm_ + ss_ < 1e-12 ? 0.0f : (float) (ss_ / (mm_ + ss_));
}

void StereoMeter::goniometer(std::array<std::pair<float, float>, kGoniometerPoints>& out) const
{
    for (int i = 0; i < kGoniometerPoints; ++i)
        out[(size_t) i] = points_[(size_t) ((pointPos_ + i) % kGoniometerPoints)];
}

// ---------------------------------------------------------------- SpectrumAnalyzer

namespace
{
constexpr double kLowHz = 20.0, kHighHz = 20000.0;
double logFrequency(double position) { return kLowHz * std::pow(kHighHz / kLowHz, position); } // position 0..1
} // namespace

float SpectrumAnalyzer::barFrequency(int bar)
{
    return (float) logFrequency(((double) bar + 0.5) / kBars);
}

float SpectrumAnalyzer::curveFrequency(int point)
{
    return (float) logFrequency((double) point / (kCurvePoints - 1));
}

SpectrumAnalyzer::SpectrumAnalyzer(double sampleRate)
    : sampleRate_(sampleRate), fft_(kFftLog2), window_((size_t) kFftSize), ring_((size_t) kFftSize, 0.0f),
      work_((size_t) kFftSize), db_((size_t) kFftSize / 2 + 1, kFloorDb)
{
    for (int i = 0; i < kFftSize; ++i)
        window_[(size_t) i] = 0.5f - 0.5f * std::cos(2.0f * (float) kPi * (float) i / (float) kFftSize);
    prepare(sampleRate);
}

void SpectrumAnalyzer::prepare(double sampleRate)
{
    sampleRate_ = std::max(8000.0, sampleRate);
    reset();
}

void SpectrumAnalyzer::reset()
{
    std::fill(ring_.begin(), ring_.end(), 0.0f);
    std::fill(db_.begin(), db_.end(), kFloorDb);
    bars_.fill(kFloorDb);
    barPeaks_.fill(kFloorDb);
    curve_.fill(kFloorDb);
    peakAge_.fill(0.0);
    ringPos_ = 0;
}

void SpectrumAnalyzer::push(const float* left, const float* right, int numSamples)
{
    for (int i = 0; i < numSamples; ++i)
    {
        ring_[(size_t) ringPos_] = 0.5f * (left[i] + (right != nullptr ? right[i] : left[i]));
        ringPos_ = (ringPos_ + 1) % kFftSize;
    }
}

float SpectrumAnalyzer::magnitudeAt(double hz) const
{
    const double position = hz / (sampleRate_ / kFftSize);
    if (position < 0.0 || position >= (double) db_.size() - 1)
        return kFloorDb;
    const size_t i = (size_t) position;
    const float f = (float) (position - (double) i);
    return db_[i] + (db_[i + 1] - db_[i]) * f;
}

void SpectrumAnalyzer::analyze(double seconds)
{
    for (int i = 0; i < kFftSize; ++i)
        work_[(size_t) i] = {ring_[(size_t) ((ringPos_ + i) % kFftSize)] * window_[(size_t) i], 0.0f};
    fft_.transform(work_);
    const float scale = 4.0f / (float) kFftSize; // a full scale sine on a bin reads 1.0 with a Hann window
    for (size_t k = 0; k < db_.size(); ++k)
    {
        const float amp = std::abs(work_[k]) * scale;
        db_[k] = amp > 1e-5f ? std::max(kFloorDb, 20.0f * std::log10(amp)) : kFloorDb;
    }

    const float release = (float) (36.0 * seconds), peakFall = (float) (18.0 * seconds);
    const double nyquist = sampleRate_ * 0.5;
    for (int b = 0; b < kBars; ++b)
    {
        const double lo = logFrequency((double) b / kBars), hi = logFrequency((double) (b + 1) / kBars);
        float level = kFloorDb;
        if (lo < nyquist)
        {
            const int k0 = (int) std::ceil(lo / (sampleRate_ / kFftSize)), k1 = (int) std::floor(hi / (sampleRate_ / kFftSize));
            for (int k = std::max(1, k0); k <= std::min(k1, kFftSize / 2); ++k)
                level = std::max(level, db_[(size_t) k]);
            if (k1 < k0) // band narrower than a bin: interpolate at its centre
                level = magnitudeAt(barFrequency(b));
        }
        float& bar = bars_[(size_t) b];
        bar = std::max(level, bar - release);
        float& peak = barPeaks_[(size_t) b];
        double& age = peakAge_[(size_t) b];
        if (bar >= peak)
        {
            peak = bar;
            age = 0.0;
        }
        else if ((age += seconds) > 1.0)
            peak = std::max(bar, peak - peakFall);
    }

    for (int p = 0; p < kCurvePoints; ++p)
    {
        const double f = curveFrequency(p);
        double power = 0.0;
        int n = 0;
        for (int j = -2; j <= 2; ++j) // +-1/12 octave
        {
            const float v = magnitudeAt(f * std::pow(2.0, (double) j / 24.0));
            power += std::pow(10.0, (double) v / 10.0);
            ++n;
        }
        const float level = std::max(kFloorDb, (float) (10.0 * std::log10(std::max(1e-12, power / n))));
        float& c = curve_[(size_t) p];
        c = level > c ? c + (level - c) * 0.6f : std::max(level, c - release);
    }
}

// ---------------------------------------------------------------- Scope

void Scope::prepare(double sampleRate)
{
    sampleRate_ = std::max(8000.0, sampleRate);
    l_.assign((size_t) (sampleRate_ * 0.5), 0.0f);
    r_.assign(l_.size(), 0.0f);
    pos_ = 0;
}

void Scope::reset()
{
    std::fill(l_.begin(), l_.end(), 0.0f);
    std::fill(r_.begin(), r_.end(), 0.0f);
    pos_ = 0;
}

void Scope::push(const float* left, const float* right, int numSamples)
{
    const int size = (int) l_.size();
    for (int i = 0; i < numSamples; ++i)
    {
        l_[(size_t) pos_] = left[i];
        r_[(size_t) pos_] = right != nullptr ? right[i] : left[i];
        pos_ = (pos_ + 1) % size;
    }
}

void Scope::trace(int columns, double windowMs, bool triggered, std::vector<Column>& out) const
{
    out.assign((size_t) std::max(0, columns), Column{0, 0, 0, 0});
    if (columns <= 0 || l_.empty())
        return;
    const int size = (int) l_.size();
    const int window = std::clamp((int) std::lround(windowMs * 0.001 * sampleRate_), columns, size / 2);
    auto at = [&](const std::vector<float>& v, int offsetFromWrite) { return v[(size_t) (((pos_ + offsetFromWrite) % size + size) % size)]; };

    int start = -window; // offset from the write position of the first sample shown
    if (triggered)
    {
        const int searchFrom = -window - std::min(window, size - window - 2);
        for (int o = searchFrom + 1; o <= -window; ++o)
            if (at(l_, o - 1) <= 0.0f && at(l_, o) > 0.0f)
            {
                start = o;
                break;
            }
    }
    for (int c = 0; c < columns; ++c)
    {
        const int a = start + (int) ((long long) c * window / columns);
        const int b = std::max(a + 1, start + (int) ((long long) (c + 1) * window / columns));
        Column col{1e9f, -1e9f, 1e9f, -1e9f};
        for (int o = a; o < b && o < 0; ++o)
        {
            const float lv = at(l_, o), rv = at(r_, o);
            col.minL = std::min(col.minL, lv);
            col.maxL = std::max(col.maxL, lv);
            col.minR = std::min(col.minR, rv);
            col.maxR = std::max(col.maxR, rv);
        }
        if (col.minL > col.maxL) col = Column{0, 0, 0, 0};
        out[(size_t) c] = col;
    }
}

} // namespace sonora::dsp
