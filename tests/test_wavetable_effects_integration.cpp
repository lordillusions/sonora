#include "TestFramework.h"
#include "devices/native/WavetableSynth.h"
#include "dsp/Dsp.h"

#include <algorithm>
#include <limits>
#include <set>

using namespace sonora;

namespace
{
using W = devices::WavetableSynth;
using E = devices::WavetableEffects;
constexpr double rate = 48000;
E::Module effect(E::ModuleId id, E::Type type)
{
    E::Module module;
    module.id = id; module.type = type;
    for (const auto& descriptor : E::parameterDescriptors(type)) module.values.push_back(descriptor.initial);
    return module;
}
void set(E::Module& module, std::string_view name, float value)
{
    const int index = E::parameterIndex(module.type, name);
    REQUIRE(index >= 0);
    module.values[static_cast<size_t>(index)] = value;
}
void configure(W& synth, const E::Configuration& configuration)
{
    std::string error;
    REQUIRE(synth.setEffectsConfiguration(configuration, error)); CHECK(error.empty());
}
void isolated(W& synth, int capacity = 512)
{
    synth.prepare(rate, capacity);
    synth.setParameter(W::Volume, -12);
    synth.setParameter(W::SubOn, 0); synth.setParameter(W::NoiseOn, 0);
    for (int oscillator = 0; oscillator < W::kOscillators; ++oscillator)
    {
        synth.setParameter(W::oscParam(oscillator, W::On), oscillator == 0 ? 1.0f : 0.0f);
        synth.setParameter(W::oscParam(oscillator, W::Table), W::Basic);
        synth.setParameter(W::oscParam(oscillator, W::Position), 0);
        synth.setParameter(W::oscParam(oscillator, W::Level), 0.8f);
        synth.setParameter(W::oscParam(oscillator, W::Random), 0);
        synth.setParameter(W::oscParam(oscillator, W::Phase), 0.25f);
        synth.setParameter(W::oscParam(oscillator, W::Unison), 1);
    }
    for (int filter = 0; filter < W::kFilters; ++filter) synth.setParameter(W::filterParam(filter, W::FilterOn), 0);
    for (auto parameter : {W::Delay, W::Attack, W::Hold, W::Decay, W::Release}) synth.setParameter(W::envParam(0, parameter), 0);
    synth.setParameter(W::envParam(0, W::Sustain), 1);
}
juce::MidiBuffer events(int start = 17, int end = 3000)
{
    juce::MidiBuffer midi;
    midi.ensureSize(256);
    midi.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)), start);
    if (end >= 0) midi.addEvent(juce::MidiMessage::noteOff(1, 60), end);
    return midi;
}
juce::AudioBuffer<float> render(W& synth, const juce::MidiBuffer& input, int samples = 8192, int blockSize = 512, float foreign = 0)
{
    juce::AudioBuffer<float> result(2, samples);
    for (int channel = 0; channel < 2; ++channel)
        for (int sample = 0; sample < samples; ++sample) result.setSample(channel, sample, foreign);
    juce::MidiBuffer midi; midi.ensureSize(32768);
    for (int start = 0; start < samples; start += blockSize)
    {
        const int count = std::min(blockSize, samples - start);
        juce::AudioBuffer<float> block(result.getArrayOfWritePointers(), 2, start, count);
        midi.clear(); midi.addEvents(input, start, count, -start);
        synth.setHostInfo({123, static_cast<double>(start) * 123 / (60 * rate), true, rate});
        synth.process(block, midi);
    }
    return result;
}
double maxDifference(const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b)
{
    REQUIRE(a.getNumSamples() == b.getNumSamples());
    double result = 0;
    for (int channel = 0; channel < 2; ++channel)
        for (int sample = 0; sample < a.getNumSamples(); ++sample)
            result = std::max(result, static_cast<double>(std::abs(a.getSample(channel, sample) - b.getSample(channel, sample))));
    return result;
}
double energy(const juce::AudioBuffer<float>& audio, int start, int end)
{
    double result = 0;
    for (int sample = start; sample < end; ++sample) result += static_cast<double>(audio.getSample(0, sample)) * audio.getSample(0, sample);
    return result / (end - start);
}
}

TEST_CASE("wavetable effects integration: base IDs remain stable and module controls survive graph reorder by identity")
{
    W initial, configured;
    CHECK(initial.parameters().size() == W::ParameterCount); CHECK(W::ParameterCount >= 609); CHECK(W::coarseParam(2) == 608);
    E::Configuration graph;
    auto distortion = effect(70, E::Type::Distortion), utility = effect(3, E::Type::Utility);
    graph.buses[0] = {distortion, utility};
    configure(configured, graph);
    REQUIRE(configured.effects() != nullptr);
    for (int parameter = 0; parameter < W::ParameterCount; ++parameter)
        CHECK(configured.parameters()[static_cast<size_t>(parameter)].id == initial.parameters()[static_cast<size_t>(parameter)].id);
    const int drive = configured.fxParameterIndex(70, E::parameterIndex(E::Type::Distortion, "Drive"));
    const int enabled = configured.fxEnabledParameterIndex(70);
    REQUIRE(drive >= W::ParameterCount); REQUIRE(enabled >= W::ParameterCount);
    CHECK(configured.parameters()[static_cast<size_t>(drive)].id == "Fx.70/Drive");
    configured.setParameter(drive, 19); configured.setParameter(enabled, 0);
    CHECK_NEAR(configured.effects()->getParameter(70, E::parameterIndex(E::Type::Distortion, "Drive")), 19, 0);
    auto snapshot = configured.effectsConfiguration();
    CHECK(! snapshot.buses[0][0].enabled);
    std::reverse(snapshot.buses[0].begin(), snapshot.buses[0].end());
    configure(configured, snapshot);
    CHECK(configured.fxParameterIndex(70, E::parameterIndex(E::Type::Distortion, "Drive")) == drive);
    CHECK(configured.fxEnabledParameterIndex(70) == enabled);
    CHECK_NEAR(configured.getParameter(drive), 19, 0);
    configured.setParameter(drive, std::numeric_limits<float>::quiet_NaN()); CHECK_NEAR(configured.getParameter(drive), 19, 0);
    configured.setParameter(drive, 10000); CHECK_NEAR(configured.getParameter(drive), E::parameterDescriptors(E::Type::Distortion)[2].maximum, 0);
    CHECK(configured.fxParameterIndex(999, 0) == -1); CHECK(configured.fxParameterIndex(70, 999) == -1);
    std::set<std::string> ids;
    for (const auto& parameter : configured.parameters()) CHECK(ids.insert(parameter.id).second);
}

TEST_CASE("wavetable effects integration: configured empty graph matches existing synth and never processes foreign input")
{
    W legacy, configured;
    isolated(legacy); configure(configured, {}); isolated(configured);
    CHECK(configured.parameters().size() == W::ParameterCount + W::FxRoutingParams);
    const auto input = events();
    const auto reference = render(legacy, input);
    const auto actual = render(configured, input, 8192, 333, 0.3f);
    for (int channel = 0; channel < 2; ++channel)
        for (int sample = 0; sample < actual.getNumSamples(); ++sample)
            CHECK_NEAR(actual.getSample(channel, sample), reference.getSample(channel, sample) + 0.3f, 2e-6);
    W silence;
    E::Configuration graph;
    auto distortion = effect(10, E::Type::Distortion);
    set(distortion, "Drive", 36); set(distortion, "Shape", 4); set(distortion, "Bias", 0.75f);
    graph.buses[0].push_back(distortion);
    configure(silence, graph); isolated(silence);
    const auto untouched = render(silence, {}, 8192, 1024, 0.23f);
    for (int channel = 0; channel < 2; ++channel)
        for (int sample = 0; sample < untouched.getNumSamples(); ++sample) CHECK_NEAR(untouched.getSample(channel, sample), 0.23f, 0);
}

TEST_CASE("wavetable effects integration: base-class model restore loads dynamic controls and rejected configuration retains the graph")
{
    W synth;
    E::Configuration graph;
    auto distortion = effect(42, E::Type::Distortion); set(distortion, "Drive", 9);
    graph.buses[0].push_back(distortion); graph.routing.mainToBus1 = 0.3f;
    configure(synth, graph);
    model::Device model;
    for (const auto& [path, value] : std::array<std::pair<const char*, double>, 3>{
             std::pair{"Fx.42/Drive", 21.0}, {"Fx.42/On", 0.0}, {"FxRouting/Bus2ToMain", 0.7}})
    {
        model::Parameter parameter;
        parameter.path = path; parameter.value = value;
        model.parameters.push_back(std::move(parameter));
    }
    devices::NativeDevice* base = &synth;
    base->loadFromModel(model);
    CHECK_NEAR(synth.getParameter(synth.fxParameterIndex(42, E::parameterIndex(E::Type::Distortion, "Drive"))), 21, 0);
    CHECK_NEAR(synth.getParameter(synth.fxEnabledParameterIndex(42)), 0, 0);
    CHECK_NEAR(synth.getParameter(synth.fxRoutingParameterIndex(W::Bus2ToMain)), 0.7f, 1e-7);
    CHECK_NEAR(synth.getParameter(synth.fxRoutingParameterIndex(W::MainToBus1)), 0.3f, 0);
    auto invalid = graph; invalid.buses[0].push_back(distortion);
    std::string error;
    const auto* previous = synth.effects();
    CHECK(! synth.setEffectsConfiguration(invalid, error)); CHECK(! error.empty());
    CHECK(synth.effects() == previous);
    CHECK_NEAR(synth.getParameter(synth.fxParameterIndex(42, E::parameterIndex(E::Type::Distortion, "Drive"))), 21, 0);
    synth.prepare(96000, 1);
    CHECK(synth.effectsPreparationError().empty());
    CHECK_NEAR(synth.getParameter(synth.fxParameterIndex(42, E::parameterIndex(E::Type::Distortion, "Drive"))), 21, 0);
}

TEST_CASE("wavetable effects integration: master gain follows distortion and compressor while voice modulation precedes effects")
{
    for (auto type : {E::Type::Distortion, E::Type::Compressor})
    {
        E::Configuration graph;
        auto module = effect(1, type);
        if (type == E::Type::Distortion) { set(module, "Drive", 24); set(module, "Tone", 20000); }
        else { set(module, "Threshold", -30); set(module, "Ratio", 10); set(module, "Attack", 0.001f); }
        graph.buses[0].push_back(module);
        W high, low;
        configure(high, graph); configure(low, graph); isolated(high); isolated(low);
        high.setParameter(W::Volume, 0); low.setParameter(W::Volume, -12);
        const auto a = render(high, events(17, -1)), b = render(low, events(17, -1));
        const float gain = dsp::dbToGain(-12);
        for (int channel = 0; channel < 2; ++channel)
            for (int sample = 0; sample < a.getNumSamples(); ++sample) CHECK_NEAR(b.getSample(channel, sample), a.getSample(channel, sample) * gain, 2e-6);
        CHECK(energy(a, 1024, 8192) > 1e-5);
    }
}

TEST_CASE("wavetable effects integration: per-voice amplitude modulation drives the nonlinear stage before master output")
{
    E::Configuration graph;
    auto distortion = effect(33, E::Type::Distortion); set(distortion, "Drive", 18); set(distortion, "Tone", 20000);
    graph.buses[0].push_back(distortion);
    W modulated, master;
    configure(modulated, graph); configure(master, graph); isolated(modulated); isolated(master);
    modulated.setParameter(W::macroParam(0), 1);
    modulated.setParameter(W::modParam(0, W::ModSource), W::Macro1);
    modulated.setParameter(W::modParam(0, W::ModDestination), W::Amplitude);
    modulated.setParameter(W::modParam(0, W::ModAmount), 0.25f);
    master.setParameter(W::Volume, -12 + dsp::gainToDb(2));
    const auto a = render(modulated, events(17, -1)), b = render(master, events(17, -1));
    CHECK(maxDifference(a, b) > 0.05);
    CHECK(energy(a, 1000, 8192) < energy(b, 1000, 8192));
}

TEST_CASE("wavetable effects integration: own stereo sends and returns have independent dynamic controls")
{
    W reference, routed;
    isolated(reference); configure(routed, {}); isolated(routed);
    const int send = routed.fxRoutingParameterIndex(W::MainToBus1);
    const int returning = routed.fxRoutingParameterIndex(W::Bus1ToMain);
    REQUIRE(send >= W::ParameterCount); REQUIRE(returning >= W::ParameterCount);
    routed.setParameter(send, 0.5f); routed.setParameter(returning, 0.4f);
    CHECK_NEAR(routed.effectsConfiguration().routing.mainToBus1, 0.5f, 0);
    CHECK_NEAR(routed.getParameter(returning), 0.4f, 0);
    const auto a = render(reference, events()), b = render(routed, events());
    for (int channel = 0; channel < 2; ++channel)
        for (int sample = 0; sample < a.getNumSamples(); ++sample) CHECK_NEAR(b.getSample(channel, sample), a.getSample(channel, sample) * 1.2f, 2e-6);
}

TEST_CASE("wavetable effects integration: oversized chunks preserve MIDI offsets control phases tails and nested routing")
{
    E::Configuration graph;
    auto delay = effect(10, E::Type::Delay);
    set(delay, "Time", 0.037f); set(delay, "RightTime", 0.051f); set(delay, "Feedback", 0.45f);
    graph.buses[1].push_back(delay);
    auto split = effect(40, E::Type::Split3);
    auto filter = effect(50, E::Type::Filter); set(filter, "Cutoff", 800);
    split.bands[0].push_back(filter);
    graph.buses[0].push_back(split);
    graph.routing.mainToBus1 = 0.7f;
    W small, oversized;
    configure(small, graph); configure(oversized, graph); isolated(small, 128); isolated(oversized, 128);
    for (auto* synth : {&small, &oversized})
    {
        synth->setParameter(W::oscParam(0, W::Unison), 7);
        synth->setParameter(W::NoiseOn, 1); synth->setParameter(W::NoiseLevel, 0.05f);
        synth->setParameter(W::lfoParam(0, W::SyncOn), 1); synth->setParameter(W::lfoParam(0, W::Mode), W::Global);
        synth->setParameter(W::modParam(0, W::ModSource), W::Lfo1);
        synth->setParameter(W::modParam(0, W::ModDestination), W::Osc1Position);
        synth->setParameter(W::modParam(0, W::ModAmount), 0.2f);
    }
    auto midi = events(17, 3377);
    midi.addEvent(juce::MidiMessage::controllerEvent(1, 1, 77), 247);
    midi.addEvent(juce::MidiMessage::pitchWheel(1, 10500), 1123);
    midi.addEvent(juce::MidiMessage::noteOn(2, 67, static_cast<juce::uint8>(90)), 1779);
    midi.addEvent(juce::MidiMessage::noteOff(2, 67), 2999);
    const auto a = render(small, midi, 8192, 97), b = render(oversized, midi, 8192, 4096);
    CHECK(maxDifference(a, b) < 2e-5);
    CHECK(energy(a, 3400, 6000) > 1e-9);
}

TEST_CASE("wavetable effects integration: convolution latency aligns dry wet sends and direct sub before post gain")
{
    const auto impulse = devices::ConvolutionIR::fromSamples(std::array<float, 1>{1}, 1, rate);
    REQUIRE(impulse);
    for (bool subDirect : {false, true})
    {
        W reference, convolved;
        E::Configuration graph;
        auto convolution = effect(5, E::Type::Convolution); convolution.impulse = impulse.data;
        graph.buses[0].push_back(convolution);
        configure(convolved, graph); isolated(reference); isolated(convolved);
        CHECK(convolved.latencySamples() == E::convolutionPartition);
        if (subDirect)
            for (auto* synth : {&reference, &convolved})
            {
                synth->setParameter(W::oscParam(0, W::On), 0); synth->setParameter(W::SubOn, 1);
                synth->setParameter(W::SubLevel, 0.8f); synth->setParameter(W::SubDirect, 1);
            }
        const auto a = render(reference, events()), b = render(convolved, events(), 8192, 4000, 0.2f);
        for (int channel = 0; channel < 2; ++channel)
            for (int sample = 0; sample < a.getNumSamples(); ++sample)
                CHECK_NEAR(b.getSample(channel, sample), 0.2f + (sample < 128 ? 0 : a.getSample(channel, sample - 128)), 2e-6);
    }
}

TEST_CASE("wavetable effects integration: released notes retain effects tails while reset and bypass clear them")
{
    E::Configuration graph;
    auto delay = effect(7, E::Type::Delay);
    set(delay, "Time", 0.005f); set(delay, "RightTime", 0.005f); set(delay, "Feedback", 0.7f);
    graph.buses[0].push_back(delay);
    W synth; configure(synth, graph); isolated(synth);
    const auto tail = render(synth, events(17, 300), 2048);
    CHECK(synth.activeVoiceCount() == 0); CHECK(energy(tail, 320, 1000) > 1e-7);
    synth.reset();
    const auto clear = render(synth, {}, 2048, 2048, 0.1f);
    for (int channel = 0; channel < 2; ++channel)
        for (int sample = 0; sample < clear.getNumSamples(); ++sample) CHECK_NEAR(clear.getSample(channel, sample), 0.1f, 0);
    const auto before = render(synth, events(17, 300), 512); CHECK(energy(before, 240, 512) > 1e-7);
    synth.setBypassed(true);
    const auto bypassed = render(synth, {}, 1024, 1024, 0.15f);
    CHECK(synth.activeVoiceCount() == 0);
    synth.setBypassed(false);
    const auto resumed = render(synth, {}, 2048, 2048, 0.15f);
    for (int channel = 0; channel < 2; ++channel)
    {
        for (int sample = 0; sample < bypassed.getNumSamples(); ++sample) CHECK_NEAR(bypassed.getSample(channel, sample), 0.15f, 0);
        for (int sample = 0; sample < resumed.getNumSamples(); ++sample) CHECK_NEAR(resumed.getSample(channel, sample), 0.15f, 0);
    }
}

TEST_CASE("wavetable effects integration: system reset and last-channel sound-off clear shared tails at exact MIDI offsets")
{
    E::Configuration graph;
    auto delay = effect(19, E::Type::Delay);
    set(delay, "Time", 0.003f); set(delay, "RightTime", 0.003f); set(delay, "Feedback", 0.75f);
    graph.buses[0].push_back(delay);
    for (bool systemReset : {false, true})
    {
        W synth; configure(synth, graph); isolated(synth, 97);
        auto midi = events(17, -1);
        if (systemReset)
        {
            constexpr uint8_t reset = 0xff;
            midi.addEvent(&reset, 1, 333);
        }
        else midi.addEvent(juce::MidiMessage::controllerEvent(1, 120, 0), 333);
        const auto audio = render(synth, midi, 2048, 2048, 0.125f);
        CHECK(synth.activeVoiceCount() == 0);
        CHECK(energy(audio, 150, 330) > 1e-3);
        for (int channel = 0; channel < 2; ++channel)
            for (int sample = 333; sample < audio.getNumSamples(); ++sample) CHECK_NEAR(audio.getSample(channel, sample), 0.125f, 0);
    }
}

TEST_CASE("wavetable effects integration: imported FX routes follow the measured law table and the focused voice")
{
    W synth;
    isolated(synth);
    E::Configuration graph;
    graph.buses[0] = {effect(5, E::Type::Utility)};
    configure(synth, graph);
    const int width = E::parameterIndex(E::Type::Utility, "Width");
    REQUIRE(width >= 0);
    std::vector<float> ramp(1025);
    for (size_t i = 0; i < ramp.size(); ++i) ramp[i] = static_cast<float>(i) / 1024; // the table is the position itself
    const int target = synth.addImportedFxTarget(5, 0.2f, {{width, ramp}});
    REQUIRE(target == 0);
    CHECK(synth.addImportedFxTarget(5, 0.2f, {{width, {1.0f}}}) == -1); // tables need two points
    CHECK(! synth.setImportedFxSlot(0, 3)); REQUIRE(synth.setImportedFxSlot(0, target)); REQUIRE(synth.setImportedFxSlot(1, target));
    CHECK(synth.setImportedModulationDomain(W::FxParameter, W::ImportedModulationDomain::Unit));
    // Macro route: position = base + amount x source, clamped; written once per FX block.
    synth.setParameter(W::macroParam(0), 0.3f);
    synth.setParameter(W::modParam(0, W::ModSource), W::Macro1);
    synth.setParameter(W::modParam(0, W::ModDestination), W::FxParameter);
    synth.setParameter(W::modParam(0, W::ModAmount), 0.5f);
    render(synth, {}, 512);
    CHECK_NEAR(synth.effects()->getParameter(5, width), 0.35f, 1e-5);
    synth.setParameter(W::macroParam(0), 1.0f); synth.setParameter(W::modParam(0, W::ModAmount), 1.0f);
    render(synth, {}, 512);
    CHECK_NEAR(synth.effects()->getParameter(5, width), 1.0f, 1e-6);
    synth.setParameter(W::modParam(0, W::ModAmount), 0.0f);
    // Envelope 2 route: one focused voice drives the target; a note that loses the focus is frozen until it regains it.
    synth.setParameter(W::envParam(1, W::Attack), 0.5f); synth.setParameter(W::envParam(1, W::Decay), 0);
    synth.setParameter(W::envParam(1, W::Sustain), 1); synth.setParameter(W::envParam(1, W::Release), 0.2f);
    synth.setParameter(W::envParam(0, W::Release), 1.0f);
    synth.setParameter(W::modParam(1, W::ModSource), W::Env2);
    synth.setParameter(W::modParam(1, W::ModDestination), W::FxParameter);
    synth.setParameter(W::modParam(1, W::ModAmount), 0.5f);
    const auto block = [&](const std::vector<juce::MidiMessage>& messages) {
        juce::AudioBuffer<float> audio(2, 512); audio.clear();
        juce::MidiBuffer midi; for (const auto& message : messages) midi.addEvent(message, 0);
        synth.process(audio, midi);
        return synth.effects()->getParameter(5, width) - 0.2f;
    };
    float level = block({juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100))});
    CHECK(level > 0 && level < 0.002f); // written for the block's last 16-sample sub-block: the attack has barely started
    for (int i = 0; i < 20; ++i) level = block({});
    CHECK(level > 0.05f && level < 0.45f);
    const float first = block({}), second = block({}); // A runs one more block after `second` before B takes the focus
    CHECK(block({juce::MidiMessage::noteOn(1, 64, static_cast<juce::uint8>(100))}) < 0.01f); // the newer note starts fresh
    for (int i = 0; i < 5; ++i) block({});
    CHECK_NEAR(block({juce::MidiMessage::noteOff(1, 64)}), second + (second - first), 1e-3); // the older held note resumes where it stopped
    const float before = block({juce::MidiMessage::noteOff(1, 60)});
    const float during = block({});
    CHECK(during > 0.02f && during < before); // the focused note releases with its own envelope
    for (int i = 0; i < 40; ++i) level = block({});
    CHECK_NEAR(level, 0.0f, 1e-6);
}
