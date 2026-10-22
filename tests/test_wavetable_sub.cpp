#include "TestFramework.h"
#include "devices/native/WavetableSynth.h"
#include "dsp/Dsp.h"

#include <algorithm>
#include <cmath>
#include <limits>

using namespace sonora;
namespace
{
using W = devices::WavetableSynth;
void subOnly(W& synth)
{
    synth.prepare(48000, 512);
    synth.setParameter(W::Volume, -12);
    synth.setParameter(W::SubOn, 1); synth.setParameter(W::SubLevel, 1); synth.setParameter(W::SubOctave, 0);
    synth.setParameter(W::NoiseOn, 0);
    for (int oscillator = 0; oscillator < W::kOscillators; ++oscillator)
        synth.setParameter(W::oscParam(oscillator, W::On), 0);
    for (int filter = 0; filter < W::kFilters; ++filter)
        synth.setParameter(W::filterParam(filter, W::FilterOn), 0);
    for (const auto parameter : {W::Delay, W::Attack, W::Hold, W::Decay, W::Release})
        synth.setParameter(W::envParam(0, parameter), 0);
    synth.setParameter(W::envParam(0, W::Sustain), 1);
}
juce::AudioBuffer<float> renderSubSequence(W& synth, const juce::MidiBuffer& sequence)
{
    constexpr int frames = 52000;
    juce::AudioBuffer<float> output(2, frames); output.clear();
    juce::MidiBuffer midi; midi.ensureSize(128);
    for (int start = 0; start < frames; start += 512)
    {
        const int count = std::min(512, frames - start); midi.clear(); midi.addEvents(sequence, start, count, -start);
        juce::AudioBuffer<float> block(output.getArrayOfWritePointers(), 2, start, count);
        synth.setHostInfo({120, static_cast<double>(start) / 24000, true, 48000}); synth.process(block, midi);
    }
    return output;
}
juce::AudioBuffer<float> renderSub(W& synth, int secondNote = -1, bool releaseFirst = true)
{
    juce::MidiBuffer sequence; sequence.ensureSize(128);
    sequence.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)), 17);
    if (releaseFirst) sequence.addEvent(juce::MidiMessage::noteOff(1, 60), 12000);
    if (secondNote >= 0) sequence.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)), secondNote);
    return renderSubSequence(synth, sequence);
}
void scaledWindow(const juce::AudioBuffer<float>& actual, int from, const juce::AudioBuffer<float>& reference, int referenceFrom, int count, double left, double right)
{
    double residual = 0;
    for (int channel = 0; channel < 2; ++channel) for (int sample = 0; sample < count; ++sample)
        residual = std::max(residual, std::abs(actual.getSample(channel, from + sample)
            - reference.getSample(channel, referenceFrom + sample) * (channel == 0 ? left : right)));
    CHECK_NEAR(residual, 0, 2e-6);
}
}

TEST_CASE("wavetable Sub: imported sine scale and polarity retain the own signal and actual preview")
{
    W own, imported; subOnly(own); subOnly(imported); imported.setImportedSubProfile(true);
    const auto reference = renderSub(own), result = renderSub(imported);
    const double scale = -dsp::dbToGain(5.9f) / 3 * 1.00316f;
    scaledWindow(result, 17, reference, 17, 11000, scale, scale);
    CHECK_NEAR(own.subWaveformAt(.25f), 1, 1e-6); CHECK_NEAR(imported.subWaveformAt(.25f), scale, 1e-6);
    imported.setParameter(W::SubPhase, .25f); CHECK_NEAR(imported.subWaveformAt(0), scale, 1e-6);
    imported.setParameter(W::SubContinuous, 1); CHECK_NEAR(imported.subWaveformAt(0), 0, 1e-6);
    CHECK(std::isfinite(imported.subWaveformAt(-.25f))); CHECK_NEAR(imported.subWaveformAt(std::numeric_limits<float>::quiet_NaN()), 0, 0);
}

TEST_CASE("wavetable Sub: phase continuation retains active cycles through unequal silent gaps and reset clears it")
{
    W sustained; subOnly(sustained); const auto reference = renderSub(sustained, -1, false);
    for (const int second : {36000, 48000}) for (const float phase : {0.0f, .25f})
    {
        W continuous; subOnly(continuous); continuous.setParameter(W::SubPhase, phase); continuous.setParameter(W::SubContinuous, 1);
        const auto result = renderSub(continuous, second);
        scaledWindow(result, 17, reference, 17, 1000, 1, 1);
        scaledWindow(result, second, reference, 12000, 2000, 1, 1);
        for (int sample = 13000; sample < second; ++sample) CHECK_NEAR(result.getSample(0, sample), 0, 0);
        continuous.reset(); const auto afterReset = renderSub(continuous);
        scaledWindow(afterReset, 17, reference, 17, 1000, 1, 1);
    }
    W retrigger; subOnly(retrigger); retrigger.setParameter(W::SubPhase, .25f); const auto result = renderSub(retrigger, 36000);
    scaledWindow(result, 36000, result, 17, 2000, 1, 1);
}

TEST_CASE("wavetable Sub: quadratic imported pan keeps center and endpoints distinct from own balance")
{
    W centered; subOnly(centered); centered.setImportedSubProfile(true); const auto reference = renderSub(centered);
    for (const float u : {0.0f, .1f, .2f, .25f, .3f, .4f, .5f, .6f, .7f, .75f, .8f, .9f, 1.0f})
    {
        W synth; subOnly(synth); synth.setImportedSubProfile(true); synth.setParameter(W::SubPan, 2 * u - 1);
        const auto result = renderSub(synth);
        scaledWindow(result, 17, reference, 17, 11000, W::observedPanGain(u, false), W::observedPanGain(u, true));
        CHECK_NEAR(W::observedPanGain(u, false), W::observedPanGain(1-u, true), 2e-7);
    }
    CHECK_NEAR(W::observedPanGain(.5f, false), 1, 1e-7);
    CHECK_NEAR(W::observedPanGain(0, false), 1.65570443, 2e-7);
    W own; subOnly(own); own.setParameter(W::SubPan, -1); const auto ownResult = renderSub(own);
    W ownCenter; subOnly(ownCenter); scaledWindow(ownResult, 17, renderSub(ownCenter), 17, 11000, 1, 0);
}

TEST_CASE("wavetable Sub: newest active phase survives voice pool reuse and retiring older notes")
{
    W synth; subOnly(synth); synth.setParameter(W::SubContinuous, 1);
    struct Tone { int note, on, off; double frequency, phase = 0; };
    std::array<Tone, 5> tones{{{69, 17, 30000, 440}, {72, 24000, 42000, 0},
        {67, 36000, 45000, 0}, {76, 39000, 52000, 0}, {64, 48000, 52000, 0}}};
    const auto wrap = [](double phase) { return phase - std::floor(phase); };
    juce::MidiBuffer sequence; sequence.ensureSize(256);
    for (size_t i = 0; i < tones.size(); ++i)
    {
        auto& tone = tones[i]; tone.frequency = 440 * std::exp2(static_cast<double>(tone.note - 69) / 12);
        if (i > 0)
        {
            const auto& carrier = tones[i - 1];
            tone.phase = wrap(carrier.phase + static_cast<double>(tone.on - carrier.on) * carrier.frequency / 48000);
        }
        sequence.addEvent(juce::MidiMessage::noteOn(1, tone.note, static_cast<juce::uint8>(100)), tone.on);
        if (tone.off < 52000) sequence.addEvent(juce::MidiMessage::noteOff(1, tone.note), tone.off);
    }
    const auto audio = renderSubSequence(synth, sequence); double residual = 0;
    for (int sample = 17; sample < 52000; ++sample)
    {
        double expected = 0;
        for (const auto& tone : tones) if (sample >= tone.on && sample < tone.off)
            expected += std::sin(6.283185307179586 * wrap(tone.phase + static_cast<double>(sample - tone.on) * tone.frequency / 48000));
        expected *= dsp::dbToGain(-12.0f);
        residual = std::max(residual, std::abs(audio.getSample(0, sample) - expected));
    }
    CHECK_NEAR(residual, 0, 4e-6);
}
