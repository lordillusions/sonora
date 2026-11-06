// DEV-28b: the face of the imported Wavetable (`InstrumentVector`), matched to a 2000 x 561 capture of the reference
// device ("Grungey Ambient Pad", default light theme): the device body (x 5..1148, y 320..517, 197 px tall) and its
// expanded envelope / LFO view (y 7..290 above the chain). Sonora shows the expanded view to the right of the body,
// scaled to the body height (factor 197 / 285), and its expand switch left of the Osc tabs (the reference puts it next
// to the activator, where Sonora's title draws the name). Every box below is in the capture's pixels.

#include "ui/views/ReferenceFace.h"

#include "devices/native/WavetableInstrument.h"
#include "devices/native/WavetableSprites.h"
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
using W = devices::WavetableInstrument;
using Law = ReferenceFace::Law;
constexpr float kExpandX = 1151.0f, kExpandScale = 197.0f / 285.0f; // expanded view: top-area x 6 / y 6 map here
constexpr float kMainWidth = 1143.0f, kExpandedWidth = kMainWidth + 8.0f + (1990.0f - 6.0f) * kExpandScale;

std::string printed(float value, const char* format)
{
    char text[48];
    std::snprintf(text, sizeof(text), format, (double) value);
    return text;
}
std::string percent(float v)
{
    const float p = v * 100.0f;
    return printed(p, std::abs(p) >= 99.95f ? "%.0f %%" : "%.1f %%");
}
std::string decibels(float g) { return g <= 0.0000317f ? std::string("-inf dB") : printed(dsp::gainToDb(g) + 0.0f, "%.1f dB"); }
std::string seconds(float s)
{
    if (s >= 1.0f) return ReferenceFace::significant(s) + " s";
    return ReferenceFace::significant(s * 1000.0f) + " ms";
}
std::string panText(float pan)
{
    const int v = (int) std::lround(pan * 50.0f);
    return v == 0 ? std::string("C") : std::to_string(std::abs(v)) + (v < 0 ? "L" : "R");
}
std::string amountText(float a)
{
    const float p = a * 100.0f;
    return std::abs(p) >= 9.95f ? printed(p, "%.0f") : printed(p, "%.1f");
}

void stroke(SkCanvas& c, SkPathBuilder& path, SkColor ink, float width)
{
    SkPaint pen;
    pen.setAntiAlias(true);
    pen.setStyle(SkPaint::kStroke_Style);
    pen.setStrokeWidth(width);
    pen.setStrokeJoin(SkPaint::kRound_Join);
    pen.setStrokeCap(SkPaint::kRound_Cap);
    pen.setColor(ink);
    c.drawPath(path.detach(), pen);
}

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

// Filter type glyphs (the reference's filter header: a light glyph on the dark display).
void filterGlyph(SkCanvas& c, Rect r, int type, SkColor ink, float width)
{
    const float x = r.x, y = r.y, w = r.w, h = r.h, k = h * 0.45f;
    SkPathBuilder p;
    switch (type)
    {
        case 0: p.moveTo(x, y).lineTo(x + w * 0.6f, y).quadTo(x + w * 0.85f, y, x + w * 0.85f, y + k).lineTo(x + w * 0.85f, y + h); break;
        case 1: p.moveTo(x + w * 0.15f, y + h).lineTo(x + w * 0.15f, y + k).quadTo(x + w * 0.15f, y, x + w * 0.4f, y).lineTo(x + w, y); break;
        case 2: p.moveTo(x, y + h).quadTo(x + w * 0.5f, y - h * 0.4f, x + w, y + h); break;
        case 3: p.moveTo(x, y).lineTo(x + w * 0.35f, y).quadTo(x + w * 0.5f, y, x + w * 0.5f, y + h).quadTo(x + w * 0.5f, y, x + w * 0.65f, y).lineTo(x + w, y); break;
        default: p.moveTo(x, y).quadTo(x + w * 0.4f, y, x + w * 0.45f, y + h); p.moveTo(x + w * 0.55f, y + h).quadTo(x + w * 0.6f, y, x + w, y); break;
    }
    stroke(c, p, ink, width);
}

// LFO shape glyphs of the expanded view (sine, triangle, saw, square, random).
void lfoGlyph(SkCanvas& c, Rect r, int shape, SkColor ink, float width)
{
    const float x = r.x, y = r.y, w = r.w, h = r.h, mid = y + h * 0.5f;
    SkPathBuilder p;
    switch (shape)
    {
        case 0:
            p.moveTo(x, mid);
            for (int i = 1; i <= 20; ++i) p.lineTo(x + w * (float) i / 20, mid - std::sin((float) i / 20 * 6.2831853f) * h * 0.5f);
            break;
        case 1: p.moveTo(x, y + h).lineTo(x + w * 0.5f, y).lineTo(x + w, y + h); break;
        case 2: p.moveTo(x, y).lineTo(x + w, y + h * 0.7f); p.moveTo(x, y).lineTo(x, y + h); break;
        case 3: p.moveTo(x, y + h).lineTo(x, y).lineTo(x + w * 0.5f, y).lineTo(x + w * 0.5f, y + h).lineTo(x + w, y + h).lineTo(x + w, y); break;
        default:
            p.moveTo(x, y + h);
            for (int i = 0; i < 4; ++i)
            {
                const float level[] = {0.4f, 0.8f, 0.2f, 0.6f};
                p.lineTo(x + w * 0.25f * (float) i, y + h * level[i]).lineTo(x + w * 0.25f * (float) (i + 1), y + h * level[i]);
            }
            break;
    }
    stroke(c, p, ink, width);
}

// A value on a dark display: optional grey label, then the value (drag vertically / wheel; double-click resets).
class DragValue : public Widget
{
public:
    struct Spec
    {
        std::function<float()> norm;                 // 0..1
        std::function<void(float)> setNorm;
        std::function<std::string()> text;
        std::function<void()> reset;
        std::string label;
        std::function<SkColor(const Theme&)> ink;
        std::function<float()> size;
        Align align = Align::Left;
        bool labelAbove = false; // label row above the value (the expanded view's value rows)
    };
    explicit DragValue(Spec spec) : spec_(std::move(spec)) {}
    void paint(SkCanvas& c, const Theme& t) override
    {
        const float size = spec_.size();
        const auto ink = spec_.ink ? spec_.ink(t) : t.color("RangeDefault");
        if (spec_.labelAbove)
        {
            paint::text(c, spec_.label, {0, 0, width(), height() * 0.5f}, t.color("RetroDisplayScaleText"), size, Align::Centre);
            paint::text(c, spec_.text(), {0, height() * 0.5f, width(), height() * 0.5f}, ink, size, Align::Centre);
            return;
        }
        float x = 0;
        if (! spec_.label.empty())
        {
            paint::text(c, spec_.label, localBounds(), t.color("RetroDisplayScaleText"), size, spec_.align);
            x = gfx::measureText(spec_.label, size) + size * 0.4f;
        }
        paint::text(c, spec_.text(), {x, 0, width() - x, height()}, ink, size, spec_.align);
    }
    void mouseDown(const MouseEvent& e) override { pressed_ = ! e.right; startY_ = e.y; start_ = spec_.norm(); }
    void mouseDrag(const MouseEvent& e) override
    {
        if (! pressed_ || ! spec_.setNorm) return;
        spec_.setNorm(std::clamp(start_ + (startY_ - e.y) / (e.shift ? 1000.0f : 200.0f), 0.0f, 1.0f));
        repaint();
    }
    void mouseUp(const MouseEvent&) override { pressed_ = false; }
    void mouseDoubleClick(const MouseEvent& e) override
    {
        if (e.right || ! spec_.reset) return;
        spec_.reset();
        repaint();
    }
    bool mouseWheel(const MouseEvent& e) override
    {
        if (! spec_.setNorm || std::abs(e.wheelDeltaX) > std::abs(e.wheelDeltaY)) return false;
        spec_.setNorm(std::clamp(spec_.norm() + (e.wheelDeltaY > 0 ? 1.0f : -1.0f) * (e.shift ? 0.002f : 0.02f), 0.0f, 1.0f));
        repaint();
        return true;
    }

private:
    Spec spec_;
    float startY_ = 0, start_ = 0;
    bool pressed_ = false;
};

// A text chooser on a dark display: amber (or grey) text and a down arrow; a click opens the choices.
class TextChooser final : public Widget
{
public:
    std::function<std::vector<std::string>()> labels;
    std::function<int()> current;
    std::function<void(int)> pick;
    std::function<std::string()> shown; // text when it differs from labels()[current()]
    std::function<SkColor(const Theme&)> ink;
    std::function<float()> size;
    std::function<void(SkCanvas&, Rect, SkColor)> glyph; // drawn instead of the text
    bool arrow = true;
    void paint(SkCanvas& c, const Theme& t) override
    {
        const auto colour = ink ? ink(t) : t.color("ChosenDefault");
        const float s = size();
        const auto r = localBounds();
        if (glyph) glyph(c, {r.x, r.y + r.h * 0.2f, r.w - s * 0.9f, r.h * 0.6f}, colour);
        else
        {
            std::string text = shown ? shown() : std::string();
            if (! shown)
            {
                const auto all = labels();
                const int i = current();
                if (i >= 0 && (size_t) i < all.size()) text = all[(size_t) i];
            }
            c.save();
            c.clipRect(SkRect::MakeXYWH(0, 0, std::max(0.0f, r.w - (arrow ? s * 0.8f : 0.0f)), r.h));
            paint::text(c, text, {0, 0, r.w, r.h}, colour, s, Align::Left);
            c.restore();
        }
        if (arrow) ctl::triangle(c, r.right() - s * 0.4f, r.centreY() + s * 0.05f, s * 0.55f, s * 0.35f, ctl::Dir::Down, colour);
    }
    void mouseUp(const MouseEvent& e) override
    {
        if (e.right || ! localBounds().contains(e.x, e.y)) return;
        choose(this, labels(), current(), [this](int i) { pick(i); repaint(); });
    }
    bool mouseWheel(const MouseEvent& e) override
    {
        if (std::abs(e.wheelDeltaX) > std::abs(e.wheelDeltaY)) return false;
        const int count = (int) labels().size();
        if (count > 0) pick(std::clamp(current() + (e.wheelDeltaY > 0 ? -1 : 1), 0, count - 1));
        repaint();
        return true;
    }
};

// A click target that draws itself through the face (slope boxes, LFO shape glyphs, table arrows).
class ClickArea final : public Widget
{
public:
    std::function<void(SkCanvas&, const Theme&, Rect)> draw;
    std::function<void()> onClick;
    void paint(SkCanvas& c, const Theme& t) override { if (draw) draw(c, t, localBounds()); }
    void mouseUp(const MouseEvent& e) override
    {
        if (! e.right && localBounds().contains(e.x, e.y) && onClick) { onClick(); if (parent()) parent()->repaint(); }
    }
};

// The reference's small square activators (filter on, oscillator tabs): amber filled when on, an amber frame when off.
class SquareToggle final : public Widget
{
public:
    std::function<bool()> isOn;
    std::function<void()> onClick;
    void paint(SkCanvas& c, const Theme& t) override
    {
        const auto r = localBounds();
        const float d = std::min(r.w, r.h);
        const Rect box{r.centreX() - d * 0.5f, r.centreY() - d * 0.5f, d, d};
        if (isOn()) paint::fill(c, box, t.color("ChosenDefault"));
        else paint::stroke(c, box.reduced(d * 0.06f, d * 0.06f), t.color("ChosenDefault"), std::max(1.0f, d * 0.12f));
    }
    void mouseUp(const MouseEvent& e) override
    {
        if (! e.right && localBounds().contains(e.x, e.y)) { onClick(); repaint(); }
    }
};

// Vertical amber slider (oscillator gain and position): a thin track with a filled part and a small handle.
class VerticalSlider final : public Widget
{
public:
    std::function<float()> norm;
    std::function<void(float)> setNorm;
    std::function<void()> reset;
    bool handle = false; // position: a square handle on a thin line; gain: a filled bar in a frame
    void paint(SkCanvas& c, const Theme& t) override
    {
        const auto r = localBounds();
        const float n = std::clamp(norm(), 0.0f, 1.0f), amber = 0;
        (void) amber;
        const auto ink = t.color("ChosenDefault");
        if (handle)
        {
            const float x = r.centreX(), y = r.bottom() - n * r.h;
            paint::line(c, x, r.y, x, r.bottom(), paint::withAlpha(ink, 150), std::max(1.0f, r.w * 0.16f));
            const float s = r.w * 0.7f;
            paint::fill(c, {x - s * 0.5f, y - s * 0.5f, s, s}, t.color("RetroDisplayBackground"));
            paint::stroke(c, {x - s * 0.5f, y - s * 0.5f, s, s}, ink, std::max(1.0f, r.w * 0.12f));
            return;
        }
        paint::stroke(c, r, paint::withAlpha(t.color("RetroDisplayScaleText"), 120), std::max(1.0f, r.w * 0.1f));
        const Rect inner = r.reduced(r.w * 0.22f, r.w * 0.22f);
        paint::fill(c, {inner.x, inner.bottom() - inner.h * n, inner.w, inner.h * n}, ink);
        ctl::triangle(c, r.right() + r.w * 0.9f, r.y + r.w * 0.6f, r.w * 0.9f, r.w * 0.9f, ctl::Dir::Left, t.color("RetroDisplayScaleText"));
    }
    void mouseDown(const MouseEvent& e) override { pressed_ = ! e.right; mouseDrag(e); }
    void mouseDrag(const MouseEvent& e) override
    {
        if (! pressed_) return;
        setNorm(std::clamp((height() - e.y) / std::max(1.0f, height()), 0.0f, 1.0f));
        repaint();
    }
    void mouseUp(const MouseEvent&) override { pressed_ = false; }
    void mouseDoubleClick(const MouseEvent& e) override { if (! e.right && reset) { reset(); repaint(); } }
    bool mouseWheel(const MouseEvent& e) override
    {
        if (std::abs(e.wheelDeltaX) > std::abs(e.wheelDeltaY)) return false;
        setNorm(std::clamp(norm() + (e.wheelDeltaY > 0 ? 0.02f : -0.02f), 0.0f, 1.0f));
        repaint();
        return true;
    }

private:
    bool pressed_ = false;
};

// Flat tab of the device title (Osc 1 / Osc 2, Matrix / MIDI / MPE): the selected one dark, the others lighter.
class TitleTab final : public Widget
{
public:
    std::string label;
    std::function<bool()> selected;
    std::function<void()> onSelect;
    std::function<bool()> activator; // Osc tabs: the oscillator's on switch drawn as a square
    std::function<void()> toggleActivator;
    void paint(SkCanvas& c, const Theme& t) override
    {
        const auto r = localBounds();
        const bool on = selected();
        paint::fillRounded(c, r, r.h * 0.12f, on ? t.color("SurfaceBackground") : paint::withAlpha(t.color("ControlContrastFrame"), 40));
        float x = r.h * 0.35f;
        if (activator)
        {
            const float d = r.h * 0.5f;
            const Rect box{x, r.centreY() - d * 0.5f, d, d};
            if (activator()) paint::fill(c, box, t.color("ChosenDefault"));
            else paint::stroke(c, box, t.color("ChosenDefault"), std::max(1.0f, d * 0.12f));
            x += d + r.h * 0.3f;
        }
        paint::text(c, label, {x, 0, r.w - x, r.h}, t.color("ControlForeground"), r.h * 0.68f, activator ? Align::Left : Align::Centre);
    }
    void mouseUp(const MouseEvent& e) override
    {
        if (e.right || ! localBounds().contains(e.x, e.y)) return;
        if (activator && e.x < height() * 1.05f) toggleActivator();
        else onSelect();
        if (parent()) parent()->repaint();
    }
};

// The expand switch: a circle with a down (open) or up (close) chevron.
class ExpandButton final : public Widget
{
public:
    std::function<bool()> expanded;
    std::function<void()> onClick;
    void paint(SkCanvas& c, const Theme& t) override
    {
        const auto r = localBounds();
        const float d = std::min(r.w, r.h), cx = r.centreX(), cy = r.centreY();
        SkPaint pen;
        pen.setAntiAlias(true);
        pen.setStyle(SkPaint::kStroke_Style);
        pen.setStrokeWidth(d * 0.09f);
        pen.setColor(expanded() ? t.color("ChosenDefault") : t.color("ControlForeground"));
        c.drawCircle(cx, cy, d * 0.45f, pen);
        ctl::triangle(c, cx, cy, d * 0.4f, d * 0.26f, expanded() ? ctl::Dir::Up : ctl::Dir::Down, pen.getColor());
    }
    void mouseUp(const MouseEvent& e) override { if (! e.right && localBounds().contains(e.x, e.y)) onClick(); }
};

class WavetableFace final : public ReferenceFace
{
public:
    WavetableFace(Widget& box, const model::Device& model, W* synth, Tag tagger)
        : ReferenceFace(synth, kMainWidth, 5, 320, std::move(tagger), 197), synth_(synth), modelId_(model.id),
          modelSource_(model.sourceXml)
    {
        buildTitle(box);
        buildSub();
        for (int osc = 0; osc < 2; ++osc) buildOscillator(osc);
        buildFilters();
        buildMatrix();
        buildGlobal();
        buildExpanded();
        animated_ = true; // the table display follows the playing position
        // A set saved with the envelopes / LFOs shown reopens with them (the reference's BreakoutIsExpanded).
        if (model.sourceXml && model.sourceXml->find("<BreakoutIsExpanded Value=\"true\"") != std::string::npos)
        {
            expanded_ = true;
            captureWidth_ = kExpandedWidth;
        }
    }

    void layout() override
    {
        ReferenceFace::layout();
        // Title widgets live in the device box: the band above the face (capture y 300..319).
        const float u = unit();
        for (auto& [widget, x0, x1] : titleWidgets_)
            widget->setBounds({bounds().x + (x0 - originX_) * u, 1.5f, (x1 - x0) * u, 15.0f}); // the box's 18-unit title band
    }

    void paint(SkCanvas& c, const Theme& t) override
    {
        fillBody(c, t, false);
        paintSub(c, t);
        paintOscillator(c, t);
        paintFilters(c, t);
        paintMatrix(c, t);
        paintGlobal(c, t);
        if (expanded_) paintExpanded(c, t);
    }

    void mouseDown(const MouseEvent& e) override
    {
        dragFilter_ = -1;
        if (e.right) return;
        // Filter handles: drag frequency across, resonance up.
        for (int f = 0; f < 2; ++f)
        {
            const auto h = handle(f);
            if (std::hypot(e.x - h.x, e.y - h.y) < 10 * unit()) dragFilter_ = f;
        }
        if (dragFilter_ < 0 && at(429, 343, 698, 413).contains(e.x, e.y)) dragFilter_ = synth_->getParameter(W::F1On) > 0.5f ? 0 : 1;
        if (dragFilter_ >= 0) mouseDrag(e);
        // Envelope display mode labels (Time / Slope / Value).
        if (expanded_)
            for (int env = 0; env < 3; ++env)
                for (int mode = 0; mode < (env == 0 ? 2 : 3); ++mode)
                    if (envModeBox(env, mode).contains(e.x, e.y)) { envMode_[env] = mode; layout(); repaint(); }
    }
    void mouseDrag(const MouseEvent& e) override
    {
        if (dragFilter_ < 0) return;
        const int base = dragFilter_ == 0 ? W::F1On : W::F2On;
        const float x = e.x / unit() + originX_, y = e.y / unit() + originY_;
        synth_->setParameter(base + 5, std::clamp(20.0f * std::pow(1000.0f, (x - kGraphX0) / (kGraphX1 - kGraphX0)), 20.0f, 20000.0f));
        synth_->setParameter(base + 6, std::clamp((kGraphY1 - 6 - y) / (kGraphY1 - kGraphY0 - 12) * 1.25f, 0.0f, 1.25f));
        repaint();
    }
    void mouseUp(const MouseEvent&) override { dragFilter_ = -1; }
    bool mouseWheel(const MouseEvent& e) override
    {
        if (! at(701, 346, 1068, 482).contains(e.x, e.y) || std::abs(e.wheelDeltaY) < std::abs(e.wheelDeltaX)) return false;
        matrixScroll_ = std::max(0, matrixScroll_ + (e.wheelDeltaY > 0 ? -1 : 1));
        layout();
        repaint();
        return true;
    }

private:
    // ---------------------------------------------------------------- geometry helpers
    static constexpr float kGraphX0 = 431, kGraphX1 = 696, kGraphY0 = 343, kGraphY1 = 413;
    static float ex(float x) { return kExpandX + (x - 6.0f) * kExpandScale; }
    static float ey(float y) { return 320.0f + (y - 6.0f) * kExpandScale; }
    float small() const { return text() * 0.92f; }

    template <typename T> T* placed(T* widget, float x0, float y0, float x1, float y1, std::function<bool()> when = {})
    {
        place(widget, x0, y0, x1, y1);
        if (when) showWhen(widget, std::move(when));
        return widget;
    }
    template <typename T> T* expandedWidget(T* widget, float x0, float y0, float x1, float y1, std::function<bool()> when = {})
    {
        return placed(widget, ex(x0), ey(y0), ex(x1), ey(y1), [this, when] { return expanded_ && (! when || when()); });
    }
    std::function<bool()> oscShown(int osc) { return [this, osc] { return oscTab_ == osc; }; }
    float value(int p) const { return synth_->getParameter(p); }
    void set(int p, float v) { synth_->setParameter(p, v); }

    DragValue* drag(int p, Law law, std::function<std::string(float)> format, std::string label = {}, bool amber = true,
                    Align align = Align::Left, bool above = false)
    {
        DragValue::Spec s;
        s.norm = [this, p, law] { return std::clamp(law.toNorm(value(p)), 0.0f, 1.0f); };
        s.setNorm = [this, p, law](float n) {
            const auto& info = synth_->parameters()[(size_t) p];
            set(p, std::clamp(law.fromNorm(n), info.minValue, info.maxValue));
        };
        s.text = [this, p, format] { return format(value(p)); };
        s.reset = [this, p] { set(p, synth_->parameters()[(size_t) p].defaultValue); };
        s.label = std::move(label);
        s.ink = [amber](const Theme& t) { return t.color(amber ? "ChosenDefault" : "RangeDefault"); };
        s.size = [this, tiny = buildingExpanded_] { return tiny ? small() * 0.78f : small(); };
        s.align = align;
        s.labelAbove = above;
        auto* widget = add<DragValue>(std::move(s));
        tag(widget, p);
        return widget;
    }
    TextChooser* paramChooser(int p, std::function<SkColor(const Theme&)> ink = {})
    {
        auto* chooserWidget = add<TextChooser>();
        chooserWidget->labels = [this, p] { return synth_->parameters()[(size_t) p].choices; };
        chooserWidget->current = [this, p] { return (int) std::lround(value(p)); };
        chooserWidget->pick = [this, p](int i) { set(p, (float) i); layout(); };
        chooserWidget->ink = std::move(ink);
        chooserWidget->size = [this, tiny = buildingExpanded_] { return tiny ? small() * 0.78f : small(); };
        tag(chooserWidget, p);
        return chooserWidget;
    }

    // ---------------------------------------------------------------- title
    void buildTitle(Widget& box)
    {
        auto* expand = box.add<ExpandButton>();
        expand->expanded = [this] { return expanded_; };
        expand->onClick = [this] {
            expanded_ = ! expanded_;
            captureWidth_ = expanded_ ? kExpandedWidth : kMainWidth;
            relayoutChain();
        };
        expand->tooltip = "Show the envelopes and LFOs";
        titleWidgets_.push_back({expand, 192, 209});
        for (int osc = 0; osc < 2; ++osc)
        {
            auto* tab = box.add<TitleTab>();
            tab->label = osc == 0 ? "Osc 1" : "Osc 2";
            tab->selected = [this, osc] { return oscTab_ == osc; };
            tab->onSelect = [this, osc] { oscTab_ = osc; layout(); repaint(); };
            const int on = osc == 0 ? W::Osc1On : W::Osc2On;
            tab->activator = [this, on] { return value(on) > 0.5f; };
            tab->toggleActivator = [this, on] { set(on, value(on) > 0.5f ? 0.0f : 1.0f); repaint(); };
            tag(tab, on); // MIDI mapping of the tab reaches the oscillator switch
            titleWidgets_.push_back({tab, osc == 0 ? 212.0f : 273.0f, osc == 0 ? 270.0f : 330.0f});
        }
        const char* names[] = {"Matrix", "MIDI", "MPE"};
        const float x[] = {803, 857, 901, 941};
        for (int i = 0; i < 3; ++i)
        {
            auto* tab = box.add<TitleTab>();
            tab->label = names[i];
            tab->selected = [this, i] { return matrixTab_ == i; };
            tab->onSelect = [this, i] { matrixTab_ = i; layout(); repaint(); };
            titleWidgets_.push_back({tab, x[i], x[i + 1] - 2});
        }
    }

    // ---------------------------------------------------------------- sub / transpose
    void buildSub()
    {
        toggle(W::SubOn, "Sub", 12, 329, 72, 343);
        for (auto [p, label, y] : {std::tuple{W::SubGain, "Gain", 347.0f}, std::tuple{W::SubTone, "Tone", 391.0f}})
        {
            auto* k = knob(p, label, 10, y, 76, y + 44, p == W::SubGain ? Law{[](float g) { return g <= 0 ? 0.0f : std::clamp(1 + dsp::gainToDb(g) / 70.0f, 0.0f, 1.0f); },
                                                                              [](float n) { return n <= 0.001f ? 0.0f : dsp::dbToGain((n - 1) * 70.0f); }}
                                                                        : Law::linear(0, 1),
                           p == W::SubGain ? decibels : percent);
            k->style = Knob::Style::Compact;
        }
        option(W::SubTranspose, 0, "0", 12, 452, 30, 466);
        option(W::SubTranspose, 1, "-1", 33, 452, 51, 466);
        option(W::SubTranspose, 2, "-2", 54, 452, 72, 466);
        slider(W::GlobalTranspose, 12, 495, 72, 509, Law::linear(-48, 48), [](float v) { return printed(v, "%.0f st"); });
    }
    void paintSub(SkCanvas& c, const Theme& t)
    {
        label(c, t, "Octave", 12, 436, 72, 450, Align::Left);
        paint::fill(c, at(8, 474, 76, 476), paint::withAlpha(t.color("ControlContrastFrame"), 60));
        label(c, t, "Transpose", 12, 480, 76, 494, Align::Left);
    }

    // ---------------------------------------------------------------- oscillators
    void buildOscillator(int osc)
    {
        const int b = osc == 0 ? W::Osc1On : W::Osc2On;
        const int mode = osc == 0 ? W::Osc1EffectMode : W::Osc2EffectMode;
        const auto shown = oscShown(osc);
        // Category and table choosers (factory tables of the installed reference library).
        auto* category = add<TextChooser>();
        category->size = [this] { return small(); };
        category->labels = [] {
            std::vector<std::string> out;
            for (const auto& s : devices::factoryWavetables())
                if (out.empty() || out.back() != s.category) out.push_back(s.category);
            return out;
        };
        category->current = [this, osc, category] {
            const auto all = category->labels();
            const auto it = std::find(all.begin(), all.end(), synth_->tableCategory(osc));
            return it == all.end() ? -1 : (int) (it - all.begin());
        };
        category->shown = [this, osc] { return synth_->tableCategory(osc).empty() ? std::string("User") : synth_->tableCategory(osc); };
        category->pick = [this, osc, category](int i) {
            const auto all = category->labels();
            if (i < 0 || (size_t) i >= all.size()) return;
            for (const auto& s : devices::factoryWavetables())
                if (s.category == all[(size_t) i]) { chooseTable(osc, s.name); break; }
        };
        placed(category, 84, 326, 160, 341, shown);
        auto* table = add<TextChooser>();
        table->size = [this] { return small(); };
        table->labels = [this, osc] {
            std::vector<std::string> out;
            for (const auto& s : devices::factoryWavetables())
                if (s.category == synth_->tableCategory(osc)) out.push_back(s.name);
            return out;
        };
        table->current = [this, osc, table] {
            const auto all = table->labels();
            const auto it = std::find(all.begin(), all.end(), synth_->tableName(osc));
            return it == all.end() ? -1 : (int) (it - all.begin());
        };
        table->shown = [this, osc] { return synth_->tableName(osc); };
        table->pick = [this, osc, table](int i) {
            const auto all = table->labels();
            if (i >= 0 && (size_t) i < all.size()) chooseTable(osc, all[(size_t) i]);
        };
        placed(table, 170, 326, 290, 341, shown);
        for (int step : {-1, 1})
        {
            auto* arrow = add<ClickArea>();
            arrow->draw = [step](SkCanvas& c, const Theme& t, Rect r) {
                ctl::triangle(c, r.centreX(), r.centreY(), r.h * 0.4f, r.h * 0.5f, step < 0 ? ctl::Dir::Left : ctl::Dir::Right, t.color("RetroDisplayScaleText"));
            };
            arrow->onClick = [this, osc, step, table] {
                const auto all = table->labels();
                const int next = table->current() + step;
                if (next >= 0 && (size_t) next < all.size()) chooseTable(osc, all[(size_t) next]);
            };
            arrow->tooltip = step < 0 ? "Previous wavetable" : "Next wavetable";
            placed(arrow, step < 0 ? 356.0f : 368.0f, 327.0f, step < 0 ? 366.0f : 378.0f, 340.0f, shown);
        }
        // Pan above the gain bar, gain bar and its dB value, position bar and value.
        placed(drag(b + 6, Law::linear(-1, 1), panText, {}, true, Align::Centre), 98, 345, 128, 358, shown);
        auto* gain = add<VerticalSlider>();
        gain->norm = [this, b] { return value(b + 7); };
        gain->setNorm = [this, b](float n) { set(b + 7, n); };
        gain->reset = [this, b] { set(b + 7, 1); };
        tag(gain, b + 7);
        placed(gain, 109, 368, 119, 470, shown);
        placed(drag(b + 7, Law::linear(0, 1), decibels, {}, true, Align::Centre), 90, 480, 140, 494, shown);
        auto* position = add<VerticalSlider>();
        position->handle = true;
        position->norm = [this, b] { return value(b + 3); };
        position->setNorm = [this, b](float n) { set(b + 3, n); };
        position->reset = [this, b] { set(b + 3, 0); };
        tag(position, b + 3);
        placed(position, 385, 345, 399, 470, shown);
        placed(drag(b + 3, Law::linear(0, 1), [](float v) { return printed(v * 100, "%.0f %%"); }, {}, true, Align::Centre), 372, 480, 418, 494, shown);
        // Effect mode, FX 1 / FX 2 (named by mode), Semi and Det.
        placed(paramChooser(mode), 84, 498, 140, 512, shown);
        for (int fx = 0; fx < 2; ++fx)
        {
            const int p = b + 4 + fx;
            auto* value = drag(p, fx == 0 ? Law::linear(-1, 1) : Law::linear(0, 1), percent);
            placed(value, fx == 0 ? 178.0f : 251.0f, 498.0f, fx == 0 ? 216.0f : 289.0f, 512.0f,
                   [this, osc, mode] { return oscTab_ == osc && std::lround(this->value(mode)) != 0; });
        }
        placed(drag(b + 1, Law::linear(-48, 48), [](float v) { return printed(v, "%.0f st"); }, "Semi"), 293, 498, 352, 512, shown);
        placed(drag(b + 2, Law::linear(-1, 1), [](float v) { return printed(v * 100, "%.0f ct"); }, "Det"), 360, 498, 420, 512, shown);
    }
    void chooseTable(int osc, const std::string& name)
    {
        // A new table is a model edit (saved as SpriteName, undoable); the graph rebuild loads it.
        if (! ctx().editDevice || ctx().song == nullptr) return;
        int trackIndex = -1, deviceIndex = -1, index = 0;
        const auto visit = [&](const model::Track& track) {
            for (size_t d = 0; d < track.devices.size(); ++d)
                if (trackIndex < 0 && track.devices[d].type == "InstrumentVector" && track.devices[d].id == modelId_
                    && track.devices[d].sourceXml == modelSource_)
                {
                    trackIndex = index;
                    deviceIndex = (int) d;
                }
            ++index;
        };
        for (const auto& track : ctx().song->tracks) visit(track);
        for (const auto& track : ctx().song->returns) visit(track);
        visit(ctx().song->main);
        if (trackIndex < 0) return; // inside a rack: not addressed yet (DEV-28b3)
        ctx().editDevice(trackIndex, deviceIndex, [osc, name](model::Device& d) { devices::setWavetableSpriteName(d, osc, name); },
                         "Choose Wavetable");
    }
    void paintOscillator(SkCanvas& c, const Theme& t)
    {
        displayPanel(c, t, 78, 321, 428, 517);
        const int osc = oscTab_;
        const int b = osc == 0 ? W::Osc1On : W::Osc2On;
        const auto scale = t.color("RetroDisplayScaleText");
        paint::fill(c, at(165, 328, 166, 339), paint::withAlpha(scale, 120));
        paint::fill(c, at(289, 499, 290, 511), paint::withAlpha(scale, 120));
        // Menu (list) and circle icons at the header's right (drawn; table import is DEV-28b3).
        for (int i = 0; i < 4; ++i) paint::fill(c, at(392, 328.5f + 3.2f * (float) i, 404, 329.7f + 3.2f * (float) i), scale);
        SkPaint ring;
        ring.setAntiAlias(true);
        ring.setStyle(SkPaint::kStroke_Style);
        ring.setStrokeWidth(1.2f * unit());
        ring.setColor(scale);
        const auto circle = at(409, 328, 420, 339);
        c.drawCircle(circle.centreX(), circle.centreY(), circle.w * 0.5f, ring);
        // FX labels (greyed values when the mode is None).
        const int mode = (int) std::lround(value(osc == 0 ? W::Osc1EffectMode : W::Osc2EffectMode));
        static const char* fxNames[4][2] = {{"FX 1", "FX 2"}, {"Tune", "Amt"}, {"PW", "Sync"}, {"Warp", "Fold"}};
        const auto dim = paint::withAlpha(scale, 200);
        for (int fx = 0; fx < 2; ++fx)
        {
            const float x = fx == 0 ? 146.0f : 219.0f;
            paint::text(c, fxNames[mode][fx], at(x, 498, x + 34, 512), dim, small(), Align::Left);
            if (mode == 0) paint::text(c, percent(value(b + 4 + fx)), at(x + 32, 498, x + 72, 512), dim, small(), Align::Left);
        }
        paintTable(c, t, osc);
    }
    void paintTable(SkCanvas& c, const Theme& t, int osc)
    {
        // The table as a stack of frames, front (first frame) at the bottom; the current position in amber.
        const auto table = synth_->table(osc);
        if (! table) return;
        const int b = osc == 0 ? W::Osc1On : W::Osc2On;
        const float live = synth_->displayPosition(osc);
        const float position = live >= 0 ? live : value(b + 3);
        const int frames = table->frames();
        const int lines = std::min(frames, 20);
        const Rect area = at(150, 352, 362, 486);
        const float amplitude = area.h * 0.1f, depth = area.h - 2 * amplitude;
        const auto curve = [&](float pos, float baseline, float shift, SkColor ink, float width) {
            SkPathBuilder p;
            const int points = 96;
            for (int i = 0; i <= points; ++i)
            {
                const float x = area.x + shift + (area.w - shift * 1.6f) * (float) i / points;
                const float y = baseline - amplitude * table->sample(pos, (double) i / points, 0);
                if (i == 0) p.moveTo(x, y); else p.lineTo(x, y);
            }
            stroke(c, p, ink, width);
        };
        const float u = unit();
        for (int line = lines - 1; line >= 0; --line)
        {
            const float pos = lines > 1 ? (float) line / (float) (lines - 1) : 0.0f;
            const float k = lines > 1 ? (float) line / (float) (lines - 1) : 0.0f;
            curve(pos, area.bottom() - amplitude - depth * k, area.w * 0.04f * k, paint::withAlpha(t.color("RetroDisplayScaleText"), 150), 0.9f * u);
        }
        curve(position, area.bottom() - amplitude - depth * position, area.w * 0.04f * position, t.color("ChosenDefault"), 2.0f * u);
    }

    // ---------------------------------------------------------------- filters
    void buildFilters()
    {
        for (int f = 0; f < 2; ++f)
        {
            const int b = f == 0 ? W::F1On : W::F2On;
            const float x = f == 0 ? 433.0f : 572.0f;
            auto* on = add<SquareToggle>();
            on->isOn = [this, b] { return value(b) > 0.5f; };
            on->onClick = [this, b] { set(b, value(b) > 0.5f ? 0.0f : 1.0f); };
            tag(on, b);
            placed(on, x, 327, x + 13, 340);
            placed(paramChooser(b + 1, [](const Theme& t) { return t.color("RetroDisplayScaleText"); }), x + 18, 326, x + 50, 341)->glyph =
                [this, b](SkCanvas& c, Rect r, SkColor ink) { filterGlyph(c, r, (int) std::lround(value(b + 1)), ink, 1.6f * unit()); };
            auto* slope = add<ClickArea>();
            slope->onClick = [this, b] { set(b + 4, value(b + 4) > 0.5f ? 0.0f : 1.0f); };
            slope->draw = [this, b](SkCanvas& c, const Theme& t, Rect r) {
                // "12" / "24": cyan while the filter is on, light while it is off (the reference's header boxes).
                paint::fill(c, r, value(b) > 0.5f ? t.color("RangeDefault") : t.color("ControlTextBack"));
                paint::text(c, value(b + 4) > 0.5f ? "24" : "12", r, t.color("RetroDisplayBackground"), small(), Align::Centre);
            };
            tag(slope, b + 4);
            placed(slope, x + 54, 326, x + 73, 341);
            slopes_[(size_t) f] = slope;
            const auto lphp = [this, b] { return std::lround(value(b + 1)) <= 1; };
            const auto ink = [this, b](const Theme& t) { return value(b) > 0.5f ? t.color("ChosenDefault") : t.color("RetroDisplayScaleText"); };
            placed(paramChooser(b + 2, ink), x + 79, 326, x + 122, 341, lphp);
            placed(paramChooser(b + 3, ink), x + 79, 326, x + 122, 341, [lphp] { return ! lphp(); });
        }
        placed(paramChooser(W::FilterRouting, [](const Theme& t) { return t.color("RetroDisplayScaleText"); }), 460, 417, 520, 431);
        // Filter 1: Res, Drive (circuits other than Clean) / Morph, Frequency. Filter 2 mirrors it.
        for (int f = 0; f < 2; ++f)
        {
            const int b = f == 0 ? W::F1On : W::F2On;
            const float resX = f == 0 ? 436.0f : 640.0f, freqX = f == 0 ? 489.0f : 573.0f;
            auto* res = knob(b + 6, "Res", resX, 438, resX + 52, 482, Law::linear(0, 1.25f), [](float v) { return printed(v * 100, v < 0.0995f ? "%.1f %%" : "%.0f %%"); });
            res->style = Knob::Style::Compact;
            auto* freq = knob(b + 5, "Frequency", freqX, 437, freqX + 64, 514, Law::logarithmic(20, 20000));
            freq->deviceStroke = 2.45f;
            const auto drive = [this, b] {
                const int type = (int) std::lround(value(b + 1));
                return type <= 1 ? std::lround(value(b + 2)) != 0 : value(b + 3) > 0.5f;
            };
            const float dx = f == 0 ? 438.0f : 638.0f;
            placed(slider(b + 7, dx, 496, dx + 49, 510, Law::linear(0, 24), [](float v) { return printed(v, "%.2f dB"); }), dx, 496, dx + 49, 510,
                   [this, b, drive] { return drive() && std::lround(value(b + 1)) != 4; });
            placed(slider(b + 8, dx, 496, dx + 49, 510, Law::linear(0, 1), percent), dx, 496, dx + 49, 510,
                   [this, b] { return std::lround(value(b + 1)) == 4; });
            driveShown_[(size_t) f] = drive;
        }
    }
    Rect handlePoint(int f) const
    {
        const int b = f == 0 ? W::F1On : W::F2On;
        const float hz = std::clamp(value(b + 5), 20.0f, 20000.0f);
        const float x = kGraphX0 + (kGraphX1 - kGraphX0) * std::log(hz / 20.0f) / std::log(1000.0f);
        const float y = kGraphY1 - 6 - (kGraphY1 - kGraphY0 - 12) * value(b + 6) / 1.25f;
        return at(x, y, x, y);
    }
    Rect handle(int f) const { return handlePoint(f); }
    float responseDb(double hz) const
    {
        double db = 0;
        const int routing = (int) std::lround(value(W::FilterRouting));
        double parallel = 0;
        int active = 0;
        for (int f = 0; f < 2; ++f)
        {
            const int b = f == 0 ? W::F1On : W::F2On;
            if (value(b) < 0.5f) continue;
            devices::BiquadCoeffs stages[2];
            const int n = synth_->filterStages(f, value(b + 5), value(b + 6), value(b + 8), stages);
            double g = 1;
            for (int s = 0; s < n; ++s) g *= stages[s].magnitudeAt(hz, 44100);
            if (routing == 0) db += 20 * std::log10(std::max(1e-9, g));
            else parallel += g;
            ++active;
        }
        if (routing != 0 && active > 0) db = 20 * std::log10(std::max(1e-9, parallel / (routing == 2 ? active : 1)));
        return (float) db;
    }
    void paintFilters(SkCanvas& c, const Theme& t)
    {
        displayPanel(c, t, 429, 321, 698, 432);
        const auto scale = t.color("RetroDisplayScaleText");
        paint::fill(c, at(565, 328, 566, 339), paint::withAlpha(scale, 120));
        paint::fill(c, at(429, 413, 698, 414), paint::withAlpha(scale, 90));
        // Routing glyph: two linked boxes.
        const auto glyph = at(437, 420, 453, 428);
        paint::stroke(c, {glyph.x + glyph.w * 0.25f, glyph.y, glyph.w * 0.22f, glyph.h}, scale, unit());
        paint::stroke(c, {glyph.x + glyph.w * 0.55f, glyph.y, glyph.w * 0.22f, glyph.h}, scale, unit());
        paint::line(c, glyph.x, glyph.centreY(), glyph.right(), glyph.centreY(), scale, unit());
        // Response of both filters as routed, and the two handles.
        const auto area = at(429, kGraphY0, 698, kGraphY1);
        c.save();
        c.clipRect(area.sk());
        SkPathBuilder curve;
        const float zero = at(0, 375, 0, 375).y, perDb = 1.4f * unit();
        for (int i = 0; i <= (int) area.w; ++i)
        {
            const float x = area.x + (float) i;
            const float cx = x / unit() + originX_;
            const double hz = 20.0 * std::pow(1000.0, (cx - kGraphX0) / (kGraphX1 - kGraphX0));
            const float y = zero - perDb * std::clamp(responseDb(hz), -60.0f, 30.0f);
            if (i == 0) curve.moveTo(x, y); else curve.lineTo(x, y);
        }
        stroke(c, curve, t.color("RangeDefault"), 2.0f * unit());
        c.restore();
        for (int f = 0; f < 2; ++f)
        {
            const int b = f == 0 ? W::F1On : W::F2On;
            const auto h = handle(f);
            const bool on = value(b) > 0.5f;
            SkPaint ring;
            ring.setAntiAlias(true);
            ring.setStyle(SkPaint::kStroke_Style);
            ring.setStrokeWidth(1.6f * unit());
            ring.setColor(on ? t.color("ChosenDefault") : scale);
            c.drawCircle(h.x, h.y, 7.5f * unit(), ring);
            paint::text(c, f == 0 ? "1" : "2", {h.x - 6 * unit(), h.y - 7 * unit(), 12 * unit(), 14 * unit()}, ring.getColor(), small() * 0.9f, Align::Centre);
        }
        paint::fill(c, at(563, 436, 564, 512), paint::withAlpha(t.color("ControlContrastFrame"), 120));
        for (int f = 0; f < 2; ++f)
            if (driveShown_[(size_t) f]() || std::lround(value(f == 0 ? W::F1Type : W::F2Type)) == 4)
                label(c, t, std::lround(value(f == 0 ? W::F1Type : W::F2Type)) == 4 ? "Morph" : "Drive", f == 0 ? 438.0f : 638.0f, 483, f == 0 ? 488.0f : 690.0f, 495, Align::Left);
    }

    // ---------------------------------------------------------------- matrix
    static constexpr float kColumns[6] = {797, 845, 894, 943, 992, 1040};
    static constexpr float kRowY = 348, kRowStep = 22, kRowHeight = 20;
    static constexpr int kVisibleRows = 6;
    std::vector<int> columnSources() const
    {
        if (matrixTab_ == 1) return {W::SrcVelocity, W::SrcNote, W::SrcPitchBend, W::SrcPressure, W::SrcModWheel, W::SrcRandom};
        if (matrixTab_ == 2) return {W::SrcNotePb, W::SrcSlide, W::SrcPressure};
        return {W::SrcAmp, W::SrcEnv2, W::SrcEnv3, W::SrcLfo1, W::SrcLfo2};
    }
    std::vector<int> rows() const
    {
        std::vector<int> out;
        for (int target = 0; target < W::kTargets; ++target)
        {
            bool used = std::find(addedRows_.begin(), addedRows_.end(), target) != addedRows_.end();
            for (int s = 0; s < W::kSources && ! used; ++s) used = std::abs(value(W::matrixParam(target, s))) > 1e-6f;
            if (used) out.push_back(target);
        }
        return out;
    }
    float columnX(int i, int count) const
    {
        // MIDI has six columns over the same span; MPE three.
        const float span = kColumns[5] - kColumns[0];
        return kColumns[0] + span * (float) i / (float) std::max(5, count);
    }
    void buildMatrix()
    {
        for (int row = 0; row < kVisibleRows; ++row)
        {
            auto* name = add<TextChooser>();
            name->size = [this] { return small(); };
            name->ink = [](const Theme& t) { return t.color("RetroDisplayScaleText"); };
            name->labels = [this] {
                std::vector<std::string> out;
                for (int target = 0; target < W::kTargets; ++target)
                    out.push_back(W::targetName(target, (int) std::lround(value(W::Osc1EffectMode)), (int) std::lround(value(W::Osc2EffectMode))));
                return out;
            };
            name->current = [this, row] { const auto r = rows(); return row + matrixScroll_ < (int) r.size() ? r[(size_t) (row + matrixScroll_)] : -1; };
            name->shown = [name] { const int i = name->current(); return i < 0 ? std::string() : name->labels()[(size_t) i]; };
            name->pick = [this](int target) {
                if (std::find(addedRows_.begin(), addedRows_.end(), target) == addedRows_.end()) addedRows_.push_back(target);
                layout();
            };
            name->tooltip = "Modulation target (choose one to add a row)";
            name->arrow = false;
            placed(name, 707, kRowY + kRowStep * (float) row, 790, kRowY + kRowStep * (float) row + kRowHeight);
            rowLabels_[(size_t) row] = name;
            for (int column = 0; column < 6; ++column)
            {
                DragValue::Spec s;
                const auto parameter = [this, row, column]() -> int {
                    const auto r = rows();
                    const auto sources = columnSources();
                    if (row + matrixScroll_ >= (int) r.size() || column >= (int) sources.size()) return -1;
                    return W::matrixParam(r[(size_t) (row + matrixScroll_)], sources[(size_t) column]);
                };
                s.norm = [this, parameter] { const int p = parameter(); return p < 0 ? 0.5f : (value(p) + 1) * 0.5f; };
                s.setNorm = [this, parameter](float n) { if (const int p = parameter(); p >= 0) set(p, std::round((n * 2 - 1) * 1000) / 1000); };
                s.reset = [this, parameter] { if (const int p = parameter(); p >= 0) set(p, 0); };
                s.text = [] { return std::string(); };
                s.size = [this] { return small(); };
                auto* cell = add<DragValue>(std::move(s));
                cell->tooltip = "Drag to set the modulation amount, double-click to clear it";
                placed(cell, 0, 0, 1, 1); // laid out by layoutMatrix()
                cells_[(size_t) row][(size_t) column] = {cell, parameter};
            }
        }
        placed(drag(W::TimeScale, Law::linear(-1, 1), percent, "Time", false), 873, 496, 945, 510)->tooltip = "Global time";
        auto* amount = drag(W::ModAmount, Law::linear(0, 2), percent, "Amount", false);
        placed(amount, 960, 496, 1052, 510);
    }
    void layoutMatrix()
    {
        const auto count = (int) columnSources().size();
        for (int row = 0; row < kVisibleRows; ++row)
            for (int column = 0; column < 6; ++column)
            {
                auto& cell = cells_[(size_t) row][(size_t) column];
                const float x0 = columnX(column, count), x1 = columnX(column + 1, count) - 2;
                cell.widget->setBounds(at(x0, kRowY + kRowStep * (float) row, x1, kRowY + kRowStep * (float) row + kRowHeight));
                cell.widget->visible = column < count && cell.parameter() >= 0;
            }
    }
    void paintMatrix(SkCanvas& c, const Theme& t)
    {
        layoutMatrix();
        displayPanel(c, t, 701, 321, 1068, 517);
        const auto scale = t.color("RetroDisplayScaleText");
        paint::text(c, "Target", at(710, 330, 760, 343), scale, small(), Align::Left);
        paint::text(c, "Global", at(711, 496, 760, 510), scale, small(), Align::Left);
        const auto sources = columnSources();
        const int count = (int) sources.size();
        const auto colour = [&t](int source) { return source == W::SrcLfo1 || source == W::SrcLfo2 ? t.color("ChosenDefault") : t.color("RangeDefault"); };
        const auto empty = paint::mix(t.color("RetroDisplayBackground"), scale, 0.22f);
        const auto r = rows();
        for (int i = 0; i < count; ++i)
        {
            const float x0 = columnX(i, count), x1 = columnX(i + 1, count) - 2;
            paint::text(c, W::sourceName(sources[(size_t) i]), at(x0, 330, x1, 343), colour(sources[(size_t) i]), small(), Align::Centre);
            paint::fill(c, at(x0, 344, x1, 345.2f), paint::withAlpha(colour(sources[(size_t) i]), 200));
            for (int row = 0; row < kVisibleRows; ++row)
            {
                const auto cell = at(x0, kRowY + kRowStep * (float) row, x1, kRowY + kRowStep * (float) row + kRowHeight);
                const int target = row + matrixScroll_ < (int) r.size() ? r[(size_t) (row + matrixScroll_)] : -1;
                const float amount = target >= 0 ? value(W::matrixParam(target, sources[(size_t) i])) : 0.0f;
                if (std::abs(amount) < 1e-6f) { paint::fill(c, cell, empty); continue; }
                paint::fill(c, cell, t.color("RetroDisplayBackground"));
                const auto ink = colour(sources[(size_t) i]);
                paint::text(c, amountText(amount), {cell.x, cell.y, cell.w, cell.h * 0.8f}, ink, small(), Align::Centre);
                paint::fill(c, {cell.x, cell.bottom() - 1.5f * unit(), cell.w, 1.2f * unit()}, paint::withAlpha(ink, 70));
                const float half = cell.w * 0.5f, w = std::max(2.5f * unit(), half * std::abs(amount));
                paint::fill(c, {amount >= 0 ? cell.centreX() - 1.2f * unit() : cell.centreX() - w + 1.2f * unit(), cell.bottom() - 2.2f * unit(), w, 2.2f * unit()}, ink);
            }
        }
        // Row label boxes behind the names (the reference fills them to the text width).
        for (int row = 0; row < kVisibleRows; ++row)
        {
            const int target = row + matrixScroll_ < (int) r.size() ? r[(size_t) (row + matrixScroll_)] : -1;
            if (target < 0) continue;
            const auto name = W::targetName(target, (int) std::lround(value(W::Osc1EffectMode)), (int) std::lround(value(W::Osc2EffectMode)));
            const float w = gfx::measureText(name, small()) / unit() + 8;
            paint::fill(c, at(705, kRowY + kRowStep * (float) row, std::min(790.0f, 705 + w), kRowY + kRowStep * (float) row + kRowHeight), empty);
        }
    }

    // ---------------------------------------------------------------- global column
    void buildGlobal()
    {
        slider(W::Volume, 1075, 338, 1141, 353,
               {[](float g) { return g <= 0.0000317f ? 0.0f : std::clamp((dsp::gainToDb(g) + 70.0f) / 76.0f, 0.0f, 1.0f); },
                [](float n) { return n <= 0.002f ? 0.0f : dsp::dbToGain(-70.0f + 76.0f * n); }},
               decibels);
        toggle(W::MonoPoly, "Poly", 1075, 362, 1109, 376);
        chooser(W::PolyVoices, 1112, 362, 1141, 376);
        slider(W::Glide, 1075, 395, 1141, 409, Law::power(0, 5, 0.3f), seconds);
        chooser(W::UnisonMode, 1075, 431, 1141, 445);
        slider(W::UnisonVoices, 1075, 465, 1141, 479, Law::linear(2, 8), [](float v) { return printed(std::round(v), "%.0f"); });
        slider(W::UnisonAmount, 1075, 499, 1141, 513, Law::linear(0, 1), [](float v) { return printed(v * 100, "%.0f %%"); });
    }
    void paintGlobal(SkCanvas& c, const Theme& t)
    {
        label(c, t, "Volume", 1075, 325, 1141, 337, Align::Left);
        paint::fill(c, at(1074, 357.5f, 1143, 358.7f), paint::withAlpha(t.color("ControlContrastFrame"), 90));
        label(c, t, "Glide", 1075, 382, 1141, 394, Align::Left);
        paint::fill(c, at(1074, 413.5f, 1143, 414.7f), paint::withAlpha(t.color("ControlContrastFrame"), 90));
        label(c, t, "Unison", 1075, 419, 1141, 431, Align::Left);
        label(c, t, "Voices", 1075, 452, 1141, 464, Align::Left);
        label(c, t, "Amount", 1075, 486, 1141, 498, Align::Left);
    }

    // ---------------------------------------------------------------- expanded envelopes / LFOs
    static constexpr float kEnvX[3] = {6, 335, 665}, kEnvW = 327, kLfoX[2] = {997, 1494}, kLfoW = 495;
    Rect envModeBox(int env, int mode) const
    {
        const float x = kEnvX[env] + kEnvW * 0.5f + (env == 0 ? -46.0f : -69.0f) + 46.0f * (float) mode;
        return at(ex(x), ey(233), ex(x + 46), ey(250));
    }
    void buildExpanded()
    {
        buildingExpanded_ = true; // smaller text: the expanded view is drawn at 0.69 of its capture size
        for (int env = 0; env < 3; ++env)
        {
            const float x = kEnvX[env];
            const int loop = env == 0 ? W::AmpLoop : env == 1 ? W::E2Loop : W::E3Loop;
            expandedWidget(paramChooser(loop, [](const Theme& t) { return t.color("RangeDefault"); }), x + kEnvW - 58, 12, x + kEnvW - 8, 26);
            // Value row: four columns, the content follows the display mode (Time / Slope / Value).
            for (int column = 0; column < 4; ++column)
            {
                const float cx = x + 64 + 52.5f * (float) column;
                for (int mode = 0; mode < 3; ++mode)
                {
                    const int p = envValueParam(env, mode, column);
                    if (p < 0) continue;
                    const char* names[3][4] = {{"A", "D", "S", "R"}, {"A", "D", "", "R"}, {"Initial", "Peak", "Sustain", "Final"}};
                    const bool time = mode == 0 && column != 2;
                    const bool sustainDb = env == 0 && mode == 0 && column == 2;
                    auto* widget = drag(p, time ? Law::power(0, 20, 0.25f) : mode == 1 ? Law::linear(-1, 1) : sustainDb ? Law::linear(0, 1) : Law::linear(-1, 1),
                                        time ? seconds : sustainDb ? decibels : mode == 1 ? percent : percent, names[mode][column], false, Align::Centre, true);
                    if (env > 0 && mode == 0 && column == 2) // Env 2 / 3 sustain is a value
                        widget = drag(p, Law::linear(-1, 1), percent, "S", false, Align::Centre, true);
                    expandedWidget(widget, cx - 26, 256, cx + 26, 288, [this, env, mode] { return envMode_[env] == mode; });
                }
            }
        }
        for (int l = 0; l < 2; ++l)
        {
            const float x = kLfoX[l];
            const int b = l == 0 ? W::L1Retrigger : W::L2Retrigger;
            const auto amber = true;
            expandedWidget(drag(b + 8, Law::power(0, 20, 0.25f), seconds, "A", amber), x + 330, 12, x + 414, 26);
            auto* sync = add<ClickArea>();
            sync->onClick = [this, b] { set(b + 5, value(b + 5) > 0.5f ? 0.0f : 1.0f); layout(); };
            sync->draw = [this, b](SkCanvas& c, const Theme& t, Rect r) {
                paint::text(c, value(b + 5) > 0.5f ? "Sync" : "Hz", r, t.color("ChosenDefault"), small() * 0.78f, Align::Centre);
            };
            tag(sync, b + 5);
            expandedWidget(sync, x + 420, 12, x + 462, 26);
            syncButtons_[(size_t) l] = sync;
            auto* retrigger = add<SquareToggle>();
            retrigger->isOn = [this, b] { return value(b) > 0.5f; };
            retrigger->onClick = [this, b] { set(b, value(b) > 0.5f ? 0.0f : 1.0f); };
            retrigger->tooltip = "Retrigger";
            tag(retrigger, b);
            expandedWidget(retrigger, x + 466, 12, x + 481, 26);
            for (int shape = 0; shape < 5; ++shape)
            {
                auto* button = add<ClickArea>();
                button->onClick = [this, b, shape] { set(b + 1, (float) shape); };
                button->draw = [this, b, shape](SkCanvas& c, const Theme& t, Rect r) {
                    const float u = unit() * kExpandScale;
                    lfoGlyph(c, r.reduced(2 * u, 3 * u), shape, std::lround(value(b + 1)) == shape ? t.color("ChosenDefault") : t.color("RetroDisplayScaleText"), 1.6f * u);
                };
                button->tooltip = synth_->parameters()[(size_t) (b + 1)].choices[(size_t) shape];
                tag(button, b + 1);
                expandedWidget(button, x + 177 + 29.0f * (float) shape, 235, x + 199 + 29.0f * (float) shape, 252);
                shapeButtons_[(size_t) l][(size_t) shape] = button;
            }
            const float cols[4] = {x + 154, x + 216, x + 264, x + 319};
            expandedWidget(drag(b + 6, Law::logarithmic(0.01f, 40), [](float v) { return ReferenceFace::significant(v) + " Hz"; }, "Rate", true, Align::Centre, true),
                           cols[0] - 28, 256, cols[0] + 28, 288, [this, b] { return value(b + 5) < 0.5f; });
            expandedWidget(drag(b + 7, Law::linear(0, W::kSyncedRates - 1), [](float v) { return std::string(W::syncedRateName((int) std::lround(v))); }, "Rate", true,
                                Align::Centre, true),
                           cols[0] - 28, 256, cols[0] + 28, 288, [this, b] { return value(b + 5) >= 0.5f; });
            expandedWidget(drag(b + 2, Law::linear(0, 1), [](float v) { return printed(v * 100, "%.0f %%"); }, "Amount", true, Align::Centre, true), cols[1] - 28, 256,
                           cols[1] + 28, 288);
            expandedWidget(drag(b + 3, Law::linear(-1, 1), percent, "Shape", true, Align::Centre, true), cols[2] - 24, 256, cols[2] + 24, 288);
            expandedWidget(drag(b + 4, Law::linear(0, 360), [](float v) { return printed(v, "%.1f") + "\xC2\xB0"; }, "Offset", true, Align::Centre, true),
                           cols[3] - 26, 256, cols[3] + 26, 288);
        }
        buildingExpanded_ = false;
    }
    static int envValueParam(int env, int mode, int column)
    {
        if (env == 0)
        {
            if (mode == 0) return column == 0 ? W::AmpAttack : column == 1 ? W::AmpDecay : column == 2 ? W::AmpSustain : W::AmpRelease;
            if (mode == 1) return column == 0 ? W::AmpSlopeAttack : column == 1 ? W::AmpSlopeDecay : column == 3 ? W::AmpSlopeRelease : -1;
            return -1;
        }
        const int b = env == 1 ? W::E2Attack : W::E3Attack;
        if (mode == 0) return column == 0 ? b : column == 1 ? b + 1 : column == 2 ? b + 8 : b + 2;
        if (mode == 1) return column == 0 ? b + 3 : column == 1 ? b + 4 : column == 3 ? b + 5 : -1;
        return b + 6 + column;
    }
    void paintExpanded(SkCanvas& c, const Theme& t)
    {
        const auto scale = t.color("RetroDisplayScaleText");
        const float u = unit() * kExpandScale;
        for (int env = 0; env < 3; ++env)
        {
            const float x = kEnvX[env];
            displayPanel(c, t, ex(x), ey(7), ex(x + kEnvW), ey(290));
            paint::text(c, env == 0 ? "Amp" : env == 1 ? "Env 2" : "Env 3", at(ex(x + 5), ey(12), ex(x + 80), ey(26)), t.color("RangeDefault"), small() * 0.78f, Align::Left);
            paint::fill(c, at(ex(x), ey(252), ex(x + kEnvW), ey(253)), paint::withAlpha(scale, 90));
            for (int mode = 0; mode < (env == 0 ? 2 : 3); ++mode)
            {
                const auto r = envModeBox(env, mode);
                const bool on = envMode_[env] == mode;
                paint::text(c, mode == 0 ? "Time" : mode == 1 ? "Slope" : "Value", r, on ? t.color("ControlTextBack") : scale, small() * 0.78f, Align::Centre, on);
                if (on) paint::fill(c, {r.x + r.w * 0.1f, r.bottom() - u, r.w * 0.8f, 1.5f * u}, t.color("ControlTextBack"));
            }
            paintEnvelope(c, t, env);
        }
        for (int l = 0; l < 2; ++l)
        {
            const float x = kLfoX[l];
            const int b = l == 0 ? W::L1Retrigger : W::L2Retrigger;
            displayPanel(c, t, ex(x), ey(7), ex(x + kLfoW), ey(290));
            const auto amber = t.color("ChosenDefault");
            paint::text(c, l == 0 ? "LFO 1" : "LFO 2", at(ex(x + 4), ey(12), ex(x + 80), ey(26)), amber, small() * 0.78f, Align::Left);
            paint::fill(c, at(ex(x), ey(253), ex(x + kLfoW), ey(254)), paint::withAlpha(scale, 90));
            // The waveform: 1.55 cycles from the phase offset, scaled by the amount.
            const auto area = at(ex(x + 22), ey(40), ex(x + kLfoW - 24), ey(222));
            SkPathBuilder p;
            const int shape = (int) std::lround(value(b + 1));
            const float shaping = value(b + 3), amount = value(b + 2), offset = value(b + 4) / 360.0f;
            for (int i = 0; i <= 200; ++i)
            {
                double phase = 1.55 * i / 200.0 + offset;
                phase -= std::floor(phase);
                float q = (float) phase;
                if (std::abs(shaping) > 1e-4f) q = std::pow(q, std::exp2(-2.0f * shaping));
                float y = 0;
                switch (shape)
                {
                    case 0: y = std::sin(6.2831853f * q); break;
                    case 1: y = 1.0f - 4.0f * std::abs(std::fmod(q + 0.75f, 1.0f) - 0.5f); break;
                    case 2: y = 1.0f - 2.0f * q; break;
                    case 3: y = q < 0.5f ? 1.0f : -1.0f; break;
                    default: y = std::sin(37.0f * std::floor((float) (1.55 * i / 200.0 + offset) * 2.0f + 1.0f)) * 0.8f; break;
                }
                const float px = area.x + area.w * (float) i / 200.0f, py = area.centreY() - area.h * 0.5f * amount * y;
                if (i == 0) p.moveTo(px, py); else p.lineTo(px, py);
            }
            stroke(c, p, amber, 2.0f * u);
        }
    }
    void paintEnvelope(SkCanvas& c, const Theme& t, int env)
    {
        // Geometry measured on the capture: stage widths 44.3 px x seconds^0.25, the sustain hold fills to a 229-px
        // total (at least 70 px); levels between y 55 (1) and y 207 (0); slope handles at stage midpoints.
        const float x = kEnvX[env];
        const int b = env == 0 ? W::AmpAttack : env == 1 ? W::E2Attack : W::E3Attack;
        const float attack = value(b), decay = value(b + 1), release = value(b + 2);
        const float slopeA = value(env == 0 ? W::AmpSlopeAttack : b + 3), slopeD = value(env == 0 ? W::AmpSlopeDecay : b + 4),
                    slopeR = value(env == 0 ? W::AmpSlopeRelease : b + 5);
        const float initial = env == 0 ? 0 : value(b + 6), peak = env == 0 ? 1 : value(b + 7), sustain = env == 0 ? value(W::AmpSustain) : value(b + 8),
                    final = env == 0 ? 0 : value(b + 9);
        const auto width = [](float s) { return 44.3f * std::pow(std::max(0.0f, s), 0.25f); };
        const float wa = width(attack), wd = width(decay), wr = width(release), ws = std::max(70.0f, 229.0f - wa - wd - wr);
        const float x0 = x + 19, y1 = 207, y0 = 55;
        const auto level = [&](float v) { return y1 - (y1 - y0) * std::clamp(v, -1.0f, 1.0f); };
        SkPathBuilder p;
        const auto segment = [&](float xa, float wseg, float from, float to, float slope, bool first) {
            for (int i = 0; i <= 24; ++i)
            {
                const float f = (float) i / 24.0f;
                const auto pt = at(ex(xa + wseg * f), ey(level(from + (to - from) * W::slopeCurve(f, slope))), 0, 0);
                if (first && i == 0) p.moveTo(pt.x, pt.y); else p.lineTo(pt.x, pt.y);
            }
        };
        segment(x0, wa, initial, peak, slopeA, true);
        segment(x0 + wa, wd, peak, sustain, slopeD, false);
        const auto hold = at(ex(x0 + wa + wd + ws), ey(level(sustain)), 0, 0);
        p.lineTo(hold.x, hold.y);
        segment(x0 + wa + wd + ws, wr, sustain, final, slopeR, false);
        const float u = unit() * kExpandScale;
        stroke(c, p, t.color("RangeDefault"), 2.0f * u);
        // Corner marks and the stage handles.
        const auto mark = t.color("RangeDefault");
        for (float cx : {x + 17.0f, x + kEnvW - 17.0f})
            for (float cy : {50.0f, 212.0f})
            {
                const float dx = cx < x + kEnvW * 0.5f ? 1.0f : -1.0f, dy = cy < 100 ? 1.0f : -1.0f;
                const auto a = at(ex(cx), ey(cy), 0, 0), b2 = at(ex(cx + 8 * dx), ey(cy + 8 * dy), 0, 0);
                paint::line(c, a.x, a.y, b2.x, a.y, mark, 1.3f * u);
                paint::line(c, a.x, a.y, a.x, b2.y, mark, 1.3f * u);
            }
        const auto square = [&](float px, float py) {
            const auto q = at(ex(px), ey(py), 0, 0);
            paint::stroke(c, {q.x - 3.5f * u, q.y - 3.5f * u, 7 * u, 7 * u}, t.color("ChosenDefault"), 1.3f * u);
        };
        const auto dot = [&](float px, float py) {
            const auto q = at(ex(px), ey(py), 0, 0);
            SkPaint fill;
            fill.setAntiAlias(true);
            fill.setColor(paint::withAlpha(t.color("RetroDisplayScaleText"), 200));
            c.drawCircle(q.x, q.y, 3 * u, fill);
        };
        if (env > 0) square(x0, level(initial));
        square(x0 + wa, level(peak));
        square(x0 + wa + wd, level(sustain));
        square(x0 + wa + wd + ws + wr, level(final));
        dot(x0 + wa * 0.5f, level(initial + (peak - initial) * W::slopeCurve(0.5f, slopeA)));
        dot(x0 + wa + wd * 0.5f, level(peak + (sustain - peak) * W::slopeCurve(0.5f, slopeD)));
        dot(x0 + wa + wd + ws + wr * 0.5f, level(sustain + (final - sustain) * W::slopeCurve(0.5f, slopeR)));
    }

    struct Cell
    {
        DragValue* widget = nullptr;
        std::function<int()> parameter;
    };
    W* synth_;
    int modelId_;
    std::shared_ptr<const std::string> modelSource_;
    std::vector<std::tuple<Widget*, float, float>> titleWidgets_;
    std::array<TextChooser*, kVisibleRows> rowLabels_{};
    std::array<std::array<Cell, 6>, kVisibleRows> cells_{};
    std::array<Widget*, 2> slopes_{}, syncButtons_{};
    std::array<std::array<Widget*, 5>, 2> shapeButtons_{};
    std::array<std::function<bool()>, 2> driveShown_{};
    std::vector<int> addedRows_;
    int oscTab_ = 0, matrixTab_ = 0, matrixScroll_ = 0, dragFilter_ = -1;
    int envMode_[3] = {0, 0, 0};
    bool expanded_ = false, buildingExpanded_ = false;
};
} // namespace

ReferenceFace* addWavetableFace(Widget& parent, const model::Device& model, devices::DeviceProcessor* processor, ReferenceFace::Tag tag)
{
    if (auto* synth = dynamic_cast<W*>(processor)) return parent.add<WavetableFace>(parent, model, synth, std::move(tag));
    return nullptr;
}
} // namespace sonora::ui
