// UI-78 browser search field editing; LIB-3 Show in Browser; LIB-4 the similar list; LIB-5 replacing from the sample
// editor. Driven through RootWidget input (dispatchMouseDown / dispatchKey), not by calling handlers.
#include "TestFramework.h"

#include "gfx/SkiaCanvasComponent.h"
#include "io/als/AlsReader.h"
#include "ui/SampleFiles.h"
#include "ui/TrackMenu.h"
#include "ui/UiContext.h"
#include "ui/views/BrowserPanel.h"
#include "ui/views/MainLayout.h"
#include "ui/views/SampleView.h"

#include <juce_core/juce_core.h>

using namespace sonora;

namespace
{
template <typename T>
T* findWidget(ui::Widget& w)
{
    if (auto* found = dynamic_cast<T*>(&w)) return found;
    for (auto& child : w.children()) if (auto* found = findWidget<T>(*child)) return found;
    return nullptr;
}

ui::MouseEvent mouseAt(float x, float y, int clicks = 1)
{
    ui::MouseEvent e; e.x = x; e.y = y; e.clicks = clicks;
    return e;
}

ui::KeyEvent typed(char c) { ui::KeyEvent k; k.textChar = (uint32_t) c; k.keyCode = c; return k; }
ui::KeyEvent keyCode(int code, bool cmd = false, bool shift = false) { ui::KeyEvent k; k.keyCode = code; k.cmd = cmd; k.shift = shift; return k; }
ui::KeyEvent command(char c) { ui::KeyEvent k; k.textChar = (uint32_t) c; k.keyCode = c; k.cmd = true; return k; }

// A real folder of files: Show in Browser and the sample editor need them on disk.
struct Files
{
    juce::File root = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("sonora_sample_files_ui");
    Files()
    {
        root.deleteRecursively();
        for (const char* name : {"Drums/Kick 1.wav", "Drums/Kick 2.wav", "Drums/Snare.wav", "Drums/Hat.wav", "Other/Pad.wav"})
        {
            root.getChildFile(name).getParentDirectory().createDirectory();
            root.getChildFile(name).replaceWithText("x"); // listed by name; never decoded here
        }
    }
    ~Files() { root.deleteRecursively(); }
    std::string path(const char* name) const { return root.getChildFile(name).getFullPathName().toStdString(); }
};

// The fixture set with its audio clip (track 1) pointing at Drums/Kick 1.wav, selected, Clip view open.
struct Rig
{
    Files files;
    std::unique_ptr<model::Song> song;
    model::EditHistory history;
    ui::UiContext ctx;
    std::unique_ptr<ui::MainLayout> root;
    std::vector<std::string> opened;
    std::vector<std::pair<int, std::string>> replaced;
    std::vector<int> swaps;
    std::vector<std::string> similarSearches;
    int cancels = 0;
    Rig()
    {
        auto loaded = io::loadAls(juce::File(SONORA_TEST_FIXTURES).getChildFile("recording_set.xml"));
        REQUIRE(loaded);
        song = std::move(loaded.song);
        auto& clip = song->tracks[1].arrangementClips[0];
        REQUIRE(clip.kind == model::Clip::Kind::Audio);
        clip.sample = {};
        clip.sample.absolutePath = files.path("Drums/Kick 1.wav");
        clip.sampleRate = 48000; clip.lengthSamples = 48000;
        ctx.song = song.get();
        ctx.selectedTrack = ctx.selectedClipTrack = 1;
        ctx.selectedClipUid = clip.uid;
        ctx.detailTab = ui::DetailTab::Clip;
        ctx.applyEdit = [this](model::EditCommand c) { history.perform(*song, std::move(c)); };
        ctx.openBrowserFolder = [this](const std::string& path) { ctx.browserFolder = path; opened.push_back(path); list(path); };
        ctx.revealInFinder = [](const std::string&) {};
        ctx.findSimilarSamples = [this](const std::string& path, const model::ClipLocation& from) {
            similarSearches.push_back(path);
            ctx.similarSamples.active = true; ctx.similarSamples.running = false;
            ctx.similarSamples.reference = path; ctx.similarSamples.clip = from;
            ctx.similarSamples.results = {{files.path("Drums/Kick 2.wav"), 0.1f}, {files.path("Drums/Snare.wav"), 0.5f}, {files.path("Other/Pad.wav"), 2.0f}};
            ++ctx.similarSamples.revision;
        };
        ctx.cancelSimilarSamples = [this] { ++cancels; };
        ctx.replaceClipSample = [this](const model::ClipLocation& at, const std::string& path) { replaced.push_back({at.uid, path}); };
        ctx.swapSimilarSample = [this](const model::ClipLocation&, int direction) { swaps.push_back(direction); };
        root = std::make_unique<ui::MainLayout>(ctx);
        root->setBounds({0, 0, 1300, 800});
        root->layout();
    }
    // What AppController::openBrowserFolder publishes: the folder's files, directories first.
    void list(const std::string& path)
    {
        ctx.browserFiles.clear();
        juce::Array<juce::File> found = juce::File(path).findChildFiles(juce::File::findFilesAndDirectories, false);
        found.sort();
        for (const auto& f : found) if (f.isDirectory()) ctx.browserFiles.push_back({f.getFullPathName().toStdString(), true});
        for (const auto& f : found) if (! f.isDirectory()) ctx.browserFiles.push_back({f.getFullPathName().toStdString(), false});
        ++ctx.browserContentRevision;
    }
    ui::BrowserPanel& browser() { auto* b = findWidget<ui::BrowserPanel>(*root); REQUIRE(b); return *b; }
    void click(ui::Widget& w, ui::Rect r, int clicks = 1, float dx = 0)
    {
        const auto origin = w.boundsInRoot();
        auto e = mouseAt(origin.x + (dx > 0 ? r.x + dx : r.centreX()), origin.y + r.centreY(), clicks);
        root->dispatchMouseDown(e); root->dispatchMouseUp(e);
    }
    void type(const char* s) { for (; *s; ++s) root->dispatchKey(typed(*s)); }
    void tick() { root->tickTree(); }
    std::string png(const char* name)
    {
        const auto path = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile(name).getFullPathName().toStdString();
        REQUIRE(gfx::renderToPng(*root, 1300, 800, 1, path));
        return path;
    }
};
} // namespace

TEST_CASE("browser search: Cmd+F selects the text, Cmd+A and clicks select, typing replaces, the x and Escape clear")
{
    Rig rig;
    auto& browser = rig.browser();
    rig.root->dispatchKey(command('f'));
    CHECK(browser.searchHasFocus());
    rig.type("kick drum");
    CHECK(browser.query() == "kick drum");
    CHECK(browser.clearSearchVisible());
    // Cmd+A selects everything; typing replaces the selection.
    rig.root->dispatchKey(command('a'));
    CHECK(browser.searchText().allSelected());
    rig.type("hat");
    CHECK(browser.query() == "hat");
    // Shift+Left selects the last letter; Backspace removes just it.
    rig.root->dispatchKey(keyCode(juce::KeyPress::leftKey, false, true));
    CHECK(browser.searchText().hasSelection());
    rig.root->dispatchKey(keyCode(juce::KeyPress::backspaceKey));
    CHECK(browser.query() == "ha");
    // Cmd+F again selects what was typed, so the next search starts fresh.
    rig.type("t snare");
    rig.root->dispatchKey(command('f'));
    CHECK(browser.searchText().allSelected());
    rig.type("pad");
    CHECK(browser.query() == "pad");
    // A double click selects a word, a triple click everything.
    rig.type(" one");
    const auto field = browser.searchFieldBounds();
    rig.click(browser, field, 2, 6); // over "pad"
    CHECK(browser.searchText().hasSelection());
    CHECK(browser.searchText().selectionStart() == 0);
    CHECK(browser.searchText().selectionEnd() == 3);
    rig.type("kit");
    CHECK(browser.query() == "kit one");
    rig.click(browser, field, 3, 6);
    CHECK(browser.searchText().allSelected());
    const auto selectedPng = rig.png("sonora_browser_search_selected.png");
    // The x clears the text and the tag filters; it is hidden once nothing filters the list.
    browser.toggleTagFilter({"Type", "Sample"});
    CHECK(! browser.tagFilters().empty());
    rig.png("sonora_browser_search_clear.png");
    rig.click(browser, browser.clearSearchBounds());
    CHECK(browser.query().empty());
    CHECK(browser.tagFilters().empty());
    CHECK(! browser.clearSearchVisible());
    CHECK(browser.searchHasFocus());
    // Escape clears the field and the filters and leaves it.
    rig.type("bass");
    browser.toggleTagFilter({"Type", "Sample"});
    rig.root->dispatchKey(keyCode(juce::KeyPress::escapeKey));
    CHECK(browser.query().empty());
    CHECK(browser.tagFilters().empty());
    CHECK(! browser.searchHasFocus());
    // Typed letters never reach shortcuts while the field has the focus ('m' toggles the computer MIDI keyboard).
    rig.root->dispatchKey(command('f'));
    const bool keyboard = rig.ctx.computerKeyboard;
    rig.type("m");
    CHECK(rig.ctx.computerKeyboard == keyboard);
    CHECK(browser.query() == "m");
}

TEST_CASE("clip sample: Show in Browser opens the file's folder and selects its row; the menu lists the file actions")
{
    Rig rig;
    const auto location = rig.ctx.selectedClipLocation();
    auto actions = ui::clipContextActions(rig.ctx, location);
    auto find = [&](const std::string& label) -> const ui::MenuAction* {
        for (const auto& action : actions) if (action.label == label) return &action;
        return nullptr;
    };
    for (const char* label : {"Show in Browser", "Show in Finder", "Show Similar Files", "Replace Sample..."})
    {
        REQUIRE(find(label) != nullptr);
        CHECK(find(label)->enabled);
    }
    rig.ctx.browserVisible = false;
    find("Show in Browser")->run();
    CHECK(rig.ctx.browserVisible);
    CHECK(rig.ctx.browserReveal == rig.files.path("Drums/Kick 1.wav"));
    rig.root->layout();
    rig.tick(); // opens the folder
    CHECK(rig.ctx.browserFolder == rig.files.root.getChildFile("Drums").getFullPathName().toStdString());
    rig.tick(); // selects the row once listed
    CHECK(rig.ctx.browserReveal.empty());
    auto& browser = rig.browser();
    CHECK(browser.rowLabel(browser.selectedRow()) == "Kick 1.wav");
    rig.png("sonora_show_in_browser.png");
    // The sample editor's file name does the same.
    auto* view = findWidget<ui::SampleView>(*rig.root);
    REQUIRE(view);
    REQUIRE(! view->nameBounds().isEmpty());
    rig.ctx.browserReveal.clear();
    rig.click(*view, view->nameBounds());
    CHECK(rig.ctx.browserReveal == rig.files.path("Drums/Kick 1.wav"));
    // A missing file: the actions are greyed and say so.
    rig.song->tracks[1].arrangementClips[0].sample.absolutePath = rig.files.path("Drums/Gone.wav");
    actions = ui::clipContextActions(rig.ctx, location);
    CHECK(! find("Show in Browser")->enabled);
    CHECK(find("Replace Sample...")->enabled); // a missing file can still be replaced
}

TEST_CASE("similar list: a 'Similar to' chip over ranked rows, Enter/double-click swaps the clip's file, the chip's x leaves")
{
    Rig rig;
    const auto location = rig.ctx.selectedClipLocation();
    for (const auto& action : ui::clipContextActions(rig.ctx, location))
        if (action.label == "Show Similar Files") action.run();
    REQUIRE(rig.similarSearches.size() == 1);
    CHECK(rig.similarSearches[0] == rig.files.path("Drums/Kick 1.wav"));
    rig.root->layout();
    rig.tick();
    auto& browser = rig.browser();
    CHECK(browser.similarList());
    CHECK(! browser.similarChipBounds().isEmpty());
    CHECK(browser.rowLabel(0) == "Kick 2.wav");
    CHECK(browser.rowLabel(2) == "Pad.wav");
    CHECK(browser.clearSearchVisible());
    rig.png("sonora_similar_list.png");
    // A double-click on the second match puts it into the clip (in the remembered scope).
    rig.click(browser, browser.listRowBounds(1), 2);
    REQUIRE(rig.replaced.size() == 1);
    CHECK(rig.replaced[0].second == rig.files.path("Drums/Snare.wav"));
    CHECK(rig.replaced[0].first == location.uid);
    // Down + Enter (the list has the focus) takes the next one.
    rig.root->dispatchKey(keyCode(juce::KeyPress::downKey));
    rig.root->dispatchKey(keyCode(juce::KeyPress::returnKey));
    REQUIRE(rig.replaced.size() == 2);
    CHECK(rig.replaced[1].second == rig.files.path("Other/Pad.wav"));
    // Typing narrows the list by name.
    rig.root->dispatchKey(command('f'));
    rig.type("kick");
    CHECK(browser.rowLabel(0) == "Kick 2.wav");
    CHECK(browser.rowLabel(1).empty());
    // The chip's x leaves the list.
    const auto chip = browser.similarChipBounds();
    rig.click(browser, chip, 1, chip.w - 6);
    CHECK(! browser.similarList());
    CHECK(rig.cancels == 1);
    CHECK(! rig.ctx.similarSamples.active);
}

TEST_CASE("sample editor: the scope switch, swap arrows and a dropped sample replace the clip's file")
{
    Rig rig;
    auto* view = findWidget<ui::SampleView>(*rig.root);
    REQUIRE(view);
    REQUIRE(! view->scopeBounds(1).isEmpty());
    CHECK(rig.ctx.sampleReplaceScope == model::SampleScope::Track); // LIB-5d: every clip of the file on the track
    rig.click(*view, view->scopeBounds(0));
    CHECK(rig.ctx.sampleReplaceScope == model::SampleScope::Clip);
    rig.click(*view, view->scopeBounds(1));
    CHECK(rig.ctx.sampleReplaceScope == model::SampleScope::Track);
    rig.click(*view, view->swapBounds(1));
    rig.click(*view, view->swapBounds(-1));
    CHECK((rig.swaps == std::vector<int>{1, -1}));
    rig.png("sonora_sample_editor_header.png");
    // Drag a browser sample over the editor and drop it.
    ui::BrowserDrag drag;
    drag.kind = ui::BrowserDrag::Kind::Sample;
    drag.value = rig.files.path("Drums/Hat.wav");
    drag.label = "Hat.wav";
    const auto origin = view->boundsInRoot();
    const float x = origin.x + view->waveArea().centreX(), y = origin.y + view->waveArea().centreY();
    rig.ctx.browserDrag = drag;
    rig.root->dispatchMouseDown(mouseAt(x, y));
    rig.ctx.browserDrag = drag; // the press clears a stale drag; the browser starts a new one on its drag
    rig.root->dispatchMouseDrag(mouseAt(x, y));
    rig.png("sonora_sample_editor_drop.png");
    rig.root->dispatchMouseUp(mouseAt(x, y));
    REQUIRE(rig.replaced.size() == 1);
    CHECK(rig.replaced[0].second == rig.files.path("Drums/Hat.wav"));
}

TEST_CASE("replace sample (LIB-5c): the browser opens on the similar list under a replace bar that any sample row answers")
{
    Rig rig;
    const auto location = rig.ctx.selectedClipLocation();
    rig.ctx.browserVisible = false;
    for (const auto& action : ui::clipContextActions(rig.ctx, location))
        if (action.label == "Replace Sample...") action.run();
    CHECK(rig.ctx.browserVisible);
    CHECK(rig.ctx.sampleSwap.active);
    REQUIRE(rig.similarSearches.size() == 1); // it starts on the samples that sound like the clip's file
    rig.root->layout();
    rig.tick();
    auto& browser = rig.browser();
    REQUIRE(browser.swapBar());
    CHECK(browser.similarList());
    CHECK(browser.similarChipBounds().y >= browser.swapBarBounds().bottom()); // the similar chip sits under the bar
    CHECK(browser.listRowBounds(0).y > browser.similarChipBounds().bottom());
    CHECK(browser.swapScopeLabel().rfind("in Track (", 0) == 0); // LIB-5d: the track's clips of that file by default
    rig.png("sonora_replace_bar.png");
    // A similar match replaces the clip's file; the bar stays for the next try.
    rig.click(browser, browser.listRowBounds(0), 2);
    REQUIRE(rig.replaced.size() == 1);
    CHECK(rig.replaced[0].second == rig.files.path("Drums/Kick 2.wav"));
    CHECK(browser.swapBar());
    // Any folder: the similar list goes, the bar stays, a double-clicked sample row replaces (instead of auditioning).
    browser.showFolder(rig.files.root.getChildFile("Drums").getFullPathName().toStdString());
    rig.root->layout();
    rig.tick();
    CHECK(! browser.similarList());
    CHECK(browser.swapBar());
    const int hat = browser.rowIndex("Hat.wav");
    REQUIRE(hat >= 0);
    rig.click(browser, browser.listRowBounds(hat), 2);
    REQUIRE(rig.replaced.size() == 2);
    CHECK(rig.replaced[1].second == rig.files.path("Drums/Hat.wav"));
    CHECK(rig.replaced[1].first == location.uid);
    // Escape with nothing else to clear ends it; the bar's x too.
    rig.root->dispatchKey(keyCode(juce::KeyPress::escapeKey));
    CHECK(! rig.ctx.sampleSwap.active);
    REQUIRE(ui::beginSampleSwap(rig.ctx, location));
    rig.root->layout();
    rig.tick();
    const auto close = browser.swapCloseBounds();
    REQUIRE(! close.isEmpty());
    rig.click(browser, close);
    CHECK(! rig.ctx.sampleSwap.active);
    CHECK(! browser.similarList()); // the clip's similar list goes with it
    // The clip going away ends it too.
    REQUIRE(ui::beginSampleSwap(rig.ctx, location));
    rig.song->tracks[1].arrangementClips.clear();
    rig.tick();
    CHECK(! rig.ctx.sampleSwap.active);
    // A MIDI clip or nothing to replace: no bar.
    CHECK(! ui::beginSampleSwap(rig.ctx, location));
}
