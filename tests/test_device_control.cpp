#include "TestFramework.h"
#include "control/DeviceControl.h"
#include "core/DeviceCatalog.h"
#include "devices/DeviceFactory.h"
#include "devices/RackProcessor.h"
#include "io/als/AlsNewSet.h"
#include "io/als/AlsReader.h"
#include "io/als/AlsWriter.h"
#include "midi/MidiManager.h"
#include "plugins/PluginHost.h"
#include "agent/AgentProtocol.h"

using namespace sonora;
namespace
{
using control::object;
struct DeviceHarness
{
    juce::ScopedJuceInitialiser_GUI gui;
    juce::File folder = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("device-control-" + juce::Uuid().toString());
    std::unique_ptr<model::Song> song;
    plugins::PluginHost host;
    midi::MidiManager midi;
    engine::AudioEngine engine{host, midi};
    model::EditHistory history;
    std::unique_ptr<control::ControlService> service;
    std::string document = "device-control-test", busy;
    int key = 0;
    DeviceHarness()
    {
        folder.createDirectory(); std::string error; const auto file = folder.getChildFile("Source.als");
        io::writeAlsDom(*io::createNewSetDom(), file, error); song = std::move(io::loadAls(file).song); engine.loadSong(*song);
        control::Backend backend;
        backend.state = [this] { control::State s; s.song = song.get(); s.documentId = document; s.revision = std::to_string(history.version()); s.busy = busy; return s; };
        backend.apply = [this](auto command) { history.perform(*song, std::move(command)); engine.loadSong(*song); return control::ApplyResult::success(); };
        backend.undo = [this] { history.undo(*song); engine.loadSong(*song); return control::ApplyResult::success(); };
        control::configureDeviceBackend(backend, host, engine, [this] { return song.get(); });
        service = std::make_unique<control::ControlService>(std::move(backend));
    }
    ~DeviceHarness() { service.reset(); folder.deleteRecursively(); }
    juce::var call(const char* operation, juce::var args)
    {
        return service->execute(object({{"apiVersion", "1.0"}, {"operation", operation}, {"arguments", args},
            {"target", object({{"instanceId", service->instanceId()}, {"documentId", document}})},
            {"expectedRevision", std::to_string(history.version())}, {"idempotencyKey", "device-test-" + juce::String(++key)}}));
    }
    juce::var insert(const std::string& type, int track = 0)
    { return call("devices.insert", object({{"trackId", song->tracks[static_cast<size_t>(track)].id}, {"catalogId", "native:" + type}})); }
    juce::var parameters(int id)
    { return call("devices.parameters", object({{"trackId", song->tracks[0].id}, {"deviceId", id}})); }
    juce::var set(int id, const juce::var& writes)
    { return call("devices.parameters.set", object({{"trackId", song->tracks[0].id}, {"deviceId", id}, {"parameters", writes}})); }
    juce::var undo(const juce::var& receipt)
    { return call("history.undo", object({{"undoToken", receipt["data"]["undoToken"]}})); }
    devices::DeviceProcessor* processor(int id)
    {
        for (size_t i = 0; i < song->tracks[0].devices.size(); ++i)
            if (song->tracks[0].devices[i].id == id) return engine.latestGraph()->nodes[0].processorForModelDevice[i];
        return nullptr;
    }
};
juce::var write(const char* id, const char* name, double value)
{ return juce::Array<juce::var>{object({{"parameterId", id}, {"name", name}, {"value", value}})}; }
float steadyGain(devices::DeviceProcessor& processor)
{
    juce::AudioBuffer<float> buffer(2, 512); juce::MidiBuffer midi;
    for (int i = 0; i < 20; ++i)
    {
        for (int c = 0; c < 2; ++c) std::fill_n(buffer.getWritePointer(c), 512, 1.0f);
        processor.process(buffer, midi);
    }
    return buffer.getSample(0, 511);
}
}
TEST_CASE("control: every implemented native catalogue device exposes editable values that persist")
{
    DeviceHarness h;
    const auto catalog = h.call("devices.catalog", object({{"kind", "native"}, {"limit", 128}})); REQUIRE(static_cast<bool>(catalog["ok"]));
    int implemented = 0; for (const auto& entry : model::deviceCatalog()) if (entry.implemented) ++implemented;
    CHECK(catalog["data"]["total"].operator int() == implemented);
    const auto page = h.call("devices.catalog", object({{"kind", "native"}, {"offset", 1}, {"limit", 2}}));
    CHECK(page["data"]["devices"].size() == 2); CHECK(static_cast<bool>(page["data"]["hasMore"]));
    for (const auto& entry : *catalog["data"]["devices"].getArray())
    {
        h.song->tracks[0].devices.clear(); h.engine.loadSong(*h.song);
        const auto inserted = h.call("devices.insert", object({{"trackId", h.song->tracks[0].id}, {"catalogId", entry["catalogId"]}}));
        if (! static_cast<bool>(inserted["ok"])) std::printf("Native insertion failed: %s %s\n", entry["name"].toString().toRawUTF8(), juce::JSON::toString(inserted).toRawUTF8());
        REQUIRE(static_cast<bool>(inserted["ok"]));
        const int id = inserted["data"]["deviceId"]; const auto values = h.parameters(id); REQUIRE(values["data"].size() > 0);
        const auto parameter = values["data"][0];
        const double value = static_cast<double>(parameter["minimum"]); // exact valid grid endpoint, including toggles/enums
        const auto changed = h.set(id, juce::Array<juce::var>{object({{"parameterId", parameter["parameterId"]}, {"name", parameter["name"]}, {"value", value}})});
        if (! static_cast<bool>(changed["ok"])) std::printf("Native write failed: %s %s\n", entry["name"].toString().toRawUTF8(), juce::JSON::toString(changed).toRawUTF8());
        REQUIRE(static_cast<bool>(changed["ok"])); CHECK(static_cast<bool>(changed["data"]["stateReloadVerified"]));
        CHECK(static_cast<bool>(changed["data"]["liveReadbackVerified"]));
        const auto saved = h.folder.getChildFile("Edited.als"); REQUIRE(io::saveAls(*h.song, saved).ok);
        auto loaded = io::loadAls(saved); REQUIRE(loaded);
        devices::BuildContext context; context.placeholderForMissingInstruments = false;
        auto restored = devices::createDevice(loaded.song->tracks[0].devices[0], context); REQUIRE(restored);
        CHECK_NEAR(restored->getParameter(0), value, 1e-4);
        REQUIRE(static_cast<bool>(h.undo(changed)["ok"]));
        h.history.redo(*h.song); h.engine.loadSong(*h.song);
        REQUIRE(io::saveAls(*h.song, saved).ok); auto redone = io::loadAls(saved); REQUIRE(redone);
        restored = devices::createDevice(redone.song->tracks[0].devices[0], context); REQUIRE(restored);
        CHECK_NEAR(restored->getParameter(0), value, 1e-4);
    }
}
TEST_CASE("control: native writes capture live before values, validate batches and guard automated targets and undo")
{
    DeviceHarness h; auto inserted = h.insert("StereoGain"); REQUIRE(static_cast<bool>(inserted["ok"]));
    const int id = inserted["data"]["deviceId"]; auto* p = h.processor(id); REQUIRE(p); p->setParameter(0, .6f);
    const auto changed = h.set(id, write("Gain", "Gain", .25)); REQUIRE(static_cast<bool>(changed["ok"]));
    const int newId = changed["data"]["deviceId"]; CHECK(newId != id); CHECK_NEAR(h.processor(newId)->getParameter(0), .25, 1e-6);
    CHECK_NEAR(steadyGain(*h.processor(newId)), .25, 1e-5);
    h.processor(newId)->setBypassed(true); CHECK(h.undo(changed)["error"]["code"].toString() == "undo_conflict");
    h.processor(newId)->setBypassed(false);
    REQUIRE(static_cast<bool>(h.undo(changed)["ok"])); CHECK_NEAR(h.processor(id)->getParameter(0), .6, 1e-6);
    CHECK_NEAR(steadyGain(*h.processor(id)), .6, 1e-5);
    const int version = h.history.version();
    for (const auto& bad : {write("Gain", "Wrong", .1), write("Gain", "Gain", -1), write("ChannelMode", "Channel", .5)})
        CHECK(! static_cast<bool>(h.set(id, bad)["ok"]));
    auto both = object({{"parameterId", "Gain"}, {"name", "Gain"}, {"normalized", .5}, {"value", 1}});
    CHECK(! static_cast<bool>(h.set(id, juce::Array<juce::var>{both})["ok"]));
    auto invalid = write("Gain", "Gain", .4); invalid.getArray()->add(object({{"parameterId", "missing"}, {"name", "missing"}, {"value", 1}}));
    CHECK(! static_cast<bool>(h.set(id, invalid)["ok"])); CHECK(h.history.version() == version); CHECK_NEAR(h.processor(id)->getParameter(0), .6, 1e-6);
    model::AutomationEnvelope envelope; envelope.targetId = h.song->tracks[0].devices[0].parameters[0].automationTargetId; envelope.points = {{0, .8}};
    model::Clip clip; clip.envelopes = {envelope}; model::ClipSlot slot; slot.clip = clip; h.song->tracks[0].slots.push_back(slot);
    CHECK(h.set(id, write("Gain", "Gain", .2))["error"]["code"].toString() == "automation_conflict");
    // An unrelated edit must retain the authored gain base, even if the current automation output differs.
    const double authored = h.song->tracks[0].devices[0].param("Gain", 1); h.processor(id)->setParameter(0, .8f);
    const auto balance = h.set(id, write("Balance", "Balance", .2)); REQUIRE(static_cast<bool>(balance["ok"]));
    CHECK_NEAR(h.song->tracks[0].devices[0].param("Gain", -1), authored, 1e-6);
    h.processor(balance["data"]["deviceId"])->setParameter(1, .7f);
    CHECK(h.undo(balance)["error"]["code"].toString() == "undo_conflict");
}
TEST_CASE("control: native normalized values use declared linear ranges and rack automation is protected")
{
    DeviceHarness h; const auto inserted = h.insert("StereoGain"); REQUIRE(static_cast<bool>(inserted["ok"]));
    const auto changed = h.set(inserted["data"]["deviceId"], juce::Array<juce::var>{
        object({{"parameterId", "Balance"}, {"name", "Balance"}, {"normalized", .75}}),
        object({{"parameterId", "Mono"}, {"name", "Mono"}, {"normalized", 1}})});
    REQUIRE(static_cast<bool>(changed["ok"]));
    CHECK_NEAR(h.song->tracks[0].devices[0].param("Balance", -1), .5, 1e-6);
    CHECK_NEAR(h.song->tracks[0].devices[0].param("Mono", -1), 1, 1e-6);
    auto rack = devices::nativeDeviceModel("AudioEffectGroupDevice"); model::Device::Chain chain;
    chain.devices.push_back(devices::nativeDeviceModel("StereoGain")); rack.chains.push_back(chain);
    model::allocateDeviceIds(*h.song, rack); h.song->tracks[0].devices = {rack};
    model::AutomationEnvelope envelope; envelope.targetId = rack.chains[0].devices[0].parameters[0].automationTargetId; envelope.points = {{0, .5}};
    h.song->tracks[0].automation = {envelope}; h.engine.loadSong(*h.song);
    CHECK(h.set(rack.id, write("MacroControls.0", "Macro 1", 64))["error"]["code"].toString() == "automation_conflict");
    h.song->tracks[0].automation.clear();
    auto* liveRack = dynamic_cast<devices::RackProcessor*>(h.processor(rack.id)); REQUIRE(liveRack);
    liveRack->chains()[0].processorForModelDevice[0]->setParameter(0, .42f);
    const auto macro = h.set(rack.id, write("MacroControls.0", "Macro 1", 64)); REQUIRE(static_cast<bool>(macro["ok"]));
    REQUIRE(static_cast<bool>(h.undo(macro)["ok"]));
    liveRack = dynamic_cast<devices::RackProcessor*>(h.processor(rack.id)); REQUIRE(liveRack);
    CHECK_NEAR(liveRack->chains()[0].processorForModelDevice[0]->getParameter(0), .42, 1e-6);
    const auto again = h.set(rack.id, write("MacroControls.0", "Macro 1", 96)); REQUIRE(static_cast<bool>(again["ok"]));
    liveRack = dynamic_cast<devices::RackProcessor*>(h.processor(again["data"]["deviceId"])); REQUIRE(liveRack);
    liveRack->chains()[0].processorForModelDevice[0]->setParameter(0, .77f);
    CHECK(h.undo(again)["error"]["code"].toString() == "undo_conflict");
    const auto removed = h.call("devices.remove", object({{"trackId", h.song->tracks[0].id}, {"deviceId", again["data"]["deviceId"]}}));
    REQUIRE(static_cast<bool>(removed["ok"])); REQUIRE(static_cast<bool>(h.undo(removed)["ok"]));
    CHECK_NEAR(h.song->tracks[0].devices[0].chains[0].devices[0].param("Gain", -1), .77, 1e-6);
}
TEST_CASE("control: device insertion ordering and removal retain live state across save undo redo")
{
    DeviceHarness h; auto effect = h.insert("StereoGain"); REQUIRE(static_cast<bool>(effect["ok"]));
    auto instrument = h.insert("SonoraFm"); REQUIRE(static_cast<bool>(instrument["ok"]));
    auto midi = h.insert("MidiPitcher"); REQUIRE(static_cast<bool>(midi["ok"]));
    CHECK(h.song->tracks[0].devices[0].type == "MidiPitcher"); CHECK(h.song->tracks[0].devices[1].type == "SonoraFm");
    CHECK(h.insert("SonoraVector")["error"]["code"].toString() == "instrument_conflict");
    CHECK(h.insert("SonoraFm", 2)["error"]["code"].toString() == "incompatible_track");
    CHECK(! static_cast<bool>(h.call("devices.insert", object({{"trackId", h.song->tracks[0].id}, {"catalogId", "native:StereoGain"}, {"position", 0}}))["ok"]));
    CHECK(! static_cast<bool>(h.insert("UnknownType")["ok"]));
    const int id = effect["data"]["deviceId"]; h.processor(id)->setParameter(0, .37f);
    const auto removed = h.call("devices.remove", object({{"trackId", h.song->tracks[0].id}, {"deviceId", id}})); REQUIRE(static_cast<bool>(removed["ok"]));
    const auto file = h.folder.getChildFile("Removed.als"); REQUIRE(io::saveAls(*h.song, file).ok);
    auto loaded = io::loadAls(file); REQUIRE(loaded); CHECK(loaded.song->tracks[0].devices.size() == 2);
    REQUIRE(static_cast<bool>(h.undo(removed)["ok"])); CHECK_NEAR(h.processor(id)->getParameter(0), .37, 1e-6);
    REQUIRE(io::saveAls(*h.song, file).ok); loaded = io::loadAls(file); REQUIRE(loaded);
    CHECK(loaded.song->tracks[0].devices.size() == 3); CHECK_NEAR(loaded.song->tracks[0].devices.back().param("Gain", -1), .37, 1e-6);
    h.history.redo(*h.song); h.engine.loadSong(*h.song); REQUIRE(io::saveAls(*h.song, file).ok);
    loaded = io::loadAls(file); REQUIRE(loaded); CHECK(loaded.song->tracks[0].devices.size() == 2);
}
TEST_CASE("control: native agent adapter binds catalogue insertion writes and removal to the current document")
{
    DeviceHarness h;
    const auto invoke = [&](const char* name, juce::var arguments) {
        return agent::executeTool(*h.service, {"test", name, arguments}, h.document, "native-agent-" + juce::String(++h.key));
    };
    auto catalog = invoke("sonora_devices_catalog", object({{"filter", "Utility"}})); REQUIRE(static_cast<bool>(catalog["ok"]));
    REQUIRE(catalog["data"]["devices"].size() == 1);
    auto inserted = invoke("sonora_devices_insert", object({{"trackId", h.song->tracks[0].id},
        {"catalogId", catalog["data"]["devices"][0]["catalogId"]}, {"expectedRevision", "0"}})); REQUIRE(static_cast<bool>(inserted["ok"]));
    auto changed = invoke("sonora_parameters_set", object({{"trackId", h.song->tracks[0].id}, {"deviceId", inserted["data"]["deviceId"]},
        {"parameters", write("Gain", "Gain", .25)}, {"expectedRevision", inserted["revision"]}})); REQUIRE(static_cast<bool>(changed["ok"]));
    auto removed = invoke("sonora_devices_remove", object({{"trackId", h.song->tracks[0].id}, {"deviceId", changed["data"]["deviceId"]},
        {"expectedRevision", changed["revision"]}})); REQUIRE(static_cast<bool>(removed["ok"])); CHECK(h.song->tracks[0].devices.empty());
    auto undone = invoke("sonora_history_undo", object({{"undoToken", removed["data"]["undoToken"]}, {"expectedRevision", removed["revision"]}}));
    REQUIRE(static_cast<bool>(undone["ok"])); CHECK_NEAR(h.song->tracks[0].devices[0].param("Gain", -1), .25, 1e-6);
    h.document = "replaced";
    CHECK(agent::executeTool(*h.service, {"old", "sonora_devices_catalog", object({})}, "device-control-test", "old")["error"]["code"].toString() == "stale_document");
}
