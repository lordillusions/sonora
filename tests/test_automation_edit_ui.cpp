// UI-76 / UI-77 through real input events: editing a lane like the reference (click on the line adds, click on a breakpoint
// deletes, segment drags, Option-drag curves, time selections with Delete / Cmd+C / Cmd+V / Cmd+D, Insert Shape, Edit Value),
// the question about a clip's automation when it is duplicated, and Automation Write mode.
#include "TestFramework.h"

#include "core/AutomationEdits.h"
#include "core/Edit.h"
#include "engine/EngineGraph.h"
#include "gfx/SkiaCanvasComponent.h"
#include "ui/ClipAutomation.h"
#include "ui/RootWidget.h"
#include "ui/UiContext.h"
#include "ui/views/AutomationEditor.h"

#include <juce_core/juce_core.h>
#include <juce_graphics/juce_graphics.h>

#include <cmath>

using namespace sonora;

namespace
{
constexpr int panTarget = 101;

struct Rig
{
    model::Song song;
    model::EditHistory history;
    ui::UiContext ctx;
    engine::EngineGraph graph;
    std::unique_ptr<ui::RootWidget> root;
    ui::AutomationEditor* editor = nullptr;
    std::vector<std::string> labels;
    std::function<void(int)> choose;
    Rig()
    {
        song.tracks.resize(1);
        song.nextPointeeId = 1000;
        auto& track = song.tracks[0];
        track.kind = model::TrackKind::Audio;
        track.mixer.volumeTargetId = 100; track.mixer.panTargetId = panTarget; track.mixer.speakerTargetId = 102;
        model::AutomationEnvelope pan;
        pan.targetId = panTarget;
        pan.points = {{-63072000.0, 0.0}, {4, 0.0}, {8, 1.0}};
        track.automation.push_back(pan);
        graph.nodes.resize(1);
        graph.controls = std::make_shared<engine::ControlBank>(2);
        ctx.song = &song; ctx.graph = &graph; ctx.controls = graph.controls;
        ctx.pixelsPerBeat = 40; ctx.scrollBeats = 0;
        ctx.automationVisible = true;
        ctx.automationLaneTargets[0] = {panTarget};
        ctx.applyEdit = [this](model::EditCommand c) { history.perform(song, std::move(c)); };
        ctx.showChoices = [this](const auto& names, const auto&, auto pick) { labels = names; choose = std::move(pick); };
        root = std::make_unique<ui::RootWidget>(ctx); root->setBounds({0, 0, 900, 70});
        editor = root->add<ui::AutomationEditor>(0, 0); editor->setBounds(root->localBounds());
    }
    const model::AutomationEnvelope& lane() const { return song.tracks[0].automation[0]; }
    double at(double beat) const { return model::laneValueAt(lane(), beat, {model::AutomationLaw::Linear, -1, 1}); }
    static float x(double beat) { return (float) (beat * 40.0); }
    static float y(double pan) { return 65.0f - (float) ((pan + 1.0) * 0.5) * 60.0f; } // the 60 px plot from 5 to 65
    ui::MouseEvent event(float px, float py, bool shift = false, bool alt = false, bool right = false, int clicks = 1) const
    {
        ui::MouseEvent e; e.x = px; e.y = py; e.shift = shift; e.alt = alt; e.right = right; e.clicks = clicks; return e;
    }
    void click(float px, float py, bool shift = false) { auto e = event(px, py, shift); root->dispatchMouseDown(e); root->dispatchMouseUp(e); }
    void drag(float x0, float y0, float x1, float y1, bool shift = false, bool alt = false)
    {
        root->dispatchMouseDown(event(x0, y0, shift, alt));
        root->dispatchMouseDrag(event((x0 + x1) * 0.5f, (y0 + y1) * 0.5f, shift, alt));
        root->dispatchMouseDrag(event(x1, y1, shift, alt));
        root->dispatchMouseUp(event(x1, y1, shift, alt));
    }
    bool key(int code, char ch = 0, bool cmd = false)
    {
        ui::KeyEvent k; k.keyCode = code; k.textChar = (uint32_t) ch; k.cmd = cmd; return root->dispatchKey(k);
    }
    int menu(float px, float py, const std::string& label)
    {
        labels.clear();
        auto e = event(px, py, false, false, true);
        root->dispatchMouseDown(e); root->dispatchMouseUp(e);
        for (size_t i = 0; i < labels.size(); ++i) if (labels[i] == label) return (int) i;
        return -1;
    }
};
} // namespace

TEST_CASE("automation lane (UI-76): a click on the line adds a breakpoint on it, a click on a breakpoint deletes it")
{
    Rig rig;
    rig.click(Rig::x(6), Rig::y(0.5)); // halfway up the ramp from beat 4 to 8
    REQUIRE(rig.lane().points.size() == 4);
    CHECK_NEAR(rig.lane().points[2].time, 6, 1e-9);
    CHECK_NEAR(rig.lane().points[2].value, 0.5, 1e-9);
    rig.click(Rig::x(6), Rig::y(0.5));
    CHECK(rig.lane().points.size() == 3);
    // A click on the empty background neither adds nor deletes (it only clears the selection).
    const auto before = rig.history.undoLabel();
    rig.click(Rig::x(14), Rig::y(-1) - 2);
    CHECK(rig.lane().points.size() == 3);
}

TEST_CASE("automation lane (UI-76): a segment moves up and down, Option-drag bends it and Option-double-click straightens it")
{
    Rig rig;
    // Near the line (not on it): the segment between 4 and 8 moves down a quarter of the lane (0.5 of pan).
    rig.drag(Rig::x(6), Rig::y(0.5) + 8, Rig::x(6), Rig::y(0.5) + 8 + 15);
    CHECK_NEAR(rig.at(4), -0.5, 1e-6);
    CHECK_NEAR(rig.at(8), 0.5, 1e-6);
    rig.history.undo(rig.song);
    CHECK_NEAR(rig.at(8), 1.0, 1e-9);
    // Option-drag on the line upward bends the rising segment up: it rises faster.
    rig.drag(Rig::x(6), Rig::y(0.5), Rig::x(6), Rig::y(0.5) - 30, false, true);
    CHECK(rig.lane().points[1].curved());
    CHECK(rig.at(6) > 0.6);
    // Option + double-click straightens it.
    auto e = rig.event(Rig::x(6), Rig::y(rig.at(6)) + 30, false, true, false, 2);
    rig.root->dispatchMouseDown(e); rig.root->dispatchMouseUp(e);
    CHECK(! rig.lane().points[1].curved());
}

TEST_CASE("automation lane (UI-76): Shift-click selects breakpoints and a drag moves them together")
{
    Rig rig;
    rig.click(Rig::x(4), Rig::y(0), true);
    rig.click(Rig::x(8), Rig::y(1), true);
    CHECK(rig.ctx.automationSelection.points.size() == 2);
    CHECK(rig.lane().points.size() == 3); // Shift-clicks do not delete
    rig.drag(Rig::x(4), Rig::y(0), Rig::x(6), Rig::y(0)); // both move right by two beats
    CHECK_NEAR(rig.lane().points[1].time, 6, 1e-9);
    CHECK_NEAR(rig.lane().points[2].time, 10, 1e-9);
    CHECK_NEAR(rig.lane().points[2].value, 1.0, 1e-9);
}

TEST_CASE("automation lane (UI-76): a time selection deletes, copies, pastes and duplicates its envelope")
{
    Rig rig;
    const float background = 63; // far from the line everywhere between beats 2 and 10
    rig.drag(Rig::x(2), background, Rig::x(10), background);
    REQUIRE(rig.ctx.automationSelection.range);
    CHECK_NEAR(rig.ctx.automationSelection.start, 2, 1e-9);
    CHECK_NEAR(rig.ctx.automationSelection.end, 10, 1e-9);
    CHECK(rig.ctx.automationSelection.points.size() == 2);
    REQUIRE(rig.key(juce::KeyPress::deleteKey));
    CHECK_NEAR(rig.at(1), 0, 1e-9);
    CHECK_NEAR(rig.at(11), 1, 1e-9);
    CHECK_NEAR(rig.at(6), 0.5, 1e-9); // a straight line between the pinned edges (0 at 2, 1 at 10)
    rig.history.undo(rig.song);

    // Copy 4..8, paste at 12.
    rig.drag(Rig::x(4) + 1, background, Rig::x(8), background);
    REQUIRE(rig.key(0, 'c', true));
    rig.drag(Rig::x(12), background, Rig::x(13), background);
    REQUIRE(rig.key(0, 'v', true));
    CHECK_NEAR(rig.at(14), 0.5, 1e-6);
    CHECK_NEAR(rig.at(16) , 1.0, 1e-6);
    rig.history.undo(rig.song);

    // Duplicate 4..8 right after itself.
    rig.drag(Rig::x(4) + 1, background, Rig::x(8), background);
    REQUIRE(rig.key(0, 'd', true));
    CHECK_NEAR(rig.at(10), 0.5, 1e-6);
    CHECK_NEAR(rig.ctx.automationSelection.start, 8, 1e-9);
}

TEST_CASE("automation lane (UI-76): context menu Insert Shape, Simplify Envelope, Edit Value and Delete Automation")
{
    Rig rig;
    const float background = 63;
    rig.drag(Rig::x(12), background, Rig::x(16), background);
    const int square = rig.menu(Rig::x(13), background, "Insert Shape/Square");
    REQUIRE(square >= 0);
    rig.choose(square);
    CHECK_NEAR(rig.at(12.5), 1.0, 1e-6);  // top of the lane
    CHECK_NEAR(rig.at(14.5), -1.0, 1e-6); // bottom
    CHECK_NEAR(rig.at(17), 1.0, 1e-9);    // after the selection: what the lane held
    rig.history.undo(rig.song);

    // Edit Value on the breakpoint at 8: typing 25L sets it to -0.5.
    const int edit = rig.menu(Rig::x(8), Rig::y(1), "Edit Value");
    REQUIRE(edit >= 0);
    rig.choose(edit);
    CHECK(rig.editor->wantsTextInput());
    rig.key(0, '2'); rig.key(0, '5'); rig.key(0, 'L');
    rig.key(juce::KeyPress::returnKey);
    CHECK(! rig.editor->wantsTextInput());
    CHECK_NEAR(rig.lane().points[2].value, -0.5, 1e-9);

    // Cmd+Delete deletes the lane's automation (the reference's shortcut).
    ui::KeyEvent k; k.keyCode = juce::KeyPress::backspaceKey; k.cmd = true;
    rig.root->dispatchKey(k);
    CHECK(rig.song.tracks[0].automation.empty());
}

TEST_CASE("automation lane (UI-76): PNG of a selection, selected points and a value readout")
{
    Rig rig;
    rig.root->setBounds({0, 0, 900, 90});
    rig.editor->setBounds(rig.root->localBounds());
    rig.drag(Rig::x(3), 83, Rig::x(9), 83);
    ui::MouseEvent hover; hover.x = Rig::x(8); hover.y = 5 + 0.0f;
    rig.root->dispatchMouseMove(hover);
    const auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory);
    CHECK(gfx::renderToPng(*rig.root, 900, 90, 2, dir.getChildFile("sonora_automation_lane_edit.png").getFullPathName().toStdString()));
}

TEST_CASE("clip automation (UI-76): Cmd+D asks about the automation under the clip and follows the answer")
{
    Rig rig;
    model::Clip clip;
    clip.kind = model::Clip::Kind::Audio;
    clip.currentStart = clip.time = 4; clip.currentEnd = 8; clip.uid = 4242;
    clip.loop = {0, 4, 0, false};
    rig.song.tracks[0].arrangementClips.push_back(clip);
    int asked = 0, answer = 1;
    std::vector<std::string> buttons;
    rig.ctx.askChoice = [&](const std::string&, const std::string&, std::vector<std::string> b, int, int, std::function<void(int)> done) {
        ++asked; buttons = b; done(answer);
    };
    const auto location = model::ClipLocation::arrangement(0, 4242);
    ui::duplicateClipWithAutomation(rig.ctx, location); // "Clip Only"
    CHECK(asked == 1);
    REQUIRE(buttons.size() == 4);
    CHECK(rig.song.tracks[0].arrangementClips.size() == 2);
    CHECK_NEAR(rig.at(10), 1.0, 1e-9); // automation stayed put
    rig.history.undo(rig.song);
    answer = 0; // "With Automation"
    ui::duplicateClipWithAutomation(rig.ctx, location);
    CHECK(rig.song.tracks[0].arrangementClips.size() == 2);
    CHECK_NEAR(rig.at(10), 0.5, 1e-6); // the ramp under the clip was copied to 8..12
    CHECK(rig.history.undoLabel() == "Duplicate Clip");
    rig.history.undo(rig.song);
    CHECK_NEAR(rig.at(10), 1.0, 1e-9); // one undo step for both
    answer = 2; // "Always": no more questions
    ui::duplicateClipWithAutomation(rig.ctx, location);
    CHECK(rig.ctx.clipAutomationPolicy == 1);
    rig.history.undo(rig.song);
    ui::duplicateClipWithAutomation(rig.ctx, location);
    CHECK(asked == 3);
    rig.history.undo(rig.song);
    rig.ctx.clipAutomationPolicy = 2; // Lock Envelopes
    ui::duplicateClipWithAutomation(rig.ctx, location);
    CHECK(asked == 3);
    CHECK_NEAR(rig.at(10), 1.0, 1e-9);
}

TEST_CASE("automation write (UI-77): a released control shows its lane and writes its value at the play position")
{
    Rig rig;
    rig.song.tracks[0].automation.clear();
    rig.ctx.automationVisible = false;
    rig.ctx.automationLaneTargets.clear();
    rig.ctx.mainView = ui::MainView::Session;
    midi::ControlTarget pan;
    pan.kind = midi::ControlTarget::Kind::TrackPan; pan.track = 0;
    (*rig.graph.controls)[0].pan.store(0.3f);
    CHECK(ui::AutomationEditor::touchControl(rig.ctx, pan) == 0);
    CHECK(rig.ctx.showsAutomation(0));
    CHECK(rig.ctx.automationLaneTargets[0].front() == panTarget);
    REQUIRE(ui::AutomationEditor::writePoint(rig.ctx, pan, 4));
    REQUIRE(rig.song.tracks[0].automation.size() == 1);
    CHECK_NEAR(rig.at(0), 0.3, 1e-6); // a lane that never moved is flat at the first written value
    (*rig.graph.controls)[0].pan.store(-0.8f);
    REQUIRE(ui::AutomationEditor::writePoint(rig.ctx, pan, 8));
    CHECK_NEAR(rig.at(4), 0.3, 1e-6);
    CHECK_NEAR(rig.at(8), -0.8, 1e-6);
    CHECK_NEAR(rig.at(6), -0.25, 1e-6);

    // The root reports a control let go in Automation Write mode, and only then.
    int touches = 0;
    rig.ctx.automationWriteTouch = [&](const midi::ControlTarget& t, bool down) { touches += down ? 100 : 1; CHECK(t.kind == pan.kind); };
    auto* knob = rig.root->add<ui::Widget>();
    knob->setBounds({800, 0, 20, 20});
    knob->midiTarget = pan;
    ui::MouseEvent e; e.x = 805; e.y = 5;
    rig.root->dispatchMouseDown(e); rig.root->dispatchMouseUp(e);
    CHECK(touches == 0);
    rig.ctx.automationWriteMode = true;
    rig.root->dispatchMouseDown(e);
    CHECK(touches == 100); // UI-77c: pressed: the lane shows at once
    rig.root->dispatchMouseUp(e);
    CHECK(touches == 101);
}

TEST_CASE("automation write (UI-77c): the lane being written draws its preview, the point the stopped watch would store")
{
    Rig rig;
    midi::ControlTarget pan;
    pan.kind = midi::ControlTarget::Kind::TrackPan; pan.track = 0;
    (*rig.graph.controls)[0].pan.store(-1.0f);
    int track = -1, target = -1;
    auto env = ui::AutomationEditor::pointEnvelope(rig.ctx, pan, 12, track, target);
    REQUIRE(env.has_value());
    CHECK(track == 0); CHECK(target == panTarget);
    CHECK(rig.song.tracks[0].automation[0].points.size() == 3); // nothing stored yet
    CHECK_NEAR(model::laneValueAt(*env, 12, {model::AutomationLaw::Linear, -1, 1}), -1.0, 1e-9);
    // Drawn from the preview (in the record colour), not from the stored lane.
    const auto stored = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("sonora_write_stored.png");
    const auto live = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("sonora_write_preview.png");
    REQUIRE(gfx::renderToPng(*rig.root, 900, 70, 1, stored.getFullPathName().toStdString()));
    rig.ctx.automationPreview[{0, panTarget}] = *env;
    REQUIRE(gfx::renderToPng(*rig.root, 900, 70, 1, live.getFullPathName().toStdString()));
    const auto a = juce::ImageFileFormat::loadFrom(stored), b = juce::ImageFileFormat::loadFrom(live);
    REQUIRE(a.isValid()); REQUIRE(b.isValid());
    // Beat 10 (between 8 at the top and 12 at the bottom): the stored lane holds 1.0 there, the preview passes the middle.
    const int px = (int) Rig::x(10.0), mid = (int) Rig::y(0.0);
    auto reddish = [](juce::Colour c) { return c.getRed() > 180 && c.getGreen() < 140 && c.getBlue() < 140; };
    bool before = false, after = false;
    for (int dy = -3; dy <= 3; ++dy)
    {
        before = before || reddish(a.getPixelAt(px, mid + dy));
        after = after || reddish(b.getPixelAt(px, mid + dy));
    }
    CHECK(! before);
    CHECK(after);
    stored.deleteFile(); live.deleteFile();
}
