// LIB-1: the Browser's Packs list (docs/SAMPLE_PACKS.md): buttons, opt-in row, Style facets, opening a pack and back.
#include "TestFramework.h"
#include "ui/views/BrowserPanel.h"
#include "ui/views/MainLayout.h"

#include <juce_gui_basics/juce_gui_basics.h>

using namespace sonora;

namespace
{
// Native paths: on Windows JUCE reads "/packs" as "C:\packs", so the pack's parent must be built the same way.
std::string packsRoot() { return juce::File("/packs").getFullPathName().toStdString(); }
std::string houseKit() { return juce::File("/packs").getChildFile("Sonora House Kit").getFullPathName().toStdString(); }

struct PacksRig
{
    ui::UiContext context;
    std::unique_ptr<ui::MainLayout> root;
    ui::BrowserPanel* browser = nullptr;
    std::vector<std::string> downloads, cancels, opened;
    int listRequests = 0, refreshes = 0;
    explicit PacksRig(float browserWidth = 420)
    {
        using State = ui::UiContext::SamplePack::State;
        context.detailVisible = false;
        context.theme.metrics.browserWidth = browserWidth;
        context.theme.metrics.browserSidebarWidth = 150;
        context.packsPath.clear();
        context.samplePacksRoot = packsRoot();
        context.samplePackList = ui::UiContext::SamplePackList::Hidden;
        context.samplePacks = {
            {"house", "Sonora House Kit", "1.0", {}, "CC0-1.0", houseKit(), {}, {"House"}, {"Drums"}, 5'000'000, 0, State::Installed},
            {"techno", "Sonora Techno Kit", "1.0", {}, "CC0-1.0", {}, {}, {"Techno"}, {"Drums"}, 6'000'000, 2'000'000, State::Downloading},
            {"ambient", "Sonora Ambient Textures", "1.0", {}, "CC0-1.0", {}, {}, {"Ambient"}, {"Pads"}, 35'000'000, 0, State::Available},
        };
        context.showSamplePackList = [this] { ++listRequests; };
        context.refreshSamplePacks = [this] { ++refreshes; };
        context.downloadSamplePack = [this](const std::string& id) { downloads.push_back(id); };
        context.cancelSamplePack = [this](const std::string& id) { cancels.push_back(id); };
        context.openBrowserFolder = [this](const std::string& path) { opened.push_back(path); context.browserFolder = path; ++context.browserContentRevision; };
        root = std::make_unique<ui::MainLayout>(context);
        root->setBounds({0, 0, 1300, 800});
        root->layout();
        for (const auto& child : root->children())
            if (auto* b = dynamic_cast<ui::BrowserPanel*>(child.get())) browser = b;
        REQUIRE(browser);
        browser->showCategory(ui::BrowserPanel::packsCategory());
    }
    ui::MouseEvent at(ui::Rect box) const
    {
        const auto origin = browser->boundsInRoot();
        ui::MouseEvent e; e.x = origin.x + box.centreX(); e.y = origin.y + box.centreY(); return e;
    }
    void click(ui::Rect box) { const auto e = at(box); root->dispatchMouseDown(e); root->dispatchMouseUp(e); }
    void doubleClick(ui::Rect box)
    {
        auto e = at(box);
        root->dispatchMouseDown(e); root->dispatchMouseUp(e);
        e.clicks = 2;
        root->dispatchMouseDown(e); root->dispatchMouseUp(e);
    }
};
} // namespace

TEST_CASE("packs list: Get Packs row, Download and Cancel buttons, Style facet, open a pack and come back")
{
    PacksRig rig;
    CHECK(ui::BrowserPanel::packsCategory() > 0);
    CHECK(rig.refreshes == 1); // opening Packs rescans, but fetches nothing by itself
    CHECK(rig.listRequests == 0);
    CHECK(rig.downloads.empty());

    // Row 0 is the opt-in "Get Packs..." row; double-clicking it is what fetches the catalogue.
    rig.doubleClick(rig.browser->listRowBounds(0));
    CHECK(rig.listRequests == 1);

    // Rows follow the app's order: installed, downloading, available.
    rig.click(rig.browser->packButtonBounds(3));
    CHECK(rig.downloads == std::vector<std::string>{"ambient"});
    rig.click(rig.browser->packButtonBounds(2));
    CHECK(rig.cancels == std::vector<std::string>{"techno"});
    rig.click(rig.browser->packButtonBounds(1)); // installed: no button
    CHECK(rig.downloads.size() == 1);
    CHECK(rig.cancels.size() == 1);

    // Selecting a row shows its tags; the Style facet filters the packs.
    rig.click(rig.browser->listRowBounds(3));
    CHECK(ui::BrowserTags::matches(rig.browser->selectedTags(), {{"Style", "Ambient"}, {"Pack", "Available"}}));
    CHECK(! rig.browser->filterGroupBounds("Style").isEmpty());
    rig.browser->toggleTagFilter({"Style", "House"});
    rig.doubleClick(rig.browser->listRowBounds(0));
    REQUIRE(rig.opened.size() == 1);
    CHECK(rig.opened[0] == houseKit());

    // Up from the pack's folder returns to the Packs list instead of listing the packs folder.
    const auto origin = rig.browser->boundsInRoot();
    ui::MouseEvent up; up.x = origin.x + 12; up.y = origin.y + 14;
    rig.root->dispatchMouseDown(up); rig.root->dispatchMouseUp(up);
    CHECK(rig.opened.size() == 1);
    rig.browser->toggleTagFilter({"Style", "House"});
    rig.click(rig.browser->packButtonBounds(3));
    CHECK(rig.downloads.size() == 2);
}

TEST_CASE("packs list: the reference factory packs folder opens and Up returns to the list")
{
    const auto factory = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("sonora-factory-packs-test");
    factory.createDirectory();
    PacksRig rig;
    rig.context.packsPath = factory.getFullPathName().toStdString();
    ++rig.context.browserContentRevision;
    rig.doubleClick(rig.browser->listRowBounds(1)); // row 0 = Get Packs, row 1 = the factory folder
    REQUIRE(rig.opened.size() == 1);
    CHECK(rig.opened[0] == rig.context.packsPath);
    const auto origin = rig.browser->boundsInRoot();
    ui::MouseEvent up; up.x = origin.x + 12; up.y = origin.y + 14;
    rig.root->dispatchMouseDown(up); rig.root->dispatchMouseUp(up);
    CHECK(rig.opened.size() == 1);
    rig.click(rig.browser->packButtonBounds(4)); // the list again: Get Packs, factory, house, techno, ambient
    CHECK(rig.downloads == std::vector<std::string>{"ambient"});
    factory.deleteRecursively();
}

TEST_CASE("packs list: files of an installed pack carry the pack's tags")
{
    PacksRig rig(600);
    rig.context.samplePackFileTags["/packs/Sonora House Kit/Kicks/House Kick 01.wav"] = {{"Style", "House"}, {"Sound", "Kick"}};
    rig.context.browserFiles = {{"/packs/Sonora House Kit/Kicks/House Kick 01.wav", false}, {"/elsewhere/Other.wav", false}};
    rig.browser->showFolder("/packs/Sonora House Kit/Kicks");
    rig.context.browserFiles = {{"/packs/Sonora House Kit/Kicks/House Kick 01.wav", false}, {"/elsewhere/Other.wav", false}};
    ++rig.context.browserContentRevision;
    rig.browser->toggleTagFilter({"Sound", "Kick"});
    rig.click(rig.browser->listRowBounds(0));
    CHECK(ui::BrowserTags::matches(rig.browser->selectedTags(), {{"Sound", "Kick"}, {"Style", "House"}, {"Type", "Sample"}}));
}
