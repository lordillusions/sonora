// DEV-30: Channel EQ (`ChannelEq`): parameters from the set, the response laws, processing and rack macros.
#include "TestFramework.h"
#include "devices/DeviceFactory.h"
#include "devices/RackProcessor.h"
#include "devices/native/ChannelEq.h"
#include "core/DeviceCatalog.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <cmath>
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

TEST_CASE("channel eq: response laws (HP 80 Hz, shelves, mid peak, high cut low-pass, output) (DEV-30)")
{
    E::Settings flat;
    for (double hz : {20.0, 100.0, 1000.0, 10000.0, 20000.0}) CHECK_NEAR(responseDb(flat, hz), 0.0, 1e-6);
    E::Settings hp;
    hp.highpass = true;
    CHECK_NEAR(responseDb(hp, 80), -3.01, 0.02);  // first order: -3 dB at the corner
    CHECK_NEAR(responseDb(hp, 20), -12.3, 0.1);   // 6 dB per octave below it
    CHECK_NEAR(responseDb(hp, 1000), 0.0, 0.03);
    E::Settings low;
    low.lowDb = 15;
    CHECK_NEAR(responseDb(low, 20), 14.8, 0.3);
    CHECK_NEAR(responseDb(low, E::kLowShelfHz), 7.5, 0.02); // the shelf's midpoint
    CHECK_NEAR(responseDb(low, 5000), 0.0, 0.05);
    E::Settings mid;
    for (double hz : {120.0, 1500.0, 7500.0})
    {
        mid.midHz = hz;
        mid.midDb = 12;
        CHECK_NEAR(responseDb(mid, hz), 12.0, 0.01);
        mid.midDb = -12;
        CHECK_NEAR(responseDb(mid, hz), -12.0, 0.01);
    }
    E::Settings high;
    high.highDb = 15;
    CHECK_NEAR(responseDb(high, E::kHighShelfHz), 7.5, 0.02);
    CHECK_NEAR(responseDb(high, 50), 0.0, 0.05);
    CHECK_NEAR(E::lowpassHz(0), 0, 0);
    CHECK_NEAR(E::lowpassHz(3), 0, 0);
    CHECK_NEAR(E::lowpassHz(-15), 8000, 0.01);   // manual: 20 kHz at 0 dB down to 8 kHz at -15 dB
    CHECK_NEAR(E::lowpassHz(-7.5), 12649, 1);
    high.highDb = -15;
    CHECK(responseDb(high, 8000) < -15.0);        // the shelf plus the low-pass at its corner
    CHECK(responseDb(high, 16000) < responseDb(high, 8000) - 6.0);
    E::Settings out;
    out.outputDb = 6;
    CHECK_NEAR(responseDb(out, 440), 6.0, 1e-6);
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
