// DEV-14b / UI-43k: the Sampler's zone editor, matched to window captures of the reference with ZoneEditorVisible (Key,
// Vel and Sel tabs; 3456 x 2060 retina, 2 px per point). The panel is 490 px (245 pt) tall above the device view. Header
// row 50 px: hot-swap, auto-select, Lin; RR / Forward / Reset centred over the layer list; Key / Vel / Sel (56 x 26, 10 px
// apart) ending 14 px before the ruler. Layer rows 36 px + a 2-px rule: name 20 px in, speaker and S boxes (32 x 32) at the
// list's right end. The ruler / zone area is 1026 px wide (128 cells of 8 px): per row a crossfade bar (4 px fill in 2-px
// black frames) and the zone bar (16 px fill); key zones green #5cba62 / #bcf4ba, velocity red #d85e50 / #eaaaa5, selector
// blue #3c65cb / #c3d5f1 (window pixels, Display P3 as captured).

#include "ui/views/SamplerFace.h"

#include "devices/DeviceProcessor.h"
#include "engine/EngineGraph.h"

#include "gfx/Fonts.h"
#include "ui/Controls.h"
#include "ui/UiContext.h"

#include "include/core/SkPaint.h"
#include "include/core/SkPathBuilder.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <cmath>

namespace sonora::ui
{
namespace
{
constexpr float kHeader = 25, kRow = 18, kZoneWidth = 513, kCell = kZoneWidth / 128.0f, kButton = 16;
constexpr float kTabW = 28, kTabGap = 5;

struct Colours { SkColor zone, fade; };
Colours colours(sampler::Axis axis)
{
    switch (axis)
    {
        case sampler::Axis::Velocity: return {SkColorSetRGB(0xd8, 0x5e, 0x50), SkColorSetRGB(0xea, 0xaa, 0xa5)};
        case sampler::Axis::Selector: return {SkColorSetRGB(0x3c, 0x65, 0xcb), SkColorSetRGB(0xc3, 0xd5, 0xf1)};
        case sampler::Axis::Key: break;
    }
    return {SkColorSetRGB(0x5c, 0xba, 0x62), SkColorSetRGB(0xbc, 0xf4, 0xba)};
}
const SkColor kLane = SkColorSetRGB(0xc0, 0xc3, 0xca), kRule = SkColorSetRGB(0x58, 0x5b, 0x66);

bool blackKey(int key) { const int k = key % 12; return k == 1 || k == 3 || k == 6 || k == 8 || k == 10; }

void choose(Widget* owner, const std::vector<std::string>& labels, int current, std::function<void(int)> pick)
{
    std::vector<bool> ticks(labels.size());
    if (current >= 0 && (size_t) current < ticks.size()) ticks[(size_t) current] = true;
    auto safe = [owner, chosen = std::move(pick)](int index) { if (Widget::isAlive(owner) && index >= 0) chosen(index); };
    if (owner->ctx().showChoices) { owner->ctx().showChoices(labels, ticks, std::move(safe)); return; }
    juce::PopupMenu menu;
    for (size_t i = 0; i < labels.size(); ++i) menu.addItem((int) i + 1, labels[i], true, ticks[i]);
    menu.showMenuAsync(juce::PopupMenu::Options(), [select = std::move(safe)](int c) { if (c > 0) select(c - 1); });
}

// The engine's names (devices::Sampler RoundRobinMode / RoundRobinResetPeriod). TODO(DEV-14b): not checked against a
// capture of the reference's open choosers.
const std::vector<std::string> kRoundRobinModes{"Forward", "Backward", "Other", "Random"};
const std::vector<std::string> kResetPeriods{"None", "1/16", "1/8", "1/4", "1/2", "1 Bar", "2 Bars", "4 Bars"};

// Header geometry (points from the panel's left edge).
Rect hotSwap() { return {2, 5, 15, 15}; }
Rect autoSelect() { return {20, 6, 27, 13}; }
Rect linear() { return {52, 6, 28, 13}; }
} // namespace

sampler::ZoneTarget SamplerZoneEditor::target() const { return sampler::zoneEditorTarget(ctx()); }

devices::DeviceProcessor* SamplerZoneEditor::processor(int& index, const std::string& path) const
{
    index = -1;
    const auto t = target();
    const auto* graph = ctx().graph;
    if (! t.device || t.where.chain >= 0 || graph == nullptr || t.where.track < 0 || (size_t) t.where.track >= graph->nodes.size()) return nullptr;
    const auto& processors = graph->nodes[(size_t) t.where.track].processorForModelDevice;
    if (t.where.device < 0 || (size_t) t.where.device >= processors.size() || processors[(size_t) t.where.device] == nullptr) return nullptr;
    auto* p = processors[(size_t) t.where.device];
    const auto& infos = p->parameters();
    for (size_t i = 0; i < infos.size(); ++i)
        if (infos[i].id == path) { index = (int) i; return p; }
    return nullptr;
}

double SamplerZoneEditor::value(const model::Device& device, const std::string& path, double fallback) const
{
    int index = -1;
    if (auto* p = processor(index, path)) return p->getParameter(index);
    return sampler::deviceValue(device, path, fallback);
}

void SamplerZoneEditor::setValue(const std::string& path, double v, const std::string& label)
{
    int index = -1;
    if (auto* p = processor(index, path)) { p->setParameter(index, (float) v); repaint(); return; } // live, saved by the engine sync
    edit([path, v](model::Device& d) {
        for (auto& q : d.parameters)
            if (q.path == path) { q.value = v; return; }
        d.plain[path] = v; // the Sampler's plain leaves (Player/MultiSampleMap/...); SampleSelector is a <Manual>
        if (path == "Player/SampleSelector")
        {
            d.plain.erase(path);
            model::Parameter q;
            q.path = path;
            q.value = v;
            d.parameters.push_back(std::move(q));
        }
    }, label);
}

int SamplerZoneEditor::tab() const
{
    const auto t = target();
    if (! t.device) return 0;
    const auto it = ctx().samplerViews.find(t.device->id);
    return it == ctx().samplerViews.end() ? 0 : std::clamp(it->second.zoneTab, 0, 2);
}
void SamplerZoneEditor::setTab(int tab)
{
    if (const auto t = target(); t.device) ctx().samplerViews[t.device->id].zoneTab = tab;
    repaint();
}
sampler::Axis SamplerZoneEditor::axis() const { return (sampler::Axis) tab(); }
float SamplerZoneEditor::zoneLeft() const { return width() - 2 - kZoneWidth; }
float SamplerZoneEditor::valueX(float value) const { return zoneLeft() + value * kCell; }
Rect SamplerZoneEditor::tabBounds(int tab) const
{
    return {zoneLeft() - 7 - 3 * kTabW - 2 * kTabGap + (float) tab * (kTabW + kTabGap), 6, kTabW, 13};
}
Rect SamplerZoneEditor::rowBounds(int part) const { return {0, kHeader + 2 + (float) part * (kRow + 1), width(), kRow}; }
Rect SamplerZoneEditor::zoneBar(int part) const
{
    const auto t = target();
    if (! t.device || part < 0 || (size_t) part >= t.device->sampleParts.size()) return {};
    const auto r = shown(part, t.device->sampleParts[(size_t) part]);
    const auto row = rowBounds(part);
    return {valueX((float) r.min), row.y + 7, (float) (r.max + 1 - r.min) * kCell, 10};
}
Rect SamplerZoneEditor::fadeBar(int part) const
{
    const auto t = target();
    if (! t.device || part < 0 || (size_t) part >= t.device->sampleParts.size()) return {};
    const auto r = shown(part, t.device->sampleParts[(size_t) part]);
    const auto row = rowBounds(part);
    return {valueX((float) r.min), row.y + 3, (float) (r.max + 1 - r.min) * kCell, 2};
}
sampler::ZoneRange SamplerZoneEditor::shown(int part, const model::SamplePart& p) const
{
    if (drag_.part == part && drag_.what != Drag::What::None && drag_.what != Drag::What::Selector) return drag_.current;
    return sampler::zoneRange(p, axis());
}

void SamplerZoneEditor::layout() {}

void SamplerZoneEditor::paint(SkCanvas& c, const Theme& t)
{
    const auto target = this->target();
    paint::fill(c, localBounds(), t.color("SurfaceBackground"));
    if (! target.device) return;
    const auto& device = *target.device;
    const float size = t.metrics.fontSize + 0.5f, zx = zoneLeft();
    const auto ink = t.color("ControlForeground");
    const auto axis = this->axis();

    // Header.
    titleIcon(c, TitleIcon::HotSwap, hotSwap().centreX(), hotSwap().centreY(), 14, ink);
    const bool autoOn = sampler::deviceValue(device, "ViewSettings/IsAutoSelectEnabled", 0) > 0.5;
    ctl::framed(c, t, autoSelect(), autoOn ? t.color("ChosenDefault") : t.color("ControlFillHandle"));
    {
        const auto r = autoSelect();
        paint::fill(c, {r.centreX() - 5, r.centreY() - 2.5f, 1.2f, 5}, ink);
        paint::fill(c, {r.centreX() - 3, r.centreY() - 2.5f, 1.2f, 5}, ink);
        ctl::triangle(c, r.centreX() + 2, r.centreY(), 4, 5, ctl::Dir::Right, ink);
    }
    const bool power = value(device, "Player/MultiSampleMap/LayerCrossfade", 0) > 0.5;
    ctl::framed(c, t, linear(), t.color("ChosenAlternative"));
    paint::text(c, power ? "Pow" : "Lin", linear(), ink, size, Align::Centre);
    const bool rr = value(device, "Player/MultiSampleMap/RoundRobin", 0) > 0.5;
    const float group = 156, gx = std::max(90.0f, (zx - 30) * 0.5f - group * 0.5f);
    ctl::toggle(c, t, {gx, 6, 17, 13}, "RR", rr, t.color("ChosenDefault"), true, false, size - 1);
    const int mode = (int) value(device, "Player/MultiSampleMap/RoundRobinMode", 0);
    const auto field = [&](Rect r, const std::string& text) {
        ctl::framed(c, t, r, t.color("ControlTextBack"));
        c.save();
        c.clipRect(SkRect::MakeXYWH(r.x, r.y, r.w - 9, r.h));
        paint::text(c, text, {r.x + 2, r.y, gfx::measureText(text, size - 1.5f) + 6, r.h}, ink, size - 1.5f);
        c.restore();
        ctl::triangle(c, r.right() - 5, r.centreY(), 6, 4, ctl::Dir::Down, ink);
    };
    field({gx + 20, 6, 47, 13}, kRoundRobinModes[(size_t) std::clamp(mode, 0, (int) kRoundRobinModes.size() - 1)]);
    paint::text(c, "Reset", {gx + 72, 6, 30, 13}, ink, size - 1);
    const int reset = (int) value(device, "Player/MultiSampleMap/RoundRobinResetPeriod", 0);
    field({gx + 105, 6, 47, 13}, kResetPeriods[(size_t) std::clamp(reset, 0, (int) kResetPeriods.size() - 1)]);
    const char* names[] = {"Key", "Vel", "Sel"};
    for (int i = 0; i < 3; ++i) ctl::toggle(c, t, tabBounds(i), names[i], tab() == i, t.color("ChosenDefault"), true, false, size);
    paint::fill(c, {0, kHeader, zx - 2, 2}, kRule);

    // Ruler.
    paint::fill(c, {zx - 2, 0, 2, height()}, kRule);
    if (axis == sampler::Axis::Key)
    {
        paint::fill(c, {zx, 0, kZoneWidth, 14}, t.color("SurfaceBackground"));
        for (int key = 0; key < 128; key += 12)
        {
            const float x = valueX((float) key);
            paint::fill(c, {x, 0, 1, 14}, ink);
            paint::text(c, std::string("C") + std::to_string(key / 12 - 2), {x + 2, 0, 40, 14}, ink, size);
        }
        paint::fill(c, {zx, 14, kZoneWidth, kHeader + 2 - 14}, kRule);
        for (int key = 0; key < 128; ++key)
            paint::fill(c, {valueX((float) key) + 0.5f, 15, kCell - 1, kHeader - 15}, blackKey(key) ? ink : SkColorSetRGB(0xf3, 0xf4, 0xf5));
    }
    else
    {
        paint::fill(c, {zx, 0, kZoneWidth, kHeader + 2}, t.color("SurfaceBackground"));
        const int first = axis == sampler::Axis::Velocity ? 1 : 0;
        for (int v = first; v <= 127; v += v == first && first == 1 ? 7 : 8)
        {
            const float x = valueX((float) v);
            paint::fill(c, {x, kHeader - 6, 1, 8}, ink);
            if (v <= 120) paint::text(c, std::to_string(v), {x + 1, 2, 30, 14}, ink, size - 1.5f);
        }
        paint::text(c, "127", {zx + kZoneWidth - 32, 2, 30, 14}, ink, size - 1.5f, Align::Right);
        if (axis == sampler::Axis::Selector)
        {
            const double selector = pendingSelector_ >= 0 ? pendingSelector_ : value(device, "Player/SampleSelector", 0);
            paint::fill(c, {valueX((float) selector), 0, 2, kHeader + 2}, t.color("RangeDefault"));
        }
    }
    // Lanes: the darker cells (black keys, or every 8th value).
    paint::fill(c, {zx, kHeader + 2, kZoneWidth, height() - kHeader - 2}, kLane);
    for (int v = 0; v < 128; ++v)
    {
        const bool dark = axis == sampler::Axis::Key ? blackKey(v) : (v % 8 == 0 && v > 0) || (axis == sampler::Axis::Velocity && v == 1);
        if (dark) paint::fill(c, {valueX((float) v), kHeader + 2, kCell, height() - kHeader - 2}, t.color("SurfaceBackground"));
    }

    // Layers.
    const int selected = sampler::selectedPart(ctx(), device);
    const auto col = colours(axis);
    for (size_t i = 0; i < device.sampleParts.size(); ++i)
    {
        const auto& part = device.sampleParts[i];
        const auto row = rowBounds((int) i);
        if (row.y > height()) break;
        paint::fill(c, {0, row.y, zx - 2, row.h}, (int) i == selected ? paint::mix(t.color("SurfaceHighlight"), t.color("ChosenAlternative"), 0.12f) : t.color("SurfaceBackground"));
        paint::fill(c, {0, row.bottom(), width(), 1}, kRule);
        paint::text(c, part.name, {10, row.y, zx - 70, row.h}, ink, size);
        const bool active = sampler::partValue(part, "IsActive", 1) > 0.5, solo = sampler::partValue(part, "Solo", 0) > 0.5;
        const Rect speaker{zx - 42, row.y + 1.5f, kButton, kButton}, soloBox{zx - 21, row.y + 1.5f, kButton, kButton};
        ctl::framed(c, t, speaker, active ? t.color("ChosenDefault") : t.color("ControlFillHandle"));
        {
            SkPathBuilder cone;
            const float x = speaker.x + 5, y = speaker.centreY();
            cone.moveTo(x, y - 1.5f).lineTo(x + 2, y - 1.5f).lineTo(x + 5, y - 4).lineTo(x + 5, y + 4).lineTo(x + 2, y + 1.5f).lineTo(x, y + 1.5f).close();
            SkPaint pen;
            pen.setAntiAlias(true);
            pen.setStyle(SkPaint::kStroke_Style);
            pen.setStrokeWidth(1.1f);
            pen.setColor(ink);
            c.drawPath(cone.detach(), pen);
        }
        ctl::toggle(c, t, soloBox, "S", solo, t.color("ChosenAlternative"), true, false, size);
        // Zone: the crossfade bar (ramps where the range fades) above the zone bar.
        const auto r = shown((int) i, part);
        const float x0 = valueX((float) r.min), x1 = valueX((float) r.max + 1), f0 = valueX((float) r.fadeMin), f1 = valueX((float) r.fadeMax + 1);
        const Rect fade{x0, row.y + 3, x1 - x0, 2};
        paint::fill(c, fade.reduced(-1), SkColorSetRGB(0, 0, 0));
        paint::fill(c, fade, kLane);
        paint::fill(c, {f0, fade.y, f1 - f0, fade.h}, col.fade);
        if (f0 > x0)
        {
            SkPathBuilder ramp;
            ramp.moveTo(x0, fade.bottom()).lineTo(f0, fade.y).lineTo(f0, fade.bottom()).close();
            SkPaint fill;
            fill.setAntiAlias(true);
            fill.setColor(col.fade);
            c.drawPath(ramp.detach(), fill);
        }
        if (f1 < x1)
        {
            SkPathBuilder ramp;
            ramp.moveTo(f1, fade.y).lineTo(x1, fade.bottom()).lineTo(f1, fade.bottom()).close();
            SkPaint fill;
            fill.setAntiAlias(true);
            fill.setColor(col.fade);
            c.drawPath(ramp.detach(), fill);
        }
        const Rect bar{x0, row.y + 8, x1 - x0, 8};
        paint::fill(c, bar.reduced(-1), SkColorSetRGB(0, 0, 0));
        paint::fill(c, bar, active ? col.zone : paint::mix(col.zone, kLane, 0.6f));
        if (axis == sampler::Axis::Key && part.rootKey >= r.min && part.rootKey <= r.max)
            paint::text(c, "R", {valueX((float) part.rootKey) - 4, bar.y - 1, kCell + 8, bar.h + 2}, ink, 7, Align::Centre, true);
    }
}

void SamplerZoneEditor::edit(const std::function<void(model::Device&)>& operation, const std::string& label)
{
    const auto t = target();
    if (! t.device || ! ctx().editDevice) return;
    const int chain = t.where.chain, child = t.where.child;
    ctx().editDevice(t.where.track, t.where.device, [operation, chain, child](model::Device& root) {
        if (chain < 0) operation(root);
        else if ((size_t) chain < root.chains.size() && child >= 0 && (size_t) child < root.chains[(size_t) chain].devices.size())
        {
            root.chainsEdited = true;
            operation(root.chains[(size_t) chain].devices[(size_t) child]);
        }
    }, label);
}

void SamplerZoneEditor::editPart(int part, const std::function<void(model::SamplePart&)>& operation, const std::string& label)
{
    edit([part, operation](model::Device& d) {
        if (part < 0 || (size_t) part >= d.sampleParts.size()) return;
        operation(d.sampleParts[(size_t) part]);
        d.samplePartsEdited = true;
    }, label);
}

void SamplerZoneEditor::mouseDown(const MouseEvent& e)
{
    drag_ = {};
    const auto t = target();
    if (! t.device || e.right) return;
    const auto& device = *t.device;
    const auto plainToggle = [&](const std::string& key, const std::string& label) { setValue(key, value(device, key, 0) > 0.5 ? 0 : 1, label); };
    if (e.y < kHeader)
    {
        for (int i = 0; i < 3; ++i)
            if (tabBounds(i).contains(e.x, e.y)) { setTab(i); return; }
        if (hotSwap().contains(e.x, e.y)) { ctx().statusText = "Hot-swap of Sampler layers is not available yet (UI-42b)"; return; }
        if (autoSelect().contains(e.x, e.y))
        {
            if (ctx().setDeviceViewValue)
                ctx().setDeviceViewValue(t.where.track, t.where.device, t.where.chain, t.where.child, "ViewSettings/IsAutoSelectEnabled",
                                         sampler::deviceValue(device, "ViewSettings/IsAutoSelectEnabled", 0) > 0.5 ? 0 : 1);
            repaint();
            return;
        }
        if (linear().contains(e.x, e.y)) { plainToggle("Player/MultiSampleMap/LayerCrossfade", "Change Zone Crossfade"); return; }
        const float gx = std::max(90.0f, (zoneLeft() - 30) * 0.5f - 78);
        if (Rect{gx, 6, 17, 13}.contains(e.x, e.y)) { plainToggle("Player/MultiSampleMap/RoundRobin", "Change Round Robin"); return; }
        const auto chooser = [&](Rect r, const std::string& key, const std::vector<std::string>& labels) {
            if (! r.contains(e.x, e.y)) return false;
            choose(this, labels, (int) value(device, key, 0), [this, key](int pick) { setValue(key, pick, "Change Round Robin"); });
            return true;
        };
        if (chooser({gx + 20, 6, 47, 13}, "Player/MultiSampleMap/RoundRobinMode", kRoundRobinModes)) return;
        if (chooser({gx + 105, 6, 47, 13}, "Player/MultiSampleMap/RoundRobinResetPeriod", kResetPeriods)) return;
        if (axis() == sampler::Axis::Selector && e.x >= zoneLeft())
        {
            drag_.what = Drag::What::Selector;
            mouseDrag(e);
        }
        return;
    }
    for (size_t i = 0; i < device.sampleParts.size(); ++i)
    {
        const auto row = rowBounds((int) i);
        if (! row.contains(e.x, e.y)) continue;
        const int part = (int) i;
        const float zx = zoneLeft();
        if (e.x < zx)
        {
            if (Rect{zx - 42, row.y + 1.5f, kButton, kButton}.contains(e.x, e.y))
            {
                const double v = sampler::partValue(device.sampleParts[i], "IsActive", 1) > 0.5 ? 0 : 1;
                editPart(part, [v](model::SamplePart& p) { sampler::setPartValue(p, "IsActive", v); }, "Change Layer Activator");
                return;
            }
            if (Rect{zx - 21, row.y + 1.5f, kButton, kButton}.contains(e.x, e.y))
            {
                const bool solo = sampler::partValue(device.sampleParts[i], "Solo", 0) < 0.5;
                const bool exclusive = ! e.cmd;
                edit([part, solo, exclusive](model::Device& d) {
                    for (size_t k = 0; k < d.sampleParts.size(); ++k)
                        if ((int) k == part) sampler::setPartValue(d.sampleParts[k], "Solo", solo ? 1 : 0);
                        else if (exclusive && sampler::partValue(d.sampleParts[k], "Solo", 0) > 0.5) sampler::setPartValue(d.sampleParts[k], "Solo", 0);
                    d.samplePartsEdited = true;
                }, "Solo Layer");
                return;
            }
            ctx().samplerViews[device.id].part = part; // selects the layer for the Sample tab
            Widget* root = this;
            while (root->parent() != nullptr) root = root->parent();
            root->layout(); // the Sample tab follows the selection
            repaint();
            return;
        }
        ctx().samplerViews[device.id].part = part;
        const auto range = sampler::zoneRange(device.sampleParts[i], axis());
        const auto zone = zoneBar(part), fade = fadeBar(part);
        const float edge = 3;
        drag_.part = part;
        drag_.start = drag_.current = range;
        drag_.startX = e.x;
        if (e.y >= fade.y - 1.5f && e.y < fade.bottom() + 1.5f)
        {
            const float f0 = valueX((float) range.fadeMin), f1 = valueX((float) range.fadeMax + 1);
            drag_.what = std::abs(e.x - f0) <= edge ? Drag::What::FadeMin : std::abs(e.x - f1) <= edge ? Drag::What::FadeMax : Drag::What::Move;
        }
        else if (e.y >= zone.y - 1.5f && e.y < zone.bottom() + 2)
            drag_.what = std::abs(e.x - zone.x) <= edge ? Drag::What::Min : std::abs(e.x - zone.right()) <= edge ? Drag::What::Max : Drag::What::Move;
        else drag_ = {};
        repaint();
        return;
    }
}

void SamplerZoneEditor::mouseDrag(const MouseEvent& e)
{
    using W = Drag::What;
    if (drag_.what == W::None) return;
    if (drag_.what == W::Selector)
    {
        pendingSelector_ = std::clamp(std::round((e.x - zoneLeft()) / kCell), 0.0f, 127.0f);
        int index = -1;
        if (auto* p = processor(index, "Player/SampleSelector")) { p->setParameter(index, (float) pendingSelector_); pendingSelector_ = -1; }
        repaint();
        return;
    }
    const int dv = (int) std::lround((e.x - drag_.startX) / kCell);
    const int low = axis() == sampler::Axis::Velocity ? 1 : 0;
    auto r = drag_.start;
    switch (drag_.what)
    {
        case W::Move:
        {
            const int shift = std::clamp(dv, low - r.min, 127 - r.max);
            r.min += shift; r.max += shift; r.fadeMin += shift; r.fadeMax += shift;
            break;
        }
        case W::Min: r.min = std::clamp(r.min + dv, low, r.max); r.fadeMin = std::max(r.fadeMin, r.min); r.fadeMax = std::max(r.fadeMax, r.fadeMin); break;
        case W::Max: r.max = std::clamp(r.max + dv, r.min, 127); r.fadeMax = std::min(r.fadeMax, r.max); r.fadeMin = std::min(r.fadeMin, r.fadeMax); break;
        case W::FadeMin: r.fadeMin = std::clamp(r.fadeMin + dv, r.min, r.fadeMax); break;
        case W::FadeMax: r.fadeMax = std::clamp(r.fadeMax + dv, r.fadeMin, r.max); break;
        case W::None: case W::Selector: break;
    }
    drag_.current = r;
    repaint();
}

void SamplerZoneEditor::mouseUp(const MouseEvent&)
{
    using W = Drag::What;
    const auto drag = drag_;
    drag_ = {};
    if (drag.what == W::Selector)
    {
        if (pendingSelector_ >= 0) setValue("Player/SampleSelector", pendingSelector_, "Change Sample Selector"); // no engine: one undo step
        if (ctx().editDevice) pendingSelector_ = -1;
        return;
    }
    if (drag.what == W::None || drag.part < 0) return;
    const auto a = axis();
    const auto range = drag.current;
    if (range.min == drag.start.min && range.max == drag.start.max && range.fadeMin == drag.start.fadeMin && range.fadeMax == drag.start.fadeMax)
    {
        repaint();
        return;
    }
    editPart(drag.part, [a, range](model::SamplePart& p) { sampler::setZoneRange(p, a, range); },
             a == sampler::Axis::Key ? "Change Key Zone" : a == sampler::Axis::Velocity ? "Change Velocity Zone" : "Change Selector Zone");
    repaint();
}

bool SamplerZoneEditor::mouseWheel(const MouseEvent&) { return false; }

bool SamplerZoneEditor::browserDrop(const BrowserDrag& payload, const MouseEvent&)
{
    const auto t = target();
    if (payload.kind != BrowserDrag::Kind::Sample || ! t.device || ! ctx().dropDeviceItem) return false;
    ctx().dropDeviceItem(payload, t.where.track, t.where.device, t.where.chain, (int) t.device->sampleParts.size(), t.where.child);
    return true;
}
} // namespace sonora::ui
