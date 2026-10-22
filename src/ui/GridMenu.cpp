#include "ui/GridMenu.h"
#include "ui/MenuLookAndFeel.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>

namespace sonora::ui
{

bool handleGridShortcut(GridSettings& settings, int c)
{
    switch (c)
    {
        case '1': narrowGrid(settings); return true;
        case '2': widenGrid(settings); return true;
        case '3': settings.triplet = ! settings.triplet; return true;
        case '4': settings.snap = ! settings.snap; return true;
        case '5': settings.fixed = ! settings.fixed; return true;
        default: return false;
    }
}

namespace
{
const char* densityNames[] = {"Widest", "Wide", "Medium", "Narrow", "Narrowest"};
constexpr GridSettings::Density densities[] = {GridSettings::Density::Widest, GridSettings::Density::Wide,
    GridSettings::Density::Medium, GridSettings::Density::Narrow, GridSettings::Density::Narrowest};
}

GridChoiceTable::GridChoiceTable(GridSettings& settings, double beatsPerBar, bool adaptive, std::function<void()> changed, float zoom)
    : CustomComponent(false), settings_(settings), beatsPerBar_(beatsPerBar), adaptive_(adaptive), zoom_(zoom), changed_(std::move(changed))
{
    setWantsKeyboardFocus(true);
}

void GridChoiceTable::getIdealSize(int& w, int& h) { w = (int) std::ceil(280 * zoom_); h = (int) std::ceil(52 * zoom_); }

juce::Rectangle<float> GridChoiceTable::cellBounds(int index) const
{
    const int columns = adaptive_ ? 3 : 5;
    const float cell = ((float) getWidth() - 24 * zoom_) / (float) columns;
    return {16 * zoom_ + (float) (index % columns) * cell, (17 + 16 * (float) (index / columns)) * zoom_, cell, 16 * zoom_};
}

void GridChoiceTable::paint(juce::Graphics& g)
{
    auto& look = getLookAndFeel();
    g.setColour(look.findColour(juce::PopupMenu::textColourId));
    g.setFont(look.getPopupMenuFont());
    g.drawText(adaptive_ ? "Adaptive Grid:" : "Fixed Grid:", juce::Rectangle<float>{6 * zoom_, 0, (float) getWidth() - 12 * zoom_, 17 * zoom_}, juce::Justification::centredLeft);
    for (int i = 0; i < (adaptive_ ? 5 : kFixedGridCount); ++i)
    {
        const auto box = cellBounds(i);
        const bool checked = adaptive_ ? ! settings_.fixed && settings_.density == densities[i] : settings_.fixed && settings_.fixedIndex == i;
        if (hasKeyboardFocus(false) && focused_ == i)
        { g.setColour(look.findColour(juce::PopupMenu::highlightedBackgroundColourId)); g.fillRect(box); }
        g.setColour(look.findColour(juce::PopupMenu::textColourId));
        std::string label = adaptive_ ? densityNames[i] : i == 3 ? "1 Bar" : gridLabel(fixedGridBeats(i, beatsPerBar_), beatsPerBar_);
        g.drawText(juce::String(label), box, juce::Justification::centredLeft, true);
        if (checked) g.drawText(juce::String::fromUTF8("✓"), box.withX(box.getX() - 12 * zoom_).withWidth(12 * zoom_), juce::Justification::centred);
    }
}

void GridChoiceTable::choose(int index)
{
    if (index < 0 || index >= (adaptive_ ? 5 : kFixedGridCount)) return;
    if (adaptive_) { settings_.fixed = false; settings_.density = densities[index]; }
    else { settings_.fixed = true; settings_.fixedIndex = index; }
    if (changed_) changed_();
    if (getParentComponent()) triggerMenuItem();
}

void GridChoiceTable::mouseUp(const juce::MouseEvent& event)
{
    for (int i = 0; i < (adaptive_ ? 5 : kFixedGridCount); ++i) if (cellBounds(i).contains(event.position)) { choose(i); return; }
}

bool GridChoiceTable::keyPressed(const juce::KeyPress& key)
{
    const int columns = adaptive_ ? 3 : 5;
    const int code = key.getKeyCode();
    if (code == juce::KeyPress::returnKey || code == juce::KeyPress::spaceKey) { choose(focused_); return true; }
    int delta = code == juce::KeyPress::leftKey ? -1 : code == juce::KeyPress::rightKey ? 1
              : code == juce::KeyPress::upKey ? -columns : code == juce::KeyPress::downKey ? columns : 0;
    if (! delta) return false;
    focused_ = std::clamp(focused_ + delta, 0, (adaptive_ ? 5 : kFixedGridCount) - 1); repaint(); return true;
}

juce::PopupMenu createGridMenu(GridSettings& settings, double beatsPerBar, std::function<void()> changed, std::vector<MenuAction> extra, float zoom)
{
    juce::PopupMenu menu;
    for (const auto& action : extra)
    {
        if (action.label.empty()) { menu.addSeparator(); continue; }
        juce::PopupMenu::Item item(action.label);
        item.action = action.run; item.isEnabled = action.enabled; item.shortcutKeyDescription = action.shortcut;
        menu.addItem(std::move(item));
    }
    if (! extra.empty()) menu.addSeparator();
    const auto item = [&](const char* label, const char* shortcut, bool checked, bool enabled, std::function<void(GridSettings&)> run) {
        juce::PopupMenu::Item entry(label); entry.shortcutKeyDescription = juce::String::fromUTF8(shortcut);
        entry.isTicked = checked; entry.isEnabled = enabled;
        entry.action = [&settings, changed, run] { run(settings); if (changed) changed(); };
        menu.addItem(std::move(entry));
    };
    item("Snap to Grid", "⌘4", settings.snap, true, [](auto& g) { g.snap = ! g.snap; });
    menu.addCustomItem(10001, std::make_unique<GridChoiceTable>(settings, beatsPerBar, true, changed, zoom), nullptr, "Adaptive Grid");
    menu.addCustomItem(10002, std::make_unique<GridChoiceTable>(settings, beatsPerBar, false, changed, zoom), nullptr, "Fixed Grid");
    menu.addSeparator();
    item("Narrow Grid", "⌘1", false, settings.fixed ? settings.fixedIndex < kFixedGridCount - 1 : settings.density != GridSettings::Density::Narrowest,
         [](auto& g) { narrowGrid(g); });
    item("Widen Grid", "⌘2", false, settings.fixed ? settings.fixedIndex > 0 : settings.density != GridSettings::Density::Widest,
         [](auto& g) { widenGrid(g); });
    menu.addSeparator();
    item("Triplet Grid", "⌘3", settings.triplet, true, [](auto& g) { g.triplet = ! g.triplet; });
    return menu;
}

void showGridMenu(GridSettings& settings, double beatsPerBar, std::function<void()> changed, std::vector<MenuAction> extra, float zoom)
{
    createGridMenu(settings, beatsPerBar, std::move(changed), std::move(extra), zoom).showMenuAsync(juce::PopupMenu::Options());
}

juce::Image renderGridMenuPreview(GridSettings& settings, double beatsPerBar, const Theme& theme, std::vector<MenuAction> extra, float scale)
{
    MenuLookAndFeel look; look.configure(theme);
    auto menu = createGridMenu(settings, beatsPerBar, {}, std::move(extra));
    int width = 0, height = 4;
    juce::PopupMenu::MenuItemIterator sizes(menu);
    while (sizes.next())
    {
        int w = 0, h = 0;
        if (sizes.getItem().customComponent) sizes.getItem().customComponent->getIdealSize(w, h);
        else look.getIdealPopupMenuItemSize(sizes.getItem().text, sizes.getItem().isSeparator, 0, w, h);
        width = std::max(width, w); height += h;
    }
    juce::Image image(juce::Image::ARGB, (int) std::ceil((float) width * scale), (int) std::ceil((float) height * scale), true);
    juce::Graphics graphics(image); graphics.addTransform(juce::AffineTransform::scale(scale));
    look.drawPopupMenuBackground(graphics, width, height);
    juce::PopupMenu::MenuItemIterator rows(menu);
    int y = 2;
    while (rows.next())
    {
        const auto& entry = rows.getItem(); int w = 0, h = 0;
        if (entry.customComponent)
        {
            entry.customComponent->getIdealSize(w, h); entry.customComponent->setBounds(0, y, width, h);
            entry.customComponent->setLookAndFeel(&look);
            { const juce::Graphics::ScopedSaveState saved(graphics); graphics.setOrigin(0, y); entry.customComponent->paint(graphics); }
            entry.customComponent->setLookAndFeel(nullptr);
        }
        else
        {
            look.getIdealPopupMenuItemSize(entry.text, entry.isSeparator, 0, w, h);
            look.drawPopupMenuItem(graphics, {0, y, width, h}, entry.isSeparator, entry.isEnabled, false, entry.isTicked, false,
                                  entry.text, entry.shortcutKeyDescription, nullptr, nullptr);
        }
        y += h;
    }
    return image;
}

} // namespace sonora::ui
