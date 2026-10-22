#include "ui/views/WavetableModulationEditor.h"
#include "dsp/ExponentialCurveGraph.h"
#include "ui/Painter.h"
#include "ui/UiContext.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPathBuilder.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace sonora::ui
{
namespace
{
using Graph = dsp::CurveGraph;
constexpr SkColor background = 0xff11171f, panel = 0xff202a35, raised = 0xff2d3946, line = 0xff3b4b5c;
constexpr SkColor text = 0xffe5edf7, dim = 0xff96a9bd, cyan = 0xff43d9e7, blue = 0xff4f9eff;
constexpr std::array<float, 10> columns{0, 0.03f, 0.18f, 0.32f, 0.57f, 0.66f, 0.73f, 0.82f, 0.91f, 1};
constexpr std::array<const char*, 9> labels{"#", "SOURCE", "VIA", "DESTINATION", "AMOUNT", "BIPOLAR", "OUT", "SRC CURVE", "VIA CURVE"};
class HitButton final : public Widget
{
public:
    explicit HitButton(std::function<void()> action) : action_(std::move(action)) { bypassMapMode = true; }
    void mouseUp(const MouseEvent& event) override
    { const auto action = action_; if (localBounds().contains(event.x, event.y) && action) action(); }
private:
    std::function<void()> action_;
};
void cell(SkCanvas& canvas, Rect area, const std::string& label, bool active = false)
{
    paint::fillRounded(canvas, area, 2, active ? 0xff246980 : raised); paint::stroke(canvas, area, active ? cyan : line);
    paint::text(canvas, label, area.reduced(4, 0), text, 9, Align::Centre);
}
std::string number(float value)
{ char buffer[32]; std::snprintf(buffer, sizeof(buffer), "%.2f", static_cast<double>(value)); return buffer; }
Graph bounded(Graph graph)
{
    graph.count = std::clamp(graph.count, 2, Graph::kMaxPoints);
    for (int i = 0; i < graph.count; ++i)
    {
        if (! std::isfinite(graph.x[i])) graph.x[i] = static_cast<float>(i) / static_cast<float>(graph.count - 1);
        if (! std::isfinite(graph.y[i])) graph.y[i] = 0;
        if (! std::isfinite(graph.curve[i])) graph.curve[i] = 0;
        graph.x[i] = std::clamp(graph.x[i], i > 0 ? graph.x[i - 1] : 0.0f, 1.0f);
        graph.y[i] = std::clamp(graph.y[i], 0.0f, 1.0f); graph.curve[i] = std::clamp(graph.curve[i], -1.0f, 1.0f);
    }
    graph.x[0] = 0; graph.x[graph.count - 1] = 1; return graph;
}
} // namespace

WavetableModulationEditor::WavetableModulationEditor(Binding binding) : binding_(std::move(binding))
{
    binding_.slots = std::clamp(binding_.slots, 1, 64); curveButtons_.resize(static_cast<size_t>(binding_.slots));
    for (int slot = 0; slot < binding_.slots; ++slot)
    {
        curveButtons_[static_cast<size_t>(slot)].primary = add<HitButton>([this, slot] { openCurve(slot, false); });
        curveButtons_[static_cast<size_t>(slot)].auxiliary = add<HitButton>([this, slot] { openCurve(slot, true); });
    }
    for (int i = 0; i < 2; ++i)
    {
        const int direction = i == 0 ? -1 : 1;
        pages_[static_cast<size_t>(i)] = add<HitButton>([this, direction] {
            view_.page = (view_.page + pageCount() + direction) % pageCount(); storeView(); layout(); repaint();
        });
        selectors_[static_cast<size_t>(i)] = add<HitButton>([this, i] { view_.auxiliary = i > 0; storeView(); repaint(); });
        enables_[static_cast<size_t>(i)] = add<HitButton>([this, i] { toggleCurve(i > 0); });
        slots_[static_cast<size_t>(i)] = add<HitButton>([this, direction] {
            view_.selectedSlot = (view_.selectedSlot + binding_.slots + direction) % binding_.slots;
            view_.page = view_.selectedSlot / rows_; storeView(); repaint();
        });
    }
    close_ = add<HitButton>([this] { view_.curveOpen = false; storeView(); layout(); repaint(); });
    reset_ = add<HitButton>([this] { resetCurve(); });
}
void WavetableModulationEditor::restoreViewState(ViewState state)
{
    view_ = state; view_.selectedSlot = std::clamp(view_.selectedSlot, 0, binding_.slots - 1);
    view_.page = std::clamp(view_.page, 0, pageCount() - 1); layout(); repaint();
}
void WavetableModulationEditor::storeView() { if (binding_.onViewChange) binding_.onViewChange(view_); }
int WavetableModulationEditor::pageCount() const { return (binding_.slots + rows_ - 1) / rows_; }
std::vector<int> WavetableModulationEditor::visibleSlots() const
{
    if (view_.curveOpen) return {view_.selectedSlot};
    std::vector<int> result;
    for (int slot = view_.page * rows_; slot < std::min(binding_.slots, (view_.page + 1) * rows_); ++slot) result.push_back(slot);
    return result;
}
Rect WavetableModulationEditor::rowCell(int slot) const
{
    if (view_.curveOpen || slot < view_.page * rows_ || slot >= std::min(binding_.slots, (view_.page + 1) * rows_)) return {};
    const float rowHeight = std::min(34.0f, std::max(24.0f, (height() - 52) / static_cast<float>(rows_)));
    return {4, 26 + static_cast<float>(slot - view_.page * rows_) * rowHeight, std::max(0.0f, width() - 8), rowHeight - 2};
}
Rect WavetableModulationEditor::parameterCell(int slot, Field field) const
{
    const auto row = rowCell(slot); if (row.isEmpty() || field == Field::Count) return {};
    const auto column = static_cast<size_t>(field) + 1;
    return {row.x + columns[column] * row.w + 1, row.y, (columns[column + 1] - columns[column]) * row.w - 3, row.h};
}
Rect WavetableModulationEditor::curveCell(int slot, bool auxiliary) const
{
    const auto row = rowCell(slot); if (row.isEmpty()) return {};
    const size_t column = auxiliary ? 8 : 7;
    return {row.x + columns[column] * row.w + 1, row.y, (columns[column + 1] - columns[column]) * row.w - 3, row.h};
}
Rect WavetableModulationEditor::pageCell(int direction) const { return {direction < 0 ? 4.0f : width() - 30, height() - 23, 26, 19}; }
Rect WavetableModulationEditor::closeCell() const { return {width() - 62, 3, 56, 21}; }
Rect WavetableModulationEditor::resetCell() const { return {width() - 124, 3, 56, 21}; }
Rect WavetableModulationEditor::slotCell(int direction) const { return {direction < 0 ? 6.0f : 133.0f, 3, 24, 21}; }
Rect WavetableModulationEditor::curveSelectorCell(bool auxiliary) const
{
    const float half = (width() - 16) * 0.5f;
    return {8 + (auxiliary ? half : 0), 30, std::max(30.0f, half - 76), 22};
}
Rect WavetableModulationEditor::curveEnableCell(bool auxiliary) const
{ const auto selector = curveSelectorCell(auxiliary); return {selector.right() + 5, selector.y, 64, selector.h}; }
Rect WavetableModulationEditor::plot() const
{ return view_.curveOpen ? Rect{10, 61, std::max(1.0f, width() - 20), std::max(1.0f, height() - 88)} : Rect{}; }
void WavetableModulationEditor::layout()
{
    rows_ = std::clamp(static_cast<int>((height() - 52) / 30), 1, 8); view_.page = std::clamp(view_.page, 0, pageCount() - 1);
    for (int slot = 0; slot < binding_.slots; ++slot)
    {
        auto& buttons = curveButtons_[static_cast<size_t>(slot)];
        buttons.primary->visible = buttons.auxiliary->visible = ! rowCell(slot).isEmpty();
        buttons.primary->setBounds(curveCell(slot, false)); buttons.auxiliary->setBounds(curveCell(slot, true));
    }
    for (int i = 0; i < 2; ++i)
    {
        const auto index = static_cast<size_t>(i); pages_[index]->visible = ! view_.curveOpen && pageCount() > 1;
        pages_[index]->setBounds(pageCell(i == 0 ? -1 : 1));
        selectors_[index]->visible = enables_[index]->visible = slots_[index]->visible = view_.curveOpen;
        selectors_[index]->setBounds(curveSelectorCell(i > 0)); enables_[index]->setBounds(curveEnableCell(i > 0));
        slots_[index]->setBounds(slotCell(i == 0 ? -1 : 1));
    }
    close_->visible = reset_->visible = view_.curveOpen; close_->setBounds(closeCell()); reset_->setBounds(resetCell());
}
WavetableModulationEditor::ParameterBinding WavetableModulationEditor::parameter(int slot, Field field) const
{ return binding_.parameter ? binding_.parameter(slot, field) : ParameterBinding{}; }
WavetableModulationEditor::CurveSnapshot WavetableModulationEditor::curve(int slot, bool auxiliary) const
{ auto result = binding_.curve ? binding_.curve(slot, auxiliary) : CurveSnapshot{}; result.graph = bounded(result.graph); return result; }
float WavetableModulationEditor::value(const ParameterBinding& bound) const
{ return bound.index >= 0 && binding_.getParameter ? binding_.getParameter(bound.index) : bound.info.defaultValue; }
void WavetableModulationEditor::setValue(const ParameterBinding& bound, float next)
{
    if (bound.index < 0) return;
    const auto action = binding_.setParameter; const int index = bound.index;
    next = std::clamp(next, bound.info.minValue, bound.info.maxValue); repaint(); if (action) action(index, next);
}
std::pair<int, WavetableModulationEditor::Field> WavetableModulationEditor::fieldAt(float x, float y) const
{
    if (! view_.curveOpen)
        for (int slot : visibleSlots()) for (int field = 0; field < static_cast<int>(Field::Count); ++field)
            if (parameterCell(slot, static_cast<Field>(field)).contains(x, y)) return {slot, static_cast<Field>(field)};
    return {-1, Field::Count};
}
std::optional<midi::ControlTarget> WavetableModulationEditor::mappingTargetAt(float x, float y) const
{ const auto [slot, field] = fieldAt(x, y); return slot >= 0 ? parameter(slot, field).target : std::nullopt; }

void WavetableModulationEditor::paintCurve(SkCanvas& canvas, Rect area, const CurveSnapshot& snapshot, bool handles) const
{
    if (area.isEmpty()) return;
    paint::fill(canvas, area, background); paint::stroke(canvas, area, line);
    if (handles) for (int i = 1; i < 4; ++i)
    {
        paint::line(canvas, area.x + area.w * static_cast<float>(i) / 4, area.y, area.x + area.w * static_cast<float>(i) / 4, area.bottom(), paint::withAlpha(line, 120));
        paint::line(canvas, area.x, area.y + area.h * static_cast<float>(i) / 4, area.right(), area.y + area.h * static_cast<float>(i) / 4, paint::withAlpha(line, 120));
    }
    dsp::ExponentialCurveGraph prepared; if (snapshot.law == CurveLaw::Imported) prepared.prepare(snapshot.graph);
    const auto evaluate = [&](float phase) { return snapshot.law == CurveLaw::Imported ? prepared.eval(phase) : snapshot.graph.eval(phase); };
    const auto color = snapshot.enabled ? cyan : dim;
    SkPathBuilder path; const int steps = handles ? 256 : 48;
    for (int i = 0; i <= steps; ++i)
    {
        const float phase = static_cast<float>(i) / static_cast<float>(steps);
        const float x = area.x + phase * area.w, y = area.bottom() - evaluate(phase) * area.h;
        if (i == 0) path.moveTo(x, y); else path.lineTo(x, y);
    }
    SkPaint pen; pen.setAntiAlias(true); pen.setStyle(SkPaint::kStroke_Style); pen.setStrokeWidth(handles ? 1.8f : 1.2f); pen.setColor(color);
    canvas.drawPath(path.detach(), pen);
    if (! handles) return;
    for (int i = 0; i < snapshot.graph.count; ++i)
    {
        const float x = area.x + snapshot.graph.x[i] * area.w, y = area.bottom() - snapshot.graph.y[i] * area.h;
        pen.setStyle(SkPaint::kFill_Style); canvas.drawCircle(x, y, 3, pen);
        if (i + 1 < snapshot.graph.count && snapshot.graph.x[i + 1] > snapshot.graph.x[i] + 1e-4f
            && std::abs(snapshot.graph.y[i + 1] - snapshot.graph.y[i]) > 1e-4f)
        {
            const float phase = (snapshot.graph.x[i] + snapshot.graph.x[i + 1]) * 0.5f;
            pen.setStyle(SkPaint::kStroke_Style); canvas.drawCircle(area.x + phase * area.w, area.bottom() - evaluate(phase) * area.h, 3.5f, pen);
        }
    }
}
void WavetableModulationEditor::paint(SkCanvas& canvas, const Theme&)
{
    paint::fillRounded(canvas, localBounds(), 3, panel); paint::stroke(canvas, localBounds(), line);
    if (view_.curveOpen)
    {
        cell(canvas, slotCell(-1), "<"); cell(canvas, slotCell(1), ">");
        paint::text(canvas, "ROUTE " + std::to_string(view_.selectedSlot + 1), {34, 3, 95, 21}, text, 10, Align::Centre, true);
        cell(canvas, closeCell(), "CLOSE"); cell(canvas, resetCell(), "RESET");
        for (int auxiliary = 0; auxiliary < 2; ++auxiliary)
        {
            const auto snapshot = curve(view_.selectedSlot, auxiliary > 0);
            cell(canvas, curveSelectorCell(auxiliary > 0), auxiliary > 0 ? "VIA CURVE" : "SOURCE CURVE", view_.auxiliary == (auxiliary > 0));
            cell(canvas, curveEnableCell(auxiliary > 0), snapshot.enabled ? "ON" : "OFF", snapshot.enabled);
        }
        const auto snapshot = drag_ == Drag::Point || drag_ == Drag::Curve ? draft_ : curve(view_.selectedSlot, view_.auxiliary);
        paintCurve(canvas, plot(), snapshot, true);
        paint::text(canvas, "Double-click adds a point · Drag hollow handles to bend · Alt snaps", {10, height() - 23, width() - 20, 19}, dim, 8, Align::Centre);
        return;
    }
    const auto targets = binding_.targets ? binding_.targets() : std::vector<Target>{};
    const float rowWidth = std::max(0.0f, width() - 8);
    for (size_t i = 0; i < labels.size(); ++i)
        paint::text(canvas, labels[i], {4 + columns[i] * rowWidth, 3, (columns[i + 1] - columns[i]) * rowWidth, 19}, dim, 8, Align::Centre, true);
    for (int slot : visibleSlots())
    {
        const auto row = rowCell(slot); paint::text(canvas, std::to_string(slot + 1), {row.x, row.y, row.w * columns[1], row.h}, dim, 9, Align::Centre);
        for (int fieldIndex = 0; fieldIndex < static_cast<int>(Field::Count); ++fieldIndex)
        {
            const auto field = static_cast<Field>(fieldIndex); const auto bound = parameter(slot, field); const float current = value(bound);
            std::string label; bool active = false;
            if (field == Field::Destination)
            {
                const auto id = binding_.destination ? binding_.destination(slot) : std::string();
                const auto found = std::find_if(targets.begin(), targets.end(), [&](const auto& target) { return target.id == id; });
                label = found != targets.end() ? found->label : id.empty() ? "Off" : "Missing: " + id;
            }
            else if (field == Field::Bipolar) { active = current > 0.5f; label = active ? "+/−" : "+"; }
            else if (! bound.info.choices.empty()) label = bound.info.choices[static_cast<size_t>(std::clamp(static_cast<int>(std::lround(current)), 0, static_cast<int>(bound.info.choices.size()) - 1))];
            else label = number(current);
            const auto area = parameterCell(slot, field); cell(canvas, area, label, active);
            if (field == Field::Amount || field == Field::Out)
            {
                const float span = bound.info.maxValue - bound.info.minValue;
                const float normalized = span > 0 ? std::clamp((current - bound.info.minValue) / span, 0.0f, 1.0f) : 0;
                paint::line(canvas, area.x + 2, area.bottom() - 2, area.x + 2 + (area.w - 4) * normalized, area.bottom() - 2, field == Field::Out ? blue : cyan, 1.5f);
            }
        }
        for (int auxiliary = 0; auxiliary < 2; ++auxiliary)
            paintCurve(canvas, curveCell(slot, auxiliary > 0).reduced(2), curve(slot, auxiliary > 0), false);
    }
    if (pageCount() > 1) { cell(canvas, pageCell(-1), "<"); cell(canvas, pageCell(1), ">"); }
    paint::text(canvas, "ROUTES " + std::to_string(view_.page * rows_ + 1) + "–" + std::to_string(std::min(binding_.slots, (view_.page + 1) * rows_)) + " / " + std::to_string(binding_.slots),
                {34, height() - 23, width() - 68, 19}, dim, 8, Align::Centre);
}

void WavetableModulationEditor::openCurve(int slot, bool auxiliary)
{ view_.selectedSlot = slot; view_.auxiliary = auxiliary; view_.curveOpen = true; drag_ = Drag::None; storeView(); layout(); repaint(); }
void WavetableModulationEditor::commitCurve(const CurveSnapshot& snapshot)
{
    const auto action = binding_.commitCurve; const int slot = view_.selectedSlot; const bool auxiliary = view_.auxiliary;
    const auto graph = bounded(snapshot.graph); const bool enabled = snapshot.enabled; drag_ = Drag::None; dragIndex_ = -1;
    storeView(); repaint(); if (action) action(slot, auxiliary, graph, enabled);
}
void WavetableModulationEditor::toggleCurve(bool auxiliary)
{
    view_.auxiliary = auxiliary; auto snapshot = curve(view_.selectedSlot, auxiliary); snapshot.enabled = ! snapshot.enabled;
    commitCurve(snapshot);
}
void WavetableModulationEditor::resetCurve()
{ auto snapshot = curve(view_.selectedSlot, view_.auxiliary); snapshot.graph = Graph::preset(3); commitCurve(snapshot); }
void WavetableModulationEditor::stepChoice(int slot, Field field, int direction)
{
    const auto bound = parameter(slot, field); if (bound.info.choices.empty()) return;
    setValue(bound, std::clamp(std::round(value(bound)) + static_cast<float>(direction), 0.0f, static_cast<float>(bound.info.choices.size() - 1)));
}
void WavetableModulationEditor::chooseField(int slot, Field field)
{
    const auto bound = parameter(slot, field); if (bound.index < 0 || bound.info.choices.empty()) return;
    juce::PopupMenu menu; const int selected = static_cast<int>(std::lround(value(bound)));
    if (ctx().showChoices)
    {
        const auto show = ctx().showChoices; std::vector<bool> ticked(bound.info.choices.size());
        if (selected >= 0 && static_cast<size_t>(selected) < ticked.size()) ticked[static_cast<size_t>(selected)] = true;
        show(bound.info.choices, ticked, [this, bound](int result) {
            if (Widget::isAlive(this) && result >= 0 && static_cast<size_t>(result) < bound.info.choices.size()) setValue(bound, static_cast<float>(result));
        });
        return;
    }
    for (size_t i = 0; i < bound.info.choices.size(); ++i) menu.addItem(static_cast<int>(i) + 1, bound.info.choices[i], true, static_cast<int>(i) == selected);
    menu.showMenuAsync(juce::PopupMenu::Options(), [this, bound](int result) { if (result > 0 && Widget::isAlive(this)) setValue(bound, static_cast<float>(result - 1)); });
}
void WavetableModulationEditor::chooseDestination(int slot)
{
    const auto targets = binding_.targets ? binding_.targets() : std::vector<Target>{};
    const auto current = binding_.destination ? binding_.destination(slot) : std::string();
    const auto action = binding_.commitDestination;
    const auto select = [this, slot, action, targets](std::string id) {
        if (! Widget::isAlive(this) || ! action || std::none_of(targets.begin(), targets.end(), [&](const auto& target) { return target.id == id; })) return;
        view_.selectedSlot = slot; storeView(); repaint(); action(slot, id);
    };
    if (onChooseDestination) { const auto chooser = onChooseDestination; chooser(targets, current, select); return; }
    juce::PopupMenu menu; std::vector<std::string> groups;
    for (const auto& target : targets) if (std::find(groups.begin(), groups.end(), target.group) == groups.end()) groups.push_back(target.group);
    for (const auto& group : groups)
    {
        juce::PopupMenu children;
        for (size_t i = 0; i < targets.size(); ++i) if (targets[i].group == group)
            children.addItem(static_cast<int>(i) + 1, targets[i].label, true, targets[i].id == current);
        if (group.empty())
        {
            for (size_t i = 0; i < targets.size(); ++i) if (targets[i].group.empty())
                menu.addItem(static_cast<int>(i) + 1, targets[i].label, true, targets[i].id == current);
        }
        else menu.addSubMenu(group, children);
    }
    menu.showMenuAsync(juce::PopupMenu::Options(), [select, targets](int result) {
        if (result > 0 && static_cast<size_t>(result) <= targets.size()) select(targets[static_cast<size_t>(result - 1)].id);
    });
}
float WavetableModulationEditor::phaseAt(float x, bool snap) const
{ const auto area = plot(); const float phase = std::clamp((x - area.x) / area.w, 0.0f, 1.0f); return snap ? std::round(phase * 16) / 16 : phase; }
float WavetableModulationEditor::levelAt(float y, bool snap) const
{ const auto area = plot(); const float level = std::clamp((area.bottom() - y) / area.h, 0.0f, 1.0f); return snap ? std::round(level * 8) / 8 : level; }
int WavetableModulationEditor::pointAt(const Graph& graph, float x, float y) const
{
    const auto area = plot(); int result = -1; float distance = 49;
    for (int i = 0; i < graph.count; ++i)
    {
        const float dx = area.x + graph.x[i] * area.w - x, dy = area.bottom() - graph.y[i] * area.h - y;
        if (dx * dx + dy * dy <= distance) { result = i; distance = dx * dx + dy * dy; }
    }
    return result;
}
int WavetableModulationEditor::handleAt(const CurveSnapshot& snapshot, float x, float y) const
{
    const auto area = plot(); dsp::ExponentialCurveGraph prepared; if (snapshot.law == CurveLaw::Imported) prepared.prepare(snapshot.graph);
    for (int i = 0; i + 1 < snapshot.graph.count; ++i)
    {
        if (snapshot.graph.x[i + 1] <= snapshot.graph.x[i] + 1e-4f || std::abs(snapshot.graph.y[i + 1] - snapshot.graph.y[i]) <= 1e-4f) continue;
        const float phase = (snapshot.graph.x[i] + snapshot.graph.x[i + 1]) * 0.5f;
        const float level = snapshot.law == CurveLaw::Imported ? prepared.eval(phase) : snapshot.graph.eval(phase);
        const float dx = area.x + phase * area.w - x, dy = area.bottom() - level * area.h - y;
        if (dx * dx + dy * dy < 49) return i;
    }
    return -1;
}
void WavetableModulationEditor::insertPoint(float phase, float level)
{
    auto& graph = draft_.graph; if (graph.count >= Graph::kMaxPoints || phase <= 0 || phase >= 1) return;
    int at = 1; while (at < graph.count && graph.x[at] <= phase) ++at;
    for (int i = graph.count; i > at; --i) { graph.x[i] = graph.x[i - 1]; graph.y[i] = graph.y[i - 1]; graph.curve[i] = graph.curve[i - 1]; }
    graph.x[at] = phase; graph.y[at] = level; graph.curve[at] = graph.curve[at - 1]; ++graph.count; dragIndex_ = at;
}
void WavetableModulationEditor::mouseDown(const MouseEvent& event)
{
    drag_ = Drag::None; pressedSlot_ = -1; pressedField_ = Field::Count;
    if (view_.curveOpen)
    {
        if (! plot().reduced(-4).contains(event.x, event.y)) return;
        draft_ = curve(view_.selectedSlot, view_.auxiliary); dragIndex_ = pointAt(draft_.graph, event.x, event.y);
        if ((event.right || event.cmd) && dragIndex_ > 0 && dragIndex_ + 1 < draft_.graph.count)
        {
            for (int i = dragIndex_; i + 1 < draft_.graph.count; ++i)
            { draft_.graph.x[i] = draft_.graph.x[i + 1]; draft_.graph.y[i] = draft_.graph.y[i + 1]; draft_.graph.curve[i] = draft_.graph.curve[i + 1]; }
            --draft_.graph.count; commitCurve(draft_); return;
        }
        if (event.right || event.cmd) return;
        if (dragIndex_ >= 0) { drag_ = Drag::Point; return; }
        dragIndex_ = handleAt(draft_, event.x, event.y); if (dragIndex_ >= 0) drag_ = Drag::Curve;
        return;
    }
    const auto [slot, field] = fieldAt(event.x, event.y); if (slot < 0) return;
    pressedSlot_ = slot; pressedField_ = field; const auto bound = parameter(slot, field);
    if (event.right && binding_.parameterContext)
    { const auto action = binding_.parameterContext; const int index = bound.index; pressedSlot_ = -1; if (index >= 0) action(index); return; }
    if (field == Field::Amount || field == Field::Out)
    { drag_ = Drag::Parameter; dragY_ = event.y; dragValue_ = value(bound); }
}
void WavetableModulationEditor::mouseDrag(const MouseEvent& event)
{
    if (drag_ == Drag::Parameter)
    {
        const auto bound = parameter(pressedSlot_, pressedField_);
        const float range = bound.info.maxValue - bound.info.minValue;
        setValue(bound, dragValue_ + (dragY_ - event.y) * range / (event.shift ? 1000.0f : 200.0f)); return;
    }
    if (drag_ != Drag::Point && drag_ != Drag::Curve) return;
    auto& graph = draft_.graph; const int point = dragIndex_; if (point < 0 || point >= graph.count) return;
    if (drag_ == Drag::Point)
    {
        graph.y[point] = levelAt(event.y, event.alt);
        if (point > 0 && point + 1 < graph.count) graph.x[point] = std::clamp(phaseAt(event.x, event.alt), graph.x[point - 1], graph.x[point + 1]);
    }
    else if (point + 1 < graph.count)
    {
        const float fraction = std::clamp((levelAt(event.y, false) - graph.y[point]) / (graph.y[point + 1] - graph.y[point]), 0.02f, 0.98f);
        graph.curve[point] = draft_.law == CurveLaw::Imported
            ? 1 - 2 * (fraction < 0.5f ? std::cbrt(fraction / 4) : 1 - std::cbrt((1 - fraction) / 4))
            : fraction < 0.5f ? std::clamp((std::log(fraction) / std::log(0.5f) - 1) / 3, 0.0f, 1.0f)
                              : -std::clamp((std::log(1 - fraction) / std::log(0.5f) - 1) / 3, 0.0f, 1.0f);
    }
    repaint();
}
void WavetableModulationEditor::mouseUp(const MouseEvent& event)
{
    if (ignoreUp_) { ignoreUp_ = false; return; }
    if (drag_ == Drag::Point || drag_ == Drag::Curve) { const auto snapshot = draft_; commitCurve(snapshot); return; }
    drag_ = Drag::None; const int slot = pressedSlot_; const auto field = pressedField_; pressedSlot_ = -1;
    if (slot < 0 || ! parameterCell(slot, field).contains(event.x, event.y)) return;
    if (field == Field::Destination) { chooseDestination(slot); return; }
    const auto bound = parameter(slot, field);
    if (field == Field::Bipolar) { setValue(bound, value(bound) > 0.5f ? 0.0f : 1.0f); return; }
    if (field == Field::Source || field == Field::Via)
    {
        const auto area = parameterCell(slot, field);
        if (event.x < area.x + 12) { stepChoice(slot, field, -1); return; }
        if (event.x > area.right() - 12) { stepChoice(slot, field, 1); return; }
        chooseField(slot, field);
    }
}
void WavetableModulationEditor::mouseDoubleClick(const MouseEvent& event)
{
    if (! view_.curveOpen)
    {
        const auto [slot, field] = fieldAt(event.x, event.y); if (slot < 0 || field == Field::Destination) return;
        ignoreUp_ = true; const auto bound = parameter(slot, field); setValue(bound, bound.info.defaultValue); return;
    }
    if (! plot().contains(event.x, event.y)) return;
    draft_ = curve(view_.selectedSlot, view_.auxiliary); const int point = pointAt(draft_.graph, event.x, event.y);
    if (point > 0 && point + 1 < draft_.graph.count)
    {
        for (int i = point; i + 1 < draft_.graph.count; ++i)
        { draft_.graph.x[i] = draft_.graph.x[i + 1]; draft_.graph.y[i] = draft_.graph.y[i + 1]; draft_.graph.curve[i] = draft_.graph.curve[i + 1]; }
        --draft_.graph.count;
    }
    else if (point < 0) insertPoint(phaseAt(event.x, event.alt), levelAt(event.y, event.alt));
    else return;
    ignoreUp_ = true; const auto snapshot = draft_; commitCurve(snapshot);
}
void WavetableModulationEditor::focusLost() { drag_ = Drag::None; pressedSlot_ = -1; dragIndex_ = -1; repaint(); }
bool WavetableModulationEditor::mouseWheel(const MouseEvent& event)
{
    if (std::abs(event.wheelDeltaX) > std::abs(event.wheelDeltaY) || std::abs(event.wheelDeltaY) < 1e-6f) return false;
    const auto [slot, field] = fieldAt(event.x, event.y); if (slot < 0) return false;
    if (field == Field::Source || field == Field::Via) { stepChoice(slot, field, event.wheelDeltaY > 0 ? 1 : -1); return true; }
    if (field == Field::Amount || field == Field::Out)
    {
        const auto bound = parameter(slot, field); setValue(bound, value(bound) + event.wheelDeltaY * (event.shift ? 0.005f : 0.05f) * (bound.info.maxValue - bound.info.minValue)); return true;
    }
    return false;
}
bool WavetableModulationEditor::keyPressed(const KeyEvent& event)
{
    if (event.keyCode == juce::KeyPress::escapeKey && view_.curveOpen)
    { view_.curveOpen = false; drag_ = Drag::None; storeView(); layout(); repaint(); return true; }
    return false;
}
} // namespace sonora::ui
