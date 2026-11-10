#include "dsp/SoundFeatures.h"

#include "dsp/Fft.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <limits>
#include <vector>

namespace sonora::dsp
{

namespace
{
constexpr float kPi = 3.14159265358979f;

float clamp01(float v) { return std::isfinite(v) ? std::clamp(v, 0.0f, 1.0f) : 0.0f; }
// 20 Hz .. 20 kHz on a log scale -> 0..1
float logFrequency(float hz) { return clamp01(std::log2(std::max(hz, 20.0f) / 20.0f) / 10.0f); }
float decibels(float power) { return 10.0f * std::log10(std::max(power, 1e-12f)); }

// Weights of each value in the distance (a feature group's total matters, not a single band).
constexpr std::array<float, SoundFeatures::Count> makeWeights()
{
    std::array<float, SoundFeatures::Count> w{};
    w[SoundFeatures::Duration] = 1.6f;
    w[SoundFeatures::Centroid] = 3.0f;
    w[SoundFeatures::Spread] = 1.5f;
    w[SoundFeatures::Flatness] = 2.5f;
    w[SoundFeatures::Rolloff] = 2.0f;
    w[SoundFeatures::CentroidVariation] = 1.0f;
    w[SoundFeatures::ZeroCrossings] = 1.5f;
    w[SoundFeatures::Attack] = 1.5f;
    w[SoundFeatures::TemporalCentre] = 1.5f;
    w[SoundFeatures::Onsets] = 1.5f;
    w[SoundFeatures::Loudness] = 0.3f;
    for (int i = 0; i < SoundFeatures::kBands; ++i) w[(size_t) (SoundFeatures::FirstBand + i)] = 0.7f;
    for (int i = 0; i < SoundFeatures::kEnvelopePoints; ++i) w[(size_t) (SoundFeatures::FirstEnvelope + i)] = 1.0f;
    return w;
}
constexpr auto kWeights = makeWeights();
} // namespace

SoundFeatures extractSoundFeatures(const float* mono, int64_t frames, double sampleRate, double fileSeconds)
{
    SoundFeatures result;
    if (mono == nullptr || frames <= 0 || ! (sampleRate > 0)) return result;
    const int64_t analysed = std::min<int64_t>(frames, (int64_t) std::ceil(kSoundFeatureSeconds * sampleRate));
    const int sizeLog2 = sampleRate >= 32000 ? 11 : 10;
    const Fft fft(sizeLog2);
    const int size = fft.size(), hop = size / 4, bins = size / 2;
    const float binHz = (float) (sampleRate / size);
    std::vector<float> window((size_t) size);
    for (int i = 0; i < size; ++i) window[(size_t) i] = 0.5f - 0.5f * std::cos(2.0f * kPi * (float) i / (float) size);

    // Band edges (log-spaced 30 Hz .. 16 kHz) as bin ranges.
    std::array<int, SoundFeatures::kBands + 1> edges{};
    for (int b = 0; b <= SoundFeatures::kBands; ++b)
    {
        const float hz = 30.0f * std::pow(16000.0f / 30.0f, (float) b / (float) SoundFeatures::kBands);
        edges[(size_t) b] = std::clamp((int) std::lround(hz / binHz), 1, bins);
    }

    struct Frame
    {
        float rms = 0, power = 0, centroid = 0, spread = 0, flatness = 0, rolloff = 0, flux = 0;
        std::array<float, SoundFeatures::kBands> bands{};
    };
    std::vector<Frame> frameData;
    std::vector<std::complex<float>> buffer((size_t) size);
    std::vector<float> spectrum((size_t) bins + 1), previous((size_t) bins + 1, 0.0f);
    int64_t crossings = 0;
    for (int64_t i = 1; i < analysed; ++i)
        crossings += (mono[i - 1] >= 0.0f) != (mono[i] >= 0.0f);
    // Frames start at 0 and hop until the analysed audio ends; a short file is zero-padded into one frame.
    for (int64_t start = 0; start < analysed; start += hop)
    {
        Frame frame;
        double energy = 0;
        for (int i = 0; i < size; ++i)
        {
            const int64_t at = start + i;
            const float s = at < analysed ? mono[at] : 0.0f;
            energy += (double) s * s;
            buffer[(size_t) i] = {s * window[(size_t) i], 0.0f};
        }
        frame.rms = (float) std::sqrt(energy / size);
        fft.transform(buffer);
        double total = 0, weighted = 0, logSum = 0, flux = 0;
        for (int k = 1; k <= bins; ++k)
        {
            const float p = std::norm(buffer[(size_t) k]);
            spectrum[(size_t) k] = p;
            total += p;
            weighted += (double) p * (double) k * binHz;
            logSum += std::log((double) p + 1e-12);
            const float magnitude = std::sqrt(p), before = std::sqrt(previous[(size_t) k]);
            if (magnitude > before) flux += magnitude - before;
        }
        std::swap(spectrum, previous);
        const auto& p = previous; // this frame's spectrum
        frame.power = (float) total;
        frame.flux = (float) flux;
        if (total > 1e-12)
        {
            const double centroid = weighted / total;
            double variance = 0, cumulative = 0;
            int rolloffBin = bins;
            for (int k = 1; k <= bins; ++k)
            {
                const double d = (double) k * binHz - centroid;
                variance += p[(size_t) k] * d * d;
            }
            for (int k = 1; k <= bins; ++k)
            {
                cumulative += p[(size_t) k];
                if (cumulative >= 0.85 * total) { rolloffBin = k; break; }
            }
            frame.centroid = (float) centroid;
            frame.spread = (float) std::sqrt(variance / total);
            frame.flatness = (float) (std::exp(logSum / bins) / (total / bins));
            frame.rolloff = (float) rolloffBin * binHz;
            for (int b = 0; b < SoundFeatures::kBands; ++b)
            {
                double band = 0;
                for (int k = edges[(size_t) b]; k < std::max(edges[(size_t) b] + 1, edges[(size_t) b + 1]); ++k) band += p[(size_t) k];
                frame.bands[(size_t) b] = (float) band;
            }
        }
        frameData.push_back(frame);
    }
    if (frameData.empty()) return result;
    const float hopSeconds = (float) (hop / sampleRate);
    size_t peakFrame = 0;
    for (size_t i = 0; i < frameData.size(); ++i)
        if (frameData[i].rms > frameData[peakFrame].rms) peakFrame = i;
    const float peakRms = frameData[peakFrame].rms;
    if (! (peakRms > 1e-5f)) return result; // silence

    // Spectral values, power-weighted over the frames within 50 dB of the loudest one.
    const float floor = peakRms * std::pow(10.0f, -50.0f / 20.0f);
    double weightSum = 0, centroid = 0, spread = 0, flatness = 0, rolloff = 0, logCentroidSum = 0, logCentroidSquares = 0;
    std::array<double, SoundFeatures::kBands> bands{};
    for (const auto& frame : frameData)
    {
        if (frame.rms < floor || frame.power <= 0) continue;
        const double w = frame.rms * frame.rms;
        weightSum += w;
        centroid += w * frame.centroid;
        spread += w * frame.spread;
        flatness += w * frame.flatness;
        rolloff += w * frame.rolloff;
        const double logCentroid = logFrequency(frame.centroid);
        logCentroidSum += w * logCentroid;
        logCentroidSquares += w * logCentroid * logCentroid;
        for (int b = 0; b < SoundFeatures::kBands; ++b) bands[(size_t) b] += w * frame.bands[(size_t) b] / frame.power;
    }
    if (weightSum <= 0) return result;
    auto& v = result.values;
    v[SoundFeatures::Centroid] = logFrequency((float) (centroid / weightSum));
    v[SoundFeatures::Spread] = clamp01(std::log2(1.0f + (float) (spread / weightSum) / 20.0f) / 10.0f);
    v[SoundFeatures::Flatness] = clamp01(std::sqrt((float) (flatness / weightSum)));
    v[SoundFeatures::Rolloff] = logFrequency((float) (rolloff / weightSum));
    const double meanLog = logCentroidSum / weightSum;
    v[SoundFeatures::CentroidVariation] = clamp01((float) std::sqrt(std::max(0.0, logCentroidSquares / weightSum - meanLog * meanLog)) * 5.0f);
    v[SoundFeatures::ZeroCrossings] = clamp01(std::sqrt((float) crossings / (float) analysed * 4.0f));
    // Band shape: each band's share of the energy in dB, 60 dB of range.
    for (int b = 0; b < SoundFeatures::kBands; ++b)
        v[(size_t) (SoundFeatures::FirstBand + b)] = clamp01((decibels((float) (bands[(size_t) b] / weightSum)) + 60.0f) / 60.0f);

    // Envelope: attack (first frame within 40 dB of the peak -> the peak), decay after the peak, energy centre.
    size_t onset = 0;
    const float attackFloor = peakRms * 0.01f;
    while (onset < peakFrame && frameData[onset].rms < attackFloor) ++onset;
    const float attackMs = (float) (peakFrame - onset) * hopSeconds * 1000.0f;
    v[SoundFeatures::Attack] = clamp01(std::log10(1.0f + attackMs) / 3.5f);
    constexpr float kEnvelopeMs[SoundFeatures::kEnvelopePoints] = {10, 25, 50, 100, 200, 400, 800, 1600};
    const float audioSeconds = (float) std::min(fileSeconds > 0 ? fileSeconds : (double) frames / sampleRate, (double) frames / sampleRate);
    for (int i = 0; i < SoundFeatures::kEnvelopePoints; ++i)
    {
        const float t = (float) peakFrame * hopSeconds + kEnvelopeMs[i] / 1000.0f;
        const auto at = (size_t) std::lround(t / hopSeconds);
        float level = 0;
        if (t < audioSeconds && at < frameData.size())
            level = clamp01((20.0f * std::log10(std::max(frameData[at].rms, 1e-9f) / peakRms) + 60.0f) / 60.0f);
        else if (t < audioSeconds) // past the analysed seconds: assume the last level holds (pads, loops)
            level = clamp01((20.0f * std::log10(std::max(frameData.back().rms, 1e-9f) / peakRms) + 60.0f) / 60.0f);
        v[(size_t) (SoundFeatures::FirstEnvelope + i)] = level;
    }
    double energySum = 0, energyTime = 0;
    for (size_t i = 0; i < frameData.size(); ++i)
    {
        const double e = (double) frameData[i].rms * frameData[i].rms;
        energySum += e;
        energyTime += e * (double) i * hopSeconds;
    }
    v[SoundFeatures::TemporalCentre] = clamp01(std::log2(1.0f + (float) (energyTime / std::max(energySum, 1e-18)) * 10.0f) / 5.0f);

    // Onsets: spectral-flux peaks above an adaptive threshold, at least 50 ms apart, per second of analysed audio.
    int onsets = 0;
    size_t last = 0;
    bool any = false;
    const size_t span = 6;
    for (size_t i = 1; i + 1 < frameData.size(); ++i)
    {
        double local = 0;
        size_t n = 0;
        for (size_t j = i > span ? i - span : 0; j < std::min(frameData.size(), i + span + 1); ++j) { local += frameData[j].flux; ++n; }
        const float threshold = (float) (local / (double) n) * 1.5f + 1e-6f;
        const auto& f = frameData[i];
        if (f.rms < floor * 10.0f || f.flux < threshold || f.flux < frameData[i - 1].flux || f.flux < frameData[i + 1].flux) continue;
        if (any && (float) (i - last) * hopSeconds < 0.05f) continue;
        ++onsets;
        last = i;
        any = true;
    }
    const float analysedSeconds = (float) ((double) analysed / sampleRate);
    v[SoundFeatures::Onsets] = clamp01((float) onsets / std::max(0.25f, analysedSeconds) / 8.0f);
    v[SoundFeatures::Loudness] = clamp01((20.0f * std::log10(peakRms) + 60.0f) / 60.0f);
    const double duration = fileSeconds > 0 ? fileSeconds : (double) frames / sampleRate;
    v[SoundFeatures::Duration] = clamp01((float) std::log2(std::max(duration, 0.02) / 0.02) / 12.0f);
    result.valid = true;
    return result;
}

float soundDistance(const SoundFeatures& a, const SoundFeatures& b)
{
    if (! a.valid || ! b.valid) return std::numeric_limits<float>::infinity();
    float sum = 0;
    for (size_t i = 0; i < a.values.size(); ++i)
    {
        const float d = a.values[i] - b.values[i];
        sum += kWeights[i] * d * d;
    }
    return std::sqrt(sum);
}

} // namespace sonora::dsp
