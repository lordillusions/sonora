#include "devices/native/SeraphGlottis.h"

#include "dsp/Fft.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <mutex>

namespace sonora::devices::seraph
{
namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr int kAnalysisLog2 = 13; // 8192 points per period for the analysis of the pulse

// The Rosenberg glottal flow (rising half cosine, falling quarter cosine, then closed) differentiated: the flow derivative,
// which is what a voice radiates. The jump at closure gives harmonics falling by 6 dB per octave. `oq` is the open
// quotient, `sq` the speed quotient (opening time / closing time).
double flowDerivative(double t, double oq, double sq)
{
    const double tp = oq * sq / (1.0 + sq), tn = oq / (1.0 + sq);
    if (t < tp) return kPi / (2.0 * tp) * std::sin(kPi * t / tp);
    if (t < oq) return -kPi / (2.0 * tn) * std::sin(kPi * (t - tp) / (2.0 * tn));
    return 0.0;
}

// The complex harmonics c_k (k = 1..kMaxHarmonics) of shape s: the pulse's Fourier series with a spectral tilt that
// grows towards breathy (a softer closure, i.e. a return phase), normalised so the first 32 harmonics carry unit power.
std::vector<std::complex<float>> pulseHarmonics(int shape)
{
    const float x = (float) shape / (float) (kPulseShapes - 1); // 0 pressed .. 1 breathy
    const double oq = openQuotient((float) shape);
    const double sq = 3.2 - 1.6 * x;
    const double tiltHarmonic = 40.0 * std::pow(4.0 / 40.0, x); // the return phase: -6 dB/octave more above it
    const int m = 1 << kAnalysisLog2;
    dsp::Fft fft(kAnalysisLog2);
    std::vector<std::complex<float>> data((size_t) m);
    for (int i = 0; i < m; ++i) data[(size_t) i] = {(float) flowDerivative((i + 0.5) / m, oq, sq), 0.0f};
    fft.transform(data);
    std::vector<std::complex<float>> c((size_t) kMaxHarmonics);
    double power = 0;
    for (int k = 1; k <= kMaxHarmonics; ++k)
    {
        const double tilt = 1.0 / std::sqrt(1.0 + (k / tiltHarmonic) * (k / tiltHarmonic));
        c[(size_t) k - 1] = data[(size_t) k] * (float) (tilt / m);
        if (k <= 32) power += 2.0 * std::norm(c[(size_t) k - 1]);
    }
    const float scale = (float) (1.0 / std::sqrt(std::max(power, 1e-20)));
    for (auto& v : c) v *= scale;
    return c;
}

const std::vector<std::vector<std::complex<float>>>& allHarmonics()
{
    // Every shape takes the phases of the modal one: the face blends two neighbouring shapes sample by sample, and pulses
    // whose harmonics are out of phase would cancel each other in the blend (measured: up to 8 dB lost around F1 on low
    // notes). The ear hears the magnitudes; the modal pulse's phases keep a glottal-like waveform.
    static const std::vector<std::vector<std::complex<float>>> all = [] {
        std::vector<std::vector<std::complex<float>>> out;
        for (int s = 0; s < kPulseShapes; ++s) out.push_back(pulseHarmonics(s));
        const auto reference = out[(size_t) kPulseShapes / 2 - 1];
        for (auto& shape : out)
            for (size_t k = 0; k < shape.size(); ++k)
            {
                const float magnitude = std::abs(shape[k]), refMagnitude = std::abs(reference[k]);
                shape[k] = refMagnitude > 0.0f ? reference[k] * (magnitude / refMagnitude) : std::complex<float>{magnitude, 0.0f};
            }
        return out;
    }();
    return all;
}

// ---- loudness table: [type 0..4 (4 = Boys)][hum 0/1][shape][vowel 0, 0.5 .. 4][pitch MIDI 24, 25 .. 108], in dB. The power
// of a voice depends on where its harmonics fall on the formant peaks, so the pitch grid is one semitone.
constexpr int kTypesWithBoys = kVoiceTypes + 1;
constexpr int kHums = 2, kVowelPoints = 9, kPitchPoints = 85;
constexpr float kPitchLow = 24.0f, kPitchStep = 1.0f;
constexpr float kLoudnessTopHz = 8000.0f;

struct LoudnessTable
{
    std::vector<float> db = std::vector<float>((size_t) kTypesWithBoys * kHums * kPulseShapes * kVowelPoints * kPitchPoints);
    static size_t index(int type, int hum, int shape, int vowel, int pitch)
    {
        return ((((size_t) type * kHums + (size_t) hum) * kPulseShapes + (size_t) shape) * kVowelPoints + (size_t) vowel) * kPitchPoints
               + (size_t) pitch;
    }
    float& at(int type, int hum, int shape, int vowel, int pitch) { return db[index(type, hum, shape, vowel, pitch)]; }
    float get(int type, int hum, int shape, int vowel, int pitch) const { return db[index(type, hum, shape, vowel, pitch)]; }
};

const LoudnessTable& loudnessTable()
{
    static const LoudnessTable table = [] {
        LoudnessTable t;
        const auto& harmonics = allHarmonics();
        std::vector<float> response; // |H(k f0)|^2
        for (int type = 0; type < kTypesWithBoys; ++type)
            for (int hum = 0; hum < kHums; ++hum)
                for (int vowel = 0; vowel < kVowelPoints; ++vowel)
                    for (int p = 0; p < kPitchPoints; ++p)
                    {
                        const float hz = 440.0f * std::pow(2.0f, (kPitchLow + kPitchStep * (float) p - 69.0f) / 12.0f);
                        FormantShape shape;
                        shape.vowel = 0.5f * (float) vowel;
                        shape.hum = (float) hum;
                        shape.scale = type == kVoiceTypes ? kBoysFormantScale : 1.0f;
                        shape.f0 = hz;
                        const auto f = formants(type == kVoiceTypes ? Soprano : type, shape);
                        const int count = std::min(kMaxHarmonics, std::max(1, (int) (kLoudnessTopHz / hz)));
                        response.assign((size_t) count, 0.0f);
                        for (int k = 1; k <= count; ++k)
                        {
                            const float x = hz * (float) k;
                            std::complex<float> h{};
                            for (int i = 0; i < kFormants; ++i)
                            {
                                const std::complex<float> num{0.0f, x * f.bw[i]};
                                const std::complex<float> den{f.hz[i] * f.hz[i] - x * x, x * f.bw[i]};
                                h += (i % 2 == 0 ? 1.0f : -1.0f) * f.gain[i] * (num / den);
                            }
                            response[(size_t) k - 1] = std::norm(h);
                        }
                        for (int s = 0; s < kPulseShapes; ++s)
                        {
                            double power = 0;
                            for (int k = 1; k <= count; ++k) power += 2.0 * std::norm(harmonics[(size_t) s][(size_t) k - 1]) * response[(size_t) k - 1];
                            t.at(type, hum, s, vowel, p) = (float) (-10.0 * std::log10(std::max(power, 1e-12)));
                        }
                    }
        return t;
    }();
    return table;
}
} // namespace

const std::vector<float>& GlottalTables::harmonics(int shape)
{
    static const std::vector<std::vector<float>> magnitudes = [] {
        std::vector<std::vector<float>> out;
        for (const auto& c : allHarmonics())
        {
            std::vector<float> m;
            for (const auto& v : c) m.push_back(2.0f * std::abs(v));
            out.push_back(std::move(m));
        }
        return out;
    }();
    return magnitudes[(size_t) std::clamp(shape, 0, kPulseShapes - 1)];
}

GlottalTables::GlottalTables(double sampleRate) : sampleRate_(sampleRate > 0 ? sampleRate : 44100.0)
{
    data_.assign((size_t) kPulseShapes * kPulseOctaves * (kTableSize + 1), 0.0f);
    const auto& harmonics = allHarmonics();
    constexpr int log2 = 11;
    static_assert((1 << log2) == kTableSize);
    dsp::Fft fft(log2);
    std::vector<std::complex<float>> spectrum((size_t) kTableSize);
    const double top = std::min(0.45 * sampleRate_, 20000.0);
    for (int s = 0; s < kPulseShapes; ++s)
        for (int o = 0; o < kPulseOctaves; ++o)
        {
            const double maxHz = kLowestTableHz * std::pow(2.0, o);
            const int count = std::clamp((int) (top / maxHz), 1, std::min(kMaxHarmonics, kTableSize / 2 - 1));
            std::fill(spectrum.begin(), spectrum.end(), std::complex<float>{});
            // x[n] = sum_k c_k e^{+i 2 pi k n / N} + conj; via a forward transform of the conjugate.
            for (int k = 1; k <= count; ++k)
            {
                const auto c = harmonics[(size_t) s][(size_t) k - 1];
                spectrum[(size_t) k] = std::conj(c);
                spectrum[(size_t) (kTableSize - k)] = c;
            }
            fft.transform(spectrum);
            float* out = data_.data() + (size_t) (s * kPulseOctaves + o) * (kTableSize + 1);
            for (int n = 0; n < kTableSize; ++n) out[n] = spectrum[(size_t) n].real();
            out[kTableSize] = out[0];
        }
}

std::shared_ptr<const GlottalTables> GlottalTables::forRate(double sampleRate)
{
    static std::mutex mutex;
    static std::vector<std::shared_ptr<const GlottalTables>> cache;
    const std::lock_guard<std::mutex> lock(mutex);
    for (const auto& t : cache)
        if (std::abs(t->sampleRate() - sampleRate) < 0.5) return t;
    auto made = std::make_shared<const GlottalTables>(sampleRate);
    cache.push_back(made);
    return made;
}

float loudnessGain(int type, float vowel, float hum, float shape, float hz)
{
    const auto& table = loudnessTable();
    type = std::clamp(type, 0, kTypesWithBoys - 1);
    const float midi = 69.0f + 12.0f * std::log2(std::max(hz, 1.0f) / 440.0f);
    auto split = [](float x, int points, int& i, float& t) {
        x = std::clamp(x, 0.0f, (float) (points - 1));
        i = std::min((int) x, points - 2);
        t = x - (float) i;
    };
    int hi, si, vi, pi;
    float ht, st, vt, pt;
    split(hum, kHums, hi, ht);
    split(shape, kPulseShapes, si, st);
    split(vowel * 2.0f, kVowelPoints, vi, vt);
    split((midi - kPitchLow) / kPitchStep, kPitchPoints, pi, pt);
    float db = 0;
    for (int a = 0; a < 2; ++a)
        for (int b = 0; b < 2; ++b)
            for (int c = 0; c < 2; ++c)
                for (int d = 0; d < 2; ++d)
                {
                    const float w = (a ? ht : 1 - ht) * (b ? st : 1 - st) * (c ? vt : 1 - vt) * (d ? pt : 1 - pt);
                    db += w * table.get(type, hi + a, si + b, vi + c, pi + d);
                }
    return std::pow(10.0f, db / 20.0f);
}
} // namespace sonora::devices::seraph
