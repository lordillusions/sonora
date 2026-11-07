// DEV-28j: the face of Tension (`StringStudio`), matched to 1370 x 420 retina captures of the reference device (default
// light theme, String and Filter/Global tabs, plus 24 variants with every parameter at k / 11). The device box spans
// x 18..1342; its title band is y 20..54 (34 px) and the body y 54..396 (342 px, `captureBody`). Every box below is in
// those captures' pixels. The title band carries the String / Filter-Global tabs (the plain parameter CurrentTab, so
// the set saves the shown tab) and the inert A | A | S letters. Value texts follow the processor's value laws
// (Tension::filterHz, envelopeMs, ...), which were read from the same captures.

#include "ui/views/ReferenceFace.h"

#include "devices/native/Tension.h"
#include "gfx/Fonts.h"
#include "ui/Controls.h"
#include "ui/UiContext.h"
#include "ui/widgets/Knob.h"

#include "include/core/SkPaint.h"
#include "include/core/SkPathBuilder.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <tuple>

namespace sonora::ui
{
namespace
{
using T = devices::Tension;
using Law = ReferenceFace::Law;
constexpr float kOriginX = 18, kOriginY = 54, kWidth = 1324, kBody = 342;
constexpr float kRight = 1342, kTitleTop = 20, kTitleHeight = 34; // the capture's title band (the box's 18-unit band)
constexpr const char* kAngle = "\xE2\x80\xB9";                    // the reference's "‹Key" / "‹Mod" mark

std::string printed(double value, const char* format)
{
    char text[48];
    std::snprintf(text, sizeof(text), format, value);
    return text;
}
float clean(float v, float tiny) { return std::abs(v) < tiny ? 0.0f : v; } // no "-0 %" / "-0.00"
std::string percent(float v) { return printed(clean(v * 100.0f, 0.5f), "%.0f %%"); }
std::string amountPercent(float v) // Pressure / Slide amounts: "-9.1 %", "25 %"
{
    const float p = clean(v * 100.0f, 0.05f);
    return printed(p, std::abs(p) < 9.95f ? "%.1f %%" : "%.0f %%");
}
std::string milliseconds(float ms) { return ms < 999.5f ? printed(clean(ms, 0.5f), "%.0f ms") : ReferenceFace::significant(ms / 1000.0f) + " s"; }
std::string hertz(float hz) { return printed(hz, hz < 9.95f ? "%.1f Hz" : "%.0f Hz"); }
std::string twoDecimals(float v) { return printed(clean(v, 0.005f), "%.2f"); }
std::string whole(float v, const char* unit) { return printed(clean(std::round(v), 0.5f), "%.0f") + unit; }
std::string decibels(float v)
{
    if (v <= 0.0f) return "-\xE2\x88\x9E dB";
    // The reference shows its default 0.705 (2 v^2 = -0.05 dB) as 0.0 dB.
    const float db = clean(T::volumeDb(v), 0.075f);
    return printed(db, std::abs(db) >= 9.95f ? "%.0f dB" : "%.1f dB");
}

// Inverse of a monotonic value law by bisection (handle drags on the displays).
float inverse(const std::function<float(float)>& law, float target)
{
    float lo = 0, hi = 1;
    for (int i = 0; i < 32; ++i)
    {
        const float mid = 0.5f * (lo + hi);
        (law(mid) < target ? lo : hi) = mid;
    }
    return 0.5f * (lo + hi);
}

void stroke(SkCanvas& c, SkPathBuilder& path, SkColor ink, float width, bool round = true)
{
    SkPaint pen;
    pen.setAntiAlias(true);
    pen.setStyle(SkPaint::kStroke_Style);
    pen.setStrokeWidth(width);
    pen.setStrokeJoin(round ? SkPaint::kRound_Join : SkPaint::kMiter_Join);
    pen.setStrokeCap(round ? SkPaint::kRound_Cap : SkPaint::kButt_Cap);
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

// The reference's framed controls: a 2-capture-px dark frame (1 pt) around a fill.
void framedBox(SkCanvas& c, const Theme& t, Rect r, float frame, SkColor fill)
{
    paint::fill(c, r, t.color("ControlContrastFrame"));
    paint::fill(c, r.reduced(frame), fill);
}

// LFO shape glyphs of the F. LFO chooser (30 x 15 capture px): sine, triangle, rectangle, random steps, smooth random.
void shapeGlyph(SkCanvas& c, Rect r, int shape, SkColor ink, float width)
{
    const float x = r.x, y = r.y, w = r.w, h = r.h;
    SkPathBuilder p;
    switch (shape)
    {
        case 0:
            for (int i = 0; i <= 24; ++i)
            {
                const float f = (float) i / 24.0f, py = y + h * (0.5f - 0.5f * std::sin(6.2831853f * (f * 1.05f - 0.08f)));
                if (i == 0) p.moveTo(x + w * f, py); else p.lineTo(x + w * f, py);
            }
            break;
        case 1: p.moveTo(x, y + h).lineTo(x + w * 0.5f, y).lineTo(x + w, y + h); break;
        case 2: p.moveTo(x, y + h).lineTo(x, y).lineTo(x + w * 0.4f, y).lineTo(x + w * 0.4f, y + h).lineTo(x + w, y + h).lineTo(x + w, y); break;
        case 3:
            p.moveTo(x, y + h * 0.75f).lineTo(x + w * 0.15f, y + h * 0.75f).lineTo(x + w * 0.15f, y).lineTo(x + w * 0.35f, y)
                .lineTo(x + w * 0.35f, y + h).lineTo(x + w * 0.5f, y + h).lineTo(x + w * 0.5f, y + h * 0.45f).lineTo(x + w * 0.75f, y + h * 0.45f)
                .lineTo(x + w * 0.75f, y + h * 0.15f).lineTo(x + w, y + h * 0.15f);
            break;
        default:
            p.moveTo(x, y + h * 0.7f).cubicTo(x + w * 0.15f, y + h * 0.7f, x + w * 0.12f, y, x + w * 0.25f, y)
                .cubicTo(x + w * 0.38f, y, x + w * 0.3f, y + h, x + w * 0.48f, y + h)
                .cubicTo(x + w * 0.62f, y + h, x + w * 0.62f, y + h * 0.45f, x + w * 0.78f, y + h * 0.45f)
                .cubicTo(x + w * 0.9f, y + h * 0.45f, x + w * 0.9f, y + h * 0.2f, x + w, y + h * 0.2f);
            break;
    }
    stroke(c, p, ink, width, shape != 2 && shape != 3);
}

// The sync-rate glyph of the LFO's Hz / sync buttons (a metronome).
void metronomeGlyph(SkCanvas& c, Rect r, SkColor ink, float width)
{
    const float x = r.x, y = r.y, w = r.w, h = r.h;
    SkPathBuilder p;
    p.moveTo(x + w * 0.25f, y + h * 0.1f).lineTo(x, y + h).lineTo(x + w, y + h).lineTo(x + w * 0.8f, y + h * 0.5f);
    p.moveTo(x + w * 0.35f, y + h * 0.62f).lineTo(x + w * 0.75f, y);
    stroke(c, p, ink, width, false);
}

// ---------------------------------------------------------------- widgets
// The reference's small device knob: a 39-px ring open at the lower right (the value arc runs clockwise from six
// o'clock to three), a needle from the centre and the value to the lower right. The label is the face's (it changes
// with the exciter type). `dim`: the section is off (grey ring, white value arc), as the reference draws it.
class Dial final : public Knob
{
public:
    Dial(Knob::Source source, std::function<float()> unit) : Knob("", source), source_(std::move(source)), unit_(std::move(unit)) {}
    std::function<bool()> dim;
    bool bipolar = false; // Str/Body: the arc from the middle, marked by a triangle outside the ring
    static constexpr float kRingX = 22, kRingY = 40; // ring centre, capture px from the widget's top left

    void paint(SkCanvas& c, const Theme& t) override
    {
        const float u = unit_(), norm = std::clamp(source_.get ? source_.get() : 0.0f, 0.0f, 1.0f);
        const bool off = dim && dim();
        const float cx = kRingX * u, cy = kRingY * u, thick = 4.6f * u, radius = 19.5f * u - thick * 0.5f;
        const SkColor ink = t.color(off ? "DeactivatedPoti" : "Poti"), lit = t.color(off ? "ControlDisabled" : "RangeDefault");
        const SkRect oval = SkRect::MakeLTRB(cx - radius, cy - radius, cx + radius, cy + radius);
        SkPaint pen;
        pen.setAntiAlias(true);
        pen.setStyle(SkPaint::kStroke_Style);
        pen.setStrokeWidth(thick);
        pen.setColor(ink);
        c.drawArc(oval, 90, 270, false, pen);
        const float from = bipolar ? 0.5f : 0.0f;
        if (std::abs(norm - from) > 0.002f)
        {
            pen.setColor(lit);
            c.drawArc(oval, 90 + 270 * std::min(norm, from), 270 * std::abs(norm - from), false, pen);
        }
        const float angle = (90 + 270 * norm) * (float) M_PI / 180.0f, reach = 19.5f * u;
        pen.setColor(ink);
        c.drawLine(cx, cy, cx + std::cos(angle) * reach, cy + std::sin(angle) * reach, pen);
        if (bipolar) // the centre mark outside the ring at the arc's middle (upper left), pointing in
        {
            const float a = 225 * (float) M_PI / 180.0f, d = 22.5f * u, s = 4.6f * u;
            const float tx = cx + std::cos(a) * d, ty = cy + std::sin(a) * d;
            SkPathBuilder tri;
            tri.moveTo(tx + std::cos(a) * s * 0.2f + std::cos(a + 1.5708f) * s, ty + std::sin(a) * s * 0.2f + std::sin(a + 1.5708f) * s)
                .lineTo(tx + std::cos(a) * s * 0.2f - std::cos(a + 1.5708f) * s, ty + std::sin(a) * s * 0.2f - std::sin(a + 1.5708f) * s)
                .lineTo(tx - std::cos(a) * s, ty - std::sin(a) * s)
                .close();
            SkPaint fill;
            fill.setAntiAlias(true);
            fill.setColor(std::abs(norm - 0.5f) > 0.002f && ! off ? lit : ink);
            c.drawPath(tri.detach(), fill);
        }
        // The value starts 14 px right of the ring centre, or ends 72 px right of it when longer ("0.0 dB").
        const float size = fontSize > 0 ? fontSize : t.metrics.smallFontSize;
        const auto value = source_.format ? source_.format(0) : std::string();
        const float w = gfx::measureText(value, size), left = std::max(0.0f, std::min((kRingX + 14) * u, (kRingX + 72) * u - w));
        paint::text(c, value, {left, (kRingY + 5.5f) * u, w + 2, 20 * u}, t.color("ControlForeground"), size);
    }

private:
    Knob::Source source_;
    std::function<float()> unit_;
};

// A framed number box: the slider kind fills with cyan up to the value ("22.0 kHz", "50 %"); the modulation kind
// (Key / Vel / Env / LFO amounts, Detune, Stretch) has the triangle at its right, cyan while the amount is not neutral.
// Drag vertically (Shift fine), wheel, double-click resets. `look`: 0 normal, 1 its section is off (grey fill, no cyan),
// 2 off but light (Pickup, Portamento: the reference keeps the light field and hides the fill).
class Box final : public Widget
{
public:
    enum class Kind { Slider, Mod };
    Box(T* p, int index, Law law, std::function<std::string(float)> format, Kind kind, std::function<float()> size, std::function<float()> unit)
        : p_(p), index_(index), law_(std::move(law)), format_(std::move(format)), kind_(kind), size_(std::move(size)), unit_(std::move(unit)) {}
    std::function<int()> look;
    float neutral = 0, step = 0;

    float norm() const { return std::clamp(law_.toNorm(p_->getParameter(index_)), 0.0f, 1.0f); }
    void paint(SkCanvas& c, const Theme& t) override
    {
        const auto r = localBounds();
        const float u = unit_(), frame = 2 * u;
        const int state = look ? look() : 0;
        framedBox(c, t, r, frame, t.color(state == 1 ? "SurfaceBackground" : "ControlTextBack"));
        const auto inner = r.reduced(frame);
        if (kind_ == Kind::Slider && state == 0 && norm() > 0)
            paint::fill(c, {inner.x, inner.y, inner.w * norm(), inner.h}, t.color("RangeEditField"));
        paint::text(c, format_(p_->getParameter(index_)), {0, 0, std::max(r.w, gfx::measureText(format_(p_->getParameter(index_)), size_()) + 2), r.h},
                    t.color("ControlForeground"), size_(), Align::Centre);
        if (kind_ == Kind::Mod)
        {
            const float tw = 12 * u, right = inner.right();
            const bool moved = std::abs(p_->getParameter(index_) - neutral) > 1e-4f;
            const SkColor ink = state == 1 ? t.color("ControlDisabled")
                                : moved    ? t.color("RangeDefault")
                                           : paint::mix(t.color("ControlTextBack"), t.color("SurfaceBackground"), 0.45f);
            SkPathBuilder tri;
            tri.moveTo(right, inner.y + 1 * u).lineTo(right, inner.bottom() - 1 * u).lineTo(right - tw, inner.centreY()).close();
            SkPaint fill;
            fill.setAntiAlias(true);
            fill.setColor(ink);
            c.drawPath(tri.detach(), fill);
        }
    }
    void mouseDown(const MouseEvent& e) override { pressed_ = ! e.right; startY_ = e.y; start_ = norm(); }
    void mouseDrag(const MouseEvent& e) override
    {
        if (! pressed_) return;
        set(start_ + (startY_ - e.y) / (e.shift ? 1000.0f : 200.0f));
    }
    void mouseUp(const MouseEvent&) override { pressed_ = false; }
    void mouseDoubleClick(const MouseEvent& e) override
    {
        if (e.right) return;
        p_->setParameter(index_, p_->parameters()[(size_t) index_].defaultValue);
        repaint();
    }
    bool mouseWheel(const MouseEvent& e) override
    {
        if (e.right || std::abs(e.wheelDeltaX) > std::abs(e.wheelDeltaY)) return false;
        const auto& info = p_->parameters()[(size_t) index_];
        if (step > 0) p_->setParameter(index_, std::clamp(p_->getParameter(index_) + (e.wheelDeltaY > 0 ? step : -step), info.minValue, info.maxValue));
        else set(norm() + (e.wheelDeltaY > 0 ? 1.0f : -1.0f) * (e.shift ? 0.002f : 0.02f));
        repaint();
        return true;
    }

private:
    void set(float n)
    {
        const auto& info = p_->parameters()[(size_t) index_];
        float v = std::clamp(law_.fromNorm(std::clamp(n, 0.0f, 1.0f)), info.minValue, info.maxValue);
        if (step > 0) v = std::round(v / step) * step;
        p_->setParameter(index_, v);
        repaint();
    }
    T* p_;
    int index_;
    Law law_;
    std::function<std::string(float)> format_;
    Kind kind_;
    std::function<float()> size_, unit_;
    float startY_ = 0, start_ = 0;
    bool pressed_ = false;
};

// Square on/off switch with its label: on light panels a framed box (amber when on); on the dark displays an amber
// frame that fills when on; the Vibrato switch is cyan.
class Check final : public Widget
{
public:
    enum class Look { Panel, Display, Cyan };
    Check(T* p, int index, std::string label, Look look, bool bold, float box, float gap, std::function<float()> size, std::function<float()> unit)
        : p_(p), index_(index), label_(std::move(label)), look_(look), bold_(bold), box_(box), gap_(gap), size_(std::move(size)), unit_(std::move(unit)) {}
    void paint(SkCanvas& c, const Theme& t) override
    {
        const float u = unit_(), b = box_ * u, frame = 2 * u;
        const Rect square{0, (height() - b) * 0.5f, b, b};
        const bool on = p_->getParameter(index_) > 0.5f;
        if (look_ == Look::Panel)
            framedBox(c, t, square, frame, on ? t.color("ChosenDefault") : t.color("ControlTextBack"));
        else
        {
            const auto ink = t.color(look_ == Look::Cyan ? "RangeDefault" : "ChosenDefault");
            if (on) paint::fill(c, square, ink);
            else
            {
                paint::fill(c, square, ink);
                paint::fill(c, square.reduced(frame), t.color("RetroDisplayBackground"));
            }
        }
        const float x = b + gap_ * u;
        paint::text(c, label_, {x, 0, width() - x, height()}, look_ == Look::Panel ? t.color("ControlForeground") : t.color("SurfaceBackground"),
                    size_(), Align::Left, bold_);
    }
    void mouseUp(const MouseEvent& e) override
    {
        if (e.right || ! localBounds().contains(e.x, e.y)) return;
        p_->setParameter(index_, p_->getParameter(index_) > 0.5f ? 0.0f : 1.0f);
        if (parent()) parent()->layout();
        repaint();
    }

private:
    T* p_;
    int index_;
    std::string label_;
    Look look_;
    bool bold_;
    float box_, gap_;
    std::function<float()> size_, unit_;
};

// Framed text / glyph button (Fix.Pos, Unison, unison voices 2 / 4, LFO Hz / sync): amber when on. While its section is
// off (`dim`) the chosen one turns white and the others grey, as in the reference.
class Button final : public Widget
{
public:
    Button(std::string label, std::function<float()> size, std::function<float()> unit) : label_(std::move(label)), size_(std::move(size)), unit_(std::move(unit)) {}
    std::function<bool()> on, dim;
    std::function<void()> click;
    std::function<void(SkCanvas&, Rect, SkColor)> glyph;
    void paint(SkCanvas& c, const Theme& t) override
    {
        const bool lit = on && on(), off = dim && dim();
        const SkColor fill = lit ? (off ? t.color("ControlDisabled") : t.color("ChosenDefault"))
                                 : (off ? paint::mix(t.color("ControlTextBack"), t.color("SurfaceBackground"), 0.3f) : t.color("ControlTextBack"));
        framedBox(c, t, localBounds(), 2 * unit_(), fill);
        const SkColor ink = off && ! lit ? t.color("TextDisabled") : t.color("ControlForeground");
        if (glyph) glyph(c, localBounds(), ink);
        else paint::text(c, label_, localBounds(), ink, size_(), Align::Centre);
    }
    void mouseUp(const MouseEvent& e) override
    {
        if (e.right || ! localBounds().contains(e.x, e.y) || ! click) return;
        click();
        if (parent()) parent()->layout();
        repaint();
    }

private:
    std::string label_;
    std::function<float()> size_, unit_;
};

// A chooser on a dark display: the choice's text (or glyph) and a small down arrow, both in `ink`; a click opens the list.
class DarkChooser final : public Widget
{
public:
    DarkChooser(T* p, int index, std::function<float()> size, std::function<float()> unit) : p_(p), index_(index), size_(std::move(size)), unit_(std::move(unit)) {}
    std::function<SkColor(const Theme&)> ink;
    std::function<void(SkCanvas&, Rect, int, SkColor)> glyph;
    float arrowX = 0; // arrow centre, capture px from the left
    void paint(SkCanvas& c, const Theme& t) override
    {
        const float u = unit_();
        const auto colour = ink(t);
        const int v = current();
        const auto& choices = p_->parameters()[(size_t) index_].choices;
        if (glyph) glyph(c, {0, height() * 0.5f - 7.5f * u, 30 * u, 15 * u}, v, colour);
        else if (v >= 0 && (size_t) v < choices.size())
            paint::text(c, choices[(size_t) v], {0, 0, arrowX * u - 8 * u, height()}, colour, size_());
        ctl::triangle(c, arrowX * u, height() * 0.5f + 1 * u, 11 * u, 6 * u, ctl::Dir::Down, colour);
    }
    void mouseUp(const MouseEvent& e) override
    {
        if (e.right || ! localBounds().contains(e.x, e.y)) return;
        choose(this, p_->parameters()[(size_t) index_].choices, current(), [this](int pick) {
            p_->setParameter(index_, (float) pick);
            if (parent()) parent()->layout();
            repaint();
        });
    }
    bool mouseWheel(const MouseEvent& e) override
    {
        if (e.right || std::abs(e.wheelDeltaX) > std::abs(e.wheelDeltaY)) return false;
        const auto& info = p_->parameters()[(size_t) index_];
        p_->setParameter(index_, std::clamp(std::round(p_->getParameter(index_)) + (e.wheelDeltaY > 0 ? -1.0f : 1.0f), info.minValue, info.maxValue));
        repaint();
        return true;
    }

private:
    int current() const { return (int) std::lround(p_->getParameter(index_)); }
    T* p_;
    int index_;
    std::function<float()> size_, unit_;
};

// The light drop-down field of the reference (Exciter type, body type / size, Voices, Priority): text 4 px in, a
// 11 x 7 arrow 9 px from the right edge, so a 48-px field still shows "XS".
class LightChooser final : public Widget
{
public:
    LightChooser(T* p, int index, std::function<float()> size, std::function<float()> unit) : p_(p), index_(index), size_(std::move(size)), unit_(std::move(unit)) {}
    void paint(SkCanvas& c, const Theme& t) override
    {
        const float u = unit_();
        framedBox(c, t, localBounds(), 2 * u, t.color("ControlTextBack"));
        const auto& choices = p_->parameters()[(size_t) index_].choices;
        const int v = current();
        const auto ink = t.color("ControlForeground");
        if (v >= 0 && (size_t) v < choices.size()) // never shortened: "XS" touches the arrow in the reference too
            paint::text(c, choices[(size_t) v], {4 * u, 0, std::max(width() - 21 * u, gfx::measureText(choices[(size_t) v], size_()) + 2), height()}, ink,
                        size_());
        ctl::triangle(c, width() - 11 * u, height() * 0.5f, 13 * u, 9 * u, ctl::Dir::Down, ink);
    }
    void mouseUp(const MouseEvent& e) override
    {
        if (e.right || ! localBounds().contains(e.x, e.y)) return;
        choose(this, p_->parameters()[(size_t) index_].choices, current(), [this](int pick) {
            p_->setParameter(index_, (float) pick);
            if (parent()) parent()->layout();
            repaint();
        });
    }
    bool mouseWheel(const MouseEvent& e) override
    {
        if (e.right || std::abs(e.wheelDeltaX) > std::abs(e.wheelDeltaY)) return false;
        const auto& info = p_->parameters()[(size_t) index_];
        p_->setParameter(index_, std::clamp(std::round(p_->getParameter(index_)) + (e.wheelDeltaY > 0 ? -1.0f : 1.0f), info.minValue, info.maxValue));
        repaint();
        return true;
    }

private:
    int current() const { return (int) std::lround(p_->getParameter(index_)); }
    T* p_;
    int index_;
    std::function<float()> size_, unit_;
};

// A value on a dark display: an optional grey label above it (the String / Vibrato strip) or the value alone (the
// Pressure / Slide amounts, Pitch Bend). Drag vertically, wheel, double-click resets.
class Readout final : public Widget
{
public:
    Readout(T* p, int index, std::string label, std::function<std::string(float)> format, std::function<float()> size, std::function<float()> unit)
        : p_(p), index_(index), label_(std::move(label)), format_(std::move(format)), size_(std::move(size)), unit_(std::move(unit)) {}
    std::function<SkColor(const Theme&)> ink;
    float valueX = 0, step = 0; // value's left edge (capture px) under a label
    Align align = Align::Left;
    void paint(SkCanvas& c, const Theme& t) override
    {
        const float u = unit_();
        const auto text = format_(p_->getParameter(index_));
        if (label_.empty())
        {
            const float w = std::max(width(), gfx::measureText(text, size_()) + 2);
            const float x = align == Align::Right ? width() - w : align == Align::Centre ? (width() - w) * 0.5f : 0.0f;
            paint::text(c, text, {x, 0, w, height()}, ink(t), size_(), align);
            return;
        }
        const float half = height() * 0.5f;
        paint::text(c, label_, {0, 0, width(), half}, t.color("SurfaceBackground"), size_());
        paint::text(c, text, {valueX * u, half + 2 * u, std::max(width() - valueX * u, gfx::measureText(text, size_()) + 2), half - 2 * u}, ink(t), size_());
    }
    void mouseDown(const MouseEvent& e) override
    {
        pressed_ = ! e.right;
        startY_ = e.y;
        start_ = norm();
    }
    void mouseDrag(const MouseEvent& e) override { if (pressed_) set(start_ + (startY_ - e.y) / (e.shift ? 1000.0f : 200.0f)); }
    void mouseUp(const MouseEvent&) override { pressed_ = false; }
    void mouseDoubleClick(const MouseEvent& e) override
    {
        if (e.right) return;
        p_->setParameter(index_, p_->parameters()[(size_t) index_].defaultValue);
        repaint();
    }
    bool mouseWheel(const MouseEvent& e) override
    {
        if (e.right || std::abs(e.wheelDeltaX) > std::abs(e.wheelDeltaY)) return false;
        set(norm() + (e.wheelDeltaY > 0 ? 1.0f : -1.0f) * (e.shift ? 0.002f : 0.02f));
        return true;
    }

private:
    float norm() const
    {
        const auto& info = p_->parameters()[(size_t) index_];
        return (p_->getParameter(index_) - info.minValue) / (info.maxValue - info.minValue);
    }
    void set(float n)
    {
        const auto& info = p_->parameters()[(size_t) index_];
        float v = info.minValue + std::clamp(n, 0.0f, 1.0f) * (info.maxValue - info.minValue);
        if (step > 0) v = std::round(v / step) * step;
        p_->setParameter(index_, v);
        repaint();
    }
    T* p_;
    int index_;
    std::string label_;
    std::function<std::string(float)> format_;
    std::function<float()> size_, unit_;
    float startY_ = 0, start_ = 0;
    bool pressed_ = false;
};

// String / Filter-Global tab of the device title (in the box's title band): both panel grey; the shown one runs down
// into the body. The Filter/Global tab carries the amber dot while the filter is on.
class TitleTab final : public Widget
{
public:
    std::string label;
    std::function<bool()> selected, dot;
    std::function<void()> onSelect;
    std::function<float()> unit; // box units per capture px
    void paint(SkCanvas& c, const Theme& t) override
    {
        const float k = unit(), tab = 26 * k;
        const auto fill = t.color("SurfaceBackground");
        if (selected()) // down into the body, square at the bottom
        {
            paint::fillRounded(c, {0, 0, width(), height()}, 4 * k, fill);
            paint::fill(c, {0, height() - 6 * k, width(), 6 * k}, fill);
        }
        else
            paint::fillRounded(c, {0, 0, width(), tab}, 4 * k, fill);
        const float size = 20 * k;
        const float w = gfx::measureText(label, size);
        float x = (width() - w) * 0.5f;
        if (dot && dot())
        {
            SkPaint fillDot;
            fillDot.setAntiAlias(true);
            fillDot.setColor(t.color("ChosenDefault"));
            c.drawCircle(17 * k, tab * 0.5f, 4 * k, fillDot);
            x = std::max(x, 27 * k);
        }
        paint::text(c, label, {x, 0, width() - x, tab}, t.color("ControlForeground"), size, Align::Left);
    }
    void mouseUp(const MouseEvent& e) override
    {
        if (e.right || ! localBounds().contains(e.x, e.y)) return;
        onSelect();
        repaint();
    }
};

// The inert A | A | S letters left of the title circles.
class HintLetters final : public Widget
{
public:
    std::function<float()> unit;
    void paint(SkCanvas& c, const Theme& t) override
    {
        const float k = unit();
        const auto ink = t.color("TextDisabled");
        const auto dark = paint::mix(t.color("ControlForeground"), t.color("SurfaceBackground"), 0.25f);
        const float size = 20 * k;
        const char* letters[] = {"A", "A", "S"};
        const float centres[] = {10, 32, 53}; // from x 1180
        for (int i = 0; i < 3; ++i)
            paint::text(c, letters[i], {centres[i] * k - 8 * k, 0, 16 * k, height()}, dark, size, Align::Centre, true);
        for (float x : {21.0f, 42.0f}) paint::line(c, x * k, 2 * k, x * k, height() - 2 * k, ink, 1.6f * k);
    }
};

// ---------------------------------------------------------------- the face
class TensionFace final : public ReferenceFace
{
public:
    TensionFace(Widget& box, T* synth, Tag tagger) : ReferenceFace(synth, kWidth, kOriginX, kOriginY, std::move(tagger), kBody), synth_(synth)
    {
        buildTitle(box);
        buildString();
        buildGlobal();
        // Volume closes both tabs.
        dial(T::Volume, 1259, 355.5f, "Volume", -1, {});
    }

    void layout() override
    {
        ReferenceFace::layout();
        // Title widgets live in the device box, anchored to its right edge at the title band's scale.
        const float k = titleUnit(), right = bounds().right();
        for (auto& [widget, x0, y0, x1, y1] : titleWidgets_)
            widget->setBounds({right - (kRight - x0) * k, (y0 - kTitleTop) * k, (x1 - x0) * k, (y1 - y0) * k});
        refreshNames();
    }

    void paint(SkCanvas& c, const Theme& t) override
    {
        refreshNames();
        fillBody(c, t, true);
        if (tab() == 0) paintString(c, t);
        else paintGlobal(c, t);
        panel(c, t, 1228, 298, 1338, 392);
        for (const auto& d : dials_)
            if (d.widget->visible) dialLabel(c, t, d);
    }

    void mouseDown(const MouseEvent& e) override
    {
        drag_ = Drag::None;
        if (e.right || tab() != 1) return;
        const float x = e.x / unit() + originX_, y = e.y / unit() + originY_;
        const auto hits = [&](float hx, float hy, float r) { return std::hypot(x - hx, y - hy) < r; };
        if (hits(filterHandleX(), filterHandleY(), 16) || (x > 22 && x < 280 && y > 88 && y < 228)) drag_ = Drag::Filter;
        else if (hits(envAttackX(), 101, 12)) drag_ = Drag::Attack;
        else if (hits(envDecayX(), envLevelY(value(T::FenvSustain)), 12)) drag_ = Drag::Decay;
        else if (hits(430 + envWidth(T::FenvRelease), 210, 12)) drag_ = Drag::Release;
        if (drag_ != Drag::None) mouseDrag(e);
    }
    void mouseDrag(const MouseEvent& e) override
    {
        const float x = e.x / unit() + originX_, y = e.y / unit() + originY_;
        const auto envFrom = [](float px) { return inverse(T::envelopeMs, 5.0f * std::exp(std::max(0.0f, px) / kEnvScale)); };
        switch (drag_)
        {
            case Drag::Filter:
                set(T::FilterFreq, std::clamp((x - 34) / 232, 0.0f, 1.0f));
                set(T::FilterRes, std::clamp((216 - y) / 101, 0.0f, 1.0f));
                break;
            case Drag::Attack: set(T::FenvAttack, envFrom(x - 302)); break;
            case Drag::Decay:
                set(T::FenvDecay, envFrom(x - envAttackX()));
                set(T::FenvSustain, std::clamp((210 - y) / 109, 0.0f, 1.0f));
                break;
            case Drag::Release: set(T::FenvRelease, envFrom(x - 430)); break;
            case Drag::None: return;
        }
        repaint();
    }
    void mouseUp(const MouseEvent&) override { drag_ = Drag::None; }

private:
    enum class Drag { None, Filter, Attack, Decay, Release };
    struct DialInfo
    {
        Dial* widget;
        int parameter;
        float cx, cy, labelCentre; // labelCentre < 0: the reference's rule (see dialLabel)
        std::string label;
    };
    static constexpr float kEnvScale = 7.8f; // envelope display: capture px per e-fold of the stage time above 5 ms

    float value(int p) const { return synth_->getParameter(p); }
    void set(int p, float v) { synth_->setParameter(p, v); }
    bool on(int p) const { return value(p) > 0.5f; }
    int tab() const { return std::lround(value(T::CurrentTab)) == 1 ? 1 : 0; }
    float titleUnit() const { return 18.0f / kTitleHeight; } // DetailView's kTitle per capture title px
    std::function<float()> unitFn() { return [this] { return unit(); }; }
    std::function<float()> sizeFn() { return [this] { return text(); }; }

    template <typename W> W* placed(W* widget, float x0, float y0, float x1, float y1, int page, std::function<bool()> when = {})
    {
        place(widget, x0, y0, x1, y1);
        showWhen(widget, [this, page, when] { return (page < 0 || tab() == page) && (! when || when()); });
        return widget;
    }

    // ---- control builders (capture boxes)
    Dial* dial(int p, float cx, float cy, std::string label, int page, std::function<bool()> dim, float labelCentre = -1)
    {
        Knob::Source s;
        s.minValue = 0;
        s.maxValue = 1;
        s.get = [this, p] { return std::clamp(value(p), 0.0f, 1.0f); };
        s.set = [this, p](float n) { set(p, std::clamp(n, 0.0f, 1.0f)); };
        s.defaultValue = synth_->parameters()[(size_t) p].defaultValue;
        s.format = [this, p](float) { return tensionValueText(*synth_, p, value(p)); };
        auto* widget = add<Dial>(std::move(s), unitFn());
        widget->style = Knob::Style::Device;
        widget->dim = std::move(dim);
        tag(widget, p);
        placed(widget, cx - Dial::kRingX, cy - Dial::kRingY, cx + 76, cy + 28, page);
        dials_.push_back({widget, p, cx, cy, labelCentre, std::move(label)});
        return widget;
    }
    Box* box(int p, Box::Kind kind, float x0, float y0, float x1, float y1, int page, std::function<int()> look = {}, Law law = Law::linear(0, 1))
    {
        if (kind == Box::Kind::Mod && synth_->parameters()[(size_t) p].minValue < 0) law = Law::linear(-1, 1);
        auto* widget = add<Box>(synth_, p, std::move(law), [this, p](float v) { return tensionValueText(*synth_, p, v); }, kind, sizeFn(), unitFn());
        widget->look = std::move(look);
        tag(widget, p);
        return placed(widget, x0, y0, x1, y1, page);
    }
    Check* check(int p, std::string label, Check::Look look, bool bold, float x0, float y0, float x1, float y1, float boxSize, float gap, int page)
    {
        auto* widget = add<Check>(synth_, p, std::move(label), look, bold, boxSize, gap, sizeFn(), unitFn());
        tag(widget, p);
        return placed(widget, x0, y0, x1, y1, page);
    }
    Button* button(int p, std::string label, float x0, float y0, float x1, float y1, int page, std::function<bool()> isOn, std::function<void()> click,
                   std::function<bool()> dim = {})
    {
        auto* widget = add<Button>(std::move(label), sizeFn(), unitFn());
        widget->on = std::move(isOn);
        widget->click = std::move(click);
        widget->dim = std::move(dim);
        tag(widget, p);
        return placed(widget, x0, y0, x1, y1, page);
    }
    Button* toggleButton(int p, std::string label, float x0, float y0, float x1, float y1, int page, std::function<bool()> dim = {})
    {
        return button(p, std::move(label), x0, y0, x1, y1, page, [this, p] { return on(p); }, [this, p] { set(p, on(p) ? 0.0f : 1.0f); }, std::move(dim));
    }
    Button* optionButton(int p, float v, std::string label, float x0, float y0, float x1, float y1, int page, std::function<bool()> dim = {})
    {
        return button(p, std::move(label), x0, y0, x1, y1, page, [this, p, v] { return std::abs(value(p) - v) < 0.5f; }, [this, p, v] { set(p, v); },
                      std::move(dim));
    }
    Readout* readout(int p, std::string label, float x0, float y0, float x1, float y1, int page, std::function<SkColor(const Theme&)> ink, float valueX = 10)
    {
        auto* widget = add<Readout>(synth_, p, std::move(label), [this, p](float v) { return tensionValueText(*synth_, p, v); }, sizeFn(), unitFn());
        widget->ink = std::move(ink);
        widget->valueX = valueX;
        tag(widget, p);
        return placed(widget, x0, y0, x1, y1, page);
    }
    DarkChooser* darkChooser(int p, float x0, float y0, float x1, float y1, float arrowX, std::function<SkColor(const Theme&)> ink)
    {
        auto* widget = add<DarkChooser>(synth_, p, sizeFn(), unitFn());
        widget->ink = std::move(ink);
        widget->arrowX = arrowX - x0;
        tag(widget, p);
        return placed(widget, x0, y0, x1, y1, 1);
    }
    LightChooser* lightChooser(int p, float x0, float y0, float x1, float y1, int page)
    {
        auto* widget = add<LightChooser>(synth_, p, sizeFn(), unitFn());
        tag(widget, p);
        return placed(widget, x0, y0, x1, y1, page);
    }

    // ---- title
    void buildTitle(Widget& box)
    {
        const char* names[] = {"String", "Filter/Global"};
        const float x[][2] = {{832, 1000}, {1004, 1172}};
        for (int i = 0; i < 2; ++i)
        {
            auto* t = box.add<TitleTab>();
            t->label = names[i];
            t->unit = [this] { return titleUnit(); };
            t->selected = [this, i] { return tab() == i; };
            if (i == 1) t->dot = [this] { return on(T::FilterOn); };
            t->onSelect = [this, i] {
                set(T::CurrentTab, (float) i);
                layout();
                repaint();
            };
            t->tooltip = std::string("Show the ") + names[i] + " tab";
            tag(t, T::CurrentTab);
            titleWidgets_.push_back({t, x[i][0], 24, x[i][1], 54});
        }
        auto* letters = box.add<HintLetters>();
        letters->unit = [this] { return titleUnit(); };
        letters->tooltip = "Hint letters of the reference's title (not used by Sonora)";
        titleWidgets_.push_back({letters, 1180, 22, 1246, 52});
    }

    // ---- String tab
    void buildString()
    {
        const auto excOff = [this] { return ! on(T::ExcOn); };
        const auto damperOff = [this] { return ! on(T::DamperOn); };
        const auto termOff = [this] { return ! on(T::TermOn); };
        const auto bodyOff = [this] { return ! on(T::BodyOn); };
        const auto grey = [](std::function<bool()> off) { return [off] { return off() ? 1 : 0; }; };

        // Exciter: switch, type, five knobs (labels follow the type), Vel / Key rows and Fix.Pos.
        check(T::ExcOn, "Exciter", Check::Look::Panel, true, 30, 62, 180, 84, 22, 7, 0);
        lightChooser(T::ExcType, 30, 92, 178, 118, 0);
        const int excKnobs[] = {T::ExcX, T::ExcStiffness, T::ExcVelocity, T::ExcPosition, T::ExcDamping};
        const float excX[] = {209, 317, 417, 517, 617}, excY[] = {111.5f, 111.5f, 113.5f, 113.5f, 113.5f};
        for (int i = 0; i < 5; ++i)
            dial(excKnobs[i], excX[i], excY[i], "", 0,
                 i == 4 ? std::function<bool()>([this, excOff] { return excOff() || std::lround(value(T::ExcType)) == T::Bow; }) : excOff);
        const int excVel[] = {T::ExcXVel, T::ExcStiffnessVel, T::ExcVelocityVel, T::ExcPositionVel};
        const int excKey[] = {T::ExcXKey, T::ExcStiffnessKey, T::ExcVelocityKey, T::ExcPositionKey};
        for (int i = 0; i < 4; ++i)
        {
            const float x0 = 192 + 100.0f * (float) i;
            box(excVel[i], Box::Kind::Mod, x0, 148, x0 + 82, 174, 0, grey(excOff));
            box(excKey[i], Box::Kind::Mod, x0, 180, x0 + 82, 206, 0, grey(excOff));
        }
        toggleButton(T::ExcPositionAbs, "Fix.Pos", 596, 180, 682, 206, 0, excOff);

        // Damper.
        check(T::DamperOn, "Damper", Check::Look::Panel, true, 702, 62, 830, 84, 22, 7, 0);
        check(T::DamperGated, "Gated", Check::Look::Panel, false, 702, 96, 800, 114, 18, 8, 0);
        const int damperKnobs[] = {T::DamperMass, T::DamperStiffness, T::DamperVelocity, T::DamperPosition, T::DamperDamping};
        const char* damperNames[] = {"Mass", "Stiffness", "Velocity", "Position", "Damping"};
        for (int i = 0; i < 5; ++i) dial(damperKnobs[i], 873 + 98.0f * (float) i, 113.5f, damperNames[i], 0, damperOff);
        box(T::DamperPositionVel, Box::Kind::Mod, 1142, 148, 1224, 174, 0, grey(damperOff));
        const int damperKey[] = {T::DamperMassKey, T::DamperStiffnessKey, T::DamperVelocityKey, T::DamperPositionKey};
        for (int i = 0; i < 4; ++i) // the reference keeps the position's Key box light while the damper is off
            box(damperKey[i], Box::Kind::Mod, 848 + 98.0f * (float) i, 180, 930 + 98.0f * (float) i, 206, 0, i == 3 ? std::function<int()>{} : grey(damperOff));
        toggleButton(T::DamperPositionAbs, "Fix.Pos", 1244, 180, 1330, 206, 0, damperOff);

        // The dark String / Vibrato strip.
        const auto cyan = [](const Theme& t) { return t.color("RangeDefault"); };
        const auto amber = [](const Theme& t) { return t.color("ChosenDefault"); };
        const std::string key = std::string(kAngle) + "Key";
        readout(T::StringDecay, "Decay", 150, 230, 237, 280, 0, cyan);
        readout(T::StringDecayKey, key, 238, 230, 325, 280, 0, amber, 3);
        readout(T::StringRatio, "Ratio", 326, 230, 413, 280, 0, cyan);
        readout(T::StringInharm, "Inharm", 414, 230, 501, 280, 0, cyan);
        readout(T::StringDamping, "Damping", 502, 230, 597, 280, 0, cyan);
        readout(T::StringDampingKey, key, 598, 230, 686, 280, 0, amber, 3);
        check(T::VibratoOn, "Vibrato", Check::Look::Cyan, false, 704, 230, 826, 252, 18, 12, 0);
        const auto vibrato = [this](bool mod) {
            return [this, mod](const Theme& t) { return on(T::VibratoOn) ? t.color(mod ? "ChosenDefault" : "RangeDefault") : t.color("SurfaceBackground"); };
        };
        readout(T::VibratoDelay, "Delay", 832, 230, 919, 280, 0, vibrato(false));
        readout(T::VibratoFadeIn, "Attack", 920, 230, 1009, 280, 0, vibrato(false));
        readout(T::VibratoRate, "Rate", 1010, 230, 1093, 280, 0, vibrato(false));
        readout(T::VibratoAmount, "Amount", 1094, 230, 1177, 280, 0, vibrato(false));
        readout(T::VibratoModWheel, std::string(kAngle) + "Mod", 1178, 230, 1257, 280, 0, vibrato(true));
        readout(T::VibratoError, "Error", 1258, 230, 1334, 280, 0, vibrato(false));

        // Termination, Pickup, Body.
        check(T::TermOn, "Termination", Check::Look::Panel, true, 28, 306, 190, 328, 22, 7, 0);
        box(T::TermFingerMassVel, Box::Kind::Mod, 28, 358, 100, 384, 0, grey(termOff));
        box(T::TermFingerMassKey, Box::Kind::Mod, 106, 358, 178, 384, 0, grey(termOff));
        dial(T::TermFingerMass, 217, 355.5f, "Finger Mass", 0, termOff);
        dial(T::TermFingerStiffness, 353, 355.5f, "Finger Stiff", 0, termOff);
        dial(T::TermFretStiffness, 485, 355.5f, "Fret Stiff", 0, termOff);
        check(T::PickupOn, "Pickup", Check::Look::Panel, true, 584, 306, 686, 328, 22, 7, 0);
        box(T::PickupPosition, Box::Kind::Slider, 584, 360, 682, 386, 0, [this] { return on(T::PickupOn) ? 0 : 2; });
        check(T::BodyOn, "Body", Check::Look::Panel, true, 702, 306, 840, 328, 22, 7, 0);
        lightChooser(T::BodyType, 702, 336, 788, 362, 0);
        lightChooser(T::BodySize, 794, 336, 842, 362, 0);
        dial(T::BodyDecay, 872.5f, 355.5f, "Decay", 0, bodyOff);
        dial(T::BodyMix, 962.5f, 355.5f, "Str/Body", 0, bodyOff, 975)->bipolar = true;
        dial(T::BodyLowCut, 1056.5f, 355.5f, "Low Cut", 0, bodyOff);
        dial(T::BodyHighCut, 1148.5f, 355.5f, "High Cut", 0, bodyOff);
    }

    // ---- Filter / Global tab
    void buildGlobal()
    {
        const auto filterLook = [this] { return on(T::FilterOn) ? 0 : 1; };
        const auto fenvLook = [this] { return on(T::FenvOn) ? 0 : 1; };
        const auto lfoLook = [this] { return on(T::LfoOn) ? 0 : 1; };
        const auto unisonLook = [this] { return on(T::UnisonOn) ? 0 : 1; };

        // Filter display and its boxes.
        check(T::FilterOn, "Filter", Check::Look::Display, false, 34, 64, 150, 86, 18, 8, 1);
        darkChooser(T::FilterType, 186, 64, 272, 86, 262,
                    [this](const Theme& t) { return on(T::FilterOn) ? t.color("ChosenDefault") : t.color("ControlTextBack"); });
        box(T::FilterFreq, Box::Kind::Slider, 78, 262, 172, 288, 1, filterLook);
        box(T::FilterRes, Box::Kind::Slider, 178, 262, 272, 288, 1, filterLook);
        const int freqMods[] = {T::FilterFreqEnv, T::FilterFreqLfo, T::FilterFreqKey}, resMods[] = {T::FilterResEnv, T::FilterResLfo, T::FilterResKey};
        for (int i = 0; i < 3; ++i)
        {
            const float y0 = 294 + 32.0f * (float) i;
            box(freqMods[i], Box::Kind::Mod, 78, y0, 172, y0 + 26, 1, filterLook);
            box(resMods[i], Box::Kind::Mod, 178, y0, 272, y0 + 26, 1, filterLook);
        }

        // Filter envelope.
        check(T::FenvOn, "Filter Envelope", Check::Look::Display, false, 296, 64, 500, 86, 18, 8, 1);
        const int stages[] = {T::FenvAttack, T::FenvDecay, T::FenvSustain, T::FenvRelease};
        for (int i = 0; i < 4; ++i) box(stages[i], Box::Kind::Slider, 316, 262 + 32.0f * (float) i, 406, 288 + 32.0f * (float) i, 1, fenvLook);
        box(T::FenvAttackVel, Box::Kind::Mod, 412, 262, 502, 288, 1, fenvLook);
        box(T::FenvAmountVel, Box::Kind::Mod, 412, 326, 502, 352, 1, fenvLook);

        // Filter LFO.
        check(T::LfoOn, "F. LFO", Check::Look::Display, false, 532, 64, 622, 86, 18, 8, 1);
        auto* shape = darkChooser(T::LfoShape, 627, 64, 684, 86, 673,
                                  [this](const Theme& t) { return on(T::LfoOn) ? t.color("ChosenDefault") : t.color("ControlTextBack"); });
        shape->glyph = [this](SkCanvas& c, Rect r, int v, SkColor ink) { shapeGlyph(c, r, v, ink, 2.6f * unit()); };
        const auto synced = [this] { return on(T::LfoSyncOn); };
        auto* rate = box(T::LfoRate, Box::Kind::Slider, 600, 262, 682, 288, 1, lfoLook);
        showWhen(rate, [this, synced] { return tab() == 1 && ! synced(); });
        auto* sync = box(T::LfoSync, Box::Kind::Slider, 600, 262, 682, 288, 1, lfoLook, Law::linear(0, T::kSyncRates - 1));
        sync->step = 1;
        showWhen(sync, [this, synced] { return tab() == 1 && synced(); });
        const auto lfoOff = [this] { return ! on(T::LfoOn); };
        optionButton(T::LfoSyncOn, 0, "Hz", 600, 294, 638, 320, 1, lfoOff);
        optionButton(T::LfoSyncOn, 1, "", 644, 294, 682, 320, 1, lfoOff)->glyph = [this](SkCanvas& c, Rect r, SkColor ink) {
            const float u = unit();
            metronomeGlyph(c, {r.centreX() - 7.5f * u, r.y + 5 * u, 15 * u, 16 * u}, ink, 2.4f * u);
        };
        box(T::LfoFadeIn, Box::Kind::Slider, 600, 326, 682, 352, 1, lfoLook);
        box(T::LfoDelay, Box::Kind::Slider, 600, 358, 682, 384, 1, lfoLook);

        // Pressure / Slide / Pitch Bend.
        const auto cyan = [](const Theme& t) { return t.color("RangeDefault"); };
        const int targets[] = {T::PressureTarget1, T::PressureTarget2, T::SlideTarget1, T::SlideTarget2};
        const int amounts[] = {T::PressureAmount1, T::PressureAmount2, T::SlideAmount1, T::SlideAmount2};
        const float rows[] = {102, 133, 219, 251};
        for (int i = 0; i < 4; ++i)
        {
            darkChooser(targets[i], 722, rows[i] - 12, 914, rows[i] + 12, 907, cyan);
            readout(amounts[i], "", 916, rows[i] - 12, 986, rows[i] + 12, 1, cyan)->align = Align::Right;
        }
        auto* bend = readout(T::PitchBendRange, "", 930, 327, 990, 351, 1, cyan);
        bend->align = Align::Centre;
        bend->step = 1;
        auto* noteBend = readout(T::NotePbRange, "", 924, 359, 986, 383, 1, cyan);
        noteBend->align = Align::Right;
        noteBend->step = 1;

        // Keyboard.
        box(T::Octave, Box::Kind::Slider, 1024, 86, 1116, 112, 1, {}, Law::linear(-3, 3))->step = 1;
        box(T::Transpose, Box::Kind::Slider, 1122, 86, 1214, 112, 1, {}, Law::linear(-12, 12))->step = 1;
        box(T::FineTune, Box::Kind::Mod, 1024, 142, 1116, 168, 1)->neutral = 0.5f;
        lightChooser(T::Polyphony, 1122, 142, 1214, 168, 1);
        box(T::Stretch, Box::Kind::Mod, 1024, 198, 1116, 224, 1)->neutral = 0.5f;
        box(T::KeyError, Box::Kind::Slider, 1122, 198, 1214, 224, 1);
        lightChooser(T::Priority, 1024, 252, 1116, 278, 1);
        check(T::PortamentoOn, "Portamento", Check::Look::Panel, true, 1024, 302, 1210, 324, 18, 8, 1);
        box(T::PortamentoTime, Box::Kind::Slider, 1024, 360, 1108, 386, 1, [this] { return on(T::PortamentoOn) ? 0 : 2; });
        check(T::PortamentoLegato, "Legato", Check::Look::Panel, false, 1120, 334, 1214, 356, 18, 8, 1);
        check(T::PortamentoProportional, "Prop.", Check::Look::Panel, false, 1120, 362, 1214, 384, 18, 8, 1);

        // Unison.
        const auto unisonOff = [this] { return ! on(T::UnisonOn); };
        toggleButton(T::UnisonOn, "Unison", 1238, 64, 1328, 90, 1);
        optionButton(T::UnisonVoices, 0, "2", 1238, 142, 1280, 168, 1, unisonOff);
        optionButton(T::UnisonVoices, 1, "4", 1286, 142, 1328, 168, 1, unisonOff);
        box(T::UnisonDetune, Box::Kind::Slider, 1238, 198, 1328, 224, 1, unisonLook);
        box(T::UnisonDelay, Box::Kind::Slider, 1238, 252, 1328, 278, 1, unisonLook);
    }

    // ---- painting
    void dialLabel(SkCanvas& c, const Theme& t, const DialInfo& d) const
    {
        // Labels sit centred 14 px right of the ring centre, but never start more than 18 px left of it (measured on
        // every knob of both captures; Str/Body is placed by hand).
        const float size = text(), w = gfx::measureText(d.label, size) / unit();
        const float left = d.labelCentre >= 0 ? d.labelCentre - w * 0.5f : std::max(d.cx + 14 - w * 0.5f, d.cx - 18);
        const float y = d.cy < 200 ? 75.0f : 317.0f; // label rows of the top and bottom knob rows
        label(c, t, d.label, left - 4, y - 11, left + w + 4, y + 11);
    }
    void refreshNames()
    {
        // The exciter's knobs are named after its type (Bow: Force / Friction; Plectrum: Protrusion; hammers: Mass).
        const int type = (int) std::lround(value(T::ExcType));
        const char* first = type == T::Bow ? "Force" : type == T::Plectrum ? "Protrusion" : "Mass";
        const char* second = type == T::Bow ? "Friction" : "Stiffness";
        for (auto& d : dials_)
        {
            if (d.parameter == T::ExcX) d.label = first;
            else if (d.parameter == T::ExcStiffness) d.label = second;
            else if (d.parameter == T::ExcVelocity) d.label = "Velocity";
            else if (d.parameter == T::ExcPosition) d.label = "Position";
            else if (d.parameter == T::ExcDamping) d.label = "Damping";
            d.widget->tooltip = d.label;
        }
    }

    void paintString(SkCanvas& c, const Theme& t)
    {
        panel(c, t, 22, 54, 690, 214);
        panel(c, t, 694, 54, 1338, 214);
        displayPanel(c, t, 22, 218, 1338, 294);
        panel(c, t, 22, 298, 572, 392);
        panel(c, t, 576, 298, 690, 392);
        panel(c, t, 694, 298, 1224, 392);
        for (auto [name, y0] : {std::pair{"Vel", 148.0f}, std::pair{"Key", 180.0f}})
        {
            label(c, t, name, 120, y0, 186, y0 + 26, Align::Right);
            label(c, t, name, 770, y0, 842, y0 + 26, Align::Right);
        }
        label(c, t, "String", 32, 230, 140, 255, Align::Left, false, t.color("SurfaceBackground"));
        const auto line = at(690, 224, 692, 288);
        paint::fill(c, line, t.color("DeactivatedPoti"));
        label(c, t, "Vel", 30, 334, 90, 356, Align::Left);
        label(c, t, "Key", 108, 334, 170, 356, Align::Left);
        label(c, t, "Position", 584, 336, 686, 358, Align::Left);
    }

    void paintGlobal(SkCanvas& c, const Theme& t)
    {
        displayPanel(c, t, 22, 54, 280, 228);
        panel(c, t, 22, 232, 280, 392);
        displayPanel(c, t, 284, 54, 516, 228);
        panel(c, t, 284, 232, 516, 392);
        displayPanel(c, t, 520, 54, 692, 228);
        panel(c, t, 520, 232, 692, 392);
        displayPanel(c, t, 696, 54, 1010, 392);
        panel(c, t, 1014, 54, 1224, 392);
        panel(c, t, 1228, 54, 1338, 294);
        // Filter panel labels.
        label(c, t, "Freq", 78, 238, 172, 260);
        label(c, t, "Res", 178, 238, 272, 260);
        for (auto [name, y0] : {std::pair{"Env", 294.0f}, std::pair{"LFO", 326.0f}, std::pair{"Key", 358.0f}})
            label(c, t, name, 28, y0, 76, y0 + 26, Align::Left);
        label(c, t, "Vel", 412, 238, 502, 260);
        for (auto [name, y0] : {std::pair{"A", 262.0f}, std::pair{"D", 294.0f}, std::pair{"S", 326.0f}, std::pair{"R", 358.0f}})
            label(c, t, name, 292, y0, 312, y0 + 26);
        for (auto [name, y0] : {std::pair{"Rate", 262.0f}, std::pair{"Attack", 326.0f}, std::pair{"Delay", 358.0f}})
            label(c, t, name, 530, y0, 600, y0 + 26, Align::Left);
        // Keyboard / unison labels.
        for (auto [name, x0, x1, y0] : {std::tuple{"Octave", 1024.0f, 1116.0f, 62.0f}, std::tuple{"Semi", 1122.0f, 1214.0f, 62.0f},
                                        std::tuple{"Detune", 1024.0f, 1116.0f, 118.0f}, std::tuple{"Voices", 1122.0f, 1214.0f, 118.0f},
                                        std::tuple{"Stretch", 1024.0f, 1116.0f, 174.0f}, std::tuple{"Error", 1122.0f, 1214.0f, 174.0f},
                                        std::tuple{"Priority", 1024.0f, 1116.0f, 228.0f}, std::tuple{"Voices", 1238.0f, 1328.0f, 118.0f},
                                        std::tuple{"Detune", 1238.0f, 1328.0f, 174.0f}, std::tuple{"Delay", 1238.0f, 1328.0f, 228.0f}})
            label(c, t, name, x0, y0, x1, y0 + 22);
        paint::fill(c, at(1022, 292, 1212, 294), t.color("DeactivatedPoti"));
        label(c, t, "Time", 1026, 334, 1100, 356, Align::Left);
        paintModulation(c, t);
        paintFilter(c, t);
        paintEnvelope(c, t);
        paintLfo(c, t);
    }

    void paintModulation(SkCanvas& c, const Theme& t)
    {
        const auto grey = t.color("SurfaceBackground"), rule = t.color("DisplayBackground");
        const float u = unit();
        for (auto [name, y] : {std::pair{"Pressure", 71.0f}, std::pair{"Slide", 189.0f}, std::pair{"Pitch Bend", 309.0f}})
        {
            const auto dot = at(711, y, 711, y);
            SkPaint fill;
            fill.setAntiAlias(true);
            fill.setColor(rule);
            c.drawCircle(dot.x, dot.y, 5 * u, fill);
            label(c, t, name, 722, y - 11, 990, y + 11, Align::Left, false, grey);
            const float end = 722 + gfx::measureText(name, text()) / u + 8;
            paint::fill(c, at(end, y + 5, 1002, y + 7), rule);
        }
        label(c, t, "Pitch Bend Range", 722, 327, 930, 351, Align::Left, false, grey);
        label(c, t, "Note PB Range", 722, 359, 930, 383, Align::Left, false, grey);
    }

    // Filter display: 30 Hz .. 22 kHz over x 34..266 (the handle's travel), 0 dB at y 144, 1.6 px per dB; the handle
    // marks cutoff (x) and resonance (y 216 .. 115). The formant types show their vowel axis instead of a curve.
    float filterHandleX() const { return 34 + 232 * std::clamp(value(T::FilterFreq), 0.0f, 1.0f); }
    float filterHandleY() const { return 216 - 101 * std::clamp(value(T::FilterRes), 0.0f, 1.0f); }
    void paintFilter(SkCanvas& c, const Theme& t)
    {
        const bool active = on(T::FilterOn);
        const auto ink = active ? t.color("RangeDefault") : t.color("SurfaceBackground");
        const int type = (int) std::lround(value(T::FilterType));
        const float u = unit();
        const auto clip = at(22, 54, 280, 228);
        c.save();
        c.clipRect(clip.sk());
        if (type >= 8)
        {
            const char* vowels[] = {"U", "O", "A", "E", "I"};
            for (int i = 0; i < 5; ++i) label(c, t, vowels[i], 28, 101 + 25.0f * (float) i, 48, 123 + 25.0f * (float) i, Align::Centre, false, t.color("DeactivatedPoti"));
        }
        else
        {
            // Display Q: the reference's notches stay narrow; the other types peak moderately at full resonance.
            const float res = value(T::FilterRes), q = type == 4 || type == 5 ? 2.0f + 6.0f * res : 0.5f + 1.5f * res;
            const float cutoff = T::filterHz(value(T::FilterFreq));
            SkPathBuilder path;
            for (int i = 0; i <= 160; ++i)
            {
                const float x = 22 + 258.0f * (float) i / 160.0f, f = 30.0f * std::pow(22000.0f / 30.0f, (x - 34) / 232);
                const std::complex<float> s(0, f / cutoff), d = s * s + s / q + 1.0f;
                std::complex<float> h;
                switch (type)
                {
                    case 0: case 1: h = 1.0f / d; break;
                    case 2: case 3: h = (s / q) / d; break;
                    case 4: case 5: h = (s * s + 1.0f) / d; break;
                    default: h = (s * s) / d; break;
                }
                float magnitude = std::abs(h);
                if (type % 2 == 1) magnitude *= magnitude; // the 24 dB (4-pole) variants
                const float db = 20 * std::log10(std::max(magnitude, 1e-6f)), y = std::min(240.0f, 144 - 1.6f * db);
                const auto pt = at(x, y, x, y);
                if (i == 0) path.moveTo(pt.x, pt.y); else path.lineTo(pt.x, pt.y);
            }
            stroke(c, path, ink, 2.6f * u);
        }
        const auto h = at(filterHandleX(), filterHandleY(), filterHandleX(), filterHandleY());
        SkPaint ring;
        ring.setAntiAlias(true);
        ring.setStyle(SkPaint::kStroke_Style);
        ring.setStrokeWidth(2.6f * u);
        ring.setColor(active ? t.color("ChosenDefault") : t.color("SurfaceBackground"));
        c.drawCircle(h.x, h.y, 11 * u, ring);
        c.restore();
    }

    // Filter envelope display: stages start at x 302 and take 7.8 px per e-fold of their time above 5 ms; the release
    // starts at x 430; levels between y 210 (0) and 101 (1). Handles: attack peak, decay end, release end.
    float envWidth(int p) const { return kEnvScale * std::log(std::max(1.0f, T::envelopeMs(value(p)) / 5.0f)); }
    float envAttackX() const { return 302 + envWidth(T::FenvAttack); }
    float envDecayX() const { return envAttackX() + envWidth(T::FenvDecay); }
    static float envLevelY(float level) { return 210 - 109 * std::clamp(level, 0.0f, 1.0f); }
    void paintEnvelope(SkCanvas& c, const Theme& t)
    {
        const bool active = on(T::FenvOn);
        const float u = unit(), sustain = value(T::FenvSustain);
        const auto ink = active ? t.color("RangeDefault") : t.color("SurfaceBackground");
        SkPathBuilder path;
        const auto to = [&](float x, float y, bool first = false) {
            const auto pt = at(x, y, x, y);
            if (first) path.moveTo(pt.x, pt.y); else path.lineTo(pt.x, pt.y);
        };
        const auto curve = [&](float x0, float w, float from, float target) { // fast first, then easing into the target
            for (int i = 1; i <= 24; ++i)
            {
                const float f = (float) i / 24.0f;
                to(x0 + w * f, envLevelY(from + (target - from) * (1 - std::pow(1 - f, 3.0f))));
            }
        };
        to(302, envLevelY(0), true);
        curve(302, envWidth(T::FenvAttack), 0, 1);
        curve(envAttackX(), envWidth(T::FenvDecay), 1, sustain);
        to(430, envLevelY(sustain));
        curve(430, envWidth(T::FenvRelease), sustain, 0);
        stroke(c, path, ink, 2.6f * u);
        const auto handle = [&](float x, float y) {
            const auto q = at(x - 5, y - 5, x + 5, y + 5);
            paint::fill(c, q, t.color("RetroDisplayBackground"));
            paint::stroke(c, q, active ? t.color("ChosenDefault") : t.color("SurfaceBackground"), 2 * u);
        };
        handle(envAttackX(), envLevelY(1));
        handle(envDecayX(), envLevelY(sustain));
        handle(430 + envWidth(T::FenvRelease), envLevelY(0));
    }

    // F. LFO display: the shape across x 520..692 around y 156 (51 px each way). The reference shows (Hz - 0.1) x 1.85
    // cycles in Hz mode (a still line at its slowest rate); synced rates show four beats.
    void paintLfo(SkCanvas& c, const Theme& t)
    {
        const bool active = on(T::LfoOn);
        const int shape = (int) std::lround(value(T::LfoShape));
        const double cycles = on(T::LfoSyncOn) ? 4.0 / T::syncBeats((int) std::lround(value(T::LfoSync)))
                                               : (T::lfoHz(value(T::LfoRate)) - 0.1) * 1.85;
        const auto noise = [](int n) { uint32_t s = (uint32_t) n * 2654435761u + 0x9E3779B9u; s ^= s >> 15; s *= 2246822519u; s ^= s >> 13; return (float) (s & 0xFFFF) / 32767.5f - 1.0f; };
        const auto level = [&](double phase) {
            const double ph = phase - std::floor(phase);
            const int n = (int) std::floor(phase);
            switch (shape)
            {
                case 0: return (float) std::sin(2 * M_PI * ph);
                case 1: return (float) (1 - 4 * std::min(ph, 1 - ph));
                case 2: return ph < 0.5 ? 1.0f : -1.0f;
                case 3: return noise(n);
                default: return noise(n) + (noise(n + 1) - noise(n)) * (float) (0.5 - 0.5 * std::cos(M_PI * ph));
            }
        };
        const auto clip = at(520, 54, 692, 228);
        c.save();
        c.clipRect(clip.sk());
        SkPathBuilder path;
        const int points = 172; // one sample per capture px: fast rates alias as in the reference
        for (int i = 0; i <= points; ++i)
        {
            const float f = (float) i / (float) points, x = 520 + 172 * f, y = 156 - 51 * level(cycles * f);
            const auto pt = at(x, y, x, y);
            if (i == 0) path.moveTo(pt.x, pt.y); else path.lineTo(pt.x, pt.y);
        }
        stroke(c, path, active ? t.color("RangeDefault") : t.color("SurfaceBackground"), 2.6f * unit(), shape != 2 && shape != 3);
        c.restore();
    }

    T* synth_;
    std::vector<DialInfo> dials_;
    std::vector<std::tuple<Widget*, float, float, float, float>> titleWidgets_;
    Drag drag_ = Drag::None;
};
} // namespace

std::string tensionValueText(const devices::DeviceProcessor& processor, int p, float v)
{
    using P = T::Param;
    switch (p)
    {
        case P::ExcPosition: case P::DamperPosition: case P::PickupPosition: return percent(v * 0.5f);
        case P::ExcPositionKey: case P::ExcPositionVel: case P::DamperPositionKey: case P::DamperPositionVel: case P::ExcXKey: case P::ExcXVel:
        case P::ExcStiffnessKey: case P::ExcStiffnessVel: case P::ExcVelocityKey: case P::ExcVelocityVel: case P::DamperMassKey:
        case P::DamperStiffnessKey: case P::DamperVelocityKey: case P::StringDampingKey: case P::StringDecayKey: case P::TermFingerMassKey:
        case P::TermFingerMassVel:
            return twoDecimals(T::keyAmount(v));
        case P::FilterFreq: { const float hz = T::filterHz(v); return hz < 999.5f ? printed(hz, "%.0f Hz") : printed(hz / 1000.0f, "%.1f kHz"); }
        case P::FilterFreqEnv: return twoDecimals(T::cutoffEnvAmount(v));
        case P::FilterFreqKey: case P::FilterFreqLfo: case P::FilterResKey: case P::FilterResLfo: case P::FilterResEnv: return twoDecimals(T::modAmount(v));
        case P::FenvAttack: case P::FenvDecay: case P::FenvRelease: return milliseconds(T::envelopeMs(v));
        case P::FenvAttackVel: case P::FenvAmountVel: return twoDecimals(T::envelopeVelAmount(v));
        case P::VibratoDelay: case P::VibratoFadeIn: return milliseconds(T::vibratoMs(v));
        case P::VibratoRate: return hertz(T::vibratoHz(v));
        case P::LfoRate: return hertz(T::lfoHz(v));
        case P::LfoDelay: case P::LfoFadeIn: return milliseconds(T::lfoMs(v));
        case P::LfoSync: return T::syncName((int) std::lround(v));
        case P::UnisonDelay: return milliseconds(T::unisonMs(v));
        case P::Volume: return decibels(v);
        case P::BodyMix: case P::Stretch: return percent(2 * v - 1);
        case P::FineTune: return twoDecimals(v - 0.5f);
        case P::Octave: case P::PitchBendRange: return whole(v, "");
        case P::Transpose: case P::NotePbRange: return whole(v, " st");
        case P::PressureAmount1: case P::PressureAmount2: case P::SlideAmount1: case P::SlideAmount2: return amountPercent(v);
        case P::ExcType: case P::BodyType: case P::BodySize: case P::FilterType: case P::LfoShape: case P::Polyphony: case P::Priority:
        case P::UnisonVoices: case P::PressureTarget1: case P::PressureTarget2: case P::SlideTarget1: case P::SlideTarget2: case P::LfoSyncOn:
        case P::CurrentTab:
        {
            const auto& choices = processor.parameters()[(size_t) p].choices;
            return choices.empty() ? std::string() : choices[(size_t) std::clamp((int) std::lround(v), 0, (int) choices.size() - 1)];
        }
        case P::ExcOn: case P::ExcPositionAbs: case P::DamperPositionAbs: case P::PickupOn: case P::DamperOn: case P::DamperGated: case P::TermOn:
        case P::LfoOn: case P::FilterOn: case P::FenvOn: case P::BodyOn: case P::VibratoOn: case P::PortamentoOn: case P::PortamentoProportional:
        case P::PortamentoLegato: case P::UnisonOn:
            return v > 0.5f ? "On" : "Off";
        default: return percent(v); // the remaining 0..1 controls show percent
    }
}

ReferenceFace* addTensionFace(Widget& parent, devices::DeviceProcessor* processor, ReferenceFace::Tag tag)
{
    if (auto* synth = dynamic_cast<T*>(processor)) return parent.add<TensionFace>(parent, synth, std::move(tag));
    return nullptr;
}
} // namespace sonora::ui
