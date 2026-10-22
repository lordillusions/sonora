#include "ui/views/NativeEditors.h"
#include "devices/native/LfoShaper.h"
#include "ui/UiContext.h"
#include "ui/Painter.h"
#include "include/core/SkPathBuilder.h"
#include <algorithm>
#include <cmath>

namespace sonora::ui
{
namespace
{
std::string noteName(int note)
{
    static constexpr const char* names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    return std::string(names[note % 12]) + std::to_string(note / 12 - 2);
}
}
void LfoCurveEditor::paint(SkCanvas& c, const Theme& t)
{
    paint::fill(c, localBounds(), t.color("SurfaceBackground"));
    for (int i = 1; i < 4; ++i)
    {
        paint::fill(c, {(float) i * width() / 4, 0, 1, height()}, t.color("SurfaceArea"));
        paint::fill(c, {0, (float) i * height() / 4, width(), 1}, t.color("SurfaceArea"));
    }
    SkPathBuilder curve;
    for (int i = 0; i <= 256; ++i)
    {
        const float x = (float) i / 256;
        const float y = 3 + (1 - processor_->levelAt(x)) * std::max(1.0f, height() - 6);
        if (i == 0) curve.moveTo(x * width(), y); else curve.lineTo(x * width(), y);
    }
    SkPaint pen; pen.setAntiAlias(true); pen.setColor(t.color("ChosenDefault")); pen.setStrokeWidth(2); pen.setStyle(SkPaint::kStroke_Style);
    c.drawPath(curve.detach(), pen);
    if (processor_->getParameter(devices::LfoShaper::Shape) >= 5.5f)
        for (int i = 0; i < devices::LfoShaper::kPoints; ++i)
        {
            const float level = processor_->getParameter(devices::LfoShaper::PointStart + i);
            paint::fill(c, {(float) i / 16 * (width() - 4), 1 + (1 - level) * (height() - 6), 4, 4}, t.color("ControlForeground"));
        }
    paint::text(c, "Draw envelope", {5, 2, width() - 10, 16}, t.color("TextDisabled"), t.metrics.smallFontSize);
}
void LfoCurveEditor::mouseDown(const MouseEvent& e) { lastPoint_ = -1; mouseDrag(e); }
void LfoCurveEditor::mouseDrag(const MouseEvent& e)
{
    using L = devices::LfoShaper;
    if ((int) std::lround(processor_->getParameter(L::Shape)) != 6)
    {
        for (int i = 0; i < L::kPoints; ++i) processor_->setParameter(L::PointStart + i, processor_->levelAt((double) i / 16));
        processor_->setParameter(L::Shape, 6);
    }
    const int index = std::clamp((int) std::lround(e.x / std::max(1.0f, width()) * 16), 0, 16);
    const float level = std::clamp(1 - (e.y - 3) / std::max(1.0f, height() - 6), 0.0f, 1.0f);
    if (lastPoint_ >= 0 && lastPoint_ != index)
        for (int i = std::min(index, lastPoint_); i <= std::max(index, lastPoint_); ++i)
            processor_->setParameter(L::PointStart + i, lastValue_ + (level - lastValue_) * (float) (i - lastPoint_) / (float) (index - lastPoint_));
    processor_->setParameter(L::PointStart + index, level);
    lastPoint_ = index; lastValue_ = level; repaint();
}

float NativeDevicePanel::tableTop() const { return 24.0f; }
Rect NativeDevicePanel::padBounds(int note) const
{
    const int i = note - padBase();
    if (i < 0 || i >= 16) return {};
    const float h = std::max(16.0f, (height() - 24) / 4);
    return {16 + (float) (i % 4) * 50, 24 + (float) (3 - i / 4) * h, 47, h - 3};
}
Rect NativeDevicePanel::cellBounds(int row, int column) const
{
    const float nameWidth = tableWidth() * 0.4f, cell = tableWidth() * 0.6f / 6;
    return {tableLeft() + nameWidth + (float) column * cell, tableTop() + 20 + (float) (row - scroll_) * 20, cell, 20};
}
int NativeDevicePanel::rowAt(float y) const { return scroll_ + (int) std::floor((y - tableTop() - 20) / 20); }
int NativeDevicePanel::selectedChain() const
{
    return ctx().selectedRackId == device_.id ? ctx().selectedRackChain : 0;
}
int NativeDevicePanel::padBase() const
{
    const auto found = ctx().drumPadBanks.find(device_.id);
    return found != ctx().drumPadBanks.end() ? found->second : 36;
}
int NativeDevicePanel::padAt(float x, float y) const
{
    for (int i = 0; i < 16; ++i) if (padBounds(padBase() + i).contains(x, y)) return padBase() + i;
    return -1;
}
void NativeDevicePanel::edit(const std::function<void(model::Device&)>& operation, const std::string& label)
{
    const int chain = chain_, child = child_;
    if (ctx().editDevice) ctx().editDevice(track_, index_, [operation, chain, child](model::Device& root) {
        if (chain >= 0)
        {
            if ((size_t) chain >= root.chains.size() || child < 0 || (size_t) child >= root.chains[(size_t) chain].devices.size()) return;
            root.chainsEdited = true;
            operation(root.chains[(size_t) chain].devices[(size_t) child]);
        }
        else operation(root);
    }, label);
}
void NativeDevicePanel::paint(SkCanvas& c, const Theme& t)
{
    paint::fill(c, localBounds(), t.color("SurfaceBackground"));
    const auto fg = t.color("ControlForeground"), dim = t.color("TextDisabled");
    paint::text(c, rack() ? "+ Chain" : "Drop samples to add zones", {tableLeft() + 5, 0, tableWidth() - 100, 20}, fg, t.metrics.smallFontSize);
    if (rack()) paint::text(c, "Map: M" + std::to_string(ctx().rackMacroToMap + 1), {width() - 94, 0, 90, 20}, fg, t.metrics.smallFontSize);
    if (drums())
    {
        for (int i = 0; i < 16; ++i)
        {
            const int note = padBase() + i;
            const Rect pad = padBounds(note);
            int ci = -1;
            for (size_t k = 0; k < device_.chains.size(); ++k) if (device_.chains[k].receivingNote == note) ci = (int) k;
            paint::fillRounded(c, pad, 2, ci >= 0 && ci == selectedChain() ? t.color("SelectionBackground") : t.color("SurfaceHighlight"));
            paint::stroke(c, pad, t.color("ControlContrastFrame"), 0.7f);
            paint::text(c, ci >= 0 ? device_.chains[(size_t) ci].name : noteName(note), pad.reduced(3, 0).withHeight(pad.h * 0.6f), ci >= 0 ? fg : dim, t.metrics.smallFontSize, Align::Centre);
            if (ci >= 0) paint::text(c, noteName(note), {pad.x + 3, pad.y + pad.h * 0.6f, pad.w - 6, pad.h * 0.4f}, dim, t.metrics.smallFontSize - 1, Align::Centre);
        }
        paint::text(c, noteName(padBase()) + " – " + noteName(padBase() + 15), {16, 0, 197, 18}, dim, t.metrics.smallFontSize, Align::Centre);
        for (int bank = 0; bank < 8; ++bank)
            paint::fill(c, {1, 24 + (float) (7 - bank) * (height() - 24) / 8, 8, std::max(2.0f, (height() - 24) / 8 - 2)},
                        padBase() / 16 == bank ? t.color("ChosenDefault") : t.color("ControlBackground"));
    }
    const float top = tableTop();
    const float left = tableLeft(), nameWidth = tableWidth() * 0.40f, cell = (tableWidth() - nameWidth) / 6;
    paint::text(c, rack() ? "Chain" : "Sample", {left + 4, top, nameWidth - 4, 20}, fg, t.metrics.smallFontSize, Align::Left, true);
    const char* zoneHeads[] = {"Root", "Key Lo", "Key Hi", "Vel Lo", "Vel Hi", "Loop"};
    const char* chainHeads[] = {"Key Lo", "Key Hi", "Vel Lo", "Vel Hi", "Sel Lo", "Sel Hi"};
    const char* drumHeads[] = {"Note", "Send", "Choke", "Gain", "Pan", "On"};
    for (int col = 0; col < 6; ++col)
        paint::text(c, rack() ? (drums() ? drumHeads[col] : chainHeads[col]) : zoneHeads[col],
                    {left + nameWidth + (float) col * cell, top, cell, 20}, dim, t.metrics.smallFontSize - (drums() ? 1.0f : 0.0f), Align::Centre);
    const int count = rack() ? (int) device_.chains.size() : (int) device_.sampleParts.size();
    for (int row = scroll_; row < count; ++row)
    {
        const float y = top + 20 + (float) (row - scroll_) * 20;
        if (y + 20 > height() - 16) break;
        if (rack() && row == selectedChain()) paint::fill(c, {left, y, tableWidth(), 20}, t.color("StandbySelectionBackground"));
        std::string name;
        std::array<std::string, 6> cells;
        if (rack())
        {
            const auto& chain = device_.chains[(size_t) row]; name = chain.name;
            if (drums()) cells = {noteName(std::clamp(chain.receivingNote, 0, 127)), noteName(chain.sendingNote), std::to_string(chain.chokeGroup),
                                  std::to_string((int) std::lround(chain.volume * 100)) + "%", std::to_string((int) std::lround(chain.pan * 100)), chain.speakerOn ? "On" : "Off"};
            else cells = {std::to_string(chain.keyMin), std::to_string(chain.keyMax), std::to_string(chain.velMin),
                          std::to_string(chain.velMax), std::to_string(chain.selMin), std::to_string(chain.selMax)};
        }
        else
        {
            const auto& zone = device_.sampleParts[(size_t) row]; name = zone.name;
            cells = {std::to_string(zone.rootKey), std::to_string(zone.keyMin), std::to_string(zone.keyMax),
                     std::to_string(zone.velMin), std::to_string(zone.velMax), zone.loopMode == 0 ? "Off" : zone.loopMode == 1 ? "Fwd" : "Ping"};
        }
        paint::text(c, name, {left + 4, y, nameWidth - 6, 20}, fg, t.metrics.smallFontSize);
        for (int col = 0; col < 6; ++col) paint::text(c, cells[(size_t) col], {left + nameWidth + (float) col * cell, y, cell, 20}, fg, t.metrics.smallFontSize - (drums() ? 1.0f : 0.0f), Align::Centre);
    }
    if (count == 0) paint::text(c, rack() ? "Drop an Instrument or Sample Here" : "Drop Sample Here", {left + 4, top + 24, tableWidth() - 8, 40}, dim, t.metrics.smallFontSize, Align::Centre);
}

void NativeDevicePanel::mouseDown(const MouseEvent& e)
{
    dragRow_ = dragColumn_ = -1;
    if (rack() && e.y < 20 && e.x >= tableLeft())
    {
        if (e.x > width() - 100) { ctx().rackMacroToMap = (ctx().rackMacroToMap + 1) % 16; repaint(); return; }
        edit([](auto& d) { model::Device::Chain chain; chain.name = "Chain " + std::to_string(d.chains.size() + 1); d.chains.push_back(std::move(chain)); d.chainsEdited = true; }, "Add Rack Chain");
        return;
    }
    if (drums() && padAt(e.x, e.y) >= 0)
    {
        const int note = padAt(e.x, e.y);
        for (size_t i = 0; i < device_.chains.size(); ++i) if (device_.chains[i].receivingNote == note)
        { ctx().selectedRackId = device_.id; ctx().selectedRackChain = (int) i; repaint(); return; }
        return;
    }
    if (e.x < tableLeft()) return;
    const int row = rowAt(e.y), count = rack() ? (int) device_.chains.size() : (int) device_.sampleParts.size();
    if (e.y < tableTop() + 20 || row < 0 || row >= count) return;
    if (e.right)
    {
        const bool isRack = rack();
        edit([row, isRack](auto& d) {
            if (isRack) { d.chains.erase(d.chains.begin() + row); d.chainsEdited = true; }
            else { d.sampleParts.erase(d.sampleParts.begin() + row); d.samplePartsEdited = true; }
        }, isRack ? "Remove Rack Chain" : "Remove Sample Zone");
        return;
    }
    if (rack()) { ctx().selectedRackId = device_.id; ctx().selectedRackChain = row; }
    const float start = tableLeft() + tableWidth() * 0.4f;
    if (e.x >= start)
    {
        dragColumn_ = std::clamp((int) ((e.x - start) / (tableWidth() * 0.6f / 6)), 0, 5);
        dragRow_ = row; dragY_ = e.y; dragDelta_ = 0;
    }
    repaint();
}
void NativeDevicePanel::mouseDrag(const MouseEvent& e)
{
    if (dragRow_ >= 0) { dragDelta_ = (int) std::lround((dragY_ - e.y) / 3); repaint(); }
}
void NativeDevicePanel::mouseUp(const MouseEvent&)
{
    if (dragRow_ < 0) return;
    const int row = dragRow_, col = dragColumn_, delta = dragDelta_;
    dragRow_ = dragColumn_ = -1;
    if (delta != 0) changeCell(row, col, delta);
}
void NativeDevicePanel::changeCell(int row, int col, int delta)
{
    const bool isRack = rack(), isDrum = drums();
    edit([row, col, delta, isRack, isDrum](auto& d) {
        auto bounded = [delta](int& v, int lo, int hi) { v = std::clamp(v + delta, lo, hi); };
        if (! isRack)
        {
            auto& z = d.sampleParts[(size_t) row];
            switch (col)
            {
                case 0: bounded(z.rootKey, 0, 127); break;
                case 1: bounded(z.keyMin, 0, z.keyMax); break;
                case 2: bounded(z.keyMax, z.keyMin, 127); break;
                case 3: bounded(z.velMin, 1, z.velMax); break;
                case 4: bounded(z.velMax, z.velMin, 127); break;
                default: bounded(z.loopMode, 0, 2); if (z.loopEnd <= z.loopStart) { z.loopStart = z.sampleStart; z.loopEnd = z.sampleEnd; } break;
            }
            d.samplePartsEdited = true;
        }
        else
        {
            auto& c = d.chains[(size_t) row];
            if (isDrum)
            {
                switch (col)
                {
                    case 0: bounded(c.receivingNote, 0, 127); break;
                    case 1: bounded(c.sendingNote, 0, 127); break;
                    case 2: bounded(c.chokeGroup, 0, 16); break;
                    case 3: c.volume = std::clamp(c.volume + delta * 0.01, 0.0, 2.0); break;
                    case 4: c.pan = std::clamp(c.pan + delta * 0.01, -1.0, 1.0); break;
                    default: c.speakerOn = delta > 0; break;
                }
            }
            else switch (col)
            {
                case 0: bounded(c.keyMin, 0, c.keyMax); break;
                case 1: bounded(c.keyMax, c.keyMin, 127); break;
                case 2: bounded(c.velMin, 1, c.velMax); break;
                case 3: bounded(c.velMax, c.velMin, 127); break;
                case 4: bounded(c.selMin, 0, c.selMax); break;
                default: bounded(c.selMax, c.selMin, 127); break;
            }
            d.chainsEdited = true;
        }
    }, "Edit Device Zone");
}
bool NativeDevicePanel::mouseWheel(const MouseEvent& e)
{
    if (std::abs(e.wheelDeltaX) > std::abs(e.wheelDeltaY))
        return false; // a sideways swipe scrolls the device chain instead of editing a cell
    if (rack() && e.y < 20 && e.x > width() - 100)
    { ctx().rackMacroToMap = std::clamp(ctx().rackMacroToMap + (e.wheelDeltaY > 0 ? 1 : -1), 0, 15); repaint(); return true; }
    if (drums() && e.x < tableLeft())
    { ctx().drumPadBanks[device_.id] = std::clamp(padBase() + (e.wheelDeltaY > 0 ? 16 : -16), 0, 112); repaint(); return true; }
    const int row = rowAt(e.y), count = rack() ? (int) device_.chains.size() : (int) device_.sampleParts.size();
    if (e.y >= tableTop() + 20 && row >= 0 && row < count && e.x >= tableLeft() + tableWidth() * 0.4f)
    { changeCell(row, std::clamp((int) ((e.x - tableLeft() - tableWidth() * 0.4f) / (tableWidth() * 0.6f / 6)), 0, 5), e.wheelDeltaY > 0 ? 1 : -1); return true; }
    const int visible = std::max(1, (int) ((height() - tableTop() - 36) / 20));
    scroll_ = std::clamp(scroll_ + (e.wheelDeltaY > 0 ? -1 : 1), 0, std::max(0, count - visible)); repaint(); return true;
}
bool NativeDevicePanel::browserDrop(const BrowserDrag& payload, const MouseEvent& e)
{
    if (payload.kind != BrowserDrag::Kind::Sample && ! rack()) return false;
    int chain = chain_, zone = -1;
    if (rack())
    {
        if (drums() && padAt(e.x, e.y) >= 0) { chain = 128 + padAt(e.x, e.y); zone = 0; }
        else { chain = rowAt(e.y); if (e.y < tableTop() + 20 || chain < 0 || (size_t) chain >= device_.chains.size()) chain = -1; }
    }
    else { zone = rowAt(e.y); if (e.y < tableTop() + 20 || zone < 0 || (size_t) zone >= device_.sampleParts.size()) zone = -1; }
    if (ctx().dropDeviceItem) ctx().dropDeviceItem(payload, track_, index_, chain, zone, child_);
    return true;
}
} // namespace sonora::ui
