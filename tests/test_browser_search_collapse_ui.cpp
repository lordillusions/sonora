// UI-90: a search's category headings close and open with a click, like the reference's browser, and stay closed while
// that search stays (a refresh keeps them); another query opens them all again.
#include "TestFramework.h"
#include "ui/views/BrowserPanel.h"
#include "ui/views/MainLayout.h"
#include <juce_gui_basics/juce_gui_basics.h>

using namespace sonora;

namespace
{
struct SearchRig
{
    ui::UiContext context;
    std::unique_ptr<ui::MainLayout> root;
    ui::BrowserPanel* browser = nullptr;
    SearchRig()
    {
        context.detailVisible = false;
        context.theme.metrics.browserWidth = 720;
        context.theme.metrics.browserSidebarWidth = 170;
        context.browserPlugins = {{"Alpha Synth", "synth3", "Maker", "VST3", true, "Synth"},
                                  {"Echo Verb", "verb3", "Maker", "VST3", false, "Reverb"},
                                  {"Echo Delay", "delay3", "Other", "VST3", false, "Delay"},
                                  {"Echo Tape", "tape3", "Other", "VST3", false, "Delay"}};
        root = std::make_unique<ui::MainLayout>(context);
        root->setBounds({0, 0, 1300, 800});
        root->layout();
        for (const auto& child : root->children()) if (auto* b = dynamic_cast<ui::BrowserPanel*>(child.get())) browser = b;
        REQUIRE(browser);
    }
    void click(ui::Rect box)
    {
        const auto origin = browser->boundsInRoot();
        ui::MouseEvent e;
        e.x = origin.x + box.centreX();
        e.y = origin.y + box.centreY();
        root->dispatchMouseDown(e);
        root->dispatchMouseUp(e);
    }
    void key(int code, uint32_t ch = 0, bool cmd = false)
    {
        ui::KeyEvent e;
        e.keyCode = code;
        e.textChar = ch;
        e.cmd = cmd;
        root->dispatchKey(e);
    }
    void search(const std::string& text)
    {
        key('F', 'f', true); // Cmd+F focuses the browser's search field
        for (const char ch : text) key(0, (uint32_t) ch);
    }
    std::vector<std::string> rows() const
    {
        std::vector<std::string> out;
        for (int i = 0;; ++i)
        {
            auto label = browser->rowLabel(i);
            if (label.empty()) return out;
            out.push_back(std::move(label));
        }
    }
};
} // namespace

TEST_CASE("browser search: clicking a category heading collapses and expands its results while the query stays (UI-90)")
{
    SearchRig rig;
    rig.browser->showCategory(ui::BrowserPanel::PlugIns);
    rig.search("echo");
    REQUIRE(rig.browser->query() == "echo");
    using Rows = std::vector<std::string>;
    CHECK(rig.rows() == (Rows{"Maker", "Echo Verb", "Other", "Echo Delay", "Echo Tape"}));

    // The heading's triangle closes it; the other category keeps its results.
    rig.click(rig.browser->disclosureBounds(0));
    CHECK(rig.rows() == (Rows{"Maker", "Other", "Echo Delay", "Echo Tape"}));
    // A click on the heading's name closes the second one too, and opens it again.
    rig.click(rig.browser->listRowBounds(1));
    CHECK(rig.rows() == (Rows{"Maker", "Other"}));
    rig.click(rig.browser->listRowBounds(1));
    CHECK(rig.rows() == (Rows{"Maker", "Other", "Echo Delay", "Echo Tape"}));

    // A refresh of the library (new scan, folder publication) keeps what was closed while the query is the same.
    ++rig.context.browserContentRevision;
    rig.root->layout();
    rig.root->tickTree();
    CHECK(rig.rows() == (Rows{"Maker", "Other", "Echo Delay", "Echo Tape"}));
    // The arrow keys open and close the selected heading as well.
    rig.click(rig.browser->listRowBounds(0)); // opens Maker (selects it)
    CHECK(rig.rows() == (Rows{"Maker", "Echo Verb", "Other", "Echo Delay", "Echo Tape"}));
    rig.key(juce::KeyPress::leftKey);
    CHECK(rig.rows() == (Rows{"Maker", "Other", "Echo Delay", "Echo Tape"}));
    rig.key(juce::KeyPress::rightKey);
    CHECK(rig.rows() == (Rows{"Maker", "Echo Verb", "Other", "Echo Delay", "Echo Tape"}));
    rig.key(juce::KeyPress::leftKey);

    // Another query is another search: every heading opens again.
    rig.search("ech"); // Cmd+F selects the field's text: the typing replaces it
    CHECK(rig.browser->query() == "ech");
    CHECK(rig.rows() == (Rows{"Maker", "Echo Verb", "Other", "Echo Delay", "Echo Tape"}));

    // Without a search the headings keep their own open / closed state (closed until clicked, as before).
    rig.click(rig.browser->clearSearchBounds());
    REQUIRE(rig.browser->query().empty());
    CHECK(rig.rows() == (Rows{"Maker", "Other"}));
    rig.click(rig.browser->disclosureBounds(1));
    CHECK(rig.rows() == (Rows{"Maker", "Other", "Echo Delay", "Echo Tape"}));
}

TEST_CASE("browser search: All's category and group headings collapse under a search (UI-90)")
{
    SearchRig rig;
    rig.browser->showCategory(ui::BrowserPanel::AllItems);
    rig.search("echo");
    const auto all = rig.rows();
    const int plugIns = rig.browser->rowIndex("Plug-ins");
    REQUIRE(plugIns >= 0);
    CHECK(rig.browser->rowIndex("Echo Delay") > plugIns);
    // A group inside Plug-Ins closes on its own.
    const int other = rig.browser->rowIndex("Other");
    REQUIRE(other > plugIns);
    rig.click(rig.browser->disclosureBounds(other));
    CHECK(rig.browser->rowIndex("Echo Delay") < 0);
    CHECK(rig.browser->rowIndex("Echo Verb") > plugIns);
    // The category heading closes everything under it.
    rig.click(rig.browser->disclosureBounds(plugIns));
    CHECK(rig.browser->rowIndex("Echo Verb") < 0);
    CHECK(rig.browser->rowIndex("Maker") < 0);
    CHECK(rig.browser->rowIndex("Other") < 0);
    CHECK(rig.rows().size() < all.size());
    // Opening it again shows the group closed before still closed (same search).
    rig.click(rig.browser->disclosureBounds(plugIns));
    CHECK(rig.browser->rowIndex("Echo Verb") > plugIns);
    CHECK(rig.browser->rowIndex("Echo Delay") < 0);
    CHECK(rig.browser->rowIndex("Other") > plugIns);
    // The same words searched again after clearing are a new search: all open.
    rig.click(rig.browser->clearSearchBounds());
    rig.search("echo");
    CHECK(rig.rows() == all);
}
