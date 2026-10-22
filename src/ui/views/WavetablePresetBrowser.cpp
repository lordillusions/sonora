#include "ui/views/WavetablePresetBrowser.h"

#include "gfx/Fonts.h"
#include "ui/Painter.h"
#include "ui/WheelScale.h"

#include "include/core/SkCanvas.h"
#include "include/core/SkPaint.h"

#include <juce_gui_basics/juce_gui_basics.h>
#include <algorithm>

namespace sonora::ui
{
namespace
{
// The dashboard's palette (WavetableEditor.cpp), so the browser reads as part of the synth.
constexpr SkColor kBackground = 0xff11171f, kPanel = 0xff202a35, kRaised = 0xff2d3946, kLine = 0xff3b4b5c;
constexpr SkColor kText = 0xffe5edf7, kDim = 0xff96a9bd, kCyan = 0xff43d9e7, kField = 0xff0b1118, kSelection = 0xff246980;
constexpr float kPad = 8, kGap = 6;
} // namespace

WavetablePresetBrowser::WavetablePresetBrowser() : items_(std::make_shared<Library>()) { bypassMapMode = true; }

void WavetablePresetBrowser::setLibrary(std::shared_ptr<const Library> items, std::vector<std::string> bankFolders)
{
    items_ = items ? std::move(items) : std::make_shared<Library>();
    banks_ = plugins::serumLibraryBanks(*items_);
    for (auto& bank : bankFolders)
        if (std::find(banks_.begin(), banks_.end(), bank) == banks_.end()) banks_.push_back(std::move(bank));
    // Named banks in natural order; the unnamed bank of loose files last.
    std::stable_sort(banks_.begin(), banks_.end(), [](const auto& a, const auto& b) {
        if (a.empty() != b.empty()) return b.empty();
        return juce::String(a).compareNatural(juce::String(b)) < 0;
    });
    repaint();
}

WavetablePresetBrowser::State WavetablePresetBrowser::initialState() const
{
    State state;
    state.open = state.focused = true;
    if (const int index = plugins::serumLibraryFind(*items_, current_); index >= 0 && items_->at((size_t) index).path == current_)
    {
        state.bank = plugins::serumLibraryBank(items_->at((size_t) index));
        state.selected = index;
    }
    else if (! banks_.empty())
    {
        // The first bank that has presets ("Factory" before an empty "S1 Presets").
        const auto filled = plugins::serumLibraryBanks(*items_);
        state.bank = filled.empty() ? banks_.front() : *std::find_first_of(banks_.begin(), banks_.end(), filled.begin(), filled.end());
    }
    return state;
}

void WavetablePresetBrowser::restore(State state)
{
    state_ = std::move(state);
    revealPending_ = height() <= 0;
    if (! revealPending_) keepSelectionVisible(true);
    repaint();
}
void WavetablePresetBrowser::layout()
{
    if (revealPending_ && height() > 0) { revealPending_ = false; keepSelectionVisible(true); }
}

std::vector<std::string> WavetablePresetBrowser::groups() const { return plugins::serumLibraryGroups(*items_, state_.bank); }
std::vector<int> WavetablePresetBrowser::visiblePresets() const
{
    return plugins::serumLibraryFilter(*items_, state_.bank, state_.group, state_.query);
}

void WavetablePresetBrowser::setQuery(std::string query)
{
    if (query == state_.query) return;
    state_.query = std::move(query);
    state_.presetScroll = 0;
    keepSelectionVisible();
    repaint();
}
void WavetablePresetBrowser::selectBank(const std::string& bank)
{
    if (bank != state_.bank) { state_.bank = bank; state_.group.reset(); state_.groupScroll = state_.presetScroll = 0; }
    repaint();
}
void WavetablePresetBrowser::selectGroup(std::optional<std::string> group)
{
    if (group != state_.group) { state_.group = std::move(group); state_.presetScroll = 0; }
    repaint();
}

std::string WavetablePresetBrowser::bankLabel(const std::string& bank) { return bank.empty() ? "Loose Presets" : bank; }
std::string WavetablePresetBrowser::groupLabel(const std::string& group) { return group.empty() ? "Other" : group; }

Rect WavetablePresetBrowser::bankList() const
{
    return {kPad, kTop + kColumnHeader, kBankWidth, std::max(0.0f, height() - kTop - kColumnHeader - footerHeight() - 2)};
}
Rect WavetablePresetBrowser::groupList() const
{
    auto list = bankList(); list.x += kBankWidth + kGap; list.w = kGroupWidth; return list;
}
Rect WavetablePresetBrowser::presetList() const
{
    auto list = groupList(); list.x += kGroupWidth + kGap; list.w = std::max(0.0f, width() - list.x - kPad); return list;
}

Rect WavetablePresetBrowser::searchCell() const
{
    const float left = 112, right = width() - 150;
    return {left, 6, std::max(60.0f, right - left), kTop - 12};
}
Rect WavetablePresetBrowser::clearCell() const
{
    if (state_.query.empty()) return {};
    const auto field = searchCell();
    return {field.right() - field.h, field.y, field.h, field.h};
}
Rect WavetablePresetBrowser::closeCell() const { return {width() - kPad - 26, 5, 26, kTop - 10}; }
Rect WavetablePresetBrowser::bankCell(int row) const
{
    const auto list = bankList();
    return {list.x, list.y + (float) row * kRow - state_.bankScroll, list.w, kRow};
}
Rect WavetablePresetBrowser::groupCell(int row) const
{
    const auto list = groupList();
    return {list.x, list.y + (float) row * kRow - state_.groupScroll, list.w, kRow};
}
Rect WavetablePresetBrowser::presetRowCell(int row) const
{
    const auto list = presetList();
    return {list.x, list.y + (float) row * kRow - state_.presetScroll, list.w, kRow};
}

void WavetablePresetBrowser::paint(SkCanvas& canvas, const Theme&)
{
    paint::fill(canvas, localBounds(), kBackground);
    paint::stroke(canvas, localBounds(), kLine);
    // Top bar: title, search field, result count, close.
    paint::text(canvas, "PRESETS", {kPad + 2, 0, 100, kTop}, kText, 12, Align::Left, true);
    const auto field = searchCell();
    paint::fillRounded(canvas, field, 4, kField);
    {
        SkPaint frame; frame.setAntiAlias(true); frame.setStyle(SkPaint::kStroke_Style); frame.setStrokeWidth(1);
        frame.setColor(state_.focused ? kCyan : kLine);
        canvas.drawRoundRect(field.reduced(0.5f).sk(), 4, 4, frame);
        // Magnifier: a ring and a short handle.
        SkPaint glass; glass.setAntiAlias(true); glass.setStyle(SkPaint::kStroke_Style); glass.setStrokeWidth(1.4f); glass.setColor(kDim);
        const float cx = field.x + 12, cy = field.centreY() - 1;
        canvas.drawCircle(cx, cy, 4.5f, glass);
        canvas.drawLine(cx + 3.2f, cy + 3.2f, cx + 6.5f, cy + 6.5f, glass);
    }
    const Rect textArea{field.x + 24, field.y, field.w - 24 - (state_.query.empty() ? 6 : field.h), field.h};
    if (state_.query.empty())
        paint::text(canvas, state_.focused ? "" : "Search presets", textArea, kDim, 10.5f);
    else
        paint::text(canvas, state_.query, textArea, kText, 10.5f);
    if (state_.focused)
    {
        const float caret = std::min(textArea.right() - 1, textArea.x + gfx::measureText(state_.query, 10.5f) + 1);
        paint::line(canvas, caret, field.y + 5, caret, field.bottom() - 5, kCyan);
    }
    if (const auto clear = clearCell(); ! clear.isEmpty())
    {
        const auto c = clear.reduced(6);
        paint::line(canvas, c.x, c.y, c.right(), c.bottom(), kDim, 1.4f);
        paint::line(canvas, c.right(), c.y, c.x, c.bottom(), kDim, 1.4f);
    }
    const auto presets = visiblePresets();
    paint::text(canvas, std::to_string(presets.size()) + (presets.size() == 1 ? " preset" : " presets"),
                {field.right() + 10, 0, closeCell().x - field.right() - 16, kTop}, kDim, 10, Align::Right);
    {
        const auto close = closeCell();
        paint::fillRounded(canvas, close, 3, kRaised);
        const auto c = close.reduced(8.5f, (close.h - (close.w - 17)) * 0.5f);
        paint::line(canvas, c.x, c.y, c.right(), c.bottom(), kText, 1.5f);
        paint::line(canvas, c.right(), c.y, c.x, c.bottom(), kText, 1.5f);
    }

    const auto column = [&](Rect list, const char* title, const char* trailing) {
        const Rect header{list.x, list.y - kColumnHeader, list.w, kColumnHeader};
        paint::text(canvas, title, header.reduced(6, 0), kDim, 9, Align::Left, true);
        if (trailing) paint::text(canvas, trailing, header.reduced(6, 0), kDim, 9, Align::Right, true);
        paint::fill(canvas, list, kPanel);
    };
    const auto row = [&](SkCanvas& c, Rect cell, Rect list, const std::string& label, const std::string& count, bool chosen, bool cursor) {
        if (cell.bottom() <= list.y || cell.y >= list.bottom()) return;
        if (chosen) paint::fill(c, cell.reduced(1, 0.5f), kSelection);
        else if (cursor) paint::fill(c, cell.reduced(1, 0.5f), kRaised);
        paint::text(c, label, {cell.x + 8, cell.y, cell.w - 50, cell.h}, kText, 10.5f);
        if (! count.empty()) paint::text(c, count, {cell.right() - 46, cell.y, 40, cell.h}, chosen ? kText : kDim, 9.5f, Align::Right);
    };

    // Banks with their preset counts.
    const auto banks = bankList();
    column(banks, "BANK", nullptr);
    canvas.save(); canvas.clipRect(banks.sk());
    for (size_t i = 0; i < banks_.size(); ++i)
    {
        const auto count = plugins::serumLibraryFilter(*items_, banks_[i], std::nullopt, "").size();
        row(canvas, bankCell((int) i), banks, bankLabel(banks_[i]), std::to_string(count), banks_[i] == state_.bank, false);
    }
    canvas.restore();

    // Groups of the selected bank: "All" first.
    const auto groupArea = groupList();
    column(groupArea, "GROUP", nullptr);
    canvas.save(); canvas.clipRect(groupArea.sk());
    const auto groupNames = groups();
    row(canvas, groupCell(0), groupArea, "All", std::to_string(plugins::serumLibraryFilter(*items_, state_.bank, std::nullopt, "").size()),
        ! state_.group.has_value(), false);
    for (size_t i = 0; i < groupNames.size(); ++i)
        row(canvas, groupCell((int) i + 1), groupArea, groupLabel(groupNames[i]),
            std::to_string(plugins::serumLibraryFilter(*items_, state_.bank, groupNames[i], "").size()), state_.group == groupNames[i], false);
    canvas.restore();

    // Presets: name, then the folder below the bank; the loaded one in cyan, the cursor raised.
    const auto list = presetList();
    column(list, "NAME", "FOLDER");
    canvas.save(); canvas.clipRect(list.sk());
    const float nameWidth = std::max(80.0f, list.w * 0.58f);
    for (size_t i = 0; i < presets.size(); ++i)
    {
        const auto cell = presetRowCell((int) i);
        if (cell.bottom() <= list.y || cell.y >= list.bottom()) continue;
        const auto& item = items_->at((size_t) presets[i]);
        const bool loaded = item.path == current_, cursor = presets[i] == state_.selected;
        if (cursor) paint::fill(canvas, cell.reduced(1, 0.5f), state_.focused ? kSelection : kRaised);
        if (loaded) paint::fill(canvas, {cell.x + 1, cell.y + 3, 3, cell.h - 6}, kCyan);
        paint::text(canvas, item.name, {cell.x + 10, cell.y, nameWidth - 14, cell.h}, loaded ? kCyan : kText, 10.5f, Align::Left, loaded);
        const auto folder = juce::String(item.folder).fromFirstOccurrenceOf("/", false, false).toStdString();
        paint::text(canvas, folder, {cell.x + nameWidth, cell.y, cell.w - nameWidth - 8, cell.h}, kDim, 9.5f);
    }
    if (presets.empty())
    {
        const std::string message = items_->empty() ? "No presets yet. Save one with Save Preset... in the preset menu."
                                  : state_.query.empty() ? "This bank has no presets" : "No presets match \"" + state_.query + "\"";
        paint::text(canvas, message, {list.x + 12, list.y + 10, list.w - 24, kRow}, kDim, 10.5f);
    }
    canvas.restore();

    if (footerHeight() > 0)
        paint::text(canvas, "Click loads  -  Up / Down step and load  -  Double-click or Return closes  -  Esc clears the search, then closes",
                    {kPad + 2, height() - kFooter, width() - 2 * kPad, kFooter}, kDim, 9);
}

void WavetablePresetBrowser::mouseDown(const MouseEvent& event)
{
    state_.focused = true;
    if (closeCell().contains(event.x, event.y)) { if (onClose) onClose(); return; }
    if (clearCell().contains(event.x, event.y)) { setQuery({}); return; }
    if (searchCell().contains(event.x, event.y)) { repaint(); return; }
    if (bankList().contains(event.x, event.y))
    {
        for (size_t i = 0; i < banks_.size(); ++i)
            if (bankCell((int) i).contains(event.x, event.y)) selectBank(banks_[i]);
        return;
    }
    if (groupList().contains(event.x, event.y))
    {
        if (groupCell(0).contains(event.x, event.y)) { selectGroup(std::nullopt); return; }
        const auto names = groups();
        for (size_t i = 0; i < names.size(); ++i)
            if (groupCell((int) i + 1).contains(event.x, event.y)) selectGroup(names[i]);
        return;
    }
    if (presetList().contains(event.x, event.y))
    {
        const auto presets = visiblePresets();
        for (size_t i = 0; i < presets.size(); ++i)
            if (presetRowCell((int) i).contains(event.x, event.y)) { load(presets[i]); return; } // `this` may be gone
    }
    repaint();
}

void WavetablePresetBrowser::mouseDoubleClick(const MouseEvent& event)
{
    // The first click already loaded the row, possibly in a previous instance of this widget; the second one closes.
    if (presetList().contains(event.x, event.y))
    {
        const auto presets = visiblePresets();
        for (size_t i = 0; i < presets.size(); ++i)
            if (presetRowCell((int) i).contains(event.x, event.y))
            {
                if (items_->at((size_t) presets[i]).path != current_) { load(presets[i]); return; }
                if (onClose) onClose();
                return;
            }
    }
    mouseDown(event);
}

bool WavetablePresetBrowser::mouseWheel(const MouseEvent& event)
{
    const auto scroll = [&](float& offset, Rect list, size_t rows) {
        const float maximum = std::max(0.0f, (float) rows * kRow - list.h);
        offset = std::clamp(offset - event.wheelDeltaY * kWheelPixels, 0.0f, maximum);
        repaint();
        return true;
    };
    if (bankList().contains(event.x, event.y)) return scroll(state_.bankScroll, bankList(), banks_.size());
    if (groupList().contains(event.x, event.y)) return scroll(state_.groupScroll, groupList(), groups().size() + 1);
    if (presetList().contains(event.x, event.y)) return scroll(state_.presetScroll, presetList(), visiblePresets().size());
    return true; // the dashboard underneath never scrolls through the browser
}

bool WavetablePresetBrowser::keyPressed(const KeyEvent& key)
{
    if (! state_.open || ! state_.focused) return false;
    if (key.keyCode == juce::KeyPress::escapeKey)
    {
        if (! state_.query.empty()) setQuery({});
        else if (onClose) onClose();
        return true;
    }
    if (key.keyCode == juce::KeyPress::returnKey)
    {
        const auto presets = visiblePresets();
        if (state_.selected >= 0 && std::find(presets.begin(), presets.end(), state_.selected) != presets.end()
            && items_->at((size_t) state_.selected).path != current_)
        { load(state_.selected); return true; }
        if (onClose) onClose();
        return true;
    }
    if (! key.cmd && ! key.alt && (key.keyCode == juce::KeyPress::upKey || key.keyCode == juce::KeyPress::downKey))
    {
        moveSelection(key.keyCode == juce::KeyPress::upKey ? -1 : 1); // may load and destroy this widget
        return true;
    }
    if (key.keyCode == juce::KeyPress::backspaceKey)
    {
        auto query = state_.query;
        if (key.cmd) query.clear(); else paint::popLastCodePoint(query);
        setQuery(std::move(query));
        return true;
    }
    if (key.cmd && ! key.alt && (key.textChar == 'v' || key.textChar == 'V'))
    {
        setQuery((juce::String::fromUTF8(state_.query.c_str()) + juce::SystemClipboard::getTextFromClipboard().upToFirstOccurrenceOf("\n", false, false))
                     .substring(0, 80).toStdString());
        return true;
    }
    if (! key.cmd && ! key.alt && ! key.ctrl && key.textChar >= 32 && key.textChar != 127)
    {
        if (juce::String::fromUTF8(state_.query.c_str()).length() < 80)
            setQuery(state_.query + juce::String::charToString((juce::juce_wchar) key.textChar).toStdString());
        return true;
    }
    return false;
}

void WavetablePresetBrowser::moveSelection(int delta)
{
    const auto presets = visiblePresets();
    if (presets.empty()) return;
    const auto found = std::find(presets.begin(), presets.end(), state_.selected);
    int position = found == presets.end() ? (delta > 0 ? 0 : (int) presets.size() - 1) : (int) (found - presets.begin()) + delta;
    position = std::clamp(position, 0, (int) presets.size() - 1);
    load(presets[(size_t) position]);
}

void WavetablePresetBrowser::keepSelectionVisible(bool centre)
{
    const auto presets = visiblePresets();
    const auto found = std::find(presets.begin(), presets.end(), state_.selected);
    const auto list = presetList();
    if (found != presets.end() && list.h > 0)
    {
        const float top = (float) (found - presets.begin()) * kRow;
        if (centre && (top < state_.presetScroll || top + kRow > state_.presetScroll + list.h)) state_.presetScroll = top - (list.h - kRow) * 0.5f;
        else if (top < state_.presetScroll) state_.presetScroll = top;
        else if (top + kRow > state_.presetScroll + list.h) state_.presetScroll = top + kRow - list.h;
    }
    state_.presetScroll = std::clamp(state_.presetScroll, 0.0f, std::max(0.0f, (float) presets.size() * kRow - list.h));
    const auto index = std::find(banks_.begin(), banks_.end(), state_.bank);
    if (index != banks_.end() && bankList().h > 0)
    {
        const float top = (float) (index - banks_.begin()) * kRow;
        if (top + kRow > state_.bankScroll + bankList().h) state_.bankScroll = top + kRow - bankList().h;
    }
}

void WavetablePresetBrowser::load(int index)
{
    if (index < 0 || (size_t) index >= items_->size()) return;
    state_.selected = index;
    keepSelectionVisible();
    repaint();
    const auto path = items_->at((size_t) index).path;
    if (const auto action = onLoad) action(path); // last: the editor may rebuild and delete this widget
}
} // namespace sonora::ui
