#pragma once
// Flat, compact context menus. Native macOS menu-bar menus retain the operating system's rendering.
#include "ui/Theme.h"
#include "ui/UiContext.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <cmath>

namespace sonora::ui
{
class MenuLookAndFeel : public juce::LookAndFeel_V4
{
public:
    void configure(const Theme& theme, float zoom = 1)
    {
        scale_ = zoom; fontSize_ = theme.metrics.fontSize * zoom;
        setColour(juce::PopupMenu::backgroundColourId, juce::Colour(theme.color("ControlTextBack")));
        setColour(juce::PopupMenu::textColourId, juce::Colour(theme.color("ControlForeground")));
        setColour(juce::PopupMenu::highlightedBackgroundColourId, juce::Colour(theme.color("SelectionBackground")));
        setColour(juce::PopupMenu::highlightedTextColourId, juce::Colour(theme.color("SelectionForeground")));
        disabled_ = juce::Colour(theme.color("TextDisabled"));
        frame_ = juce::Colour(theme.color("ControlContrastFrame"));
    }
    juce::Font getPopupMenuFont() override { return juce::Font(juce::FontOptions(fontSize_)); }
    void drawPopupMenuBackground(juce::Graphics& g, int w, int h) override
    {
        g.fillAll(findColour(juce::PopupMenu::backgroundColourId));
        g.setColour(frame_); g.drawRect(0, 0, w, h, std::max(1, (int) std::lround(scale_)));
    }
    void getIdealPopupMenuItemSize(const juce::String& text, bool separator, int, int& w, int& h) override
    {
        h = (int) std::ceil((separator ? 9.0f : 15.0f) * scale_);
        w = (int) std::ceil(std::max(250.0f * scale_, (float) text.length() * fontSize_ * 0.6f + 80.0f * scale_));
    }
    void drawPopupMenuItem(juce::Graphics& g, const juce::Rectangle<int>& area, bool separator, bool active,
                           bool highlighted, bool ticked, bool subMenu, const juce::String& text,
                           const juce::String& shortcut, const juce::Drawable* icon, const juce::Colour*) override
    {
        if (separator)
        {
            g.setColour(frame_); g.fillRect((float) area.getX(), (float) area.getCentreY(), (float) area.getWidth(), scale_);
            return;
        }
        if (highlighted && active)
        { g.setColour(findColour(juce::PopupMenu::highlightedBackgroundColourId)); g.fillRect(area.reduced(1, 0)); }
        const auto ink = ! active ? disabled_ : findColour(highlighted ? juce::PopupMenu::highlightedTextColourId : juce::PopupMenu::textColourId);
        g.setColour(ink); g.setFont(getPopupMenuFont());
        auto row = area.toFloat().reduced(16 * scale_, 0);
        if (ticked) g.drawText(juce::String::fromUTF8("✓"), area.toFloat().withWidth(14 * scale_), juce::Justification::centred);
        if (icon) icon->drawWithin(g, area.toFloat().withWidth(14 * scale_).reduced(2 * scale_), juce::RectanglePlacement::centred, active ? 1.0f : 0.4f);
        g.drawText(text, row, juce::Justification::centredLeft, true);
        g.drawText(shortcut, row.withRight((float) area.getRight() - 8 * scale_), juce::Justification::centredRight, false);
        if (subMenu)
        {
            juce::Path arrow;
            const float x = (float) area.getRight() - 7 * scale_, y = (float) area.getCentreY();
            arrow.addTriangle(x - 2 * scale_, y - 3 * scale_, x + 2 * scale_, y, x - 2 * scale_, y + 3 * scale_);
            g.fillPath(arrow);
        }
    }
private:
    float scale_ = 1, fontSize_ = 10;
    juce::Colour disabled_{0xff575b67}, frame_{0xff313541};
};

// UI-46: options for a menu that drops down from a chooser (`rootRect` in root-widget coordinates), at least as wide as it,
// like Live's device and parameter choosers. Falls back to the mouse position when the canvas has no screen mapping.
inline juce::PopupMenu::Options dropDownOptions(const UiContext& ctx, const Rect& rootRect)
{
    auto options = juce::PopupMenu::Options();
    if (! ctx.rootToScreen) return options;
    const auto r = ctx.rootToScreen(rootRect);
    return options.withTargetScreenArea(juce::Rectangle<float>(r.x, r.y, r.w, r.h).toNearestInt())
        .withMinimumWidth((int) std::lround(r.w))
        .withPreferredPopupDirection(juce::PopupMenu::Options::PopupDirection::downwards);
}
} // namespace sonora::ui
