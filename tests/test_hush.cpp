#include "TestFramework.h"
#include "devices/native/Hush.h"

#include <cmath>
#include <random>

using namespace sonora;
namespace
{
using H = devices::Hush;
constexpr double kRate = 48000.0;

void flatCurve(H& h)
{
    for (int b = 0; b < H::kBands; ++b) h.setParameter(H::bandParam(b, H::BandUsed), 0.0f);
}

// Runs `input` (stereo) through the device in 512-sample blocks.
juce::AudioBuffer<float> run(H& h, const juce::AudioBuffer<float>& input, int block = 512)
{
    juce::AudioBuffer<float> out(input);
    juce::MidiBuffer midi;
    for (int start = 0; start < out.getNumSamples(); start += block)
    {
        const int n = std::min(block, out.getNumSamples() - start);
        juce::AudioBuffer<float> view(out.getArrayOfWritePointers(), 2, start, n);
        h.process(view, midi);
    }
    return out;
}

juce::AudioBuffer<float> noisePlusTone(int n, float noise, float tone, float hz, float gain = 1.0f, unsigned seed = 7)
{
    juce::AudioBuffer<float> a(2, n);
    std::mt19937 rng(seed);
    std::normal_distribution<float> dist(0.0f, 1.0f);
    for (int s = 0; s < n; ++s)
    {
        const float v = (noise * dist(rng) + tone * std::sin(2.0f * 3.14159265f * hz * (float) s / (float) kRate)) * gain;
        a.setSample(0, s, v);
        a.setSample(1, s, v);
    }
    return a;
}

// Level of `hz` in a (latency-compensated) region, by correlation with a sine and cosine.
double toneLevel(const juce::AudioBuffer<float>& a, int from, int count, double hz)
{
    double c = 0, s = 0;
    for (int i = 0; i < count; ++i)
    {
        const double ph = 2.0 * 3.14159265358979 * hz * (double) (from + i) / kRate;
        c += a.getSample(0, from + i) * std::cos(ph);
        s += a.getSample(0, from + i) * std::sin(ph);
    }
    return 2.0 * std::sqrt(c * c + s * s) / count;
}
} // namespace

TEST_CASE("hush: identity at zero depth, with the reported latency, in every mode")
{
    struct Mode { float quality, linear, low; int latency; };
    for (const Mode m : {Mode{0, 0, 0, 2304}, Mode{1, 0, 0, 2304}, Mode{2, 0, 0, 2304}, Mode{0, 1, 0, 3456}, Mode{0, 0, 1, 0}})
    {
        H h;
        h.setParameter(H::Depth, 0.0f);
        h.setParameter(H::Quality, m.quality);
        h.setParameter(H::LinearPhase, m.linear);
        h.setParameter(H::LowLatency, m.low);
        h.prepare(kRate, 512);
        CHECK((h.latencySamples()) == (m.latency));
        juce::AudioBuffer<float> in(2, 24000);
        in.clear();
        in.setSample(0, 5000, 0.5f);
        in.setSample(1, 5000, -0.25f);
        const auto out = run(h, in);
        int peak = 0;
        for (int i = 0; i < out.getNumSamples(); ++i)
            if (std::abs(out.getSample(0, i)) > std::abs(out.getSample(0, peak))) peak = i;
        CHECK((peak) == (5000 + m.latency));
        CHECK_NEAR(out.getSample(0, peak), 0.5, 2e-3);
        CHECK_NEAR(out.getSample(1, peak), -0.25, 2e-3);
        double energy = 0;
        for (int i = 0; i < out.getNumSamples(); ++i) energy += out.getSample(0, i) * out.getSample(0, i);
        CHECK_NEAR(energy, 0.25, 2e-3);
    }
}

TEST_CASE("hush: mid/side processing at zero depth is transparent")
{
    H h;
    h.setParameter(H::Depth, 0.0f);
    h.setParameter(H::StereoMode, 1.0f);
    h.prepare(kRate, 512);
    auto in = noisePlusTone(30000, 0.1f, 0.0f, 1000);
    for (int s = 0; s < in.getNumSamples(); ++s) in.setSample(1, s, 0.3f * in.getSample(1, s));
    const auto out = run(h, in);
    double err = 0;
    for (int s = 6000; s < 26000; ++s)
        for (int ch = 0; ch < 2; ++ch) err = std::max(err, (double) std::abs(out.getSample(ch, s) - in.getSample(ch, s - 2304)));
    CHECK(err < 1e-3);
}

TEST_CASE("hush: a resonance is cut, the noise around it is not")
{
    H h;
    flatCurve(h);
    h.prepare(kRate, 512);
    const int n = 48000 * 4;
    const auto in = noisePlusTone(n, 0.05f, 0.5f, 1000);
    const auto out = run(h, in);
    const double before = toneLevel(in, 48000 * 2, 48000, 1000), after = toneLevel(out, 48000 * 2 + 2304, 48000, 1000);
    const double cut = 20 * std::log10(after / before);
    CHECK(cut < -1.5);
    CHECK(cut > -8.0); // measured reference at depth 5 / detail 5: about -2.9 dB
    CHECK(h.maxReduction(0) > 1.0f);
    // Far from the tone (here: broadband RMS with the tone removed by the comparison above) little changes.
    const auto noiseOnly = noisePlusTone(n, 0.05f, 0.0f, 1000);
    H flat;
    flatCurve(flat);
    flat.prepare(kRate, 512);
    const auto noiseOut = run(flat, noiseOnly);
    const double rin = noiseOnly.getRMSLevel(0, 48000, 96000), rout = noiseOut.getRMSLevel(0, 48000 + 2304, 96000);
    CHECK_NEAR(20 * std::log10(rout / rin), 0.0, 0.6);
}

TEST_CASE("hush: soft mode ignores the input level, hard mode cuts loud input harder")
{
    double cuts[2][2];
    for (int mode = 0; mode < 2; ++mode)
        for (int g = 0; g < 2; ++g)
        {
            H h;
            flatCurve(h);
            h.setParameter(H::Mode, (float) mode);
            h.prepare(kRate, 512);
            const float gain = g == 0 ? 0.1f : 1.0f;
            const auto in = noisePlusTone(48000 * 4, 0.05f, 0.5f, 1000, gain);
            const auto out = run(h, in);
            cuts[mode][g] = 20 * std::log10(toneLevel(out, 48000 * 2 + 2304, 48000, 1000) / toneLevel(in, 48000 * 2, 48000, 1000));
        }
    CHECK_NEAR(cuts[0][0], cuts[0][1], 0.3);
    CHECK(cuts[1][1] < cuts[1][0] - 1.5);
}

TEST_CASE("hush: delta is the removed part, mix and bypass return the dry signal")
{
    const auto in = noisePlusTone(48000 * 2, 0.05f, 0.5f, 1000);
    H wet, delta, dry, bypass;
    for (H* h : {&wet, &delta, &dry, &bypass}) flatCurve(*h);
    delta.setParameter(H::Delta, 1.0f);
    dry.setParameter(H::Mix, 0.0f);
    bypass.setParameter(H::Bypass, 1.0f);
    for (H* h : {&wet, &delta, &dry, &bypass}) h->prepare(kRate, 512);
    const auto w = run(wet, in), d = run(delta, in), m = run(dry, in), b = run(bypass, in);
    double sumErr = 0, dryErr = 0, bypassErr = 0;
    for (int s = 10000; s < 90000; ++s)
    {
        const float x = in.getSample(0, s - 2304);
        sumErr = std::max(sumErr, (double) std::abs(w.getSample(0, s) + d.getSample(0, s) - x));
        dryErr = std::max(dryErr, (double) std::abs(m.getSample(0, s) - x));
        bypassErr = std::max(bypassErr, (double) std::abs(b.getSample(0, s) - x));
    }
    CHECK(sumErr < 1e-4);
    CHECK(dryErr < 1e-4);
    CHECK(bypassErr < 1e-4);
}

TEST_CASE("hush: depth curve shapes")
{
    using namespace devices::hush;
    std::array<Band, kBands> bands{};
    bands[0] = Band{true, true, Shape::LowCut, 1000.0f, 0.0f, 0.707f, 5.0f, 0.5f};
    CHECK(evaluate(bands, 4000, 0).weight() > 0.95f);
    CHECK(evaluate(bands, 500, 0).weight() < 0.2f); // measured: one octave under a slope-5 cut keeps ~11 %
    bands[0] = Band{true, true, Shape::Notch, 1000.0f, 0.0f, 0.707f, 5.0f, 0.5f};
    CHECK(evaluate(bands, 1000, 0).weight() < 0.01f);
    CHECK(evaluate(bands, 1400, 0).weight() < 0.1f);
    bands[0] = Band{true, true, Shape::BandPass, 1000.0f, 0.0f, 0.707f, 5.0f, 0.5f};
    CHECK(evaluate(bands, 1000, 0).weight() > 0.99f);
    CHECK(evaluate(bands, 8000, 0).weight() < 0.1f);
    bands[1] = Band{true, true, Shape::BandPass, 8000.0f, 0.0f, 0.707f, 5.0f, 0.5f}; // pass bands add up
    CHECK(evaluate(bands, 8000, 0).weight() > 0.99f);
    bands = {};
    bands[0] = Band{true, true, Shape::Bell, 4000.0f, 10.0f, 0.707f, 5.0f, 0.0f}; // focused fully left/mid
    CHECK(evaluate(bands, 4000, 0).curve > 9.0f);
    CHECK_NEAR(evaluate(bands, 4000, 1).curve, 0.0, 1e-6);
    bands[0].enabled = false;
    CHECK_NEAR(evaluate(bands, 4000, 0).curve, 0.0, 1e-6);
}
