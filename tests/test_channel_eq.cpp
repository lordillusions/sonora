// DEV-30: Channel EQ (`ChannelEq`): parameters from the set, the response laws, processing and rack macros.
#include "TestFramework.h"
#include "devices/DeviceFactory.h"
#include "devices/RackProcessor.h"
#include "devices/native/ChannelEq.h"
#include "core/DeviceCatalog.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <cmath>
#include <cstdio>
#include <random>

using namespace sonora;
namespace
{
using E = devices::ChannelEq;
double responseDb(const E::Settings& s, double hz, double rate = 48000) { return 20 * std::log10(E::response(s, hz, rate)); }
float db(float gain) { return 20 * std::log10(gain); }
float gain(float decibels) { return std::pow(10.0f, decibels / 20.0f); }

std::vector<float> noise(int n, unsigned seed = 7)
{
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> u(-0.5f, 0.5f);
    std::vector<float> v((size_t) n);
    for (auto& x : v) x = u(rng);
    return v;
}

// Runs `input` through a fresh device in blocks of `block`; `change` is applied before the block starting at `at`.
std::vector<float> render(const std::vector<float>& input, int block, int at, const std::function<void(E&)>& setup,
                          const std::function<void(E&)>& change)
{
    E eq;
    setup(eq);
    eq.prepare(48000, block);
    std::vector<float> out(input.size());
    juce::AudioBuffer<float> buffer(2, block);
    juce::MidiBuffer midi;
    for (int start = 0; start < (int) input.size(); start += block)
    {
        if (start == at && change) change(eq);
        const int n = std::min(block, (int) input.size() - start);
        buffer.setSize(2, n, false, false, true);
        for (int i = 0; i < n; ++i) buffer.setSample(0, i, input[(size_t) (start + i)]), buffer.setSample(1, i, input[(size_t) (start + i)]);
        eq.process(buffer, midi);
        for (int i = 0; i < n; ++i) out[(size_t) (start + i)] = buffer.getSample(0, i);
    }
    return out;
}

// Steady-state gain of a sine at `hz` through the device.
double sineGainDb(const std::function<void(E&)>& setup, double hz)
{
    std::vector<float> in(48000);
    for (size_t i = 0; i < in.size(); ++i) in[i] = 0.25f * (float) std::sin(2 * M_PI * hz * (double) i / 48000.0);
    const auto out = render(in, 256, -1, setup, {});
    double a = 0, b = 0;
    for (size_t i = 24000; i < in.size(); ++i) { a += (double) in[i] * in[i]; b += (double) out[i] * out[i]; }
    return 10 * std::log10(b / a);
}
} // namespace

TEST_CASE("channel eq: catalog, parameters and .als values (DEV-30)")
{
    const auto* entry = model::findCatalogEntry("ChannelEq");
    REQUIRE(entry);
    CHECK(entry->implemented);
    auto model = devices::nativeDeviceModel("ChannelEq");
    CHECK(model.displayName == "Channel EQ");
    // The owner's piano rack values: HP on, Mid 1.5 kHz, shelves at the .als ranges' ends.
    for (auto& p : model.parameters)
    {
        if (p.path == "HighpassOn") p.value = 1;
        if (p.path == "LowShelfGain") p.value = 5.60000134;
        if (p.path == "HighShelfGain") p.value = 0.1800000072;
        if (p.path == "MidFrequency") p.value = 1500.00012;
        if (p.path == "Gain") p.value = 0.25;
    }
    devices::BuildContext build;
    auto processor = devices::createDevice(model, build);
    REQUIRE(processor);
    auto* eq = dynamic_cast<E*>(processor.get());
    REQUIRE(eq);
    const auto s = eq->settings();
    CHECK(s.highpass);
    CHECK_NEAR(s.lowDb, 14.96, 0.01);
    CHECK_NEAR(s.highDb, -14.89, 0.01);
    CHECK_NEAR(s.midHz, 1500, 0.01);
    CHECK_NEAR(s.outputDb, -12.04, 0.01);
    const auto& infos = eq->parameters();
    CHECK_NEAR(infos[E::LowShelfGain].minValue, 0.18, 1e-6);
    CHECK_NEAR(infos[E::LowShelfGain].maxValue, 5.6, 1e-6);
    CHECK_NEAR(infos[E::MidGain].maxValue, 4, 0);
    CHECK_NEAR(infos[E::MidFrequency].minValue, 120, 0);
    CHECK_NEAR(infos[E::MidFrequency].maxValue, 7500, 0);
    CHECK(infos[E::HighpassOn].isToggle);
}

TEST_CASE("channel eq: response laws (HP 80 Hz, adaptive low shelf, mid peak, high cut low-pass, droop) (DEV-30b)")
{
    E::Settings flat;
    CHECK_NEAR(responseDb(flat, 1000, 44100), 0.0, 1e-4);
    CHECK_NEAR(responseDb(flat, 20000, 44100), -0.47, 0.02); // the fixed top-end droop
    E::Settings hp;
    hp.highpass = true;
    CHECK_NEAR(responseDb(hp, 80, 44100), -3.01, 0.02); // first order: -3 dB at the corner
    // Low shelf: the midpoint follows the linear gain.
    CHECK_NEAR(E::lowShelfHz(14.9638), 101.0, 0.2);
    CHECK_NEAR(E::lowShelfHz(3), 178.2, 0.2);
    CHECK_NEAR(E::lowShelfHz(-3), 191.3, 0.2);
    CHECK_NEAR(E::lowShelfHz(-14.8945), 201.0, 0.2);
    E::Settings low;
    low.lowDb = 9;
    CHECK_NEAR(responseDb(low, E::lowShelfHz(9), 44100), 4.5, 0.02);
    // Mid: the set gain at the set frequency at every rate, cuts mirror boosts.
    for (double rate : {44100.0, 48000.0, 96000.0})
        for (double hz : {120.0, 1500.0, 7500.0})
        {
            E::Settings mid;
            mid.midHz = hz;
            mid.midDb = 12;
            const double boost = responseDb(mid, hz, rate) - responseDb(flat, hz, rate);
            CHECK_NEAR(boost, 12.0, 0.01);
            mid.midDb = -12;
            CHECK_NEAR(responseDb(mid, hz, rate) - responseDb(flat, hz, rate), -boost, 1e-6);
        }
    // High cut: no low-pass at or above 0 dB, measured poles at the anchors, continuous from 0 dB.
    CHECK_NEAR(E::lowpassPoles(0, 44100).hz, 0, 0);
    CHECK_NEAR(E::lowpassPoles(3, 44100).hz, 0, 0);
    CHECK_NEAR(E::lowpassPoles(-9, 44100).hz, 14765.5, 1);
    CHECK_NEAR(E::lowpassPoles(-9, 44100).q, 0.6454, 1e-3);
    E::Settings high;
    high.highDb = -1e-4;
    CHECK_NEAR(responseDb(high, 19000, 44100), responseDb(flat, 19000, 44100), 0.01);
    E::Settings out;
    out.outputDb = 6;
    CHECK_NEAR(responseDb(out, 440, 44100), 6.0, 1e-4);
}

TEST_CASE("channel eq: transfer functions match the reference's audio (DEV-30b)")
{
    // White noise through the reference's Channel EQ (suite channeleq_laws.txt, 44.1 kHz): Welch transfer functions of
    // each case stem against the source stem of the same export, in dB at 40 Hz .. 19 kHz. Settings are the stored
    // (linear) .als values. q-hi+15 is left out: the reference's 24-bit export clipped it.
    struct Case
    {
        const char* name;
        bool hp;
        float low, mid, midHz, high, output;
        std::array<double, 10> db;
    };
    const std::array<double, 10> hz{40, 100, 250, 630, 1600, 4000, 7500, 12000, 16000, 19000};
    const Case cases[] = {
        {"q-flat", false, 1.0f, 1.0f, 1500, 1.0f, 1.0f, {-0.00, -0.00, 0.00, -0.00, -0.00, -0.00, -0.01, -0.02, -0.06, -0.24}},
        {"q-hp", true, 1.0f, 1.0f, 1500, 1.0f, 1.0f, {-6.91, -2.14, -0.42, -0.07, -0.01, -0.00, -0.01, -0.02, -0.06, -0.24}},
        {"q-lo+15", false, 5.6f, 1.0f, 1500, 1.0f, 1.0f, {13.66, 7.55, 1.31, 0.17, 0.02, 0.00, -0.00, -0.02, -0.06, -0.24}},
        {"q-lo+9", false, 2.81838293f, 1.0f, 1500, 1.0f, 1.0f, {8.72, 6.85, 1.82, 0.22, 0.03, 0.00, -0.00, -0.02, -0.06, -0.24}},
        {"q-lo+3", false, 1.41253754f, 1.0f, 1500, 1.0f, 1.0f, {2.94, 2.51, 0.82, 0.10, 0.01, 0.00, -0.00, -0.02, -0.06, -0.24}},
        {"q-lo-3", false, 0.707945784f, 1.0f, 1500, 1.0f, 1.0f, {-2.95, -2.59, -0.94, -0.12, -0.02, -0.00, -0.01, -0.02, -0.06, -0.24}},
        {"q-lo-9", false, 0.354813389f, 1.0f, 1500, 1.0f, 1.0f, {-8.84, -7.79, -3.08, -0.40, -0.05, -0.01, -0.01, -0.02, -0.06, -0.24}},
        {"q-lo-15", false, 0.18f, 1.0f, 1500, 1.0f, 1.0f, {-14.62, -12.78, -5.40, -0.76, -0.10, -0.02, -0.01, -0.02, -0.06, -0.24}},
        {"q-mid+12-1k5", false, 1.0f, 4.0f, 1500, 1.0f, 1.0f, {0.05, 0.28, 1.54, 6.11, 11.97, 5.18, 1.83, 0.58, 0.14, -0.17}},
        {"q-mid+6-1k5", false, 1.0f, 1.99526231f, 1500, 1.0f, 1.0f, {0.02, 0.11, 0.66, 3.05, 5.97, 2.53, 0.80, 0.23, 0.02, -0.21}},
        {"q-mid-12-1k5", false, 1.0f, 0.25f, 1500, 1.0f, 1.0f, {-0.05, -0.28, -1.54, -6.11, -11.97, -5.18, -1.84, -0.61, -0.27, -0.31}},
        {"q-mid-6-1k5", false, 1.0f, 0.501187234f, 1500, 1.0f, 1.0f, {-0.02, -0.11, -0.66, -3.05, -5.97, -2.54, -0.81, -0.26, -0.15, -0.27}},
        {"q-mid+12-120", false, 1.0f, 4.0f, 120, 1.0f, 1.0f, {4.63, 11.54, 7.16, 1.94, 0.35, 0.05, 0.01, -0.01, -0.06, -0.24}},
        {"q-mid+12-7k5", false, 1.0f, 4.0f, 7500, 1.0f, 1.0f, {0.00, 0.02, 0.07, 0.37, 2.05, 7.52, 12.04, 8.04, 4.00, 1.57}},
        {"q-mid+12-400", false, 1.0f, 4.0f, 400, 1.0f, 1.0f, {0.63, 3.02, 9.39, 9.52, 2.99, 0.59, 0.15, 0.03, -0.05, -0.23}},
        {"q-hi+9", false, 1.0f, 1.0f, 1500, 2.81838293f, 1.0f, {0.00, 0.00, 0.02, 0.17, 1.43, 6.45, 8.45, 8.85, 8.89, 8.75}},
        {"q-hi+3", false, 1.0f, 1.0f, 1500, 1.41253754f, 1.0f, {0.00, 0.00, 0.01, 0.05, 0.46, 2.17, 2.82, 2.94, 2.92, 2.76}},
        {"q-hi-3", false, 1.0f, 1.0f, 1500, 0.707945784f, 1.0f, {-0.00, -0.00, -0.01, -0.05, -0.46, -2.19, -2.91, -3.37, -5.01, -10.11}},
        {"q-hi-9", false, 1.0f, 1.0f, 1500, 0.354813389f, 1.0f, {-0.00, -0.00, -0.03, -0.17, -1.45, -6.55, -9.16, -12.87, -20.13, -28.90}},
        {"q-hi-15", false, 1.0f, 1.0f, 1500, 0.18f, 1.0f, {-0.00, -0.01, -0.05, -0.32, -2.61, -10.76, -16.03, -23.34, -32.25, -41.36}},
        {"q-out+6", false, 1.0f, 1.0f, 1500, 1.0f, 1.99526231f, {6.00, 6.00, 6.00, 6.00, 6.00, 6.00, 6.00, 5.98, 5.94, 5.76}},
        {"q-piano", true, 0.794328235f, 1.0f, 1500, 1.25892541f, 1.0f, {-8.88, -3.86, -1.04, -0.11, 0.28, 1.44, 1.88, 1.95, 1.93, 1.76}},
        {"q-hp-lo+15", true, 5.6f, 1.0f, 1500, 1.0f, 1.0f, {6.75, 5.41, 0.89, 0.10, 0.01, 0.00, -0.00, -0.02, -0.06, -0.24}},
    };
    double worst = 0;
    for (const auto& c : cases)
    {
        E eq;
        eq.setParameter(E::HighpassOn, c.hp ? 1.0f : 0.0f);
        eq.setParameter(E::LowShelfGain, c.low);
        eq.setParameter(E::MidGain, c.mid);
        eq.setParameter(E::MidFrequency, c.midHz);
        eq.setParameter(E::HighShelfGain, c.high);
        eq.setParameter(E::Gain, c.output);
        const auto s = eq.settings();
        double caseWorst = 0;
        for (size_t i = 0; i < hz.size(); ++i)
            caseWorst = std::max(caseWorst, std::abs(responseDb(s, hz[i], 44100) - c.db[i]));
        if (caseWorst > 0.15) std::printf("  channel eq %s: %.3f dB off\n", c.name, caseWorst);
        worst = std::max(worst, caseWorst);
    }
    CHECK(worst < 0.15);
}

TEST_CASE("channel eq: the piano rack setting matches the curve the reference draws (DEV-30)")
{
    // HP on, Low -2 dB, High +2 dB: the cyan curve of the reference's Channel EQ in the owner's "Ac Piano Upright"
    // (read off a 2000 x 231 capture: 93.75 px per decade, 3.146 px per dB).
    E::Settings piano;
    piano.highpass = true;
    piano.lowDb = -2;
    piano.highDb = 2;
    const std::array<std::array<double, 2>, 15> capture{{{25, -12.61}, {40, -8.94}, {63, -6.08}, {100, -3.83}, {160, -2.11},
        {250, -0.97}, {400, -0.19}, {630, -0.00}, {1000, 0.06}, {1600, 0.34}, {2500, 0.84}, {4000, 1.57}, {6300, 1.91}, {10000, 1.95}, {16000, 1.95}}};
    double worst = 0;
    for (const auto& [hz, expected] : capture) worst = std::max(worst, std::abs(responseDb(piano, hz) - expected));
    CHECK(worst < 0.3);
}

TEST_CASE("channel eq: processing follows the response, stays finite and is block-size invariant (DEV-30)")
{
    // Sines through the running device match response() (the smoothing has settled after half a second).
    const auto setup = [](E& eq) {
        eq.setParameter(E::HighpassOn, 1);
        eq.setParameter(E::LowShelfGain, gain(-6));
        eq.setParameter(E::MidGain, gain(9));
        eq.setParameter(E::MidFrequency, 700);
        eq.setParameter(E::HighShelfGain, gain(-9));
        eq.setParameter(E::Gain, gain(3));
    };
    E probe;
    setup(probe);
    for (double hz : {40.0, 200.0, 700.0, 3000.0, 12000.0})
        CHECK_NEAR(sineGainDb(setup, hz), responseDb(probe.settings(), hz), 0.05);

    // Block sizes 1 / 64 / 256 / 512 with a setting change at the same sample give the same output.
    const auto input = noise(16384);
    const auto change = [](E& eq) {
        eq.setParameter(E::MidFrequency, 4000);
        eq.setParameter(E::HighShelfGain, gain(12));
        eq.setParameter(E::HighpassOn, 0);
    };
    const auto reference = render(input, 512, 4096, setup, change);
    for (int block : {1, 64, 256})
    {
        const auto out = render(input, block, 4096, setup, change);
        float diff = 0;
        for (size_t i = 0; i < out.size(); ++i) diff = std::max(diff, std::abs(out[i] - reference[i]));
        CHECK(diff < 1e-6f);
    }
    // Extreme settings, loud input and fast automation stay finite; bypass leaves the audio alone.
    E eq;
    eq.prepare(44100, 128);
    juce::AudioBuffer<float> buffer(2, 128);
    juce::MidiBuffer midi;
    bool finite = true;
    for (int b = 0; b < 400; ++b)
    {
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < 128; ++i) buffer.setSample(ch, i, (i % 2 == 0 ? 4.0f : -4.0f));
        eq.setParameter(E::LowShelfGain, b % 2 == 0 ? 5.6f : 0.18f);
        eq.setParameter(E::HighShelfGain, b % 3 == 0 ? 5.6f : 0.18f);
        eq.setParameter(E::MidGain, b % 5 == 0 ? 4.0f : 0.25f);
        eq.setParameter(E::MidFrequency, b % 7 == 0 ? 120.0f : 7500.0f);
        eq.setParameter(E::HighpassOn, (float) (b % 2));
        eq.process(buffer, midi);
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < 128; ++i) finite = finite && std::isfinite(buffer.getSample(ch, i)) && std::abs(buffer.getSample(ch, i)) < 1000;
    }
    CHECK(finite);
    eq.setBypassed(true);
    for (int i = 0; i < 128; ++i) buffer.setSample(0, i, 0.5f);
    eq.process(buffer, midi);
    CHECK_NEAR(buffer.getSample(0, 77), 0.5, 0);
}

TEST_CASE("channel eq: a rack macro sweeps the shelves in dB (the piano's Tone macro) (DEV-30)")
{
    auto rack = devices::nativeDeviceModel("InstrumentGroupDevice");
    auto eqModel = devices::nativeDeviceModel("ChannelEq");
    for (auto& p : eqModel.parameters)
    {
        // The owner's mapping: Low from +15 to -15 dB, High from -15 to +15 dB, both on macro 7 ("Tone", index 6).
        if (p.path == "LowShelfGain") { p.macroIndex = 6; p.rangeMin = 5.60000134; p.rangeMax = 0.1800000072; }
        if (p.path == "HighShelfGain") { p.macroIndex = 6; p.rangeMin = 0.1800000072; p.rangeMax = 5.60000134; }
    }
    model::Device::Chain chain;
    chain.devices = {eqModel};
    rack.chains = {chain};
    model::Parameter tone;
    tone.path = "MacroControls.6";
    tone.value = 72;
    std::erase_if(rack.parameters, [](const auto& p) { return p.path == "MacroControls.6"; });
    rack.parameters.push_back(tone);
    devices::BuildContext build;
    auto processor = devices::createDevice(rack, build);
    REQUIRE(processor);
    auto* r = dynamic_cast<devices::RackProcessor*>(processor.get());
    REQUIRE(r);
    r->prepare(48000, 256);
    auto* eq = dynamic_cast<E*>(r->chains()[0].devices[0].get());
    REQUIRE(eq);
    CHECK_NEAR(db(eq->getParameter(E::LowShelfGain)), -2.0, 0.06);  // the reference shows -2.0 dB
    CHECK_NEAR(db(eq->getParameter(E::HighShelfGain)), 2.0, 0.06);  // and 2.0 dB
    r->setParameter(6, 127);
    CHECK_NEAR(db(eq->getParameter(E::LowShelfGain)), -14.89, 0.01);
    CHECK_NEAR(db(eq->getParameter(E::HighShelfGain)), 14.96, 0.01);
    r->setParameter(6, 63.5f);
    CHECK_NEAR(db(eq->getParameter(E::LowShelfGain)), 0.03, 0.01); // the ends' dB midpoint
}
