#include "TestFramework.h"
#include "devices/native/EqEight.h"
#include "dsp/DeviceSpectrum.h"

using namespace sonora;
namespace
{
void tone(juce::AudioBuffer<float>& audio, double rate, double hz, float left = 0.5f, float right = -0.25f)
{
    for (int i = 0; i < audio.getNumSamples(); ++i)
    {
        const float sine = (float) std::sin(6.283185307179586 * hz * i / rate);
        audio.setSample(0, i, sine * left); audio.setSample(1, i, sine * right);
    }
}
float loudest(const dsp::DeviceSpectrum& display) { return *std::max_element(display.curve().begin(), display.curve().end()); }
}

TEST_CASE("EQ display: stereo spectrum locates tones at multiple sample rates without antiphase cancellation")
{
    for (double rate : {44100.0, 48000.0, 96000.0})
    {
        devices::EqEight eq; eq.prepare(rate, 4096);
        dsp::DeviceSpectrum display; display.update(eq.displayTap(), 1.0 / 30);
        juce::AudioBuffer<float> audio(2, 4096); juce::MidiBuffer midi;
        for (int frame = 0; frame < 10; ++frame)
        {
            tone(audio, rate, 1000, 0.5f, -0.5f);
            eq.process(audio, midi); display.update(eq.displayTap(), 1.0 / 30);
        }
        const auto strongest = std::max_element(display.curve().begin(), display.curve().end());
        CHECK_NEAR(dsp::SpectrumAnalyzer::curveFrequency((int) (strongest - display.curve().begin())), 1000, 50);
        CHECK(*strongest > -14); CHECK(*strongest < -5);
        CHECK_NEAR(display.levels()[0], 0.5, 0.002); CHECK_NEAR(display.levels()[1], 0.5, 0.002);
    }
}

TEST_CASE("EQ display: meters and spectrum follow filters and output gain, then decay on bypass or stop")
{
    using E = devices::EqEight;
    E eq; eq.prepare(48000, 4096);
    dsp::DeviceSpectrum display; display.update(eq.displayTap(), 1.0 / 30);
    juce::AudioBuffer<float> audio(2, 4096); juce::MidiBuffer midi;
    auto feed = [&]
    {
        tone(audio, 48000, 1000); eq.process(audio, midi); display.update(eq.displayTap(), 1.0 / 30);
    };
    for (int i = 0; i < 20; ++i) feed();
    CHECK_NEAR(display.levels()[0], 0.5, 0.002); CHECK_NEAR(display.levels()[1], 0.25, 0.002);
    const auto flat = loudest(display);
    eq.setParameter(E::kGlobalGain, -6);
    for (int i = 0; i < 30; ++i) feed();
    CHECK_NEAR(display.levels()[0], 0.5 * std::pow(10, -6.0 / 20), 0.005);
    CHECK_NEAR(loudest(display), flat - 6, 0.15);
    eq.setParameter(E::bandParam(3, E::Type), 3); eq.setParameter(E::bandParam(3, E::Freq), 1000);
    eq.setParameter(E::bandParam(3, E::GainDb), -12);
    for (int i = 0; i < 40; ++i) feed();
    CHECK(loudest(display) < flat - 16);
    eq.setBypassed(true);
    for (int i = 0; i < 100; ++i) feed();
    CHECK_NEAR(display.levels()[0], 0, 0); CHECK_NEAR(display.levels()[1], 0, 0);
    CHECK_NEAR(loudest(display), dsp::SpectrumAnalyzer::kFloorDb, 0);
    CHECK(display.holds()[0] < 0.001f);
    eq.setBypassed(false); feed();
    for (int i = 0; i < 100; ++i) display.update(eq.displayTap(), 1.0 / 30);
    CHECK_NEAR(loudest(display), dsp::SpectrumAnalyzer::kFloorDb, 0);
    CHECK_NEAR(display.levels()[0], 0, 0);
}

TEST_CASE("EQ display: analyzer off retains stereo meters and reprepare clears stale audio")
{
    devices::EqEight eq; eq.prepare(48000, 4096);
    dsp::DeviceSpectrum display; display.update(eq.displayTap(), 1.0 / 30, false);
    juce::AudioBuffer<float> audio(2, 4096); juce::MidiBuffer midi;
    tone(audio, 48000, 1000); eq.process(audio, midi);
    display.update(eq.displayTap(), 1.0 / 30, false);
    CHECK_NEAR(display.levels()[0], 0.5, 0.002);
    CHECK_NEAR(loudest(display), dsp::SpectrumAnalyzer::kFloorDb, 0);
    display.update(eq.displayTap(), 1.0 / 30, true);
    tone(audio, 48000, 1000); eq.process(audio, midi);
    eq.prepare(96000, 4096);
    display.update(eq.displayTap(), 1.0 / 30);
    CHECK_NEAR(display.levels()[0], 0, 0); CHECK_NEAR(loudest(display), dsp::SpectrumAnalyzer::kFloorDb, 0);
}

TEST_CASE("EQ display: capture queue is bounded, wraps in order and expires without a visible consumer")
{
    dsp::AudioDisplayTap tap; tap.prepare(48000);
    dsp::AudioDisplayTap::Samples left{}, right{}, outL{}, outR{};
    std::array<float, 2> peaks{};
    for (size_t i = 0; i < left.size(); ++i) { left[i] = (float) i; right[i] = -(float) i; }
    tap.push(left.data(), right.data(), 512); CHECK(tap.read(outL, outR, peaks) == 0);
    tap.request(true);
    tap.push(left.data(), right.data(), 8192); tap.push(left.data(), right.data(), 512);
    CHECK(tap.read(outL, outR, peaks) == 8192);
    CHECK(outL == left); CHECK(outR == right);
    for (int pass = 0; pass < 20; ++pass)
    {
        tap.request(true); tap.push(left.data(), right.data(), 777);
        CHECK(tap.read(outL, outR, peaks) == 777);
        CHECK_NEAR(outL[776], 776, 0); CHECK_NEAR(outR[776], -776, 0);
    }
    tap.request(false);
    tap.push(left.data(), right.data(), 8192); tap.push(left.data(), right.data(), 8192);
    tap.discard();
    tap.push(left.data(), right.data(), 512);
    CHECK(tap.read(outL, outR, peaks) == 0); CHECK_NEAR(peaks[0], 0, 0);
}

TEST_CASE("EQ display: observing the device leaves its processed audio unchanged")
{
    using E = devices::EqEight;
    E observed, reference;
    for (auto* eq : {&observed, &reference})
    {
        eq->prepare(48000, 512);
        eq->setParameter(E::bandParam(2, E::GainDb), 5);
        eq->setParameter(E::bandParamB(3, E::GainDb), -7);
        eq->setParameter(E::kGlobalGain, -3);
    }
    dsp::DeviceSpectrum display; display.update(observed.displayTap(), 1.0 / 30);
    juce::AudioBuffer<float> input(2, 512), copy(2, 512); juce::MidiBuffer midi;
    for (int mode = 0; mode < 3; ++mode)
    {
        observed.setParameter(E::kMode, (float) mode); reference.setParameter(E::kMode, (float) mode);
        for (int block = 0; block < 20; ++block)
        {
            tone(input, 48000, 730); copy.makeCopyOf(input);
            observed.process(input, midi); reference.process(copy, midi);
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < input.getNumSamples(); ++i) CHECK_NEAR(input.getSample(ch, i), copy.getSample(ch, i), 0);
            display.update(observed.displayTap(), 1.0 / 30);
        }
    }
}
