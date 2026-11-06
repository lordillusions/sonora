#include "TestFramework.h"
#include "core/Edit.h"
#include "plugins/LfoToolState.h"
#include "plugins/NativeReplacement.h"
#include "plugins/PluginDevice.h"
#include "ui/DeviceMenu.h"
#include "ui/UiContext.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <bit>
#include <zstd.h>

// PLG-16f: "Replace with Sonora Prism / Curve LFO" in the device menu, for installed plug-ins too, through the menu's
// own actions: live state first, one undoable edit, the prompt's notes. PLG-16e: the Serum 2 patch name.
using namespace sonora;

namespace
{
using Bytes = std::vector<uint8_t>;
void argument(Bytes& out, uint8_t major, uint64_t value)
{
    if (value < 24) { out.push_back(static_cast<uint8_t>((uint64_t) major << 5 | value)); return; }
    out.push_back(static_cast<uint8_t>(major << 5 | 25));
    out.push_back(static_cast<uint8_t>(value >> 8)); out.push_back(static_cast<uint8_t>(value));
}
void text(Bytes& out, std::string_view s) { argument(out, 3, s.size()); out.insert(out.end(), s.begin(), s.end()); }
void number(Bytes& out, double n)
{
    out.push_back(0xfb);
    const auto bits = std::bit_cast<uint64_t>(n);
    for (unsigned i = 8; i-- > 0;) out.push_back(static_cast<uint8_t>(bits >> (i * 8)));
}
void little(Bytes& out, uint64_t value, unsigned count)
{
    for (unsigned i = 0; i < count; ++i) out.push_back(static_cast<uint8_t>(value >> (8 * i)));
}
// A synthetic Serum 2 container (XferJson, zstd CBOR) for `component`, Macro 1 at `macro` %, a patch name in the
// container metadata as the hosted plug-in writes it in its controller state.
Bytes serumState(const std::string& component, double macro, const std::string& presetName = {})
{
    Bytes cbor;
    argument(cbor, 5, component == "processor" ? 5 : 4);
    text(cbor, "product"); text(cbor, "Serum2");
    text(cbor, "component"); text(cbor, component);
    text(cbor, "version"); number(cbor, 9);
    text(cbor, "productVersion"); text(cbor, "2.0.24");
    if (component == "processor")
    {
        text(cbor, "Macro0"); argument(cbor, 5, 1); text(cbor, "plainParams"); argument(cbor, 5, 1);
        text(cbor, "kParamValue"); number(cbor, macro);
    }
    Bytes compressed(ZSTD_compressBound(cbor.size()));
    const auto size = ZSTD_compress(compressed.data(), compressed.size(), cbor.data(), cbor.size(), 3);
    REQUIRE(! ZSTD_isError(size));
    compressed.resize(size);
    std::string meta = "{\"component\":\"" + component + "\",";
    if (! presetName.empty()) meta += "\"presetName\":\"" + presetName + "\",";
    meta += "\"product\":\"Serum2\",\"productVersion\":\"2.0.24\",\"version\":9.0}";
    Bytes out{'X', 'f', 'e', 'r', 'J', 's', 'o', 'n', 0};
    little(out, meta.size(), 8);
    out.insert(out.end(), meta.begin(), meta.end());
    little(out, cbor.size(), 4);
    little(out, 2, 4);
    out.insert(out.end(), compressed.begin(), compressed.end());
    return out;
}

model::Device serum2(int id, double macro)
{
    model::Device d;
    d.kind = model::DeviceKind::PluginVst3; d.type = "PluginDevice"; d.displayName = "Serum 2"; d.id = id;
    d.plugin.emplace();
    d.plugin->name = "Serum 2";
    d.plugin->vst3Uid = {1448297816u, 1718833267u, 1701999981u, 540147712u};
    d.plugin->isInstrument = true;
    d.plugin->state = serumState("processor", macro);
    d.plugin->controllerState = serumState("controller", 0, "[PAD] - Darkness");
    model::PluginParam macro1; macro1.name = "Macro 1"; macro1.value = macro / 100; macro1.automationTargetId = 900 + id;
    d.pluginParams = {macro1};
    return d;
}
model::Device lfoTool(int id)
{
    model::Device d;
    d.kind = model::DeviceKind::PluginVst2; d.type = "PluginDevice"; d.displayName = "LFOTool"; d.id = id;
    d.plugin.emplace();
    d.plugin->name = "LFOTool";
    d.plugin->vst2UniqueId = 1483105871;
    d.plugin->vst2PresetType = 0x46424368; // 'FBCh'
    Bytes chunk(18824, 0); // a 1.25 state: version, 1/4 synced, graph 1 = saw up (y stored from the top)
    const auto put = [&chunk](size_t offset, auto value) {
        const auto bits = std::bit_cast<std::conditional_t<sizeof(value) == 8, uint64_t, uint32_t>>(value);
        for (size_t i = 0; i < sizeof(value); ++i) chunk[offset + i] = (uint8_t) (bits >> (8 * i));
    };
    put(18432 + 340, 1.25f); put(18432 + 26 * 4, 18.5f / 42); put(18432 + 31 * 4, 1.0f); put(18432 + 35 * 4, 1.0f);
    put(18432 + 39 * 4, 0.5f); // phase 180 degrees
    for (size_t g = 0; g < 12; ++g) put(18432 + (13 + g) * 4, 8.0f / 17);
    put(8 * 768 + 8, 65536.0); put(8 * 1536, 1.0); put(0, 0.5); put(8, 0.5);
    chunk[18432 + 352] = 1;
    d.plugin->state = chunk;
    return d;
}

struct Rig
{
    model::Song song;
    model::EditHistory history;
    ui::UiContext ctx;
    int captures = 0;
    double liveMacro = 40;
    Rig()
    {
        song.nextDeviceId = 100; song.nextPointeeId = 1000;
        model::Track lead;
        lead.kind = model::TrackKind::Midi; lead.id = 1; lead.name = "1-Lead";
        lead.devices.push_back(serum2(10, 75));
        song.tracks.push_back(lead);
        model::Track pad = lead;
        pad.id = 2; pad.name = "2-Pad"; pad.devices = {serum2(11, 10)};
        song.tracks.push_back(pad);
        model::Track bass;
        bass.kind = model::TrackKind::Audio; bass.id = 3; bass.name = "3-Bass";
        model::Device rack;
        rack.kind = model::DeviceKind::Rack; rack.type = "AudioEffectGroupDevice"; rack.displayName = "Rack"; rack.id = 20;
        rack.chains.resize(1);
        rack.chains[0].devices.push_back(lfoTool(21));
        bass.devices.push_back(rack);
        song.tracks.push_back(bass);
        model::Track frozen = pad;
        frozen.id = 4; frozen.name = "4-Frozen"; frozen.frozen = true; frozen.devices = {serum2(12, 50)};
        song.tracks.push_back(frozen);
        ctx.song = &song;
        ctx.applyEdit = [this](model::EditCommand c) { history.perform(song, std::move(c)); };
        // Stands in for AppController::syncModelFromEngine: the running Serum 2 on track 1 was edited since the load.
        ctx.capturePluginStates = [this] {
            ++captures;
            song.tracks[0].devices[0].plugin->state = serumState("processor", liveMacro);
        };
    }
    const ui::MenuAction* action(const std::vector<ui::MenuAction>& actions, const std::string& prefix)
    {
        for (const auto& a : actions) if (a.label.rfind(prefix, 0) == 0) return &a;
        return nullptr;
    }
};
double param(const model::Device& d, const std::string& path)
{
    const auto it = std::find_if(d.parameters.begin(), d.parameters.end(), [&](const auto& p) { return p.path == path; });
    REQUIRE(it != d.parameters.end());
    return it->value;
}
} // namespace

TEST_CASE("device menu: Replace with Sonora Prism swaps an installed Serum 2 from its live state, one undo")
{
    Rig rig;
    const auto actions = ui::deviceContextActions(rig.ctx, 0, {0});
    REQUIRE(rig.action(actions, "Delete") != nullptr); // the device's own commands stay first
    const auto* one = rig.action(actions, "Replace with Sonora Prism");
    const auto* all = rig.action(actions, "Replace All 2 Serum 2 in This Set with Sonora Prism"); // the frozen one is not counted
    REQUIRE(one != nullptr); REQUIRE(all != nullptr);
    CHECK(one->enabled); CHECK(all->enabled);
    CHECK(rig.action(actions, "Replace with Sonora Curve LFO") == nullptr);

    // What the swap must produce: the same conversion as the prompt's, from the live state.
    auto expectedSong = rig.song;
    expectedSong.tracks[0].devices[0].plugin->state = serumState("processor", rig.liveMacro);
    const auto expected = plugins::nativeReplacementAt(expectedSong, 0, {0});
    REQUIRE(expected.has_value());
    const auto original = rig.song.tracks[0].devices[0];

    one->run();
    CHECK(rig.captures == 1);
    const auto& prism = rig.song.tracks[0].devices[0];
    CHECK(prism.type == "SonoraWavetable");
    CHECK(prism.displayName == "[PAD] - Darkness"); // PLG-16e
    REQUIRE(prism.parameters.size() == expected->device.parameters.size());
    for (size_t i = 0; i < prism.parameters.size(); ++i)
    {
        CHECK(prism.parameters[i].path == expected->device.parameters[i].path);
        CHECK_NEAR(prism.parameters[i].value, expected->device.parameters[i].value, 1e-12);
    }
    CHECK_NEAR(param(prism, "Macro.0"), 0.4, 1e-9); // the live 40 %, not the stored 75 %
    const auto lane = std::find_if(prism.parameters.begin(), prism.parameters.end(), [](const auto& p) { return p.path == "Macro.0"; });
    CHECK(lane->automationTargetId == 910); // Serum's Macro 1 lane drives Prism's macro
    CHECK(rig.ctx.selectedDeviceId == prism.id);
    CHECK(rig.song.tracks[1].devices[0].kind == model::DeviceKind::PluginVst3); // only this instance
    CHECK(rig.ctx.statusText.rfind("Replaced 1 plug-in with Sonora devices", 0) == 0);
    CHECK(std::any_of(rig.ctx.buildLog.begin(), rig.ctx.buildLog.end(),
                      [](const std::string& line) { return line.rfind("Serum 2 → Sonora Prism (1-Lead): ", 0) == 0; }));

    REQUIRE(rig.history.undo(rig.song) != nullptr); // one undo brings the hosted plug-in back, with the captured state
    const auto& back = rig.song.tracks[0].devices[0];
    CHECK(back.kind == model::DeviceKind::PluginVst3);
    REQUIRE(back.plugin.has_value());
    CHECK(back.plugin->name == "Serum 2");
    CHECK(back.plugin->state == serumState("processor", rig.liveMacro));
    CHECK(back.plugin->controllerState == original.plugin->controllerState);
    CHECK(back.id == original.id);
}

TEST_CASE("device menu: Replace All covers the set, racks included; frozen tracks and recording are left alone")
{
    Rig rig;
    auto actions = ui::deviceContextActions(rig.ctx, 1, {0});
    rig.action(actions, "Replace All 2 Serum 2")->run();
    CHECK(rig.song.tracks[0].devices[0].type == "SonoraWavetable");
    CHECK(rig.song.tracks[1].devices[0].type == "SonoraWavetable");
    CHECK(rig.song.tracks[3].devices[0].kind == model::DeviceKind::PluginVst3); // frozen: kept
    CHECK(rig.song.tracks[2].devices[0].chains[0].devices[0].kind == model::DeviceKind::PluginVst2); // LFOTool untouched
    CHECK(rig.ctx.statusText.rfind("Replaced 2 plug-ins", 0) == 0);
    REQUIRE(rig.history.undo(rig.song) != nullptr);
    CHECK(rig.song.tracks[0].devices[0].kind == model::DeviceKind::PluginVst3);
    CHECK(rig.song.tracks[1].devices[0].kind == model::DeviceKind::PluginVst3);

    // A rack child: its menu offers the swap alone; the Curve LFO gets LFOTool's state.
    actions = ui::deviceContextActions(rig.ctx, 2, {0, 0, 0});
    CHECK(rig.action(actions, "Delete") == nullptr);
    CHECK(rig.action(actions, "Replace All") == nullptr); // one LFOTool in the set
    const auto* swap = rig.action(actions, "Replace with Sonora Curve LFO");
    REQUIRE(swap != nullptr);
    swap->run();
    const auto& rack = rig.song.tracks[2].devices[0];
    CHECK(rack.chainsEdited);
    const auto& curve = rack.chains[0].devices[0];
    CHECK(curve.type == "SonoraCurveLfo");
    CHECK_NEAR(param(curve, "Phase"), 0.5, 1e-6);
    CHECK_NEAR(param(curve, "G1.0Y"), 0.0, 1e-6); // saw up
    CHECK_NEAR(param(curve, "G1.1Y"), 1.0, 1e-6);
    REQUIRE(rig.history.undo(rig.song) != nullptr);
    CHECK(rig.song.tracks[2].devices[0].chains[0].devices[0].kind == model::DeviceKind::PluginVst2);

    actions = ui::deviceContextActions(rig.ctx, 3, {0});
    CHECK(! rig.action(actions, "Replace with Sonora Prism")->enabled); // frozen track
    rig.ctx.recording = true;
    actions = ui::deviceContextActions(rig.ctx, 0, {0});
    CHECK(! rig.action(actions, "Replace with Sonora Prism")->enabled);
    CHECK(ui::replaceWithNativeDevices(rig.ctx, 0, {0}, false) == 0);
}

TEST_CASE("plug-in capture: an all-zero chunk is no report and keeps the set's state")
{
    const Bytes real(18824, 7), zeros(18824, 0);
    CHECK(plugins::pluginChunkUnreported(model::DeviceKind::PluginVst2, zeros, real));
    CHECK(! plugins::pluginChunkUnreported(model::DeviceKind::PluginVst2, real, zeros));
    CHECK(! plugins::pluginChunkUnreported(model::DeviceKind::PluginVst2, zeros, zeros)); // nothing better to keep
    CHECK(! plugins::pluginChunkUnreported(model::DeviceKind::PluginVst3, zeros, real));
    const auto plist = [](const Bytes& chunk) {
        Bytes fxp{'C', 'c', 'n', 'K'}; fxp.resize(60, 1); fxp.insert(fxp.end(), chunk.begin(), chunk.end());
        const auto text = "<?xml version=\"1.0\"?><plist version=\"1.0\"><dict><key>vstdata</key><data>"
                          + juce::Base64::toBase64(fxp.data(), fxp.size()) + "</data></dict></plist>";
        return Bytes(text.toRawUTF8(), text.toRawUTF8() + text.getNumBytesAsUTF8());
    };
    CHECK(plugins::pluginChunkUnreported(model::DeviceKind::PluginAu, plist(zeros), plist(real)));
    CHECK(! plugins::pluginChunkUnreported(model::DeviceKind::PluginAu, plist(real), plist(zeros)));
}
