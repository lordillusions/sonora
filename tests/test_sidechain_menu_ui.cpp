// PLG-14i: choosing where a built-in device's sidechain listens (the device menu's "Sidechain" submenu, and the built-in
// Hush's Sidechain switch), routed through the engine graph.
#include "TestFramework.h"
#include "devices/DeviceFactory.h"
#include "devices/native/HushSession.h"
#include "engine/EngineGraph.h"
#include "engine/SampleCache.h"
#include "ui/DeviceMenu.h"
#include "ui/UiContext.h"

using namespace sonora;

TEST_CASE("sidechain menu: Hush on the bass listens to the kick track; None turns it off")
{
    model::Song song;
    song.tracks.resize(2);
    song.tracks[0].id = 7;
    song.tracks[0].name = "Kick";
    song.tracks[1].id = 9;
    song.tracks[1].name = "Bass";
    auto hushModel = devices::nativeDeviceModel("SonoraHush");
    hushModel.id = 30;
    song.tracks[1].devices = {hushModel};
    ui::UiContext ctx;
    ctx.song = &song;
    int edits = 0;
    std::string label;
    ctx.editDevice = [&](int track, int device, std::function<void(model::Device&)> edit, std::string name) {
        edit(song.tracks[(size_t) track].devices[(size_t) device]);
        label = name;
        ++edits;
    };

    CHECK(ui::deviceTakesSidechain(song.tracks[1].devices[0]));
    auto actions = ui::sidechainActions(ctx, 1, 0);
    REQUIRE(actions.size() == 3); // None, separator, Kick (not the bass itself)
    CHECK(actions[0].label == "None");
    CHECK(actions[0].ticked);
    CHECK(actions[2].label == "Kick");
    // The device menu carries it as a submenu.
    bool found = false;
    for (const auto& a : ui::deviceContextActions(ctx, 1, {0}))
        if (a.label == "Sidechain") found = a.submenu.size() == actions.size();
    CHECK(found);

    actions[2].run();
    const auto& d = song.tracks[1].devices[0];
    CHECK(edits == 1);
    CHECK(label == "Sidechain from Kick");
    CHECK(d.sidechain.routed());
    CHECK(d.sidechain.sourceTrackId == 7);
    CHECK(d.sidechain.edited);
    CHECK_NEAR(d.param("Sidechain_on", -1), 1.0, 1e-9);
    CHECK(ui::sidechainActions(ctx, 1, 0)[2].ticked);

    // The engine links the kick's tap into the bass's Hush, and Hush detects on it.
    engine::SampleCache samples;
    devices::BuildContext build;
    auto graph = engine::buildGraph(song, build, samples);
    REQUIRE(graph.graph->nodes.size() >= 2);
    REQUIRE(graph.graph->nodes[1].sidechains.size() == 1);
    CHECK(graph.graph->nodes[1].sidechains[0].sourceNode == 0);
    auto* hush = dynamic_cast<devices::HushDevice*>(graph.graph->nodes[1].processorForModelDevice[0]);
    REQUIRE(hush != nullptr);
    CHECK(hush->getParameter(devices::Hush::Sidechain) > 0.5f);

    ui::sidechainActions(ctx, 1, 0)[0].run();
    CHECK(! song.tracks[1].devices[0].sidechain.routed());
    CHECK_NEAR(song.tracks[1].devices[0].param("Sidechain_on", -1), 0.0, 1e-9);
    auto plain = engine::buildGraph(song, build, samples);
    CHECK(plain.graph->nodes[1].sidechains.empty());

    // A device that takes no sidechain gets no submenu.
    song.tracks[0].devices = {devices::nativeDeviceModel("Eq8")};
    CHECK(ui::sidechainActions(ctx, 0, 0).empty());
}
