#include "ui/views/CurveLfoEditor.h"
#include "devices/native/CurveLfo.h"
#include "ui/Controls.h"
#include "ui/Painter.h"
#include "ui/UiContext.h"
#include "ui/widgets/Buttons.h"
#include "ui/widgets/Knob.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPathBuilder.h"
#include "include/effects/SkGradientShader.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace sonora::ui
{
namespace
{
using Graph = dsp::CurveGraph;
using L = devices::CurveLfo;
constexpr float kLeftWidth = 300, kRouteWidth = 206, kGap = 12;
constexpr float kHeader = 24, kInset = 6, kHit = 7, kTabHeight = 17;
constexpr float kGridX = 1.0f / 16, kGridY = 1.0f / 8;
Graph& clipboard() { static Graph g = Graph::preset(0); return g; }

float snapTo(float v, float step) { return std::round(v / step) * step; }
std::string printf1(const char* format, double value)
{
    char text[48]; std::snprintf(text, sizeof(text), format, value); return text;
}
std::string formatValue(const std::string& unit, const std::vector<std::string>& choices, float v)
{
    if (! choices.empty()) return choices[(size_t) std::clamp((int) std::lround(v), 0, (int) choices.size() - 1)];
    if (unit == "Hz") return v < 10 ? printf1("%.2f Hz", v) : printf1("%.1f Hz", v);
    if (unit == "ms") return v < 10 ? printf1("%.1f ms", v) : printf1("%.0f ms", v);
    if (unit == "%") return printf1("%.0f %%", v * 100.0);
    if (unit == "smp") return printf1("%.0f smp", v);
    return printf1("%.0f", v);
}
// Route colours: one per destination (Cut, Res, Vol, Pan, Var), used by the routing rows and the graph tabs.
SkColor routeColour(int destination)
{
    static constexpr SkColor colours[L::kDestinations] = {0xffffb901, 0xffff6b3d, 0xff2fd3e8, 0xffb08cff, 0xff6fdc5a};
    return colours[std::clamp(destination, 0, L::kDestinations - 1)];
}
const char* routeName(int destination)
{
    static constexpr const char* names[L::kDestinations] = {"Cutoff", "Reso", "Volume", "Pan", "Var"};
    return names[std::clamp(destination, 0, L::kDestinations - 1)];
}
bool routeActive(const L& lfo, int destination, int graph)
{
    return (int) std::lround(lfo.getParameter(L::sourceParam(destination))) == graph
        && std::abs(lfo.getParameter(L::amountParam(destination))) > 0.001f;
}
void strokeCurve(SkCanvas& c, const Graph& graph, const Rect& r, SkColor colour, float width, int steps)
{
    SkPathBuilder path;
    for (int i = 0; i <= steps; ++i)
    {
        const float phase = (float) i / (float) steps;
        const float x = r.x + phase * r.w, y = r.y + (1 - graph.eval(phase)) * r.h;
        if (i == 0) path.moveTo(x, y); else path.lineTo(x, y);
    }
    SkPaint pen; pen.setAntiAlias(true); pen.setColor(colour); pen.setStrokeWidth(width); pen.setStyle(SkPaint::kStroke_Style);
    pen.setStrokeJoin(SkPaint::kRound_Join);
    c.drawPath(path.detach(), pen);
}

// Horizontal amount bar of a destination row: fills from the centre (bipolar) or the left, value text inside.
class RouteAmount final : public Knob
{
public:
    RouteAmount(Source source, SkColor colour)
        : Knob({}, source), get_(source.get), format_(source.format), min_(source.minValue), max_(source.maxValue), colour_(colour) {}
    void paint(SkCanvas& c, const Theme& t) override
    {
        const auto r = localBounds();
        ctl::framed(c, t, r, t.color("ControlTextBack"));
        const float v = get_ ? get_() : 0, span = std::max(1e-6f, max_ - min_);
        const float origin = min_ < 0 ? (0 - min_) / span : 0, at = std::clamp((v - min_) / span, 0.0f, 1.0f);
        const auto inner = r.reduced(1);
        const float x0 = inner.x + inner.w * std::min(origin, at), x1 = inner.x + inner.w * std::max(origin, at);
        paint::fill(c, {x0, inner.y, std::max(0.0f, x1 - x0), inner.h}, paint::withAlpha(colour_, 200));
        if (min_ < 0) // centre ticks, clear of the value text
        {
            const float cx = inner.x + inner.w * origin, tick = std::min(3.0f, inner.h * 0.2f);
            const auto colour = paint::withAlpha(t.color("ControlForeground"), 110);
            paint::line(c, cx, inner.y, cx, inner.y + tick, colour); paint::line(c, cx, inner.bottom() - tick, cx, inner.bottom(), colour);
        }
        paint::text(c, format_ ? format_(v) : std::string{}, r, t.color("ControlForeground"), t.metrics.smallFontSize, Align::Centre);
    }
private:
    std::function<float()> get_;
    std::function<std::string(float)> format_;
    float min_, max_;
    SkColor colour_;
};

// Source-graph chooser of a destination row: the graph number and a thumbnail of its curve. Drag or scroll to change;
// a click without a drag also opens that graph in the display.
class RouteGraph final : public Knob
{
public:
    RouteGraph(Source source, L* lfo, int destination) : Knob({}, source), lfo_(lfo), destination_(destination) {}
    void paint(SkCanvas& c, const Theme& t) override
    {
        const auto r = localBounds();
        const int graph = std::clamp((int) std::lround(lfo_->getParameter(L::sourceParam(destination_))), 0, L::kGraphs - 1);
        const bool shown = (int) std::lround(lfo_->getParameter(L::SelectedGraph)) == graph;
        paint::fill(c, r, t.color("RetroDisplayBackground"));
        if (shown) paint::stroke(c, r.reduced(0.5f), t.color("RetroDisplayForeground"), 1);
        paint::text(c, std::to_string(graph + 1), {r.x + 2, r.y, 16, r.h}, t.color("RetroDisplayScaleText"), t.metrics.smallFontSize, Align::Centre);
        strokeCurve(c, lfo_->graph(graph), {r.x + 19, r.y + 4, r.w - 23, r.h - 8}, routeColour(destination_), 1.25f, 40);
    }
    void mouseDown(const MouseEvent& e) override { downY_ = e.y; Knob::mouseDown(e); }
    void mouseUp(const MouseEvent& e) override
    {
        Knob::mouseUp(e);
        if (! e.right && std::abs(e.y - downY_) < 3)
            lfo_->setParameter(L::SelectedGraph, std::round(lfo_->getParameter(L::sourceParam(destination_))));
    }
private:
    L* lfo_;
    int destination_;
    float downY_ = 0;
};
} // namespace

// ---------------------------------------------------------------------------------------------------------------------
// Controls

Knob* CurveLfoEditor::knob(int parameter, int page, const std::string& label)
{
    const auto& info = processor_->parameters()[(size_t) parameter];
    Knob::Source source;
    source.get = [this, parameter] { return processor_->getParameter(parameter); };
    source.set = [this, parameter, discrete = ! info.choices.empty() || info.unit.empty()](float value) {
        processor_->setParameter(parameter, discrete ? std::round(value) : value);
    };
    source.minValue = info.minValue; source.maxValue = info.maxValue; source.defaultValue = info.defaultValue;
    source.bipolar = info.minValue < 0 && info.maxValue > 0;
    source.logarithmic = info.unit == "Hz";
    source.format = [unit = info.unit, choices = info.choices](float value) { return formatValue(unit, choices, value); };
    return attach(add<Knob>(label.empty() ? info.name : label, std::move(source)), parameter, page,
                  info.name + ": double-click to reset; Shift-drag for fine adjustment.");
}
Knob* CurveLfoEditor::attach(Knob* control, int parameter, int page, const std::string& tip)
{
    control->tooltip = tip;
    controls_.push_back({control, page});
    if (tag_) tag_(control, parameter);
    return control;
}
TextButton* CurveLfoEditor::choice(const std::string& label, int parameter, int value, int page)
{
    auto* control = add<TextButton>(label);
    control->isOn = [this, parameter, value] { return (int) std::lround(processor_->getParameter(parameter)) == value; };
    control->onClick = [this, parameter, value] { processor_->setParameter(parameter, (float) value); repaint(); };
    controls_.push_back({control, page});
    if (tag_) tag_(control, parameter);
    return control;
}
TextButton* CurveLfoEditor::toggle(const std::string& label, int parameter, int page)
{
    auto* control = choice(label, parameter, 1, page);
    control->onClick = [this, parameter] { processor_->setParameter(parameter, processor_->getParameter(parameter) > 0.5f ? 0.0f : 1.0f); repaint(); };
    return control;
}

CurveLfoEditor::CurveLfoEditor(devices::CurveLfo* processor, std::function<void(Widget*, int)> tag)
    : processor_(processor), tag_(std::move(tag))
{
    for (int p = 0; p < 3; ++p)
    {
        auto* tab = add<TextButton>(p == LfoPage ? "LFO" : p == FilterPage ? "Filter" : "MIDI");
        tab->isOn = [this, p] { return page_ == p; };
        tab->onClick = [this, p] { selectPage(p); };
        tabs_.push_back(tab);
    }
    // LFO page. Rate shows the synced division or the free rate, whichever Sync selects; dotted / triplet divisions
    // are only reachable while their switch is on.
    {
        auto allowed = [this](float value) {
            int index = std::clamp((int) std::lround(value), 0, L::kRates - 1);
            const int variant = index % 3;
            if ((variant == 1 && processor_->getParameter(L::Dotted) < 0.5f) || (variant == 2 && processor_->getParameter(L::Triplet) < 0.5f))
                index -= variant;
            return index;
        };
        const auto& info = processor_->parameters()[(size_t) L::Rate];
        Knob::Source source;
        source.get = [this] { return processor_->getParameter(L::Rate); };
        source.set = [this, allowed](float value) { processor_->setParameter(L::Rate, (float) allowed(value)); };
        source.minValue = info.minValue; source.maxValue = info.maxValue; source.defaultValue = info.defaultValue;
        source.format = [choices = info.choices, allowed](float value) { return choices[(size_t) allowed(value)]; };
        rate_ = attach(add<Knob>("Rate", std::move(source)), L::Rate, LfoPage,
                       "Rate: synced cycle length. Turn on Dotted or Triplet to reach those divisions.");
    }
    hertz_ = knob(L::Hertz, LfoPage, "Rate");
    swing_ = knob(L::Swing, LfoPage);
    phase_ = knob(L::Phase, LfoPage);
    pwm_ = knob(L::Pwm, LfoPage);
    depth_ = knob(L::Depth, LfoPage);
    smooth_ = knob(L::Smooth, LfoPage);
    offset_ = knob(L::Offset, LfoPage);
    sync_ = toggle("Sync", L::Sync, LfoPage);
    dotted_ = toggle("Dotted", L::Dotted, LfoPage);
    triplet_ = toggle("Triplet", L::Triplet, LfoPage);
    anchor_ = toggle("Anchor", L::Anchor, LfoPage);
    for (auto* b : {dotted_, triplet_, anchor_}) b->isEnabled = [this] { return processor_->getParameter(L::Sync) > 0.5f; };
    anchor_->tooltip = "Anchor: lock the cycle to the song position (off: free-running while the song plays).";

    // Filter page.
    filterType_ = knob(L::FilterType, FilterPage, "Type");
    cutoff_ = knob(L::Cutoff, FilterPage);
    resonance_ = knob(L::Resonance, FilterPage);
    filterVar_ = knob(L::FilterVar, FilterPage);
    filterMix_ = knob(L::FilterMix, FilterPage, "Mix");
    filterOn_ = toggle("Filter On", L::FilterOn, FilterPage);

    // MIDI page.
    for (int mode = 0; mode < 3; ++mode)
        retrig_.push_back(choice(mode == 0 ? "Free" : mode == 1 ? "Retrig" : "Env", L::NoteRetrig, mode, MidiPage));
    midiToggles_ = {toggle("Note Gate", L::NoteGate, MidiPage), toggle("Vel > PWM", L::VelPwm, MidiPage),
                    toggle("Note > Rate", L::NoteRate, MidiPage), toggle("Note > Cutoff", L::NoteCutoff, MidiPage)};
    {
        Knob::Source source;
        source.get = [this] { return processor_->getParameter(L::CcOut); };
        source.set = [this](float value) { processor_->setParameter(L::CcOut, std::round(value)); };
        source.minValue = 0; source.maxValue = 128; source.defaultValue = 0;
        source.format = [](float value) { const int cc = (int) std::lround(value); return cc <= 0 ? std::string("Off") : "CC " + std::to_string(cc - 1); };
        ccOut_ = attach(add<Knob>("CC Out", std::move(source)), L::CcOut, MidiPage, "CC Out: send the cutoff curve as this MIDI controller.");
    }
    ccDepth_ = knob(L::CcDepth, MidiPage);

    // Destination routes (always visible).
    for (int d : kRouteOrder)
    {
        Knob::Source graph;
        graph.get = [this, d] { return processor_->getParameter(L::sourceParam(d)); };
        graph.set = [this, d](float value) { processor_->setParameter(L::sourceParam(d), std::round(value)); };
        graph.minValue = 0; graph.maxValue = L::kGraphs - 1; graph.defaultValue = 0;
        auto* picker = add<RouteGraph>(std::move(graph), processor_, d);
        picker->tooltip = std::string(routeName(d)) + " graph: drag or scroll to pick one of the twelve graphs; click to edit it.";
        if (tag_) tag_(picker, L::sourceParam(d));
        routeGraphs_.push_back(picker);

        const auto& info = processor_->parameters()[(size_t) L::amountParam(d)];
        Knob::Source amount;
        amount.get = [this, d] { return processor_->getParameter(L::amountParam(d)); };
        amount.set = [this, d](float value) { processor_->setParameter(L::amountParam(d), value); };
        amount.minValue = info.minValue; amount.maxValue = info.maxValue; amount.defaultValue = info.defaultValue;
        amount.bipolar = info.minValue < 0;
        amount.format = [](float value) { return printf1("%.0f %%", value * 100.0); };
        auto* bar = add<RouteAmount>(std::move(amount), routeColour(d));
        bar->tooltip = std::string(routeName(d)) + " amount: drag up/down; double-click to reset.";
        if (tag_) tag_(bar, L::amountParam(d));
        routeAmounts_.push_back(bar);
    }
    revision_ = processor_->displayRevision();
}

void CurveLfoEditor::selectPage(int page)
{
    page_ = std::clamp(page, 0, 2);
    layout(); repaint();
}

Rect CurveLfoEditor::leftArea() const { return {0, 0, kLeftWidth, height()}; }
Rect CurveLfoEditor::routeArea() const { return {width() - kRouteWidth, 0, kRouteWidth, height()}; }
Rect CurveLfoEditor::routeRow(int destination) const
{
    const auto area = routeArea();
    int row = 0;
    for (int i = 0; i < L::kDestinations; ++i) if (kRouteOrder[i] == destination) row = i;
    const float h = std::clamp((area.h - kTabHeight - 5) / L::kDestinations, 16.0f, 26.0f);
    return {area.x, area.y + kTabHeight + 5 + (float) row * h, area.w, h};
}

void CurveLfoEditor::layout()
{
    const auto left = leftArea();
    for (size_t i = 0; i < tabs_.size(); ++i) tabs_[i]->setBounds({left.x + (float) i * 66, 0, 64, kTabHeight});
    for (const auto& control : controls_) control.widget->visible = control.page == page_;
    const float kw = 60, kh = std::clamp((height() - kTabHeight - 6) / 2, 36.0f, 62.0f), top = kTabHeight + 5;
    auto cell = [&](int column, int row) { return Rect{left.x + (float) column * kw, top + (float) row * kh, kw - 2, kh}; };
    auto stack = [&](int row) { return Rect{left.x + 4 * kw + 2, top + 2 + (float) row * 21, left.w - 4 * kw - 2, 17}; };

    syncShown_ = processor_->getParameter(L::Sync) > 0.5f;
    rate_->visible = page_ == LfoPage && syncShown_;
    hertz_->visible = page_ == LfoPage && ! syncShown_;
    rate_->setBounds(cell(0, 0)); hertz_->setBounds(cell(0, 0));
    swing_->setBounds(cell(1, 0)); phase_->setBounds(cell(2, 0)); pwm_->setBounds(cell(3, 0));
    depth_->setBounds(cell(0, 1)); smooth_->setBounds(cell(1, 1)); offset_->setBounds(cell(2, 1));
    sync_->setBounds(stack(0)); dotted_->setBounds(stack(1)); triplet_->setBounds(stack(2)); anchor_->setBounds(stack(3));

    filterType_->setBounds(cell(0, 0)); cutoff_->setBounds(cell(1, 0)); resonance_->setBounds(cell(2, 0)); filterVar_->setBounds(cell(3, 0));
    filterMix_->setBounds(cell(0, 1)); filterOn_->setBounds(stack(0));

    for (size_t i = 0; i < retrig_.size(); ++i) retrig_[i]->setBounds({left.x + 38 + (float) i * 58, top + 2, 56, 17});
    for (size_t i = 0; i < midiToggles_.size(); ++i)
        midiToggles_[i]->setBounds({left.x + (float) (i % 2) * 106, top + 26 + (float) (i / 2) * 21, 104, 17});
    ccOut_->setBounds({left.x + 216, top, 42, kh}); ccDepth_->setBounds({left.x + 258, top, 42, kh});

    for (size_t i = 0; i < routeGraphs_.size(); ++i)
    {
        const auto row = routeRow(kRouteOrder[i]);
        const float h = row.h - 5;
        routeGraphs_[i]->setBounds({row.x + 56, row.y + 1, 50, h});
        routeAmounts_[i]->setBounds({row.x + 110, row.y + 1, row.w - 110, h});
    }
}

void CurveLfoEditor::tick()
{
    if ((processor_->getParameter(L::Sync) > 0.5f) != syncShown_) layout();
    const auto revision = processor_->displayRevision();
    if (revision != revision_) { revision_ = revision; repaint(); }
}

// ---------------------------------------------------------------------------------------------------------------------
// Graph display geometry

Rect CurveLfoEditor::display() const
{
    const float x = kLeftWidth + kGap;
    return {x, 0, std::max(120.0f, width() - kRouteWidth - kGap - x), std::max(40.0f, height())};
}
Rect CurveLfoEditor::graphCell(int graph) const
{
    const auto d = display();
    const float buttons = 62 + 50 + 40 + 42 + 3 * 2 + 8;
    const float cell = std::clamp((d.w - buttons) / L::kGraphs, 14.0f, 40.0f);
    return {d.x + (float) graph * cell, 0, cell - 2, kHeader - 2};
}
Rect CurveLfoEditor::pasteCell() const { const auto d = display(); return {d.right() - 42, 2, 42, kHeader - 6}; }
Rect CurveLfoEditor::copyCell() const { const auto p = pasteCell(); return {p.x - 42, p.y, 40, p.h}; }
Rect CurveLfoEditor::warpCell() const { const auto c = copyCell(); return {c.x - 52, c.y, 50, c.h}; }
Rect CurveLfoEditor::shapeCell() const { const auto w = warpCell(); return {w.x - 64, w.y, 62, w.h}; }
Rect CurveLfoEditor::plot() const
{
    const auto d = display();
    return Rect{d.x, kHeader, d.w, std::max(20.0f, d.h - kHeader)}.reduced(kInset + 2, kInset);
}
float CurveLfoEditor::pointX(float phase) const { const auto p = plot(); return p.x + std::clamp(phase, 0.0f, 1.0f) * p.w; }
float CurveLfoEditor::pointY(float level) const { const auto p = plot(); return p.y + (1 - std::clamp(level, 0.0f, 1.0f)) * p.h; }
float CurveLfoEditor::phaseAt(float x, bool snap) const
{
    const auto p = plot(); const float v = std::clamp((x - p.x) / p.w, 0.0f, 1.0f);
    return snap ? snapTo(v, kGridX) : v;
}
float CurveLfoEditor::levelAt(float y, bool snap) const
{
    const auto p = plot(); const float v = std::clamp(1 - (y - p.y) / p.h, 0.0f, 1.0f);
    return snap ? snapTo(v, kGridY) : v;
}
int CurveLfoEditor::selectedGraph() const
{
    return std::clamp((int) std::lround(processor_->getParameter(L::SelectedGraph)), 0, L::kGraphs - 1);
}

// ---------------------------------------------------------------------------------------------------------------------
// Painting

void CurveLfoEditor::paint(SkCanvas& c, const Theme& t)
{
    const float small = t.metrics.smallFontSize;
    const auto dim = t.color("TextDisabled"), fg = t.color("ControlForeground");
    const auto left = leftArea();
    const float top = kTabHeight + 5;
    paint::line(c, left.x, kTabHeight + 2, left.right(), kTabHeight + 2, paint::withAlpha(dim, 90));
    if (page_ == FilterPage)
    {
        const float kh = std::clamp((height() - kTabHeight - 6) / 2, 36.0f, 62.0f);
        paint::text(c, "The Cutoff, Reso and Var routes", {left.x + 64, top + kh + 12, left.w - 64, 14}, dim, small);
        paint::text(c, "move these around their settings.", {left.x + 64, top + kh + 26, left.w - 64, 14}, dim, small);
    }
    else if (page_ == MidiPage)
    {
        paint::text(c, "Notes", {left.x, top + 2, 36, 17}, fg, small);
        paint::text(c, "Notes below C3 pick a graph per", {left.x, top + 74, 214, 14}, dim, small);
        paint::text(c, "destination (one octave each).", {left.x, top + 88, 214, 14}, dim, small);
    }
    paintDisplay(c, t);
    paintRoutes(c, t);
}

void CurveLfoEditor::paintRoutes(SkCanvas& c, const Theme& t)
{
    const auto area = routeArea();
    const float small = t.metrics.smallFontSize;
    const auto dim = t.color("TextDisabled"), fg = t.color("ControlForeground");
    paint::text(c, "Destination", {area.x, 0, 56, kTabHeight}, fg, small);
    paint::text(c, "Graph", {area.x + 56, 0, 50, kTabHeight}, dim, small, Align::Centre);
    paint::text(c, "Amount", {area.x + 110, 0, area.w - 110, kTabHeight}, dim, small, Align::Centre);
    paint::line(c, area.x, kTabHeight + 2, area.right(), kTabHeight + 2, paint::withAlpha(dim, 90));
    const int selected = selectedGraph();
    for (int d : kRouteOrder)
    {
        const auto row = routeRow(d);
        const bool active = std::abs(processor_->getParameter(L::amountParam(d))) > 0.001f;
        const bool shown = (int) std::lround(processor_->getParameter(L::sourceParam(d))) == selected;
        const float cy = row.y + 1 + (row.h - 5) * 0.5f;
        SkPaint dot; dot.setAntiAlias(true); dot.setColor(active ? routeColour(d) : paint::withAlpha(dim, 120));
        c.drawCircle(row.x + 5, cy, 3.5f, dot);
        paint::text(c, routeName(d), {row.x + 13, cy - 7, 42, 14}, active || shown ? fg : dim, small, Align::Left, shown);
    }
}

void CurveLfoEditor::paintDisplay(SkCanvas& c, const Theme& t)
{
    const int selected = selectedGraph();
    const float small = t.metrics.smallFontSize;
    const auto screen = t.color("RetroDisplayBackground"), scale = t.color("RetroDisplayScaleText");
    const auto accent = t.color("RetroDisplayForeground");
    const auto fg = t.color("ControlForeground");
    const auto d = display();

    // Graph tabs: number, thumbnail and a dot per destination that plays the graph.
    for (int g = 0; g < L::kGraphs; ++g)
    {
        const auto cell = graphCell(g);
        const bool on = g == selected;
        const auto tab = on ? Rect{cell.x, cell.y, cell.w, kHeader} : cell;
        paint::fill(c, tab, on ? screen : t.color("ControlTextBack"));
        if (! on) paint::stroke(c, cell, paint::withAlpha(t.color("ControlContrastFrame"), 90));
        strokeCurve(c, processor_->graph(g), {cell.x + 3, cell.y + 9, cell.w - 6, cell.h - 12}, on ? accent : paint::withAlpha(fg, 120), on ? 1.4f : 1.0f, 30);
        paint::text(c, std::to_string(g + 1), {cell.x + 2, cell.y, cell.w - 4, 10}, on ? scale : fg, small - 1.5f, Align::Left, on);
        float dotX = cell.right() - 3;
        for (int dest = L::kDestinations - 1; dest >= 0; --dest)
            if (routeActive(*processor_, dest, g))
            {
                SkPaint dot; dot.setAntiAlias(true); dot.setColor(routeColour(dest));
                c.drawCircle(dotX, cell.y + 4, 1.8f, dot); dotX -= 4.5f;
            }
    }
    ctl::dropdown(c, t, shapeCell(), Graph::shapeName(shapeIndex_));
    const int warpIndex = std::clamp((int) std::lround(processor_->getParameter(L::warpParam(selected))), 0, 6);
    ctl::dropdown(c, t, warpCell(), processor_->parameters()[(size_t) L::warpParam(selected)].choices[(size_t) warpIndex]);
    ctl::toggle(c, t, copyCell(), "Copy", false, accent);
    ctl::toggle(c, t, pasteCell(), "Paste", false, accent);

    const Rect screenArea{d.x, kHeader, d.w, d.h - kHeader};
    paint::fill(c, screenArea, screen);
    const auto area = plot();
    for (int i = 1; i < 16; ++i)
    {
        const float x = area.x + area.w * (float) i / 16;
        paint::line(c, x, area.y, x, area.bottom(), paint::withAlpha(scale, i % 4 == 0 ? (i == 8 ? 60 : 42) : 18));
    }
    for (int i = 1; i < 8; ++i)
    {
        const float y = area.y + area.h * (float) i / 8;
        paint::line(c, area.x, y, area.right(), y, paint::withAlpha(scale, i == 4 ? 52 : i % 2 == 0 ? 32 : 14));
    }
    paint::stroke(c, Rect{area.x, area.y, area.w, area.h}, paint::withAlpha(scale, 40));

    // Scope: the audio level through one cycle of this graph, mirrored around the centre: the input as a faint shadow,
    // the output (shaped by the LFO) in front of it.
    {
        constexpr int bins = L::kScopeBins;
        float peak = 0.25f;
        for (int i = 0; i < bins; ++i) peak = std::max(peak, processor_->scopeInput(i));
        const float half = area.h * 0.5f, cy = area.y + half;
        auto envelope = [&](bool output) {
            SkPathBuilder path;
            for (int i = 0; i < bins; ++i)
            {
                const float v = std::min(1.0f, (output ? processor_->scopeOutput(i) : processor_->scopeInput(i)) / peak);
                const float x = area.x + area.w * ((float) i + 0.5f) / bins, y = cy - v * half;
                if (i == 0) path.moveTo(area.x, y);
                path.lineTo(x, y);
            }
            for (int i = bins - 1; i >= 0; --i)
            {
                const float v = std::min(1.0f, (output ? processor_->scopeOutput(i) : processor_->scopeInput(i)) / peak);
                const float x = area.x + area.w * ((float) i + 0.5f) / bins, y = cy + v * half;
                if (i == bins - 1) path.lineTo(area.right(), y);
                path.lineTo(x, y);
            }
            path.close();
            return path.detach();
        };
        SkPaint wave; wave.setAntiAlias(true);
        wave.setColor(paint::withAlpha(scale, 26)); c.drawPath(envelope(false), wave);
        wave.setColor(paint::withAlpha(scale, 78)); c.drawPath(envelope(true), wave);
    }

    // The curve: translucent gradient fill, then the line.
    const auto graph = processor_->graph(selected);
    SkPathBuilder line, fill;
    constexpr int steps = 400;
    fill.moveTo(area.x, area.bottom());
    for (int i = 0; i <= steps; ++i)
    {
        const float phase = (float) i / steps;
        const float x = pointX(phase), y = pointY(graph.eval(phase));
        if (i == 0) line.moveTo(x, y); else line.lineTo(x, y);
        fill.lineTo(x, y);
    }
    fill.lineTo(area.right(), area.bottom()); fill.close();
    {
        const SkPoint ends[2] = {{0, area.y}, {0, area.bottom()}};
        const SkColor colours[2] = {paint::withAlpha(accent, 64), paint::withAlpha(accent, 6)};
        SkPaint shade; shade.setAntiAlias(true);
        shade.setShader(SkGradientShader::MakeLinear(ends, colours, nullptr, 2, SkTileMode::kClamp));
        c.drawPath(fill.detach(), shade);
    }
    SkPaint pen; pen.setAntiAlias(true); pen.setColor(accent); pen.setStrokeWidth(2); pen.setStyle(SkPaint::kStroke_Style);
    pen.setStrokeJoin(SkPaint::kRound_Join);
    c.drawPath(line.detach(), pen);

    // Points and bend handles.
    SkPaint hollow; hollow.setAntiAlias(true); hollow.setStyle(SkPaint::kStroke_Style); hollow.setStrokeWidth(1.5f);
    for (int i = 0; i + 1 < graph.count; ++i)
        if (graph.x[i + 1] > graph.x[i] + 1e-4f && std::abs(graph.y[i + 1] - graph.y[i]) > 1e-4f)
        {
            const float mid = (graph.x[i] + graph.x[i + 1]) * 0.5f;
            const bool hot = i == hoverHandle_ || (drag_ == Drag::Curve && i == dragIndex_);
            hollow.setColor(hot ? accent : paint::withAlpha(scale, 170));
            c.drawCircle(pointX(mid), pointY(graph.eval(mid)), hot ? 4.5f : 3.5f, hollow);
        }
    for (int i = 0; i < graph.count; ++i)
    {
        const bool hot = i == hoverPoint_ || (drag_ == Drag::Point && i == dragIndex_);
        const float x = pointX(graph.x[i]), y = pointY(graph.y[i]), r = hot ? 4.5f : 3.5f;
        paint::fill(c, {x - r - 1, y - r - 1, 2 * r + 2, 2 * r + 2}, screen);
        paint::fill(c, {x - r, y - r, 2 * r, 2 * r}, hot ? accent : scale);
    }

    // Playhead and the current level on the curve; dimmed while the transport is stopped.
    const bool running = processor_->displayRunning();
    const float position = processor_->displayPosition(selected);
    const float head = pointX(position);
    paint::line(c, head, area.y, head, area.bottom(), paint::withAlpha(scale, running ? 170 : 70), 1.25f);
    {
        SkPaint glow; glow.setAntiAlias(true);
        const float y = pointY(graph.eval(position));
        glow.setColor(paint::withAlpha(accent, running ? 70 : 30)); c.drawCircle(head, y, 7, glow);
        glow.setColor(running ? accent : paint::withAlpha(accent, 140)); c.drawCircle(head, y, 3.5f, glow);
    }

    // Caption: graph and its routes, timing on the right.
    std::string routes;
    for (int dest : kRouteOrder)
        if (routeActive(*processor_, dest, selected)) routes += std::string(routes.empty() ? "" : ", ") + routeName(dest);
    paint::text(c, "Graph " + std::to_string(selected + 1) + (routes.empty() ? "  (not routed)" : "  >  " + routes),
                {area.x + 4, area.y + 2, area.w * 0.6f, 14}, paint::withAlpha(scale, 200), small);
    std::string timing;
    if (processor_->getParameter(L::Sync) > 0.5f)
    {
        const auto& rates = processor_->parameters()[(size_t) L::Rate].choices;
        int index = std::clamp((int) std::lround(processor_->getParameter(L::Rate)), 0, L::kRates - 1);
        const int variant = index % 3;
        if ((variant == 1 && processor_->getParameter(L::Dotted) < 0.5f) || (variant == 2 && processor_->getParameter(L::Triplet) < 0.5f)) index -= variant;
        timing = rates[(size_t) index];
    }
    else timing = formatValue("Hz", {}, processor_->getParameter(L::Hertz));
    if (warpIndex != 3) timing += "  " + processor_->parameters()[(size_t) L::warpParam(selected)].choices[(size_t) warpIndex];
    if (! running) timing += "  (stopped)";
    paint::text(c, timing, {area.x + area.w * 0.4f, area.y + 2, area.w * 0.6f - 4, 14}, paint::withAlpha(scale, 200), small, Align::Right);
}

// ---------------------------------------------------------------------------------------------------------------------
// Editing

int CurveLfoEditor::pointAt(const Graph& g, float x, float y) const
{
    int best = -1; float bestDistance = kHit * kHit;
    for (int i = 0; i < g.count; ++i)
    {
        const float dx = pointX(g.x[i]) - x, dy = pointY(g.y[i]) - y, d = dx * dx + dy * dy;
        if (d <= bestDistance) { best = i; bestDistance = d; }
    }
    return best;
}
int CurveLfoEditor::handleAt(const Graph& g, float x, float y) const
{
    for (int i = 0; i + 1 < g.count; ++i)
    {
        if (g.x[i + 1] <= g.x[i] + 1e-4f || std::abs(g.y[i + 1] - g.y[i]) <= 1e-4f) continue;
        const float mid = (g.x[i] + g.x[i + 1]) * 0.5f;
        const float dx = pointX(mid) - x, dy = pointY(g.eval(mid)) - y;
        if (dx * dx + dy * dy <= kHit * kHit) return i;
    }
    return -1;
}
void CurveLfoEditor::insertPoint(Graph& g, float phase, float level) const
{
    if (g.count >= Graph::kMaxPoints) return;
    int at = 1;
    while (at < g.count && g.x[at] <= phase) ++at;
    for (int i = g.count; i > at; --i) { g.x[i] = g.x[i - 1]; g.y[i] = g.y[i - 1]; g.curve[i] = g.curve[i - 1]; }
    g.x[at] = phase; g.y[at] = level; g.curve[at] = g.curve[at - 1];
    ++g.count;
}

void CurveLfoEditor::chooseShape()
{
    std::vector<std::string> labels;
    std::vector<bool> ticks;
    for (int i = 0; i < Graph::kShapeCount; ++i) { labels.push_back(Graph::shapeName(i)); ticks.push_back(i == shapeIndex_); }
    auto pick = [this](int chosen) {
        if (! Widget::isAlive(this) || chosen < 0 || chosen >= Graph::kShapeCount) return;
        shapeIndex_ = chosen; processor_->setGraph(selectedGraph(), Graph::preset(chosen)); repaint();
    };
    if (ctx().showChoices) { ctx().showChoices(labels, ticks, std::move(pick)); return; }
    juce::PopupMenu menu;
    for (size_t i = 0; i < labels.size(); ++i) menu.addItem((int) i + 1, labels[i], true, ticks[i]);
    menu.showMenuAsync(juce::PopupMenu::Options(), [selection = std::move(pick)](int choice) { if (choice > 0) selection(choice - 1); });
}
void CurveLfoEditor::chooseWarp()
{
    const int graph = selectedGraph();
    const auto labels = processor_->parameters()[(size_t) L::warpParam(graph)].choices;
    std::vector<bool> ticks(labels.size());
    ticks[(size_t) std::clamp((int) std::lround(processor_->getParameter(L::warpParam(graph))), 0, (int) labels.size() - 1)] = true;
    auto pick = [this, graph](int chosen) {
        if (! Widget::isAlive(this) || chosen < 0) return;
        processor_->setParameter(L::warpParam(graph), (float) chosen); repaint();
    };
    if (ctx().showChoices) { ctx().showChoices(labels, ticks, std::move(pick)); return; }
    juce::PopupMenu menu;
    for (size_t i = 0; i < labels.size(); ++i) menu.addItem((int) i + 1, labels[i] + " speed", true, ticks[i]);
    menu.showMenuAsync(juce::PopupMenu::Options(), [selection = std::move(pick)](int choice) { if (choice > 0) selection(choice - 1); });
}

bool CurveLfoEditor::headerClick(const MouseEvent& e)
{
    if (e.y >= kHeader || ! display().contains(e.x, e.y)) return false;
    const int selected = selectedGraph();
    for (int g = 0; g < L::kGraphs; ++g)
        if (graphCell(g).contains(e.x, e.y)) { processor_->setParameter(L::SelectedGraph, (float) g); repaint(); return true; }
    if (shapeCell().contains(e.x, e.y)) chooseShape();
    else if (warpCell().contains(e.x, e.y)) chooseWarp();
    else if (copyCell().contains(e.x, e.y)) clipboard() = processor_->graph(selected);
    else if (pasteCell().contains(e.x, e.y)) processor_->setGraph(selected, clipboard());
    repaint();
    return true;
}

void CurveLfoEditor::mouseDown(const MouseEvent& e)
{
    drag_ = Drag::None; dragIndex_ = -1;
    if (routeArea().contains(e.x, e.y))
    {
        for (int d : kRouteOrder)
            if (const auto row = routeRow(d); row.contains(e.x, e.y) && e.x < row.x + 56)
                processor_->setParameter(L::SelectedGraph, std::round(processor_->getParameter(L::sourceParam(d))));
        repaint(); return;
    }
    if (! display().contains(e.x, e.y)) return;
    if (headerClick(e)) return;
    const int selected = selectedGraph();
    auto graph = processor_->graph(selected);
    const int point = pointAt(graph, e.x, e.y);
    if (point >= 0)
    {
        if (e.cmd && graph.count > 2 && point > 0 && point + 1 < graph.count)
        {
            for (int i = point; i + 1 < graph.count; ++i) { graph.x[i] = graph.x[i + 1]; graph.y[i] = graph.y[i + 1]; graph.curve[i] = graph.curve[i + 1]; }
            --graph.count; processor_->setGraph(selected, graph); hoverPoint_ = -1; repaint(); return;
        }
        drag_ = Drag::Point; dragIndex_ = point; repaint(); return;
    }
    if (const int handle = handleAt(graph, e.x, e.y); handle >= 0) { drag_ = Drag::Curve; dragIndex_ = handle; dragAll_ = e.alt; repaint(); return; }
    if (e.shift)
    {
        const float start = std::floor(phaseAt(e.x, false) / kGridX) * kGridX, level = levelAt(e.y, true);
        insertPoint(graph, start, level);
        insertPoint(graph, std::min(1.0f, start + kGridX), level);
        processor_->setGraph(selected, graph); repaint();
    }
}
void CurveLfoEditor::mouseDoubleClick(const MouseEvent& e)
{
    if (! display().contains(e.x, e.y)) { mouseDown(e); return; }
    if (e.y < kHeader) { headerClick(e); return; }
    const int selected = selectedGraph();
    auto graph = processor_->graph(selected);
    if (const int point = pointAt(graph, e.x, e.y); point >= 0)
    {
        if (graph.count > 2 && point > 0 && point + 1 < graph.count)
        {
            for (int i = point; i + 1 < graph.count; ++i) { graph.x[i] = graph.x[i + 1]; graph.y[i] = graph.y[i + 1]; graph.curve[i] = graph.curve[i + 1]; }
            --graph.count;
        }
    }
    else insertPoint(graph, phaseAt(e.x, e.alt), levelAt(e.y, e.alt));
    processor_->setGraph(selected, graph); hoverPoint_ = hoverHandle_ = -1; repaint();
    drag_ = Drag::None;
}
void CurveLfoEditor::mouseDrag(const MouseEvent& e)
{
    if (drag_ == Drag::None) return;
    const int selected = selectedGraph();
    auto graph = processor_->graph(selected);
    if (dragIndex_ < 0 || dragIndex_ >= graph.count) return;
    if (drag_ == Drag::Point)
    {
        const int i = dragIndex_;
        graph.y[i] = levelAt(e.y, e.alt);
        if (i > 0 && i + 1 < graph.count)
            graph.x[i] = std::clamp(phaseAt(e.x, e.alt), graph.x[i - 1], graph.x[i + 1]);
    }
    else
    {
        const int i = dragIndex_;
        if (i + 1 >= graph.count) return;
        // Choose the tension for which the segment midpoint follows the pointer vertically.
        const float span = graph.y[i + 1] - graph.y[i];
        const float s = std::clamp((levelAt(e.y, false) - graph.y[i]) / span, 0.02f, 0.98f);
        const float tension = s < 0.5f ? std::clamp((std::log(s) / std::log(0.5f) - 1) / 3, 0.0f, 1.0f)
                                       : -std::clamp((std::log(1 - s) / std::log(0.5f) - 1) / 3, 0.0f, 1.0f);
        for (int k = 0; k + 1 < graph.count; ++k)
            if (k == i || (dragAll_ && graph.x[k + 1] > graph.x[k] + 1e-4f && std::abs(graph.y[k + 1] - graph.y[k]) > 1e-4f))
                graph.curve[k] = tension;
    }
    processor_->setGraph(selected, graph); repaint();
}
void CurveLfoEditor::mouseUp(const MouseEvent&) { drag_ = Drag::None; dragIndex_ = -1; repaint(); }
void CurveLfoEditor::mouseMove(const MouseEvent& e)
{
    int point = -1, handle = -1;
    if (plot().reduced(-kHit).contains(e.x, e.y))
    {
        const auto graph = processor_->graph(selectedGraph());
        point = pointAt(graph, e.x, e.y);
        if (point < 0) handle = handleAt(graph, e.x, e.y);
    }
    if (point != hoverPoint_ || handle != hoverHandle_) { hoverPoint_ = point; hoverHandle_ = handle; repaint(); }
}
void CurveLfoEditor::mouseExit()
{
    if (hoverPoint_ >= 0 || hoverHandle_ >= 0) { hoverPoint_ = hoverHandle_ = -1; repaint(); }
}
bool CurveLfoEditor::mouseWheel(const MouseEvent& e)
{
    if (e.right || std::abs(e.wheelDeltaX) > std::abs(e.wheelDeltaY) || e.y >= kHeader || ! display().contains(e.x, e.y)) return false;
    const int step = e.wheelDeltaY > 0 ? -1 : 1, selected = selectedGraph();
    if (shapeCell().contains(e.x, e.y))
    {
        shapeIndex_ = (shapeIndex_ + step + Graph::kShapeCount) % Graph::kShapeCount;
        processor_->setGraph(selected, Graph::preset(shapeIndex_));
    }
    else if (warpCell().contains(e.x, e.y))
        processor_->setParameter(L::warpParam(selected), (float) std::clamp((int) std::lround(processor_->getParameter(L::warpParam(selected))) - step, 0, 6));
    else if (e.x < shapeCell().x)
        processor_->setParameter(L::SelectedGraph, (float) std::clamp(selected + step, 0, L::kGraphs - 1));
    else return false;
    repaint();
    return true;
}

} // namespace sonora::ui
