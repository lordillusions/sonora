#include "TestFramework.h"
#include "gfx/SkiaCanvasComponent.h"
#include "ui/BrowserTags.h"
#include "ui/views/BrowserPanel.h"
#include "ui/views/MainLayout.h"
#include <juce_gui_basics/juce_gui_basics.h>

using namespace sonora;

TEST_CASE("browser tags: defaults removals additions and format-specific identities survive JSON reload")
{
    ui::BrowserTags tags;
    const ui::BrowserTags::Tags defaults{{"Creator", "Maker"}, {"Device", "Synthesizer"}, {"Format", "VST3"}};
    CHECK(tags.set("plugin:synth-vst3", {"Device", "Synthesizer"}, false));
    CHECK(tags.set("plugin:synth-vst3", {"User", "  Bright 東京  "}, true));
    CHECK(! tags.set("plugin:synth-vst3", {"user", "bright 東京"}, true));
    CHECK(! tags.set("plugin:synth-vst3", {"User", "   "}, true));
    const auto selected = tags.tagsFor("plugin:synth-vst3", defaults);
    CHECK(selected == ui::BrowserTags::Tags({{"Creator", "Maker"}, {"Format", "VST3"}, {"User", "Bright 東京"}}));
    CHECK(tags.tagsFor("plugin:synth-au", defaults) == defaults);
    ui::BrowserTags loaded; loaded.loadJson(tags.toJson());
    CHECK(loaded.tagsFor("plugin:synth-vst3", defaults) == selected);
    CHECK(loaded.knownTags().size() == 2);
    CHECK(loaded.set("plugin:synth-vst3", {"Device", "Synthesizer"}, true));
    CHECK(ui::BrowserTags::matches(loaded.tagsFor("plugin:synth-vst3", defaults), {{"Device", "Synthesizer"}}));
    loaded.loadJson("not JSON"); CHECK(loaded.tagsFor("plugin:synth-vst3", defaults) == defaults);
    loaded.loadJson(R"([{"key":"x","added":[3,["User",false],["User","Good"]],"removed":null},null])");
    CHECK(loaded.tagsFor("x", {}) == ui::BrowserTags::Tags({{"User", "Good"}}));
}

TEST_CASE("browser tags: filter alternatives OR within a group and AND across groups")
{
    const ui::BrowserTags::Tags tags{{"Type", "Instrument"}, {"Format", "VST3"}, {"Creator", "Maker"}};
    CHECK(ui::BrowserTags::matches(tags, {}));
    CHECK(ui::BrowserTags::matches(tags, {{"Format", "AU"}, {"Format", "VST3"}, {"Type", "Instrument"}}));
    CHECK(! ui::BrowserTags::matches(tags, {{"Format", "AU"}, {"Type", "Instrument"}}));
    CHECK(! ui::BrowserTags::matches(tags, {{"Format", "VST3"}, {"Type", "Audio Effect"}}));
    CHECK(ui::BrowserTags::matches(tags, {{"creator", "maker"}}));
}

namespace
{
struct BrowserRig
{
    ui::UiContext context;
    std::unique_ptr<ui::MainLayout> root;
    ui::BrowserPanel* browser = nullptr;
    std::string stored;
    BrowserRig()
    {
        context.detailVisible = false;
        context.theme.metrics.browserWidth = 720;
        context.theme.metrics.browserSidebarWidth = 170;
        context.browserPlugins = {{"Alpha Synth", "synth3", "Maker", "VST3", true, "Synth"},
                                  {"Alpha Synth", "synthau", "Maker", "AudioUnit", true, "Synth"},
                                  {"Echo", "fx3", "Maker", "VST3", false, "Delay"},
                                  {"Compressor", "fx2", "Other", "VST", false, "Dynamics"}};
        context.browserTagsChanged = [&] { stored = context.browserTags.toJson(); };
        root = std::make_unique<ui::MainLayout>(context); root->setBounds({0, 0, 1300, 800}); root->layout();
        for (const auto& child : root->children()) if (auto* b = dynamic_cast<ui::BrowserPanel*>(child.get())) browser = b;
        REQUIRE(browser); browser->showCategory(6);
    }
    ui::MouseEvent at(ui::Rect box)
    {
        const auto origin = browser->boundsInRoot();
        ui::MouseEvent e; e.x = origin.x + box.centreX(); e.y = origin.y + box.centreY(); return e;
    }
    void click(ui::Rect box) { const auto e = at(box); root->dispatchMouseDown(e); root->dispatchMouseUp(e); }
    void key(int code, uint32_t ch = 0) { ui::KeyEvent e; e.keyCode = code; e.textChar = ch; root->dispatchKey(e); }
};
}

TEST_CASE("browser tag gestures: filtered drag keeps plugin identity and typed tags persist without song edits")
{
    BrowserRig rig;
    rig.browser->toggleTagFilter({"Type", "Instrument"});
    rig.browser->toggleTagFilter({"Format", "VST3"});
    CHECK(! rig.browser->filterGroupBounds("Creator").isEmpty());
    rig.click(rig.browser->listRowBounds(1));
    CHECK(ui::BrowserTags::matches(rig.browser->selectedTags(), {{"Device", "Synthesizer"}, {"Creator", "Maker"}}));
    const auto grab = rig.at(rig.browser->listRowBounds(1));
    rig.root->dispatchMouseDown(grab); auto drag = grab; drag.x += 20; rig.root->dispatchMouseDrag(drag);
    REQUIRE(rig.context.browserDrag); CHECK(rig.context.browserDrag->value == "synth3");
    CHECK(rig.context.browserDrag->label == "Alpha Synth"); rig.root->dispatchMouseUp(drag); rig.context.browserDrag.reset();
    rig.browser->beginTagEntry();
    for (const char ch : std::string("Bright")) rig.key(0, (uint32_t) ch);
    rig.key(juce::KeyPress::returnKey);
    CHECK(! rig.stored.empty());
    ui::BrowserTags reloaded; reloaded.loadJson(rig.stored);
    CHECK(ui::BrowserTags::matches(reloaded.tagsFor("plugin:synth3", {}), {{"User", "Bright"}}));
    CHECK(! rig.context.documentDirty);
    rig.browser->beginTagEntry(); rig.key(0, 'x'); rig.key(juce::KeyPress::escapeKey);
    CHECK(! ui::BrowserTags::matches(rig.browser->selectedTags(), {{"User", "x"}}));
    rig.browser->toggleTagFilter({"User", "Bright"}); rig.click(rig.browser->listRowBounds(1));
    REQUIRE(! rig.browser->selectedTags().empty());
    rig.browser->tagSelection({"User", "Bright"}, false); // removing the active tag empties the results
    CHECK(rig.browser->selectedTags().empty());
    rig.click(rig.browser->listRowBounds(1)); CHECK(rig.browser->selectedTags().empty());
    rig.click(rig.browser->filterChipBounds(2)); // x removes the active User filter
    CHECK(rig.browser->tagFilters().size() == 2);
    rig.click(rig.browser->listRowBounds(1)); CHECK(! rig.browser->selectedTags().empty());
    // Remove an automatically supplied tag through its visible x.
    const auto tags = rig.browser->selectedTags();
    for (size_t i = 0; i < tags.size(); ++i)
        if (tags[i] == ui::BrowserTag{"Creator", "Maker"})
        {
            auto chip = rig.browser->tagChipBounds((int) i); REQUIRE(! chip.isEmpty());
            chip.x = chip.right() - 10; chip.w = 8; rig.click(chip);
        }
    reloaded.loadJson(rig.stored);
    CHECK(! ui::BrowserTags::matches(reloaded.tagsFor("plugin:synth3", {{"Creator", "Maker"}}), {{"Creator", "Maker"}}));
}

TEST_CASE("browser icon rendering: VST VST3 and AU instruments and effects have distinct glyphs")
{
    BrowserRig rig;
    rig.context.browserPlugins.clear();
    int id = 0;
    for (const std::string format : {"VST", "VST3", "AudioUnit"})
        for (const bool instrument : {false, true})
        {
            const auto number = std::to_string(id++);
            rig.context.browserPlugins.push_back({number, number, "Maker", format, instrument, {}});
        }
    rig.click(rig.browser->listRowBounds(0)); // expand Maker
    std::set<std::vector<uint32_t>> glyphs;
    for (const float scale : {1.0f, 2.0f})
    {
        juce::TemporaryFile output(".png");
        REQUIRE(gfx::renderToPng(*rig.root, 1300, 800, scale, output.getFile().getFullPathName().toStdString()));
        const auto image = juce::ImageFileFormat::loadFrom(output.getFile()); REQUIRE(image.isValid());
        const auto origin = rig.browser->boundsInRoot();
        glyphs.clear();
        for (int row = 1; row <= 6; ++row)
        {
            const auto bounds = rig.browser->listRowBounds(row);
            std::vector<uint32_t> pixels;
            for (int y = 0; y < (int) (16 * scale); ++y)
                for (int x = 0; x < (int) (20 * scale); ++x)
                    pixels.push_back(image.getPixelAt((int) ((origin.x + bounds.x + 33) * scale) + x,
                                                     (int) ((origin.y + bounds.centreY() - 7) * scale) + y).getARGB());
            glyphs.insert(std::move(pixels));
        }
        CHECK(glyphs.size() == 6);
    }
}

TEST_CASE("browser tags: sample and native-device tags use stable library keys and filtered sample drags")
{
    BrowserRig rig;
    rig.context.browserFiles = {{"/tmp/Kick.wav", false}, {"/tmp/Kick.aif", false}, {"/tmp/Sub", true}};
    rig.browser->showFolder("/tmp");
    rig.click(rig.browser->listRowBounds(0));
    CHECK(ui::BrowserTags::matches(rig.browser->selectedTags(), {{"Type", "Sample"}, {"Format", "WAV"}}));
    rig.browser->tagSelection({"User", "Drums"}, true);
    rig.browser->toggleTagFilter({"User", "Drums"});
    CHECK(! rig.browser->selectedTags().empty()); // selected identity survives a matching filter
    const auto grab = rig.at(rig.browser->listRowBounds(0)); auto drag = grab; drag.x += 20;
    rig.root->dispatchMouseDown(grab); rig.root->dispatchMouseDrag(drag);
    REQUIRE(rig.context.browserDrag);
    CHECK(rig.context.browserDrag->kind == ui::BrowserDrag::Kind::Sample);
    CHECK(rig.context.browserDrag->value == "/tmp/Kick.wav"); rig.root->dispatchMouseUp(drag);
    ui::BrowserTags saved; saved.loadJson(rig.stored);
    CHECK(ui::BrowserTags::matches(saved.tagsFor("file:/tmp/Kick.wav", {}), {{"User", "Drums"}}));
    CHECK(saved.tagsFor("file:/tmp/Kick.aif", {}).empty());
    rig.browser->showCategory(3);
    CHECK(rig.browser->tagFilters().empty());
    rig.browser->toggleTagFilter({"Type", "Instrument"});
    rig.browser->toggleTagFilter({"Device", "Synthesizer"});
    rig.click(rig.browser->listRowBounds(1));
    CHECK(ui::BrowserTags::matches(rig.browser->selectedTags(), {{"Type", "Instrument"}, {"Device", "Synthesizer"}}));
}

TEST_CASE("browser tags: narrow filter controls wrap within the results column and hide without losing filters")
{
    BrowserRig rig;
    rig.context.theme.metrics.browserWidth = 260;
    rig.context.theme.metrics.browserSidebarWidth = 100;
    rig.root->layout();
    rig.browser->toggleTagFilter({"Type", "Instrument"});
    rig.browser->toggleTagFilter({"Format", "VST3"});
    for (const std::string group : {"Creator", "Device", "Format", "Type"})
    {
        const auto box = rig.browser->filterGroupBounds(group);
        CHECK(box.x >= 100); CHECK(box.right() <= rig.browser->width());
    }
    CHECK(rig.browser->filterGroupBounds("Type").y > rig.browser->filterGroupBounds("Creator").y);
    juce::TemporaryFile output(".png");
    REQUIRE(gfx::renderToPng(*rig.root, 1300, 800, 1, output.getFile().getFullPathName().toStdString()));
    const float listTop = rig.browser->listRowBounds(0).y;
    rig.click({rig.browser->width() - 25, 5, 18, 18});
    CHECK(rig.browser->filterGroupBounds("Type").isEmpty());
    CHECK(rig.browser->listRowBounds(0).y < listTop);
    CHECK(rig.browser->tagFilters().size() == 2);
}

TEST_CASE("browser tags: Cmd F leaves tag entry and searches without creating an unintended tag")
{
    BrowserRig rig;
    rig.click(rig.browser->listRowBounds(0)); // expand Maker
    rig.click(rig.browser->listRowBounds(1));
    rig.browser->beginTagEntry();
    ui::KeyEvent search; search.cmd = true; search.textChar = 'f'; rig.root->dispatchKey(search);
    for (const char ch : std::string("Echo")) rig.key(0, (uint32_t) ch);
    rig.key(juce::KeyPress::returnKey);
    CHECK(rig.stored.empty());
    const auto grab = rig.at(rig.browser->listRowBounds(1)); auto drag = grab; drag.x += 20;
    rig.root->dispatchMouseDown(grab); rig.root->dispatchMouseDrag(drag);
    REQUIRE(rig.context.browserDrag); CHECK(rig.context.browserDrag->value == "fx3"); rig.root->dispatchMouseUp(drag);
}
