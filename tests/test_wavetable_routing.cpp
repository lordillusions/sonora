#include "TestFramework.h"
#include "devices/native/WavetableSynth.h"
#include <algorithm>

using namespace sonora;
namespace
{
using W = devices::WavetableSynth;
using E = devices::WavetableEffects;
void sourceOnly(W& synth, float sustain = 1)
{
    synth.prepare(48000, 512); synth.setImportedEnvelopeProfile(true);
    synth.setParameter(W::Volume, -12); synth.setParameter(W::SourceRoutingOn, 1);
    synth.setParameter(W::NoiseOn, 0); synth.setParameter(W::SubOn, 0);
    for (int oscillator = 0; oscillator < 3; ++oscillator)
    {
        synth.setParameter(W::oscParam(oscillator, W::On), oscillator == 0 ? 1 : 0);
        synth.setParameter(W::oscParam(oscillator, W::Level), .8f);
        synth.setParameter(W::oscParam(oscillator, W::Position), 0);
        synth.setParameter(W::oscParam(oscillator, W::Unison), 1);
        synth.setParameter(W::oscParam(oscillator, W::Random), 0);
        synth.setParameter(W::oscParam(oscillator, W::Phase), .25f);
    }
    for (int filter = 0; filter < 2; ++filter)
    {
        synth.setParameter(W::filterParam(filter, W::FilterOn), 0);
        synth.setParameter(W::filterParam(filter, W::FilterMix), 0);
    }
    for (auto p : {W::Delay, W::Attack, W::Hold, W::Decay, W::Release}) synth.setParameter(W::envParam(0, p), 0);
    synth.setParameter(W::envParam(0, W::Sustain), sustain);
}
juce::AudioBuffer<float> capture(W& synth)
{
    juce::AudioBuffer<float> output(2, 4096); output.clear(); juce::MidiBuffer midi; midi.ensureSize(128);
    for (int start = 0; start < output.getNumSamples(); start += 512)
    {
        juce::AudioBuffer<float> block(output.getArrayOfWritePointers(), 2, start, 512); midi.clear();
        if (start == 0) midi.addEvent(juce::MidiMessage::noteOn(1, 69, static_cast<juce::uint8>(100)), 17);
        synth.setHostInfo({120, static_cast<double>(start)/24000, true, 48000}); synth.process(block, midi);
    }
    return output;
}
void scaled(const juce::AudioBuffer<float>& actual, const juce::AudioBuffer<float>& original, double gain)
{
    double maxResidual = 0;
    for (int channel = 0; channel < 2; ++channel) for (int sample = 17; sample < original.getNumSamples(); ++sample)
        maxResidual = std::max(maxResidual, std::abs(actual.getSample(channel, sample) - original.getSample(channel, sample)*gain));
    CHECK_NEAR(maxResidual, 0, 2e-6);
}
E::Module utility(int id, float gain)
{
    E::Module result; result.id = static_cast<E::ModuleId>(id); result.type = E::Type::Utility;
    for (const auto& descriptor : E::parameterDescriptors(result.type)) result.values.push_back(descriptor.initial);
    result.values[static_cast<size_t>(E::parameterIndex(result.type, "OutputGain"))] = gain;
    return result;
}
void graph(W& synth, bool before, float directGain = 1)
{
    E::Configuration configuration; configuration.returnBeforeMain = {before, before};
    configuration.routing.directGain = directGain; configuration.buses[0] = {utility(1, 0)};
    std::string error; REQUIRE(synth.setEffectsConfiguration(configuration, error));
}
}

TEST_CASE("wavetable routing: seven source sends cube independently while None removes only ordinary output")
{
    W baseline; sourceOnly(baseline); const auto reference = capture(baseline);
    for (int bus = 0; bus < 2; ++bus) for (const float send : {.25f, .5f, .75f, 1.0f})
    {
        W synth; sourceOnly(synth); synth.setParameter(W::sourceRoutingParam(0, W::SourceDestination), W::RouteToNone);
        synth.setParameter(W::sourceRoutingParam(0, bus == 0 ? W::SourceBus1 : W::SourceBus2), send);
        scaled(capture(synth), reference, send*send*send);
    }
    W additive; sourceOnly(additive); additive.setParameter(W::sourceRoutingParam(0, W::SourceBus1), 1);
    additive.setParameter(W::sourceRoutingParam(0, W::SourceBus2), 1); scaled(capture(additive), reference, 3);
}

TEST_CASE("wavetable routing: pre-Main returns share Main mute and post returns share Direct gain")
{
    W baseline; sourceOnly(baseline); const auto reference = capture(baseline);
    for (int bus = 0; bus < 2; ++bus) for (const bool before : {false, true}) for (const float direct : {0.0f, .25f, 1.0f, 2.25f, 4.0f})
    {
        W synth; sourceOnly(synth); graph(synth, before, direct);
        synth.setParameter(W::sourceRoutingParam(0, W::SourceDestination), W::RouteToNone);
        synth.setParameter(W::sourceRoutingParam(0, bus == 0 ? W::SourceBus1 : W::SourceBus2), .5f);
        scaled(capture(synth), reference, before ? 0 : .125*direct);
        CHECK_NEAR(synth.getParameter(W::DirectGain), direct, 0);
        synth.setParameter(W::DirectGain, 0); CHECK_NEAR(synth.effects()->getRoutingParameter(5), 0, 0);
    }
    W bypass; sourceOnly(bypass); graph(bypass, true, .25f);
    bypass.setParameter(W::sourceRoutingParam(0, W::SourceDestination), W::RouteToDirect);
    scaled(capture(bypass), reference, .25);
}

TEST_CASE("wavetable routing: source envelope placement is independent of filter output and serial orientation")
{
    W baseline; sourceOnly(baseline); const auto reference = capture(baseline);
    for (const bool via : {false, true}) for (const int destination : {W::RouteToMain, W::RouteToDirect, W::RouteToNone})
    {
        W synth; sourceOnly(synth, .5f);
        synth.setParameter(W::sourceRoutingParam(0, W::SourceDestination), static_cast<float>(destination));
        synth.setParameter(W::sourceRoutingParam(0, W::SourceEnvelope), via ? 1 : 0);
        if (destination == W::RouteToNone) synth.setParameter(W::sourceRoutingParam(0, W::SourceBus1), 1);
        scaled(capture(synth), reference, via ? .25 : 1);
    }
    for (const int first : {0, 1}) for (const bool sourceVia : {false, true}) for (const bool firstVia : {false, true}) for (const bool finalVia : {false, true})
    {
        W synth; sourceOnly(synth, .5f); const int second = 1-first;
        synth.setParameter(W::sourceRoutingParam(0, W::SourceFilterBalance), first == 0 ? -1 : 1);
        synth.setParameter(W::sourceRoutingParam(0, W::SourceEnvelope), sourceVia ? 1 : 0);
        synth.setParameter(W::sourceRoutingParam(5+first, W::SourceDestination), W::RouteToFilters);
        synth.setParameter(W::sourceRoutingParam(5+first, W::SourceEnvelope), firstVia ? 1 : 0);
        synth.setParameter(W::sourceRoutingParam(5+second, W::SourceEnvelope), finalVia ? 1 : 0);
        scaled(capture(synth), reference, finalVia ? .25 : 1);
    }
}

TEST_CASE("wavetable routing: enabled filter output Level scales serial signal but bypass ignores Level")
{
    W baseline; sourceOnly(baseline); const auto reference = capture(baseline);
    for (const int filter : {0, 1}) for (const bool on : {false, true}) for (const float gain : {0.0f, .25f, 1.0f, 2.25f, 4.0f})
    {
        W synth; sourceOnly(synth);
        synth.setParameter(W::sourceRoutingParam(0, W::SourceFilterBalance), filter == 0 ? -1 : 1);
        synth.setParameter(W::filterParam(filter, W::FilterOn), on ? 1 : 0); synth.setParameter(W::filterOutputParam(filter), gain);
        scaled(capture(synth), reference, on ? gain : 1);
    }
}

TEST_CASE("wavetable routing: independent matrix send targets sum normalized depths before each source gain cube")
{
    for (int source = 0; source < W::kRoutingSources; ++source) for (int bus = 0; bus < 2; ++bus)
    {
        const auto isolate = [source](W& synth) {
            sourceOnly(synth);
            if (source == 1 || source == 2)
            {
                synth.setParameter(W::oscParam(0, W::On), 0); synth.setParameter(W::oscParam(source, W::On), 1);
                synth.setParameter(W::sourceRoutingParam(source, W::SourceDestination), W::RouteToMain);
            }
            else if (source == 3 || source == 4)
            {
                synth.setParameter(W::oscParam(0, W::On), 0);
                synth.setParameter(source == 3 ? W::NoiseOn : W::SubOn, 1);
                synth.setParameter(source == 3 ? W::NoiseLevel : W::SubLevel, .8f);
            }
            else if (source >= 5)
                synth.setParameter(W::sourceRoutingParam(0, W::SourceFilterBalance), source == 5 ? -1 : 1);
            else synth.setParameter(W::sourceRoutingParam(0, W::SourceDestination), W::RouteToMain);
        };
        W baseline; isolate(baseline); const auto reference = capture(baseline);
        for (const float macro : {0.0f, .5f, 1.0f})
        {
            W synth; isolate(synth); const int target = W::sourceSendDestination(source, bus);
            REQUIRE(synth.setImportedModulationDomain(target, W::ImportedModulationDomain::SourceSend));
            synth.setParameter(W::sourceRoutingParam(source, W::SourceDestination), W::RouteToNone);
            synth.setParameter(W::sourceRoutingParam(source, bus == 0 ? W::SourceBus1 : W::SourceBus2), .25f);
            for (int slot = 0; slot < 2; ++slot)
            {
                synth.setParameter(W::modParam(slot, W::ModSource), W::Macro1);
                synth.setParameter(W::modParam(slot, W::ModDestination), static_cast<float>(target));
                synth.setParameter(W::modParam(slot, W::ModAmount), .5f);
            }
            synth.setParameter(W::macroParam(0), macro);
            const double send = std::clamp(.25 + macro, 0.0, 1.0);
            scaled(capture(synth), reference, send * send * send);
            CHECK_NEAR(synth.getParameter(W::sourceRoutingParam(source, bus == 0 ? W::SourceBus1 : W::SourceBus2)), .25, 0);
        }
    }
}

TEST_CASE("wavetable routing: imported send matrix applies centered bipolar Via and Out before clipping without changing the base")
{
    W baseline; sourceOnly(baseline); const auto reference = capture(baseline);
    for (const float amount : {-1.0f, .5f, 1.0f}) for (const bool bipolar : {false, true})
    {
        W synth; sourceOnly(synth); const int target = W::sourceSendDestination(0, 0);
        REQUIRE(synth.setImportedModulationDomain(target, W::ImportedModulationDomain::SourceSend));
        synth.setParameter(W::sourceRoutingParam(0, W::SourceDestination), W::RouteToNone);
        synth.setParameter(W::sourceRoutingParam(0, W::SourceBus1), .25f);
        synth.setParameter(W::modParam(0, W::ModSource), W::Macro1);
        synth.setParameter(W::modParam(0, W::ModDestination), static_cast<float>(target));
        synth.setParameter(W::modParam(0, W::ModAmount), amount);
        synth.setParameter(W::modParam(0, W::ModBipolar), bipolar ? 1 : 0);
        synth.setParameter(W::modParam(0, W::AuxSource), W::Macro2);
        synth.setParameter(W::outParam(0), .5f);
        synth.setParameter(W::macroParam(0), 1); synth.setParameter(W::macroParam(1), .5f);
        const double send = std::clamp(.25 + (bipolar ? .5 : 1) * amount * .25, 0.0, 1.0);
        scaled(capture(synth), reference, send * send * send);
    }
}
