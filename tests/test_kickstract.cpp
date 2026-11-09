// PLG-17: Sonora Kickstract, the 3-band kick extractor.
#include "TestFramework.h"
#include "core/DeviceCatalog.h"
#include "devices/DeviceFactory.h"
#include "devices/native/Kickstract.h"
#include "plugins/SonoraPluginState.h"

#include <cmath>
#include <memory>
#include <random>

using namespace sonora;
namespace
{
using K = devices::Kickstract;
constexpr double kRate = 48000.0;
constexpr float kTwoPi = 6.28318531f;

juce::AudioBuffer<float> run(K& k, const juce::AudioBuffer<float>& input, int block = 512)
{
    juce::AudioBuffer<float> out(input);
    juce::MidiBuffer midi;
    for (int start = 0; start < out.getNumSamples(); start += block)
    {
        const int n = std::min(block, out.getNumSamples() - start);
        juce::AudioBuffer<float> view(out.getArrayOfWritePointers(), 2, start, n);
        k.process(view, midi);
    }
    return out;
}

juce::AudioBuffer<float> sine(float hz, int n, float gain = 0.5f)
{
    juce::AudioBuffer<float> a(2, n);
    for (int s = 0; s < n; ++s)
        for (int ch = 0; ch < 2; ++ch) a.setSample(ch, s, gain * std::sin(kTwoPi * hz * (float) s / (float) kRate));
    return a;
}

float rms(const juce::AudioBuffer<float>& a, int from, int to)
{
    double sum = 0;
    for (int s = from; s < to; ++s) sum += (double) a.getSample(0, s) * a.getSample(0, s);
    return (float) std::sqrt(sum / std::max(1, to - from));
}

float db(float v) { return 20.0f * std::log10(std::max(v, 1e-9f)); }

// A mix: a kick (pitch drop 140 -> 50 Hz, 250 ms decay, a short click) every 500 ms, steady hats (high noise bursts
// every 125 ms) and a 330 Hz pad that never stops.
struct Mix
{
    juce::AudioBuffer<float> all, kickOnly;
    std::vector<int> hits;
};
Mix makeMix(int kicks)
{
    const int period = (int) (0.5 * kRate), n = period * kicks + period / 2;
    Mix m{juce::AudioBuffer<float>(2, n), juce::AudioBuffer<float>(2, n), {}};
    m.all.clear(); m.kickOnly.clear();
    std::mt19937 rng(3);
    std::normal_distribution<float> noise(0.0f, 1.0f);
    float hp = 0, prev = 0;
    for (int s = 0; s < n; ++s)
    {
        const float t = (float) s / (float) kRate;
        const int sinceKick = s % period;
        float kick = 0;
        if (s / period < kicks)
        {
            const float tk = (float) sinceKick / (float) kRate;
            const float phase = kTwoPi * (50.0f * tk + 90.0f * 0.03f * (1.0f - std::exp(-tk / 0.03f)));
            kick = 0.8f * std::exp(-tk / 0.09f) * std::sin(phase) + 0.25f * std::exp(-tk / 0.002f) * noise(rng);
        }
        const float white = noise(rng);
        hp = 0.7f * (hp + white - prev); // crude high-pass for the hats
        prev = white;
        const int sinceHat = (s + period / 8) % (period / 4);
        const float hat = 0.12f * std::exp(-(float) sinceHat / (float) kRate / 0.03f) * hp;
        const float pad = 0.1f * std::sin(kTwoPi * 330.0f * t);
        for (int ch = 0; ch < 2; ++ch)
        {
            m.all.setSample(ch, s, kick + hat + pad);
            m.kickOnly.setSample(ch, s, kick);
        }
    }
    for (int i = 0; i < kicks; ++i) m.hits.push_back(i * period);
    return m;
}

std::unique_ptr<K> prepared()
{
    auto k = std::make_unique<K>();
    k->prepare(kRate, 512);
    return k;
}
} // namespace

TEST_CASE("kickstract: defaults follow the video (150 Hz / 1.5 kHz splits), latency is the lookahead")
{
    auto kOwner = prepared();
    auto& k = *kOwner;
    CHECK_NEAR(k.getParameter(K::LowSplit), 150.0f, 1e-3);
    CHECK_NEAR(k.getParameter(K::HighSplit), 1500.0f, 1e-3);
    CHECK(k.parameters().size() == (size_t) K::NumParams);
    CHECK(k.latencySamples() == (int) std::lround(K::kLookaheadSeconds * kRate));
    k.setParameter(K::HighSplit, 100.0f); // below the low split: kept above it
    CHECK(k.highSplitHz() >= k.lowSplitHz() * 1.25f - 1e-3f);
}

TEST_CASE("kickstract: with Chop off the three bands sum flat (Linkwitz-Riley)")
{
    for (const float hz : {40.0f, 100.0f, 150.0f, 400.0f, 1500.0f, 4000.0f, 12000.0f})
    {
        auto kOwner = prepared();
    auto& k = *kOwner;
        k.setParameter(K::Chop, 0.0f);
        k.setParameter(K::Punch, 0.0f);
        const auto in = sine(hz, 48000);
        const auto out = run(k, in);
        const float gain = db(rms(out, 24000, 48000)) - db(rms(in, 24000, 48000));
        CHECK_NEAR(gain, 0.0f, 0.1f);
    }
}

TEST_CASE("kickstract: solo picks one band")
{
    auto measure = [](int band, float hz) {
        auto kOwner = prepared();
    auto& k = *kOwner;
        k.setParameter(K::Chop, 0.0f);
        k.setParameter(K::Punch, 0.0f);
        k.setParameter(K::bandParam(band, K::BandSolo), 1.0f);
        const auto in = sine(hz, 48000);
        return db(rms(run(k, in), 24000, 48000)) - db(rms(in, 24000, 48000));
    };
    CHECK(measure(K::Low, 50.0f) > -0.5f);
    CHECK(measure(K::Low, 5000.0f) < -40.0f);
    CHECK(measure(K::Mid, 500.0f) > -1.0f);
    CHECK(measure(K::Mid, 40.0f) < -25.0f);
    CHECK(measure(K::High, 6000.0f) > -0.5f);
    CHECK(measure(K::High, 100.0f) < -40.0f);
}

TEST_CASE("kickstract: finds every kick in a mix and keeps only the kick")
{
    const auto mix = makeMix(8);
    auto kOwner = prepared();
    auto& k = *kOwner;
    k.setParameter(K::Punch, 0.0f);
    const auto out = run(k, mix.all);
    CHECK(k.triggerCount() == 8u);

    const int lat = k.latencySamples();
    float hitError = 0, gapOut = 0, gapIn = 0, kickOut = 0, kickIn = 0;
    for (const int hit : mix.hits)
    {
        // Between kicks (after Length + Fade) only the hats and the pad were there: the gates closed on them.
        const int gapFrom = hit + (int) (0.40 * kRate), gapTo = hit + (int) (0.49 * kRate);
        gapOut += rms(out, gapFrom + lat, gapTo + lat);
        gapIn += rms(mix.all, gapFrom, gapTo);
        // The hit itself: the kick's first 120 ms come through nearly whole.
        const int hitTo = hit + (int) (0.12 * kRate);
        kickOut += rms(out, hit + lat, hitTo + lat);
        kickIn += rms(mix.kickOnly, hit, hitTo);
        hitError = std::max(hitError, std::abs(db(rms(out, hit + lat, hitTo + lat)) - db(rms(mix.kickOnly, hit, hitTo))));
    }
    CHECK(db(gapOut) - db(gapIn) < -40.0f);
    // The click: the first 4 ms of each hit (where the low band has barely started) come through too: the gates are
    // already open there, so the result equals the same mix through the crossover with Chop off (all bands open).
    auto openOwner = prepared();
    openOwner->setParameter(K::Punch, 0.0f);
    openOwner->setParameter(K::Chop, 0.0f);
    const auto mixThroughFilters = run(*openOwner, mix.all);
    float clickError = 0;
    for (const int hit : mix.hits)
    {
        const int from = hit + lat, to = hit + lat + (int) (0.004 * kRate);
        clickError = std::max(clickError, std::abs(db(rms(out, from, to)) - db(rms(mixThroughFilters, from, to))));
    }
    CHECK(clickError < 2.0f);
    CHECK(std::abs(db(kickOut) - db(kickIn)) < 1.5f);
    CHECK(hitError < 2.0f);
}

TEST_CASE("kickstract: Removed holds what the gates took out; Input is the delayed input")
{
    const auto mix = makeMix(4);
    auto kOwner = prepared();
    auto& k = *kOwner;
    k.setParameter(K::Listen, (float) K::ListenRemoved);
    const auto removed = run(k, mix.all);
    const int lat = k.latencySamples();
    const int gapFrom = (int) (0.40 * kRate), gapTo = (int) (0.49 * kRate);
    CHECK_NEAR(db(rms(removed, gapFrom + lat, gapTo + lat)), db(rms(mix.all, gapFrom, gapTo)), 1.0f);

    auto plainOwner = prepared();
    auto& plain = *plainOwner;
    plain.setParameter(K::Listen, (float) K::ListenInput);
    const auto delayed = run(plain, mix.all);
    float worst = 0;
    for (int s = 0; s < 10000; ++s) worst = std::max(worst, std::abs(delayed.getSample(0, s + lat) - mix.all.getSample(0, s)));
    CHECK(worst < 1e-6f);
}

TEST_CASE("kickstract: a high threshold and a long retrigger change what counts as a hit")
{
    const auto mix = makeMix(8);
    auto deafOwner = prepared();
    auto& deaf = *deafOwner;
    deaf.setParameter(K::Threshold, 0.0f); // 0 dBFS: the kick never gets there in the low band
    run(deaf, mix.all);
    CHECK(deaf.triggerCount() == 0u);

    auto slowOwner = prepared();
    auto& slow = *slowOwner;
    slow.setParameter(K::Retrigger, 500.0f); // hits come every 500 ms: at most every other one... or each exactly
    run(slow, mix.all);
    CHECK(slow.triggerCount() <= 8u);
    CHECK(slow.triggerCount() >= 4u);
}

TEST_CASE("kickstract: the last kick is captured for the face")
{
    const auto mix = makeMix(3);
    auto kOwner = prepared();
    auto& k = *kOwner;
    K::KickShape shape;
    CHECK(! k.takeKick(shape, 0));
    run(k, mix.all);
    REQUIRE(k.takeKick(shape, 0));
    CHECK(shape.bins == K::kCaptureBins);
    CHECK(shape.windowMs > 60.0f);
    float low = 0, high = 0, top = 0;
    for (int i = 0; i < shape.bins; ++i)
    {
        low = std::max(low, shape.band[K::Low][(size_t) i]);
        high = std::max(high, shape.band[K::High][(size_t) i]);
        top = std::max(top, shape.outMax[(size_t) i]);
    }
    CHECK(low > 0.3f);   // the body is in the low band
    CHECK(high > 0.0f);  // the click in the high band
    CHECK(top > 0.3f);
    CHECK(! k.takeKick(shape, shape.serial)); // nothing new since
}

TEST_CASE("kickstract: punch and drive stay finite and bounded")
{
    const auto mix = makeMix(4);
    auto kOwner = prepared();
    auto& k = *kOwner;
    k.setParameter(K::Punch, 1.0f);
    k.setParameter(K::Drive, 1.0f);
    const auto out = run(k, mix.all);
    float peak = 0;
    bool finite = true;
    for (int s = 0; s < out.getNumSamples(); ++s)
    {
        finite &= std::isfinite(out.getSample(0, s));
        peak = std::max(peak, std::abs(out.getSample(0, s)));
    }
    CHECK(finite);
    CHECK(peak < 1.5f);
    CHECK(peak > 0.1f);
}

TEST_CASE("kickstract: catalog, factory and state round trip")
{
    const auto* entry = model::findCatalogEntry("SonoraKickstract");
    REQUIRE(entry != nullptr);
    CHECK(std::string(entry->displayName) == "Sonora Kickstract");
    CHECK(model::deviceCategory(*entry) == "EQ & Filters");
    const auto d = devices::nativeDeviceModel("SonoraKickstract");
    CHECK(d.parameters.size() == (size_t) K::NumParams);
    CHECK_NEAR(d.param("Split/Low", -1), 150.0, 1e-6);

    K a;
    a.setParameter(K::LowSplit, 90.0f);
    a.setParameter(K::bandParam(K::High, K::BandLength), 7.0f);
    a.setParameter(K::Listen, (float) K::ListenRemoved);
    const auto bytes = plugins::encodeKickstractState(a);
    K b;
    std::string error;
    REQUIRE(plugins::decodeKickstractState(bytes.data(), bytes.size(), b, error));
    for (int i = 0; i < K::NumParams; ++i) CHECK_NEAR(b.getParameter(i), a.getParameter(i), 1e-6);
    CHECK(! plugins::decodeKickstractState("nope", 4, b, error));
}
