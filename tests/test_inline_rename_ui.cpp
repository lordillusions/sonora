// UI-58: Cmd+R renames a track in place: the name turns into a selected text field in the Arrangement header or the
// Session title; typing replaces it, Return commits, Escape cancels, a click elsewhere commits.
#include "TestFramework.h"
#include "engine/Transport.h"
#include "gfx/SkiaCanvasComponent.h"
#include "io/als/AlsNewSet.h"
#include "io/als/AlsReader.h"
#include "ui/InlineEdit.h"
#include "ui/Theme.h"
#include "ui/views/MainLayout.h"

#include <juce_graphics/juce_graphics.h>

using namespace sonora;
namespace
{
ui::KeyEvent key(int code, uint32_t text = 0, bool cmd = false)
{
    ui::KeyEvent k; k.keyCode = code; k.textChar = text; k.cmd = cmd;
    return k;
}
ui::KeyEvent typed(char ch) { return key(ch, (uint32_t) ch); }

struct Rig
{
    std::unique_ptr<model::Song> song;
    model::EditHistory history;
    engine::Transport transport;
    ui::UiContext ctx;
    std::unique_ptr<ui::MainLayout> root;
    explicit Rig(ui::MainView view = ui::MainView::Arrangement)
    {
        song = io::parseAls(*io::createNewSetDom(), "/tmp/inline-rename-ui.als").song;
        song->tracks[0].name = "Bass"; song->tracks[0].nameIsAutomatic = false;
        ctx.song = song.get(); ctx.transport = &transport; ctx.browserVisible = ctx.detailVisible = false;
        ctx.mainView = view;
        ctx.applyEdit = [this](model::EditCommand cmd) { history.perform(*song, std::move(cmd)); };
        root = std::make_unique<ui::MainLayout>(ctx); root->setBounds({0, 0, 1300, 800}); root->layout();
    }
    void type(const char* s) { for (; *s; ++s) root->dispatchKey(typed(*s)); }
    juce::Image render(const char* name)
    {
        const auto path = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile(name);
        REQUIRE(gfx::renderToPng(*root, 1300, 800, 1, path.getFullPathName().toStdString()));
        return juce::ImageFileFormat::loadFrom(path);
    }
};

void expectColour(juce::Colour actual, SkColor expected)
{
    CHECK(std::abs(actual.getRed() - (int) SkColorGetR(expected)) < 12);
    CHECK(std::abs(actual.getGreen() - (int) SkColorGetG(expected)) < 12);
    CHECK(std::abs(actual.getBlue() - (int) SkColorGetB(expected)) < 12);
}
} // namespace

TEST_CASE("inline rename: the field starts selected; typing replaces, arrows place the caret, UTF-8 safe")
{
    ui::InlineTextEdit edit;
    edit.start("Bass");
    CHECK(edit.allSelected);
    CHECK(edit.key(typed('K')) == ui::InlineTextEdit::Result::Edited);
    CHECK(edit.text == "K");
    CHECK(! edit.allSelected);
    edit.key(typed('e')); edit.key(typed('y'));
    edit.key(key(juce::KeyPress::leftKey));
    edit.key(typed('Z'));
    CHECK(edit.text == "KeZy");
    edit.key(key(juce::KeyPress::backspaceKey));
    CHECK(edit.text == "Key");
    edit.key(key(juce::KeyPress::homeKey));
    edit.key(key(juce::KeyPress::deleteKey));
    CHECK(edit.text == "ey");
    edit.key(key('A', 'a', true));
    CHECK(edit.allSelected);
    edit.key(key(juce::KeyPress::backspaceKey));
    CHECK(edit.text.empty());
    edit.start("Caf\xC3\xA9");
    edit.key(key(juce::KeyPress::rightKey)); // collapses the selection to the end
    CHECK(! edit.allSelected);
    edit.key(key(juce::KeyPress::backspaceKey));
    CHECK(edit.text == "Caf"); // the whole two-byte code point
    edit.insert("a\nb");
    CHECK(edit.text == "Cafa b");
    CHECK(edit.key(key(juce::KeyPress::returnKey)) == ui::InlineTextEdit::Result::Commit);
    CHECK(edit.key(key(juce::KeyPress::escapeKey)) == ui::InlineTextEdit::Result::Cancel);
}

TEST_CASE("inline rename: Return commits one undoable Rename Track, Escape keeps the name, keys never reach shortcuts")
{
    Rig rig;
    REQUIRE(ui::beginTrackRename(rig.ctx, 0));
    CHECK(rig.ctx.trackRename.edit.text == "Bass");
    CHECK(rig.ctx.trackRename.edit.allSelected);
    const bool keyboardBefore = rig.ctx.computerKeyboard;
    rig.type("Lead m"); // 'm' would toggle the computer MIDI keyboard; here it is a letter
    CHECK(rig.ctx.computerKeyboard == keyboardBefore);
    CHECK(rig.song->tracks[0].name == "Bass"); // nothing changes before Return
    rig.root->dispatchKey(key(juce::KeyPress::returnKey));
    CHECK(! rig.ctx.trackRename.active());
    CHECK(rig.song->tracks[0].name == "Lead m");
    CHECK(rig.history.undoLabel() == "Rename Track");
    rig.history.undo(*rig.song);
    CHECK(rig.song->tracks[0].name == "Bass");

    REQUIRE(ui::beginTrackRename(rig.ctx, 0));
    rig.type("Gone");
    rig.root->dispatchKey(key(juce::KeyPress::escapeKey));
    CHECK(! rig.ctx.trackRename.active());
    CHECK(rig.song->tracks[0].name == "Bass");

    // An empty or unchanged name is no edit.
    REQUIRE(ui::beginTrackRename(rig.ctx, 0));
    rig.root->dispatchKey(key(juce::KeyPress::backspaceKey));
    rig.root->dispatchKey(key(juce::KeyPress::returnKey));
    CHECK(rig.song->tracks[0].name == "Bass");
    CHECK(! rig.history.canUndo());

    // Main cannot be renamed.
    CHECK(! ui::beginTrackRename(rig.ctx, rig.ctx.numTracks() + rig.ctx.numReturns()));
}

TEST_CASE("inline rename: the Arrangement header draws the selected name in place; a click elsewhere commits")
{
    Rig rig;
    REQUIRE(ui::beginTrackRename(rig.ctx, 0));
    auto image = rig.render("sonora_inline_rename_arrangement.png");
    REQUIRE(image.isValid());
    const auto field = rig.ctx.trackRename.bounds;
    REQUIRE(! field.isEmpty());
    expectColour(image.getPixelAt((int) (field.x + 5), (int) (field.y + 2.5f)), rig.ctx.theme.color("RangeEditField"));
    // A click inside the field keeps editing.
    ui::MouseEvent inside; inside.x = field.centreX(); inside.y = field.centreY();
    rig.root->dispatchMouseDown(inside); rig.root->dispatchMouseUp(inside);
    CHECK(rig.ctx.trackRename.active());
    rig.type("Keys");
    ui::MouseEvent away; away.x = 650; away.y = 500;
    rig.root->dispatchMouseDown(away); rig.root->dispatchMouseUp(away);
    CHECK(! rig.ctx.trackRename.active());
    CHECK(rig.song->tracks[0].name == "Keys");
}

TEST_CASE("inline rename: the Session title draws the field too")
{
    Rig rig(ui::MainView::Session);
    REQUIRE(ui::beginTrackRename(rig.ctx, 0));
    auto image = rig.render("sonora_inline_rename_session.png");
    REQUIRE(image.isValid());
    const auto field = rig.ctx.trackRename.bounds;
    REQUIRE(! field.isEmpty());
    expectColour(image.getPixelAt((int) (field.x + 5), (int) (field.y + 2.5f)), rig.ctx.theme.color("RangeEditField"));
}
