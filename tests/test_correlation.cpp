// PLG-24: Sonora Correlation measures what left and right have in common (wideband, per third octave, the low end) and
// passes the audio through untouched unless Listen is on a check mode.
#include "TestFramework.h"
#include "core/DeviceCatalog.h"
#include "devices/DeviceFactory.h"
#include "devices/native/Correlation.h"
#include "io/als/LiveCompat.h"
#include "plugins/SonoraPluginState.h"

#include <cmath>
#include <functional>
#include <random>

using namespace sonora;
namespace
{
using C = devices::Correlation;
constexpr double kRate = 48000.0;
constexpr double kPi = 3.14159265358979323846;
using Source = std::function<std::pair<float, float>(int sample)>;

// Runs `seconds` of `source` through the device in 512-sample blocks; returns the output of the last block and checks
// (when `untouched`) that every output sample equals its input.
juce::AudioBuffer<float> run(C& meter, const Source& source, double seconds, int& at, bool* untouched = nullptr)
{
    const int total = (int) (seconds * kRate);
    juce::AudioBuffer<float> block(2, 512);
    juce::MidiBuffer midi;
    for (int done = 0; done < total; done += 512)
    {
        const int n = std::min(512, total - done);
        block.setSize(2, n, false, false, true);
        for (int s = 0; s < n; ++s)
        {
            const auto [l, r] = source(at + s);
            block.setSample(0, s, l);
            block.setSample(1, s, r);
        }
        juce::AudioBuffer<float> input(block);
        meter.process(block, midi);
        if (untouched != nullptr)
            for (int s = 0; s < n; ++s)
                for (int ch = 0; ch < 2; ++ch)
                    if (std::abs(block.getSample(ch, s) - input.getSample(ch, s)) > 0.0f) *untouched = false;
        at += n;
    }
    return block;
}

float sine(double hz, int s, double phase = 0.0) { return (float) std::sin(2.0 * kPi * hz * (double) s / kRate + phase); }

int bandOf(float hz)
{
    for (int b = 0; b < C::kBands; ++b)
        if (std::abs(C::bandCentre(b) / hz - 1.0f) < 0.1f) return b;
    return -1;
}
} // namespace

TEST_CASE("correlation: mono, inverted, unrelated and 90 degrees; balance, width and the mono sum; audio untouched")
{
    C meter;
    meter.prepare(kRate, 512);
    int at = 0;
    bool untouched = true;
    run(meter, [](int s) { const float x = 0.5f * sine(440, s); return std::make_pair(x, x); }, 1.0, at, &untouched);
    auto r = meter.reading();
    CHECK(untouched);
    CHECK(r.signal);
    CHECK(r.correlation > 0.999f);
    CHECK(r.width < 0.01f);
    CHECK_NEAR(r.monoDb, 0.0, 0.01);
    CHECK_NEAR(r.balanceDb, 0.0, 0.01);
    CHECK_NEAR(r.levelL, 20.0 * std::log10(0.5 / std::sqrt(2.0)), 0.2);

    meter.reset();
    run(meter, [](int s) { const float x = 0.5f * sine(440, s); return std::make_pair(x, -x); }, 1.0, at, &untouched);
    r = meter.reading();
    CHECK(untouched);
    CHECK(r.correlation < -0.999f);
    CHECK(r.monoDb <= -59.0f); // the floor: nothing left in mono
    CHECK(r.width > 9.0f);

    meter.reset();
    std::mt19937 random(7);
    std::uniform_real_distribution<float> noise(-0.5f, 0.5f);
    run(meter, [&](int) { return std::make_pair(noise(random), noise(random)); }, 3.0, at);
    r = meter.reading();
    CHECK(std::abs(r.correlation) < 0.1f);
    CHECK_NEAR(r.monoDb, -3.0, 0.5);
    CHECK_NEAR(r.width, 1.0, 0.15);

    meter.reset();
    run(meter, [](int s) { return std::make_pair(0.5f * sine(1000, s), 0.5f * sine(1000, s, kPi / 2)); }, 1.0, at);
    CHECK(std::abs(meter.reading().correlation) < 0.02f);

    meter.reset();
    run(meter, [](int s) { const float x = sine(440, s); return std::make_pair(0.5f * x, 0.25f * x); }, 1.0, at);
    r = meter.reading();
    CHECK(r.correlation > 0.999f);
    CHECK_NEAR(r.balanceDb, -6.02, 0.05); // the right is 6 dB quieter
}

TEST_CASE("correlation: per band and the low end (an inverted 63 Hz under a mono 1 kHz)")
{
    C meter;
    meter.prepare(kRate, 512);
    int at = 0;
    run(meter, [](int s) {
        const float low = 0.4f * sine(63, s), high = 0.3f * sine(1000, s);
        return std::make_pair(high + low, high - low);
    }, 2.0, at);
    const auto low = meter.band(bandOf(63)), high = meter.band(bandOf(1000)), empty = meter.band(bandOf(8000));
    CHECK(low.signal);
    CHECK(low.correlation < -0.9f);
    CHECK(high.signal);
    CHECK(high.correlation > 0.9f);
    CHECK(empty.levelDb < low.levelDb - 40.0f);
    const auto r = meter.reading();
    CHECK(r.lowSignal);
    CHECK(r.lowEnd < -0.8f);
    CHECK(r.correlation < 0.0f); // more inverted low end than mono highs
    // At 44.1 kHz the 20 kHz band is past the sample rate's range.
    C slow;
    slow.prepare(44100.0, 512);
    CHECK(! slow.band(C::kBands - 1).active);
    CHECK(slow.band(C::kBands - 2).active);
    CHECK(C::bandLabel(0) == "20");
    CHECK(C::bandLabel(17) == "1k");
    CHECK_NEAR(C::bandCentre(17), 1000.0, 1e-3);
}

TEST_CASE("correlation: silence, NaN input, holds, reset, history, the scope and Freeze")
{
    C meter;
    meter.prepare(kRate, 512);
    int at = 0;
    run(meter, [](int) { return std::make_pair(0.0f, 0.0f); }, 0.5, at);
    CHECK(! meter.reading().signal);
    CHECK(std::abs(meter.reading().correlation) <= 0.0f);
    run(meter, [](int s) { return s % 100 == 0 ? std::make_pair(NAN, INFINITY) : std::make_pair(0.0f, 0.0f); }, 0.2, at);
    const auto mono = [](int s) { const float x = 0.5f * sine(220, s); return std::make_pair(x, x); };
    const auto inverted = [](int s) { const float x = 0.5f * sine(220, s); return std::make_pair(x, -x); };
    run(meter, mono, 1.0, at);
    CHECK(meter.reading().correlation > 0.99f); // the NaNs left nothing behind

    meter.setParameter(C::Hold, (float) C::Hold1s);
    meter.setParameter(C::Integration, 50.0f);
    run(meter, inverted, 0.5, at);
    run(meter, mono, 0.5, at);
    auto r = meter.reading();
    CHECK(r.correlation > 0.99f);
    CHECK(r.minHeld < -0.95f); // held for 1 s
    CHECK(r.maxHeld > 0.99f);
    run(meter, mono, 3.5, at); // the hold ends, then it moves back at 1.0 a second
    CHECK(meter.reading().minHeld > 0.95f);

    meter.setParameter(C::Hold, (float) C::HoldForever);
    run(meter, inverted, 0.3, at);
    run(meter, mono, 4.0, at);
    CHECK(meter.reading().minHeld < -0.95f);
    CHECK(meter.band(bandOf(200)).minHeld < -0.9f);
    meter.requestReset();
    run(meter, mono, 0.1, at);
    CHECK(meter.reading().minHeld > 0.95f);
    CHECK(meter.band(bandOf(200)).minHeld > 0.0f);

    // History: 40 readings a second, newest first.
    CHECK(meter.historyCount() >= 3u);
    CHECK(meter.historyCount() <= 5u);
    run(meter, mono, 10.0, at);
    CHECK(meter.historyCount() == (uint32_t) C::kHistory);
    CHECK(meter.history(0).signal);
    CHECK(meter.history(0).correlation > 0.99f);
    CHECK(! meter.history(C::kHistory).signal);

    // The scope: mono is vertical (x = 0), y = (L + R) / sqrt 2.
    std::vector<float> x(256), y(256);
    REQUIRE(meter.scope(x.data(), y.data(), 256) == 256);
    float peakY = 0;
    for (int i = 0; i < 256; ++i)
    {
        CHECK(std::abs(x[(size_t) i]) < 1e-6f);
        peakY = std::max(peakY, std::abs(y[(size_t) i]));
    }
    CHECK_NEAR(peakY, 0.5 * std::sqrt(2.0), 0.01);

    // Freeze: the readings and the scope stop, the audio still passes.
    meter.setFrozen(true);
    const auto seen = meter.samplesSeen();
    const auto before = meter.reading();
    bool untouched = true;
    run(meter, inverted, 0.5, at, &untouched);
    CHECK(untouched);
    CHECK(meter.samplesSeen() == seen);
    CHECK(std::abs(meter.reading().correlation - before.correlation) <= 0.0f);
    CHECK(meter.reading().frozen);
    meter.setFrozen(false);
    run(meter, inverted, 0.5, at);
    CHECK(meter.reading().correlation < -0.99f);
    CHECK(meter.samplesSeen() > seen);
}

TEST_CASE("correlation: Listen modes replace the output (crossfaded), Stereo is untouched again")
{
    C meter;
    meter.prepare(kRate, 512);
    int at = 0;
    const auto source = [](int s) { return std::make_pair(0.6f * sine(300, s), 0.2f * sine(700, s)); };
    const auto expect = [&](int mode, const juce::AudioBuffer<float>& out, int startSample) {
        int bad = 0;
        for (int s = 0; s < out.getNumSamples(); ++s)
        {
            const auto [l, r] = source(startSample + s);
            float el = l, er = r;
            if (mode == C::ListenMono) el = er = 0.5f * (l + r);
            else if (mode == C::ListenSide) el = er = 0.5f * (l - r);
            else if (mode == C::ListenLeft) el = er = l;
            else if (mode == C::ListenRight) el = er = r;
            if (std::abs(out.getSample(0, s) - el) > 1e-6f || std::abs(out.getSample(1, s) - er) > 1e-6f) ++bad;
        }
        return bad;
    };
    for (const int mode : {C::ListenMono, C::ListenSide, C::ListenLeft, C::ListenRight})
    {
        meter.setParameter(C::Listen, (float) mode);
        run(meter, source, 0.05, at); // past the 10 ms crossfade
        const int start = at;
        const auto out = run(meter, source, 512.0 / kRate, at);
        CHECK(expect(mode, out, start) == 0);
    }
    // The switch itself is a crossfade: no jump from Right to Mono in one sample.
    meter.setParameter(C::Listen, (float) C::ListenMono);
    const int start = at;
    const auto fading = run(meter, source, 512.0 / kRate, at);
    const auto [l0, r0] = source(start);
    CHECK(std::abs(fading.getSample(0, 0) - r0) < 0.02f);
    (void) l0;
    meter.setParameter(C::Listen, (float) C::ListenStereo);
    run(meter, source, 0.05, at);
    bool untouched = true;
    run(meter, source, 0.2, at, &untouched);
    CHECK(untouched);
}

TEST_CASE("correlation: catalog, the built-in device for the VST3's class id, and its state")
{
    const auto* entry = model::findCatalogEntry("SonoraCorrelation");
    REQUIRE(entry != nullptr);
    CHECK(std::string(entry->displayName) == "Sonora Correlation");
    CHECK(model::deviceCategory(*entry) == "Utilities");
    auto d = devices::nativeDeviceModel("SonoraCorrelation");
    CHECK(dynamic_cast<C*>(devices::createNative(d).get()) != nullptr);
    C a;
    a.setParameter(C::Integration, 1200.0f);
    a.setParameter(C::Hold, (float) C::HoldForever);
    a.setParameter(C::ScopeMode, (float) C::Polar);
    a.setParameter(C::ScopeAuto, 0.0f);
    a.setParameter(C::ScopeZoom, 12.0f);
    a.setParameter(C::Listen, (float) C::ListenSide);
    const auto bytes = plugins::encodeParameterState(a, "SonoraCorrelation");
    C b;
    std::string error;
    REQUIRE(plugins::decodeParameterState(bytes.data(), bytes.size(), b, "SonoraCorrelation", "Sonora Correlation", error));
    for (int i = 0; i < C::NumParams; ++i) CHECK_NEAR(b.getParameter(i), a.getParameter(i), 1e-6);

    model::PluginInfo info;
    const auto uid = io::sonoraVst3Uid("SonoraCorrelation");
    for (size_t i = 0; i < 4; ++i) info.vst3Uid[i] = uid[i];
    CHECK(plugins::sonoraPluginFor(info, model::DeviceKind::PluginVst3) == plugins::SonoraPlugin::Correlation);
}
