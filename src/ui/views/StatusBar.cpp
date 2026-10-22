#include "ui/views/StatusBar.h"
#include "ui/views/DetailView.h"
#include "ui/Controls.h"
#include "ui/UiContext.h"
#include "gfx/Brand.h"
#include "include/core/SkPaint.h"
#include <juce_gui_basics/juce_gui_basics.h>

namespace sonora::ui
{
Rect StatusBar::navigationBounds() const
{
    const auto& m = ctx().theme.metrics;
    const float w = std::min(300.0f, width() * 0.45f);
    return {width() - m.outerMargin - 43 - w, m.panelGap, w, height() - m.panelGap - 6};
}
Rect StatusBar::overviewBounds() const
{
    auto r = navigationBounds();
    const float w = std::min(155.0f, r.w * 0.53f);
    return {r.right() - w - 2, r.y + 1, w, r.h - 2};
}
Rect StatusBar::mixerBounds() const { return {width() - ctx().theme.metrics.outerMargin - 36, 4, 36, height() - 8}; }
void StatusBar::paint(SkCanvas& c, const Theme& t)
{
    auto& x = ctx();
    const auto& m = t.metrics;
    Rect r{m.outerMargin, m.panelGap, width() - 2 * m.outerMargin, height() - m.panelGap - 6};
    const auto* busy = x.currentBusy();
    if (busy) // UI-51: the activity bars replace the info badge while the user waits
        gfx::brand::drawActivity(c, SkRect::MakeXYWH(r.x + 1, r.centreY() - 6, 12, 12), juce::Time::getMillisecondCounterHiRes() / 1000.0,
                                 t.color("ControlForeground"));
    else
    {
        SkPaint ink; ink.setAntiAlias(true); ink.setColor(t.color("SurfaceBackground"));
        c.drawCircle(r.x + 7, r.centreY(), 7, ink);
        paint::text(c, "i", {r.x, r.y, 14, r.h}, t.color("Desktop"), 10, Align::Centre, true);
    }
    r.removeFromLeft(22);
    const auto nav = navigationBounds(), overview = overviewBounds(), mixer = mixerBounds();
    paint::fillRounded(c, nav, 3, t.color("SurfaceBackground"));
    paint::fillRounded(c, {nav.x, nav.y, 23, nav.h}, 3, x.detailVisible ? t.color("ChosenDefault") : t.color("SurfaceBackground"));
    ctl::triangle(c, nav.x + 11.5f, nav.centreY(), 8, 7, x.detailVisible ? ctl::Dir::Up : ctl::Dir::Down, t.color("ControlForeground"));
    if (x.trackAt(x.selectedTrack))
        paint::text(c, x.trackDisplayName(x.selectedTrack), {nav.x + 30, nav.y, overview.x - nav.x - 34, nav.h}, t.color("ControlForeground"), m.fontSize, Align::Left, true);
    paint::fillRounded(c, overview, 3, t.color("RetroDisplayBackground"));
    if (detailView) detailView->paintOverview(c, t, overview.reduced(2, 1));
    paint::stroke(c, overview, t.color("ControlContrastFrame"), 1.5f);
    for (int i = 0; i < 3; ++i)
    {
        const float h = i == 1 ? 17.0f : i == 0 ? 12.0f : 9.0f;
        paint::fill(c, {mixer.x + 2 + (float) i * 5, mixer.centreY() + 8 - h, 2.5f, h}, t.color(x.mixerVisible ? "ChosenDefault" : "TextDisabled"));
    }
    ctl::triangle(c, mixer.right() - 5, mixer.centreY(), 7, 6, ctl::Dir::Down, t.color("TextDisabled"));
    r.w = std::max(0.0f, nav.x - 8 - r.x);
    paint::fillRounded(c, r, 3, t.color("SurfaceBackground"));
    std::string text = x.statusText;
    if (busy)
    {
        text = busy->text;
        if (busy->progress >= 0) // a hairline along the bottom of the message field
        {
            const Rect bar{r.x + 3, r.bottom() - 2.5f, (r.w - 6) * std::clamp(busy->progress, 0.0f, 1.0f), 1.5f};
            paint::fill(c, bar, t.color("ControlForeground"));
        }
    }
    else if (logIndex_ >= 0 && (size_t) logIndex_ < x.buildLog.size()) text = "[" + std::to_string(logIndex_ + 1) + "/" + std::to_string(x.buildLog.size()) + "] " + x.buildLog[(size_t) logIndex_];
    paint::text(c, text, r.reduced(8, 0), t.color("ControlForeground"), m.fontSize);
}
void StatusBar::tick()
{
    if (ctx().currentBusy()) repaint(); // animates the activity bars
}
void StatusBar::navigate(float x)
{
    const auto r = overviewBounds();
    if (detailView) detailView->navigateOverview(std::clamp((x - r.x) / std::max(1.0f, r.w), 0.0f, 1.0f));
    repaint();
}
void StatusBar::mouseDown(const MouseEvent& e)
{
    if (overviewBounds().contains(e.x, e.y)) { dragging_ = true; navigate(e.x); return; }
    auto& x = ctx();
    if (mixerBounds().contains(e.x, e.y))
    {
        if (e.x < mixerBounds().right() - 12)
        {
            x.mixerVisible = ! x.mixerVisible;
            x.statusText = x.mixerVisible ? "Show Mixer" : "Hide Mixer";
            if (parent_) parent_->layout();
            if (x.layoutChanged) x.layoutChanged();
        }
        else
        {
            auto pick = [this](int index) {
                if (! Widget::isAlive(this) || index < 0) return;
                auto& context = ctx();
                if (index == 0) context.mixerVisible = ! context.mixerVisible;
                if (index == 1) context.sendsVisible = ! context.sendsVisible;
                if (index == 2) context.ioVisible = ! context.ioVisible;
                if (parent_) parent_->layout();
                if (context.layoutChanged) context.layoutChanged();
                repaint();
            };
            if (x.showChoices) x.showChoices({"Mixer", "Sends", "In/Out"}, {x.mixerVisible, x.sendsVisible, x.ioVisible}, std::move(pick));
            else
            {
                juce::PopupMenu menu;
                menu.addItem(1, "Mixer", true, x.mixerVisible); menu.addItem(2, "Sends", true, x.sendsVisible); menu.addItem(3, "In/Out", true, x.ioVisible);
                menu.showMenuAsync(juce::PopupMenu::Options(), [onPick = std::move(pick)](int result) { if (result > 0) onPick(result - 1); });
            }
        }
    }
    else if (navigationBounds().contains(e.x, e.y))
    {
        if (e.x < navigationBounds().x + 24 && ! e.right) x.detailVisible = ! x.detailVisible;
        else
        {
            auto pick = [this](int index) {
                if (! Widget::isAlive(this) || index < 0) return;
                ctx().detailTab = static_cast<DetailTab>(index); ctx().detailVisible = true;
                if (parent_) parent_->layout(); repaint();
            };
            if (x.showChoices) x.showChoices({"Devices", "Clip", "Monitor"}, {x.detailTab == DetailTab::Devices, x.detailTab == DetailTab::Clip, x.detailTab == DetailTab::Monitor}, std::move(pick));
            else
            {
                juce::PopupMenu menu;
                menu.addItem(1, "Devices"); menu.addItem(2, "Clip"); menu.addItem(3, "Monitor");
                menu.showMenuAsync(juce::PopupMenu::Options(), [onPick = std::move(pick)](int result) { if (result > 0) onPick(result - 1); });
            }
        }
        if (parent_) parent_->layout();
    }
    else
    {
        const int n = (int) x.buildLog.size();
        logIndex_ = n == 0 ? -1 : (logIndex_ + 1 >= n ? -1 : logIndex_ + 1);
    }
    repaint();
}
void StatusBar::mouseDrag(const MouseEvent& e) { if (dragging_) navigate(e.x); }
void StatusBar::mouseUp(const MouseEvent&) { dragging_ = false; }
bool StatusBar::mouseWheel(const MouseEvent& e)
{
    if (! detailView || ! overviewBounds().contains(e.x, e.y)) return false;
    return detailView->mouseWheel(e);
}
} // namespace sonora::ui
