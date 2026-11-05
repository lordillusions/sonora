// COL-4c: Sonora's own VST3s that are not installed here (a collab guest, a set from another computer) play through the
// built-in Prism / Curve LFO from their saved state, and saving leaves that state as it was.
#include "TestFramework.h"
#include "devices/DeviceFactory.h"
#include "devices/native/CurveLfo.h"
#include "devices/native/WavetableSynth.h"
#include "plugins/PrismPreset.h"
#include "plugins/SonoraPluginState.h"

#include <juce_data_structures/juce_data_structures.h>

using namespace sonora;
namespace
{
using W = devices::WavetableSynth;
using L = devices::CurveLfo;
constexpr uint32_t kPrism = 0x536E5072, kCurveLfo = 0x536E436C, kHush = 0x536E4873; // 'SnPr', 'SnCl', 'SnHs'
constexpr uint32_t kWavetable = 0x536E5774; // 'SnWt': Prism's VST3 before UI-49

// What JUCE's VST3 wrapper saves: the plug-in's bytes, int64 0, the private data, its int64 size, "JUCEPrivateData".
std::vector<uint8_t> wrapLikeJuceVst3(std::vector<uint8_t> body, bool bypassed)
{
    juce::MemoryOutputStream extra;
    extra.writeInt64(0);
    juce::ValueTree privateData("JUCEPrivateData");
    privateData.setProperty("Bypass", bypassed, nullptr);
    privateData.writeToStream(extra);
    extra.writeInt64((juce::int64) (extra.getDataSize() - sizeof(juce::int64)));
    extra << "JUCEPrivateData";
    const auto* bytes = static_cast<const uint8_t*>(extra.getData());
    body.insert(body.end(), bytes, bytes + extra.getDataSize());
    return body;
}

model::Device sonoraVst3(const char* name, uint32_t code, std::vector<uint8_t> state)
{
    model::Device d;
    d.id = 7;
    d.kind = model::DeviceKind::PluginVst3;
    d.type = "PluginDevice";
    d.displayName = name;
    model::PluginInfo info;
    info.name = name;
    info.vendor = "Sonora";
    info.vst3Uid = {0xABCDEF01u, 0x9182FAEBu, 0x536E7261u, code}; // as Live stores it (Fields.0..3)
    info.isInstrument = code == kPrism;
    info.state = std::move(state);
    d.plugin = std::move(info);
    return d;
}

bool logHas(const std::vector<std::string>& log, const std::string& text)
{
    return std::any_of(log.begin(), log.end(), [&](const std::string& line) { return line.find(text) != std::string::npos; });
}
} // namespace

TEST_CASE("sonora stand-in: Sonora VST3s are recognised by class id, and JUCE's trailer is removed")
{
    using plugins::SonoraPlugin;
    const auto kind = model::DeviceKind::PluginVst3;
    CHECK(plugins::sonoraPluginFor(*sonoraVst3("Sonora Prism", kPrism, {}).plugin, kind) == SonoraPlugin::Prism);
    CHECK(plugins::sonoraPluginFor(*sonoraVst3("Sonora Curve LFO", kCurveLfo, {}).plugin, kind) == SonoraPlugin::CurveLfo);
    CHECK(plugins::sonoraPluginFor(*sonoraVst3("Sonora Hush", kHush, {}).plugin, kind) == SonoraPlugin::Hush);
    CHECK(plugins::sonoraPluginFor(*sonoraVst3("Sonora Prism", kPrism, {}).plugin, model::DeviceKind::PluginVst2) == SonoraPlugin::None);
    auto other = *sonoraVst3("Sonora Prism", kPrism, {}).plugin;
    other.vst3Uid[2] = 0x4D616E75; // another JUCE manufacturer with the same plug-in code
    CHECK(plugins::sonoraPluginFor(other, kind) == SonoraPlugin::None);

    const std::vector<uint8_t> body{1, 2, 3, 4, 5};
    const auto wrapped = wrapLikeJuceVst3(body, false);
    const auto stripped = plugins::juceVst3StateBody(wrapped);
    CHECK(std::vector<uint8_t>(stripped.begin(), stripped.end()) == body);
    CHECK(plugins::juceVst3StateBody(body).size() == body.size()); // no trailer: as it is
    auto damaged = wrapped;
    damaged[damaged.size() - 16] = 0xFF; // private-data size larger than the state
    CHECK(plugins::juceVst3StateBody(damaged).size() == damaged.size());
}

TEST_CASE("sonora stand-in: a missing Prism VST3 plays its saved sound on the built-in Prism, state untouched on save")
{
    juce::ScopedJuceInitialiser_GUI init;
    auto native = devices::nativeDeviceModel("SonoraWavetable");
    const auto levelId = W().parameters()[(size_t) W::Osc1Level].id;
    bool found = false;
    for (auto& p : native.parameters)
        if (p.path == levelId) { p.value = 0.3; found = true; }
    REQUIRE(found);
    const auto state = wrapLikeJuceVst3(plugins::encodePrismPreset(native), false);
    auto device = sonoraVst3("Sonora Prism", kPrism, state);
    device.enabled = false;

    std::vector<std::string> log;
    devices::BuildContext ctx; // no plug-in host: nothing is installed
    ctx.log = &log;
    auto processor = devices::createDevice(device, ctx);
    auto* synth = dynamic_cast<W*>(processor.get());
    REQUIRE(synth != nullptr);
    CHECK_NEAR(synth->getParameter(W::Osc1Level), 0.3f, 1e-6);
    CHECK(synth->isBypassed());
    CHECK(logHas(log, "played by Sonora's built-in device"));

    devices::captureDeviceState(device, processor.get());
    REQUIRE(device.plugin.has_value());
    CHECK(device.plugin->state == state);
    CHECK(device.nativeContent == nullptr);
    CHECK(device.parameters.empty());

    // The "Sonora Wavetable" build before UI-49/50 saved the same tree under another name, without the PRSM header.
    juce::ValueTree legacy("SonoraWavetableState"), parameters("Parameters"), level("V");
    legacy.setProperty("version", 1, nullptr);
    level.setProperty("id", juce::String::fromUTF8(levelId.c_str()), nullptr);
    level.setProperty("v", 0.25, nullptr);
    parameters.appendChild(level, nullptr);
    legacy.appendChild(parameters, nullptr);
    juce::MemoryOutputStream legacyBytes;
    legacy.writeToStream(legacyBytes);
    const auto* legacyData = static_cast<const uint8_t*>(legacyBytes.getData());
    auto old = sonoraVst3("Sonora Wavetable", kWavetable,
                          wrapLikeJuceVst3({legacyData, legacyData + legacyBytes.getDataSize()}, false));
    auto oldProcessor = devices::createDevice(old, ctx);
    auto* oldSynth = dynamic_cast<W*>(oldProcessor.get());
    REQUIRE(oldSynth != nullptr);
    CHECK_NEAR(oldSynth->getParameter(W::Osc1Level), 0.25f, 1e-6);

    // A state that is not a Prism preset still plays (the default sound) and says why.
    log.clear();
    auto damaged = sonoraVst3("Sonora Prism", kPrism, wrapLikeJuceVst3({'n', 'o', 'p', 'e'}, false));
    auto fallback = devices::createDevice(damaged, ctx);
    CHECK(dynamic_cast<W*>(fallback.get()) != nullptr);
    CHECK(logHas(log, "could not be read"));
}

TEST_CASE("sonora stand-in: a missing Curve LFO VST3 keeps its curves on the built-in device; Hush stays a gap")
{
    juce::ScopedJuceInitialiser_GUI init;
    L source;
    const auto& infos = source.parameters();
    int control = 0; // the first control whose maximum is not its default
    while (control < L::GraphStart && std::abs(infos[(size_t) control].maxValue - infos[(size_t) control].defaultValue) < 1e-3f) ++control;
    REQUIRE(control < L::GraphStart);
    const float value = infos[(size_t) control].maxValue;
    source.setParameter(control, value);
    dsp::CurveGraph graph;
    graph.count = 3;
    graph.x[0] = 0.0f; graph.y[0] = 0.0f;
    graph.x[1] = 0.25f; graph.y[1] = 1.0f;
    graph.x[2] = 1.0f; graph.y[2] = 0.0f;
    source.setGraph(0, graph);

    auto device = sonoraVst3("Sonora Curve LFO", kCurveLfo, wrapLikeJuceVst3(plugins::encodeCurveLfoState(source), false));
    std::vector<std::string> log;
    devices::BuildContext ctx;
    ctx.log = &log;
    auto processor = devices::createDevice(device, ctx);
    auto* lfo = dynamic_cast<L*>(processor.get());
    REQUIRE(lfo != nullptr);
    CHECK_NEAR(lfo->getParameter(control), value, 1e-6);
    CHECK_NEAR(lfo->getParameter(L::countParam(0)), 3.0f, 1e-6);
    CHECK_NEAR(lfo->getParameter(L::pointParam(0, 1, L::X)), 0.25f, 1e-6);
    CHECK(! lfo->isBypassed());

    // No built-in Hush: an effect that is not installed is skipped, as before.
    log.clear();
    CHECK(devices::createDevice(sonoraVst3("Sonora Hush", kHush, {}), ctx) == nullptr);
    CHECK(! logHas(log, "built-in"));
}
