#include "ui/views/PluginPanel.h"
#include "devices/DeviceProcessor.h"
#include "ui/Controls.h"
#include "ui/UiContext.h"
#include "include/core/SkPaint.h"
#include <cmath>
#include <juce_gui_basics/juce_gui_basics.h>

namespace sonora::ui
{
PluginPanel::PluginPanel(devices::DeviceProcessor* processor, int track, int deviceId, int parentIndex, int chain)
    : processor_(processor), track_(track), deviceId_(deviceId), parentIndex_(parentIndex), chain_(chain) {}
Rect PluginPanel::pad() const { return {5, 5, width() - 10, std::max(1.0f, height() - 28)}; }
std::string PluginPanel::assignmentKey() const
{
    const auto* track = ctx().trackAt(track_);
    std::string key = std::to_string(track ? track->id : track_) + ":";
    if (track && chain_ >= 0 && parentIndex_ >= 0 && (size_t) parentIndex_ < track->devices.size())
        key += std::to_string(track->devices[(size_t) parentIndex_].id) + "/" + std::to_string(chain_) + "/";
    return key + std::to_string(deviceId_);
}
void PluginPanel::paint(SkCanvas& c, const Theme& t)
{
    const auto r = pad();
    paint::fillRounded(c, r, 1.5f, t.color("RetroDisplayBackground"));
    const auto key = assignmentKey();
    const auto found = ctx().pluginPadAssignments.find(key);
    const std::array<int, 2> assignments = found == ctx().pluginPadAssignments.end() ? std::array<int, 2>{-1, -1} : found->second;
    float position[2] = {x_, y_};
    for (int axis = 0; axis < 2; ++axis)
    {
        const int index = assignments[(size_t) axis];
        std::string label = "none";
        if (processor_ && index >= 0 && (size_t) index < processor_->parameters().size())
        {
            const auto& info = processor_->parameters()[(size_t) index];
            label = info.name;
            position[axis] = (processor_->getParameter(index) - info.minValue) / std::max(0.000001f, info.maxValue - info.minValue);
        }
        ctl::valueBox(c, t, {5 + (float) axis * (width() - 8) / 2, height() - 17, (width() - 14) / 2, 14}, label, Align::Left);
    }
    SkPaint ring;
    ring.setAntiAlias(true); ring.setStyle(SkPaint::kStroke_Style); ring.setStrokeWidth(1.5f); ring.setColor(t.color("ChosenDefault"));
    c.drawCircle(r.x + 7 + std::clamp(position[0], 0.0f, 1.0f) * std::max(1.0f, r.w - 14),
                 r.bottom() - 7 - std::clamp(position[1], 0.0f, 1.0f) * std::max(1.0f, r.h - 14), 6, ring);
}
void PluginPanel::mouseDown(const MouseEvent& e)
{
    dragging_ = pad().contains(e.x, e.y);
    if (dragging_) { mouseDrag(e); return; }
    if (e.y < height() - 20 || ! processor_) return;
    const int axis = e.x < width() / 2 ? 0 : 1;
    std::vector<std::string> labels{"none"};
    for (const auto& info : processor_->parameters()) labels.push_back(info.name);
    const auto key = assignmentKey();
    const auto found = ctx().pluginPadAssignments.find(key);
    const int selected = found == ctx().pluginPadAssignments.end() ? -1 : found->second[(size_t) axis];
    auto pick = [this, key, axis](int index) {
        if (! Widget::isAlive(this) || index < 0) return;
        auto [entry, inserted] = ctx().pluginPadAssignments.try_emplace(key, std::array<int, 2>{-1, -1});
        (void) inserted;
        entry->second[(size_t) axis] = index - 1;
        repaint();
    };
    if (ctx().showChoices)
    {
        std::vector<bool> ticked(labels.size()); ticked[(size_t) (selected + 1)] = true;
        ctx().showChoices(labels, ticked, std::move(pick));
    }
    else
    {
        juce::PopupMenu menu;
        for (size_t i = 0; i < labels.size(); ++i) menu.addItem((int) i + 1, labels[i], true, (int) i == selected + 1);
        menu.showMenuAsync(juce::PopupMenu::Options(), [onPick = std::move(pick)](int result) { if (result > 0) onPick(result - 1); });
    }
}
void PluginPanel::mouseDrag(const MouseEvent& e)
{
    if (! dragging_) return;
    const auto r = pad();
    x_ = std::clamp((e.x - r.x - 7) / std::max(1.0f, r.w - 14), 0.0f, 1.0f);
    y_ = std::clamp((r.bottom() - e.y - 7) / std::max(1.0f, r.h - 14), 0.0f, 1.0f);
    const auto found = ctx().pluginPadAssignments.find(assignmentKey());
    if (processor_ && found != ctx().pluginPadAssignments.end())
        for (int axis = 0; axis < 2; ++axis)
        {
            const int index = found->second[(size_t) axis];
            if (index < 0 || (size_t) index >= processor_->parameters().size()) continue;
            const auto& info = processor_->parameters()[(size_t) index];
            float value = info.minValue + (axis == 0 ? x_ : y_) * (info.maxValue - info.minValue);
            if (! info.choices.empty()) value = std::round(value);
            processor_->setParameter(index, value);
        }
    repaint();
}
} // namespace sonora::ui
