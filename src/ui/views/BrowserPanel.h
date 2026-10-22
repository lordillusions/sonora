#pragma once
// Browser (UI-6): category search, folder navigation, persisted Places and sample audition.

#include "ui/Widget.h"
#include "ui/BrowserTags.h"
#include <set>

namespace sonora::ui
{

class BrowserPanel : public Widget
{
public:
    void paint(SkCanvas& c, const Theme& t) override;
    void layout() override;
    void mouseDown(const MouseEvent& e) override;
    void mouseDoubleClick(const MouseEvent& e) override;
    void mouseDrag(const MouseEvent& e) override;
    void mouseUp(const MouseEvent&) override { pendingDrag_.reset(); }
    bool mouseWheel(const MouseEvent& e) override;
    bool keyPressed(const KeyEvent& k) override;
    void focusSearch();
    void focusContent() { keyboardFocused_ = true; searchFocused_ = tagEditing_ = false; repaint(); }
    // Row rectangles in this panel's coordinates (hit-testing, tests): sidebar entry / filtered list row.
    Rect sidebarRowBounds(int index) const;
    Rect listRowBounds(int index) const;
    void showFolder(const std::string& path) { selectedCategory_ = 7; filters_.clear(); tagEditing_ = false; navigate(path); }
    void showCategory(int index) { selectCategory(index); repaint(); }
    void focusLost() override { searchFocused_ = keyboardFocused_ = tagEditing_ = false; repaint(); }
    bool wantsTextInput() const override { return searchFocused_ || tagEditing_; }
    // Audition lives in the right-hand content column. Shared with hit tests.
    Rect previewButton() const;
    Rect previewFooter() const;
    Rect tagFooter() const;
    Rect addTagBounds() const;
    Rect tagChipBounds(int index) const;
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
    };
    static const std::vector<Category>& categories();
    std::vector<Item> items(bool filtered = true, bool grouped = true) const;
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
    Rect sidebarArea() const;
    Rect listArea() const;
    std::vector<Rect> sidebarRows() const;

    class Splitter* splitter_ = nullptr;
    int selectedCategory_ = 9, selectedItem_ = -1;
    float scroll_ = 0;
    bool folderMode_ = false, searchFocused_ = false, keyboardFocused_ = false;
    std::string query_;
    bool filtersVisible_ = true, tagEditing_ = false;
    std::string tagText_;
    BrowserTags::Tags filters_;
    std::set<std::string> expandedGroups_, expandedFolders_;
    std::optional<BrowserDrag> pendingDrag_;
    float dragStartX_ = 0, dragStartY_ = 0;
};

} // namespace sonora::ui
