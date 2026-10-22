#include "engine/TempoAnalysis.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>
#include <vector>

namespace sonora::engine
{
namespace
{
constexpr int kFrame = 512, kHop = 128;   // at ~11 kHz: 46 ms windows, 86 envelope frames per second
constexpr double kAnalysisRate = 11025;
constexpr double kSearchMin = 40, kSearchMax = 240; // comb tempogram range before octave folding
constexpr double kMinConfidence = 0.12, kWeakConfidence = 0.3;
constexpr double kCombSeconds = 4; // lag window of the comb tempogram

void fft(std::vector<std::complex<float>>& data, const std::vector<std::complex<float>>& twiddles)
{
    const size_t n = data.size();
    for (size_t i = 1, j = 0; i < n; ++i)
    {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(data[i], data[j]);
    }
    for (size_t length = 2; length <= n; length <<= 1)
        for (size_t i = 0; i < n; i += length)
            for (size_t k = 0; k < length / 2; ++k)
            {
                const auto odd = data[i + k + length / 2] * twiddles[k * (n / length)];
                data[i + k + length / 2] = data[i + k] - odd;
                data[i + k] += odd;
            }
}

struct Envelope
{
    std::vector<float> values; // log spectral flux, local mean removed, rectified
    double framesPerSecond = 0;
    double startSeconds = 0;   // file time of frame 0's onset position
};

// Onset strength of (at most kMaxAnalysisSeconds from the middle of) the sample, mixed to mono at ~11 kHz.
Envelope onsetEnvelope(const juce::AudioBuffer<float>& audio, double sampleRate)
{
    Envelope result;
    const int channels = audio.getNumChannels();
    const int64_t total = audio.getNumSamples();
    if (channels <= 0 || total <= 0) return result;
    const int64_t span = std::min<int64_t>(total, (int64_t) (kMaxAnalysisSeconds * sampleRate));
    const int64_t start = (total - span) / 2;
    const int decimation = std::max(1, (int) std::lround(sampleRate / kAnalysisRate));
    const double rate = sampleRate / decimation;
    std::vector<float> mono((size_t) (span / decimation));
    float peak = 0;
    for (size_t i = 0; i < mono.size(); ++i)
    {
        double sum = 0;
        for (int channel = 0; channel < channels; ++channel)
        {
            const float* samples = audio.getReadPointer(channel, (int) (start + (int64_t) i * decimation));
            for (int d = 0; d < decimation; ++d) sum += samples[d];
        }
        mono[i] = std::isfinite(sum) ? (float) (sum / (channels * decimation)) : 0.0f;
        peak = std::max(peak, std::abs(mono[i]));
    }
    if (peak < 1e-5f || mono.size() < (size_t) kFrame * 2) return result;
    for (auto& sample : mono) sample /= peak;

    std::vector<std::complex<float>> twiddles(kFrame / 2), bins(kFrame);
    for (int k = 0; k < kFrame / 2; ++k)
        twiddles[(size_t) k] = std::polar(1.0f, (float) (-2 * std::numbers::pi * k / kFrame));
    std::vector<float> window(kFrame), previous(kFrame / 2 + 1, 0.0f), current(kFrame / 2 + 1);
    for (int i = 0; i < kFrame; ++i) window[(size_t) i] = (float) (0.5 - 0.5 * std::cos(2 * std::numbers::pi * i / kFrame));
    const size_t frames = 1 + (mono.size() - kFrame) / kHop;
    std::vector<float> flux(frames, 0.0f);
    for (size_t frame = 0; frame < frames; ++frame)
    {
        for (int i = 0; i < kFrame; ++i) bins[(size_t) i] = mono[frame * kHop + (size_t) i] * window[(size_t) i];
        fft(bins, twiddles);
        float sum = 0;
        for (size_t bin = 1; bin < current.size(); ++bin)
        {
            current[bin] = std::log1p(20.0f * std::abs(bins[bin]));
            if (frame > 0) sum += std::max(0.0f, current[bin] - previous[bin]);
        }
        flux[frame] = sum;
        std::swap(current, previous);
    }
    // Remove the local mean (~0.5 s) so sustained loudness changes do not look like beats.
    result.framesPerSecond = rate / kHop;
    const int radius = std::max(2, (int) std::lround(0.25 * result.framesPerSecond));
    std::vector<double> prefix(frames + 1, 0.0);
    for (size_t i = 0; i < frames; ++i) prefix[i + 1] = prefix[i] + flux[i];
    result.values.resize(frames);
    float strongest = 0;
    for (size_t i = 0; i < frames; ++i)
    {
        const size_t lo = i >= (size_t) radius ? i - (size_t) radius : 0, hi = std::min(frames, i + (size_t) radius + 1);
        result.values[i] = std::max(0.0f, flux[i] - (float) ((prefix[hi] - prefix[lo]) / (double) (hi - lo)));
        strongest = std::max(strongest, result.values[i]);
    }
    // Window/phase beating of steady tones (flux < 0.5 on the peak-normalised input; real attacks reach 10-500) and
    // anything far below the strongest attack is not rhythm.
    const float floor = std::max(0.5f, 0.01f * strongest);
    for (auto& value : result.values)
        if (value < floor) value = 0;
    // A frame's flux peaks when an attack reaches the middle of its window.
    result.startSeconds = (double) start / sampleRate + (kFrame / 2.0 - kHop / 2.0) / rate;
    return result;
}

double interpolate(const std::vector<float>& values, double position)
{
    if (position < 0) return 0;
    const auto index = (size_t) position;
    if (index + 1 >= values.size()) return index < values.size() ? values[index] : 0;
    const double fraction = position - (double) index;
    return values[index] * (1 - fraction) + values[index + 1] * fraction;
}

// Mean onset strength on a beat grid of `period` frames starting at `phase`.
double gridStrength(const std::vector<float>& envelope, double period, double phase)
{
    double sum = 0;
    int count = 0;
    for (double position = phase; position < (double) envelope.size() - 1; position += period, ++count)
        sum += interpolate(envelope, position);
    return count > 0 ? sum / count : 0;
}

// Fraction of the onset energy on a grid of `period` frames (best phase, +-1 frame): the coarsest grid that holds nearly
// all attacks is the tatum.
double gridCoverage(const std::vector<float>& envelope, double total, double period)
{
    if (total <= 0 || period < 2) return 0;
    double best = 0;
    const auto size = (int) envelope.size();
    for (double phase = 0; phase < period; phase += 0.5)
    {
        double sum = 0;
        for (double position = phase; position < size - 0.5; position += period)
        {
            const int centre = (int) std::lround(position);
            float peak = 0;
            for (int i = std::max(0, centre - 1); i <= std::min(size - 1, centre + 1); ++i) peak = std::max(peak, envelope[(size_t) i]);
            sum += peak;
        }
        best = std::max(best, sum);
    }
    return std::min(1.0, best / total);
}

// How well a beat explains the attacks: they sit on its sixteenths or its triplets. A half tempo only covers every
// other sixteenth; a dotted or triplet relation of the real beat misses the grid.
double subdivisionCoverage(const std::vector<float>& envelope, double total, double framesPerSecond, double bpm)
{
    const double period = 60 * framesPerSecond / bpm;
    return std::max(gridCoverage(envelope, total, period / 4), gridCoverage(envelope, total, period / 3));
}

double octaveDistance(double a, double b) { return std::abs(std::log2(a / b)); }

// How plausible a tempo is before looking at the audio: log-normal around 120 BPM, 90-180 favoured, the set's tempo as
// a tie-breaker.
double tempoPrior(double bpm, double preferredBpm)
{
    const double octaves = std::log2(bpm / 120);
    return std::exp(-0.5 * octaves * octaves / 0.81) * (bpm < 90 ? 0.65 : bpm > 180 ? 0.8 : 1.0)
         * (1 + 0.02 * std::exp(-octaveDistance(bpm, preferredBpm)));
}

// Autocorrelation of the onset envelope, normalised per overlap and by its energy.
struct Tempogram
{
    std::vector<float> correlation;
    double framesPerSecond = 0, usableLag = 0;

    // A tempo is as strong as the mean correlation at its beat multiples within the same lag window for every tempo (so a
    // half tempo, which only sees the even multiples, does not win just because loops repeat every bar).
    double score(double bpm) const
    {
        const double period = 60 * framesPerSecond / bpm;
        double sum = 0;
        int count = 0;
        for (int k = 1; k * period <= usableLag; ++k, ++count) sum += interpolate(correlation, k * period);
        return count > 0 ? sum / count : 0;
    }
};

Tempogram makeTempogram(const std::vector<float>& values, double framesPerSecond)
{
    Tempogram result;
    if (values.size() < 32) return result;
    double mean = 0;
    for (float v : values) mean += v;
    mean /= (double) values.size();
    std::vector<float> centered(values.size());
    for (size_t i = 0; i < values.size(); ++i) centered[i] = (float) (values[i] - mean);
    const size_t maxLag = std::min(values.size() - 1, (size_t) std::ceil(kCombSeconds * framesPerSecond) + 2);
    result.correlation.assign(maxLag + 1, 0.0f);
    for (size_t lag = 0; lag <= maxLag; ++lag)
    {
        double sum = 0;
        for (size_t i = 0; i + lag < centered.size(); ++i) sum += (double) centered[i] * centered[i + lag];
        result.correlation[lag] = (float) (sum / (double) (centered.size() - lag));
    }
    const float energy = result.correlation[0];
    if (energy <= 1e-9f) { result.correlation.clear(); return result; }
    for (auto& c : result.correlation) c /= energy;
    result.framesPerSecond = framesPerSecond;
    result.usableLag = std::min((double) maxLag, 0.6 * (double) values.size());
    return result;
}

// Everything the tempo candidates are judged on.
struct Evidence
{
    Tempogram tempogram;
    std::vector<float> smooth; // the onset envelope, lightly smoothed against timing jitter
    std::vector<float> attacks; // its clear local peaks only (sustained sounds leave a floor of flux everywhere)
    double total = 0, framesPerSecond = 0, preferredBpm = 120;

    bool usable() const { return ! tempogram.correlation.empty() && total > 0; }
    double coverage(double bpm) const { return subdivisionCoverage(attacks, total, framesPerSecond, bpm); }
    // Periodicity x attacks on the beat's subdivisions x prior.
    double value(double bpm, double score) const
    {
        const double c = coverage(bpm);
        return score * std::pow(c, 1.5) * tempoPrior(bpm, preferredBpm);
    }
    bool onPeak(double bpm, double score) const
    {
        double peak = score;
        for (double nearby = bpm * 0.96; nearby <= bpm * 1.04; nearby += 0.1) peak = std::max(peak, tempogram.score(nearby));
        return score >= 0.6 * peak;
    }
};

// The file is a whole number of beats (loops). Each count's tempo is judged like a free candidate and must sit on a
// tempogram peak; powers of two beats are preferred, then multiples of 4 or 2 bars. This also resolves octave, dotted
// and 3-sixteenth (psytrance) pulses that the free search picks. Like Live, an exact x.0 / x.5 BPM at 2^n beats within
// 90-180 BPM is taken as a loop unless another power-of-two count fits the audio better or its subdivisions miss
// attacks; with a weak or missing audio tempo any exact, round count is used. Returns 0 when nothing fits.
double lengthTempo(double seconds, const Evidence& evidence, double audioValue, double audioScore, double& support)
{
    // Songs and stems are not cut to a beat count (and their length matches some count by chance).
    if (seconds <= 0 || seconds > 160) return 0;
    const auto clear = [&](double tempo, double threshold)
    { const double s = evidence.tempogram.score(tempo); return s >= threshold && evidence.onPeak(tempo, s); };
    double best = 0, bestValue = 0, round = 0, roundValue = 0, loop = 0, loopValue = 0;
    bool bestPowerOfTwo = false, bestExact = false;
    for (int beats = 4; beats <= 512; beats += 4)
    {
        const double bpm = 60.0 * beats / seconds;
        const bool powerOfTwo = (beats & (beats - 1)) == 0;
        if (bpm < 60 || bpm > 200 || (! powerOfTwo && beats > 192)) continue;
        const double countWeight = powerOfTwo ? 1.0 : beats % 16 == 0 ? 0.8 : beats % 8 == 0 ? 0.6 : 0.45;
        const double prior = countWeight * tempoPrior(bpm, evidence.preferredBpm);
        const bool exact = powerOfTwo ? std::abs(bpm * 2 - std::round(bpm * 2)) < 0.004 : std::abs(bpm - std::round(bpm)) < 0.002;
        if (exact && prior > roundValue) { roundValue = prior; round = bpm; }
        if (exact && powerOfTwo && bpm >= 90 && bpm <= 180 && prior > loopValue) { loopValue = prior; loop = bpm; }
        if (! evidence.usable()) continue;
        // A tempo whose odd beats are empty (patterns repeating every 2 beats) scores about half of its half tempo: it
        // may pass at half the threshold when that half tempo is a clear peak.
        if (! clear(bpm, kMinConfidence) && ! (beats % 8 == 0 && clear(bpm, 0.5 * kMinConfidence) && clear(bpm / 2, kMinConfidence)))
            continue;
        const double score = evidence.tempogram.score(bpm);
        const double value = countWeight * evidence.value(bpm, score);
        if (value >= 0.4 * audioValue && value > bestValue)
        { bestValue = value; best = bpm; support = score; bestPowerOfTwo = powerOfTwo; bestExact = exact; }
    }
    if (best > 0 && (bestPowerOfTwo || loop <= 0) && (bestExact || audioScore >= kWeakConfidence || round <= 0)) return best;
    // The loop tempo must explain the attacks as well (its subdivisions hold them) and show some periodicity.
    if (loop > 0 && (! evidence.usable() || (evidence.tempogram.score(loop) >= 0.5 * kMinConfidence
                                             && (best <= 0 || evidence.coverage(loop) >= 0.95 * evidence.coverage(best)))))
    {
        support = std::max(kMinConfidence, evidence.usable() ? evidence.tempogram.score(loop) : 0.0);
        return loop;
    }
    // A weak audio tempo (pads, risers, cymbals) does not beat an exact length.
    if (round > 0 && audioScore < kWeakConfidence && ! bestExact)
    {
        support = std::max(kMinConfidence, evidence.usable() ? evidence.tempogram.score(round) : 0.0);
        return round;
    }
    return best;
}

// Best phase (in envelope frames) of a beat grid at `bpm` over the smoothed envelope.
double gridPhase(const std::vector<float>& smooth, double framesPerSecond, double bpm, double& strength)
{
    const double period = 60 * framesPerSecond / bpm;
    double best = 0;
    strength = -1;
    for (double p = 0; p < period; p += 0.5)
        if (const double s = gridStrength(smooth, period, p); s > strength) { strength = s; best = p; }
    return best;
}
}

TempoEstimate estimateSampleTempo(const juce::AudioBuffer<float>& audio, double sampleRate, double preferredBpm)
{
    if (! std::isfinite(preferredBpm) || preferredBpm < 20 || preferredBpm > 999) preferredBpm = 120;
    if (! std::isfinite(sampleRate) || sampleRate < 1000 || audio.getNumSamples() <= 0) return {};
    const double seconds = audio.getNumSamples() / sampleRate;
    const auto envelope = onsetEnvelope(audio, sampleRate);
    const double fps = envelope.framesPerSecond;
    if (fps <= 0) return {}; // silent or shorter than two analysis windows
    Evidence evidence;
    evidence.tempogram = makeTempogram(envelope.values, fps);
    evidence.framesPerSecond = fps;
    evidence.preferredBpm = preferredBpm;
    evidence.smooth.resize(envelope.values.size());
    for (size_t i = 0; i < evidence.smooth.size(); ++i)
    {
        evidence.smooth[i] = 0.5f * envelope.values[i] + 0.25f * (i > 0 ? envelope.values[i - 1] : 0.0f)
                           + 0.25f * (i + 1 < evidence.smooth.size() ? envelope.values[i + 1] : 0.0f);
    }
    // Attacks: local maxima (+-35 ms) above 1.5x the mean of the surrounding second.
    const auto frames = (int) evidence.smooth.size();
    evidence.attacks.assign(evidence.smooth.size(), 0.0f);
    std::vector<double> prefix(evidence.smooth.size() + 1, 0.0);
    for (size_t i = 0; i < evidence.smooth.size(); ++i) prefix[i + 1] = prefix[i] + evidence.smooth[i];
    const int around = std::max(4, (int) std::lround(0.5 * fps)), nearby = std::max(1, (int) std::lround(0.035 * fps));
    for (int i = 0; i < frames; ++i)
    {
        const float v = evidence.smooth[(size_t) i];
        const int lo = std::max(0, i - around), hi = std::min(frames, i + around + 1);
        if (v <= 1.5 * (prefix[(size_t) hi] - prefix[(size_t) lo]) / (hi - lo)) continue;
        bool peak = true;
        for (int j = std::max(0, i - nearby); j <= std::min(frames - 1, i + nearby) && peak; ++j)
            peak = j == i || evidence.smooth[(size_t) j] < v || (j > i && ! (evidence.smooth[(size_t) j] > v));
        if (peak) { evidence.attacks[(size_t) i] = v; evidence.total += v; }
    }

    // Free search: the comb tempogram's peaks, judged by periodicity, subdivision coverage and the prior.
    double audioBpm = 0, audioScore = 0, audioValue = 0;
    if (evidence.usable())
    {
        // The correlation noise of an envelope of N frames is about 1/sqrt(N): long files (stems, songs) may show a
        // weaker but still significant pulse.
        const double minScore = std::clamp(5 / std::sqrt((double) evidence.smooth.size()), 0.5 * kMinConfidence, kMinConfidence);
        std::vector<std::pair<double, double>> scores; // bpm, score
        for (double bpm = kSearchMin; bpm <= kSearchMax; bpm += 0.1) scores.emplace_back(bpm, evidence.tempogram.score(bpm));
        for (size_t i = 1; i + 1 < scores.size(); ++i)
        {
            const auto [bpm, score] = scores[i];
            if (score < minScore || score < scores[i - 1].second || score < scores[i + 1].second) continue;
            if (const double value = evidence.value(bpm, score); value > audioValue) { audioValue = value; audioBpm = bpm; audioScore = score; }
        }
    }

    TempoEstimate result;
    double phase = 0, strength = 0, support = 0;
    if (const double fromLength = lengthTempo(seconds, evidence, audioValue, audioScore, support); fromLength > 0)
    {
        result.bpm = fromLength;
        result.fromLength = true;
        result.confidence = std::min(1.0, support);
        if (! evidence.smooth.empty()) phase = gridPhase(evidence.smooth, fps, fromLength, strength);
    }
    else if (audioBpm > 0)
    {
        // Refine on a beat grid over the whole analysed span, then fold into 70-180 BPM.
        double best = -1;
        for (double bpm = audioBpm * 0.985; bpm <= audioBpm * 1.015; bpm += 0.01)
            if (const double p = gridPhase(evidence.smooth, fps, bpm, strength); strength > best) { best = strength; result.bpm = bpm; phase = p; }
        result.confidence = std::min(1.0, audioScore);
        while (result.bpm < 70 && result.bpm * 2 <= 200) result.bpm *= 2;
        while (result.bpm > 180 && result.bpm / 2 >= 60) result.bpm /= 2;
    }
    if (result.bpm <= 0) return {};
    // TODO(UI-47c): the first warp marker stays on the file start; firstBeat could move it onto the first beat once stems
    // dropped together share one grid (each stem's phase is found on its own).
    const double beatSeconds = 60 / result.bpm;
    const double gridStart = fps > 0 ? envelope.startSeconds + phase / fps : 0;
    result.firstBeat = std::fmod(std::fmod(gridStart, beatSeconds) + beatSeconds, beatSeconds);
    if (result.firstBeat > beatSeconds - 0.005) result.firstBeat = 0;
    return result;
}
}
