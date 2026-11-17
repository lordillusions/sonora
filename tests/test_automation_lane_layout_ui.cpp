// UI-92: lane edits made inside the whole window (MainLayout > Arrangement > the lane under the track), not on a lone lane,
// reach the edit history, so the set counts as edited (the window title's "*", the save prompt, the autosave).
#include "TestFramework.h"

#include "core/Edit.h"
#include "io/als/AlsReader.h"
#include "ui/views/ArrangementView.h"
#include "ui/views/AutomationEditor.h"
#include "ui/views/MainLayout.h"

#include <juce_core/juce_core.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <cmath>

using namespace sonora;

namespace
{
template <typename T>
void findWidgets(ui::Widget& w, std::vector<T*>& out)
{
    if (auto* hit = dynamic_cast<T*>(&w)) out.push_back(hit);
    for (auto& child : w.children()) findWidgets<T>(*child, out);
}

struct Rig
{
    model::Song song;
    model::EditHistory history;
    ui::UiContext ctx;
    std::unique_ptr<ui::MainLayout> root;
    int target = -1;
    Rig()
    {
        auto loaded = io::loadAls(juce::File(SONORA_TEST_FIXTURES).getChildFile("two_tracks.xml"));
        REQUIRE(loaded);
        song = *loaded.song;
        REQUIRE(! song.tracks.empty());
        target = song.tracks[0].mixer.volumeTargetId;
        REQUIRE(target >= 0);
        model::AutomationEnvelope volume;
        volume.targetId = target;
        volume.points = {{-63072000.0, 0.5}, {0, 0.5}, {8, 0.9}};
        song.tracks[0].automation = {volume};
        ctx.song = &song;
        ctx.ioVisible = ctx.browserVisible = ctx.detailVisible = false;
        ctx.mainView = ui::MainView::Arrangement;
        ctx.applyEdit = [this](model::EditCommand c) { history.perform(song, std::move(c)); };
        root = std::make_unique<ui::MainLayout>(ctx);
        root->setBounds({0, 0, 1200, 700});
        root->layout();
        ctx.toggleAutomationLane(0);
        ctx.automationLaneTargets[0] = {target};
        root->layout();
    }
    ui::AutomationEditor* lane()
    {
        std::vector<ui::AutomationEditor*> editors;
        findWidgets(*root, editors);
        for (auto* e : editors) if (e->bounds().w > 0 && e->bounds().h > 0) return e;
        return nullptr;
    }
    ui::MouseEvent at(float x, float y) const { ui::MouseEvent e; e.x = x; e.y = y; e.clicks = 1; return e; }
    size_t points() const { return song.tracks[0].automation.empty() ? 0 : song.tracks[0].automation[0].points.size(); }
};
} // namespace

TEST_CASE("automation lane in the window (UI-92): a click on the line and a breakpoint drag are edits the set remembers")
{
    Rig rig;
    auto* lane = rig.lane();
    REQUIRE(lane != nullptr);
    const auto r = lane->boundsInRoot();
    REQUIRE(r.w > 100 && r.h > 20);
    CHECK(! rig.history.isDirty());

    // Find a point on the line: scan the lane's height at one x until a click adds a breakpoint.
    const size_t before = rig.points();
    const float x = r.x + r.w * 0.25f;
    bool added = false;
    for (float y = r.y + 2; y < r.bottom() - 2 && ! added; y += 1.0f)
    {
        const auto e = rig.at(x, y);
        rig.root->dispatchMouseDown(e);
        rig.root->dispatchMouseUp(e);
        added = rig.points() == before + 1;
        if (! added && rig.points() != before) break; // something else changed: the scan would only confuse the check
    }
    CHECK(added);
    CHECK(rig.history.isDirty());
    CHECK(rig.history.canUndo());

    // Drag the whole lane's middle breakpoint area: whatever the gesture, a release after a move commits an edit.
    rig.history.markSaved();
    const auto envelopeBefore = rig.song.tracks[0].automation[0].points;
    const float y0 = r.y + r.h * 0.5f;
    rig.root->dispatchMouseDown(rig.at(x, y0));
    rig.root->dispatchMouseDrag(rig.at(x + 10, y0 - 10));
    rig.root->dispatchMouseDrag(rig.at(x + 20, y0 - 20));
    rig.root->dispatchMouseUp(rig.at(x + 20, y0 - 20));
    const auto& after = rig.song.tracks[0].automation[0].points;
    const bool changed = after.size() != envelopeBefore.size()
                         || ! std::equal(after.begin(), after.end(), envelopeBefore.begin(),
                                         [](const auto& a, const auto& b) { return std::abs(a.time - b.time) < 1e-12 && std::abs(a.value - b.value) < 1e-12; });
    CHECK(changed == rig.history.isDirty()); // never a lane that moved without an edit behind it
}
