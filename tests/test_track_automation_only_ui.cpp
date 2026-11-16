// UI-88: View > Show This Track's Automation Only (Opt+Shift+A, also in the track header menu): the selected track shows a
// lane per automated parameter and no other track shows any; the same command again gives back the lanes shown before.
#include "TestFramework.h"

#include "core/Edit.h"
#include "io/als/AlsReader.h"
#include "ui/TrackMenu.h"
#include "ui/views/ArrangementView.h"
#include "ui/views/AutomationEditor.h"
#include "ui/views/MainLayout.h"

#include <juce_core/juce_core.h>
#include <juce_gui_basics/juce_gui_basics.h>

using namespace sonora;

namespace
{
template <typename T>
T* findWidget(ui::Widget& w)
{
    if (auto* hit = dynamic_cast<T*>(&w)) return hit;
    for (auto& child : w.children())
        if (auto* hit = findWidget<T>(*child)) return hit;
    return nullptr;
}

model::AutomationEnvelope lane(int targetId, double value)
{
    model::AutomationEnvelope env;
    env.targetId = targetId;
    env.points = {{-63072000.0, value}, {0, value}, {4, 1.0 - value}};
    return env;
}

struct Rig
{
    model::Song song;
    model::EditHistory history;
    ui::UiContext ctx;
    std::unique_ptr<ui::MainLayout> root;
    Rig()
    {
        auto loaded = io::loadAls(juce::File(SONORA_TEST_FIXTURES).getChildFile("two_tracks.xml"));
        REQUIRE(loaded);
        song = *loaded.song;
        REQUIRE(song.tracks.size() >= 2);
        song.tracks[0].automation = {lane(9001, 0.2), lane(9002, 0.7)};
        song.tracks[1].automation = {lane(9003, 0.4)};
        ctx.song = &song;
        ctx.ioVisible = ctx.browserVisible = ctx.detailVisible = false;
        ctx.mainView = ui::MainView::Arrangement;
        ctx.applyEdit = [this](model::EditCommand c) { history.perform(song, std::move(c)); };
        root = std::make_unique<ui::MainLayout>(ctx);
        root->setBounds({0, 0, 1200, 700});
        root->layout();
    }
    bool press() { return root->dispatchKey({.keyCode = 'A', .textChar = 'A', .shift = true, .alt = true}); }
    float laneHeight(int track)
    {
        auto* arrangement = findWidget<ui::ArrangementView>(*root);
        REQUIRE(arrangement != nullptr);
        for (const auto& l : arrangement->lanes()) if (l.controlIndex == track && l.takeLaneId < 0) return l.h;
        return -1;
    }
    ui::MenuAction menuItem()
    {
        for (auto& item : ui::trackContextActions(ctx)) if (item.label == "Show This Track's Automation Only") return item;
        return {};
    }
};
} // namespace

TEST_CASE("track automation only (UI-88): Opt+Shift+A shows the selected track's automated lanes and hides the others")
{
    Rig rig;
    const float plain = rig.laneHeight(1);
    // Show All Automation first (Shift+A): both tracks show their lanes.
    ui::AutomationEditor::toggleAllAutomation(rig.ctx); rig.root->layout();
    REQUIRE(rig.ctx.showsAutomation(0)); REQUIRE(rig.ctx.showsAutomation(1));
    const auto allTracks = rig.ctx.automationTracks;
    const auto allLanes = rig.ctx.automationLaneTargets;

    rig.ctx.selectedTrack = 0;
    REQUIRE(rig.press());
    CHECK(rig.ctx.showsAutomation(0)); CHECK(! rig.ctx.showsAutomation(1));
    CHECK((rig.ctx.automationLaneTargets[0] == std::vector<int>{9001, 9002}));
    CHECK(ui::AutomationEditor::trackAutomationOnlyActive(rig.ctx, 0));
    CHECK(rig.ctx.statusText.find("Showing only") == 0);
    CHECK_NEAR(rig.laneHeight(0) - plain, 2 * ui::UiContext::automationLaneHeight, 1e-3);
    CHECK_NEAR(rig.laneHeight(1), plain, 1e-3);
    // The track header menu offers it with the same shortcut, ticked while it is on.
    auto item = rig.menuItem();
    REQUIRE(item.run); CHECK(item.enabled); CHECK(item.ticked); CHECK(item.shortcut == "\xE2\x8C\xA5\xE2\x87\xA7" "A");

    // Pressed on another track while on: that track's lanes instead.
    rig.ctx.selectedTrack = 1;
    REQUIRE(rig.press());
    CHECK(! rig.ctx.showsAutomation(0)); CHECK(rig.ctx.showsAutomation(1));
    CHECK((rig.ctx.automationLaneTargets[1] == std::vector<int>{9003}));
    CHECK(rig.menuItem().ticked);

    // Again on the same track: the lanes shown before come back exactly.
    REQUIRE(rig.press());
    CHECK(! ui::AutomationEditor::trackAutomationOnlyActive(rig.ctx));
    CHECK(rig.ctx.automationVisible); CHECK(rig.ctx.automationTracks == allTracks);
    CHECK(rig.ctx.automationLaneTargets == allLanes);
    CHECK(rig.ctx.showsAutomation(0)); CHECK(rig.ctx.showsAutomation(1));
    CHECK(rig.ctx.statusText == "Automation lanes restored");
}

TEST_CASE("track automation only (UI-88): from hidden lanes and back, a track without automation, the menu item, stale state")
{
    Rig rig;
    REQUIRE(! rig.ctx.automationVisible);
    // From no lanes at all: the track's lanes show, and the second press hides them again.
    rig.ctx.selectedTrack = 1;
    auto item = rig.menuItem();
    REQUIRE(item.run); CHECK(! item.ticked);
    item.run();
    CHECK(rig.ctx.showsAutomation(1)); CHECK(! rig.ctx.showsAutomation(0));
    CHECK(rig.menuItem().ticked);
    rig.menuItem().run();
    CHECK(! rig.ctx.automationVisible); CHECK(rig.ctx.automationTracks.empty());
    CHECK(! rig.ctx.automationLaneTargets.count(1)); // the lanes are what they were before, none chosen yet

    // A track without automation still shows its one lane (it picks its own target) and says so.
    rig.song.tracks[1].automation.clear();
    REQUIRE(rig.press());
    CHECK(rig.ctx.showsAutomation(1)); CHECK(rig.ctx.automationLaneCount(1) == 1);
    CHECK(rig.ctx.statusText.find("has no automation yet") != std::string::npos);
    REQUIRE(rig.press());
    CHECK(! rig.ctx.automationVisible);

    // The lanes changed in between (A hid the track): the next press starts over from what shows now.
    rig.ctx.selectedTrack = 0;
    REQUIRE(rig.press()); CHECK(ui::AutomationEditor::trackAutomationOnlyActive(rig.ctx, 0));
    rig.ctx.toggleAutomationLane(0);
    CHECK(! ui::AutomationEditor::trackAutomationOnlyActive(rig.ctx));
    REQUIRE(rig.press());
    CHECK(rig.ctx.showsAutomation(0)); CHECK(ui::AutomationEditor::trackAutomationOnlyActive(rig.ctx, 0));
    REQUIRE(rig.press());
    CHECK(! rig.ctx.automationVisible); // back to the hidden lanes the second start saw

    // Another set: the remembered lanes belong to the old one and are not restored there.
    REQUIRE(rig.press());
    model::Song other = rig.song;
    rig.ctx.song = &other;
    CHECK(! ui::AutomationEditor::trackAutomationOnlyActive(rig.ctx));
    rig.ctx.song = &rig.song;

    // Opt+Shift+A does not reach the A / Shift+A handlers (they would toggle lanes too).
    rig.ctx.automationVisible = false; rig.ctx.automationTracks.clear(); rig.ctx.automationOnlyTrack.reset();
    REQUIRE(rig.press());
    CHECK(rig.ctx.automationTracks == std::vector<int>{0});
}
