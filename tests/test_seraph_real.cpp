// PLG-27b: Sonora Seraph's Real voices. Every recording here is a sung tone synthesised by Seraph's own Voices engine
// (known pitch and formants), so the tests can say what resynthesis must give: the analysis finds the note and one mark
// per period, refuses phrases, chords and noise; playback at +-7 semitones is in tune within 5 cents and keeps the formant
// peaks within 6 %; Size moves the formants and not the pitch; the read position wanders; one file sings as a section;
// several files map by note; the state keeps the files. docs/SERAPH.md "Real voices".
#include "TestFramework.h"
#include "devices/DeviceFactory.h"
#include "devices/native/Seraph.h"
#include "devices/native/SeraphReal.h"
#include "dsp/Fft.h"
#include "plugins/SonoraPluginState.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cmath>
#include <complex>
#include <memory>
#include <pthread.h>

using namespace sonora;
namespace
{
using S = devices::Seraph;
namespace sf = devices::seraph;
constexpr double kRate = 48000.0;

double midiToHz(double m) { return 440.0 * std::pow(2.0, (m - 69.0) / 12.0); }

struct Note
{
    double at, length;
    int key;
    float velocity = 0.8f;
};

juce::AudioBuffer<float> render(S& s, double seconds, const std::vector<Note>& notes, int block = 256)
{
    const int total = (int) (seconds * kRate);
    juce::AudioBuffer<float> out(2, total);
    out.clear();
    for (int start = 0; start < total; start += block)
    {
        const int len = std::min(block, total - start);
        devices::HostInfo host;
        host.sampleRate = kRate;
        s.setHostInfo(host);
        juce::MidiBuffer midi;
        for (const auto& n : notes)
        {
            const int on = (int) std::lround(n.at * kRate), off = (int) std::lround((n.at + n.length) * kRate);
            if (on >= start && on < start + len) midi.addEvent(juce::MidiMessage::noteOn(1, n.key, n.velocity), on - start);
            if (off >= start && off < start + len) midi.addEvent(juce::MidiMessage::noteOff(1, n.key), off - start);
        }
        juce::AudioBuffer<float> view(out.getArrayOfWritePointers(), 2, start, len);
        s.process(view, midi);
    }
    return out;
}

// A dry solo singer of the Voices engine with every human control at zero (vibrato optional).
std::unique_ptr<S> drySinger(int section, float vowel, float vibrato = 0)
{
    auto s = std::make_unique<S>();
    s->setParameter(S::Section, (float) section);
    for (int p : {S::Space, S::Air, S::Warmth, S::VowelMotion, S::Drift, S::Detune, S::Timing, S::Scoop, S::Human, S::SizeSpread,
                  S::Breath, S::Width, S::Swell, S::Effort})
        s->setParameter(p, 0.0f);
    s->setParameter(S::Vibrato, vibrato);
    s->setParameter(S::VibratoDelay, 0);
    s->setParameter(S::LowCut, 40);
    s->setParameter(S::Attack, 60);
    s->setParameter(S::Release, 50);
    s->setParameter(S::Fold, 0);
    s->setParameter(S::Vowel, vowel);
    s->prepare(kRate, 512);
    return s;
}

// A sung tone: `seconds` of one note, mono.
juce::AudioBuffer<float> sungTone(int section, int key, float vowel, double seconds = 2.5, float vibrato = 0)
{
    auto s = drySinger(section, vowel, vibrato);
    auto stereo = render(*s, seconds, {{0.05, seconds - 0.2, key}});
    juce::AudioBuffer<float> mono(1, stereo.getNumSamples());
    mono.copyFrom(0, 0, stereo, 0, 0, stereo.getNumSamples());
    return mono;
}

std::shared_ptr<const sf::RealVoice> analyse(const juce::AudioBuffer<float>& audio, const std::string& name, std::string& reason)
{
    return sf::analyseRealVoice(audio.getArrayOfReadPointers(), audio.getNumChannels(), audio.getNumSamples(), kRate, name, reason);
}

// A Seraph singing Real from `voices`, one dry singer.
std::unique_ptr<S> realSinger(const std::vector<std::shared_ptr<const sf::RealVoice>>& voices, int section = S::SoloTenor)
{
    auto s = drySinger(section, 0);
    s->setParameter(S::Source, S::RealSource);
    for (const auto& v : voices) s->addRealVoice(v);
    return s;
}

double periodNear(const float* x, int from, int window, double expected)
{
    const int lo = std::max(2, (int) (expected * 0.8)), hi = (int) (expected * 1.25) + 1;
    std::vector<double> r((size_t) hi + 2, 0.0);
    for (int lag = lo - 1; lag <= hi + 1; ++lag)
    {
        double sum = 0, e0 = 0, e1 = 0;
        for (int i = 0; i < window; ++i)
        {
            sum += (double) x[from + i] * x[from + i + lag];
            e0 += (double) x[from + i] * x[from + i];
            e1 += (double) x[from + i + lag] * x[from + i + lag];
        }
        r[(size_t) lag] = sum / std::sqrt(std::max(1e-30, e0 * e1));
    }
    int best = lo;
    for (int lag = lo; lag <= hi; ++lag)
        if (r[(size_t) lag] > r[(size_t) best]) best = lag;
    const double a = r[(size_t) best - 1], b = r[(size_t) best], c = r[(size_t) best + 1], d = a - 2 * b + c;
    return best + (std::abs(d) > 1e-12 ? 0.5 * (a - c) / d : 0.0);
}

double centsOff(const juce::AudioBuffer<float>& a, int key, int from, int window)
{
    const double expected = kRate / midiToHz(key);
    return 1200.0 * std::log2(expected / periodNear(a.getReadPointer(0), from, window, expected));
}

// Harmonic amplitudes (dB) of a steady note (Blackman-Harris, 32768 points from `from`).
std::vector<double> harmonicsDb(const juce::AudioBuffer<float>& a, int from, double f0, double maxHz)
{
    const int size = 1 << 15;
    dsp::Fft fft(15);
    std::vector<std::complex<float>> data((size_t) size);
    for (int i = 0; i < size; ++i)
    {
        const double t = 2.0 * 3.14159265358979 * i / (size - 1);
        const double w = 0.35875 - 0.48829 * std::cos(t) + 0.14128 * std::cos(2 * t) - 0.01168 * std::cos(3 * t);
        data[(size_t) i] = {(float) (a.getSample(0, from + i) * w), 0.0f};
    }
    fft.transform(data);
    const double bin = kRate / size;
    std::vector<double> out;
    for (int k = 1; (double) k * f0 < maxHz; ++k)
    {
        const int centre = (int) std::lround(k * f0 / bin);
        double best = 0;
        for (int b = centre - 3; b <= centre + 3; ++b) best = std::max(best, (double) std::abs(data[(size_t) b]));
        out.push_back(20 * std::log10(std::max(1e-12, best)));
    }
    return out;
}

double rmsDb(const juce::AudioBuffer<float>& a, int from, int to)
{
    double sum = 0;
    for (int i = from; i < to; ++i) sum += (double) a.getSample(0, i) * a.getSample(0, i);
    return 10 * std::log10(std::max(1e-30, sum / std::max(1, to - from)));
}

bool writeWav(const juce::File& file, const juce::AudioBuffer<float>& audio)
{
    file.deleteFile();
    std::unique_ptr<juce::OutputStream> stream(file.createOutputStream());
    if (stream == nullptr) return false;
    juce::WavAudioFormat wav;
    auto writer = wav.createWriterFor(stream, juce::AudioFormatWriterOptions{}.withSampleRate(kRate).withNumChannels(1).withBitsPerSample(24));
    return writer != nullptr && writer->writeFromAudioSampleBuffer(audio, 0, audio.getNumSamples());
}
} // namespace

TEST_CASE("seraph real: the analysis finds the sung note, one mark per period, the attack and the sustain")
{
    for (float vibrato : {0.0f, 40.0f})
    {
        const auto tone = sungTone(S::SoloTenor, 52, 0, 2.5, vibrato);
        std::string reason;
        const auto v = analyse(tone, "tenor tone", reason);
        REQUIRE(v != nullptr);
        std::printf("    tenor E3 (vibrato %.0f): note %.2f, %d marks, sustain %.2f s from %.2f s\n", vibrato, v->midi, (int) v->marks.size(),
                    v->sustainSeconds(), v->marks[(size_t) v->sustainBegin] / kRate);
        CHECK(std::abs(v->midi - 52.0f) < 0.15f);
        CHECK(v->sustainSeconds() > 1.4);
        const double period = kRate / midiToHz(52);
        std::vector<double> steps;
        for (int i = v->sustainBegin + 1; i < v->sustainEnd; ++i) steps.push_back(v->marks[(size_t) i] - v->marks[(size_t) i - 1]);
        std::sort(steps.begin(), steps.end());
        CHECK(std::abs(steps[steps.size() / 2] / period - 1.0) < 0.01);
        if (vibrato <= 0.0f) CHECK(steps.front() > period * 0.97 && steps.back() < period * 1.03); // no skipped or doubled periods
        CHECK(v->level > 0.0f);
    }
    // the other voice types and vowels
    for (const auto& [section, key, vowel] : {std::tuple<int, int, float>{S::SoloBass, 43, 4.0f}, {S::SoloAlto, 64, 3.0f}, {S::SoloSoprano, 72, 0.0f},
                                              // a soprano A4 on A: her second harmonic sits on F1 (heard an octave high before)
                                              {S::SoloSoprano, 69, 0.0f}})
    {
        std::string reason;
        const auto v = analyse(sungTone(section, key, vowel), "tone", reason);
        REQUIRE(v != nullptr);
        CHECK(std::abs(v->midi - (float) key) < 0.15f);
    }
}

TEST_CASE("seraph real: phrases, chords, noise and short files are refused with the reason")
{
    std::string reason;
    // a phrase: a new note every 250 ms
    {
        auto s = drySinger(S::SoloTenor, 0);
        s->setParameter(S::Mode, S::Legato);
        s->setParameter(S::Glide, 40);
        std::vector<Note> notes;
        for (int i = 0; i < 10; ++i) notes.push_back({0.05 + 0.25 * i, 0.3, 50 + (i * 3) % 10});
        auto stereo = render(*s, 2.8, notes);
        CHECK(analyse(stereo, "phrase", reason) == nullptr);
        CHECK(reason.find("no stable pitch") != std::string::npos);
    }
    // a chord: three singers at once
    {
        auto s = drySinger(S::Mixed, 0);
        s->setParameter(S::Singers, 1);
        auto stereo = render(*s, 2.5, {{0.05, 2.3, 52}, {0.05, 2.3, 56}, {0.05, 2.3, 59}});
        reason.clear();
        CHECK(analyse(stereo, "chord", reason) == nullptr);
        CHECK(reason.find("no stable pitch") != std::string::npos);
    }
    // noise and silence
    {
        juce::AudioBuffer<float> noise(1, (int) (2.0 * kRate));
        juce::Random random(5);
        for (int i = 0; i < noise.getNumSamples(); ++i) noise.setSample(0, i, random.nextFloat() * 0.4f - 0.2f);
        reason.clear();
        CHECK(analyse(noise, "noise", reason) == nullptr);
        juce::AudioBuffer<float> silence(1, (int) (2.0 * kRate));
        silence.clear();
        reason.clear();
        CHECK(analyse(silence, "silence", reason) == nullptr);
        juce::AudioBuffer<float> shortTone(1, (int) (0.3 * kRate));
        shortTone.clear();
        reason.clear();
        CHECK(analyse(shortTone, "short", reason) == nullptr);
        CHECK(reason.find("too short") != std::string::npos);
    }
}

TEST_CASE("seraph real: vowels and keys from file names; a key settles an octave doubt")
{
    CHECK(sf::vowelFromName("Choir_Female_ah_A3") == 0);
    CHECK(sf::vowelFromName("monk-OO-long") == 4);
    CHECK(sf::vowelFromName("soprano_o_C5") == 3);
    CHECK(sf::vowelFromName("lead vocal") == -1);
    CHECK(sf::keyFromName("Choir_Female_ah_A3") == 57);
    CHECK(sf::keyFromName("Vox_F#m_120") == -2 - 6);
    CHECK(sf::keyFromName("Bass_Eb2") == 39);
    CHECK(sf::keyFromName("nothing here") == -1);
    // A name an octave above what the pitch track hears: the marks are doubled and the note moves up an octave.
    const auto tone = sungTone(S::SoloTenor, 52, 0);
    std::string reason;
    const auto plain = analyse(tone, "tone", reason);
    const auto named = analyse(tone, "tone_E4", reason);
    REQUIRE(plain != nullptr);
    REQUIRE(named != nullptr);
    CHECK(std::abs(named->midi - plain->midi - 12.0f) < 0.01f);
    CHECK(std::abs((double) named->marks.size() / (double) plain->marks.size() - 2.0) < 0.05);
}

namespace
{
// The recording's own envelope: its harmonics (dB), log-linear between them, at frequency f.
double envelopeAt(const std::vector<double>& h, double f0, double f)
{
    const double k = f / f0;
    if (k <= 1.0) return h.front();
    const size_t i = std::min((size_t) k, h.size() - 1);
    if (i >= h.size() - 1) return h.back();
    const double t = k - (double) i;
    return h[i - 1] + t * (h[i] - h[i - 1]);
}

struct Comparison
{
    double cents = 0, worst = 0, level = 0, formantScale = 0, f1 = 0, f2 = 0;
};

// Plays `voice` at `key` (Size `size`) and compares the result with the recording `tone` sung at `source`.
Comparison compare(const std::shared_ptr<const sf::RealVoice>& voice, const juce::AudioBuffer<float>& tone, int source, int key, float size,
                   double f1Table, double f2Table)
{
    Comparison c;
    auto real = realSinger({voice});
    real->setParameter(S::Size, size);
    const auto out = render(*real, 2.0, {{0.0, 2.0, key}});
    c.cents = centsOff(out, key, (int) (0.6 * kRate), 16384);
    c.level = rmsDb(out, (int) (0.6 * kRate), (int) (1.8 * kRate));
    const double f0 = midiToHz(key), sf0 = midiToHz(source), scale = 1.0 - size;
    const auto h = harmonicsDb(out, (int) (0.6 * kRate), f0, 4000);
    const auto r = harmonicsDb(tone, (int) (0.6 * kRate), sf0, 4500);
    // aligned at the recording's loudest harmonic region
    size_t top = 0;
    for (size_t k = 0; k < r.size(); ++k)
        if (r[k] > r[top]) top = k;
    double offset = 0;
    int n = 0;
    for (size_t k = 0; k < h.size(); ++k)
    {
        const double f = (double) (k + 1) * f0;
        if (f < 150 || f > 3500) continue;
        offset += h[k] - envelopeAt(r, sf0, f / scale);
        ++n;
    }
    offset /= std::max(1, n);
    for (size_t k = 0; k < h.size(); ++k)
    {
        const double f = (double) (k + 1) * f0;
        if (f < 150 || f > 3500 || envelopeAt(r, sf0, f / scale) < r[top] - 30) continue;
        c.worst = std::max(c.worst, std::abs(h[k] - offset - envelopeAt(r, sf0, f / scale)));
    }
    // Where the formants went: the scale of the recording's envelope that best fits the new harmonics (least squares, the
    // level free). Harmonics this far apart cannot place a peak to 6 %, a whole envelope can.
    double bestError = 1e18;
    for (double alpha = 0.8; alpha <= 1.2; alpha += 0.001)
    {
        double mean = 0, squares = 0;
        int count = 0;
        for (size_t k = 0; k < h.size(); ++k)
        {
            const double f = (double) (k + 1) * f0;
            if (f < 150 || f > 3500 || envelopeAt(r, sf0, f / alpha) < r[top] - 40) continue;
            const double d = h[k] - envelopeAt(r, sf0, f / alpha);
            mean += d;
            squares += d * d;
            ++count;
        }
        if (count < 3) continue;
        const double error = squares / count - (mean / count) * (mean / count);
        if (error < bestError) { bestError = error; c.formantScale = alpha; }
    }
    c.f1 = f1Table * c.formantScale;
    c.f2 = f2Table * c.formantScale;
    return c;
}
} // namespace

TEST_CASE("seraph real: resynthesis at -7, 0 and +7 semitones is in tune within 5 cents and keeps the formant peaks within 6 %")
{
    // A bass on O at A2 (110 Hz): low enough that its harmonics show its formant peaks (O: 400 and 750 Hz).
    const int source = 45;
    const auto tone = sungTone(S::SoloBass, source, 3);
    std::string reason;
    const auto voice = analyse(tone, "bass_o", reason);
    REQUIRE(voice != nullptr);
    double level0 = 0;
    for (int shift : {0, -7, 7})
    {
        const auto c = compare(voice, tone, source, source + shift, 0.0f, 400, 750);
        if (shift == 0) level0 = c.level;
        std::printf("    %+d st: %.2f cents; formants x %.3f (F1 %.0f, F2 %.0f for 400, 750); harmonics on the recording's envelope within "
                    "%.1f dB; level %+.1f dB\n", shift, c.cents, c.formantScale, c.f1, c.f2, c.worst, c.level - level0);
        CHECK(std::abs(c.cents) < 5.0);
        CHECK(std::abs(c.formantScale - 1.0) < 0.06);
        CHECK(c.worst < 3.0);
        CHECK(std::abs(c.level - level0) < 3.0);
    }
    // a woman: an alto on A at A4 (440 Hz) down and up a fifth: in tune, on her envelope
    const auto alto = sungTone(S::SoloAlto, 64, 0);
    const auto altoVoice = analyse(alto, "alto_a", reason);
    REQUIRE(altoVoice != nullptr);
    for (int shift : {-7, 7})
    {
        const auto c = compare(altoVoice, alto, 64, 64 + shift, 0.0f, 800, 1150);
        std::printf("    alto %+d st: %.2f cents; formants x %.3f; on her envelope within %.1f dB\n", shift, c.cents, c.formantScale, c.worst);
        CHECK(std::abs(c.cents) < 5.0);
        CHECK(std::abs(c.formantScale - 1.0) < 0.06);
        CHECK(c.worst < 4.0);
    }
}

TEST_CASE("seraph real: Size moves the formants and not the pitch")
{
    const auto tone = sungTone(S::SoloBass, 45, 3);
    std::string reason;
    const auto voice = analyse(tone, "bass_o", reason);
    REQUIRE(voice != nullptr);
    const auto c = compare(voice, tone, 45, 45, 0.1f, 400, 750); // the recording's envelope scaled by 0.9
    std::printf("    Size +10 %%: %.2f cents; formants x %.3f; on the scaled envelope within %.1f dB\n", c.cents, c.formantScale, c.worst);
    CHECK(std::abs(c.cents) < 5.0);
    CHECK(std::abs(c.formantScale - 0.9) < 0.02);
    CHECK(c.worst < 3.0);
}

TEST_CASE("seraph real: the read position plays the attack once, then wanders inside the sustain, each singer its own")
{
    std::string reason;
    const auto voice = analyse(sungTone(S::SoloTenor, 52, 0, 3.0, 30), "tenor", reason);
    REQUIRE(voice != nullptr);
    const double sustainBegin = voice->marks[(size_t) voice->sustainBegin], sustainEnd = voice->marks.back();
    auto s = realSinger({voice}, S::Men);
    s->setParameter(S::Singers, 4);
    s->setParameter(S::Timing, 30);
    s->prepare(kRate, 512);
    std::vector<std::vector<double>> positions(4);
    const int total = (int) (8.0 * kRate);
    juce::AudioBuffer<float> block(2, 512);
    for (int start = 0; start < total; start += 512)
    {
        juce::MidiBuffer midi;
        if (start == 0) midi.addEvent(juce::MidiMessage::noteOn(1, 52, 0.8f), 0);
        block.clear();
        s->process(block, midi);
        // the device's singers, by index (tests may look at the singers between blocks)
        int found = 0;
        for (int v = 0; v < S::kMaxSingers && found < 4; ++v)
            if (s->singer(v).real != nullptr) positions[(size_t) found++].push_back(s->singerRealPosition(v));
    }
    int distinct = 0;
    for (const auto& track : positions)
    {
        REQUIRE(track.size() > 100);
        double lo = 1e18, hi = 0;
        int turns = 0;
        for (size_t i = track.size() / 3; i < track.size(); ++i)
        {
            lo = std::min(lo, track[i]);
            hi = std::max(hi, track[i]);
            if (i > track.size() / 3 + 1 && (track[i] - track[i - 1]) * (track[i - 1] - track[i - 2]) < 0) ++turns;
        }
        CHECK(lo >= sustainBegin - 1);
        CHECK(hi <= sustainEnd + 1);
        CHECK((hi - lo) / (sustainEnd - sustainBegin) > 0.3);
        CHECK(turns >= 1);
        if (std::abs(track.back() - positions[0].back()) > 100) ++distinct;
    }
    std::printf("    4 singers, %d away from the first at the end\n", distinct);
    CHECK(distinct >= 2);
}

TEST_CASE("seraph real: one file sings as a section; several files map by note and voice type")
{
    std::string reason;
    const auto low = analyse(sungTone(S::SoloBass, 45, 3), "man_oh", reason);
    const auto high = analyse(sungTone(S::SoloAlto, 64, 3), "woman_oh", reason);
    REQUIRE(low != nullptr);
    REQUIRE(high != nullptr);
    // one file, six singers, wide: the sides differ
    {
        auto s = realSinger({low}, S::Men);
        s->setParameter(S::Singers, 6);
        s->setParameter(S::Width, 1);
        s->setParameter(S::Detune, 8);
        s->setParameter(S::Drift, 8);
        s->setParameter(S::Human, 0.5f);
        const auto out = render(*s, 2.0, {{0.0, 2.0, 48}});
        double lr = 0, ll = 0, rr = 0;
        for (int i = (int) (0.5 * kRate); i < out.getNumSamples(); ++i)
        {
            lr += (double) out.getSample(0, i) * out.getSample(1, i);
            ll += (double) out.getSample(0, i) * out.getSample(0, i);
            rr += (double) out.getSample(1, i) * out.getSample(1, i);
        }
        const double correlation = lr / std::sqrt(ll * rr);
        std::printf("    one recording, six singers: L/R correlation %.2f, %d singing\n", correlation, s->activeSingers());
        CHECK(s->activeSingers() == 6);
        CHECK(correlation < 0.8);
    }
    // two files: a low note takes the man, a high one the woman; Mixed gives each type its own
    {
        auto s = realSinger({low, high}, S::Mixed);
        s->setParameter(S::Singers, 4);
        render(*s, 0.3, {{0.0, 0.3, 50}, {0.0, 0.3, 66}});
        int lowForLow = 0, highForHigh = 0, total = 0;
        for (int v = 0; v < S::kMaxSingers; ++v)
        {
            const auto view = s->singer(v);
            if (view.type < 0) continue;
            ++total;
            if (view.pitch < 58 && view.real == low.get()) ++lowForLow;
            if (view.pitch > 58 && view.real == high.get()) ++highForHigh;
        }
        std::printf("    two recordings: %d of %d singers on the nearer one\n", lowForLow + highForHigh, total);
        CHECK(total == 8);
        CHECK(lowForLow + highForHigh >= 6);
        CHECK(std::abs(s->realDistance() - 5.0f) < 0.01f || std::abs(s->realDistance() - 2.0f) < 0.01f);
    }
}

TEST_CASE("seraph real: Source defaults to Voices; Real without files keeps the synthesis; old presets sing Voices")
{
    S s;
    CHECK(std::lround(s.getParameter(S::Source)) == S::VoicesSource);
    for (size_t i = 0; i < S::presets().size(); ++i)
    {
        const auto& p = S::presets()[i];
        if (std::string(p.category).rfind("Real", 0) == 0) continue;
        s.applyPreset((int) i);
        CHECK(std::lround(s.getParameter(S::Source)) == S::VoicesSource);
    }
    auto empty = drySinger(S::SoloTenor, 0);
    empty->setParameter(S::Source, S::RealSource);
    CHECK(! empty->realActive());
    const auto out = render(*empty, 0.5, {{0.0, 0.5, 52}});
    CHECK(rmsDb(out, 4800, 20000) > -60);
}

TEST_CASE("seraph real: the files are kept in the set's content and in the VST3 state; a missing file never fails a load")
{
    const auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("sonora-seraph-real");
    REQUIRE(dir.createDirectory().wasOk());
    const auto a = dir.getChildFile("men_oh_A2.wav"), b = dir.getChildFile("women_ah_E4.wav");
    REQUIRE(writeWav(a, sungTone(S::SoloBass, 45, 3)));
    REQUIRE(writeWav(b, sungTone(S::SoloAlto, 64, 0)));
    S s;
    std::string error;
    REQUIRE(s.addRealFile(a.getFullPathName().toStdString(), error));
    REQUIRE(s.addRealFile(b.getFullPathName().toStdString(), error));
    CHECK(s.realVoices().size() == 2);
    CHECK(s.realVoices()[0]->vowel == 3);
    CHECK(std::abs(s.realVoices()[0]->midi - 45.0f) < 0.2f);
    s.setParameter(S::Source, S::RealSource);
    CHECK(s.realActive());

    // the set's native content
    const auto content = s.encodeContent();
    S reopened;
    REQUIRE(reopened.decodeContent(content));
    CHECK(reopened.realPaths() == s.realPaths());
    CHECK(reopened.realErrors.empty());
    // the VST3 state, and the built-in stand-in for it
    const auto bytes = plugins::encodeParameterState(s, "SonoraSeraph", s.stateAttributes());
    S restored;
    std::map<std::string, std::string> attributes;
    REQUIRE(plugins::decodeParameterState(bytes.data(), bytes.size(), restored, "SonoraSeraph", "Sonora Seraph", error, &attributes));
    restored.applyStateAttributes(attributes);
    CHECK(restored.realPaths() == s.realPaths());
    CHECK(std::lround(restored.getParameter(S::Source)) == S::RealSource);
    // a moved file: a reason, the rest loads
    a.deleteFile();
    S missing;
    missing.decodeContent(content);
    CHECK(missing.realPaths().size() == 1);
    CHECK(missing.realErrors.size() == 1);
    // a file that is not a sung note is refused with the reason
    juce::AudioBuffer<float> noise(1, (int) (2 * kRate));
    juce::Random random(9);
    for (int i = 0; i < noise.getNumSamples(); ++i) noise.setSample(0, i, random.nextFloat() - 0.5f);
    const auto c = dir.getChildFile("noise.wav");
    REQUIRE(writeWav(c, noise));
    error.clear();
    CHECK(! s.addRealFile(c.getFullPathName().toStdString(), error));
    CHECK(error.find("no stable pitch") != std::string::npos);
    dir.deleteRecursively();
}

namespace
{
struct SmallStackReal
{
    std::shared_ptr<const sf::RealVoice> voice;
    bool ok = false;
    static void* entry(void* arg)
    {
        auto& self = *static_cast<SmallStackReal*>(arg);
        auto s = std::make_unique<S>();
        s->setParameter(S::Source, S::RealSource);
        s->setParameter(S::Singers, 8);
        s->addRealVoice(self.voice);
        s->prepare(kRate, 512);
        juce::AudioBuffer<float> audio(2, 512);
        juce::MidiBuffer midi;
        for (int k = 0; k < 8; ++k) midi.addEvent(juce::MidiMessage::noteOn(1, 45 + k * 3, (juce::uint8) 100), k);
        for (int pass = 0; pass < 8; ++pass)
        {
            audio.clear();
            s->process(audio, midi);
            midi.clear();
        }
        s->clearRealVoices();
        for (int pass = 0; pass < 4; ++pass) s->process(audio, midi); // the bank changes under singing singers
        self.ok = true;
        return nullptr;
    }
};
} // namespace

TEST_CASE("seraph real: 64 Real singers fit a 256 KB stack, a bank change while singing is safe, and the CPU is low")
{
    std::string reason;
    SmallStackReal run;
    run.voice = analyse(sungTone(S::SoloTenor, 52, 0), "tenor", reason);
    REQUIRE(run.voice != nullptr);
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    CHECK(pthread_attr_setstacksize(&attributes, 256 * 1024) == 0);
    pthread_t thread;
    REQUIRE(pthread_create(&thread, &attributes, &SmallStackReal::entry, &run) == 0);
    pthread_join(thread, nullptr);
    pthread_attr_destroy(&attributes);
    CHECK(run.ok);

    auto s = realSinger({run.voice}, S::Mixed);
    s->setParameter(S::MenOctave, 1);
    s->setParameter(S::Singers, 8);
    s->prepare(kRate, 512);
    juce::AudioBuffer<float> audio(2, 512);
    double busy = 0;
    const int blocks = (int) (5.0 * kRate / 512);
    for (int b = 0; b < blocks; ++b)
    {
        juce::MidiBuffer midi;
        if (b == 0)
            for (int k : {57, 60, 64, 69}) midi.addEvent(juce::MidiMessage::noteOn(1, k, (juce::uint8) 100), 0);
        audio.clear();
        const auto t = std::chrono::steady_clock::now();
        s->process(audio, midi);
        busy += std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
    }
    std::printf("    64 Real singers: %.1f %% of one core (this build), %d singing\n", 100.0 * busy / 5.0, s->activeSingers());
    CHECK(s->activeSingers() == 64);
    CHECK(busy / 5.0 < 0.5);
}

