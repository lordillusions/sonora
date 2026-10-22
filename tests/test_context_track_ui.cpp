#include "TestFramework.h"
#include "io/als/AlsReader.h"
#include "io/als/AlsNewSet.h"
#include "io/als/AlsWriter.h"
#include "io/als/AlsTemplates.h"
#include "ui/GridMenu.h"
#include "ui/TrackMenu.h"
#include "engine/EngineGraph.h"
#include "engine/Transport.h"
#include "ui/widgets/Buttons.h"
#include "ui/views/ArrangementView.h"
#include "ui/views/SessionView.h"
#include "ui/views/MainLayout.h"
#include "gfx/SkiaCanvasComponent.h"

using namespace sonora;
namespace
{
template <class T> T* find(ui::Widget& widget)
{
    if (auto* match = dynamic_cast<T*>(&widget)) return match;
    for (const auto& child : widget.children()) if (auto* match = find<T>(*child)) return match;
    return nullptr;
}
struct MenuRig
{
    std::unique_ptr<model::Song> song;
    model::EditHistory history;
    ui::UiContext ctx;
    std::unique_ptr<ui::MainLayout> root;
    ui::ArrangementView* arrangement = nullptr;
    MenuRig()
    {
        std::string error;
        auto dom = io::createNewSetDom();
        juce::TemporaryFile file(".als");
        REQUIRE(io::writeAlsDom(*dom, file.getFile(), error));
        auto loaded = io::loadAls(file.getFile()); REQUIRE(loaded); song = std::move(loaded.song);
        song->midiClipTemplate = std::make_shared<const std::string>(io::midiClipTemplate());
        ctx.song = song.get(); ctx.browserVisible = ctx.detailVisible = false;
        ctx.applyEdit = [&](model::EditCommand cmd) { history.perform(*song, std::move(cmd)); };
        root = std::make_unique<ui::MainLayout>(ctx); root->setBounds({0, 0, 1300, 800}); root->layout();
        arrangement = find<ui::ArrangementView>(*root); REQUIRE(arrangement);
    }
    ui::MenuAction action(const std::vector<ui::MenuAction>& actions, const char* name)
    {
        for (const auto& item : actions) if (item.label == name) return item;
        REQUIRE(false); return {};
    }
};
}

TEST_CASE("track context deletion: MIDI audio returns mixed selections Main and recording guards")
{
    for (const int selected : {0, 2, 4})
    {
        MenuRig rig; const auto tracks = rig.song->tracks.size(), returns = rig.song->returns.size();
        rig.ctx.selectedTrack = selected;
        auto item = rig.action(ui::trackContextActions(rig.ctx), "Delete"); REQUIRE(item.enabled); REQUIRE(item.run);
        item.run();
        CHECK(rig.song->tracks.size() == tracks - (selected < 4 ? 1 : 0));
        CHECK(rig.song->returns.size() == returns - (selected == 4 ? 1 : 0));
        CHECK(rig.history.canUndo()); REQUIRE(rig.history.undo(*rig.song));
        CHECK(rig.song->tracks.size() == tracks); CHECK(rig.song->returns.size() == returns);
    }
    MenuRig rig;
    rig.ctx.selectedTrack = 4; rig.ctx.trackSelection = {0, 2, 4, 5};
    auto action = rig.action(ui::trackContextActions(rig.ctx), "Delete"); REQUIRE(action.enabled); action.run();
    CHECK(rig.song->tracks.size() == 2); CHECK(rig.song->returns.empty());
    REQUIRE(rig.history.undo(*rig.song)); rig.ctx.selectTrack(6, false, false);
    CHECK(! rig.action(ui::trackContextActions(rig.ctx), "Delete").enabled);
    rig.ctx.selectTrack(0, false, false); action = rig.action(ui::trackContextActions(rig.ctx), "Delete");
    rig.ctx.recordingRequested = true; action.run(); CHECK(rig.song->tracks.size() == 4);
    CHECK(! rig.action(ui::trackContextActions(rig.ctx), "Delete").enabled);
    rig.ctx.recordingRequested = false; rig.song->tracks[0].id += 10000; action.run(); CHECK(rig.song->tracks.size() == 4);
}

TEST_CASE("track context deletion: header Delete works in both views without deleting a focused clip")
{
    for (const auto view : {ui::MainView::Arrangement, ui::MainView::Session})
    {
        MenuRig rig; rig.ctx.mainView = view; rig.ctx.sessionTrackSelection = true; rig.ctx.selectedTrack = 4; rig.root->layout();
        rig.root->setKeyboardFocus(view == ui::MainView::Arrangement ? (ui::Widget*) rig.arrangement : (ui::Widget*) find<ui::SessionView>(*rig.root));
        ui::KeyEvent key; key.keyCode = juce::KeyPress::backspaceKey;
        REQUIRE(rig.root->dispatchKey(key)); CHECK(rig.song->returns.size() == 1);
    }
}

TEST_CASE("arrangement context: reference order zoom selection insert loop and disabled render commands")
{
    MenuRig rig;
    rig.ctx.arrangementSelection = {true, 4, 12, 0, 1};
    const auto lane = rig.arrangement->lanes()[0];
    ui::MouseEvent point; point.x = 60; point.y = lane.y + 25;
    auto actions = rig.arrangement->gridContextActions(point);
    CHECK(actions[0].label == "Zoom to Time Selection"); CHECK(actions[2].label == "Insert Empty MIDI Clip");
    CHECK(actions[4].label == "Activate Loop");
    CHECK(! rig.action(actions, "Freeze Track").enabled);
    CHECK(! rig.action(actions, "Bounce Track in Place").enabled);
    CHECK(! rig.action(actions, "Bounce to New Track").enabled);
    const double zoom = rig.ctx.pixelsPerBeat;
    rig.action(actions, "Zoom to Time Selection").run(); CHECK(rig.ctx.pixelsPerBeat > zoom); CHECK_NEAR(rig.ctx.scrollBeats, 4, 1e-9);
    actions = rig.arrangement->gridContextActions(point);
    CHECK(actions[0].label == "Zoom Back from Time Selection"); CHECK(actions[0].shortcut == "X");
    actions[0].run(); CHECK_NEAR(rig.ctx.pixelsPerBeat, zoom, 1e-9);
    rig.action(actions, "Insert Empty MIDI Clip").run();
    REQUIRE(rig.song->tracks[0].arrangementClips.size() == 1);
    CHECK_NEAR(rig.song->tracks[0].arrangementClips[0].currentStart, 4, 1e-9);
    CHECK_NEAR(rig.song->tracks[0].arrangementClips[0].currentEnd, 12, 1e-9);
    rig.action(actions, "Activate Loop").run(); CHECK(rig.song->loopOn); CHECK_NEAR(rig.song->loopStart, 4, 1e-9); CHECK_NEAR(rig.song->loopLength, 8, 1e-9);
    actions = rig.arrangement->gridContextActions(point);
    rig.action(actions, "Deactivate Loop").run(); CHECK(! rig.song->loopOn);
    actions = rig.arrangement->gridContextActions(point); rig.ctx.recording = true;
    rig.action(actions, "Insert Empty MIDI Clip").run(); CHECK(rig.song->tracks[0].arrangementClips.size() == 1);
}

TEST_CASE("grid context: inline choice tables five adaptive densities fixed cells and shortcut action state")
{
    juce::ScopedJuceInitialiser_GUI gui;
    ui::GridSettings settings; int changed = 0;
    auto menu = ui::createGridMenu(settings, 4, [&] { ++changed; });
    std::vector<ui::GridChoiceTable*> tables;
    juce::PopupMenu::MenuItemIterator iterator(menu);
    while (iterator.next())
    {
        auto& item = iterator.getItem(); CHECK(! item.subMenu);
        if (auto* table = dynamic_cast<ui::GridChoiceTable*>(item.customComponent.get())) tables.push_back(table);
        if (item.text == "Snap to Grid") { CHECK(item.shortcutKeyDescription == juce::String::fromUTF8("⌘4")); item.action(); CHECK(! settings.snap); }
    }
    REQUIRE(tables.size() == 2);
    int width = 0, height = 0; tables[0]->getIdealSize(width, height); tables[0]->setSize(width, height);
    tables[0]->choose(0); CHECK(settings.density == ui::GridSettings::Density::Widest); CHECK(! settings.fixed);
    tables[0]->choose(4); CHECK(settings.density == ui::GridSettings::Density::Narrowest);
    tables[1]->choose(5); CHECK(settings.fixed); CHECK(settings.fixedIndex == 5); CHECK(changed == 4);
    for (const auto density : {ui::GridSettings::Density::Widest, ui::GridSettings::Density::Wide, ui::GridSettings::Density::Medium, ui::GridSettings::Density::Narrow, ui::GridSettings::Density::Narrowest})
    {
        settings.density = density;
        auto restored = ui::decodeGrid(ui::encodeGrid(settings));
        CHECK(restored.density == density); CHECK(restored.fixedIndex == 5); CHECK(restored.fixed);
    }
    const auto image = ui::renderGridMenuPreview(settings, 4, ui::Theme{}); CHECK(image.isValid());
}

TEST_CASE("media menus: render conversion consolidation actions and global shortcuts call shared commands")
{
    MenuRig rig; int rendering = -1, conversion = -1, consolidation = 0; bool freezing = false;
    rig.ctx.renderTrack = [&](int track, bool freeze, bool) { rendering = track; freezing = freeze; };
    rig.ctx.convertAudioToMidi = [&](const auto&, int mode) { conversion = mode; };
    rig.ctx.consolidateSelection = [&] { ++consolidation; };
    rig.ctx.selectedTrack = 2;
    auto actions = ui::trackContextActions(rig.ctx);
    auto freeze = rig.action(actions, "Freeze Track"); REQUIRE(freeze.enabled); freeze.run(); CHECK(rendering == 2); CHECK(freezing);
    auto bounce = rig.action(actions, "Bounce Track in Place"); REQUIRE(bounce.enabled); bounce.run(); CHECK(! freezing);
    model::Clip clip; clip.kind = model::Clip::Kind::Audio; clip.currentStart = 0; clip.currentEnd = 4;
    clip.sourceXml = std::make_shared<const std::string>(io::audioClipTemplate(clip));
    model::insertClip(2, clip, "Audio").apply(*rig.song);
    actions = ui::clipContextActions(rig.ctx, model::ClipLocation::arrangement(2, rig.song->tracks[2].arrangementClips[0].uid));
    for (const auto* name : {"Convert Melody to New MIDI Track", "Convert Harmony to New MIDI Track", "Convert Drums to New MIDI Track", "Slice to New MIDI Track"})
    { auto action = rig.action(actions, name); REQUIRE(action.enabled); action.run(); }
    CHECK(conversion == 3);
    auto merge = rig.action(actions, "Consolidate"); REQUIRE(merge.enabled); merge.run(); CHECK(consolidation == 1);
    rig.ctx.mainView = ui::MainView::Arrangement; rig.ctx.clipEditorFocused = false;
    ui::KeyEvent key; key.cmd = true; key.textChar = 'j'; key.keyCode = 'J'; REQUIRE(rig.root->dispatchKey(key)); CHECK(consolidation == 2);
    rig.ctx.clipEditorFocused = true; rig.root->dispatchKey(key); CHECK(consolidation == 2);
    rig.song->tracks[2].frozen = true;
    CHECK(rig.action(ui::trackContextActions(rig.ctx), "Unfreeze Track").enabled);
}
TEST_CASE("audio gestures: fade diamond drags, title-bar edge resize cursor and Option/Shift edge stretch are undoable")
{
    MenuRig rig; rig.ctx.mainView = ui::MainView::Arrangement; rig.ctx.pixelsPerBeat = 100;
    rig.ctx.laneHeights.resize(7, 100); rig.root->layout();
    model::Clip clip; clip.kind = model::Clip::Kind::Audio; clip.currentStart = 1; clip.currentEnd = 5;
    clip.sampleRate = 44100; clip.lengthSamples = 88200; clip.loop = {0, 2, 0, false}; clip.fadeOut = 2;
    clip.sourceXml = std::make_shared<const std::string>(io::audioClipTemplate(clip));
    model::insertClip(2, clip, "Audio").apply(*rig.song); rig.root->layout();
    const auto lane = rig.arrangement->lanes()[2];
    CHECK(rig.arrangement->cursorAt(493, lane.y + 6) == ui::Cursor::ResizeHorizontal); // UI-45: edges in the title bar
    CHECK(rig.arrangement->cursorAt(493, lane.y + 35) != ui::Cursor::ResizeHorizontal); // the body selects time
    // The diamond is halfway through a two-beat fade, at the displayed curve's gain.
    ui::MouseEvent e; e.x = 400; e.y = lane.y + 13 + (lane.h - 15) * 0.25f;
    rig.arrangement->mouseDown(e); e.y += 20; rig.arrangement->mouseDrag(e); rig.arrangement->mouseUp(e);
    CHECK(rig.song->tracks[2].arrangementClips[0].fadeOutCurve > 0.1); REQUIRE(rig.history.undo(*rig.song));
    e.x = 497; e.y = lane.y + 6; e.alt = true;
    rig.arrangement->mouseDown(e); e.x += 200; rig.arrangement->mouseDrag(e); rig.arrangement->mouseUp(e);
    CHECK_NEAR(rig.song->tracks[2].arrangementClips[0].currentEnd, 7, 1e-9);
    CHECK(rig.song->tracks[2].arrangementClips[0].warped); CHECK_NEAR(rig.ctx.laneHeights[2], 100, 1e-9);
    REQUIRE(rig.history.undo(*rig.song)); CHECK_NEAR(rig.song->tracks[2].arrangementClips[0].currentEnd, 5, 1e-9);
    // Shift on the title-bar edge stretches too; the same drag in the body only selects time.
    e.x = 497; e.y = lane.y + 6; e.alt = false; e.shift = true;
    rig.arrangement->mouseDown(e); e.x += 100; rig.arrangement->mouseDrag(e); rig.arrangement->mouseUp(e);
    CHECK_NEAR(rig.song->tracks[2].arrangementClips[0].currentEnd, 6, 1e-9);
    REQUIRE(rig.history.undo(*rig.song));
    e.x = 497; e.y = lane.y + 60; e.shift = false;
    rig.arrangement->mouseDown(e); e.x += 100; rig.arrangement->mouseDrag(e); rig.arrangement->mouseUp(e);
    CHECK_NEAR(rig.song->tracks[2].arrangementClips[0].currentEnd, 5, 1e-9);
    CHECK(rig.ctx.arrangementSelection.active);
}

TEST_CASE("media jobs: modal processing refuses Finder document and sample drops")
{
    MenuRig rig; int opened = 0; rig.ctx.openFile = [&](const auto&) { ++opened; };
    rig.ctx.exportOpen = true;
    CHECK(! rig.root->dispatchFileDragMove(100, 100, {"/tmp/Sonora.wav"})); CHECK(! rig.ctx.browserDrag);
    CHECK(! rig.root->dispatchFileDrop({}, {"/tmp/Sonora.als"})); CHECK(opened == 0);
    rig.ctx.exportOpen = false; CHECK(rig.root->dispatchFileDrop({}, {"/tmp/Sonora.als"})); CHECK(opened == 1);
}

TEST_CASE("arrangement split: Cmd E uses the clicked insertion cursor and a multi-track time selection")
{
    MenuRig rig; rig.ctx.mainView = ui::MainView::Arrangement; rig.ctx.pixelsPerBeat = 50;
    model::Clip clip; REQUIRE(model::makeEmptyMidiClip(*rig.song, 0, 16, clip));
    model::insertClip(0, clip, "MIDI").apply(*rig.song); model::insertClip(1, clip, "MIDI").apply(*rig.song); rig.root->layout();
    rig.root->setKeyboardFocus(rig.arrangement);
    ui::MouseEvent event; event.x = 300; event.y = rig.arrangement->lanes()[0].y + 30;
    rig.arrangement->mouseDown(event); rig.arrangement->mouseUp(event);
    ui::KeyEvent key; key.cmd = true; key.keyCode = 'E'; key.textChar = 'e';
    REQUIRE(rig.root->dispatchKey(key)); REQUIRE(rig.song->tracks[0].arrangementClips.size() == 2);
    CHECK_NEAR(rig.song->tracks[0].arrangementClips[1].currentStart, 6, 1e-9);
    REQUIRE(rig.history.undo(*rig.song));
    rig.ctx.arrangementSelection = {true, 4, 8, 0, 1}; REQUIRE(rig.root->dispatchKey(key));
    CHECK(rig.song->tracks[0].arrangementClips.size() == 3); CHECK(rig.song->tracks[1].arrangementClips.size() == 3);
    REQUIRE(rig.history.undo(*rig.song)); rig.ctx.recordingRequested = true; rig.root->dispatchKey(key);
    CHECK(rig.song->tracks[0].arrangementClips.size() == 1);
}

TEST_CASE("solo buttons: exclusive clicks command additive and the selected-track keyboard action agree")
{
    for (auto view : {ui::MainView::Arrangement, ui::MainView::Session})
    {
        MenuRig rig; rig.ctx.mainView = view;
        rig.ctx.controls = std::make_shared<engine::ControlBank>((size_t) rig.ctx.numControlTracks()); rig.root->layout();
        auto solo = [&](int index) {
            ui::TextButton* result = nullptr;
            std::function<void(ui::Widget&)> visit = [&](ui::Widget& widget) {
                if (auto* button = dynamic_cast<ui::TextButton*>(&widget); button && button->label() == "S" && button->midiTarget
                    && button->midiTarget->track == index) result = button;
                for (const auto& child : widget.children()) visit(*child);
            };
            visit(*(view == ui::MainView::Arrangement ? (ui::Widget*) rig.arrangement : (ui::Widget*) find<ui::SessionView>(*rig.root)));
            REQUIRE(result); return result;
        };
        auto clickSolo = [&](int index, bool additive = false) {
            auto* button = solo(index); ui::MouseEvent event; event.x = button->width() * 0.5f; event.y = button->height() * 0.5f; event.cmd = additive;
            button->mouseDown(event); button->mouseUp(event);
        };
        auto& bank = *rig.ctx.controls;
        clickSolo(0); CHECK(bank[0].solo.load()); clickSolo(1); CHECK(! bank[0].solo.load()); CHECK(bank[1].solo.load());
        clickSolo(2, true); CHECK(bank[1].solo.load()); CHECK(bank[2].solo.load());
        clickSolo(2); CHECK(! bank[1].solo.load()); CHECK(bank[2].solo.load());
        clickSolo(2); CHECK(! bank[2].solo.load());
        clickSolo(0); clickSolo(4, true); CHECK(bank[0].solo.load()); CHECK(bank[4].solo.load()); // return track
        clickSolo(4, true); CHECK(bank[0].solo.load()); CHECK(! bank[4].solo.load());
        rig.ctx.selectedTrack = 1; ui::KeyEvent key; key.textChar = 's'; key.keyCode = 'S';
        REQUIRE(rig.root->keyPressed(key)); CHECK(! bank[0].solo.load()); CHECK(bank[1].solo.load());
        ui::toggleTrackSolo(rig.ctx, rig.ctx.numTracks() + rig.ctx.numReturns()); CHECK(bank[1].solo.load());
    }
}

TEST_CASE("arrangement A key: shows the automation lane of the selected track only (UI-46)")
{
    MenuRig rig; rig.ctx.mainView = ui::MainView::Arrangement; rig.root->layout();
    const auto height = [&](int track) {
        for (const auto& lane : rig.arrangement->lanes()) if (lane.controlIndex == track && lane.takeLaneId < 0) return lane.h;
        return -1.0f;
    };
    const float plain0 = height(0), plain1 = height(1);
    rig.ctx.selectedTrack = 1;
    ui::KeyEvent a; a.textChar = 'a';
    REQUIRE(rig.arrangement->keyPressed(a));
    CHECK(rig.ctx.showsAutomation(1)); CHECK(! rig.ctx.showsAutomation(0));
    CHECK_NEAR(height(1), plain1 + ui::UiContext::automationLaneHeight, 1e-3);
    CHECK_NEAR(height(0), plain0, 1e-3);
    rig.ctx.selectedTrack = 0; REQUIRE(rig.arrangement->keyPressed(a)); // a second track joins
    CHECK(rig.ctx.showsAutomation(0)); CHECK(rig.ctx.showsAutomation(1));
    REQUIRE(rig.arrangement->keyPressed(a)); CHECK(! rig.ctx.showsAutomation(0)); CHECK(rig.ctx.showsAutomation(1));
    rig.ctx.selectedTrack = 1; REQUIRE(rig.arrangement->keyPressed(a));
    CHECK(! rig.ctx.automationVisible); CHECK_NEAR(height(1), plain1, 1e-3);
}

TEST_CASE("arrangement loop brace: resize cursor on its ends, move cursor on its body (UI-45)")
{
    MenuRig rig; rig.ctx.mainView = ui::MainView::Arrangement; rig.ctx.pixelsPerBeat = 20; rig.ctx.scrollBeats = 0;
    engine::Transport transport; transport.setLoop(true, 4, 8); transport.beginBlock();
    rig.ctx.transport = &transport; rig.root->layout();
    const float y = rig.ctx.theme.metrics.arrangementRulerHeight - 5; // the brace strip at the bottom of the ruler
    CHECK(rig.arrangement->cursorAt(4 * 20 + 1, y) == ui::Cursor::ResizeHorizontal);
    CHECK(rig.arrangement->cursorAt(12 * 20 - 1, y) == ui::Cursor::ResizeHorizontal);
    CHECK(rig.arrangement->cursorAt(8 * 20, y) == ui::Cursor::Move);
    CHECK(rig.arrangement->cursorAt(8 * 20, 10) == ui::Cursor::Default); // the numbers row scrubs
}

TEST_CASE("bounce to new track: an Arrangement time selection on the track limits the range")
{
    ui::UiContext ctx; ctx.mainView = ui::MainView::Arrangement;
    CHECK(! ctx.bounceSelection(1));
    ctx.arrangementSelection = {true, 2.5, 6, 0, 2};
    REQUIRE(ctx.bounceSelection(1)); CHECK_NEAR(ctx.bounceSelection(1)->first, 2.5, 1e-9); CHECK_NEAR(ctx.bounceSelection(1)->second, 6, 1e-9);
    CHECK(! ctx.bounceSelection(3));                                  // another track: whole track
    ctx.mainView = ui::MainView::Session; CHECK(! ctx.bounceSelection(1)); // Session view ignores it
}

TEST_CASE("arrangement clip move: dragging an audio clip onto a MIDI track says why it stays")
{
    MenuRig rig; rig.ctx.mainView = ui::MainView::Arrangement; rig.ctx.pixelsPerBeat = 40; rig.root->layout();
    int audio = -1, midi = -1;
    for (int i = 0; i < (int) rig.song->tracks.size(); ++i)
    {
        if (rig.song->tracks[(size_t) i].kind == model::TrackKind::Audio && audio < 0) audio = i;
        if (rig.song->tracks[(size_t) i].kind == model::TrackKind::Midi && midi < 0) midi = i;
    }
    REQUIRE(audio >= 0); REQUIRE(midi >= 0);
    model::Clip clip; clip.kind = model::Clip::Kind::Audio; clip.currentStart = 0; clip.currentEnd = 4;
    clip.sampleRate = 44100; clip.lengthSamples = 88200; clip.loop = {0, 2, 0, false};
    clip.sourceXml = std::make_shared<const std::string>(io::audioClipTemplate(clip));
    model::insertClip(audio, clip, "Audio").apply(*rig.song); rig.root->layout();
    const auto lanes = rig.arrangement->lanes();
    float audioY = 0, midiY = 0;
    for (const auto& lane : lanes) { if (lane.controlIndex == audio) audioY = lane.y; if (lane.controlIndex == midi) midiY = lane.y; }
    ui::MouseEvent e; e.x = 60; e.y = audioY + 6; // title bar
    rig.arrangement->mouseDown(e); e.y = midiY + 20; rig.arrangement->mouseDrag(e);
    CHECK(rig.ctx.statusText == "Audio clips can only move to audio tracks");
    rig.arrangement->mouseUp(e);
    CHECK(rig.song->tracks[(size_t) audio].arrangementClips.size() == 1);
    CHECK(rig.song->tracks[(size_t) midi].arrangementClips.empty());
}
