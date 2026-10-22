// UI-28i: the Sonora Prism preset browser and header navigation, driven through RootWidget input events. The library
// is synthetic (injected into the browser) except in the last case, which needs the installed Serum 2 presets.
#include "TestFramework.h"
#include "devices/DeviceFactory.h"
#include "devices/native/WavetableSynth.h"
#include "engine/EngineGraph.h"
#include "engine/SampleCache.h"
#include "midi/MidiManager.h"
#include "plugins/SerumLibrary.h"
#include "ui/views/MainLayout.h"
#include "ui/views/WavetableEditor.h"
#include "ui/views/WavetablePresetBrowser.h"
#include "ui/widgets/Buttons.h"
#include <juce_gui_basics/juce_gui_basics.h>

using namespace sonora;
namespace
{
using W = devices::WavetableSynth;
using Editor = ui::WavetableEditor;
using Browser = ui::WavetablePresetBrowser;

template <typename T> void collect(ui::Widget& widget, std::vector<T*>& result)
{
    if (auto* found = dynamic_cast<T*>(&widget)) result.push_back(found);
    for (const auto& child : widget.children()) collect(*child, result);
}
plugins::SerumLibraryItem item(std::string folder, std::string name)
{
    return {"/lib/" + folder + "/" + name + ".SerumPreset", std::move(name), std::move(folder)};
}
std::shared_ptr<const Browser::Library> library()
{
    return std::make_shared<const Browser::Library>(Browser::Library{
        item("Factory/Bass", "Bass A"), item("Factory/Bass", "Bass B"), item("Factory/Pad", "Pad A"), item("Factory/Pad", "Pad B"),
        item("User", "Mine")});
}
struct Rig
{
    juce::ScopedJuceInitialiser_GUI init;
    midi::MidiManager midi;
    model::Song song;
    engine::SampleCache samples;
    std::unique_ptr<engine::EngineGraph> graph;
    ui::UiContext context;
    std::unique_ptr<ui::MainLayout> root;
    Editor* editor = nullptr;
    std::vector<std::string> loaded;

    Rig()
    {
        song.tracks.resize(1); song.tracks[0].kind = model::TrackKind::Midi;
        song.tracks[0].devices = {devices::nativeDeviceModel("SonoraWavetable")};
        song.tracks[0].devices[0].id = 77;
        context.song = &song; context.midi = &midi;
        context.browserVisible = false; context.detailTab = ui::DetailTab::Devices;
        context.theme.metrics.detailHeight = 700;
        context.computerKeyboard = true; // letters must reach the search field, not the note keys
        rebuild();
        root = std::make_unique<ui::MainLayout>(context); root->setBounds({0, 0, 1400, 1000}); root->layout();
        findEditor();
    }
    void rebuild()
    {
        devices::BuildContext build;
        auto next = engine::buildGraph(song, build, samples).graph; REQUIRE(next); next->prepare(48000, 512);
        context.graph = next.get(); context.controls = next->controls;
        graph = std::move(next);
    }
    void findEditor() { std::vector<Editor*> editors; collect(*root, editors); REQUIRE(editors.size() == 1); editor = editors[0]; }
    // The app's preset import replaces the device and rebuilds the graph and editor synchronously.
    void importRebuilding()
    {
        context.importWavetablePreset = [this](int, int, const std::string& path, int, int) {
            loaded.push_back(path);
            song.tracks[0].devices[0].displayName = juce::File(juce::String(path)).getFileNameWithoutExtension().toStdString();
            rebuild(); root->layout(); findEditor();
        };
    }
    Browser* browser() const { return editor->presetBrowser(); }
    ui::MouseEvent at(const ui::Widget& widget, ui::Rect cell) const
    {
        const auto origin = widget.boundsInRoot();
        ui::MouseEvent event; event.x = origin.x + cell.centreX(); event.y = origin.y + cell.centreY();
        return event;
    }
    void click(const ui::Widget& widget, ui::Rect cell, int clicks = 1)
    {
        REQUIRE(! cell.isEmpty()); auto e = at(widget, cell); e.clicks = clicks;
        root->dispatchMouseDown(e); root->dispatchMouseUp(e);
    }
    void type(const std::string& text)
    {
        for (const char c : text) { ui::KeyEvent key; key.keyCode = c; key.textChar = (uint32_t) c; CHECK(root->dispatchKey(key)); }
    }
    void press(int keyCode) { ui::KeyEvent key; key.keyCode = keyCode; root->dispatchKey(key); }
    // Swap the scanned library for the synthetic one, keeping the browser open.
    void inject()
    {
        browser()->setLibrary(library(), {"Empty", "Factory", "User"});
        browser()->restore(browser()->initialState());
        root->layout();
    }
    std::vector<std::string> names() const
    {
        std::vector<std::string> result;
        for (const int index : browser()->visiblePresets()) result.push_back(browser()->library()[(size_t) index].name);
        return result;
    }
};
} // namespace

TEST_CASE("wavetable preset browser UI: the name opens it over the dashboard; search, banks, groups and clicks load presets")
{
    Rig rig;
    rig.importRebuilding();
    REQUIRE(! rig.editor->presetBrowserOpen());
    const auto knob = rig.editor->parameterCell(W::oscParam(0, W::Position));
    REQUIRE(! knob.isEmpty());
    rig.click(*rig.editor, rig.editor->presetCell(0));
    REQUIRE(rig.editor->presetBrowserOpen());
    CHECK(rig.browser()->visible);
    const auto hit = [&] {
        float lx = 0, ly = 0; const auto origin = rig.editor->boundsInRoot();
        return rig.root->findAt(origin.x + knob.centreX(), origin.y + knob.centreY(), lx, ly);
    };
    CHECK(hit() == rig.browser()); // the dashboard beneath takes no clicks
    CHECK(rig.browser()->wantsTextInput()); // focused on open: typing searches straight away
    rig.inject();

    // Named banks in natural order, the empty bank folder included.
    REQUIRE(rig.browser()->banks().size() == 3);
    CHECK(rig.browser()->banks()[0] == "Empty"); CHECK(rig.browser()->banks()[1] == "Factory");
    CHECK(rig.browser()->state().bank == "Factory"); // the first bank that has presets
    CHECK(rig.names().size() == 4);

    // Typing filters (and does not play the computer keyboard); Backspace edits.
    rig.type("pad");
    CHECK(rig.browser()->state().query == "pad");
    REQUIRE(rig.names().size() == 2); CHECK(rig.names()[0] == "Pad A");
    rig.press(juce::KeyPress::backspaceKey); CHECK(rig.browser()->state().query == "pa");
    rig.press(juce::KeyPress::escapeKey); CHECK(rig.browser()->state().query.empty()); CHECK(rig.editor->presetBrowserOpen());

    // Groups narrow the list; "All" restores it. Banks switch the list.
    rig.click(*rig.browser(), rig.browser()->groupCell(1)); // Bass
    REQUIRE(rig.names().size() == 2); CHECK(rig.names()[1] == "Bass B");
    rig.click(*rig.browser(), rig.browser()->groupCell(0));
    CHECK(rig.names().size() == 4);
    rig.click(*rig.browser(), rig.browser()->bankCell(2)); // User
    REQUIRE(rig.names().size() == 1); CHECK(rig.names()[0] == "Mine");
    rig.click(*rig.browser(), rig.browser()->bankCell(1));

    // A click loads through the preset import, which rebuilds the editor; the browser reopens where it was, focused.
    rig.type("b");
    REQUIRE(rig.names().size() == 3); // Bass A, Bass B (folder "Bass"), Pad B
    rig.click(*rig.browser(), rig.browser()->presetRowCell(1));
    REQUIRE(rig.loaded.size() == 1); CHECK(rig.loaded[0] == "/lib/Factory/Bass/Bass B.SerumPreset");
    REQUIRE(rig.editor->presetBrowserOpen());
    CHECK(rig.browser()->state().query == "b");
    CHECK(rig.browser()->wantsTextInput());
    CHECK(rig.editor->libraryPresetPath() == "/lib/Factory/Bass/Bass B.SerumPreset");

    // Down steps and loads the next match; Return then closes.
    rig.browser()->setLibrary(library(), {"Empty", "Factory", "User"}); // the rebuilt editor reused the real scan
    rig.press(juce::KeyPress::downKey);
    REQUIRE(rig.loaded.size() == 2); CHECK(rig.loaded[1] == "/lib/Factory/Pad/Pad B.SerumPreset");
    rig.browser()->setLibrary(library(), {"Empty", "Factory", "User"});
    rig.press(juce::KeyPress::returnKey);
    CHECK(! rig.editor->presetBrowserOpen());
    CHECK(! rig.browser()->visible);
    CHECK(hit() != nullptr); CHECK(hit() != rig.browser()); // the dashboard is back
}

TEST_CASE("wavetable preset browser UI: close button, Escape and the name toggle close it; double-click loads and closes")
{
    Rig rig;
    std::vector<std::string> loaded;
    rig.context.importWavetablePreset = [&](int, int, const std::string& path, int, int) { loaded.push_back(path); };
    rig.click(*rig.editor, rig.editor->presetCell(0)); rig.inject();
    rig.click(*rig.browser(), rig.browser()->closeCell());
    CHECK(! rig.editor->presetBrowserOpen());
    rig.click(*rig.editor, rig.editor->presetCell(0)); REQUIRE(rig.editor->presetBrowserOpen());
    rig.click(*rig.editor, rig.editor->presetCell(0)); CHECK(! rig.editor->presetBrowserOpen());
    rig.click(*rig.editor, rig.editor->presetCell(0)); rig.inject();
    rig.press(juce::KeyPress::escapeKey); CHECK(! rig.editor->presetBrowserOpen());
    rig.click(*rig.editor, rig.editor->presetCell(0)); rig.inject();
    rig.click(*rig.browser(), rig.browser()->presetRowCell(2));
    REQUIRE(loaded.size() == 1); CHECK(loaded[0] == "/lib/Factory/Pad/Pad A.SerumPreset");
    rig.click(*rig.browser(), rig.browser()->presetRowCell(2), 2);
    CHECK(loaded.size() == 1); // already loaded: the double-click only closes
    CHECK(! rig.editor->presetBrowserOpen());
}

TEST_CASE("wavetable preset browser UI: header arrows stay in the installed bank and << >> jump groups")
{
    const auto presets = plugins::serumLibrary("Presets");
    if (presets.empty()) return; // Serum 2 not installed on this machine
    Rig rig;
    std::vector<std::string> loaded;
    rig.context.importWavetablePreset = [&](int, int, const std::string& path, int, int) { loaded.push_back(path); };
    std::vector<ui::TextButton*> buttons; collect(*rig.editor, buttons);
    const auto button = [&](const std::string& label) {
        for (auto* b : buttons) if (b->label() == label) return b;
        return (ui::TextButton*) nullptr;
    };
    REQUIRE(button("<<")); REQUIRE(button(">>"));
    CHECK(! button(">>")->isEnabled()); // no library preset loaded yet
    // The last preset of the first bank: ">" wraps to that bank's first preset, never into the next bank.
    const auto bank = plugins::serumLibraryBank(presets.front());
    int last = 0;
    while ((size_t) last + 1 < presets.size() && plugins::serumLibraryBank(presets[(size_t) last + 1]) == bank) ++last;
    rig.editor->loadLibraryPreset(presets[(size_t) last].path);
    rig.root->layout();
    rig.click(*rig.editor, rig.editor->presetCell(1));
    REQUIRE(loaded.size() == 2); CHECK(loaded[1] == presets.front().path);
    rig.click(*rig.editor, rig.editor->presetCell(-1));
    REQUIRE(loaded.size() == 3); CHECK(loaded[2] == presets[(size_t) last].path);
    // Groups: enabled when the bank has two or more; ">>" lands on the first preset of the next group.
    if (plugins::serumLibraryGroups(presets, bank).size() >= 2)
    {
        rig.root->layout();
        CHECK(button(">>")->isEnabled());
        rig.click(*rig.editor, rig.editor->presetGroupCell(1));
        REQUIRE(loaded.size() == 4);
        CHECK(loaded[3] == presets[(size_t) plugins::serumLibraryStepGroup(presets, presets[(size_t) last].path, 1)].path);
        CHECK(plugins::serumLibraryBank(presets[(size_t) plugins::serumLibraryFind(presets, loaded[3])]) == bank);
        rig.click(*rig.editor, rig.editor->presetGroupCell(-1));
        REQUIRE(loaded.size() == 5);
        CHECK(plugins::serumLibraryGroup(presets[(size_t) plugins::serumLibraryFind(presets, loaded[4])])
              == plugins::serumLibraryGroup(presets[(size_t) last]));
    }
}
