// PLG-24c: Sonora Correlation's Kick x Bass mode tells whether the bass reinforces or cancels the kick where they meet
// (a synthetic psytrance K-B-B-B pattern at 145 BPM), suggests a polarity flip or a shift, follows the tempo with Sync and
// keeps a beat view from the kick's onsets.
#include "TestFramework.h"
#include "devices/native/Correlation.h"

#include <cmath>
#include <functional>

using namespace sonora;
namespace
{
using C = devices::Correlation;
constexpr double kRate = 48000.0;
constexpr double kPi = 3.14159265358979323846;
constexpr double kTempo = 145.0;
constexpr double kBeat = 60.0 / kTempo;      // 0.414 s
constexpr double kSixteenth = kBeat / 4.0;   // 0.103 s

// One beat of a psytrance kick: a 50 Hz body (phase from the beat) that decays with `tailSeconds`, cut at 0.2 s.
double kickAt(double t, double tailSeconds = 0.045)
{
    const double inBeat = std::fmod(t, kBeat);
    if (inBeat > 0.2) return 0.0;
    const double attack = std::min(1.0, inBeat / 0.002);
    return 0.8 * attack * std::exp(-inBeat / tailSeconds) * std::sin(2.0 * kPi * 50.0 * inBeat);
}

// The bass: three 16ths after the kick (K-B-B-B), 50 Hz in the beat's phase plus `phase`, each note 90 ms.
double bassAt(double t, double phase = 0.0)
{
    const double inBeat = std::fmod(t, kBeat);
    const int step = (int) (inBeat / kSixteenth);
    if (step == 0) return 0.0;
    const double inNote = inBeat - step * kSixteenth;
    if (inNote > 0.09) return 0.0;
    const double attack = std::min(1.0, inNote / 0.003), release = std::min(1.0, (0.09 - inNote) / 0.01);
    return 0.5 * attack * release * std::sin(2.0 * kPi * 50.0 * inBeat + phase);
}

struct Pair
{
    std::function<double(double)> kick, bass;
};

// Runs `seconds` of the pair: the bass on the input (both channels) and the kick on the sidechain, or (kickLeft) the kick
// on the left and the bass on the right. Returns the last block's output; `untouched` checks output == input.
juce::AudioBuffer<float> run(C& meter, const Pair& pair, double seconds, int& at, bool kickLeft = false, bool withSidechain = true,
                             bool* untouched = nullptr)
{
    const int total = (int) (seconds * kRate);
    juce::AudioBuffer<float> block(2, 512), sidechain(2, 512);
    juce::MidiBuffer midi;
    devices::HostInfo host;
    host.tempo = kTempo;
    host.sampleRate = kRate;
    for (int done = 0; done < total; done += 512)
    {
        const int n = std::min(512, total - done);
        block.setSize(2, n, false, false, true);
        sidechain.setSize(2, n, false, false, true);
        for (int s = 0; s < n; ++s)
        {
            const double t = (double) (at + s) / kRate;
            const float k = (float) pair.kick(t), b = (float) pair.bass(t);
            block.setSample(0, s, kickLeft ? k : b);
            block.setSample(1, s, b);
            sidechain.setSample(0, s, k);
            sidechain.setSample(1, s, k);
        }
        juce::AudioBuffer<float> input(block);
        meter.setHostInfo(host);
        if (withSidechain && ! kickLeft) meter.setSidechain(&sidechain, 1.0f);
        meter.process(block, midi);
        if (untouched != nullptr)
            for (int s = 0; s < n; ++s)
                for (int ch = 0; ch < 2; ++ch)
                    if (std::abs(block.getSample(ch, s) - input.getSample(ch, s)) > 0.0f) *untouched = false;
        at += n;
    }
    return block;
}

C& kickBass(C& meter)
{
    meter.setParameter(C::Mode, (float) C::MeasureKickBass);
    meter.setParameter(C::Sync, 1.0f);
    meter.setParameter(C::IntegrationNote, 5.0f); // 1/2: two beats, a steady reading
    return meter;
}
} // namespace

TEST_CASE("correlation kick x bass: in phase, inverted and 90 degrees where the kick's tail meets the bass")
{
    C meter;
    meter.prepare(kRate, 512);
    kickBass(meter);
    int at = 0;
    bool untouched = true;
    run(meter, {[](double t) { return kickAt(t); }, [](double t) { return bassAt(t); }}, 3.0, at, false, true, &untouched);
    auto r = meter.reading();
    CHECK(untouched); // measuring never touches the bass
    CHECK(r.kickBass);
    CHECK(r.sidechain);
    CHECK(r.kick);
    CHECK(r.bass);
    CHECK(r.overlap);
    CHECK(r.signal);
    CHECK(r.correlation > 0.8f);
    // The kick's tail is ~16 dB under the bass where they meet: they add up a little, flipping the bass would cost as much.
    CHECK(r.sumDb > 0.5f);
    CHECK(r.invertedDb < -0.5f);
    CHECK(r.shared > 0.01f);
    CHECK(r.shared < 0.6f);
    CHECK(std::abs(r.shiftMs) < 0.7f); // already the best alignment
    // The low bands compare the kick with the bass too (less sharply: a third-octave band rings for ~8 periods, longer
    // than the overlap the weight marks).
    const auto band50 = meter.band(4);
    CHECK(band50.measured);
    CHECK(band50.correlation > 0.4f);

    meter.reset();
    run(meter, {[](double t) { return kickAt(t); }, [](double t) { return bassAt(t, kPi); }}, 3.0, at);
    r = meter.reading();
    CHECK(r.overlap);
    CHECK(r.correlation < -0.8f);
    CHECK(r.sumDb < -0.5f);
    CHECK(r.invertedDb > 0.5f);
    CHECK(meter.band(4).correlation < -0.4f);
    CHECK(r.minHeld < -0.8f);

    meter.reset();
    run(meter, {[](double t) { return kickAt(t); }, [](double t) { return bassAt(t, kPi / 2); }}, 3.0, at);
    r = meter.reading();
    CHECK(r.overlap);
    CHECK(std::abs(r.correlation) < 0.25f);
    CHECK(std::abs(r.sumDb) < 1.0f);
    // A quarter period (5 ms at 50 Hz) earlier or later lines them up better.
    CHECK(std::abs(std::abs(r.shiftMs) - 5.0f) < 1.0f);
    CHECK(r.shiftCorrelation > r.correlation + 0.2f);
}

TEST_CASE("correlation kick x bass: the best shift of a late bass, take turns, no kick, Kick From Left")
{
    C meter;
    meter.prepare(kRate, 512);
    kickBass(meter);
    int at = 0;
    // The bass 4 ms late: cos(72 degrees) = 0.31 now; 4 ms earlier is right.
    run(meter, {[](double t) { return kickAt(t); }, [](double t) { return bassAt(t - 0.004); }}, 3.0, at);
    auto r = meter.reading();
    CHECK(r.overlap);
    CHECK(r.correlation > 0.1f);
    CHECK(r.correlation < 0.55f);
    CHECK_NEAR(r.shiftMs, -4.0, 0.5);
    CHECK(r.shiftCorrelation > 0.85f);

    // A short kick that is gone before the first bass note: they take turns, nothing to measure.
    meter.reset();
    run(meter, {[](double t) { return kickAt(t, 0.008); }, [](double t) { return bassAt(t); }}, 3.0, at);
    r = meter.reading();
    CHECK(r.kick);
    CHECK(r.bass);
    CHECK(! r.overlap);
    CHECK(! r.signal);
    CHECK(r.shared < C::kMinShared);

    // No sidechain: no kick; the bass alone.
    meter.reset();
    run(meter, {[](double t) { return kickAt(t); }, [](double t) { return bassAt(t); }}, 1.0, at, false, false);
    r = meter.reading();
    CHECK(! r.sidechain);
    CHECK(! r.kick);
    CHECK(r.bass);
    CHECK(! r.signal);

    // The pan trick: the kick left, the bass right on one input (a group), no sidechain needed.
    meter.setParameter(C::KickFrom, (float) C::KickLeft);
    run(meter, {[](double t) { return kickAt(t); }, [](double t) { return bassAt(t, kPi); }}, 3.0, at, true);
    r = meter.reading();
    CHECK(r.kick);
    CHECK(r.bass);
    CHECK(r.overlap);
    CHECK(r.correlation < -0.8f);
}

TEST_CASE("correlation: Integration follows the tempo with Sync (1/4 = one beat); note values per time signature")
{
    CHECK_NEAR(C::noteBeats(0), 1.0 / 16.0, 1e-12); // 1/64
    CHECK_NEAR(C::noteBeats(2), 0.25, 1e-12);       // 1/16
    CHECK_NEAR(C::noteBeats(4), 1.0, 1e-12);        // 1/4
    CHECK_NEAR(C::noteBeats(6), 4.0, 1e-12);        // 1 bar of 4/4
    CHECK_NEAR(C::noteBeats(6, 7, 8), 3.5, 1e-12);  // 1 bar of 7/8
    CHECK_NEAR(C::noteBeats(7, 3, 4), 6.0, 1e-12);  // 2 bars of 3/4
    CHECK(C::noteLabel(2) == "1/16");
    CHECK(C::noteLabel(4) == "1/4");
    CHECK(C::noteLabel(7) == "2 bars");

    C meter;
    meter.prepare(kRate, 512);
    int at = 0;
    const Pair quiet{[](double) { return 0.0; }, [](double) { return 0.0; }};
    run(meter, quiet, 0.05, at);
    CHECK_NEAR(meter.integrationMs(), 300.0, 0.01); // Integration's default, in ms
    meter.setParameter(C::Sync, 1.0f);
    meter.setParameter(C::IntegrationNote, 4.0f); // 1/4
    run(meter, quiet, 0.05, at);
    CHECK_NEAR(meter.integrationMs(), 60000.0 / kTempo, 0.01);
    CHECK_NEAR(meter.tempo(), kTempo, 1e-9);
    meter.setParameter(C::IntegrationNote, 2.0f); // 1/16
    run(meter, quiet, 0.05, at);
    CHECK_NEAR(meter.integrationMs(), 15000.0 / kTempo, 0.01);
    meter.setParameter(C::Sync, 0.0f);
    meter.setParameter(C::Integration, 40.0f);
    run(meter, quiet, 0.05, at);
    CHECK_NEAR(meter.integrationMs(), 40.0, 0.01);
}

TEST_CASE("correlation kick x bass: Listen Bass Inverted flips the bass; the beat view starts at the kick")
{
    C meter;
    meter.prepare(kRate, 512);
    kickBass(meter);
    int at = 0;
    const Pair pair{[](double t) { return kickAt(t); }, [](double t) { return bassAt(t); }};
    meter.setParameter(C::Listen, (float) C::ListenBassInverted);
    run(meter, pair, 0.05, at); // past the crossfade
    int start = at;
    auto out = run(meter, pair, 512.0 / kRate, at);
    int bad = 0;
    for (int s = 0; s < out.getNumSamples(); ++s)
    {
        const float b = (float) bassAt((double) (start + s) / kRate);
        if (std::abs(out.getSample(0, s) + b) > 1e-6f || std::abs(out.getSample(1, s) + b) > 1e-6f) ++bad;
    }
    CHECK(bad == 0);
    // Kick From Left: only the bass (the right channel) flips.
    meter.setParameter(C::KickFrom, (float) C::KickLeft);
    run(meter, pair, 0.05, at, true);
    start = at;
    out = run(meter, pair, 512.0 / kRate, at, true);
    bad = 0;
    for (int s = 0; s < out.getNumSamples(); ++s)
    {
        const double t = (double) (start + s) / kRate;
        if (std::abs(out.getSample(0, s) - (float) kickAt(t)) > 1e-6f || std::abs(out.getSample(1, s) + (float) bassAt(t)) > 1e-6f) ++bad;
    }
    CHECK(bad == 0);
    meter.setParameter(C::Listen, (float) C::ListenStereo);
    meter.setParameter(C::KickFrom, (float) C::KickSidechain);

    // The beat view: one beat from 10 ms before the last complete kick onset.
    run(meter, pair, 2.0, at);
    std::vector<float> kick(600), bass(600), weight(600);
    float msPerPoint = 0;
    bool fromOnset = false;
    const int points = meter.beat(kick.data(), bass.data(), weight.data(), 600, 10.0f, 0.0f, msPerPoint, fromOnset);
    CHECK(fromOnset);
    REQUIRE(points > 100);
    CHECK_NEAR(msPerPoint * (float) points, 10.0 + 1000.0 * kBeat, 3.0);
    // The kick is silent before the onset and loud within 15 ms after it; the bass comes in after a 16th.
    float before = 0, after = 0, bassEarly = 0, bassLate = 0;
    for (int p = 0; p < points; ++p)
    {
        const float ms = (float) p * msPerPoint - 10.0f;
        if (ms < -3.0f) before = std::max(before, std::abs(kick[(size_t) p]));
        if (ms > 0.0f && ms < 15.0f) after = std::max(after, std::abs(kick[(size_t) p]));
        if (ms > 0.0f && ms < 80.0f) bassEarly = std::max(bassEarly, std::abs(bass[(size_t) p]));
        if (ms > 110.0f && ms < 190.0f) bassLate = std::max(bassLate, std::abs(bass[(size_t) p]));
    }
    CHECK(before < 0.05f);
    CHECK(after > 0.3f);
    CHECK(bassEarly < 0.05f);
    CHECK(bassLate > 0.3f);
}
