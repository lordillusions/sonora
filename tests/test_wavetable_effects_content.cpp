#include "TestFramework.h"
#include "core/Edit.h"
#include "devices/DeviceFactory.h"
#include "devices/native/WavetableContent.h"
#include "devices/native/WavetableSynth.h"
#include "io/als/AlsNewSet.h"
#include "io/als/AlsReader.h"
#include "io/als/AlsTemplates.h"
#include "io/als/AlsWriter.h"

#include <algorithm>
#include <limits>

using namespace sonora;
namespace
{
using E = devices::WavetableEffects;
using W = devices::WavetableSynth;
E::Module module(E::ModuleId id, E::Type type)
{
    E::Module result; result.id = id; result.type = type;
    for (const auto& descriptor : E::parameterDescriptors(type)) result.values.push_back(descriptor.initial);
    return result;
}
E::Configuration authoredEffects()
{
    E::Configuration result;
    result.routing = {0.25f, 0.5f, 0.75f, 0.625f, 0.875f};
    auto split = module(42, E::Type::Split3);
    split.bands[0].push_back(module(7, E::Type::Compressor));
    split.bands[1].push_back(module(9, E::Type::Eq));
    auto convolution = module(99, E::Type::Convolution);
    const std::array<float, 8> pcm{1, 0.75f, -0.5f, 0.125f, 0.25f, -0.25f, 0, 0.0625f};
    const auto ir = devices::ConvolutionIR::fromSamples(pcm, 2, 32000, "Authored stereo response"); REQUIRE(ir);
    convolution.impulse = ir.data;
    split.bands[2].push_back(convolution);
    result.buses[0] = {module(3, E::Type::Distortion), split};
    result.buses[1] = {module(111, E::Type::Delay)};
    convolution.id = 200; convolution.enabled = false;
    result.buses[2] = {convolution, module(210, E::Type::Utility)};
    result.buses[2][1].values[static_cast<size_t>(E::parameterIndex(E::Type::Utility, "Output"))] = -4;
    return result;
}
void sameChain(const std::vector<E::Module>& actual, const std::vector<E::Module>& expected)
{
    REQUIRE(actual.size() == expected.size());
    for (size_t i = 0; i < actual.size(); ++i)
    {
        const auto& a = actual[i]; const auto& b = expected[i];
        CHECK(a.id == b.id); CHECK(a.type == b.type); CHECK(a.enabled == b.enabled); CHECK(a.values == b.values);
        REQUIRE(static_cast<bool>(a.impulse) == static_cast<bool>(b.impulse));
        if (a.impulse)
        {
            CHECK(a.impulse->name() == b.impulse->name()); CHECK(a.impulse->channels() == b.impulse->channels());
            CHECK_NEAR(a.impulse->sampleRate(), b.impulse->sampleRate(), 0);
            CHECK(a.impulse->originalSamples() == b.impulse->originalSamples());
        }
        for (size_t band = 0; band < a.bands.size(); ++band) sameChain(a.bands[band], b.bands[band]);
    }
}
void sameEffects(const E::Configuration& actual, const E::Configuration& expected)
{
    CHECK(actual.alignLatency == expected.alignLatency);
    CHECK(actual.mixLaw == expected.mixLaw);
    CHECK(actual.returnBeforeMain == expected.returnBeforeMain);
    CHECK_NEAR(actual.routing.mainToBus1, expected.routing.mainToBus1, 0);
    CHECK_NEAR(actual.routing.mainToBus2, expected.routing.mainToBus2, 0);
    CHECK_NEAR(actual.routing.bus1ToMain, expected.routing.bus1ToMain, 0);
    CHECK_NEAR(actual.routing.bus2ToMain, expected.routing.bus2ToMain, 0);
    CHECK_NEAR(actual.routing.mainGain, expected.routing.mainGain, 0);
    CHECK_NEAR(actual.routing.directGain, expected.routing.directGain, 0);
    for (size_t bus = 0; bus < actual.buses.size(); ++bus) sameChain(actual.buses[bus], expected.buses[bus]);
}
juce::var contentValue(const model::Device& device)
{
    REQUIRE(device.nativeContent);
    const auto& bytes = device.nativeContent->data;
    auto value = juce::JSON::parse(juce::String::fromUTF8(reinterpret_cast<const char*>(bytes.data()), static_cast<int>(bytes.size())));
    REQUIRE(value.isObject()); return value;
}
void replaceContent(model::Device& device, const juce::var& value)
{
    auto content = std::make_shared<model::NativeContent>(*device.nativeContent);
    const auto json = juce::JSON::toString(value, true);
    const auto* bytes = reinterpret_cast<const uint8_t*>(json.toRawUTF8());
    content->data.assign(bytes, bytes + json.getNumBytesAsUTF8()); device.nativeContent = std::move(content);
}
model::Device authoredDevice()
{
    auto result = devices::nativeDeviceModel("SonoraWavetable");
    auto origin = std::make_shared<model::NativeContent>(); origin->format = "serum2-preset";
    origin->data = {1, 2, 3, 4}; origin->controllerData = {255, 128, 0}; origin->sourceDirectory = "/tmp/authored-response";
    result.nativeContent = std::move(origin);
    std::string error; REQUIRE(devices::storeWavetableEffects(result, authoredEffects(), error));
    return result;
}
}

TEST_CASE("wavetable effect content: ordered buses nested bands and shared stereo IR restore with exact source retention")
{
    const auto expected = authoredEffects(); auto device = authoredDevice();
    const auto original = device.nativeContent; const auto bytes = original->data;
    const auto root = contentValue(device); const auto fx = root.getProperty("effects", {});
    REQUIRE(fx.getProperty("impulses", {}).getArray()); CHECK(fx.getProperty("impulses", {}).size() == 1);
    CHECK(root.getProperty("origin", {}).getProperty("format", {}).toString() == "serum2-preset");
    juce::MemoryBlock preserved; REQUIRE(preserved.fromBase64Encoding(root.getProperty("origin", {}).getProperty("data", {}).toString()));
    CHECK(preserved == juce::MemoryBlock(std::array<uint8_t, 4>{1, 2, 3, 4}.data(), 4));
    CHECK(device.nativeContent->controllerData == std::vector<uint8_t>({255, 128, 0}));
    E::Configuration decoded; std::string error;
    REQUIRE(devices::readWavetableEffects(device, decoded, error)); CHECK(error.empty()); sameEffects(decoded, expected);
    CHECK(decoded.buses[0][1].bands[2][0].impulse == decoded.buses[2][0].impulse);
    decoded.buses[0].clear(); decoded.alignLatency = false;
    REQUIRE(devices::storeWavetableEffects(device, decoded, error));
    CHECK(device.nativeContent != original); CHECK(original->data == bytes);
    CHECK(device.nativeContent->sourceDirectory == original->sourceDirectory);
    CHECK(device.nativeContent->controllerData == original->controllerData);
}

TEST_CASE("wavetable effect content: surviving controls preserve automation metadata and removed module paths disappear")
{
    auto device = authoredDevice();
    auto found = std::find_if(device.parameters.begin(), device.parameters.end(), [](const auto& p) { return p.path == "Fx.111/Feedback"; });
    REQUIRE(found != device.parameters.end()); found->automationTargetId = 567; found->macroIndex = 3; found->rangeMin = 0.2; found->rangeMax = 0.8;
    auto configuration = authoredEffects();
    configuration.buses[0].erase(configuration.buses[0].begin());
    configuration.buses[1][0].values[static_cast<size_t>(E::parameterIndex(E::Type::Delay, "Feedback"))] = 0.625f;
    std::string error; REQUIRE(devices::storeWavetableEffects(device, configuration, error));
    found = std::find_if(device.parameters.begin(), device.parameters.end(), [](const auto& p) { return p.path == "Fx.111/Feedback"; });
    REQUIRE(found != device.parameters.end()); CHECK_NEAR(found->value, 0.625, 0); CHECK(found->automationTargetId == 567);
    CHECK(found->macroIndex == 3); CHECK_NEAR(found->rangeMin, 0.2, 0); CHECK_NEAR(found->rangeMax, 0.8, 0);
    CHECK(std::none_of(device.parameters.begin(), device.parameters.end(), [](const auto& p) { return p.path.starts_with("Fx.3/"); }));
    const auto root = contentValue(device); root.getDynamicObject()->setProperty("futureMetadata", "retain this note"); replaceContent(device, root);
    const auto fx = root.getProperty("effects", {}); fx.getDynamicObject()->setProperty("futureGraphMetadata", "graph note");
    fx.getProperty("buses", {}).getArray()->getReference(1).getArray()->getReference(0).getDynamicObject()->setProperty("futureModuleMetadata", "delay note");
    replaceContent(device, root);
    REQUIRE(devices::storeWavetableEffects(device, configuration, error));
    CHECK(contentValue(device).getProperty("futureMetadata", {}).toString() == "retain this note");
    const auto retained = contentValue(device).getProperty("effects", {});
    CHECK(retained.getProperty("futureGraphMetadata", {}).toString() == "graph note");
    CHECK(retained.getProperty("buses", {}).getArray()->getReference(1).getArray()->getReference(0).getProperty("futureModuleMetadata", {}).toString() == "delay note");
}

TEST_CASE("wavetable effect content: prepared dynamic controls and live snapshots survive ALS save reload undo redo")
{
    auto device = authoredDevice();
    // A preserved opaque source can be undecodable without preventing the independently stored graph.
    std::vector<std::string> warnings; auto processor = devices::createNative(device, &warnings);
    auto* synth = dynamic_cast<W*>(processor.get()); REQUIRE(synth); REQUIRE(synth->hasEffectsConfiguration());
    synth->prepare(48000, 512); CHECK(synth->effectsPreparationError().empty()); CHECK(synth->latencySamples() == 128);
    const int gain = synth->fxParameterIndex(210, E::parameterIndex(E::Type::Utility, "Output")); REQUIRE(gain >= W::ParameterCount);
    CHECK_NEAR(synth->getParameter(gain), -4, 0);
    synth->setParameter(gain, -7); synth->setParameter(synth->fxEnabledParameterIndex(200), 1);
    synth->setParameter(synth->fxRoutingParameterIndex(W::MainToBus1), 0.375f);
    devices::captureDeviceState(device, synth);
    E::Configuration saved; std::string error; REQUIRE(devices::readWavetableEffects(device, saved, error));
    CHECK_NEAR(saved.routing.mainToBus1, 0.375, 0); CHECK(saved.buses[2][0].enabled);
    CHECK_NEAR(saved.buses[2][1].values[static_cast<size_t>(E::parameterIndex(E::Type::Utility, "Output"))], -7, 0);
    const juce::File directory("/tmp/sonora_tests/wavetable_effect_content"); REQUIRE(directory.createDirectory().wasOk());
    io::NewSetOptions options; options.midiTracks = 1; options.audioTracks = 0; options.returnTracks = 0;
    const auto source = directory.getChildFile("Source.als"), destination = directory.getChildFile("Effects.als");
    REQUIRE(io::writeAlsDom(*io::createNewSetDom(options), source, error));
    auto loaded = io::loadAls(source); REQUIRE(loaded); auto& song = *loaded.song;
    auto initial = devices::nativeDeviceModel("SonoraWavetable"); model::allocateDeviceIds(song, initial);
    initial.sourceXml = std::make_shared<const std::string>(io::deviceTemplate(initial));
    model::EditHistory history; history.perform(song, model::insertDevice(0, initial)); history.clear();
    device.id = initial.id; device.sourceXml = initial.sourceXml;
    history.perform(song, model::replaceDevice(song, 0, 0, device, "Authored effects"));
    const auto content = song.tracks[0].devices[0].nativeContent;
    REQUIRE(io::saveAls(song, destination, io::AlsSaveOptions{}).ok);
    auto restored = io::loadAls(destination); REQUIRE(restored); REQUIRE(restored.song->tracks[0].devices.size() == 1);
    const auto& restoredDevice = restored.song->tracks[0].devices[0]; CHECK(restoredDevice.nativeContent->data == content->data);
    E::Configuration roundtrip; REQUIRE(devices::readWavetableEffects(restoredDevice, roundtrip, error)); sameEffects(roundtrip, saved);
    auto rebuilt = devices::createNative(restoredDevice); auto* restoredSynth = dynamic_cast<W*>(rebuilt.get()); REQUIRE(restoredSynth);
    CHECK_NEAR(restoredSynth->getParameter(restoredSynth->fxParameterIndex(210, E::parameterIndex(E::Type::Utility, "Output"))), -7, 0);
    REQUIRE(history.undo(song)); CHECK(! song.tracks[0].devices[0].nativeContent);
    REQUIRE(history.redo(song)); CHECK(song.tracks[0].devices[0].nativeContent == content);
}

TEST_CASE("wavetable effect content: invalid edits are transactional and corrupt graph fields cannot publish effects")
{
    auto device = authoredDevice(); const auto original = device.nativeContent;
    std::string error;
    const auto reject = [&](const E::Configuration& configuration) {
        CHECK(! devices::storeWavetableEffects(device, configuration, error)); CHECK(! error.empty()); CHECK(device.nativeContent == original);
    };
    auto configuration = authoredEffects(); configuration.buses[1][0].id = 42; reject(configuration);
    configuration = authoredEffects(); configuration.routing.mainGain = std::numeric_limits<float>::quiet_NaN(); reject(configuration);
    configuration = authoredEffects(); configuration.buses[0][0].type = E::Type::Unknown; reject(configuration);
    configuration = authoredEffects(); configuration.buses[0][0].values.push_back(0); reject(configuration);
    configuration = authoredEffects(); configuration.buses[0][0].values[0] = 2; reject(configuration);
    configuration = authoredEffects(); configuration.buses[0][0].bands[0].push_back(module(900, E::Type::Utility)); reject(configuration);
    configuration = authoredEffects(); configuration.buses[1][0].impulse = configuration.buses[2][0].impulse; reject(configuration);
    for (const auto& corrupt : std::vector<std::function<void(juce::var&)>>{
             [](auto& fx) { fx.getDynamicObject()->setProperty("version", 2); },
             [](auto& fx) { fx.getDynamicObject()->setProperty("routing", "invalid"); },
             [](auto& fx) { fx.getProperty("buses", {}).getArray()->getReference(1).getArray()->getReference(0).getDynamicObject()->setProperty("id", 42); },
             [](auto& fx) { fx.getProperty("buses", {}).getArray()->getReference(2).getArray()->getReference(0).getDynamicObject()->setProperty("impulse", 9); },
             [](auto& fx) { fx.getProperty("impulses", {}).getArray()->getReference(0).getDynamicObject()->setProperty("channels", 3); },
             [](auto& fx) { fx.getProperty("impulses", {}).getArray()->getReference(0).getDynamicObject()->setProperty("pcm", "bad-pcm"); }})
    {
        auto invalid = device; auto value = contentValue(invalid); auto fx = value.getProperty("effects", {}); corrupt(fx); replaceContent(invalid, value);
        E::Configuration decoded = authoredEffects(); CHECK(! devices::readWavetableEffects(invalid, decoded, error));
        sameEffects(decoded, authoredEffects());
        W synth; std::vector<std::string> warnings; devices::prepareWavetableContent(synth, invalid, warnings);
        CHECK(! warnings.empty()); CHECK(! synth.hasEffectsConfiguration());
    }
}

TEST_CASE("wavetable effect content: clearing every module preserves an explicit empty graph across save capture")
{
    auto device = authoredDevice(); E::Configuration empty; empty.routing.mainGain = 0.75f;
    std::string error; REQUIRE(devices::storeWavetableEffects(device, empty, error));
    CHECK(std::none_of(device.parameters.begin(), device.parameters.end(), [](const auto& p) { return p.path.starts_with("Fx."); }));
    auto processor = devices::createNative(device); auto* synth = dynamic_cast<W*>(processor.get()); REQUIRE(synth);
    CHECK(synth->hasEffectsConfiguration()); CHECK(synth->effects()->moduleIds().empty());
    devices::captureDeviceState(device, synth); E::Configuration restored; REQUIRE(devices::readWavetableEffects(device, restored, error));
    sameEffects(restored, empty);
}

TEST_CASE("wavetable effect content: explicit shaped profile survives persistence and missing profile preserves legacy behavior")
{
    auto device = authoredDevice(); auto configuration = authoredEffects(); configuration.mixLaw = E::MixLaw::Shaped;
    std::string error; REQUIRE(devices::storeWavetableEffects(device, configuration, error));
    E::Configuration restored; REQUIRE(devices::readWavetableEffects(device, restored, error)); CHECK(restored.mixLaw == E::MixLaw::Shaped);
    auto value = contentValue(device); auto effects = value.getProperty("effects", {}); effects.getDynamicObject()->removeProperty("mixLaw");
    replaceContent(device, value); REQUIRE(devices::readWavetableEffects(device, restored, error)); CHECK(restored.mixLaw == E::MixLaw::EqualPower);
    effects.getDynamicObject()->setProperty("mixLaw", "future unimplemented mix"); replaceContent(device, value);
    CHECK(! devices::readWavetableEffects(device, restored, error));
}

TEST_CASE("wavetable effect content: serial return placement and full Direct gain survive capture while old graphs retain defaults")
{
    auto device = authoredDevice(); auto configuration = authoredEffects();
    configuration.returnBeforeMain = {true, false};
    configuration.routing.directGain = 3.25f; configuration.routing.bus1ToMain = 3.5f;
    std::string error; REQUIRE(devices::storeWavetableEffects(device, configuration, error));
    E::Configuration restored; REQUIRE(devices::readWavetableEffects(device, restored, error)); sameEffects(restored, configuration);
    auto processor = devices::createNative(device); auto* synth = dynamic_cast<W*>(processor.get()); REQUIRE(synth);
    CHECK(synth->effectsConfiguration().returnBeforeMain == configuration.returnBeforeMain);
    CHECK_NEAR(synth->getParameter(W::DirectGain), 3.25, 0);
    synth->setParameter(W::DirectGain, 4); devices::captureDeviceState(device, synth);
    REQUIRE(devices::readWavetableEffects(device, restored, error));
    CHECK_NEAR(restored.routing.directGain, 4, 0); CHECK(restored.returnBeforeMain == configuration.returnBeforeMain);
    auto legacy = contentValue(device); auto effects = legacy.getProperty("effects", {});
    effects.getDynamicObject()->removeProperty("returnBeforeMain"); effects.getProperty("routing", {}).getArray()->removeLast();
    replaceContent(device, legacy); REQUIRE(devices::readWavetableEffects(device, restored, error));
    CHECK((restored.returnBeforeMain == std::array<bool, 2>({false, false}))); CHECK_NEAR(restored.routing.directGain, 1, 0);
    CHECK_NEAR(restored.routing.bus1ToMain, 3.5, 0);
}

TEST_CASE("wavetable effect content: malformed return placement and Direct gain reject without changing the prepared graph")
{
    const auto original = authoredEffects(); auto device = authoredDevice(); std::string error;
    for (const auto& flags : std::vector<juce::var>{"Main", juce::Array<juce::var>{true}, juce::Array<juce::var>{true, 0}, juce::Array<juce::var>{true, false, true}})
    {
        auto invalid = device; auto root = contentValue(invalid);
        root.getProperty("effects", {}).getDynamicObject()->setProperty("returnBeforeMain", flags); replaceContent(invalid, root);
        auto decoded = original; CHECK(! devices::readWavetableEffects(invalid, decoded, error)); sameEffects(decoded, original);
    }
    auto invalid = device; auto root = contentValue(invalid);
    root.getProperty("effects", {}).getProperty("routing", {}).getArray()->getReference(5) = 4.01; replaceContent(invalid, root);
    auto decoded = original; CHECK(! devices::readWavetableEffects(invalid, decoded, error)); sameEffects(decoded, original);
    auto tooHigh = original; tooHigh.routing.directGain = 4.01f; const auto retained = device.nativeContent;
    CHECK(! devices::storeWavetableEffects(device, tooHigh, error)); CHECK(device.nativeContent == retained);
}
