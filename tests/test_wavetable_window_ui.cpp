// UI-27: Sonora Prism floats in its own window by default; per-instance dock/float toggles.
#include "TestFramework.h"
#include "devices/DeviceFactory.h"
#include "devices/native/WavetableSynth.h"
#include "engine/EngineGraph.h"
#include "engine/SampleCache.h"
#include "gfx/SkiaCanvasComponent.h"
#include "midi/MidiManager.h"
#include "ui/views/MainLayout.h"
#include "ui/views/WavetableEditor.h"
#include "ui/views/WavetableWindowView.h"
#include "ui/widgets/Buttons.h"
#include <juce_gui_basics/juce_gui_basics.h>

using namespace sonora;
namespace
{
using Key = ui::UiContext::WavetableKey;

template <typename T> void collect(ui::Widget& widget, std::vector<T*>& result)
{
    if (auto* found = dynamic_cast<T*>(&widget)) result.push_back(found);
    for (const auto& child : widget.children()) collect(*child, result);
}
template <typename T> std::vector<T*> all(ui::Widget& root)
{
    std::vector<T*> result;
    collect(root, result);
    return result;
}
ui::TextButton* button(ui::Widget& root, const std::string& label)
{
    for (auto* b : all<ui::TextButton>(root)) if (b->visible && b->label() == label) return b;
    return nullptr;
}
ui::IconButton* icon(ui::Widget& root, ui::IconButton::Icon kind)
{
    for (auto* b : all<ui::IconButton>(root)) if (b->visible && b->icon() == kind) return b;
    return nullptr;
}
void click(ui::RootWidget& root, ui::Widget* widget)
{
    REQUIRE(widget != nullptr);
    const auto r = widget->boundsInRoot();
    ui::MouseEvent e; e.x = r.centreX(); e.y = r.centreY();
    root.dispatchMouseDown(e); root.dispatchMouseUp(e);
}

// Two MIDI tracks whose Wavetables share device id 42, as imported Live device ids repeat across tracks.
struct WindowRig
{
    juce::ScopedJuceInitialiser_GUI init;
    midi::MidiManager midi;
    model::Song song;
    engine::SampleCache samples;
    std::unique_ptr<engine::EngineGraph> graph;
    ui::UiContext context;
    std::unique_ptr<ui::MainLayout> root;
    std::vector<Key> opened, closed;

    WindowRig()
    {
        song.tracks.resize(2);
        for (int i = 0; i < 2; ++i)
        {
            auto& track = song.tracks[(size_t) i];
            track.kind = model::TrackKind::Midi; track.id = 10 + i; track.name = i == 0 ? "Lead" : "Pad";
            track.devices = {devices::nativeDeviceModel("SonoraWavetable")}; track.devices[0].id = 42;
        }
        rebuildGraph();
        context.midi = &midi; context.browserVisible = false; context.detailTab = ui::DetailTab::Devices;
        context.theme.metrics.detailHeight = 700;
        context.wavetableFloatingDefault = true; // as the app sets it
        context.openWavetableWindow = [this](const Key& key) { opened.push_back(key); };
        context.closeWavetableWindow = [this](const Key& key) { closed.push_back(key); };
        root = std::make_unique<ui::MainLayout>(context); root->setBounds({0, 0, 1400, 1000}); root->layout();
    }
    void rebuildGraph()
    {
        context.graph = nullptr;
        devices::BuildContext build; graph = engine::buildGraph(song, build, samples).graph;
        REQUIRE(graph); graph->prepare(48000, 512);
        context.song = &song; context.graph = graph.get(); context.controls = graph->controls;
    }
    size_t dockedEditors() { root->layout(); return all<ui::WavetableEditor>(*root).size(); }
};
} // namespace

TEST_CASE("wavetable window: floating by default, Show Editor opens it, Dock and Float toggle the same instance")
{
    WindowRig rig;
    const Key lead{10, 42, -1, -1}, pad{11, 42, -1, -1};
    CHECK(rig.context.wavetableKey(0, 0) == lead);
    CHECK(rig.context.isWavetableFloating(lead));
    CHECK(rig.dockedEditors() == 0); // the chain shows a stub, not the 1080 px dashboard
    click(*rig.root, button(*rig.root, "Show Editor"));
    REQUIRE(rig.opened.size() == 1); CHECK(rig.opened[0] == lead);
    click(*rig.root, icon(*rig.root, ui::IconButton::Icon::Wrench)); // title wrench, like a plug-in's
    REQUIRE(rig.opened.size() == 2); CHECK(rig.opened[1] == lead);

    // The floating window: header with the name and Dock, the full dashboard below.
    ui::WavetableWindowView view(rig.context, lead);
    std::string title;
    view.onTitleChanged = [&](const std::string& t) { title = t; };
    view.setBounds({0, 0, ui::WavetableWindowView::kPreferredWidth, ui::WavetableWindowView::kPreferredHeight});
    view.sync();
    REQUIRE(view.editor() != nullptr); CHECK(view.editor()->dashboardVisible());
    CHECK(title == "Sonora Prism/Lead"); CHECK(view.title() == title);
    CHECK(rig.closed.empty());
    click(view, view.dockButton());
    CHECK(! rig.context.isWavetableFloating(lead));
    CHECK(rig.context.isWavetableFloating(pad)); // per instance, even with the same device id on another track
    REQUIRE(rig.closed.size() == 1); CHECK(rig.closed[0] == lead);
    CHECK(rig.context.selectedTrack == 0); CHECK(rig.context.detailTab == ui::DetailTab::Devices);
    CHECK(rig.dockedEditors() == 1);

    // Docked header: the float button returns it to its own window.
    rig.opened.clear();
    click(*rig.root, icon(*rig.root, ui::IconButton::Icon::FloatWindow));
    CHECK(rig.context.isWavetableFloating(lead));
    REQUIRE(rig.opened.size() == 1); CHECK(rig.opened[0] == lead);
    CHECK(rig.dockedEditors() == 0);

    // The chain stub can dock too; another track's instance keeps its own choice.
    rig.closed.clear();
    click(*rig.root, button(*rig.root, "Dock"));
    CHECK(! rig.context.isWavetableFloating(lead));
    REQUIRE(rig.closed.size() == 1); CHECK(rig.closed[0] == lead);
    CHECK(rig.dockedEditors() == 1);
    rig.context.selectedTrack = 1;
    CHECK(rig.dockedEditors() == 0);
    CHECK(button(*rig.root, "Show Editor") != nullptr);
}

TEST_CASE("wavetable window: follows graph rebuilds and id swaps, closes when docked or deleted")
{
    WindowRig rig;
    const Key lead{10, 42, -1, -1};
    ui::WavetableWindowView view(rig.context, lead);
    view.setBounds({0, 0, ui::WavetableWindowView::kPreferredWidth, ui::WavetableWindowView::kPreferredHeight});
    view.sync();
    REQUIRE(view.editor() != nullptr);

    // A device edit: the song is detached first, then a new graph arrives. The window stays open throughout.
    rig.context.song = nullptr; view.sync();
    CHECK(view.editor() == nullptr); CHECK(rig.closed.empty());
    rig.rebuildGraph(); view.sync();
    REQUIRE(view.editor() != nullptr); CHECK(rig.closed.empty());

    // A structural edit, undo or redo replaces the device in place under another id: placement and window follow.
    rig.context.setWavetableFloating(lead, true);
    const auto before = ui::deviceIdsByTrack(rig.song);
    rig.song.tracks[0].devices[0].id = 77;
    const auto replaced = ui::replacedDevices(before, rig.song);
    REQUIRE(replaced.size() == 1);
    CHECK(replaced[0].track == 10); CHECK(replaced[0].oldId == 42); CHECK(replaced[0].newId == 77);
    rig.context.renameWavetableDevice(10, 42, 77);
    const Key moved{10, 77, -1, -1};
    CHECK(rig.context.wavetableFloating.count(moved) == 1); CHECK(rig.context.wavetableFloating.count(lead) == 0);
    CHECK(rig.context.wavetableFloating.count(Key{11, 42, -1, -1}) == 0);
    view.setKey(moved); rig.rebuildGraph(); view.sync();
    REQUIRE(view.editor() != nullptr); CHECK(rig.closed.empty());

    // Inserting or deleting shifts positions: those are not replacements.
    const auto beforeInsert = ui::deviceIdsByTrack(rig.song);
    rig.song.tracks[0].devices.insert(rig.song.tracks[0].devices.begin(), devices::nativeDeviceModel("StereoGain"));
    rig.song.tracks[0].devices[0].id = 5;
    CHECK(ui::replacedDevices(beforeInsert, rig.song).empty());
    rig.song.tracks[0].devices.erase(rig.song.tracks[0].devices.begin());

    // Docked elsewhere (the chain stub): the window asks to close once.
    rig.context.setWavetableFloating(moved, false); view.sync(); view.sync();
    REQUIRE(rig.closed.size() == 1); CHECK(rig.closed[0] == moved);

    // Deleted: like a plug-in window whose device went away.
    rig.closed.clear();
    ui::WavetableWindowView padView(rig.context, Key{11, 42, -1, -1});
    padView.setBounds({0, 0, 1092, 746}); padView.sync();
    REQUIRE(padView.editor() != nullptr);
    rig.song.tracks[1].devices.clear(); rig.rebuildGraph(); padView.sync();
    CHECK(padView.editor() == nullptr);
    REQUIRE(rig.closed.size() == 1); CHECK(rig.closed[0] == (Key{11, 42, -1, -1}));
}

TEST_CASE("wavetable window: a Wavetable inside a rack chain has its own key and floating editor")
{
    juce::ScopedJuceInitialiser_GUI init;
    model::Song song; song.tracks.resize(1);
    auto& track = song.tracks[0]; track.kind = model::TrackKind::Midi; track.id = 3;
    track.devices = {devices::nativeDeviceModel("InstrumentGroupDevice")};
    auto& rack = track.devices[0]; rack.id = 8; rack.chains.resize(2);
    rack.chains[1].devices = {devices::nativeDeviceModel("SonoraWavetable")}; rack.chains[1].devices[0].id = 8;
    engine::SampleCache samples; devices::BuildContext build;
    auto graph = engine::buildGraph(song, build, samples).graph; REQUIRE(graph); graph->prepare(48000, 512);
    ui::UiContext context; context.song = &song; context.graph = graph.get(); context.controls = graph->controls;
    const auto key = context.wavetableKey(0, 0, 1, 0);
    REQUIRE(key.has_value()); CHECK(*key == (Key{3, 8, 1, 8}));
    CHECK(! context.wavetableKey(0, 0, 2, 0)); CHECK(! context.isWavetableFloating(*key)); // tests/snapshots dock by default
    const auto site = ui::findWavetable(context, *key);
    REQUIRE(site.has_value());
    CHECK(site->track == 0); CHECK(site->device == 0); CHECK(site->chain == 1); CHECK(site->child == 0);
    CHECK(site->processor != nullptr);
    CHECK(! ui::findWavetable(context, Key{3, 8, -1, -1})); // the rack itself is not a Wavetable
    context.wavetableFloatingDefault = true;
    ui::WavetableWindowView view(context, *key);
    view.setBounds({0, 0, 1092, 746}); view.sync();
    REQUIRE(view.editor() != nullptr);
    std::vector<Key> closed; context.closeWavetableWindow = [&](const Key& k) { closed.push_back(k); };
    click(view, view.dockButton());
    CHECK(! context.isWavetableFloating(*key)); CHECK(context.selectedRackId == 8); CHECK(context.selectedRackChain == 1);
    CHECK(closed.size() == 1);
}
