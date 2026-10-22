// REQ-3a: rebindable menu shortcuts (model, key routing, Settings page).
#include "TestFramework.h"
#include "gfx/SkiaCanvasComponent.h"
#include "ui/Shortcuts.h"
#include "ui/views/MainLayout.h"
#include "ui/views/PreferencesPanel.h"

#include <juce_gui_basics/juce_gui_basics.h>

using namespace sonora;

namespace
{
using K = juce::KeyPress;
ui::ShortcutMap sampleMap()
{
    ui::ShortcutMap map;
    map.setCommands({
        {1, "Create", "Insert Audio Track", ui::KeyCombo::fromGlyphs("⌘T")},
        {2, "Create", "Insert MIDI Track", ui::KeyCombo::fromGlyphs("⇧⌘T")},
        {3, "Create", "Insert Return Track", ui::KeyCombo::fromGlyphs("⌥⌘T")},
        {4, "Edit", "Strip Silence", std::nullopt},
        {5, "View", "Automation Mode", ui::KeyCombo::fromGlyphs("A")},
    });
    return map;
}
ui::KeyEvent key(int code, bool cmd = false, bool shift = false, bool alt = false, bool ctrl = false)
{
    ui::KeyEvent k; k.keyCode = code; k.textChar = (uint32_t) code; k.cmd = cmd; k.shift = shift; k.alt = alt; k.ctrl = ctrl; return k;
}
ui::MouseEvent mouseAt(float x, float y) { ui::MouseEvent e; e.x = x; e.y = y; return e; }
template <typename T>
T* findWidget(ui::Widget& w)
{
    if (auto* hit = dynamic_cast<T*>(&w)) return hit;
    for (auto& child : w.children()) if (auto* hit = findWidget<T>(*child)) return hit;
    return nullptr;
}
} // namespace

TEST_CASE("rebindable shortcuts: menu glyphs and settings text parse to the same keys")
{
    const auto t = ui::KeyCombo::fromGlyphs("⌥⇧⌘F"); REQUIRE(t);
    CHECK(t->cmd); CHECK(t->shift); CHECK(t->alt); CHECK(! t->ctrl); CHECK(t->key == 'F');
    CHECK(t->glyphs() == "⌥⇧⌘F");
    CHECK(t->text() == "Opt+Shift+Cmd+F");
    CHECK(ui::KeyCombo::fromText("Opt+Shift+Cmd+F") == t);
    CHECK(ui::KeyCombo::fromGlyphs("⇧⌘F9")->key == K::F9Key);
    CHECK(ui::KeyCombo::fromGlyphs("⌘Enter")->key == K::returnKey);
    CHECK(ui::KeyCombo::fromGlyphs("⇧⇥")->key == K::tabKey);
    CHECK(ui::KeyCombo::fromGlyphs("⌥Right")->key == K::rightKey);
    CHECK(ui::KeyCombo::fromGlyphs("⌘,")->key == ',');
    CHECK(ui::KeyCombo::fromGlyphs("Delete")->key == K::backspaceKey);
    CHECK(ui::KeyCombo::fromText("Delete")->key == K::deleteKey);
    CHECK(ui::KeyCombo::fromText("Cmd++")->key == '+');
    CHECK(ui::KeyCombo::fromText("Ctrl+Cmd+F")->ctrl);
    CHECK(! ui::KeyCombo::fromGlyphs(""));
    CHECK(! ui::KeyCombo::fromGlyphs("⌘Banana"));
    CHECK(ui::KeyCombo::fromEvent(key('t', true)) == *ui::KeyCombo::fromGlyphs("⌘T")); // letters are case-insensitive
    CHECK((ui::KeyCombo{K::spaceKey}.reserved())); CHECK((ui::KeyCombo{K::escapeKey, true}.reserved()));
    CHECK((! ui::KeyCombo{K::spaceKey, true}.reserved())); CHECK((! ui::KeyCombo{'A'}.reserved()));
}

TEST_CASE("rebindable shortcuts: only changed keys are taken over; defaults the user moved away go silent")
{
    auto map = sampleMap();
    const auto cmdT = *ui::KeyCombo::fromGlyphs("⌘T"), optCmdR = *ui::KeyCombo::fromText("Opt+Cmd+R");
    CHECK(map.resolve(cmdT) == 0); // untouched defaults keep their view handlers
    CHECK(map.assign(3, optCmdR) == 0);
    CHECK(map.isCustom(3)); CHECK(map.resolve(optCmdR) == 3);
    CHECK(map.resolve(*ui::KeyCombo::fromGlyphs("⌥⌘T")) == ui::ShortcutMap::kReleased);
    // taking a key another command holds moves it
    CHECK(map.assign(4, cmdT) == 1);
    CHECK(! map.keyFor(1)); CHECK(map.isCustom(1)); CHECK(map.resolve(cmdT) == 4);
    // resetting the original owner takes its default back
    map.reset(1);
    CHECK(map.keyFor(1) == cmdT); CHECK(! map.isCustom(1)); CHECK(! map.keyFor(4)); CHECK(map.resolve(cmdT) == 0);
    // assigning a command its own default is not custom
    map.assign(5, *ui::KeyCombo::fromGlyphs("A")); CHECK(! map.isCustom(5));
    map.assign(5, std::nullopt); CHECK(map.isCustom(5)); CHECK((map.resolve(ui::KeyCombo{'A'}) == ui::ShortcutMap::kReleased));
    map.resetAll(); CHECK(! map.isCustom(3)); CHECK(! map.isCustom(5)); CHECK(map.resolve(optCmdR) == 0);
}

TEST_CASE("rebindable shortcuts: overrides persist by menu and command name, and survive renumbered ids")
{
    auto map = sampleMap();
    map.assign(3, ui::KeyCombo::fromText("Opt+Cmd+R"));
    map.assign(5, std::nullopt);
    const auto json = map.toJson();
    CHECK(json.find("Create/Insert Return Track") != std::string::npos);
    ui::ShortcutMap later; // a later build numbers the menu items differently
    later.setCommands({{30, "Create", "Insert Return Track", ui::KeyCombo::fromGlyphs("⌥⌘T")}, {50, "View", "Automation Mode", ui::KeyCombo::fromGlyphs("A")},
                       {60, "Help", "Read the Sonora Guide...", std::nullopt}});
    later.fromJson(json);
    CHECK(later.keyFor(30) == ui::KeyCombo::fromText("Opt+Cmd+R"));
    CHECK(later.isCustom(50)); CHECK(! later.keyFor(50));
    later.fromJson("{\"Create/Insert Return Track\": \"Space\", \"Gone/Command\": \"Cmd+G\"}"); // reserved and unknown ignored
    CHECK(! later.isCustom(30));
    later.fromJson("not json"); CHECK(! later.isCustom(30));
}

TEST_CASE("rebindable shortcuts: the window's key path runs rebound keys, never while Settings or typing has them")
{
    ui::UiContext ctx;
    ui::MainLayout root(ctx); root.setBounds({0, 0, 1000, 700}); root.layout();
    int ran = 0;
    ctx.userShortcut = [&](const ui::KeyEvent& k) { if (k.keyCode == 'R' && k.cmd && k.alt) { ++ran; return true; } return false; };
    CHECK(root.dispatchUserKey(key('R', true, false, true))); CHECK(ran == 1);
    root.dispatchKey(key('R', true, false, true)); CHECK(ran == 1); // synthetic menu keys bypass the user map
    ctx.preferencesOpen = true;
    root.dispatchUserKey(key('R', true, false, true)); CHECK(ran == 1);
    ctx.preferencesOpen = false; ctx.computerKeyboard = true;
    ctx.userShortcut = [&](const ui::KeyEvent&) { ++ran; return true; };
    root.dispatchUserKey(key('A')); CHECK(ran == 1); // plain keys belong to the MIDI keyboard
    root.dispatchUserKey(key('A', true)); CHECK(ran == 2);
}

TEST_CASE("rebindable shortcuts: Settings records, clears, filters and resets shortcuts")
{
    ui::UiContext ctx;
    auto map = sampleMap();
    ctx.shortcuts = &map;
    int saves = 0; ctx.shortcutsChanged = [&] { ++saves; };
    ctx.preferencesOpen = true; ctx.settingsPage = (int) ui::PreferencesPanel::Tab::Keys;
    ui::MainLayout root(ctx); root.setBounds({0, 0, 1000, 700});
    auto* settings = findWidget<ui::PreferencesPanel>(root); REQUIRE(settings);
    juce::TemporaryFile file(".png");
    auto draw = [&] { REQUIRE(gfx::renderToPng(root, 1000, 700, 1, file.getFile().getFullPathName().toStdString())); };
    auto click = [&](const std::string& label) {
        draw(); const auto r = settings->controlBounds(label); REQUIRE(! r.isEmpty());
        root.dispatchMouseDown(mouseAt(r.centreX(), r.centreY())); root.dispatchMouseUp(mouseAt(r.centreX(), r.centreY()));
    };
    click("Keyboard Shortcuts"); CHECK(ctx.settingsPage == (int) ui::PreferencesPanel::Tab::Keys);
    click("Shortcut Create/Insert Return Track"); CHECK(settings->recordingShortcut() == 3);
    root.dispatchUserKey(key('R', true, false, true)); // reaches the panel, not the user map
    CHECK(map.keyFor(3) == ui::KeyCombo::fromText("Opt+Cmd+R")); CHECK(saves == 1); CHECK(settings->recordingShortcut() == 0);
    click("Reset Create/Insert Return Track"); CHECK(! map.isCustom(3)); CHECK(saves == 2);
    click("Shortcut Edit/Strip Silence");
    root.dispatchUserKey(key(K::spaceKey)); // reserved: nothing changes
    CHECK(! map.keyFor(4)); CHECK(saves == 2);
    click("Shortcut View/Automation Mode");
    root.dispatchUserKey(key(K::backspaceKey));
    CHECK(! map.keyFor(5)); CHECK(saves == 3);
    click("Shortcut Create/Insert MIDI Track");
    root.dispatchUserKey(key(K::escapeKey)); CHECK(settings->recordingShortcut() == 0); CHECK(ctx.preferencesOpen); // Esc cancels only
    for (char ch : std::string("strip")) root.dispatchUserKey(key(ch));
    draw();
    CHECK(! settings->controlBounds("Shortcut Edit/Strip Silence").isEmpty());
    CHECK(settings->controlBounds("Shortcut Create/Insert MIDI Track").isEmpty());
    click("Reset All"); CHECK(! map.isCustom(5)); CHECK(saves == 4);
}
