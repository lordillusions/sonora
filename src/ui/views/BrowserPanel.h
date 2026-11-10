#pragma once
// Browser (UI-6): category search, folder navigation, persisted Places and sample audition.
// UI-78: the search field edits like a text field (selection, Cmd+A, double/triple click, clear button, Escape clears).
// LIB-3: Show in Browser (UiContext::browserReveal) opens a file's folder and selects its row.
// LIB-4: Show Similar Files lists UiContext::similarSamples under a "Similar to" chip.

#include "ui/Widget.h"
#include "ui/BrowserTags.h"
#include "ui/InlineEdit.h"
#include <array>
#include <set>

namespace sonora::ui
{

class BrowserPanel : public Widget
{
public:
    // Sidebar rows in order (categories(); custom Places follow). UI-64: All lists every Library category together.
    enum CategoryIndex : int
    {
        LibraryHeading, AllItems, Sounds, Drums, Instruments, AudioEffects, MidiEffects, PlugIns, Samples,
        PlacesHeading, Projects, CurrentProject, UserLibrary, Packs
    };
    void paint(SkCanvas& c, const Theme& t) override;
    void layout() override;
    void mouseDown(const MouseEvent& e) override;
    void mouseDoubleClick(const MouseEvent& e) override;
    void mouseDrag(const MouseEvent& e) override;
    void mouseUp(const MouseEvent&) override { pendingDrag_.reset(); selectingText_ = false; }
    bool mouseWheel(const MouseEvent& e) override;
    bool keyPressed(const KeyEvent& k) override;
    void tick() override;
    void focusSearch();
    void focusContent() { keyboardFocused_ = true; searchFocused_ = tagEditing_ = false; repaint(); }
    // UI-6b: the text caret of the search / new-tag field is drawn now (it blinks while either field has the focus).
    bool caretVisible() const;
    // Row rectangles in this panel's coordinates (hit-testing, tests): sidebar entry / filtered list row.
    Rect sidebarRowBounds(int index) const;
    Rect listRowBounds(int index) const;
    std::string rowLabel(int index) const; // tests: the label of a filtered list row (empty past the end)
    int rowIndex(const std::string& label) const; // tests/ui_snapshot: the first list row with that label, -1 if none
    // LIB-2: the disclosure triangle of a list row (headings, folders, devices with presets, preset folders); empty if none.
    Rect disclosureBounds(int index) const;
    void showFolder(const std::string& path) { leaveSimilar(); selectedCategory_ = Samples; filters_.clear(); tagEditing_ = false; navigate(path); }
    void showCategory(int index) { selectCategory(index); repaint(); }
    void focusLost() override { searchFocused_ = keyboardFocused_ = tagEditing_ = selectingText_ = false; repaint(); }
    // UI-78: the search field, its clear button (shown while there is text, a tag filter or a similar list) and state.
    Rect searchFieldBounds() const;
    Rect clearSearchBounds() const;
    bool clearSearchVisible() const;
    bool searchHasFocus() const { return searchFocused_; }
    const InlineTextEdit& searchText() const { return searchEdit_; }
    const std::string& query() const { return query_; }
    int selectedRow() const { return selectedItem_; }
    float scrollOffset() const { return scroll_; }
    // LIB-4: the "Similar to" chip above the similar list (its right end leaves the list).
    bool similarList() const;
    Rect similarChipBounds() const;
    bool wantsTextInput() const override { return searchFocused_ || tagEditing_; }
    // Audition lives in the right-hand content column. Shared with hit tests.
    Rect previewButton() const;
    Rect previewFooter() const;
    Rect tagFooter() const;
    Rect addTagBounds() const;
    Rect tagChipBounds(int index) const;
    Rect packButtonBounds(int listRow) const; // LIB-1: the Download / Cancel / Update button of a Packs row
    static int packsCategory();               // LIB-1: the sidebar index of Places > Packs
    Rect filterGroupBounds(const std::string& group) const;
    Rect filterChipBounds(int index) const;
    BrowserTags::Tags selectedTags() const;
    void beginTagEntry();
    void tagSelection(BrowserTag tag, bool enabled);
    void toggleTagFilter(BrowserTag tag);
    const BrowserTags::Tags& tagFilters() const { return filters_; }

private:
    void moveSelection(int delta);
    struct Category { const char* name; bool header; };
    struct Item
    {
        std::string label, path;
        bool directory = false, sample = false;
        std::optional<BrowserDrag> drag;
        std::string group{};
        bool heading = false;
        int depth = 0;
        std::string parentFolder{};
        std::string format{}, key{};
        bool instrument = false;
        BrowserTags::Tags tags{}, suggestedTags{};
        int section = -1; // UI-64: the Library category an All row comes from
        std::string packId{}; // LIB-1: a row of the Packs list ("@list" = the catalogue's status row)
        std::string hint{};   // LIB-4: the folder of a similar match, drawn dimmed at the right
        // LIB-2: a device row with presets or a preset folder row opens with this key in expandedFolders_ ("preset:<tag>",
        // "preset:<tag>/<folder>"); `open` is whether its children follow (expanded, or a search found presets in it).
        std::string disclosure{};
        bool open = false;
    };
    static const std::vector<Category>& categories();
    const std::vector<Item>& items(bool filtered = true, bool grouped = true) const;
    std::vector<Item> buildItems(bool filtered, bool grouped) const;
    void appendCategory(int category, std::vector<Item>& result, bool indexed) const;
    std::vector<Item> groupAll(std::vector<Item> result) const;
    std::vector<Item> attachPresets(std::vector<Item> rows) const; // LIB-2: preset folders and presets under device rows
    bool searchesPresets() const; // LIB-2: a search lists matching presets under their device (not in All: Sounds has them)
    static float rowInset(const Item& item);
    bool listsFolder() const { return ! similarList() && (folderMode_ || selectedCategory_ == AllItems); }
    struct ItemsKey
    {
        uint64_t contentRevision = 0, tagsRevision = 0;
        int category = -1;
        bool folderMode = false;
        std::string query, folder;
        BrowserTags::Tags filters;
        std::set<std::string> groups, folders;
        uint64_t similarRevision = 0;
    };
    mutable std::optional<ItemsKey> itemsKey_;
    mutable std::array<std::optional<std::vector<Item>>, 4> itemsCache_;
    mutable std::optional<std::set<std::string>> facetGroups_;
    std::vector<std::pair<std::string, Rect>> filterGroups() const;
    std::vector<Rect> filterChips() const;
    float filtersHeight() const;
    void showFilterMenu(const std::string& group);
    void showTagMenu();
    void changeTag(const Item& item, BrowserTag tag, bool enabled);
    void activate(const Item& item);
    void disclose(const Item& item, bool open);
    void showItemMenu(Item item);
    void selectCategory(int index);
    void navigate(const std::string& path);
    void restartCaret();
    void searchChanged(bool starting); // query_ follows searchEdit_; a new search re-reads the folder tree
    void clearSearch();                // text, tag filters and the similar list
    void leaveSimilar();
    void syncSimilar();                // a new similar list: selection and scroll start over
    void revealPending();              // LIB-3
    float similarRowHeight() const;
    void appendSimilar(std::vector<Item>& result) const;
    Rect sidebarArea() const;
    Rect listArea() const;
    std::vector<Rect> sidebarRows() const;
    // LIB-1 Packs list (docs/SAMPLE_PACKS.md): Sonora's downloadable packs and the reference factory packs folder.
    bool packList() const { return ! folderMode_ && selectedCategory_ == packsCategory(); }
    void appendPacks(std::vector<Item>& result) const;
    void paintPack(SkCanvas& c, const Theme& t, const Item& item, const Rect& row, bool selected) const;
    void packAction(const Item& item);
    void showPackMenu(const Item& item);

    class Splitter* splitter_ = nullptr;
    int selectedCategory_ = Projects, selectedItem_ = -1;
    float scroll_ = 0;
    bool folderMode_ = false, searchFocused_ = false, keyboardFocused_ = false;
    std::string query_;
    InlineTextEdit searchEdit_; // UI-78: query_ is its text
    bool selectingText_ = false;
    std::string revealing_, shownSimilar_;
    bool filtersVisible_ = true, tagEditing_ = false, allOpened_ = false;
    std::string tagText_;
    BrowserTags::Tags filters_;
    std::set<std::string> expandedGroups_, expandedFolders_;
    std::optional<BrowserDrag> pendingDrag_;
    uint32_t caretEpoch_ = 0;
    bool caretShown_ = false;
    float dragStartX_ = 0, dragStartY_ = 0;
};

} // namespace sonora::ui
