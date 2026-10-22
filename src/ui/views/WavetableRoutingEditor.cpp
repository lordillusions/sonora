#include "ui/views/WavetableRoutingEditor.h"
#include "ui/Painter.h"
#include "ui/UiContext.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace sonora::ui
{
namespace
{
constexpr SkColor panel = 0xff202a35, raised = 0xff2d3946, line = 0xff3b4b5c;
constexpr SkColor text = 0xffe5edf7, dim = 0xff96a9bd, cyan = 0xff43d9e7;
constexpr std::array<float, 8> columns{0, 0.11f, 0.36f, 0.50f, 0.63f, 0.76f, 0.89f, 1};
constexpr std::array<const char*, 7> sources{"OSC A", "OSC B", "OSC C", "NOISE", "SUB", "FILTER 1", "FILTER 2"};
constexpr std::array<const char*, 7> headings{"SOURCE", "DESTINATION", "BALANCE", "BUS 1", "BUS 2", "VIA ENV 1", "LEVEL"};
class HitButton final : public Widget
{
public:
    explicit HitButton(std::function<void()> action) : action_(std::move(action)) { bypassMapMode = true; }
    void mouseUp(const MouseEvent& event) override { const auto action = action_; if (localBounds().contains(event.x, event.y) && action) action(); }
private:
    std::function<void()> action_;
};
void cell(SkCanvas& canvas, Rect area, const std::string& label, bool on = false, bool dimmed = false)
{
    paint::fillRounded(canvas, area, 2, on ? 0xff246980 : raised); paint::stroke(canvas, area, on ? cyan : line);
    paint::text(canvas, label, area.reduced(3, 0), dimmed ? dim : text, 9, Align::Centre);
}
std::string formatted(float value, const devices::ParameterInfo& info)
{
    char buffer[48];
    const char* gainFormat = value > 0 && value < 0.0001f ? "%.2gx" : value > 0 && value < 0.01f ? "%.4fx" : "%.2fx";
    const char* format = info.unit == "%" ? "%.0f %%" : info.unit == "x" ? gainFormat : "%.2f";
    std::snprintf(buffer, sizeof(buffer), format, static_cast<double>(info.unit == "%" ? value * 100 : value));
    return buffer;
}
float toNormal(float value, const WavetableRoutingEditor::ParameterBinding& bound)
{
    if (bound.quadraticGain) return std::sqrt(std::clamp(value / 4, 0.0f, 1.0f));
    const float span = bound.info.maxValue - bound.info.minValue;
    return span > 0 ? std::clamp((value - bound.info.minValue) / span, 0.0f, 1.0f) : 0;
}
float fromNormal(float normal, const WavetableRoutingEditor::ParameterBinding& bound)
{
    normal = std::clamp(normal, 0.0f, 1.0f);
    return bound.quadraticGain ? 4 * normal * normal : bound.info.minValue + normal * (bound.info.maxValue - bound.info.minValue);
}
} // namespace
WavetableRoutingEditor::WavetableRoutingEditor(Binding binding) : binding_(std::move(binding))
{
    for (int i = 0; i < 2; ++i)
    {
        pageButtons_[static_cast<size_t>(i)] = add<HitButton>([this, i] {
            page_ = (page_ + pageCount() + (i == 0 ? -1 : 1)) % pageCount();
            if (binding_.onPageChange) binding_.onPageChange(page_); repaint();
        });
        returnButtons_[static_cast<size_t>(i)] = add<HitButton>([this, i] { chooseReturn(i); });
    }
}
void WavetableRoutingEditor::layout()
{
    rows_ = std::clamp(static_cast<int>((height() - 78) / 24), 1, 7); page_ = std::clamp(page_, 0, pageCount() - 1);
    for (int i = 0; i < 2; ++i)
    {
        pageButtons_[static_cast<size_t>(i)]->visible = pageCount() > 1;
        pageButtons_[static_cast<size_t>(i)]->setBounds(pageCell(i == 0 ? -1 : 1));
        returnButtons_[static_cast<size_t>(i)]->setBounds(returnCell(i));
    }
}
void WavetableRoutingEditor::restorePage(int page) { page_ = std::clamp(page, 0, pageCount() - 1); repaint(); }
Rect WavetableRoutingEditor::activeCell() const { return {5, 4, 82, 22}; }
Rect WavetableRoutingEditor::directCell() const { return {92, 4, std::min(180.0f, width() - 98), 22}; }
Rect WavetableRoutingEditor::returnCell(int bus) const
{ const float w = (width() - 14) / 2; return {5 + static_cast<float>(bus) * (w + 4), 28, w, 18}; }
Rect WavetableRoutingEditor::pageCell(int direction) const { return {direction < 0 ? 5.0f : width() - 30, height() - 22, 25, 18}; }
std::vector<int> WavetableRoutingEditor::visibleSources() const
{
    std::vector<int> result;
    for (int source = page_ * rows_; source < std::min(7, (page_ + 1) * rows_); ++source) result.push_back(source);
    return result;
}
Rect WavetableRoutingEditor::parameterCell(int source, Field field) const
{
    if (source < page_ * rows_ || source >= std::min(7, (page_ + 1) * rows_) || field == Field::Count
        || (source >= 5 && field == Field::FilterBalance) || (source < 5 && field == Field::Level)) return {};
    const auto bound = parameter(source, field); if (bound.index < 0) return {};
    const float w = width() - 8; const size_t column = static_cast<size_t>(field) + 1;
    return {4 + columns[column] * w + 1, 58 + static_cast<float>(source - page_ * rows_) * 24,
            (columns[column + 1] - columns[column]) * w - 3, 22};
}
WavetableRoutingEditor::ParameterBinding WavetableRoutingEditor::parameter(int source, Field field) const
{ return binding_.parameter ? binding_.parameter(source, field) : ParameterBinding{}; }
float WavetableRoutingEditor::value(const ParameterBinding& bound) const { return bound.index >= 0 && binding_.get ? binding_.get(bound.index) : bound.info.defaultValue; }
void WavetableRoutingEditor::setValue(const ParameterBinding& bound, float next)
{
    if (bound.index < 0) return; const auto action = binding_.set; const int index = bound.index;
    next = std::clamp(next, bound.info.minValue, bound.info.maxValue); repaint(); if (action) action(index, next);
}
std::pair<int, WavetableRoutingEditor::Field> WavetableRoutingEditor::fieldAt(float x, float y) const
{
    for (int source : visibleSources()) for (int field = 0; field < static_cast<int>(Field::Count); ++field)
        if (parameterCell(source, static_cast<Field>(field)).contains(x, y)) return {source, static_cast<Field>(field)};
    return {-1, Field::Count};
}
std::optional<midi::ControlTarget> WavetableRoutingEditor::mappingTargetAt(float x, float y) const
{
    if (binding_.headerParameter && activeCell().contains(x, y)) return binding_.headerParameter(false).target;
    if (binding_.headerParameter && directCell().contains(x, y)) return binding_.headerParameter(true).target;
    const auto [source, field] = fieldAt(x, y); return source >= 0 ? parameter(source, field).target : std::nullopt;
}
void WavetableRoutingEditor::paint(SkCanvas& canvas, const Theme&)
{
    paint::fillRounded(canvas, localBounds(), 3, panel); paint::stroke(canvas, localBounds(), line);
    const auto active = binding_.headerParameter ? binding_.headerParameter(false) : ParameterBinding{};
    const auto direct = binding_.headerParameter ? binding_.headerParameter(true) : ParameterBinding{};
    cell(canvas, activeCell(), "ACTIVE", value(active) > 0.5f);
    cell(canvas, directCell(), "DIRECT VOLUME  " + formatted(value(direct), direct.info));
    const auto returns = binding_.returns ? binding_.returns() : std::array<bool, 2>{false, false};
    for (int bus = 0; bus < 2; ++bus)
        cell(canvas, returnCell(bus), "BUS " + std::to_string(bus + 1) + " RETURN  →  " + (returns[static_cast<size_t>(bus)] ? "Main FX" : "Direct"));
    const float w = width() - 8;
    for (size_t column = 0; column < headings.size(); ++column)
        paint::text(canvas, headings[column], {4 + columns[column] * w, 47, (columns[column + 1] - columns[column]) * w, 10}, dim, 7.5f, Align::Centre, true);
    for (int source : visibleSources())
    {
        paint::text(canvas, sources[static_cast<size_t>(source)], {4, 58 + static_cast<float>(source - page_ * rows_) * 24, columns[1] * w, 22}, dim, 8, Align::Centre);
        for (int index = 0; index < static_cast<int>(Field::Count); ++index)
        {
            const auto field = static_cast<Field>(index); const auto area = parameterCell(source, field); if (area.isEmpty()) continue;
            const auto bound = parameter(source, field); const float current = value(bound);
            std::string label = formatted(current, bound.info);
            if (field == Field::Destination && ! bound.info.choices.empty())
                label = bound.info.choices[static_cast<size_t>(std::clamp(static_cast<int>(std::lround(current)), 0, static_cast<int>(bound.info.choices.size()) - 1))];
            if (field == Field::Envelope) label = current > 0.5f ? "ON" : "OFF";
            const bool dimmed = field == Field::Level && binding_.levelActive && ! binding_.levelActive(source);
            cell(canvas, area, label, field == Field::Envelope && current > 0.5f, dimmed);
            if (field != Field::Destination && field != Field::Envelope)
            {
                const float normal = toNormal(current, bound);
                paint::line(canvas, area.x + 2, area.bottom() - 2, area.x + 2 + (area.w - 4) * normal, area.bottom() - 2, dimmed ? dim : cyan, 1.5f);
            }
        }
    }
    if (pageCount() > 1) { cell(canvas, pageCell(-1), "<"); cell(canvas, pageCell(1), ">"); }
    paint::text(canvas, "SOURCE ROUTING · " + std::to_string(page_ + 1) + " / " + std::to_string(pageCount()), {34, height() - 22, width() - 68, 18}, dim, 8, Align::Centre);
}
void WavetableRoutingEditor::chooseDestination(int source, int step)
{
    const auto bound = parameter(source, Field::Destination); if (bound.index < 0 || bound.info.choices.empty()) return;
    const int selected = static_cast<int>(std::lround(value(bound)));
    if (step != 0)
    {
        for (int next = selected + step; next >= 0 && static_cast<size_t>(next) < bound.info.choices.size(); next += step)
            if (! binding_.destinationAllowed || binding_.destinationAllowed(source, next)) { setValue(bound, static_cast<float>(next)); return; }
        return;
    }
    const auto choose = [this, bound, source](int selectedIndex) {
        if (Widget::isAlive(this) && selectedIndex >= 0 && static_cast<size_t>(selectedIndex) < bound.info.choices.size()
            && (! binding_.destinationAllowed || binding_.destinationAllowed(source, selectedIndex))) setValue(bound, static_cast<float>(selectedIndex));
    };
    if (ctx().showChoices)
    {
        const auto show = ctx().showChoices; std::vector<bool> ticked(bound.info.choices.size());
        if (selected >= 0 && static_cast<size_t>(selected) < ticked.size()) ticked[static_cast<size_t>(selected)] = true;
        show(bound.info.choices, ticked, choose); return;
    }
    juce::PopupMenu menu;
    for (size_t i = 0; i < bound.info.choices.size(); ++i)
        menu.addItem(static_cast<int>(i) + 1, bound.info.choices[i], ! binding_.destinationAllowed || binding_.destinationAllowed(source, static_cast<int>(i)), static_cast<int>(i) == selected);
    menu.showMenuAsync(juce::PopupMenu::Options(), [choose](int result) { if (result > 0) choose(result - 1); });
}
void WavetableRoutingEditor::chooseReturn(int bus)
{
    const auto current = binding_.returns ? binding_.returns() : std::array<bool, 2>{false, false};
    const auto action = binding_.commitReturn;
    const auto choose = [this, bus, action](int result) { if (Widget::isAlive(this) && action && result >= 0 && result < 2) action(bus, result == 0); };
    if (ctx().showChoices)
    { const auto show = ctx().showChoices; show({"Main FX", "Direct"}, {current[static_cast<size_t>(bus)], ! current[static_cast<size_t>(bus)]}, choose); return; }
    juce::PopupMenu menu; menu.addItem(1, "Main FX", true, current[static_cast<size_t>(bus)]); menu.addItem(2, "Direct", true, ! current[static_cast<size_t>(bus)]);
    menu.showMenuAsync(juce::PopupMenu::Options(), [choose](int result) { if (result > 0) choose(result - 1); });
}
void WavetableRoutingEditor::mouseDown(const MouseEvent& event)
{
    dragging_ = false; headerPressed_ = false; pressedSource_ = -1;
    if (! binding_.headerParameter && (directCell().contains(event.x, event.y) || activeCell().contains(event.x, event.y))) return;
    if (directCell().contains(event.x, event.y)) { headerPressed_ = true; pressedField_ = Field::Level; }
    else if (activeCell().contains(event.x, event.y)) { headerPressed_ = true; pressedField_ = Field::Envelope; return; }
    else { const auto hit = fieldAt(event.x, event.y); pressedSource_ = hit.first; pressedField_ = hit.second; }
    if (! headerPressed_ && pressedSource_ < 0) return;
    const auto bound = headerPressed_ ? binding_.headerParameter(true) : parameter(pressedSource_, pressedField_);
    if (event.right && binding_.parameterContext) { const auto action = binding_.parameterContext; const int index = bound.index; pressedSource_ = -1; headerPressed_ = false; if (index >= 0) action(index); return; }
    if (pressedField_ != Field::Destination && pressedField_ != Field::Envelope) { dragging_ = true; dragY_ = event.y; dragNormal_ = toNormal(value(bound), bound); }
}
void WavetableRoutingEditor::mouseDrag(const MouseEvent& event)
{
    if (! dragging_) return; const auto bound = headerPressed_ ? binding_.headerParameter(true) : parameter(pressedSource_, pressedField_);
    setValue(bound, fromNormal(dragNormal_ + (dragY_ - event.y) / (event.shift ? 1000.0f : 200.0f), bound));
}
void WavetableRoutingEditor::mouseUp(const MouseEvent& event)
{
    if (ignoreUp_) { ignoreUp_ = false; return; } dragging_ = false;
    if (headerPressed_)
    {
        headerPressed_ = false;
        if (pressedField_ == Field::Envelope && activeCell().contains(event.x, event.y)) { const auto bound = binding_.headerParameter(false); setValue(bound, value(bound) > 0.5f ? 0.0f : 1.0f); }
        return;
    }
    const int source = pressedSource_; pressedSource_ = -1; if (source < 0 || ! parameterCell(source, pressedField_).contains(event.x, event.y)) return;
    if (pressedField_ == Field::Destination) { chooseDestination(source); return; }
    if (pressedField_ == Field::Envelope) { const auto bound = parameter(source, pressedField_); setValue(bound, value(bound) > 0.5f ? 0.0f : 1.0f); }
}
void WavetableRoutingEditor::mouseDoubleClick(const MouseEvent& event)
{
    if (directCell().contains(event.x, event.y)) { if (! binding_.headerParameter) return; ignoreUp_ = true; const auto bound = binding_.headerParameter(true); setValue(bound, bound.info.defaultValue); return; }
    const auto [source, field] = fieldAt(event.x, event.y);
    if (source < 0 || field == Field::Destination) return;
    ignoreUp_ = true; const auto bound = parameter(source, field); setValue(bound, bound.info.defaultValue);
}
bool WavetableRoutingEditor::mouseWheel(const MouseEvent& event)
{
    if (std::abs(event.wheelDeltaX) > std::abs(event.wheelDeltaY) || std::abs(event.wheelDeltaY) < 1e-6f) return false;
    if (directCell().contains(event.x, event.y)) { if (! binding_.headerParameter) return false; const auto bound = binding_.headerParameter(true); setValue(bound, fromNormal(toNormal(value(bound), bound) + event.wheelDeltaY * (event.shift ? 0.005f : 0.05f), bound)); return true; }
    const auto [source, field] = fieldAt(event.x, event.y); if (source < 0) return false;
    if (field == Field::Destination) { chooseDestination(source, event.wheelDeltaY > 0 ? 1 : -1); return true; }
    if (field == Field::Envelope) return false;
    const auto bound = parameter(source, field); setValue(bound, fromNormal(toNormal(value(bound), bound) + event.wheelDeltaY * (event.shift ? 0.005f : 0.05f), bound)); return true;
}
void WavetableRoutingEditor::focusLost() { dragging_ = false; headerPressed_ = false; pressedSource_ = -1; }
} // namespace sonora::ui
