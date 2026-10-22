#include "ui/views/ReferenceFace.h"

#include "devices/DeviceProcessor.h"
#include "dsp/Dsp.h"
#include "gfx/Fonts.h"
#include "ui/Controls.h"
#include "ui/UiContext.h"
#include "ui/widgets/Buttons.h"
#include "ui/widgets/Knob.h"

#include "include/core/SkPaint.h"
#include "include/core/SkPathBuilder.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <cmath>
#include <cstdio>

namespace sonora::ui
{
namespace
{
using P = devices::DeviceProcessor;

std::string printed(float value, const char* format)
{
    char text[48];
    std::snprintf(text, sizeof(text), format, (double) value);
    return text;
}

// Opens a choice menu; the pick is dropped when the widget died meanwhile (a chain rebuild).
void choose(Widget* owner, std::vector<std::string> labels, int current, std::function<void(int)> pick)
{
    std::vector<bool> ticks(labels.size());
    if (current >= 0 && (size_t) current < ticks.size()) ticks[(size_t) current] = true;
    auto safe = [owner, chosen = std::move(pick)](int index) { if (Widget::isAlive(owner) && index >= 0) chosen(index); };
    if (owner->ctx().showChoices) { owner->ctx().showChoices(labels, ticks, std::move(safe)); return; }
    juce::PopupMenu menu;
    for (size_t i = 0; i < labels.size(); ++i) menu.addItem((int) i + 1, labels[i], true, ticks[i]);
    menu.showMenuAsync(juce::PopupMenu::Options(), [select = std::move(safe)](int choice) { if (choice > 0) select(choice - 1); });
}

// Drag / wheel / double-click editing shared by the slider box and display values.
class ValueField : public Widget
{
public:
    ValueField(P* p, int index, ReferenceFace::Law law, ReferenceFace::Format format, std::function<float()> size)
        : p_(p), index_(index), law_(std::move(law)), format_(std::move(format)), size_(std::move(size)) {}
    float norm() const { return std::clamp(law_.toNorm(p_->getParameter(index_)), 0.0f, 1.0f); }
    std::string valueText() const
    {
        if (format_) return format_(p_->getParameter(index_));
        if (const auto* face = dynamic_cast<const ReferenceFace*>(parent())) return face->formatted(index_);
        return printed(p_->getParameter(index_), "%.2f");
    }
    bool choices() const { return ! p_->parameters()[(size_t) index_].choices.empty(); }
    void mouseDown(const MouseEvent& e) override { pressed_ = ! e.right; startY_ = e.y; start_ = norm(); moved_ = false; }
    void mouseDrag(const MouseEvent& e) override
    {
        if (! pressed_ || choices()) return;
        moved_ = moved_ || std::abs(e.y - startY_) > 2;
        set(start_ + (startY_ - e.y) / (e.shift ? 1000.0f : 200.0f));
    }
    void mouseUp(const MouseEvent& e) override
    {
        if (! pressed_) return;
        pressed_ = false;
        if (! choices() || moved_ || ! localBounds().contains(e.x, e.y)) return;
        const auto& info = p_->parameters()[(size_t) index_];
        choose(this, info.choices, (int) std::lround(p_->getParameter(index_)), [this](int pick) { p_->setParameter(index_, (float) pick); repaint(); });
    }
    void mouseDoubleClick(const MouseEvent& e) override
    {
        if (e.right) return;
        p_->setParameter(index_, p_->parameters()[(size_t) index_].defaultValue);
        repaint();
    }
    bool mouseWheel(const MouseEvent& e) override
    {
        if (e.right || std::abs(e.wheelDeltaX) > std::abs(e.wheelDeltaY)) return false;
        if (choices()) p_->setParameter(index_, std::round(p_->getParameter(index_)) + (e.wheelDeltaY > 0 ? 1.0f : -1.0f));
        else set(norm() + (e.wheelDeltaY > 0 ? 1.0f : -1.0f) * (e.shift ? 0.002f : 0.02f));
        repaint();
        return true;
    }

protected:
    void set(float norm)
    {
        const auto& info = p_->parameters()[(size_t) index_];
        p_->setParameter(index_, std::clamp(law_.fromNorm(std::clamp(norm, 0.0f, 1.0f)), info.minValue, info.maxValue));
        repaint();
    }
    P* p_;
    int index_;
    ReferenceFace::Law law_;
    ReferenceFace::Format format_;
    std::function<float()> size_;
    float startY_ = 0, start_ = 0;
    bool pressed_ = false, moved_ = false;
};

// The reference's light number box with a cyan fill up to the value ("100 %", "0.00 dB", "0.71").
class SliderBox final : public ValueField
{
public:
    using ValueField::ValueField;
    void paint(SkCanvas& c, const Theme& t) override
    {
        const auto r = localBounds();
        ctl::framed(c, t, r, t.color("ControlTextBack"));
        paint::fill(c, {1, 1, (r.w - 2) * norm(), r.h - 2}, t.color("RangeEditField"));
        paint::text(c, valueText(), r, t.color("ControlForeground"), size_(), Align::Centre);
    }
};

// A grey label and a cyan value on a dark display; choices add a small drop mark.
class DisplayValue final : public ValueField
{
public:
    DisplayValue(P* p, int index, std::string label, ReferenceFace::Law law, ReferenceFace::Format format, std::function<float()> size, bool mark)
        : ValueField(p, index, std::move(law), std::move(format), std::move(size)), label_(std::move(label)), mark_(mark) {}
    void paint(SkCanvas& c, const Theme& t) override
    {
        const float size = size_();
        float x = 0;
        if (! label_.empty())
        {
            paint::text(c, label_, {0, 0, width(), height()}, t.color("RetroDisplayScaleText"), size);
            x = gfx::measureText(label_, size) + size * 0.4f;
        }
        const auto value = valueText();
        paint::text(c, value, {x, 0, width() - x, height()}, t.color("RangeDefault"), size);
        if (mark_)
        {
            const float end = x + gfx::measureText(value, size) + size * 0.75f;
            ctl::triangle(c, end, height() * 0.55f, size * 0.5f, size * 0.3f, ctl::Dir::Down, t.color("RangeDefault"));
        }
    }

private:
    std::string label_;
    bool mark_;
};

// Drop-down chooser: the light field with a down arrow, showing the choice or a glyph for it.
class Chooser final : public Widget
{
public:
    Chooser(P* p, std::function<int()> index, ReferenceFace::IconPainter icon, std::function<float()> size)
        : p_(p), indexOf_(std::move(index)), icon_(std::move(icon)), size_(std::move(size)) {}
    void paint(SkCanvas& c, const Theme& t) override
    {
        const int index = indexOf_();
        const auto r = localBounds();
        const auto& info = p_->parameters()[(size_t) index];
        const int value = std::clamp((int) std::lround(p_->getParameter(index)), 0, std::max(0, (int) info.choices.size() - 1));
        const float size = size_();
        ctl::framed(c, t, r, t.color("ControlTextBack"));
        const auto ink = t.color("ControlForeground");
        if (icon_) icon_(c, t, {r.h * 0.16f, r.h * 0.24f, std::min(r.h * 1.3f, r.w - r.h * 1.1f), r.h * 0.52f}, value, ink);
        else if (! info.choices.empty()) paint::text(c, info.choices[(size_t) value], {size * 0.3f, 0, r.w - size * 1.2f, r.h}, ink, size);
        ctl::triangle(c, r.right() - size * 0.62f, r.centreY(), size * 0.62f, size * 0.42f, ctl::Dir::Down, ink);
    }
    void mouseUp(const MouseEvent& e) override
    {
        if (e.right || ! localBounds().contains(e.x, e.y)) return;
        const int index = indexOf_();
        const auto& info = p_->parameters()[(size_t) index];
        choose(this, info.choices, (int) std::lround(p_->getParameter(index)), [this, index](int pick) { p_->setParameter(index, (float) pick); repaint(); });
    }
    bool mouseWheel(const MouseEvent& e) override
    {
        if (e.right || std::abs(e.wheelDeltaX) > std::abs(e.wheelDeltaY)) return false;
        const int index = indexOf_();
        p_->setParameter(index, std::round(p_->getParameter(index)) + (e.wheelDeltaY > 0 ? -1.0f : 1.0f));
        repaint();
        return true;
    }

private:
    P* p_;
    std::function<int()> indexOf_;
    ReferenceFace::IconPainter icon_;
    std::function<float()> size_;
};

// Amber toggle / option button with the face's text size; `inert` buttons are drawn but explain instead of acting.
class FaceButton final : public TextButton
{
public:
    FaceButton(std::string label, std::function<float()> size) : TextButton(std::move(label)), size_(std::move(size)) {}
    void paint(SkCanvas& c, const Theme& t) override
    {
        // Inert buttons keep the reference's fill and frame. An unlit one (a mode Sonora does not implement) greys its
        // text; a lit one is the behaviour the processor always has and reads normally.
        const bool enabled = ! isEnabled || isEnabled(), on = isOn && isOn();
        ctl::toggle(c, t, localBounds(), enabled || on ? label() : std::string_view{}, on, t.color("ChosenDefault"), true, false, size_());
        if (! enabled && ! on) paint::text(c, label(), localBounds().reduced(1, 0), t.color("TextDisabled"), size_(), Align::Centre);
    }

private:
    std::function<float()> size_;
};
} // namespace

// ---------------------------------------------------------------- laws and text
ReferenceFace::Law ReferenceFace::Law::linear(float low, float high)
{
    return {[=](float v) { return (v - low) / (high - low); }, [=](float n) { return low + n * (high - low); }};
}
ReferenceFace::Law ReferenceFace::Law::logarithmic(float low, float high)
{
    return {[=](float v) { return std::log(std::max(v, low) / low) / std::log(high / low); },
            [=](float n) { return low * std::pow(high / low, n); }};
}
ReferenceFace::Law ReferenceFace::Law::power(float low, float high, float exponent)
{
    return {[=](float v) { return std::pow(std::clamp((v - low) / (high - low), 0.0f, 1.0f), exponent); },
            [=](float n) { return low + std::pow(std::max(n, 0.0f), 1.0f / exponent) * (high - low); }};
}

std::string ReferenceFace::significant(float v)
{
    const float a = std::abs(v);
    return printed(v, a >= 99.95f ? "%.0f" : a >= 9.995f ? "%.1f" : "%.2f");
}

std::string ReferenceFace::formatted(int parameter) const
{
    const auto& info = processor_->parameters()[(size_t) parameter];
    const float v = processor_->getParameter(parameter);
    if (! info.choices.empty()) return info.choices[(size_t) std::clamp((int) std::lround(v), 0, (int) info.choices.size() - 1)];
    if (info.unit == "Hz") return v >= 1000 ? significant(v / 1000) + " kHz" : significant(v) + " Hz";
    if (info.unit == "ms") return v >= 1000 ? significant(v / 1000) + " s" : significant(v) + " ms";
    if (info.unit == "dB") return significant(v) + " dB";
    if (info.unit == "lin") return v <= 0.0000317f ? "-inf dB" : significant(dsp::gainToDb(v)) + " dB";
    if (info.unit == "%") return significant(v * 100) + " %";
    return significant(v);
}

// ---------------------------------------------------------------- geometry
ReferenceFace::ReferenceFace(devices::DeviceProcessor* processor, float captureWidth, float originX, float originY, Tag tag)
    : processor_(processor), captureWidth_(captureWidth), originX_(originX), originY_(originY), tag_(std::move(tag))
{
}

float ReferenceFace::preferredWidth(float bodyHeight) const
{
    return (captureWidth_ - (collapsed_ ? collapseTo_ - collapseFrom_ : 0.0f)) * bodyHeight / kBodyHeight;
}

float ReferenceFace::column(float x) const
{
    if (collapsed_ && x >= collapseTo_) x -= collapseTo_ - collapseFrom_;
    else if (collapsed_ && x > collapseFrom_) x = collapseFrom_;
    return (x - originX_) * unit();
}

Rect ReferenceFace::at(float x0, float y0, float x1, float y1) const
{
    const float u = unit(), left = column(x0);
    return {left, (y0 - originY_) * u, column(x1) - left, (y1 - y0) * u};
}

void ReferenceFace::collapseColumns(float from, float to, bool collapsed)
{
    collapseFrom_ = from;
    collapseTo_ = to;
    collapsed_ = collapsed;
}

void ReferenceFace::place(Widget* widget, float x0, float y0, float x1, float y1) { items_.push_back({widget, x0, y0, x1, y1, {}}); }

void ReferenceFace::showWhen(Widget* widget, std::function<bool()> when)
{
    for (auto& item : items_)
        if (item.widget == widget) item.shown = when;
}

void ReferenceFace::layout()
{
    const float size = text();
    for (auto& item : items_)
    {
        item.widget->visible = (! item.shown || item.shown()) && ! collapsed(item.x0);
        item.widget->setBounds(at(item.x0, item.y0, item.x1, item.y1));
        if (auto* k = dynamic_cast<Knob*>(item.widget)) k->fontSize = size;
    }
}

void ReferenceFace::tick()
{
    if (animated_) repaint();
}

void ReferenceFace::relayoutChain()
{
    if (auto* box = parent())
    {
        if (auto* chain = box->parent()) chain->layout();
        else box->layout();
    }
    repaint();
}

// ---------------------------------------------------------------- controls
Knob* ReferenceFace::knob(int parameter, std::string label, float x0, float y0, float x1, float y1, Law law, Format format)
{
    auto* widget = knob([parameter] { return parameter; }, std::move(label), x0, y0, x1, y1, std::move(law), std::move(format));
    tag(widget, parameter);
    return widget;
}

Knob* ReferenceFace::knob(std::function<int()> parameterOf, std::string label, float x0, float y0, float x1, float y1, Law law, Format format)
{
    auto* p = processor_;
    Knob::Source source;
    source.minValue = 0;
    source.maxValue = 1;
    source.get = [p, parameterOf, law] { return std::clamp(law.toNorm(p->getParameter(parameterOf())), 0.0f, 1.0f); };
    source.set = [p, parameterOf, law](float n) {
        const int parameter = parameterOf();
        const auto& info = p->parameters()[(size_t) parameter];
        const float v = std::clamp(law.fromNorm(n), info.minValue, info.maxValue);
        p->setParameter(parameter, info.choices.empty() ? v : std::round(v));
    };
    source.defaultValue = std::clamp(law.toNorm(p->parameters()[(size_t) parameterOf()].defaultValue), 0.0f, 1.0f);
    source.format = [this, parameterOf, format](float) { return format ? format(processor_->getParameter(parameterOf())) : formatted(parameterOf()); };
    auto* widget = add<Knob>(std::move(label), std::move(source));
    widget->style = Knob::Style::Device;
    place(widget, x0, y0, x1, y1);
    return widget;
}

Knob* ReferenceFace::inertKnob(std::string label, float norm, std::string value, float x0, float y0, float x1, float y1)
{
    Knob::Source source;
    source.get = [norm] { return norm; };
    source.format = [value](float) { return value; };
    auto* widget = add<Knob>(std::move(label), std::move(source));
    widget->style = Knob::Style::Device;
    widget->isEnabled = [] { return false; };
    place(widget, x0, y0, x1, y1);
    return widget;
}

TextButton* ReferenceFace::toggle(int parameter, std::string label, float x0, float y0, float x1, float y1)
{
    auto* p = processor_;
    auto* button = add<FaceButton>(std::move(label), [this] { return text(); });
    button->isOn = [p, parameter] { return p->getParameter(parameter) > 0.5f; };
    button->onClick = [this, p, parameter] { p->setParameter(parameter, p->getParameter(parameter) > 0.5f ? 0.0f : 1.0f); layout(); repaint(); };
    tag(button, parameter);
    place(button, x0, y0, x1, y1);
    return button;
}

TextButton* ReferenceFace::option(int parameter, float value, std::string label, float x0, float y0, float x1, float y1)
{
    auto* p = processor_;
    auto* button = add<FaceButton>(std::move(label), [this] { return text(); });
    button->isOn = [p, parameter, value] { return std::abs(p->getParameter(parameter) - value) < 0.5f; };
    button->onClick = [this, p, parameter, value] { p->setParameter(parameter, value); layout(); repaint(); };
    tag(button, parameter);
    place(button, x0, y0, x1, y1);
    return button;
}

TextButton* ReferenceFace::button(std::string label, float x0, float y0, float x1, float y1, std::function<void()> onClick)
{
    auto* widget = add<FaceButton>(std::move(label), [this] { return text(); });
    widget->onClick = std::move(onClick);
    place(widget, x0, y0, x1, y1);
    return widget;
}

TextButton* ReferenceFace::inertButton(std::string label, bool lit, float x0, float y0, float x1, float y1, std::string why)
{
    auto* button = add<FaceButton>(std::move(label), [this] { return text(); });
    button->isOn = [lit] { return lit; };
    button->isEnabled = [] { return false; };
    button->tooltip = why;
    place(button, x0, y0, x1, y1);
    return button;
}

Widget* ReferenceFace::slider(int parameter, float x0, float y0, float x1, float y1, Law law, Format format)
{
    auto* widget = add<SliderBox>(processor_, parameter, std::move(law), std::move(format), [this] { return text(); });
    tag(widget, parameter);
    place(widget, x0, y0, x1, y1);
    return widget;
}

Widget* ReferenceFace::chooser(int parameter, float x0, float y0, float x1, float y1, IconPainter icon)
{
    auto* widget = chooser([parameter] { return parameter; }, x0, y0, x1, y1, std::move(icon));
    tag(widget, parameter);
    return widget;
}

Widget* ReferenceFace::chooser(std::function<int()> parameter, float x0, float y0, float x1, float y1, IconPainter icon)
{
    auto* widget = add<Chooser>(processor_, std::move(parameter), std::move(icon), [this] { return text(); });
    place(widget, x0, y0, x1, y1);
    return widget;
}

Widget* ReferenceFace::displayValue(int parameter, std::string label, float x0, float y0, float x1, float y1, Law law, Format format, bool menuMark)
{
    auto* widget = add<DisplayValue>(processor_, parameter, std::move(label), std::move(law), std::move(format), [this] { return text(); }, menuMark);
    tag(widget, parameter);
    place(widget, x0, y0, x1, y1);
    return widget;
}

// ---------------------------------------------------------------- painting
void ReferenceFace::fillBody(SkCanvas& c, const Theme& t, bool gutters) const
{
    paint::fill(c, localBounds(), t.color(gutters ? "SurfaceHighlight" : "SurfaceBackground"));
}

void ReferenceFace::panel(SkCanvas& c, const Theme& t, float x0, float y0, float x1, float y1) const
{
    paint::fillRounded(c, at(x0, y0, x1, y1), 3 * unit(), t.color("SurfaceBackground"));
}

void ReferenceFace::displayPanel(SkCanvas& c, const Theme& t, float x0, float y0, float x1, float y1) const
{
    paint::fillRounded(c, at(x0, y0, x1, y1), 4 * unit(), t.color("RetroDisplayBackground"));
}

void ReferenceFace::label(SkCanvas& c, const Theme& t, std::string_view s, float x0, float y0, float x1, float y1, Align align, bool bold, SkColor ink) const
{
    paint::text(c, s, at(x0, y0, x1, y1), ink != 0 ? ink : t.color("ControlForeground"), bold ? header() : text(), align, bold);
}

void ReferenceFace::led(SkCanvas& c, const Theme& t, float cx, float cy, float radius, bool lit) const
{
    const auto centre = at(cx, cy, cx, cy);
    SkPaint pen;
    pen.setAntiAlias(true);
    pen.setColor(lit ? t.color("ChosenPlay") : t.color("ClipSlotButton"));
    c.drawCircle(centre.x, centre.y, radius * unit(), pen);
}

// ---------------------------------------------------------------- glyphs
void eqBandIcon(SkCanvas& c, Rect r, int type, SkColor ink, float stroke)
{
    // Measured in the reference's band choosers: a 24 x 10 glyph on the chooser's light field.
    SkPathBuilder path;
    const float x = r.x, y = r.y, w = r.w, h = r.h, mid = y + h * 0.5f;
    switch (type)
    {
        case 0: // Low Cut 48: steep rise into a flat top
            path.moveTo(x + w * 0.18f, y + h).cubicTo(x + w * 0.3f, y + h * 0.1f, x + w * 0.35f, y, x + w * 0.55f, y).lineTo(x + w, y);
            break;
        case 1: // Low Cut 12: gentle rise
            path.moveTo(x, y + h).cubicTo(x + w * 0.25f, y + h * 0.2f, x + w * 0.35f, y, x + w * 0.6f, y).lineTo(x + w, y);
            break;
        case 2: // Low Shelf: two levels on the left meet the line
            path.moveTo(x, y).lineTo(x + w * 0.28f, y).lineTo(x + w * 0.48f, mid).lineTo(x + w * 0.28f, y + h).lineTo(x, y + h);
            path.moveTo(x + w * 0.48f, mid).lineTo(x + w, mid);
            break;
        case 3: // Bell: a diamond on the line
            path.moveTo(x, mid).lineTo(x + w * 0.22f, mid).lineTo(x + w * 0.38f, y).lineTo(x + w * 0.62f, y).lineTo(x + w * 0.78f, mid)
                .lineTo(x + w * 0.62f, y + h).lineTo(x + w * 0.38f, y + h).lineTo(x + w * 0.22f, mid);
            path.moveTo(x + w * 0.78f, mid).lineTo(x + w, mid);
            break;
        case 4: // Notch: the line dips to the bottom
            path.moveTo(x, y + h * 0.15f).lineTo(x + w * 0.38f, y + h * 0.15f).lineTo(x + w * 0.5f, y + h).lineTo(x + w * 0.62f, y + h * 0.15f).lineTo(x + w, y + h * 0.15f);
            break;
        case 5: // High Shelf: mirror of the low shelf
            path.moveTo(x, mid).lineTo(x + w * 0.52f, mid).lineTo(x + w * 0.72f, y).lineTo(x + w, y);
            path.moveTo(x + w * 0.52f, mid).lineTo(x + w * 0.72f, y + h).lineTo(x + w, y + h);
            break;
        case 6: // High Cut 12
            path.moveTo(x, y).lineTo(x + w * 0.4f, y).cubicTo(x + w * 0.65f, y, x + w * 0.75f, y + h * 0.2f, x + w, y + h);
            break;
        default: // High Cut 48: flat line into a steep fall
            path.moveTo(x, y).lineTo(x + w * 0.45f, y).cubicTo(x + w * 0.65f, y, x + w * 0.7f, y + h * 0.1f, x + w * 0.82f, y + h);
            break;
    }
    SkPaint pen;
    pen.setAntiAlias(true);
    pen.setStyle(SkPaint::kStroke_Style);
    pen.setStrokeWidth(stroke);
    pen.setStrokeJoin(SkPaint::kRound_Join);
    pen.setColor(ink);
    c.drawPath(path.detach(), pen);
}

void titleIcon(SkCanvas& c, TitleIcon icon, float cx, float cy, float d, SkColor ink)
{
    // Measured in the reference's title bars: 17.5-px circles, 1.6 px rings, glyphs about half the circle.
    SkPaint pen;
    pen.setAntiAlias(true);
    pen.setStyle(SkPaint::kStroke_Style);
    pen.setStrokeWidth(d * 0.09f);
    pen.setColor(ink);
    const float r = d * 0.5f - pen.getStrokeWidth() * 0.5f;
    c.drawCircle(cx, cy, r, pen);
    const float g = d * 0.26f;
    if (icon == TitleIcon::HotSwap)
    {
        // Two arrows chasing each other around the centre.
        const SkRect oval = SkRect::MakeLTRB(cx - g, cy - g, cx + g, cy + g);
        c.drawArc(oval, 200, 120, false, pen);
        c.drawArc(oval, 20, 120, false, pen);
        SkPaint fill;
        fill.setAntiAlias(true);
        fill.setColor(ink);
        const float a = d * 0.13f;
        SkPathBuilder head;
        head.moveTo(cx + g * std::cos(0.35f) + a, cy - g * std::sin(0.35f)).lineTo(cx + g * std::cos(0.35f) - a, cy - g * std::sin(0.35f))
            .lineTo(cx + g * std::cos(0.35f), cy - g * std::sin(0.35f) + a * 1.2f).close();
        head.moveTo(cx - g * std::cos(0.35f) - a, cy + g * std::sin(0.35f)).lineTo(cx - g * std::cos(0.35f) + a, cy + g * std::sin(0.35f))
            .lineTo(cx - g * std::cos(0.35f), cy + g * std::sin(0.35f) - a * 1.2f).close();
        c.drawPath(head.detach(), fill);
    }
    else if (icon == TitleIcon::Save)
    {
        // Floppy: filled body with a light label slot at the top.
        SkPaint fill;
        fill.setAntiAlias(true);
        fill.setColor(ink);
        const SkRect body = SkRect::MakeLTRB(cx - g, cy - g, cx + g, cy + g);
        c.drawRoundRect(body, d * 0.04f, d * 0.04f, fill);
        fill.setColor(SkColorSetARGB(255, 0xe4, 0xe5, 0xe9));
        c.drawRect(SkRect::MakeLTRB(cx - g * 0.55f, cy - g * 0.78f, cx + g * 0.4f, cy - g * 0.15f), fill);
    }
    else
    {
        SkPaint fill;
        fill.setAntiAlias(true);
        fill.setColor(ink);
        for (int i = -1; i <= 1; ++i) c.drawCircle(cx + (float) i * d * 0.21f, cy, d * 0.065f, fill);
    }
}
} // namespace sonora::ui
