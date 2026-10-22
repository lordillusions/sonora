#pragma once
// Sonora Prism preset browser (UI-28i): a bank list, the selected bank's groups and its presets with a search field,
// shown over the dashboard (the keyboard footer stays playable). Picking a preset loads it through the editor's library
// preset path; loading may rebuild the editor, so the editor stores state() before every load and reopens from it.
// The search field is Skia-drawn like the main browser's: typed characters arrive through keyPressed() while the
// browser has keyboard focus, and wantsTextInput() keeps them away from the computer MIDI keyboard and shortcuts.
#include "ui/Widget.h"
#include "plugins/SerumLibrary.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace sonora::ui
{
class WavetablePresetBrowser final : public Widget
{
public:
    using Library = std::vector<plugins::SerumLibraryItem>;
    struct State
    {
        bool open = false, focused = false;
        std::string bank, query;
        std::optional<std::string> group; // nullopt == every group of the bank
        int selected = -1;                // library index of the keyboard cursor
        float bankScroll = 0, groupScroll = 0, presetScroll = 0; // pixels scrolled in each column
    };
    static constexpr float kTop = 34, kColumnHeader = 18, kRow = 20, kFooter = 18, kBankWidth = 150, kGroupWidth = 150;

    WavetablePresetBrowser();
    // The scanned library (shared so a rebuilt editor does not rescan) and the bank folders on disk (empty banks too).
    void setLibrary(std::shared_ptr<const Library> items, std::vector<std::string> bankFolders);
    // The loaded preset's path: highlighted in the list.
    void setCurrent(std::string path) { current_ = std::move(path); repaint(); }
    // Opens on the current preset's bank (or the first bank) with the cursor on it.
    State initialState() const;
    void restore(State);
    const State& state() const { return state_; }

    const std::vector<std::string>& banks() const { return banks_; }
    std::vector<std::string> groups() const;     // the selected bank's groups
    std::vector<int> visiblePresets() const;     // library indices after bank, group and search filtering
    const Library& library() const { return *items_; }
    void setQuery(std::string);
    void selectBank(const std::string&);
    void selectGroup(std::optional<std::string>);

    // Geometry (local coordinates). Group row 0 is "All"; rows are list positions, not screen rows.
    Rect searchCell() const;
    Rect clearCell() const; // inside the search field, shown while there is a query
    Rect closeCell() const;
    Rect bankCell(int row) const;
    Rect groupCell(int row) const;
    Rect presetRowCell(int row) const;

    std::function<void(const std::string& path)> onLoad; // may destroy this widget
    std::function<void()> onClose;

    void layout() override;
    void paint(SkCanvas&, const Theme&) override;
    void mouseDown(const MouseEvent&) override;
    void mouseDoubleClick(const MouseEvent&) override;
    bool mouseWheel(const MouseEvent&) override;
    bool keyPressed(const KeyEvent&) override;
    void focusLost() override { state_.focused = false; repaint(); }
    bool wantsTextInput() const override { return state_.open && state_.focused; }

private:
    float footerHeight() const { return height() < 240 ? 0.0f : kFooter; } // the key hints yield to the lists when docked small
    Rect bankList() const;
    Rect groupList() const;
    Rect presetList() const;
    void moveSelection(int delta);
    void keepSelectionVisible(bool centre = false); // centre: an off-screen cursor lands mid-list (on open)
    void load(int index);
    static std::string bankLabel(const std::string&);
    static std::string groupLabel(const std::string&);

    std::shared_ptr<const Library> items_;
    std::vector<std::string> banks_;
    std::string current_;
    State state_;
    bool revealPending_ = false; // restored before the first layout: scroll to the cursor once sized
};
} // namespace sonora::ui
