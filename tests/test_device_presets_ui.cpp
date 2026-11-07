// LIB-2: device presets in the Browser through real mouse and key events: disclosure under the device row, folders,
// drag payload, double-click load, search, Sounds by folder and All listing a preset once.
#include "TestFramework.h"
#include "ui/views/BrowserPanel.h"
#include "ui/views/MainLayout.h"
#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>

using namespace sonora;

namespace
{
struct PresetRig
{
    ui::UiContext context;
    std::unique_ptr<ui::MainLayout> root;
    ui::BrowserPanel* browser = nullptr;
    std::vector<ui::BrowserDrag> dropped;
    std::vector<int> droppedTracks;
    PresetRig()
    {
        context.detailVisible = false;
        context.theme.metrics.browserWidth = 720;
        context.theme.metrics.browserSidebarWidth = 170;
        context.selectedTrack = 0;
        // Sorted by device tag as io::scanDevicePresets returns them (folders first, then loose presets).
        context.devicePresets = {{"/lib/Presets/Audio Effects/EQ Eight/Sonora Air.adv", "Sonora Air", "", "Eq8", "EQ Eight"},
                                 {"/lib/Devices/Instruments/Wavetable/Bass/Deep Wave.adv", "Deep Wave", "Bass", "InstrumentVector", "Sonora Wavetable"},
                                 {"/lib/Devices/Instruments/Wavetable/Bass/Slap Wave.adv", "Slap Wave", "Bass", "InstrumentVector", "Sonora Wavetable"},
                                 {"/lib/Devices/Instruments/Wavetable/Pads/Soft Pad.adv", "Soft Pad", "Pads", "InstrumentVector", "Sonora Wavetable"},
                                 {"/lib/Devices/Instruments/Wavetable/Plain Lead.adv", "Plain Lead", "", "InstrumentVector", "Sonora Wavetable"}};
        context.dropBrowserItem = [this](const ui::BrowserDrag& drag, int track, double, int) { dropped.push_back(drag); droppedTracks.push_back(track); };
        root = std::make_unique<ui::MainLayout>(context); root->setBounds({0, 0, 1300, 800}); root->layout();
        for (const auto& child : root->children()) if (auto* b = dynamic_cast<ui::BrowserPanel*>(child.get())) browser = b;
        REQUIRE(browser);
    }
    ui::MouseEvent at(ui::Rect box, float dx = 0)
    {
        const auto origin = browser->boundsInRoot();
        ui::MouseEvent e; e.x = origin.x + box.centreX() + dx; e.y = origin.y + box.centreY(); return e;
    }
    void click(ui::Rect box, float dx = 0) { const auto e = at(box, dx); root->dispatchMouseDown(e); root->dispatchMouseUp(e); }
    void open(const std::string& label)
    {
        const auto box = browser->disclosureBounds(browser->rowIndex(label));
        REQUIRE(! box.isEmpty());
        click(box);
    }
    void key(int code, uint32_t ch = 0) { ui::KeyEvent e; e.keyCode = code; e.textChar = ch; root->dispatchKey(e); }
    void search(const std::string& text)
    {
        ui::KeyEvent find; find.cmd = true; find.textChar = 'f'; root->dispatchKey(find);
        for (int i = 0; i < 20; ++i) key(juce::KeyPress::backspaceKey);
        for (char ch : text) key(0, (uint32_t) ch);
    }
    std::vector<std::string> rows() const
    {
        std::vector<std::string> out;
        for (int i = 0; ! browser->rowLabel(i).empty(); ++i) out.push_back(browser->rowLabel(i));
        return out;
    }
};
}

TEST_CASE("browser presets (LIB-2): a device row opens to its preset folders and presets; drag, double-click and keys")
{
    PresetRig rig;
    rig.browser->showCategory(ui::BrowserPanel::Instruments);
    rig.click(rig.browser->listRowBounds(rig.browser->rowIndex("Synthesis")));
    const int wavetable = rig.browser->rowIndex("Sonora Wavetable");
    REQUIRE(wavetable >= 0);
    CHECK(! rig.browser->disclosureBounds(wavetable).isEmpty());
    CHECK(rig.browser->disclosureBounds(rig.browser->rowIndex("Sonora FM")).isEmpty()); // no presets, no triangle
    CHECK(rig.browser->rowIndex("Bass") < 0);

    // A click on the name selects the device (and could drag it); only the triangle opens it.
    rig.click(rig.browser->listRowBounds(wavetable));
    CHECK(rig.browser->rowIndex("Bass") < 0);
    rig.open("Sonora Wavetable");
    auto rows = rig.rows();
    const auto after = std::find(rows.begin(), rows.end(), "Sonora Wavetable");
    REQUIRE(after + 3 < rows.end());
    CHECK(*(after + 1) == "Bass"); CHECK(*(after + 2) == "Pads"); CHECK(*(after + 3) == "Plain Lead");
    CHECK(rig.browser->rowIndex("Slap Wave") < 0); // folders start closed
    rig.open("Bass");
    CHECK(rig.browser->rowIndex("Deep Wave") == rig.browser->rowIndex("Bass") + 1);
    CHECK(rig.browser->rowIndex("Slap Wave") == rig.browser->rowIndex("Bass") + 2);
    CHECK(rig.browser->rowIndex("Pads") == rig.browser->rowIndex("Bass") + 3);
    CHECK(rig.browser->listRowBounds(0).x < rig.browser->disclosureBounds(rig.browser->rowIndex("Bass")).x);

    // Dragging a preset carries its file; the tags say what it is.
    const auto grab = rig.at(rig.browser->listRowBounds(rig.browser->rowIndex("Slap Wave")), 40);
    rig.root->dispatchMouseDown(grab); auto drag = grab; drag.x += 20; rig.root->dispatchMouseDrag(drag);
    REQUIRE(rig.context.browserDrag);
    CHECK(rig.context.browserDrag->kind == ui::BrowserDrag::Kind::DevicePreset);
    CHECK(rig.context.browserDrag->value == "/lib/Devices/Instruments/Wavetable/Bass/Slap Wave.adv");
    CHECK(rig.context.browserDrag->label == "Slap Wave");
    rig.root->dispatchMouseUp(drag); rig.context.browserDrag.reset();
    CHECK(ui::BrowserTags::matches(rig.browser->selectedTags(), {{"Type", "Preset"}, {"Sound", "Bass"}, {"Device", "Sonora Wavetable"}}));

    // Left goes from a preset to its folder; Return loads the selected preset on the selected track.
    rig.key(juce::KeyPress::leftKey);
    rig.key(juce::KeyPress::returnKey); // Return on the folder closes it
    CHECK(rig.browser->rowIndex("Slap Wave") < 0);
    rig.key(juce::KeyPress::rightKey);
    CHECK(rig.browser->rowIndex("Slap Wave") > 0);

    // A double-click on a preset loads it (like a device) on the selected track.
    auto twice = rig.at(rig.browser->listRowBounds(rig.browser->rowIndex("Plain Lead")), 40);
    rig.root->dispatchMouseDown(twice); rig.root->dispatchMouseUp(twice);
    twice.clicks = 2; rig.root->dispatchMouseDown(twice); rig.root->dispatchMouseUp(twice);
    REQUIRE(rig.dropped.size() == 1);
    CHECK(rig.dropped[0].kind == ui::BrowserDrag::Kind::DevicePreset);
    CHECK(rig.dropped[0].value == "/lib/Devices/Instruments/Wavetable/Plain Lead.adv");
    CHECK(rig.droppedTracks[0] == 0);

    // Closing the device hides everything below it.
    rig.open("Sonora Wavetable");
    CHECK(rig.browser->rowIndex("Bass") < 0); CHECK(rig.browser->rowIndex("Plain Lead") < 0);

    // Audio effect presets hang under their device the same way.
    rig.browser->showCategory(ui::BrowserPanel::AudioEffects);
    rig.click(rig.browser->listRowBounds(rig.browser->rowIndex("EQ & Filters")));
    rig.open("EQ Eight");
    CHECK(rig.browser->rowIndex("Sonora Air") == rig.browser->rowIndex("EQ Eight") + 1);
}

TEST_CASE("browser presets (LIB-2): search finds presets by name; Sounds groups them by folder; All lists each once")
{
    PresetRig rig;
    rig.browser->showCategory(ui::BrowserPanel::Instruments);
    rig.search("slap wave");
    CHECK(rig.rows() == std::vector<std::string>({"Synthesis", "Sonora Wavetable", "Bass", "Slap Wave"}));
    rig.search("bass"); // a folder name matches all of its presets
    auto rows = rig.rows();
    CHECK(std::count(rows.begin(), rows.end(), std::string("Deep Wave")) == 1);
    CHECK(std::count(rows.begin(), rows.end(), std::string("Soft Pad")) == 0);

    rig.search("");
    rig.browser->showCategory(ui::BrowserPanel::Sounds);
    CHECK(rig.rows() == std::vector<std::string>({"Bass", "Pads", "Sonora Wavetable"})); // instruments only; loose ones by device
    rig.click(rig.browser->listRowBounds(0));
    CHECK(rig.rows() == std::vector<std::string>({"Bass", "Deep Wave", "Slap Wave", "Pads", "Sonora Wavetable"}));
    const auto grab = rig.at(rig.browser->listRowBounds(2));
    rig.root->dispatchMouseDown(grab); auto drag = grab; drag.x += 20; rig.root->dispatchMouseDrag(drag);
    REQUIRE(rig.context.browserDrag);
    CHECK(rig.context.browserDrag->kind == ui::BrowserDrag::Kind::DevicePreset);
    CHECK(rig.context.browserDrag->label == "Slap Wave");
    rig.root->dispatchMouseUp(drag); rig.context.browserDrag.reset();

    rig.browser->showCategory(ui::BrowserPanel::AllItems);
    rig.search("slap");
    rows = rig.rows();
    CHECK(std::count(rows.begin(), rows.end(), std::string("Slap Wave")) == 1);
    CHECK(rows.size() >= 3);
    CHECK(rows[0] == "Sounds");
    CHECK(rows[1] == "Bass");
    CHECK(rows[2] == "Slap Wave");
}
