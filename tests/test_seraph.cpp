// PLG-27: Sonora Seraph, a choir instrument. Sessions cannot hear, so these tests measure what the ear would judge:
// pitch, formant peaks, aliasing, loudness, the human pitch model, the ensemble, sections, legato, chant, space, onsets,
// silence, stack, presets. docs/SERAPH.md "Tests".
#include "TestFramework.h"
#include "core/DeviceCatalog.h"
#include "core/Edit.h"
#include "core/Scales.h"
#include "devices/DeviceFactory.h"
#include "devices/native/Seraph.h"
#include "devices/native/SeraphFormants.h"
#include "dsp/Fft.h"
#include "io/als/AlsNewSet.h"
#include "io/als/AlsReader.h"
#include "io/als/AlsTemplates.h"
#include "io/als/AlsWriter.h"
#include "io/als/LiveCompat.h"
#include "plugins/SonoraPluginState.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <pthread.h>
#include <random>
#include <string>

using namespace sonora;
namespace
{
using S = devices::Seraph;
namespace sf = devices::seraph;

struct Note
{
    double at, length;
    int key;
    float velocity = 0.8f;
};

struct Clock
{
    double tempo = 120.0;
    bool playing = false;
    double startBeat = 0.0;
};

juce::AudioBuffer<float> render(S& s, double rate, double seconds, const std::vector<Note>& notes, Clock clock = {},
                                const std::function<void(int, double)>& perBlock = {}, int block = 256)
{
    const int total = (int) (seconds * rate);
    juce::AudioBuffer<float> out(2, total);
    out.clear();
    for (int start = 0; start < total; start += block)
    {
        const int len = std::min(block, total - start);
        devices::HostInfo host;
        host.tempo = clock.tempo;
        host.sampleRate = rate;
        host.playing = clock.playing;
        host.beats = clock.startBeat + start / rate * clock.tempo / 60.0;
        s.setHostInfo(host);
        juce::MidiBuffer midi;
        for (const auto& n : notes)
        {
            const int on = (int) std::lround(n.at * rate), off = (int) std::lround((n.at + n.length) * rate);
            if (on >= start && on < start + len) midi.addEvent(juce::MidiMessage::noteOn(1, n.key, n.velocity), on - start);
            if (off >= start && off < start + len) midi.addEvent(juce::MidiMessage::noteOff(1, n.key), off - start);
        }
        juce::AudioBuffer<float> view(out.getArrayOfWritePointers(), 2, start, len);
        s.process(view, midi);
        if (perBlock) perBlock(start, start / rate);
    }
    return out;
}

std::unique_ptr<S> makeDry(double rate, int section = S::SoloTenor)
{
    auto s = std::make_unique<S>();
    s->setParameter(S::Section, (float) section);
    s->setParameter(S::Singers, 1);
    for (int p : {S::Space, S::Air, S::Warmth, S::VowelMotion, S::Vibrato, S::Drift, S::Detune, S::Timing, S::Scoop, S::Human,
                  S::SizeSpread, S::Breath, S::Width, S::Swell, S::Effort})
        s->setParameter(p, 0.0f);
    s->setParameter(S::LowCut, 40);
    s->setParameter(S::Attack, 5);
    s->setParameter(S::Release, 50);
    s->setParameter(S::Fold, 0);
    s->prepare(rate, 512);
    return s;
}

double rmsDb(const juce::AudioBuffer<float>& a, int from, int to, int ch = 0)
{
    double sum = 0;
    to = std::min(to, a.getNumSamples());
    for (int i = from; i < to; ++i) sum += (double) a.getSample(ch, i) * a.getSample(ch, i);
    return 10.0 * std::log10(std::max(1e-30, sum / std::max(1, to - from)));
}

double peak(const juce::AudioBuffer<float>& a)
{
    return std::max(a.getMagnitude(0, 0, a.getNumSamples()), a.getMagnitude(1, 0, a.getNumSamples()));
}

// The period (in samples) near `expected`: normalised autocorrelation peak with parabolic refinement.
double periodNear(const float* x, int from, int window, double expected, double* strength = nullptr)
{
    const int lo = std::max(2, (int) (expected * 0.8)), hi = (int) (expected * 1.25) + 1;
    std::vector<double> r((size_t) (hi + 2), 0.0);
    double e0 = 0;
    for (int i = 0; i < window; ++i) e0 += (double) x[from + i] * x[from + i];
    for (int lag = lo - 1; lag <= hi + 1; ++lag)
    {
        double sum = 0, e1 = 0;
        for (int i = 0; i < window; ++i)
        {
            sum += (double) x[from + i] * x[from + i + lag];
            e1 += (double) x[from + i + lag] * x[from + i + lag];
        }
        r[(size_t) lag] = sum / std::sqrt(std::max(1e-30, e0 * e1));
    }
    int best = lo;
    for (int lag = lo; lag <= hi; ++lag)
        if (r[(size_t) lag] > r[(size_t) best]) best = lag;
    const double a = r[(size_t) best - 1], b = r[(size_t) best], c = r[(size_t) best + 1];
    const double denom = a - 2 * b + c;
    if (strength) *strength = b;
    return best + (std::abs(denom) > 1e-12 ? 0.5 * (a - c) / denom : 0.0);
}

double midiToHz(double m) { return 440.0 * std::pow(2.0, (m - 69.0) / 12.0); }

// Pitch track in cents relative to `key` (window 2048, hop `hop`).
std::vector<double> centsTrack(const juce::AudioBuffer<float>& a, double rate, int key, int from, int to, int hop = 256, int window = 2048)
{
    std::vector<double> out;
    const double expected = rate / midiToHz(key);
    const float* x = a.getReadPointer(0);
    for (int i = from; i + window + (int) (expected * 1.3) + 2 < to; i += hop)
        out.push_back(1200.0 * std::log2(expected / periodNear(x, i, window, expected)));
    return out;
}

// Magnitude spectrum (Blackman-Harris window) of `size` samples from `from`, in dB.
std::vector<double> spectrumDb(const juce::AudioBuffer<float>& a, int from, int log2size)
{
    const int size = 1 << log2size;
    dsp::Fft fft(log2size);
    std::vector<std::complex<float>> data((size_t) size);
    double windowSum = 0;
    for (int i = 0; i < size; ++i)
    {
        const double t = 2.0 * 3.14159265358979 * i / (size - 1);
        const double w = 0.35875 - 0.48829 * std::cos(t) + 0.14128 * std::cos(2 * t) - 0.01168 * std::cos(3 * t);
        windowSum += w;
        data[(size_t) i] = {(float) (a.getSample(0, from + i) * w), 0.0f};
    }
    fft.transform(data);
    std::vector<double> db((size_t) size / 2);
    for (int i = 0; i < size / 2; ++i) db[(size_t) i] = 20.0 * std::log10(std::max(1e-12, (double) std::abs(data[(size_t) i]) * 2.0 / windowSum));
    return db;
}

// Harmonic amplitudes (dB) of a steady note: the spectrum's maximum within +-2 bins of each k f0.
std::vector<double> harmonicsDb(const std::vector<double>& spectrum, double rate, double f0, double maxHz)
{
    const double bin = rate / (2.0 * (double) spectrum.size());
    std::vector<double> out;
    for (int k = 1; k * f0 < maxHz; ++k)
    {
        const int centre = (int) std::lround(k * f0 / bin);
        double best = -300;
        for (int b = centre - 2; b <= centre + 2; ++b)
            if (b > 0 && b < (int) spectrum.size()) best = std::max(best, spectrum[(size_t) b]);
        out.push_back(best);
    }
    return out;
}

// The envelope peak nearest `target` Hz among the harmonics' local maxima, refined by a parabola.
double envelopePeakNear(const std::vector<double>& h, double f0, double target)
{
    double best = -1, bestDistance = 1e9;
    for (size_t k = 1; k + 1 < h.size(); ++k)
    {
        if (h[k] < h[k - 1] || h[k] < h[k + 1]) continue;
        const double denom = h[k - 1] - 2 * h[k] + h[k + 1];
        const double shift = std::abs(denom) > 1e-9 ? std::clamp(0.5 * (h[k - 1] - h[k + 1]) / denom, -0.5, 0.5) : 0.0;
        const double hz = ((double) k + 1.0 + shift) * f0;
        if (std::abs(hz - target) < bestDistance) { bestDistance = std::abs(hz - target); best = hz; }
    }
    return best;
}

double correlation(const juce::AudioBuffer<float>& a, int from, int to)
{
    double lr = 0, ll = 0, rr = 0;
    for (int i = from; i < to; ++i)
    {
        const double l = a.getSample(0, i), r = a.getSample(1, i);
        lr += l * r;
        ll += l * l;
        rr += r * r;
    }
    return lr / std::sqrt(std::max(1e-30, ll * rr));
}
} // namespace

// ---------------------------------------------------------------- formants and source

TEST_CASE("seraph: the formant table interpolates in log frequency, Hum closes the mouth, F1 follows a high fundamental")
{
    sf::FormantShape a;
    a.vowel = 0;
    const auto bassA = sf::formants(sf::Bass, a);
    CHECK_NEAR(bassA.hz[0], 600, 0.5);
    CHECK_NEAR(bassA.hz[2], 2250, 1);
    a.vowel = 0.5f; // halfway A..E: the geometric mean
    const auto half = sf::formants(sf::Bass, a);
    CHECK_NEAR(half.hz[1], std::sqrt(1040.0 * 1620.0), 2);
    a.vowel = 0;
    a.hum = 1;
    const auto hummed = sf::formants(sf::Bass, a);
    CHECK_NEAR(hummed.hz[0], 250, 1);
    CHECK_NEAR(20 * std::log10(hummed.gain[1] / bassA.gain[1]), -24, 0.1);
    sf::FormantShape high;
    high.f0 = 880;
    const auto soprano = sf::formants(sf::Soprano, high);
    CHECK(soprano.hz[0] >= 880);
    CHECK(soprano.hz[1] > soprano.hz[0]);
}

TEST_CASE("seraph: the glottal pulse tables are band-limited per octave and fall about 6 dB per octave")
{
    auto tables = sf::GlottalTables::forRate(44100.0);
    REQUIRE(tables != nullptr);
    CHECK(sf::GlottalTables::forRate(44100.0) == tables); // shared
    const auto& modal = sf::GlottalTables::harmonics(3);
    const double slope = 20 * std::log10(modal[31] / modal[7]) / 2.0; // dB per octave from harmonic 8 to 32
    CHECK(slope < -4.0);
    CHECK(slope > -14.0);
    const auto& pressed = sf::GlottalTables::harmonics(0);
    const auto& breathy = sf::GlottalTables::harmonics(7);
    CHECK(pressed[15] / pressed[0] > breathy[15] / breathy[0] * 4.0); // pressed is brighter
    CHECK(sf::GlottalTables::octaveFor(30) == 0);
    CHECK(sf::GlottalTables::octaveFor(1046.5f) == 5);
}

// ---------------------------------------------------------------- pitch, formants, aliasing, loudness

TEST_CASE("seraph: with every human control at zero, the fundamental is within 3 cents of the key across each range")
{
    constexpr double rate = 48000;
    const int solos[4] = {S::SoloBass, S::SoloTenor, S::SoloAlto, S::SoloSoprano};
    for (int type = 0; type < 4; ++type)
        for (int key : {sf::kRangeLow[type], (sf::kRangeLow[type] + sf::kRangeHigh[type]) / 2, sf::kRangeHigh[type]})
        {
            auto s = makeDry(rate, solos[type]);
            const auto out = render(*s, rate, 0.8, {{0.0, 0.8, key}});
            const double expected = rate / midiToHz(key);
            const double period = periodNear(out.getReadPointer(0), 12000, 16384, expected);
            const double cents = 1200.0 * std::log2(expected / period);
            if (std::abs(cents) > 3.0) std::printf("    type %d key %d: %.2f cents\n", type, key, cents);
            CHECK(std::abs(cents) < 3.0);
        }
}

namespace
{
// The table's composite response (the five resonators with their signs) in dB at f.
double tableResponseDb(int type, float vowel, float size, double f0, double f)
{
    sf::FormantShape shape;
    shape.vowel = vowel;
    shape.scale = 1.0f - size;
    shape.f0 = (float) f0;
    const auto r = sf::formants(type, shape);
    std::complex<double> h{};
    for (int i = 0; i < sf::kFormants; ++i)
        h += (i % 2 == 0 ? 1.0 : -1.0) * r.gain[i] * std::complex<double>(0, f * r.bw[i]) / std::complex<double>(r.hz[i] * r.hz[i] - f * f, f * r.bw[i]);
    return 20 * std::log10(std::abs(h));
}

// The spectral envelope of a dry, steady note: each harmonic's level minus the glottal source's own level there (Tension
// 50 %: halfway between pulse shapes 3 and 4), so what is left is the vocal tract.
std::vector<double> measuredEnvelope(int section, int key, float vowel, float size)
{
    constexpr double rate = 48000;
    auto s = makeDry(rate, section);
    s->setParameter(S::Vowel, vowel);
    s->setParameter(S::Size, size);
    const auto out = render(*s, rate, 1.2, {{0.0, 1.2, key}});
    const auto h = harmonicsDb(spectrumDb(out, 16000, 15), rate, midiToHz(key), 5000);
    const auto& a = sf::GlottalTables::harmonics(3);
    const auto& b = sf::GlottalTables::harmonics(4);
    std::vector<double> env;
    for (size_t k = 0; k < h.size(); ++k) env.push_back(h[k] - 20 * std::log10(0.5 * (a[k] + b[k])));
    return env;
}
} // namespace

TEST_CASE("seraph: the spectral envelope is the table's: F1-F3 peaks within 6 %, A and U differ as the table says, Size moves them")
{
    // Notes low enough for the harmonics to sample the envelope finely: D1 (37 Hz), with Fold off so the voices strain down to it.
    struct Case
    {
        int section, type, key;
        float vowel, size;
        const char* name;
    };
    const Case cases[] = {{S::SoloBass, sf::Bass, 26, 0, 0, "bass A"},       {S::SoloBass, sf::Bass, 26, 4, 0, "bass U"},
                          {S::SoloSoprano, sf::Soprano, 26, 0, 0, "soprano A"}, {S::SoloSoprano, sf::Soprano, 26, 4, 0, "soprano U"},
                          {S::SoloBass, sf::Bass, 26, 0, 0.1f, "bass A, Size +10 % (a larger throat: formants 10 % lower)"}};
    for (const auto& c : cases)
    {
        const double f0 = midiToHz(c.key);
        const auto env = measuredEnvelope(c.section, c.key, c.vowel, c.size);
        // the measured envelope follows the table's response (aligned by their difference at the loudest harmonic)
        size_t loudest = 0;
        for (size_t k = 0; k < env.size(); ++k)
            if (tableResponseDb(c.type, c.vowel, c.size, f0, (double) (k + 1) * f0) > tableResponseDb(c.type, c.vowel, c.size, f0, (double) (loudest + 1) * f0)) loudest = k;
        const double offset = env[loudest] - tableResponseDb(c.type, c.vowel, c.size, f0, (double) (loudest + 1) * f0);
        double worst = 0;
        for (size_t k = 0; k < env.size(); ++k)
        {
            const double expected = tableResponseDb(c.type, c.vowel, c.size, f0, (double) (k + 1) * f0);
            if (expected < tableResponseDb(c.type, c.vowel, c.size, f0, (double) (loudest + 1) * f0) - 30) continue; // deep valleys
            worst = std::max(worst, std::abs(env[k] - offset - expected));
        }
        // the peaks: every table formant of F1-F3 that is a peak of the composite response
        const auto& row = sf::kFormantTable[c.type][(int) c.vowel];
        std::string peaks;
        for (int i = 0; i < 3; ++i)
        {
            const double target = row.hz[i] * (1.0 - c.size);
            bool visible = false;
            for (double f = target * 0.94; f <= target * 1.06; f += 2)
                if (tableResponseDb(c.type, c.vowel, c.size, f0, f) > tableResponseDb(c.type, c.vowel, c.size, f0, f - 2)
                    && tableResponseDb(c.type, c.vowel, c.size, f0, f) > tableResponseDb(c.type, c.vowel, c.size, f0, f + 2))
                    visible = true;
            if (! visible) { peaks += " F" + std::to_string(i + 1) + " (no peak in the table's response)"; continue; }
            // where the composite response peaks near this formant (neighbours pull it: soprano A's F2 sits at 1085, not 1150)
            double composite = target, best = 1e9;
            for (double f = target * 0.85; f <= target * 1.15; f += 1)
                if (tableResponseDb(c.type, c.vowel, c.size, f0, f) > tableResponseDb(c.type, c.vowel, c.size, f0, f - 1)
                    && tableResponseDb(c.type, c.vowel, c.size, f0, f) > tableResponseDb(c.type, c.vowel, c.size, f0, f + 1)
                    && std::abs(f - target) < best)
                {
                    best = std::abs(f - target);
                    composite = f;
                }
            const double found = envelopePeakNear(env, f0, composite);
            peaks += " F" + std::to_string(i + 1) + " " + std::to_string((int) found) + " (response " + std::to_string((int) composite)
                     + ", table " + std::to_string((int) target) + ")";
            CHECK(std::abs(found / composite - 1.0) < 0.06);
            CHECK(std::abs(composite / target - 1.0) < 0.1);
        }
        std::printf("    %s: within %.1f dB of the table's response;%s\n", c.name, worst, peaks.c_str());
        CHECK(worst < 3.0);
    }
}

TEST_CASE("seraph: a soprano note above F1 has its first peak at the fundamental")
{
    constexpr double rate = 48000;
    auto s = makeDry(rate, S::SoloSoprano);
    const int key = 81; // A5, 880 Hz: above the soprano's A F1 (800 Hz)
    const auto out = render(*s, rate, 1.2, {{0.0, 1.2, key}});
    const auto h = harmonicsDb(spectrumDb(out, 16000, 15), rate, midiToHz(key), 8000);
    REQUIRE(h.size() > 3);
    std::printf("    soprano A5 harmonics: %.1f %.1f %.1f dB\n", h[0], h[1], h[2]);
    CHECK(h[0] > h[1]);
    CHECK(h[0] > h[2]);
}

TEST_CASE("seraph: soprano C6 at 44.1 kHz has everything inharmonic below -60 dB")
{
    constexpr double rate = 44100;
    auto s = makeDry(rate, S::SoloSoprano);
    s->setParameter(S::Tension, 1.0f); // pressed: the brightest pulse
    const int key = 84;
    const auto out = render(*s, rate, 1.2, {{0.0, 1.2, key}});
    const auto spectrum = spectrumDb(out, 12000, 15);
    const double f0 = midiToHz(key), bin = rate / 32768.0;
    double top = -300, inharmonic = -300, worstHz = 0;
    for (size_t b = 3; b < spectrum.size(); ++b)
    {
        const double hz = (double) b * bin;
        top = std::max(top, spectrum[b]);
        const double k = std::round(hz / f0);
        if (std::abs(hz - k * f0) > 10 * bin && spectrum[b] > inharmonic) { inharmonic = spectrum[b]; worstHz = hz; }
    }
    std::printf("    C6: inharmonic %.1f dB below the top (at %.0f Hz)\n", top - inharmonic, worstHz);
    CHECK(top - inharmonic > 60.0);
}

TEST_CASE("seraph: loudness is within +-3 dB across vowels, voice types and each type's range at one velocity")
{
    constexpr double rate = 48000;
    const int solos[4] = {S::SoloBass, S::SoloTenor, S::SoloAlto, S::SoloSoprano};
    double lo = 1e9, hi = -1e9;
    for (int type = 0; type < 4; ++type)
        for (int key : {sf::kRangeLow[type], (sf::kRangeLow[type] + sf::kRangeHigh[type]) / 2, sf::kRangeHigh[type]})
            for (float vowel : {0.0f, 1.0f, 2.0f, 3.0f, 4.0f})
            {
                auto s = makeDry(rate, solos[type]);
                s->setParameter(S::Vowel, vowel);
                s->setParameter(S::LowCut, 40);
                const auto out = render(*s, rate, 0.5, {{0.0, 0.5, key}});
                const double db = rmsDb(out, 4800, 24000);
                if (db < lo || db > hi) std::printf("    type %d key %d vowel %.0f: %.1f dB\n", type, key, vowel, db);
                lo = std::min(lo, db);
                hi = std::max(hi, db);
            }
    std::printf("    loudness spread %.1f dB (%.1f .. %.1f)\n", hi - lo, lo, hi);
    CHECK(hi - lo <= 6.0);
}

TEST_CASE("seraph: velocity follows Dynamics")
{
    constexpr double rate = 48000;
    auto level = [&](float dynamics, float velocity) {
        auto s = makeDry(rate);
        s->setParameter(S::Dynamics, dynamics);
        const auto out = render(*s, rate, 0.4, {{0.0, 0.4, 57, velocity}});
        return rmsDb(out, 4800, 19000);
    };
    CHECK(std::abs(level(0, 1.0f) - level(0, 0.4f)) < 0.5);
    const double drop = level(1, 1.0f) - level(1, 0.4f);
    std::printf("    Dynamics 100 %%: velocity 1.0 vs 0.4 = %.1f dB\n", drop);
    CHECK(drop > 8.0);
    CHECK(drop < 20.0);
}

// ---------------------------------------------------------------- the human pitch model

TEST_CASE("seraph: vibrato rate and depth measured from the pitch track equal the settings; the delay holds it back")
{
    constexpr double rate = 48000;
    auto s = makeDry(rate);
    s->setParameter(S::Vibrato, 50);
    s->setParameter(S::VibratoRate, 5.5f);
    s->setParameter(S::VibratoDelay, 800);
    const int key = 57;
    const auto out = render(*s, rate, 3.0, {{0.0, 3.0, key}});
    const auto early = centsTrack(out, rate, key, 2400, 30000, 256, 1024);
    double earlyMax = 0;
    for (double c : early) earlyMax = std::max(earlyMax, std::abs(c));
    std::printf("    before the delay: %.1f cents\n", earlyMax);
    CHECK(earlyMax < 6.0);
    const auto late = centsTrack(out, rate, key, (int) (1.4 * rate), (int) (2.9 * rate), 64, 1024);
    REQUIRE(late.size() > 100);
    double mean = 0;
    for (double c : late) mean += c;
    mean /= (double) late.size();
    double sq = 0;
    int crossings = 0;
    for (size_t i = 0; i < late.size(); ++i)
    {
        sq += (late[i] - mean) * (late[i] - mean);
        if (i > 0 && (late[i - 1] - mean < 0) != (late[i] - mean < 0)) ++crossings;
    }
    const double depth = std::sqrt(sq / (double) late.size()) * std::sqrt(2.0);
    const double seconds = (double) late.size() * 64 / rate;
    const double hz = crossings * 0.5 / seconds;
    std::printf("    vibrato: depth %.1f cents (50), rate %.2f Hz (5.5)\n", depth, hz);
    CHECK(std::abs(depth - 50) < 10);
    CHECK(std::abs(hz - 5.5) < 0.4);
}

TEST_CASE("seraph: the scoop starts flat and reaches the pitch within 80 ms with a small overshoot")
{
    constexpr double rate = 48000;
    auto s = makeDry(rate);
    s->setParameter(S::Scoop, 80);
    const int key = 57;
    const auto out = render(*s, rate, 0.5, {{0.0, 0.5, key}});
    const auto track = centsTrack(out, rate, key, 0, (int) (0.4 * rate), 48, 512); // 1 ms hop
    REQUIRE(track.size() > 200);
    double first = 0;
    for (size_t i = 0; i < 6; ++i) first = std::min(first, track[i]);
    double settled = 0, over = 0;
    for (size_t i = 100; i < 200; ++i) settled = std::max(settled, std::abs(track[i]));
    for (size_t i = 40; i < 140; ++i) over = std::max(over, track[i]);
    std::printf("    scoop: %.1f cents at the start, overshoot %.1f, after 100 ms within %.1f\n", first, over, settled);
    CHECK(first < -25);
    CHECK(over > 1.0);
    CHECK(over < 9.0);
    CHECK(settled < 7.0);
}

TEST_CASE("seraph: jitter and shimmer at Human 100 % are inside 0.3-1 % and 1-4 %")
{
    constexpr double rate = 48000;
    auto s = makeDry(rate, S::SoloBass);
    s->setParameter(S::Human, 1.0f);
    s->setParameter(S::Vowel, 4.0f); // U: the fundamental dominates, periods are easy to mark
    const int key = 45;              // 110 Hz
    const auto out = render(*s, rate, 2.0, {{0.0, 2.0, key}});
    const float* x = out.getReadPointer(0);
    // Period marks: the largest positive sample of each period, searched around the expected position.
    const double expected = rate / midiToHz(key);
    std::vector<double> marks, amplitudes;
    int i = (int) (0.3 * rate);
    {
        int best = i;
        for (int j = i; j < i + (int) expected; ++j)
            if (x[j] > x[best]) best = j;
        i = best;
    }
    while (i + (int) (expected * 1.5) < (int) (1.9 * rate))
    {
        int best = i + (int) (expected * 0.8);
        for (int j = best; j < i + (int) (expected * 1.2); ++j)
            if (x[j] > x[best]) best = j;
        const double a = x[best - 1], b = x[best], c = x[best + 1];
        const double denom = a - 2 * b + c;
        marks.push_back(best + (std::abs(denom) > 1e-12 ? 0.5 * (a - c) / denom : 0.0));
        float low = 0;
        for (int j = i; j < best; ++j) low = std::min(low, x[j]);
        amplitudes.push_back(b - low);
        i = best;
    }
    REQUIRE(marks.size() > 100);
    double jitter = 0, meanPeriod = 0, shimmer = 0, meanAmp = 0;
    for (size_t k = 2; k < marks.size(); ++k)
    {
        const double p0 = marks[k - 1] - marks[k - 2], p1 = marks[k] - marks[k - 1];
        jitter += std::abs(p1 - p0);
        meanPeriod += p1;
        shimmer += std::abs(amplitudes[k] - amplitudes[k - 1]);
        meanAmp += amplitudes[k];
    }
    const double count = (double) marks.size() - 2;
    const double jitterPct = 100 * (jitter / count) / (meanPeriod / count);
    const double shimmerPct = 100 * (shimmer / count) / (meanAmp / count);
    std::printf("    jitter %.2f %%, shimmer %.2f %%\n", jitterPct, shimmerPct);
    CHECK(jitterPct > 0.3);
    CHECK(jitterPct < 1.0);
    CHECK(shimmerPct > 1.0);
    CHECK(shimmerPct < 4.0);
}

TEST_CASE("seraph: drift moves the pitch slowly and stays bounded")
{
    constexpr double rate = 48000;
    auto s = makeDry(rate);
    s->setParameter(S::Drift, 30);
    const int key = 57;
    const auto out = render(*s, rate, 6.0, {{0.0, 6.0, key}});
    const auto track = centsTrack(out, rate, key, 4800, (int) (5.9 * rate), 1024, 2048);
    double lo = 0, hi = 0;
    for (double c : track) { lo = std::min(lo, c); hi = std::max(hi, c); }
    std::printf("    drift: %.1f .. %.1f cents\n", lo, hi);
    CHECK(hi - lo > 8.0);
    CHECK(hi < 33.0);
    CHECK(lo > -33.0);
}

// ---------------------------------------------------------------- ensemble

TEST_CASE("seraph: six singers are not one singer louder: seeds, width, onsets and the sqrt(n) sum")
{
    constexpr double rate = 48000;
    auto choir = [&](int seed, float width, int singers) {
        auto s = std::make_unique<S>();
        s->setParameter(S::Section, S::Boys);
        s->setParameter(S::Singers, (float) singers);
        s->setParameter(S::Space, 0);
        s->setParameter(S::Width, width);
        s->setParameter(S::Seed, (float) seed);
        s->prepare(rate, 512);
        return s;
    };
    auto a = choir(0, 0.7f, 6), b = choir(1, 0.7f, 6), a2 = choir(0, 0.7f, 6);
    const auto ra = render(*a, rate, 2.0, {{0.0, 2.0, 69}});
    const auto rb = render(*b, rate, 2.0, {{0.0, 2.0, 69}});
    const auto ra2 = render(*a2, rate, 2.0, {{0.0, 2.0, 69}});
    double diffSeed = 0, diffSame = 0;
    for (int i = 0; i < ra.getNumSamples(); ++i)
    {
        diffSeed += std::abs(ra.getSample(0, i) - rb.getSample(0, i));
        diffSame += std::abs(ra.getSample(0, i) - ra2.getSample(0, i));
    }
    CHECK(diffSeed > 1.0);
    CHECK(diffSame < 1e-6); // deterministic

    // left/right correlation falls with Width
    auto narrow = choir(0, 0.0f, 6), wide = choir(0, 1.0f, 6);
    const double cNarrow = correlation(render(*narrow, rate, 2.0, {{0.0, 2.0, 69}}), 24000, 96000);
    const double cWide = correlation(render(*wide, rate, 2.0, {{0.0, 2.0, 69}}), 24000, 96000);
    std::printf("    correlation: width 0 %.2f, width 100 %% %.2f\n", cNarrow, cWide);
    CHECK(cNarrow > 0.99);
    CHECK(cWide < 0.6);

    // onsets spread within Timing
    auto timed = choir(0, 0.7f, 6);
    timed->setParameter(S::Timing, 80);
    std::vector<double> firstHeard(64, -1);
    render(*timed, rate, 0.4, {{0.0, 0.4, 69}}, {}, [&](int, double t) {
        for (int v = 0; v < 64; ++v)
            if (firstHeard[(size_t) v] < 0 && timed->singer(v).type >= 0 && timed->singer(v).level > 1e-4f) firstHeard[(size_t) v] = t;
    }, 64);
    double earliest = 1, latest = 0;
    int heard = 0;
    for (double t : firstHeard)
        if (t >= 0) { earliest = std::min(earliest, t); latest = std::max(latest, t); ++heard; }
    std::printf("    onsets: %d singers from %.0f to %.0f ms\n", heard, earliest * 1000, latest * 1000);
    CHECK(heard == 6);
    CHECK(latest - earliest > 0.02);
    CHECK(latest < 0.095);

    // the summed level grows by about sqrt(n): 4 singers about 6 dB over 1
    auto one = choir(0, 0.0f, 1), four = choir(0, 0.0f, 4);
    const double l1 = rmsDb(render(*one, rate, 2.0, {{0.0, 2.0, 69}}), 24000, 96000);
    const double l4 = rmsDb(render(*four, rate, 2.0, {{0.0, 2.0, 69}}), 24000, 96000);
    std::printf("    1 singer %.1f dB, 4 singers %.1f dB (+%.1f)\n", l1, l4, l4 - l1);
    CHECK(l4 - l1 > 4.0);
    CHECK(l4 - l1 < 8.5);
}

// ---------------------------------------------------------------- sections

TEST_CASE("seraph: each voice type sings only inside its range; Fold; Men Octave; the Solo sections use one singer")
{
    S s;
    s.prepare(48000, 512);
    for (int section = 0; section < S::kSections; ++section)
    {
        s.setParameter(S::Section, (float) section);
        s.setParameter(S::Fold, 1);
        for (int key = 30; key <= 100; ++key)
            for (int type : s.typesFor(key))
            {
                int low, high;
                sf::range(type == sf::kVoiceTypes ? sf::Soprano : type, type == sf::kVoiceTypes, low, high);
                bool fits = false;
                for (int octave = -48; octave <= 48; octave += 12) fits = fits || (key + octave >= low && key + octave <= high && (octave == 0 || ! (key >= low && key <= high)));
                CHECK(fits);
            }
    }
    // Mixed at C4: all four types; a low C2 only the bass
    s.setParameter(S::Section, S::Mixed);
    s.setParameter(S::Singers, 4);
    auto types = s.typesFor(60);
    CHECK((types == std::vector<int>{sf::Bass, sf::Tenor, sf::Alto, sf::Soprano}));
    types = s.typesFor(40);
    CHECK(std::all_of(types.begin(), types.end(), [](int t) { return t == sf::Bass; }));

    constexpr double rate = 48000;
    // Fold: a note below every man's range is sung an octave up; without Fold the bass strains down to it
    auto lowest = [&](bool fold) {
        auto d = std::make_unique<S>();
        d->setParameter(S::Section, S::Men);
        d->setParameter(S::Fold, fold ? 1.0f : 0.0f);
        d->setParameter(S::Space, 0);
        d->setParameter(S::Drift, 0);
        d->setParameter(S::Vibrato, 0);
        d->setParameter(S::Detune, 0);
        d->setParameter(S::Scoop, 0);
        d->prepare(rate, 512);
        double low = 1e9;
        render(*d, rate, 0.5, {{0.0, 0.5, 33}}, {}, [&](int, double t) {
            if (t > 0.3)
                for (int v = 0; v < 64; ++v)
                    if (d->singer(v).type >= 0) low = std::min(low, (double) d->singer(v).pitch);
        });
        return low;
    };
    CHECK_NEAR(lowest(true), 45, 1e-3);
    CHECK_NEAR(lowest(false), 33, 1e-3);

    // Men Octave: the men sing an octave below the women
    auto d = std::make_unique<S>();
    d->setParameter(S::Section, S::Mixed);
    d->setParameter(S::MenOctave, 1);
    d->setParameter(S::Singers, 3);
    d->prepare(rate, 512);
    int women = 0, men = 0;
    render(*d, rate, 0.3, {{0.0, 0.3, 69}}, {}, [&](int start, double) {
        if (start != 0) return;
        for (int v = 0; v < 64; ++v)
        {
            const auto view = d->singer(v);
            if (view.type == sf::Alto || view.type == sf::Soprano) { ++women; CHECK_NEAR(view.pitch, 69, 1e-3); }
            if (view.type == sf::Bass || view.type == sf::Tenor) { ++men; CHECK_NEAR(view.pitch, 57, 1e-3); }
        }
    });
    CHECK(women == 3);
    CHECK(men == 3);

    // Solo: one singer per note
    auto solo = std::make_unique<S>();
    solo->setParameter(S::Section, S::SoloAlto);
    solo->setParameter(S::Singers, 8);
    solo->prepare(rate, 512);
    render(*solo, rate, 0.2, {{0.0, 0.2, 64}, {0.0, 0.2, 67}});
    CHECK(solo->activeSingers() == 2);
}

TEST_CASE("seraph: at most 64 singers; stealing the oldest notes makes no click")
{
    constexpr double rate = 48000;
    auto s = std::make_unique<S>();
    s->setParameter(S::Section, S::Men);
    s->setParameter(S::Singers, 8);
    s->setParameter(S::Vowel, 4);
    s->setParameter(S::Space, 0);
    s->setParameter(S::Attack, 5);
    s->setParameter(S::Timing, 0);
    s->setParameter(S::Release, 3000);
    s->setParameter(S::Output, -6);
    s->prepare(rate, 512);
    std::vector<Note> notes;
    for (int i = 0; i < 16; ++i) notes.push_back({0.05 * i, 0.3 + 0.05 * i, 40 + i, 0.9f}); // 128 singers wanted
    int most = 0;
    const auto out = render(*s, rate, 1.5, notes, {}, [&](int, double) { most = std::max(most, s->activeSingers()); });
    CHECK(most <= 64);
    CHECK(most >= 60);
    double step = 0;
    for (int ch = 0; ch < 2; ++ch)
        for (int i = 1; i < out.getNumSamples(); ++i) step = std::max(step, (double) std::abs(out.getSample(ch, i) - out.getSample(ch, i - 1)));
    std::printf("    largest sample step %.3f, peak %.2f\n", step, peak(out));
    CHECK(step < 0.2);
}

TEST_CASE("seraph: in Legato overlapping notes glide in Glide without a new attack")
{
    constexpr double rate = 48000;
    auto s = makeDry(rate);
    s->setParameter(S::Mode, S::Legato);
    s->setParameter(S::Glide, 200);
    s->setParameter(S::Attack, 300);
    const auto out = render(*s, rate, 2.0, {{0.0, 1.5, 57}, {1.0, 1.0, 60}});
    // the pitch moves from A3 towards C4 over about 200 ms
    const auto track = centsTrack(out, rate, 57, (int) (0.9 * rate), (int) (1.6 * rate), 240, 1024);
    REQUIRE(track.size() > 100);
    CHECK(std::abs(track[0]) < 15);                // A3 before the second note
    CHECK(std::abs(track[60] - 300) < 20);         // C4 300 ms after it
    const double mid = track[(size_t) (0.2 * rate / 240.0)]; // 100 ms after the second note: halfway
    CHECK(mid > 50);
    CHECK(mid < 250);
    // no new attack: the level does not dip at the second note
    CHECK(rmsDb(out, (int) (1.0 * rate), (int) (1.1 * rate)) > rmsDb(out, (int) (0.8 * rate), (int) (0.95 * rate)) - 2.0);
}

// ---------------------------------------------------------------- chant

TEST_CASE("seraph: every chant phrase stays in the scale for 5 scales and 12 keys")
{
    REQUIRE(S::phrases().size() >= 16);
    for (int scale : {0, 1, 5, 13, 16})
        for (int key = 0; key < 12; ++key)
            for (const auto& phrase : S::phrases())
                for (int held = 36; held <= 84; ++held)
                    for (int step = 0; step < phrase.count; ++step)
                    {
                        const int pitch = S::chantPitch(held, phrase, step, key, scale);
                        if (! model::inScale(pitch, key, scale))
                        {
                            CHECK(false);
                            return;
                        }
                        CHECK(std::abs(pitch - held) <= 12);
                    }
    // phrase timing: Neighbour (0 +1 0 | 2 1 1)
    const auto& neighbour = S::phrases()[0];
    int step;
    long long absolute;
    S::phraseStepAt(neighbour, 1.5, step, absolute);
    CHECK(step == 0);
    S::phraseStepAt(neighbour, 2.5, step, absolute);
    CHECK(step == 1);
    S::phraseStepAt(neighbour, 4.2, step, absolute);
    CHECK(step == 0);
    CHECK(absolute == 3);
}

TEST_CASE("seraph: the chant follows the song position (locate, loop), organum is exact, the ison holds while a note is held")
{
    constexpr double rate = 48000;
    auto make = [&]() {
        auto s = std::make_unique<S>();
        s->setParameter(S::Section, S::Men);
        s->setParameter(S::Singers, 2);
        s->setParameter(S::Chant, 1);
        s->setParameter(S::PhraseParam, 2); // Rise: 0 +1 +2 | 1 1 2
        s->setParameter(S::ChantRate, 0);   // one beat per unit
        s->setParameter(S::Key, 9);
        s->setParameter(S::Scale, 1);       // A minor
        s->setParameter(S::Organum, S::Fifth);
        s->setParameter(S::Ison, S::IsonRoot);
        s->setParameter(S::Glide, 0);
        s->setParameter(S::Space, 0);
        s->prepare(rate, 512);
        return s;
    };
    // playing from beat 1 (unit 1: +1 step = B3 over a held A3) and from beat 5 (the cycle repeats every 4 beats)
    for (double startBeat : {1.0, 5.0, 9.0})
    {
        auto s = make();
        Clock clock;
        clock.playing = true;
        clock.startBeat = startBeat;
        std::vector<float> principal, organum, ison;
        render(*s, rate, 0.2, {{0.0, 0.2, 57}}, clock, [&](int start, double) {
            if (start != 4096) return;
            for (int v = 0; v < 64; ++v)
            {
                const auto view = s->singer(v);
                if (view.type < 0) continue;
                const float p = view.pitch;
                if (p >= 40 && p <= 51.5f && std::abs(p - 59) > 0.01f) ison.push_back(p);
                else if (std::abs(p - 59) < 0.01f) principal.push_back(p);
                else organum.push_back(p);
            }
        });
        CHECK(principal.size() == 2);
        CHECK(organum.size() == 1);
        for (float p : organum) CHECK_NEAR(p, 66, 1e-4); // B3 + a fifth
        CHECK(ison.size() == 3);
        for (float p : ison) CHECK_NEAR(p, 45, 1e-4); // A2
    }
    // the ison stops after the note is released
    auto s = make();
    s->setParameter(S::Release, 100);
    render(*s, rate, 2.0, {{0.0, 0.5, 57}});
    CHECK(s->activeSingers() == 0);
    // a loop: jumping back to an earlier beat sings that beat's note again
    auto looped = make();
    Clock clock;
    clock.playing = true;
    clock.startBeat = 2.0; // unit 2: +2 steps = C4
    float first = 0;
    render(*looped, rate, 0.1, {{0.0, 0.5, 57}}, clock, [&](int start, double) {
        if (start == 2048) first = looped->singer(0).pitch;
    });
    CHECK_NEAR(first, 60, 1e-4);
}

TEST_CASE("seraph: chant phrase notes are joined with Glide")
{
    constexpr double rate = 48000;
    auto s = makeDry(rate, S::SoloTenor);
    s->setParameter(S::Chant, 1);
    s->setParameter(S::PhraseParam, 2); // Rise
    s->setParameter(S::ChantRate, 0);
    s->setParameter(S::Key, 9);
    s->setParameter(S::Scale, 1);
    s->setParameter(S::Glide, 150);
    Clock clock;
    clock.playing = true;
    const auto out = render(*s, rate, 1.0, {{0.0, 1.0, 57}}, clock); // beat 1 (B3) at 0.5 s
    const auto track = centsTrack(out, rate, 57, (int) (0.4 * rate), (int) (0.9 * rate), 240, 1024);
    REQUIRE(track.size() > 60);
    double largest = 0;
    for (size_t i = 1; i < track.size(); ++i) largest = std::max(largest, std::abs(track[i] - track[i - 1]));
    std::printf("    chant glide: largest 5 ms pitch step %.1f cents, end %.0f cents\n", largest, track.back());
    CHECK(largest < 60);
    CHECK(std::abs(track.back() - 200) < 15);
}

// ---------------------------------------------------------------- space, gate, onsets

namespace
{
// RT60 from the Schroeder backward integral of the tail (-5..-25 dB fit, extrapolated).
double decaySeconds(const juce::AudioBuffer<float>& a, int from, double rate)
{
    std::vector<double> energy((size_t) (a.getNumSamples() - from));
    double sum = 0;
    for (int i = a.getNumSamples() - 1; i >= from; --i)
    {
        const double l = a.getSample(0, i), r = a.getSample(1, i);
        sum += l * l + r * r;
        energy[(size_t) (i - from)] = sum;
    }
    const double top = energy[0];
    int t5 = -1, t25 = -1;
    for (size_t i = 0; i < energy.size(); ++i)
    {
        const double db = 10 * std::log10(std::max(1e-30, energy[i] / top));
        if (t5 < 0 && db < -5) t5 = (int) i;
        if (t25 < 0 && db < -25) { t25 = (int) i; break; }
    }
    if (t5 < 0 || t25 < 0) return -1;
    return (t25 - t5) / rate * 3.0;
}
} // namespace

TEST_CASE("seraph: the space decays in the time of each size; Distance lowers the direct sound")
{
    constexpr double rate = 48000;
    for (int size = 0; size < S::kSpaceSizes; ++size)
    {
        // a short A minor chord of the mixed choir: many partials, so no single mode of the hall decides
        auto s = makeDry(rate, S::Mixed);
        s->setParameter(S::Singers, 4);
        s->setParameter(S::Space, 1.0f);
        s->setParameter(S::SpaceSize, (float) size);
        s->setParameter(S::Release, 20);
        const double seconds = S::kSpaceSeconds[size] * 1.3 + 1.0;
        const auto out = render(*s, rate, seconds, {{0.0, 0.15, 57}, {0.0, 0.15, 60}, {0.0, 0.15, 64}});
        const double rt = decaySeconds(out, (int) (0.4 * rate), rate);
        std::printf("    %s: %.2f s (%.1f)\n", S::kSpaceSizeNames[size], rt, (double) S::kSpaceSeconds[size]);
        CHECK(std::abs(rt / S::kSpaceSeconds[size] - 1.0) < 0.2);
    }
    auto direct = [&](float distance) {
        auto s = makeDry(rate);
        s->setParameter(S::Space, 0.35f);
        s->setParameter(S::Distance, distance);
        const auto out = render(*s, rate, 0.2, {{0.0, 0.2, 57}});
        return rmsDb(out, 100, 480); // the first 10 ms: before any reflection
    };
    const double near = direct(0), far = direct(1);
    std::printf("    direct sound: near %.1f dB, far %.1f dB\n", near, far);
    CHECK(near - far > 6.0);
}

TEST_CASE("seraph: the gate before the space leaves tails in the gaps, after the space cuts them")
{
    constexpr double rate = 48000;
    auto gaps = [&](int position) {
        auto s = std::make_unique<S>();
        s->setParameter(S::Section, S::Women);
        s->setParameter(S::Gate, 1);
        s->setParameter(S::GatePattern, 1); // Eighths: every second sixteenth closed
        s->setParameter(S::GateLength, 1.0f);
        s->setParameter(S::GateSmooth, 2);
        s->setParameter(S::GatePosition, (float) position);
        s->setParameter(S::Space, 0.6f);
        s->setParameter(S::SpaceSize, 3);
        s->prepare(rate, 512);
        Clock clock;
        clock.playing = true;
        const auto out = render(*s, rate, 3.0, {{0.0, 3.0, 69}}, clock);
        // at 120 BPM a sixteenth is 6000 samples; closed steps are odd sixteenths
        double open = 0, closed = 0;
        for (int step = 8; step < 22; ++step)
        {
            const double db = rmsDb(out, step * 6000 + 1000, step * 6000 + 5000);
            (step % 2 == 0 ? open : closed) += db / 7.0;
        }
        return open - closed;
    };
    const double before = gaps(S::BeforeSpace), after = gaps(S::AfterSpace);
    std::printf("    open - closed: before the space %.1f dB, after %.1f dB\n", before, after);
    CHECK(before < 15.0);
    CHECK(after > 40.0);
}

TEST_CASE("seraph: an H onset is noise before the voice; M and N start hummed and reach the vowel")
{
    constexpr double rate = 48000;
    {
        auto s = makeDry(rate);
        s->setParameter(S::Onset, S::H);
        const int key = 69;
        const auto out = render(*s, rate, 0.4, {{0.0, 0.4, key}});
        // harmonicity: energy on the harmonics over energy halfway between them (dB), Hann window of 1024
        auto harmonicity = [&](int from) {
            dsp::Fft fft(10);
            std::vector<std::complex<float>> data(1024);
            for (int i = 0; i < 1024; ++i)
                data[(size_t) i] = {(float) (out.getSample(0, from + i) * (0.5 - 0.5 * std::cos(2 * 3.14159265358979 * i / 1023))), 0.0f};
            fft.transform(data);
            const double bin = rate / 1024.0, f0 = midiToHz(key);
            double on = 0, off = 0;
            for (int k = 1; k < 10; ++k)
            {
                on += std::norm(data[(size_t) std::lround(k * f0 / bin)]);
                off += std::norm(data[(size_t) std::lround((k + 0.5) * f0 / bin)]);
            }
            return 10 * std::log10(on / off);
        };
        const double breath = harmonicity(240), voice = harmonicity(12000);
        std::printf("    H: harmonicity %.1f dB in the breath, %.1f dB after\n", breath, voice);
        CHECK(rmsDb(out, 240, 1264) > -80);
        CHECK(breath < 6);
        CHECK(voice > 20);
    }
    for (int onset : {S::M, S::N})
    {
        auto s = makeDry(rate);
        s->setParameter(S::Onset, (float) onset);
        s->setParameter(S::Vowel, 0);
        const auto out = render(*s, rate, 0.5, {{0.0, 0.5, 57}});
        auto highShare = [&](int from) { // energy above 1.2 kHz relative to all, dB
            const auto spec = spectrumDb(out, from, 11);
            double hi = 0, all = 0;
            for (size_t b = 1; b < spec.size(); ++b)
            {
                const double e = std::pow(10.0, spec[b] / 10.0);
                all += e;
                if ((double) b * rate / 2048.0 > 1200) hi += e;
            }
            return 10 * std::log10(hi / all);
        };
        const double hummed = highShare(240), open = highShare((int) (0.2 * rate)), steady = highShare((int) (0.4 * rate));
        std::printf("    %s: high share %.1f dB hummed, %.1f at 200 ms, %.1f steady\n", onset == S::M ? "M" : "N", hummed, open, steady);
        CHECK(hummed < steady - 10);
        CHECK(std::abs(open - steady) < 3);
    }
}

// ---------------------------------------------------------------- robustness

TEST_CASE("seraph: silence after the notes and the tail; no denormals; no NaN across random parameters")
{
    constexpr double rate = 44100;
    auto s = std::make_unique<S>();
    s->setParameter(S::SpaceSize, 1);
    s->prepare(rate, 512);
    const auto out = render(*s, rate, 8.0, {{0.0, 0.5, 60}, {0.0, 0.5, 64}});
    const int tailEnd = (int) ((0.5 + 1.0 + 2.5 * 1.6 + 0.5 + 0.3) * rate); // release to -80 dB with timing and attack, the tail
    double after = 0;
    int subnormal = 0;
    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < out.getNumSamples(); ++i)
        {
            const float v = out.getSample(ch, i);
            if (i > tailEnd) after = std::max(after, (double) std::abs(v));
            if (std::fpclassify(v) == FP_SUBNORMAL) ++subnormal;
        }
    CHECK(after == 0.0);
    CHECK(subnormal == 0);

    std::mt19937 rng(7);
    for (int run = 0; run < 24; ++run)
    {
        auto f = std::make_unique<S>();
        const auto& infos = f->parameters();
        for (int p = 0; p < S::NumParams; ++p)
        {
            std::uniform_real_distribution<float> d(infos[(size_t) p].minValue, infos[(size_t) p].maxValue);
            f->setParameter(p, d(rng));
        }
        f->setParameter(S::Output, 0);
        f->prepare(run % 2 ? 96000.0 : 44100.0, 512);
        std::vector<Note> notes;
        std::uniform_int_distribution<int> key(20, 108);
        for (int n = 0; n < 6; ++n) notes.push_back({0.05 * n, 0.4, key(rng), 0.3f + 0.1f * (float) n});
        Clock clock;
        clock.playing = run % 3 == 0;
        const auto o = render(*f, run % 2 ? 96000.0 : 44100.0, 1.0, notes, clock);
        bool finite = true;
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < o.getNumSamples(); ++i) finite = finite && std::isfinite(o.getSample(ch, i));
        CHECK(finite);
        CHECK(peak(o) <= 1.0);
    }
}

namespace
{
struct SmallStackRender
{
    bool ok = false;
    static void* entry(void* arg)
    {
        auto& self = *static_cast<SmallStackRender*>(arg);
        auto s = std::make_unique<S>();
        s->setParameter(S::Chant, 1);
        s->setParameter(S::Ison, S::IsonRootFifth);
        s->setParameter(S::Organum, S::FifthOctave);
        s->setParameter(S::Gate, 1);
        s->prepare(48000, 512);
        juce::AudioBuffer<float> audio(2, 512);
        juce::MidiBuffer midi;
        for (int k = 0; k < 8; ++k) midi.addEvent(juce::MidiMessage::noteOn(1, 50 + k * 3, (juce::uint8) 100), k);
        for (int pass = 0; pass < 8; ++pass)
        {
            audio.clear();
            s->process(audio, midi);
            midi.clear();
        }
        s->reset();
        self.ok = true;
        return nullptr;
    }
};
} // namespace

TEST_CASE("seraph: processing fits a 256 KB thread stack")
{
    SmallStackRender run;
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    CHECK(pthread_attr_setstacksize(&attributes, 256 * 1024) == 0);
    pthread_t thread;
    REQUIRE(pthread_create(&thread, &attributes, &SmallStackRender::entry, &run) == 0);
    pthread_join(thread, nullptr);
    pthread_attr_destroy(&attributes);
    CHECK(run.ok);
}

// ---------------------------------------------------------------- presets

TEST_CASE("seraph: at least 64 presets in the spec's categories; each applies, sounds under 0 dBFS and is recognised")
{
    const auto& list = S::presets();
    CHECK(list.size() >= 64);
    std::map<std::string, int> counts;
    for (const auto& p : list) ++counts[p.category];
    const std::map<std::string, int> wanted = {{"Angels", 12}, {"Monks", 12}, {"Cathedral", 10}, {"Boys", 4},
                                               {"Solo", 8},    {"Chants", 10}, {"Gated", 10},    {"Beds", 6}, {"Real", 5}, {"Real Choir", 6}};
    for (const auto& [category, count] : wanted)
    {
        if (counts[category] < count) std::printf("    %s: %d presets (%d wanted)\n", category.c_str(), counts[category], count);
        CHECK(counts[category] >= count);
    }
    for (const auto& p : list)
        CHECK(std::find(S::presetCategories().begin(), S::presetCategories().end(), p.category) != S::presetCategories().end());
    // names are unique
    for (size_t a = 0; a < list.size(); ++a)
        for (size_t b = a + 1; b < list.size(); ++b) CHECK(std::string(list[a].name) != list[b].name);

    constexpr double rate = 44100;
    double loudest = 0;
    std::string loudestName;
    S fresh;
    CHECK(fresh.matchingPreset() == 0 || std::string(list[(size_t) fresh.matchingPreset()].name) == "Choir"); // the defaults are a preset
    for (size_t i = 0; i < list.size(); ++i)
    {
        auto s = std::make_unique<S>();
        s->applyPreset((int) i);
        CHECK(s->matchingPreset() == (int) i);
        CHECK(s->presetName() == list[i].name);
        s->prepare(rate, 512);
        const bool line = std::string(list[i].category) == "Solo" || std::string(list[i].category) == "Chants";
        std::vector<Note> notes;
        if (line) notes = {{0.0, 3.0, 57, 1.0f}};
        else notes = {{0.0, 3.0, 57, 1.0f}, {0.0, 3.0, 60, 1.0f}, {0.0, 3.0, 64, 1.0f}};
        Clock clock;
        clock.playing = true;
        clock.tempo = 138;
        const auto out = render(*s, rate, 3.5, notes, clock);
        const double top = peak(out), level = rmsDb(out, (int) (1.0 * rate), (int) (3.0 * rate));
        if (top > loudest) { loudest = top; loudestName = list[i].name; }
        if (std::getenv("SERAPH_LEVELS") || top >= 0.98 || level < -45) std::printf("    %s: peak %.2f, %.1f dB RMS\n", list[i].name, top, level);
        CHECK(level > -45);
        CHECK(top < 0.98);
    }
    std::printf("    loudest preset: %s, peak %.2f\n", loudestName.c_str(), loudest);
    // a changed setting is no preset any more
    S edited;
    edited.applyPreset(3);
    edited.setParameter(S::Breath, 0.99f);
    CHECK(edited.matchingPreset() == -1);
}

// ---------------------------------------------------------------- registration and state

TEST_CASE("seraph: catalog, factory, the VST3 state with the custom gate, the stand-in for a missing VST3")
{
    const auto* entry = model::findCatalogEntry("SonoraSeraph");
    REQUIRE(entry != nullptr);
    CHECK(std::string(entry->displayName) == "Sonora Seraph");
    CHECK(entry->role == model::DeviceRole::Instrument);
    CHECK(model::deviceCategory(*entry) == "Synthesis");
    auto d = devices::nativeDeviceModel("SonoraSeraph");
    CHECK(dynamic_cast<S*>(devices::createNative(d).get()) != nullptr);

    S a;
    a.applyPreset(5);
    a.setParameter(S::Vowel, 2.5f);
    a.setParameter(S::GatePattern, (float) S::kCustomPattern);
    const float levels[12] = {1, 0, 0.5f, 1, 0, 0.25f, 1, 1, 0, 0.75f, 0, 1};
    a.setCustomGate(levels, 12);
    const auto bytes = plugins::encodeParameterState(a, "SonoraSeraph", {{"gate", a.gateText()}, {"preset", a.presetName()}});
    S b;
    std::string error;
    std::map<std::string, std::string> attributes;
    REQUIRE(plugins::decodeParameterState(bytes.data(), bytes.size(), b, "SonoraSeraph", "Sonora Seraph", error, &attributes));
    for (int i = 0; i < S::NumParams; ++i) CHECK_NEAR(b.getParameter(i), a.getParameter(i), 1e-6);
    REQUIRE(b.setGateText(attributes["gate"]));
    CHECK(b.customGateSteps() == 12);
    for (int i = 0; i < 12; ++i) CHECK_NEAR(b.customGateLevel(i), levels[i], 1e-4);
    CHECK(attributes["preset"] == S::presets()[5].name);
    CHECK(! b.setGateText("x 1 2"));
    CHECK(! b.setGateText("40 1"));

    // the VST3's class id, and a missing VST3 played by the built-in device from its state
    model::PluginInfo info;
    const auto uid = io::sonoraVst3Uid("SonoraSeraph");
    for (size_t i = 0; i < 4; ++i) info.vst3Uid[i] = uid[i];
    CHECK(plugins::sonoraPluginFor(info, model::DeviceKind::PluginVst3) == plugins::SonoraPlugin::Seraph);
    model::Device vst;
    vst.id = 7;
    vst.kind = model::DeviceKind::PluginVst3;
    vst.type = "PluginDevice";
    vst.displayName = "Sonora Seraph";
    info.name = "Sonora Seraph";
    info.vendor = "Sonora";
    info.isInstrument = true;
    info.state = bytes;
    vst.plugin = info;
    std::vector<std::string> log;
    devices::BuildContext ctx;
    ctx.log = &log;
    auto processor = devices::createDevice(vst, ctx);
    auto* standIn = dynamic_cast<S*>(processor.get());
    REQUIRE(standIn != nullptr);
    CHECK_NEAR(standIn->getParameter(S::Vowel), 2.5f, 1e-6);
    CHECK(standIn->customGateSteps() == 12);
    CHECK(standIn->presetName() == S::presets()[5].name);
}

TEST_CASE("seraph: a set saves and reopens every parameter, the custom gate and the preset name")
{
    const auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("sonora-seraph-als");
    REQUIRE(dir.createDirectory().wasOk());
    const auto destination = dir.getChildFile("Seraph.als");
    std::string error;
    REQUIRE(io::writeAlsDom(*io::createNewSetDom(), destination, error));
    auto result = io::loadAls(destination);
    REQUIRE(result);
    auto& song = *result.song;
    int track = -1;
    for (size_t t = 0; t < song.tracks.size() && track < 0; ++t)
        if (song.tracks[t].kind == model::TrackKind::Midi) track = (int) t;
    REQUIRE(track >= 0);
    model::EditHistory history;
    {
        auto model = devices::nativeDeviceModel("SonoraSeraph");
        model::allocateDeviceIds(song, model);
        model.sourceXml = std::make_shared<const std::string>(io::deviceTemplate(model));
        history.perform(song, model::insertDevice(track, std::move(model)));
    }
    REQUIRE(! song.tracks[(size_t) track].devices.empty());
    auto& inserted = song.tracks[(size_t) track].devices.back();
    devices::BuildContext context;
    auto processor = devices::createDevice(inserted, context);
    auto* seraph = dynamic_cast<S*>(processor.get());
    REQUIRE(seraph != nullptr);
    const auto& list = S::presets();
    const auto custom = std::find_if(list.begin(), list.end(), [](const auto& p) { return std::string(p.name) == "Custom Pulse"; });
    REQUIRE(custom != list.end());
    seraph->applyPreset((int) (custom - list.begin()));
    seraph->setCustomGateStep(3, 0.3f);
    seraph->setParameter(S::Chant, 1);
    std::vector<float> values(S::NumParams);
    for (int i = 0; i < S::NumParams; ++i) values[(size_t) i] = seraph->getParameter(i);
    devices::captureDeviceState(inserted, processor.get());
    const auto content = inserted.nativeContent;
    REQUIRE(content != nullptr);
    devices::captureDeviceState(inserted, processor.get()); // nothing changed: the same content object stays
    CHECK(inserted.nativeContent == content);

    REQUIRE(io::saveAls(song, destination, {}).ok);
    auto loaded = io::loadAls(destination);
    REQUIRE(loaded);
    const auto& reread = loaded.song->tracks[(size_t) track].devices.back();
    CHECK(reread.type == "SonoraSeraph");
    REQUIRE(reread.nativeContent != nullptr);
    CHECK(reread.nativeContent->data == content->data);
    std::vector<std::string> warnings;
    auto reopened = devices::createNative(reread, &warnings);
    CHECK(warnings.empty());
    auto* again = dynamic_cast<S*>(reopened.get());
    REQUIRE(again != nullptr);
    for (int i = 0; i < S::NumParams; ++i) CHECK_NEAR(again->getParameter(i), values[(size_t) i], 1e-4);
    CHECK(again->presetName() == "Custom Pulse");
    CHECK(again->customGateSteps() == (int) custom->gate.size());
    CHECK_NEAR(again->customGateLevel(3), 0.3f, 1e-4);
    CHECK(again->matchingPreset() == -1); // the edited step: no longer the factory pattern
    dir.deleteRecursively();
}

TEST_CASE("seraph: the Gated presets chop audibly (the level swings more than 12 dB within a bar)")
{
    constexpr double rate = 44100;
    for (size_t i = 0; i < S::presets().size(); ++i)
    {
        const auto& p = S::presets()[i];
        if (std::string(p.category) != "Gated") continue;
        auto s = std::make_unique<S>();
        s->applyPreset((int) i);
        s->prepare(rate, 512);
        Clock clock;
        clock.playing = true;
        clock.tempo = 138;
        const auto out = render(*s, rate, 8.0, {{0.0, 8.0, 57}, {0.0, 8.0, 60}, {0.0, 8.0, 64}}, clock);
        // 5 ms RMS frames over bars 2..4: the spread between the loud and the quiet frames
        std::vector<double> frames;
        const int frame = (int) (0.005 * rate);
        for (int at = (int) (60.0 / 138 * 4 * rate); at + frame < (int) (60.0 / 138 * 16 * rate); at += frame) frames.push_back(rmsDb(out, at, at + frame));
        std::sort(frames.begin(), frames.end());
        const double swing = frames[frames.size() * 95 / 100] - frames[frames.size() * 5 / 100];
        std::printf("    %s: %.1f dB between loud and quiet\n", p.name, swing);
        CHECK(swing > 12.0);
    }
}
