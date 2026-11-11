// PLG-21: Sonora Phase Align keeps the bass's low end in phase with the kick (sidechain), hit by hit.
#include "TestFramework.h"
#include "core/DeviceCatalog.h"
#include "devices/DeviceFactory.h"
#include "devices/native/PhaseAlign.h"
#include "plugins/SonoraPluginState.h"

#include <cmath>
#include <cstdio>

using namespace sonora;
namespace
{
using P = devices::PhaseAlign;
constexpr double kRate = 48000.0;
constexpr double kPi = 3.14159265358979323846;

// A kick every 500 ms: a 60 Hz tone that starts at phase 0 and decays (plus a short click). The bass: a steady 60 Hz
// tone at `bassPhase` degrees from the kick's tone.
struct Material
{
    juce::AudioBuffer<float> bass, kick;
};
Material material(int hits, double bassPhaseDegrees, float bassGain = 0.4f)
{
    const int period = (int) (0.5 * kRate), n = period * hits;
    Material m{juce::AudioBuffer<float>(2, n), juce::AudioBuffer<float>(2, n)};
    for (int s = 0; s < n; ++s)
    {
        const double t = (double) s / kRate, tk = (double) (s % period) / kRate;
        const float kick = (float) (0.8 * std::exp(-tk / 0.12) * std::sin(2 * kPi * 60.0 * t) + (tk < 0.002 ? 0.3 : 0.0));
        const float bass = (float) (bassGain * std::sin(2 * kPi * 60.0 * t + bassPhaseDegrees * kPi / 180.0));
        for (int ch = 0; ch < 2; ++ch)
        {
            m.kick.setSample(ch, s, kick);
            m.bass.setSample(ch, s, bass);
        }
    }
    return m;
}

juce::AudioBuffer<float> run(P& p, const Material& m, int block = 256)
{
    juce::AudioBuffer<float> out(m.bass);
    juce::MidiBuffer midi;
    for (int start = 0; start < out.getNumSamples(); start += block)
    {
        const int len = std::min(block, out.getNumSamples() - start);
        juce::AudioBuffer<float> view(out.getArrayOfWritePointers(), 2, start, len);
        juce::AudioBuffer<float> side(const_cast<float* const*>(m.kick.getArrayOfReadPointers()), 2, start, len);
        p.setSidechain(&side, 1.0f);
        p.process(view, midi);
    }
    p.setSidechain(nullptr, 1.0f);
    return out;
}

// Correlation of the output with the kick's tone over the last hit's first 90 ms (low band only matters here).
double correlation(const juce::AudioBuffer<float>& out, const Material& m, int fromSample, int samples)
{
    double sum = 0, kk = 0, bb = 0;
    for (int s = fromSample; s < fromSample + samples; ++s)
    {
        sum += (double) out.getSample(0, s) * m.kick.getSample(0, s);
        kk += (double) m.kick.getSample(0, s) * m.kick.getSample(0, s);
        bb += (double) out.getSample(0, s) * out.getSample(0, s);
    }
    return sum / std::sqrt(kk * bb + 1e-12);
}
} // namespace

TEST_CASE("phase align: a bass out of phase with the kick is rotated into phase within a few hits")
{
    for (const double offset : {180.0, 90.0, -120.0, 45.0})
    {
        P p;
        p.prepare(kRate, 256);
        p.setParameter(P::Speed, 20.0f);
        const auto m = material(8, offset);
        const auto out = run(p, m);
        const int period = (int) (0.5 * kRate), last = 7 * period;
        CHECK(p.hitCount() == 8u);
        CHECK(p.lastCoherence() > 0.9f);
        // The learned angle undoes the offset (the Hilbert pair adds its own phase, so compare the result, not the number).
        const double before = correlation(m.bass, m, last + 480, 4000);
        const double after = correlation(out, m, last + 480, 4000);
        CHECK(after > 0.95);
        if (std::abs(offset) >= 90.0) CHECK(after > before + 0.3);
        if (offset == 180.0) CHECK(p.lastGainDb() > 3.0f); // a polarity flip: the low end gets much louder together
    }
}

TEST_CASE("phase align: Hold keeps the angle, Manual uses the Angle control, no sidechain leaves it alone")
{
    P p;
    p.prepare(kRate, 256);
    p.setParameter(P::Speed, 20.0f);
    run(p, material(6, 180.0));
    const float learned = p.targetAngle();
    p.setParameter(P::ModeParam, (float) P::Hold);
    run(p, material(6, 0.0)); // a different relation: Hold does not follow it
    CHECK_NEAR(p.targetAngle(), learned, 1e-3);
    p.setParameter(P::ModeParam, (float) P::Manual);
    p.setParameter(P::Angle, 33.0f);
    run(p, material(2, 0.0));
    CHECK_NEAR(p.currentAngle(), 33.0, 0.5);

    // No sidechain: no hits, the angle stays where it was, the audio passes (allpassed low band, same level).
    P q;
    q.prepare(kRate, 256);
    const auto m = material(4, 0.0);
    juce::AudioBuffer<float> out(m.bass);
    juce::MidiBuffer midi;
    for (int start = 0; start < out.getNumSamples(); start += 256)
    {
        juce::AudioBuffer<float> view(out.getArrayOfWritePointers(), 2, start, std::min(256, out.getNumSamples() - start));
        q.process(view, midi);
    }
    CHECK(q.hitCount() == 0u);
    CHECK(! q.sidechainPresent());
    double inE = 0, outE = 0;
    for (int s = 24000; s < 96000; ++s) { inE += m.bass.getSample(0, s) * m.bass.getSample(0, s); outE += out.getSample(0, s) * out.getSample(0, s); }
    CHECK_NEAR(10.0 * std::log10(outE / inE), 0.0, 0.5); // the rotated low band against the high band near the crossover
}

TEST_CASE("phase align: Listen Sum adds the kick; a hit's picture and history are published; catalog and state")
{
    P p;
    p.prepare(kRate, 256);
    p.setParameter(P::Listen, (float) P::ListenOriginalSum);
    const auto m = material(3, 0.0);
    const auto out = run(p, m);
    CHECK_NEAR(out.getSample(0, 30000), m.bass.getSample(0, 30000) + m.kick.getSample(0, 30000), 1e-5);
    P::Shape shape;
    CHECK(p.takeShape(shape, 0));
    CHECK(shape.bins > 100);
    CHECK(p.history(0).used);
    CHECK(! p.history(5).used); // only 3 hits so far

    const auto* entry = model::findCatalogEntry("SonoraPhaseAlign");
    REQUIRE(entry != nullptr);
    CHECK(std::string(entry->displayName) == "Sonora Phase Align");
    auto d = devices::nativeDeviceModel("SonoraPhaseAlign");
    CHECK(dynamic_cast<P*>(devices::createNative(d).get()) != nullptr);
    P a;
    a.setParameter(P::Crossover, 120.0f);
    a.setParameter(P::ModeParam, (float) P::Manual);
    const auto bytes = plugins::encodeParameterState(a, "SonoraPhaseAlign");
    P b;
    std::string error;
    REQUIRE(plugins::decodeParameterState(bytes.data(), bytes.size(), b, "SonoraPhaseAlign", "Sonora Phase Align", error));
    for (int i = 0; i < P::NumParams; ++i) CHECK_NEAR(b.getParameter(i), a.getParameter(i), 1e-6);
}
