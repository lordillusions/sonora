// UI-3b: the Arrangement's locators: Set / Del, previous / next, ruler clicks, drags, keys, menus and drawing.
#include "TestFramework.h"
#include "engine/Transport.h"
#include "gfx/SkiaCanvasComponent.h"
#include "io/als/AlsNewSet.h"
#include "io/als/AlsReader.h"
#include "io/als/AlsWriter.h"
#include "ui/Locators.h"
#include "ui/Theme.h"
#include "ui/views/ArrangementView.h"
#include "ui/views/MainLayout.h"

#include <juce_graphics/juce_graphics.h>

using namespace sonora;
namespace
{
template <class T> T* find(ui::Widget& widget)
{
    if (auto* match = dynamic_cast<T*>(&widget)) return match;
    for (const auto& child : widget.children()) if (auto* match = find<T>(*child)) return match;
    return nullptr;
}

model::Locator locatorAt(double time, const char* name, int id, int colour = -1)
{
    model::Locator l;
    l.time = time; l.name = name; l.id = id; l.colorIndex = colour;
    return l;
}

struct Rig
{
    std::unique_ptr<model::Song> song;
    model::EditHistory history;
    engine::Transport transport;
    ui::UiContext ctx;
    std::unique_ptr<ui::MainLayout> root;
    ui::ArrangementView* view = nullptr;
    Rig()
    {
        song = io::parseAls(*io::createNewSetDom(), "/tmp/locators-ui.als").song;
        song->locators = {locatorAt(4, "Intro", 0), locatorAt(12, "Drop", 1, 14)};
        ctx.song = song.get(); ctx.transport = &transport; ctx.browserVisible = ctx.detailVisible = false;
        ctx.mainView = ui::MainView::Arrangement; ctx.pixelsPerBeat = 20; ctx.scrollBeats = 0;
        ctx.applyEdit = [this](model::EditCommand cmd) { history.perform(*song, std::move(cmd)); };
        root = std::make_unique<ui::MainLayout>(ctx); root->setBounds({0, 0, 1300, 800}); root->layout();
        view = find<ui::ArrangementView>(*root); REQUIRE(view);
    }
    static ui::MouseEvent mouse(float x, float y, int clicks = 1, bool right = false)
    {
        ui::MouseEvent e; e.x = x; e.y = y; e.clicks = clicks; e.right = right;
        return e;
    }
    void click(ui::Rect r) { auto e = mouse(r.x + r.w * 0.5f, r.y + r.h * 0.5f); view->mouseDown(e); view->mouseUp(e); }
    bool key(int code, bool cmd = false, int text = 0)
    {
        ui::KeyEvent k; k.keyCode = code; k.cmd = cmd; k.textChar = (uint32_t) text;
        return view->keyPressed(k);
    }
};
} // namespace

TEST_CASE("locators ui: Set adds at the insert marker and selects it; with a locator selected it reads Del and deletes")
{
    Rig rig;
    rig.ctx.arrangementCursor = 8;
    rig.click(rig.view->locatorButtonBounds(ui::ArrangementView::LocatorButton::Set));
    REQUIRE(rig.song->locators.size() == 3);
    CHECK_NEAR(rig.song->locators[1].time, 8, 1e-12); CHECK(rig.song->locators[1].name == "3");
    CHECK(rig.ctx.selectedLocator == rig.song->locators[1].id);
    CHECK(rig.history.undoLabel() == "Add Locator");
    rig.click(rig.view->locatorButtonBounds(ui::ArrangementView::LocatorButton::Set)); // now "Del"
    CHECK(rig.song->locators.size() == 2); CHECK(rig.ctx.selectedLocator == -1);
    CHECK(rig.history.undoLabel() == "Delete Locator");

    // A time selection's start wins over the insert marker; an existing locator there is selected, not duplicated.
    rig.ctx.arrangementSelection = {true, 12, 16, 0, 0};
    rig.click(rig.view->locatorButtonBounds(ui::ArrangementView::LocatorButton::Set));
    CHECK(rig.song->locators.size() == 2); CHECK(rig.ctx.selectedLocator == 1);

    // While playing, Set rounds the play position to the launch quantisation (1 bar here).
    rig.ctx.arrangementSelection = {}; rig.ctx.selectedLocator = -1;
    rig.song->globalQuantisation = 4;
    rig.transport.seekNow(21.3); rig.transport.play(); rig.transport.beginBlock();
    CHECK_NEAR(ui::locatorSetBeat(rig.ctx), 20, 1e-12);
    rig.song->globalQuantisation = 0;
    CHECK_NEAR(ui::locatorSetBeat(rig.ctx), 21.3, 1e-12);
}

TEST_CASE("locators ui: previous and next jump between locators, then to the Arrangement start and end")
{
    Rig rig;
    model::Clip clip; clip.kind = model::Clip::Kind::Midi; clip.currentStart = 0; clip.currentEnd = 40; clip.uid = model::allocateClipUid();
    rig.song->tracks[0].arrangementClips.push_back(clip);
    rig.ctx.arrangementCursor = 6;
    const auto next = rig.view->locatorButtonBounds(ui::ArrangementView::LocatorButton::Next);
    const auto previous = rig.view->locatorButtonBounds(ui::ArrangementView::LocatorButton::Previous);
    rig.click(next); CHECK_NEAR(rig.ctx.arrangementCursor, 12, 1e-12);
    rig.click(next); CHECK_NEAR(rig.ctx.arrangementCursor, 40, 1e-12); // past the last: Arrangement end
    rig.click(previous); CHECK_NEAR(rig.ctx.arrangementCursor, 12, 1e-12);
    rig.click(previous); CHECK_NEAR(rig.ctx.arrangementCursor, 4, 1e-12);
    rig.click(previous); CHECK_NEAR(rig.ctx.arrangementCursor, 0, 1e-12); // before the first: Arrangement start
    rig.transport.beginBlock(); CHECK_NEAR(rig.transport.positionBeats(), 0, 1e-12);

    // While playing, the jump waits for the next 1-bar boundary; a second Next continues from the pending target.
    rig.song->globalQuantisation = 4;
    rig.transport.seekNow(5); rig.transport.play(); rig.transport.beginBlock();
    rig.click(next);
    CHECK_NEAR(rig.transport.scheduledJumpTarget(), 12, 1e-12); CHECK_NEAR(rig.transport.positionBeats(), 5, 1e-12);
    rig.click(next);
    CHECK_NEAR(rig.transport.scheduledJumpTarget(), 40, 1e-12);
    rig.transport.advanceTo(8); CHECK(rig.transport.beginBlock().jumped); CHECK_NEAR(rig.transport.positionBeats(), 40, 1e-12);
}

TEST_CASE("locators ui: click selects and jumps, drag moves on the grid, keys nudge rename delete, and edits undo")
{
    Rig rig;
    const auto intro = rig.view->locatorBounds(rig.song->locators[0]);
    CHECK(rig.view->locatorAt(intro.x + 3, intro.y + 6) == 0);
    CHECK(rig.view->locatorAt(intro.right() - 1, intro.y + 6) == 0); // the name label is part of it
    CHECK(rig.view->cursorAt(intro.x + 3, intro.y + 6) == ui::Cursor::Default);
    CHECK(rig.view->locatorAt(intro.x + 3, 10) == -1); // overview strip

    auto e = Rig::mouse(intro.x + 3, intro.y + 6);
    rig.view->mouseDown(e);
    CHECK(rig.ctx.selectedLocator == 0); CHECK_NEAR(rig.ctx.arrangementCursor, 4, 1e-12);
    e.x += 3.2f * 20; rig.view->mouseDrag(e); rig.view->mouseUp(e);
    REQUIRE(rig.history.undoLabel() == "Move Locator");
    const double moved = rig.song->locators[0].time;
    CHECK(moved > 4); CHECK_NEAR(std::fmod(moved, 0.25), 0, 1e-9); // snapped to the grid

    // Right nudges by the grid; the locator stays selected through the edit.
    CHECK(rig.key(juce::KeyPress::rightKey));
    CHECK(rig.song->locators[0].time > moved); CHECK(rig.ctx.selectedLocator == 0);

    // Cmd+R edits the name in the ruler; Return commits it as one undoable rename.
    CHECK(rig.key('R', true, 'r')); CHECK(rig.view->wantsTextInput());
    CHECK(rig.key(juce::KeyPress::backspaceKey)); // the whole name was selected
    for (const char c : std::string("Verse")) CHECK(rig.key(0, false, c));
    CHECK(rig.key(juce::KeyPress::returnKey));
    CHECK(rig.song->locators[0].name == "Verse"); CHECK(rig.history.undoLabel() == "Rename Locator");

    CHECK(rig.key(juce::KeyPress::deleteKey));
    CHECK(rig.song->locators.size() == 1); CHECK(rig.ctx.selectedLocator == -1);
    REQUIRE(rig.history.undo(*rig.song)); CHECK(rig.song->locators.size() == 2);
    REQUIRE(rig.history.undo(*rig.song)); CHECK(rig.song->locators[0].name == "Intro");

    // Clicking a lane deselects the locator.
    rig.ctx.selectedLocator = 1;
    e = Rig::mouse(100, rig.view->rulerHeight() + 20);
    rig.view->mouseDown(e); rig.view->mouseUp(e);
    CHECK(rig.ctx.selectedLocator == -1);

    // A double-click on a locator while stopped plays from it.
    const auto drop = rig.view->locatorBounds(rig.song->locators[1]);
    e = Rig::mouse(drop.x + 3, drop.y + 6, 2);
    rig.view->mouseDown(e); rig.view->mouseUp(e);
    rig.transport.beginBlock();
    CHECK(rig.transport.isPlaying()); CHECK_NEAR(rig.transport.positionBeats(), 12, 1e-12);
}

TEST_CASE("locators ui: context menus loop to the next locator, set the song start, add at the scrub position")
{
    Rig rig;
    auto actions = ui::locatorContextActions(rig.ctx, 0, [] {}, [] {});
    const auto action = [&](const char* label) { for (auto& a : actions) if (a.label == label) return a; REQUIRE(false); return ui::MenuAction{}; };
    action("Loop to Next Locator").run();
    CHECK(rig.song->loopOn); CHECK_NEAR(rig.song->loopStart, 4, 1e-12); CHECK_NEAR(rig.song->loopLength, 8, 1e-12);
    CHECK(! action("Set Song Start Time Here").ticked);
    action("Set Song Start Time Here").run();
    CHECK(rig.song->locators[0].isSongStart);
    actions = ui::locatorContextActions(rig.ctx, 0, [] {}, [] {});
    CHECK(action("Set Song Start Time Here").ticked);
    CHECK(! ui::locatorContextActions(rig.ctx, 1, [] {}, [] {})[0].enabled); // the last locator has no next one
    action("Delete").run();
    CHECK(rig.song->locators.size() == 1);
    REQUIRE(rig.history.undo(*rig.song));

    // Play from stopped starts at the song start locator.
    rig.ctx.arrangementCursor = 30; rig.transport.seekNow(30); rig.transport.beginBlock();
    ui::startPlayback(rig.ctx); rig.transport.beginBlock();
    CHECK(rig.transport.isPlaying()); CHECK_NEAR(rig.transport.positionBeats(), 4, 1e-12);

    auto scrub = ui::scrubAreaContextActions(rig.ctx, 6);
    REQUIRE(scrub[0].label == "Add Locator"); REQUIRE(scrub[0].enabled);
    CHECK(! scrub[1].enabled); // Insert Time Signature Change: ENG-11b
    scrub[0].run();
    CHECK(rig.song->locators.size() == 3); CHECK_NEAR(rig.song->locators[1].time, 6, 1e-12);
    CHECK(! ui::scrubAreaContextActions(rig.ctx, 6)[0].enabled); // a locator is there now
}

TEST_CASE("locators ui: drawing — default outline triangle, palette colour, selected fill; Set button turns into Del")
{
    Rig rig;
    rig.ctx.selectedLocator = -1;
    const auto render = [&](const char* name) {
        const auto path = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile(name);
        path.getParentDirectory().createDirectory();
        REQUIRE(gfx::renderToPng(*rig.root, 1300, 800, 1, path.getFullPathName().toStdString()));
        return juce::ImageFileFormat::loadFrom(path);
    };
    const auto origin = rig.view->boundsInRoot();
    const auto pixel = [&](const juce::Image& image, float x, float y) { return image.getPixelAt((int) (origin.x + x), (int) (origin.y + y)); };
    auto image = render("sonora_locators.png");
    REQUIRE(image.isValid());
    const auto intro = rig.view->locatorBounds(rig.song->locators[0]);
    const auto drop = rig.view->locatorBounds(rig.song->locators[1]);
    const auto expectColour = [&](juce::Colour actual, SkColor expected) {
        CHECK(std::abs(actual.getRed() - (int) SkColorGetR(expected)) < 12);
        CHECK(std::abs(actual.getGreen() - (int) SkColorGetG(expected)) < 12);
        CHECK(std::abs(actual.getBlue() - (int) SkColorGetB(expected)) < 12);
    };
    // Triangle interior (2 px right of the time, mid height).
    expectColour(pixel(image, intro.x + 3, intro.y + 6.5f), rig.ctx.theme.color("ControlTextBack"));
    expectColour(pixel(image, drop.x + 3, drop.y + 6.5f), ui::Theme::paletteColor(14));
    rig.ctx.selectedLocator = 0;
    image = render("sonora_locators_selected.png");
    expectColour(pixel(image, intro.x + 3, intro.y + 6.5f), rig.ctx.theme.color("ArrangementRulerMarkings"));
}
