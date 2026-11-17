#include "devices/native/SeraphReal.h"

#include "dsp/Fft.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <complex>

namespace sonora::devices::seraph
{
namespace
{
constexpr int kHop = 256;
constexpr double kMinHz = 70.0, kMaxHz = 1200.0;
constexpr float kThreshold = 0.12f, kVoiced = 0.25f;

int nextPow2(int n)
{
    int p = 1;
    while (p < n) p <<= 1;
    return p;
}

int log2Of(int p)
{
    int l = 0;
    while ((1 << l) < p) ++l;
    return l;
}

std::vector<std::string> tokens(const std::string& name)
{
    std::vector<std::string> out;
    std::string current;
    for (char c : name)
    {
        if (std::isalnum((unsigned char) c) || c == '#') current += c;
        else if (! current.empty()) { out.push_back(current); current.clear(); }
    }
    if (! current.empty()) out.push_back(current);
    return out;
}

// The best-matching lag of the period around `at` within [lo, hi] (normalised correlation, parabolic refinement).
double bestLag(const std::vector<float>& x, double at, double lo, double hi, double period)
{
    const int centre = (int) std::lround(at), half = std::max(4, (int) (period * 0.5));
    const int from = std::max(0, centre - half), to = std::min((int) x.size(), centre + half);
    std::vector<double> c;
    const int l0 = (int) std::floor(lo), l1 = (int) std::ceil(hi);
    for (int lag = l0 - 1; lag <= l1 + 1; ++lag)
    {
        double sum = 0, e0 = 0, e1 = 0;
        for (int j = from; j < to; ++j)
        {
            const int k = j + lag;
            if (k < 0 || k >= (int) x.size()) continue;
            sum += (double) x[(size_t) j] * x[(size_t) k];
            e0 += (double) x[(size_t) j] * x[(size_t) j];
            e1 += (double) x[(size_t) k] * x[(size_t) k];
        }
        c.push_back(sum / std::sqrt(std::max(1e-20, e0 * e1)));
    }
    int best = 1;
    for (int i = 1; i + 1 < (int) c.size(); ++i)
        if (c[(size_t) i] > c[(size_t) best]) best = i;
    const double a = c[(size_t) best - 1], b = c[(size_t) best], d = c[(size_t) std::min(best + 1, (int) c.size() - 1)];
    const double denom = a - 2 * b + d;
    return (double) (l0 - 1 + best) + (std::abs(denom) > 1e-12 ? std::clamp(0.5 * (a - d) / denom, -0.5, 0.5) : 0.0);
}
// The grain of every mark: measure each harmonic's complex amplitude in the two-period Hann segment (the neighbours'
// window lobes are zero exactly at a harmonic), interpolate log magnitude and unwrapped phase between harmonics on a fine
// grid, back to time, window again to two periods. At the recording's own period the grains add up to its harmonics; at
// another period they sample a smooth spectrum instead of the notches between the recording's harmonics.
void buildGrains(RealVoice& v)
{
    constexpr double kTwoPi = 6.283185307179586, kPiD = 3.14159265358979;
    const auto& x = v.audio;
    v.grainData.clear();
    v.grainStart.clear();
    v.grainHalf.clear();
    std::vector<std::complex<double>> harmonics;
    std::vector<double> magnitude, phase;
    std::vector<std::complex<float>> spectrum;
    std::unique_ptr<dsp::Fft> fft, segmentFft;
    std::vector<std::complex<float>> segment;
    int fftSize = 0, segmentSize = 0;
    for (size_t m = 0; m < v.marks.size(); ++m)
    {
        const double period = v.periods[m], centre = v.marks[m];
        const int half = std::max(8, (int) std::ceil(period));
        // The two periods around the mark resampled to 2Q points (Q a power of two per period, Catmull-Rom), one FFT: with
        // a two-period Hann window, harmonic h sits exactly on bin 2h and its neighbours' lobes are zero there. (A direct
        // sum per harmonic cost about 140 ms per file.)
        int q = 128;
        while (q < (int) std::ceil(period)) q <<= 1;
        const int qSize = 2 * q;
        if (qSize != segmentSize)
        {
            segmentSize = qSize;
            int log2 = 0;
            while ((1 << log2) < qSize) ++log2;
            segmentFft = std::make_unique<dsp::Fft>(log2);
        }
        segment.assign((size_t) qSize, {});
        auto sampleAt = [&](double t) {
            const int i = (int) std::floor(t);
            const double f = t - i;
            auto x0 = [&](int k) { return k >= 0 && k < (int) x.size() ? (double) x[(size_t) k] : 0.0; };
            const double a = x0(i - 1), b = x0(i), c = x0(i + 1), d = x0(i + 2);
            return b + 0.5 * f * (c - a + f * (2.0 * a - 5.0 * b + 4.0 * c - d + f * (3.0 * (b - c) + d - a)));
        };
        for (int j = 0; j < qSize; ++j)
        {
            const double t = (double) (j - q) / q; // in periods, -1 .. 1
            const double w = 0.5 + 0.5 * std::cos(kPiD * t);
            segment[(size_t) ((j - q + qSize) % qSize)] = {(float) (w * sampleAt(centre + t * period)), 0.0f}; // the mark at index 0
        }
        segmentFft->transform(segment);
        const int count = std::max(1, std::min((int) (0.48 * period), q - 1));
        harmonics.assign((size_t) count + 1, {});
        const double toOriginal = period / q; // the resampled sum is q / period times the original's
        for (int h = 1; h <= count; ++h) harmonics[(size_t) h] = std::complex<double>(segment[(size_t) (2 * h)]) * toOriginal;
        // the grain spans four periods (a narrow formant rings longer than one), on a grid four times longer still
        const int grainHalf = 2 * half;
        const int size = std::max(256, 1 << (int) std::ceil(std::log2(8.0 * grainHalf)));
        if (size != fftSize)
        {
            fftSize = size;
            int log2 = 0;
            while ((1 << log2) < size) ++log2;
            fft = std::make_unique<dsp::Fft>(log2);
        }
        spectrum.assign((size_t) size, {});
        magnitude.assign((size_t) count + 1, 0.0);
        phase.assign((size_t) count + 1, 0.0);
        for (int h = 1; h <= count; ++h)
        {
            magnitude[(size_t) h] = std::log(std::max(1e-12, std::abs(harmonics[(size_t) h])));
            double p = std::arg(harmonics[(size_t) h]);
            if (h > 1)
            {
                while (p - phase[(size_t) h - 1] > kPiD) p -= kTwoPi;
                while (p - phase[(size_t) h - 1] < -kPiD) p += kTwoPi;
            }
            phase[(size_t) h] = p;
        }
        for (int b = 1; b < size / 2; ++b)
        {
            const double hf = (double) b * period / size; // the bin, in harmonics of this period
            if (hf > count) break;
            double mag, ph;
            if (hf < 1.0)
            {
                mag = std::exp(magnitude[1]) * hf; // fades to nothing at DC
                ph = phase[1];
            }
            else
            {
                const int h0 = std::min((int) hf, std::max(1, count - 1)), h1 = std::min(h0 + 1, count);
                const double t = std::clamp(hf - h0, 0.0, 1.0);
                mag = std::exp(magnitude[(size_t) h0] + t * (magnitude[(size_t) h1] - magnitude[(size_t) h0]));
                ph = phase[(size_t) h0] + t * (phase[(size_t) h1] - phase[(size_t) h0]);
            }
            const auto value = std::polar(mag, ph);
            spectrum[(size_t) b] = std::complex<float>((float) value.real(), (float) -value.imag()); // conjugate: the inverse
            spectrum[(size_t) (size - b)] = std::complex<float>((float) value.real(), (float) value.imag());
        }
        fft->transform(spectrum);
        v.grainStart.push_back((int) v.grainData.size());
        v.grainHalf.push_back(grainHalf);
        for (int n = -grainHalf; n <= grainHalf; ++n)
        {
            const double w = std::abs((double) n) < 2.0 * period ? 0.5 + 0.5 * std::cos(kPiD * n / (2.0 * period)) : 0.0;
            v.grainData.push_back((float) (w * spectrum[(size_t) ((n % size + size) % size)].real() / size));
        }
    }
}
} // namespace

std::vector<float> pitchTrack(const float* x, int n, double sampleRate, std::vector<float>* aperiodicity)
{
    const int maxLag = (int) (sampleRate / kMinHz), minLag = std::max(2, (int) (sampleRate / kMaxHz));
    const int window = nextPow2(std::max(maxLag + 1, (int) (sampleRate * 0.021)));
    const int span = window + maxLag + 2;
    const int size = nextPow2(span + window);
    dsp::Fft fft(log2Of(size));
    std::vector<std::complex<float>> a((size_t) size), b((size_t) size);
    std::vector<double> energy((size_t) span + 1), d((size_t) maxLag + 2), cmnd((size_t) maxLag + 2);
    std::vector<float> track, aper;
    for (int start = 0; start + span < n; start += kHop)
    {
        std::fill(a.begin(), a.end(), std::complex<float>{});
        std::fill(b.begin(), b.end(), std::complex<float>{});
        for (int j = 0; j < window; ++j) a[(size_t) j] = {x[start + j], 0.0f};
        for (int j = 0; j < span; ++j) b[(size_t) j] = {x[start + j], 0.0f};
        energy[0] = 0;
        for (int j = 0; j < span; ++j) energy[(size_t) j + 1] = energy[(size_t) j] + (double) x[start + j] * x[start + j];
        fft.transform(a);
        fft.transform(b);
        for (int k = 0; k < size; ++k) b[(size_t) k] = std::conj(std::conj(a[(size_t) k]) * b[(size_t) k]);
        fft.transform(b); // conj of the inverse (times size): the cross-correlation, real
        const double e0 = energy[(size_t) window];
        double running = 0;
        cmnd[0] = 1;
        for (int lag = 1; lag <= maxLag + 1; ++lag)
        {
            const double r = (double) b[(size_t) lag].real() / size;
            const double et = energy[(size_t) (lag + window)] - energy[(size_t) lag];
            d[(size_t) lag] = std::max(0.0, e0 + et - 2.0 * r);
            running += d[(size_t) lag];
            cmnd[(size_t) lag] = running > 0 ? d[(size_t) lag] * lag / running : 1.0;
        }
        int best = -1;
        for (int lag = minLag; lag <= maxLag; ++lag)
            if (cmnd[(size_t) lag] < kThreshold)
            {
                while (lag + 1 <= maxLag && cmnd[(size_t) lag + 1] < cmnd[(size_t) lag]) ++lag;
                best = lag;
                break;
            }
        if (best < 0)
        {
            best = minLag;
            for (int lag = minLag; lag <= maxLag; ++lag)
                if (cmnd[(size_t) lag] < cmnd[(size_t) best]) best = lag;
        }
        // An octave too high: a voice whose second harmonic dominates (a soprano with F1 on it) looks nearly periodic at half
        // its period. When twice the lag is clearly more periodic, it is the period.
        for (int twice = 2 * best; twice + 1 <= maxLag + 1 && cmnd[(size_t) twice] < 0.5 * cmnd[(size_t) best] - 0.005;)
        {
            int at = twice;
            for (int lag = std::max(minLag, twice - 2); lag <= std::min(maxLag, twice + 2); ++lag)
                if (cmnd[(size_t) lag] < cmnd[(size_t) at]) at = lag;
            best = at;
            break;
        }
        const double p0 = cmnd[(size_t) best - 1], p1 = cmnd[(size_t) best], p2 = cmnd[(size_t) best + 1];
        const double denom = p0 - 2 * p1 + p2;
        const double lag = best + (std::abs(denom) > 1e-12 ? std::clamp(0.5 * (p0 - p2) / denom, -0.5, 0.5) : 0.0);
        const bool voiced = p1 < kVoiced && e0 > 1e-10 * window;
        track.push_back(voiced ? (float) (sampleRate / lag) : 0.0f);
        aper.push_back((float) p1);
    }
    if (aperiodicity) *aperiodicity = std::move(aper);
    return track;
}

int vowelFromName(const std::string& name)
{
    for (auto t : tokens(name))
    {
        std::transform(t.begin(), t.end(), t.begin(), [](unsigned char c) { return (char) std::tolower(c); });
        if (t == "a" || t == "ah" || t == "aa" || t == "aah") return 0;
        if (t == "e" || t == "eh") return 1;
        if (t == "i" || t == "ee" || t == "ih") return 2;
        if (t == "o" || t == "oh") return 3;
        if (t == "u" || t == "oo" || t == "ooh") return 4;
    }
    return -1;
}

int keyFromName(const std::string& name)
{
    static constexpr int kClasses[7] = {9, 11, 0, 2, 4, 5, 7}; // A B C D E F G
    for (const auto& t : tokens(name))
    {
        if (t.empty() || t[0] < 'A' || t[0] > 'G') continue;
        int pc = kClasses[t[0] - 'A'];
        size_t i = 1;
        if (i < t.size() && (t[i] == '#' || t[i] == 's')) { ++pc; ++i; }
        else if (i < t.size() && t[i] == 'b') { --pc; ++i; }
        pc = (pc + 12) % 12;
        // an optional minor / major mark, then an optional octave
        if (t.compare(i, 3, "min") == 0) i += 3;
        else if (t.compare(i, 3, "maj") == 0) i += 3;
        else if (i < t.size() && (t[i] == 'm' || t[i] == 'M')) ++i;
        if (i == t.size()) return -2 - pc;
        if (i + 1 == t.size() && std::isdigit((unsigned char) t[i])) return 12 * (t[i] - '0' + 1) + pc;
    }
    return -1;
}

std::shared_ptr<const RealVoice> analyseRealVoice(const float* const* channels, int numChannels, int numSamples, double sampleRate,
                                                  const std::string& name, std::string& reason)
{
    if (numSamples < (int) (sampleRate * 0.7) || numChannels <= 0 || sampleRate <= 0)
    {
        reason = "too short: a sustained note of two seconds or more works best";
        return nullptr;
    }
    auto voice = std::make_shared<RealVoice>();
    voice->name = name;
    voice->sampleRate = sampleRate;
    voice->audio.assign((size_t) numSamples, 0.0f);
    for (int ch = 0; ch < numChannels; ++ch)
        for (int i = 0; i < numSamples; ++i) voice->audio[(size_t) i] += channels[ch][i] / (float) numChannels;
    const auto& x = voice->audio;

    std::vector<float> aper;
    const auto track = pitchTrack(x.data(), numSamples, sampleRate, &aper);
    const int frames = (int) track.size();
    const int window = nextPow2(std::max((int) (sampleRate / kMinHz) + 1, (int) (sampleRate * 0.021)));
    std::vector<float> level((size_t) frames), cents((size_t) frames), smooth((size_t) frames);
    float loudest = 1e-9f;
    for (int f = 0; f < frames; ++f)
    {
        double e = 0;
        for (int j = 0; j < window; ++j) e += (double) x[(size_t) (f * kHop + j)] * x[(size_t) (f * kHop + j)];
        level[(size_t) f] = (float) (10.0 * std::log10(std::max(1e-12, e / window)));
        loudest = std::max(loudest, level[(size_t) f]);
        cents[(size_t) f] = track[(size_t) f] > 0 ? 1200.0f * std::log2(track[(size_t) f] / 440.0f) : -1e9f;
    }
    // a smoothed pitch (about 200 ms) so vibrato does not split a held note
    const int reach = std::max(1, (int) (0.1 * sampleRate / kHop));
    for (int f = 0; f < frames; ++f)
    {
        double sum = 0;
        int count = 0;
        for (int g = std::max(0, f - reach); g <= std::min(frames - 1, f + reach); ++g)
            if (track[(size_t) g] > 0) { sum += cents[(size_t) g]; ++count; }
        smooth[(size_t) f] = count > 0 ? (float) (sum / count) : -1e9f;
    }
    // the longest stable span
    int bestStart = -1, bestLength = 0;
    for (int f = 0; f < frames;)
    {
        if (track[(size_t) f] <= 0 || level[(size_t) f] < loudest - 30) { ++f; continue; }
        const float reference = smooth[(size_t) f];
        int g = f;
        float top = level[(size_t) f];
        while (g < frames && track[(size_t) g] > 0 && std::abs(smooth[(size_t) g] - reference) < 50.0f && std::abs(cents[(size_t) g] - reference) < 150.0f
               && level[(size_t) g] > std::max(top, level[(size_t) g]) - 9.0f)
        {
            top = std::max(top, level[(size_t) g]);
            ++g;
        }
        if (g - f > bestLength) { bestLength = g - f; bestStart = f; }
        f = std::max(g, f + 1);
    }
    if (bestStart < 0 || bestLength * kHop < (int) (0.6 * sampleRate))
    {
        reason = "no stable pitch found: Real needs a dry, single sustained note (a phrase, a chord or a wet sample has none)";
        return nullptr;
    }
    // the attack: voiced frames before the span, within a scoop of its pitch, at most half a second
    int attack = bestStart;
    const float reference = smooth[(size_t) bestStart + (size_t) bestLength / 2];
    while (attack > 0 && bestStart - attack < (int) (0.5 * sampleRate / kHop) && aper[(size_t) attack - 1] < 0.35f
           && track[(size_t) attack - 1] > 0 && std::abs(cents[(size_t) attack - 1] - reference) < 300.0f)
        --attack;
    auto periodAt = [&](double sample) {
        const double frame = (sample - window * 0.5) / kHop;
        int f = std::clamp((int) std::lround(frame), attack, bestStart + bestLength - 1);
        // the nearest voiced frame's period
        for (int d = 0; d < 8 && track[(size_t) f] <= 0; ++d) f = std::min(f + 1, bestStart + bestLength - 1);
        return track[(size_t) f] > 0 ? sampleRate / track[(size_t) f] : sampleRate / (440.0 * std::exp2(reference / 1200.0));
    };
    const double begin = attack * (double) kHop + window * 0.25, sustainStart = bestStart * (double) kHop + window * 0.5;
    const double end = (bestStart + bestLength) * (double) kHop + window * 0.5;
    // the first mark: the largest sample in the first period; then one period at a time, where the waveform matches
    double period = periodAt(begin);
    int first = (int) begin;
    for (int i = (int) begin; i < (int) (begin + period) && i < numSamples; ++i)
        if (x[(size_t) i] > x[(size_t) first]) first = i;
    double mark = first;
    while (mark + period * 1.6 < end && mark + period * 1.6 < numSamples)
    {
        voice->marks.push_back(mark);
        voice->periods.push_back((float) period);
        const double step = bestLag(x, mark, period * 0.8, period * 1.25, period);
        mark += std::clamp(step, period * 0.8, period * 1.25);
        period = 0.5 * period + 0.5 * periodAt(mark);
    }
    // Move every mark to where its period's energy gathers (the excitation of the vocal tract): grains centred there have
    // the smoothest phase across harmonics, so a lower pitch does not land in notches between the recording's harmonics
    // (measured: a 12 dB notch at F2 at -7 semitones with marks on the largest sample, 1.2 dB with marks on the energy).
    if (! voice->marks.empty())
    {
        const double p = voice->periods[voice->periods.size() / 2];
        double bestShift = 0, bestEnergy = -1;
        for (int step = -16; step < 16; ++step)
        {
            const double shift = p * step / 32.0;
            double energy = 0;
            for (size_t m = voice->marks.size() / 4; m < voice->marks.size(); m += 3)
            {
                const int from = (int) (voice->marks[m] + shift - p / 6), to = (int) (voice->marks[m] + shift + p / 6);
                for (int i = std::max(0, from); i < std::min(numSamples, to); ++i)
                {
                    const double w = 0.5 + 0.5 * std::cos(3.14159265358979 * (i - (voice->marks[m] + shift)) / (p / 6));
                    energy += w * (double) x[(size_t) i] * x[(size_t) i];
                }
            }
            if (energy > bestEnergy) { bestEnergy = energy; bestShift = shift; }
        }
        for (auto& m : voice->marks) m = std::clamp(m + bestShift, 0.0, (double) numSamples - 2);
    }
    voice->sustainBegin = 0;
    while (voice->sustainBegin < (int) voice->marks.size() && voice->marks[(size_t) voice->sustainBegin] < sustainStart) ++voice->sustainBegin;
    voice->sustainEnd = (int) voice->marks.size();
    if (voice->sustainEnd - voice->sustainBegin < 20)
    {
        reason = "no stable pitch found: the held part is too short";
        return nullptr;
    }
    // the sung note, an octave doubt settled by a key in the name
    std::vector<float> held;
    for (int f = bestStart; f < bestStart + bestLength; ++f) held.push_back(cents[(size_t) f]);
    std::nth_element(held.begin(), held.begin() + (long) held.size() / 2, held.end());
    voice->midi = 69.0f + held[held.size() / 2] / 100.0f;
    if (const int key = keyFromName(name); key >= 0)
    {
        if (std::abs(voice->midi + 12.0f - (float) key) < 1.5f) // heard an octave low: halve the periods
        {
            std::vector<double> marks;
            std::vector<float> periods;
            int sustainBegin = 0;
            for (size_t i = 0; i < voice->marks.size(); ++i)
            {
                if ((int) i == voice->sustainBegin) sustainBegin = (int) marks.size();
                marks.push_back(voice->marks[i]);
                periods.push_back(voice->periods[i] * 0.5f);
                if (i + 1 < voice->marks.size())
                {
                    marks.push_back(0.5 * (voice->marks[i] + voice->marks[i + 1]));
                    periods.push_back(voice->periods[i] * 0.5f);
                }
            }
            voice->marks = std::move(marks);
            voice->periods = std::move(periods);
            voice->sustainBegin = sustainBegin;
            voice->sustainEnd = (int) voice->marks.size();
            voice->midi += 12.0f;
        }
        else if (std::abs(voice->midi - 12.0f - (float) key) < 1.5f) // heard an octave high: every second mark
        {
            std::vector<double> marks;
            std::vector<float> periods;
            int sustainBegin = 0;
            for (size_t i = 0; i < voice->marks.size(); i += 2)
            {
                if ((int) i >= voice->sustainBegin && sustainBegin == 0) sustainBegin = (int) marks.size();
                marks.push_back(voice->marks[i]);
                periods.push_back(voice->periods[i] * 2.0f);
            }
            voice->marks = std::move(marks);
            voice->periods = std::move(periods);
            voice->sustainBegin = sustainBegin;
            voice->sustainEnd = (int) voice->marks.size();
            voice->midi -= 12.0f;
        }
    }
    buildGrains(*voice);
    double e = 0;
    const int s0 = (int) voice->marks[(size_t) voice->sustainBegin], s1 = (int) voice->marks.back();
    for (int i = s0; i < s1; ++i) e += (double) x[(size_t) i] * x[(size_t) i];
    voice->level = (float) std::sqrt(e / std::max(1, s1 - s0));
    if (voice->level < 1e-5f)
    {
        reason = "the held note is silent";
        return nullptr;
    }
    voice->vowel = vowelFromName(name);
    return voice;
}
} // namespace sonora::devices::seraph
