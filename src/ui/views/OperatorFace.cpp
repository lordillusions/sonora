// DEV-28a: the face of Operator, matched to a 1490 x 394 retina capture of the reference's default Operator with
// oscillator A selected (docs/screenshots/reference/live-operator-osc-a.png, default light theme). The device box spans
// x 12..1462; its title band is y 10..44 (34 px) and the body y 44..386 (342 px, `captureBody`). Every box below is in
// that capture's pixels.
//
// Left: the four oscillator strips, D at the top (Coarse / Fine or Freq / Multi, Fixed, Level, the coloured letter that
// switches the oscillator). Right: the LFO, Filter, Pitch and Global panels. Centre: the display (x 442..1032, y 16..380).
// It starts inside the title band, so the face adds a small widget to the device box (`DisplayCap`) that paints the
// display's top over the title, with the same painting code, and forwards its mouse events to the face.
//
// Clicking a strip or a panel selects it (lighter fill) and the display shows its page. Only the oscillator page is in the
// capture. The LFO, Filter, Pitch and Global pages are laid out from the reference manual's description, in the same
// visual vocabulary, awaiting a capture (DEV-28a). Controls whose targets the engine does not know yet (Dest B and the
// MIDI routing destinations, DEV-28a2) are drawn disabled with the task ID.

#include "ui/views/ReferenceFace.h"

#include "devices/native/Operator.h"
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
#include <map>

namespace sonora::ui
{
namespace
{
using O = devices::Operator;
using Law = ReferenceFace::Law;
constexpr float kOriginX = 12, kOriginY = 44, kWidth = 1450, kBody = 342;
constexpr float kDisplayX0 = 442, kDisplayY0 = 16, kDisplayX1 = 1032, kDisplayY1 = 380;
constexpr float kFloor = 0.000317f;      // the reference's "-inf dB" (-70 dB)
constexpr float kKnobGain = 0.368f;      // Level / Volume knobs: norm = (gain / max)^0.368 (the capture's -18 dB at 0.362)
constexpr float kGraphGain = 1 / 4.2f;   // envelope displays: level y = gain^(1 / 4.2) (the sampling engine's measured law)
constexpr float kEnvPx = 16.4f;          // envelope display: capture px per e-fold of a stage's time above 1 ms
enum Page { PageA, PageB, PageC, PageD, PageLfo, PageFilter, PagePitch, PageGlobal, kPages };

// The page shown per Operator, kept while the face is rebuilt (a chain rebuild keeps the processor). View state only.
std::map<const devices::DeviceProcessor*, int>& shownPages()
{
    static std::map<const devices::DeviceProcessor*, int> pages;
    return pages;
}

std::string printed(double value, const char* format)
{
    char text[48];
    std::snprintf(text, sizeof(text), format, value);
    return text;
}
double clean(double v, double tiny) { return std::abs(v) < tiny ? 0.0 : v; } // no "-0 %"
std::string percent(double v) { return printed(clean(v, 0.5), "%.0f %%"); }
std::string decibels(double gain)
{
    if (gain <= kFloor) return "-\xE2\x88\x9E dB";
    const double db = clean(20 * std::log10(gain), 0.05);
    return printed(db, std::abs(db) >= 9.95 ? "%.0f dB" : "%.1f dB");
}
std::string milliseconds(double ms, double floor)
{
    // The reference shows its attack floor (0.1 ms) as "0.00 ms".
    if (ms <= floor + 1e-4) return "0.00 ms";
    return ms < 999.5 ? ReferenceFace::significant((float) ms) + " ms" : ReferenceFace::significant((float) (ms / 1000)) + " s";
}
std::string hertz(double hz) { return hz >= 999.5 ? ReferenceFace::significant((float) (hz / 1000)) + " kHz" : ReferenceFace::significant((float) hz) + " Hz"; }
std::string choice(const devices::DeviceProcessor& p, int index, float v)
{
    const auto& choices = p.parameters()[(size_t) index].choices;
    return choices.empty() ? std::string() : choices[(size_t) std::clamp((int) std::lround(v), 0, (int) choices.size() - 1)];
}
// The user waveform's Repeat (ExpansionMode). TODO(DEV-28a): the names after "Off" are not measured yet.
const char* repeatName(int mode)
{
    static const char* names[4] = {"Off", "1", "2", "3"};
    return names[std::clamp(mode, 0, 3)];
}

// Gain law of the Level / Volume knobs and display levels: the floor ("-inf dB") sits at 0.
Law gainLaw(float max, float exponent)
{
    return {[=](float v) { return v <= kFloor ? 0.0f : std::pow(std::clamp(v / max, 0.0f, 1.0f), exponent); },
            [=](float n) { return n <= 0 ? 0.0f : max * std::pow(std::clamp(n, 0.0f, 1.0f), 1.0f / exponent); }};
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
    menu.showMenuAsync(juce::PopupMenu::Options(), [select = std::move(safe)](int index) { if (index > 0) select(index - 1); });
}

// The reference's framed controls: a 2-capture-px dark frame around a fill.
void framedBox(SkCanvas& c, const Theme& t, Rect r, float frame, SkColor fill)
{
    paint::fill(c, r, t.color("ControlContrastFrame"));
    paint::fill(c, r.reduced(frame), fill);
}

// The oscillators' colours (the theme's Operator1..4 = A..D).
SkColor operatorColour(const Theme& t, int op) { return t.color("Operator" + std::to_string(std::clamp(op, 0, 3) + 1)); }
const char* letter(int op) { static const char* letters[4] = {"A", "B", "C", "D"}; return letters[std::clamp(op, 0, 3)]; }

// Filter type glyphs of the type chooser (Lowpass, Highpass, Bandpass, Notch, Morph), about 30 x 14 capture px.
void filterGlyph(SkCanvas& c, Rect r, int type, SkColor ink, float width)
{
    const float x = r.x, y = r.y, w = r.w, h = r.h;
    SkPathBuilder p;
    switch (type)
    {
        case 0: p.moveTo(x, y).lineTo(x + w * 0.72f, y).quadTo(x + w, y, x + w, y + h); break;
        case 1: p.moveTo(x, y + h).quadTo(x, y, x + w * 0.28f, y).lineTo(x + w, y); break;
        case 2: p.moveTo(x, y + h).quadTo(x + w * 0.35f, y, x + w * 0.5f, y).quadTo(x + w * 0.65f, y, x + w, y + h); break;
        case 3: p.moveTo(x, y).lineTo(x + w * 0.35f, y).quadTo(x + w * 0.5f, y, x + w * 0.5f, y + h).quadTo(x + w * 0.5f, y, x + w * 0.65f, y).lineTo(x + w, y); break;
        default: // Morph: low-pass into notch
            p.moveTo(x, y).lineTo(x + w * 0.3f, y).quadTo(x + w * 0.45f, y, x + w * 0.5f, y + h * 0.7f).quadTo(x + w * 0.55f, y, x + w * 0.7f, y)
                .quadTo(x + w, y, x + w, y + h);
            break;
    }
    stroke(c, p, ink, width, true);
}

// Algorithm structure as a small grid: each oscillator's column (may be fractional) and row (0 = carriers, at the bottom).
struct AlgorithmLayout { float col[4]; int row[4]; float cols; int rows; };
AlgorithmLayout algorithmLayout(int algorithm)
{
    AlgorithmLayout l{};
    int depth[4] = {0, 0, 0, 0};
    for (int pass = 0; pass < 4; ++pass) // modulators sit one row above the highest oscillator they modulate
        for (int op = 0; op < 4; ++op)
            for (int m = 0; m < 4; ++m)
                if (O::modulators(algorithm, op) & (1 << m)) depth[m] = std::max(depth[m], depth[op] + 1);
    int rows = 1;
    for (int op = 0; op < 4; ++op) rows = std::max(rows, depth[op] + 1);
    float cols = 1;
    for (int row = 0; row < rows; ++row)
    {
        float next = 0; // the first free column of this row
        for (int op = 0; op < 4; ++op)
        {
            if (depth[op] != row) continue;
            float want = 0;
            int targets = 0;
            for (int target = 0; target < 4; ++target)
                if (O::modulators(algorithm, target) & (1 << op)) { want += l.col[target]; ++targets; }
            want = targets > 0 ? want / (float) targets : next;
            l.col[op] = std::max(want, next);
            next = l.col[op] + 1;
        }
        cols = std::max(cols, next);
    }
    for (int op = 0; op < 4; ++op) l.row[op] = depth[op];
    l.cols = cols;
    l.rows = rows;
    return l;
}

// Draws an algorithm: squares of `size` on a `pitch` grid centred in `r`, connections from modulator to target.
void paintAlgorithm(SkCanvas& c, const Theme& t, Rect r, int algorithm, float size, float pitch, float frame, float alpha, bool lines)
{
    const auto l = algorithmLayout(algorithm);
    const float gw = (l.cols - 1) * pitch + size, gh = (float) (l.rows - 1) * pitch + size;
    const float left = r.centreX() - gw * 0.5f, top = r.centreY() - gh * 0.5f;
    const auto box = [&](int op) {
        return Rect{left + l.col[op] * pitch, top + (float) (l.rows - 1 - l.row[op]) * pitch, size, size};
    };
    const auto fade = [&](SkColor colour) { return alpha >= 1 ? colour : paint::withAlpha(colour, (uint8_t) (255 * alpha)); };
    if (lines)
        for (int op = 0; op < 4; ++op)
            for (int m = 0; m < 4; ++m)
                if (O::modulators(algorithm, op) & (1 << m))
                {
                    const auto from = box(m), to = box(op);
                    paint::line(c, from.centreX(), from.bottom(), to.centreX(), to.y, fade(t.color("SurfaceBackground")), frame);
                }
    for (int op = 0; op < 4; ++op)
    {
        const auto b = box(op);
        paint::fill(c, b, fade(t.color("ControlContrastFrame")));
        paint::fill(c, b.reduced(frame), fade(operatorColour(t, op)));
    }
}

// ---------------------------------------------------------------- widgets
// The reference's device knob: a 38-px ring open at the lower right (the value arc runs clockwise from six o'clock to
// three), a needle from the centre, the label above (18 px left of the centre, 23 for bipolar knobs) and the value to the
// lower right. `dim`: the section is off (grey ring, white value arc), as the reference draws it.
class Dial final : public Knob
{
public:
    Dial(std::string name, Knob::Source source, std::function<float()> unit, std::function<float()> size)
        : Knob(name, source), name_(std::move(name)), source_(std::move(source)), unit_(std::move(unit)), size_(std::move(size))
    {
        tooltip = name_;
    }
    std::function<bool()> dim;
    bool bipolar = false;
    static constexpr float kRingX = 24, kRingY = 50; // ring centre, capture px from the widget's top left
    Rect valueBounds() const override // UI-93: the value at the ring's lower right
    {
        const float u = unit_(), left = (kRingX + (bipolar ? 10 : 8)) * u;
        return {left, (kRingY + 4) * u, std::max(10.0f, width() - left), 24 * u};
    }

    void paint(SkCanvas& c, const Theme& t) override
    {
        const float u = unit_(), norm = std::clamp(source_.get ? source_.get() : 0.0f, 0.0f, 1.0f), size = size_();
        const bool off = dim && dim();
        const float cx = kRingX * u, cy = kRingY * u, thick = 4.2f * u, radius = 19 * u - thick * 0.5f;
        const SkColor ink = t.color(off ? "DeactivatedPoti" : "Poti"), lit = t.color(off ? "ControlDisabled" : "RangeDefault");
        paint::text(c, name_, {cx - (bipolar ? 23 : 18) * u, cy - 50 * u, std::max(width(), gfx::measureText(name_, size) + 2), 24 * u},
                    t.color("ControlForeground"), size);
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
        const float angle = (90 + 270 * norm) * (float) M_PI / 180.0f, reach = 19 * u;
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
            fill.setColor(std::abs(norm - 0.5f) > 0.002f && ! off ? lit : off ? t.color("ControlDisabled") : ink);
            c.drawPath(tri.detach(), fill);
        }
        const auto value = source_.format ? source_.format(source_.get ? source_.get() : 0.0f) : std::string();
        const float left = cx + (bipolar ? 10 : 8) * u;
        paint::text(c, value, {left, cy + 4 * u, std::max(width() - left, gfx::measureText(value, size) + 2), 24 * u}, t.color("ControlForeground"), size);
    }

private:
    std::string name_;
    Knob::Source source_;
    std::function<float()> unit_, size_;
};

// Square switch with an optional label on the light panels (LFO, Filter, Pitch, Fixed): a framed box, lit in `lit`.
class Check final : public Widget
{
public:
    Check(O* p, int index, std::string label, float box, std::function<float()> unit, std::function<float()> size)
        : p_(p), index_(index), label_(std::move(label)), box_(box), unit_(std::move(unit)), size_(std::move(size))
    {
        tooltip = p_->parameters()[(size_t) index_].name;
    }
    void paint(SkCanvas& c, const Theme& t) override
    {
        const float u = unit_(), b = box_ * u;
        const Rect square{0, (height() - b) * 0.5f, b, b};
        const bool on = p_->getParameter(index_) > 0.5f;
        framedBox(c, t, square, 2 * u, on ? t.color("RangeDefault") : t.color("ControlBackground"));
        if (! label_.empty()) paint::text(c, label_, {b + 6 * u, 0, width() - b - 6 * u, height()}, t.color("ControlForeground"), size_());
    }
    void mouseUp(const MouseEvent& e) override
    {
        if (e.right || ! localBounds().contains(e.x, e.y)) return;
        p_->setParameter(index_, p_->getParameter(index_) > 0.5f ? 0.0f : 1.0f);
        if (parent()) parent()->layout();
        repaint();
    }

private:
    O* p_;
    int index_;
    std::string label_;
    float box_;
    std::function<float()> unit_, size_;
};

// Framed text button on the light panels (12 / 24, the LFO's R): amber when on.
class PanelButton final : public Widget
{
public:
    PanelButton(std::string label, bool bold, std::function<float()> unit, std::function<float()> size)
        : label_(std::move(label)), bold_(bold), unit_(std::move(unit)), size_(std::move(size)) {}
    std::function<bool()> on, dim;
    std::function<void()> click;
    void paint(SkCanvas& c, const Theme& t) override
    {
        // While its section is off a lit button turns white, as the reference draws the LFO's R.
        const bool off = dim && dim();
        framedBox(c, t, localBounds(), 2 * unit_(), on() ? t.color(off ? "ControlDisabled" : "ChosenDefault") : t.color("ControlTextBack"));
        paint::text(c, label_, localBounds(), t.color("ControlForeground"), bold_ ? size_() * 0.82f : size_(), Align::Centre, bold_);
    }
    void mouseUp(const MouseEvent& e) override
    {
        if (e.right || ! localBounds().contains(e.x, e.y)) return;
        click();
        if (parent()) parent()->layout();
        repaint();
    }

private:
    std::string label_;
    bool bold_;
    std::function<float()> unit_, size_;
};

// The light drop-down field (LFO wave / range, filter type / circuit): text 6 px in, the arrow 13 px from the right edge;
// `glyph` draws the choice instead of its text (the filter type).
class LightChooser final : public Widget
{
public:
    LightChooser(O* p, int index, std::function<float()> unit, std::function<float()> size) : p_(p), index_(index), unit_(std::move(unit)), size_(std::move(size))
    {
        tooltip = p_->parameters()[(size_t) index_].name;
    }
    std::function<void(SkCanvas&, Rect, int, SkColor)> glyph;
    void paint(SkCanvas& c, const Theme& t) override
    {
        const float u = unit_();
        framedBox(c, t, localBounds(), 2 * u, t.color("ControlTextBack"));
        const auto& choices = p_->parameters()[(size_t) index_].choices;
        const int v = current();
        const auto ink = t.color("ControlForeground");
        if (glyph) glyph(c, {8 * u, height() * 0.5f - 7 * u, 30 * u, 14 * u}, v, ink);
        else if (v >= 0 && (size_t) v < choices.size())
            paint::text(c, choices[(size_t) v], {5 * u, 0, std::max(width() - 22 * u, gfx::measureText(choices[(size_t) v], size_()) + 2), height()}, ink, size_());
        ctl::triangle(c, width() - 13 * u, height() * 0.5f, 14 * u, 10 * u, ctl::Dir::Down, ink);
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
        if (parent()) parent()->layout();
        repaint();
        return true;
    }

private:
    int current() const { return (int) std::lround(p_->getParameter(index_)); }
    O* p_;
    int index_;
    std::function<float()> unit_, size_;
};

// The oscillator's coloured letter: switches the oscillator on and off (unlit grey when off).
class LetterSquare final : public Widget
{
public:
    LetterSquare(O* p, int op, std::function<float()> unit) : p_(p), op_(op), unit_(std::move(unit))
    {
        tooltip = std::string("Oscillator ") + letter(op) + " on/off";
    }
    void paint(SkCanvas& c, const Theme& t) override
    {
        const float u = unit_();
        const bool on = p_->getParameter(O::opParam(op_, O::OpOn)) > 0.5f;
        framedBox(c, t, localBounds(), 2 * u, on ? operatorColour(t, op_) : t.color("ControlBackground"));
        paint::text(c, letter(op_), localBounds(), on ? t.color("ControlForeground") : t.color("TextDisabled"), 17 * u, Align::Centre, true);
    }
    void mouseUp(const MouseEvent& e) override
    {
        if (e.right || ! localBounds().contains(e.x, e.y)) return;
        const int index = O::opParam(op_, O::OpOn);
        p_->setParameter(index, p_->getParameter(index) > 0.5f ? 0.0f : 1.0f);
        repaint();
    }

private:
    O* p_;
    int op_;
    std::function<float()> unit_;
};

// The Global panel's algorithm icon (the oscillators' squares in the algorithm's structure); a click lists the algorithms.
class AlgorithmIcon final : public Widget
{
public:
    AlgorithmIcon(O* p, std::function<float()> unit) : p_(p), unit_(std::move(unit)) { tooltip = "Algorithm"; }
    void paint(SkCanvas& c, const Theme& t) override
    {
        const float u = unit_();
        // 13-px squares on an 18-px pitch (live-operator-fixed-alg11.png: Alg. 11 is a row of four, Alg. 1 a column).
        paintAlgorithm(c, t, {0, 0, width(), height()}, current(), 13 * u, 18 * u, 2 * u, 1, false);
    }
    void mouseUp(const MouseEvent& e) override
    {
        if (e.right || ! localBounds().contains(e.x, e.y)) return;
        choose(this, p_->parameters()[(size_t) O::Algorithm].choices, current(), [this](int pick) {
            p_->setParameter(O::Algorithm, (float) pick);
            repaint();
        });
    }
    bool mouseWheel(const MouseEvent& e) override
    {
        if (e.right || std::abs(e.wheelDeltaX) > std::abs(e.wheelDeltaY)) return false;
        p_->setParameter(O::Algorithm, (float) std::clamp(current() + (e.wheelDeltaY > 0 ? -1 : 1), 0, O::kAlgorithms - 1));
        repaint();
        return true;
    }

private:
    int current() const { return std::clamp((int) std::lround(p_->getParameter(O::Algorithm)), 0, O::kAlgorithms - 1); }
    O* p_;
    std::function<float()> unit_;
};

// A label and its value on the display: the grey label above, the value below in `ink` (cyan envelope values, amber
// oscillator values, grey while inactive). Drag vertically (Shift fine), wheel, double-click resets; choices open their
// list (with the small arrow), switches toggle. `enabled` false: drawn grey, no input, the tooltip says why.
class Readout final : public Widget
{
public:
    Readout(O* p, int index, std::string label, Law law, std::function<std::string(float)> format, std::function<float()> unit,
            std::function<float()> size)
        : p_(p), index_(index), label_(std::move(label)), law_(std::move(law)), format_(std::move(format)), unit_(std::move(unit)), size_(std::move(size))
    {
        tooltip = label_;
    }
    std::function<SkColor(const Theme&)> ink;
    std::function<bool()> enabled;
    float valueX = 4, arrowX = 0; // capture px from the widget's left; arrowX > 0 draws the choice arrow there
    bool toggle = false;

    bool isOn() const { return ! enabled || enabled(); }
    void paint(SkCanvas& c, const Theme& t) override
    {
        const float u = unit_(), size = size_();
        const auto grey = t.color("SurfaceBackground");
        if (! label_.empty()) paint::text(c, label_, {0, 0, std::max(width(), gfx::measureText(label_, size) + 2), 24 * u}, grey, size);
        const auto colour = isOn() && ink ? ink(t) : grey;
        const auto text = format_(p_->getParameter(index_));
        paint::text(c, text, {valueX * u, 29 * u, std::max(width() - valueX * u, gfx::measureText(text, size) + 2), 24 * u}, colour, size);
        if (arrowX > 0) ctl::triangle(c, arrowX * u, 42 * u, 10 * u, 6 * u, ctl::Dir::Down, colour);
    }
    void mouseDown(const MouseEvent& e) override
    {
        pressed_ = ! e.right && isOn();
        startY_ = e.y;
        start_ = norm();
        moved_ = false;
    }
    void mouseDrag(const MouseEvent& e) override
    {
        if (! pressed_ || choices() || toggle) return;
        moved_ = moved_ || std::abs(e.y - startY_) > 2;
        set(start_ + (startY_ - e.y) / (e.shift ? 1000.0f : 200.0f));
    }
    void mouseUp(const MouseEvent& e) override
    {
        if (! pressed_) return;
        pressed_ = false;
        if (moved_ || ! localBounds().contains(e.x, e.y)) return;
        if (toggle) { p_->setParameter(index_, p_->getParameter(index_) > 0.5f ? 0.0f : 1.0f); repaint(); return; }
        if (! choices()) return;
        choose(this, p_->parameters()[(size_t) index_].choices, (int) std::lround(p_->getParameter(index_)), [this](int pick) {
            p_->setParameter(index_, (float) pick);
            if (parent()) parent()->layout();
            repaint();
        });
    }
    void mouseDoubleClick(const MouseEvent& e) override
    {
        if (e.right || ! isOn() || toggle) return;
        p_->setParameter(index_, p_->parameters()[(size_t) index_].defaultValue);
        repaint();
    }
    bool mouseWheel(const MouseEvent& e) override
    {
        if (e.right || ! isOn() || std::abs(e.wheelDeltaX) > std::abs(e.wheelDeltaY)) return false;
        const auto& info = p_->parameters()[(size_t) index_];
        if (choices() || toggle)
            p_->setParameter(index_, std::clamp(std::round(p_->getParameter(index_)) + (e.wheelDeltaY > 0 ? 1.0f : -1.0f), info.minValue, info.maxValue));
        else set(norm() + (e.wheelDeltaY > 0 ? 1.0f : -1.0f) * (e.shift ? 0.002f : 0.02f));
        if (parent()) parent()->layout();
        repaint();
        return true;
    }

private:
    bool choices() const { return ! p_->parameters()[(size_t) index_].choices.empty(); }
    float norm() const { return std::clamp(law_.toNorm(p_->getParameter(index_)), 0.0f, 1.0f); }
    void set(float n)
    {
        const auto& info = p_->parameters()[(size_t) index_];
        float v = std::clamp(law_.fromNorm(std::clamp(n, 0.0f, 1.0f)), info.minValue, info.maxValue);
        if (! info.choices.empty() || info.isToggle) v = std::round(v);
        p_->setParameter(index_, v);
        repaint();
    }
    O* p_;
    int index_;
    std::string label_;
    Law law_;
    std::function<std::string(float)> format_;
    std::function<float()> unit_, size_;
    float startY_ = 0, start_ = 0;
    bool pressed_ = false, moved_ = false;
};

// Small square switch on the display (R retrigger, Q quantise, Dest A targets): amber filled when on, an amber frame when
// off. `enabled` false: grey.
class DisplayButton final : public Widget
{
public:
    DisplayButton(O* p, int index, std::string label, std::function<float()> unit, std::function<float()> size)
        : p_(p), index_(index), label_(std::move(label)), unit_(std::move(unit)), size_(std::move(size))
    {
        tooltip = p_->parameters()[(size_t) index_].name;
    }
    std::function<bool()> enabled;
    void paint(SkCanvas& c, const Theme& t) override
    {
        const float u = unit_();
        const bool on = p_->getParameter(index_) > 0.5f, active = ! enabled || enabled();
        const auto ink = active ? t.color("ChosenDefault") : t.color("SurfaceBackground");
        paint::fill(c, localBounds(), ink);
        if (! on) paint::fill(c, localBounds().reduced(2 * u), t.color("RetroDisplayBackground"));
        paint::text(c, label_, localBounds(), on ? t.color("RetroDisplayBackground") : ink, size_() * (label_.size() == 1 ? 0.82f : 0.8f), Align::Centre,
                    label_.size() == 1);
    }
    void mouseUp(const MouseEvent& e) override
    {
        if (e.right || ! localBounds().contains(e.x, e.y) || (enabled && ! enabled())) return;
        p_->setParameter(index_, p_->getParameter(index_) > 0.5f ? 0.0f : 1.0f);
        repaint();
    }

private:
    O* p_;
    int index_;
    std::string label_;
    std::function<float()> unit_, size_;
};

// The top of the display, painted over the device's title band (the display starts at y 16, the body at 44). It paints
// with the face's own display code and passes its mouse events to the face.
class DisplayCap final : public Widget
{
public:
    Widget* face = nullptr;
    std::function<void(SkCanvas&, const Theme&)> paintDisplay;
    void paint(SkCanvas& c, const Theme& t) override
    {
        if (! Widget::isAlive(face)) return;
        c.save();
        c.translate(face->bounds().x - bounds().x, face->bounds().y - bounds().y);
        paintDisplay(c, t);
        c.restore();
    }
    void mouseDown(const MouseEvent& e) override { if (Widget::isAlive(face)) face->mouseDown(toFace(e)); }
    void mouseDrag(const MouseEvent& e) override { if (Widget::isAlive(face)) face->mouseDrag(toFace(e)); }
    void mouseUp(const MouseEvent& e) override { if (Widget::isAlive(face)) face->mouseUp(toFace(e)); }

private:
    MouseEvent toFace(MouseEvent e) const
    {
        e.x += bounds().x - face->bounds().x;
        e.y += bounds().y - face->bounds().y;
        return e;
    }
};

// ---------------------------------------------------------------- the face
class OperatorFace final : public ReferenceFace
{
public:
    OperatorFace(Widget& box, O* synth, Tag tagger) : ReferenceFace(synth, kWidth, kOriginX, kOriginY, std::move(tagger), kBody), synth_(synth)
    {
        auto& pages = shownPages();
        if (const auto found = pages.find(synth); found != pages.end()) page_ = found->second;
        for (int op = 0; op < O::kOperators; ++op) buildStrip(op);
        buildPanels();
        for (int op = 0; op < O::kOperators; ++op) buildOscillatorPage(op);
        buildEnvelopePage(PageLfo);
        buildEnvelopePage(PageFilter);
        buildEnvelopePage(PagePitch);
        buildGlobalPage();
        cap_ = box.add<DisplayCap>();
        cap_->face = this;
        cap_->paintDisplay = [this](SkCanvas& c, const Theme& t) { paintDisplay(c, t); };
        cap_->tooltip = "Operator display";
    }
    ~OperatorFace() override
    {
        // The cap lives in the device box and dies with it; it must not reach this face meanwhile.
        if (Widget::isAlive(cap_)) cap_->visible = false;
    }

    int page() const { return page_; }
    Rect graphBounds() const override { return at(450, 24, 1024, 160); }

    void layout() override
    {
        ReferenceFace::layout();
        if (Widget::isAlive(cap_))
        {
            const auto b = bounds();
            const float top = std::max(0.0f, b.y - (kOriginY - kDisplayY0) * unit());
            cap_->setBounds({b.x + column(kDisplayX0), top, column(kDisplayX1) - column(kDisplayX0), b.y - top});
            cap_->visible = visible;
        }
    }

    void paint(SkCanvas& c, const Theme& t) override
    {
        fillBody(c, t, true);
        for (int op = 0; op < O::kOperators; ++op)
        {
            const float top = stripTop(op);
            if (page_ != op) panelLeft(c, t, top, op == 0 ? 386 : top + 80);
            label(c, t, "Fixed", 229, top + (op == 0 ? 5 : 3), 290, top + (op == 0 ? 27 : 25), Align::Left);
        }
        for (int i = 0; i < 4; ++i)
            if (page_ != PageLfo + i) panelRight(c, t, panelTop(i), i == 3 ? 386 : panelTop(i) + 80);
        // The pitch envelope glyph above the Pitch switch.
        SkPathBuilder glyph;
        const auto a = at(1067, 244, 1067, 244), b = at(1067, 235, 1067, 235), e = at(1098, 235, 1098, 235);
        glyph.moveTo(a.x, a.y).lineTo(b.x, b.y).lineTo(e.x, e.y);
        stroke(c, glyph, t.color("ControlForeground"), 2.2f * unit(), false);
        paintDisplay(c, t);
    }

    void mouseDown(const MouseEvent& e) override
    {
        drag_ = Drag::None;
        if (e.right) return;
        const float x = e.x / unit() + originX_, y = e.y / unit() + originY_;
        if (x < kDisplayX0 || x >= kDisplayX1)
        {
            if (const int p = pageAt(x, y); p >= 0) select(p);
            return;
        }
        // Section header squares: the oscillator page's Oscillator square shows its partials in the graph.
        if (page_ <= PageD && y > 164 && y < 190)
        {
            if (x > 444 && x < 470) harmonics_ = false;
            else if (x > 820 && x < 846) harmonics_ = true;
            repaint();
            return;
        }
        if (page_ == PageGlobal)
        {
            for (int a = 0; a < O::kAlgorithms; ++a)
                if (algorithmCell(a).contains(x, y)) { synth_->setParameter(O::Algorithm, (float) a); repaint(); return; }
            return;
        }
        if (page_ <= PageD && harmonics_)
        {
            if (y >= 24 && y <= 160) { drag_ = Drag::Harmonic; mouseDrag(e); }
            return;
        }
        if (page_ == PageFilter && x >= 740 && y < 164) { drag_ = Drag::Filter; mouseDrag(e); return; }
        const auto g = geometry();
        float best = 14;
        const float hx[4] = {g.x[0], g.x[1], g.x[2], g.x[4]}, hy[4] = {g.y[0], g.y[1], g.y[2], g.y[4]};
        for (int h = 0; h < 4; ++h)
            if (const float d = std::hypot(x - hx[h], y - hy[h]); d < best) { best = d; drag_ = (Drag) ((int) Drag::Initial + h); }
        if (drag_ != Drag::None) return;
        // The middle of the attack, decay or release segment: drag vertically to bend it (its slope). The reference
        // draws no handle there until hovered.
        const int first = envelopeFirst(), slopes[3] = {O::AttackSlope, O::DecaySlope, O::ReleaseSlope}, from[3] = {0, 1, 3};
        for (int s = 0; s < 3; ++s)
        {
            const int i = from[s];
            if (g.x[i + 1] - g.x[i] < 8) continue;
            const float mx = (g.x[i] + g.x[i + 1]) * 0.5f, my = g.y[i] + (g.y[i + 1] - g.y[i]) * O::slopeCurve(0.5f, value(first + slopes[s]));
            if (std::abs(x - mx) < (g.x[i + 1] - g.x[i]) * 0.3f && std::abs(y - my) < 12)
            {
                drag_ = Drag::Slope;
                slope_ = first + slopes[s];
                startY_ = y;
                startSlope_ = value(slope_);
                return;
            }
        }
    }
    void mouseDrag(const MouseEvent& e) override
    {
        const float x = e.x / unit() + originX_, y = e.y / unit() + originY_;
        const int first = envelopeFirst();
        const auto g = geometry();
        const auto time = [&](int field, float from) {
            const auto& info = synth_->parameters()[(size_t) (first + field)];
            const float px = x - from;
            const float ms = px <= 0 ? info.minValue : std::exp(px / g.k);
            set(first + field, std::clamp(ms, info.minValue, info.maxValue));
        };
        const auto level = [&](int field) { set(first + field, levelAt(y)); };
        switch (drag_)
        {
            case Drag::None: return;
            case Drag::Initial: level(O::AttackLevel); break;
            case Drag::Peak: time(O::AttackTime, g.x[0]); level(O::DecayLevel); break;
            case Drag::Decay: time(O::DecayTime, g.x[1]); level(O::SustainLevel); break;
            case Drag::Release: time(O::ReleaseTime, g.x[3]); level(O::ReleaseLevel); break;
            case Drag::Filter:
            {
                const float f = std::clamp((x - 750) / 266, 0.0f, 1.0f);
                set(O::FilterFreq, 30.0f * std::pow(18500.0f / 30.0f, f));
                set(O::FilterRes, std::clamp((140 - y) / 100, 0.0f, 1.0f) * 1.25f);
                break;
            }
            case Drag::Slope:
            {
                // Pulling the middle towards the stage's end level bends the curve that way (both directions read as
                // "towards the later level" for rising and falling stages alike).
                set(slope_, std::clamp(startSlope_ + (y - startY_) / 60.0f, -1.0f, 1.0f));
                break;
            }
            case Drag::Harmonic:
            {
                const int op = page_, count = harmonicCount(op);
                const int h = std::clamp((int) ((x - 450) / (574.0f / (float) count)), 0, count - 1);
                if (std::lround(synth_->getParameter(O::opParam(op, O::OpWave))) != O::kWaveforms - 1)
                    set(O::opParam(op, O::OpWave), (float) (O::kWaveforms - 1)); // drawing partials makes the wave User
                set(O::harmonicParam(op, h), std::clamp((156 - y) / 128, 0.0f, 1.0f));
                break;
            }
        }
        repaint();
    }
    void mouseUp(const MouseEvent&) override { drag_ = Drag::None; }

    void descendantPressed(Widget* leaf, const MouseEvent&) override
    {
        // Touching a control of a strip or panel selects it, as a click on its background does.
        for (Widget* w = leaf; w != nullptr && w != this; w = w->parent())
            if (w->parent() == this)
            {
                const auto r = w->bounds();
                const float x = r.centreX() / unit() + originX_, y = r.centreY() / unit() + originY_;
                if (x < kDisplayX0 || x >= kDisplayX1)
                    if (const int p = pageAt(x, y); p >= 0 && p != page_) select(p);
                return;
            }
    }

private:
    enum class Drag { None, Initial, Peak, Decay, Release, Filter, Harmonic, Slope };
    // Envelope display geometry: stage starts / ends (x[0] start, x[1] attack end, x[2] decay end, x[3] release start,
    // x[4] release end), levels, and the capture px per e-fold of a stage time.
    struct Geometry { float x[5], y[5], k; };

    float value(int p) const { return synth_->getParameter(p); }
    void set(int p, float v)
    {
        const auto& info = synth_->parameters()[(size_t) p];
        synth_->setParameter(p, std::clamp(v, info.minValue, info.maxValue));
    }
    bool on(int p) const { return value(p) > 0.5f; }
    std::function<float()> unitFn() { return [this] { return unit(); }; }
    std::function<float()> sizeFn() { return [this] { return text(); }; }
    static float stripTop(int op) { return 44 + 86.0f * (float) (3 - op); } // D at the top
    static float panelTop(int i) { return 44 + 86.0f * (float) i; }          // LFO, Filter, Pitch, Global
    int pageAt(float x, float y) const
    {
        if (y < 44 || y > 386) return -1;
        const int row = std::clamp((int) ((y - 44) / 86), 0, 3);
        if (x >= 12 && x < kDisplayX0) return 3 - row;
        if (x >= kDisplayX1 && x < 1462) return PageLfo + row;
        return -1;
    }
    void select(int page)
    {
        page_ = page;
        shownPages()[synth_] = page;
        layout();
        repaint();
    }
    template <typename W> W* placed(W* widget, float x0, float y0, float x1, float y1, int page = -1, std::function<bool()> when = {})
    {
        place(widget, x0, y0, x1, y1);
        if (page >= 0 || when) showWhen(widget, [this, page, when] { return (page < 0 || page_ == page) && (! when || when()); });
        return widget;
    }

    // Parameters the reference steps in whole units (Coarse, Fine, Osc<Vel, semitones).
    static bool integral(int p)
    {
        if (p < O::GlobalFirst)
        {
            const int field = p % O::kOpFields;
            return field == O::OpCoarse || field == O::OpFine || field == O::OpVelCoarse;
        }
        return p == O::Transpose || p == O::PitchBendRange;
    }
    Law lawFor(int p) const
    {
        const auto& info = synth_->parameters()[(size_t) p];
        if (info.unit == "lin") return gainLaw(info.maxValue, kKnobGain);
        if ((info.unit == "ms" || info.unit == "Hz") && info.minValue > 0) return Law::logarithmic(info.minValue, info.maxValue);
        return Law::linear(info.minValue, info.maxValue);
    }
    std::function<std::string(float)> formatFn(int p) { return [this, p](float v) { return operatorValueText(*synth_, p, v); }; }

    // ---- builders
    Dial* dial(int p, std::string name, float cx, float cy, std::function<bool()> dim = {}, bool bipolar = false, int page = -1,
               std::function<bool()> when = {})
    {
        Knob::Source s;
        const auto law = lawFor(p);
        s.minValue = 0;
        s.maxValue = 1;
        s.get = [this, p, law] { return std::clamp(law.toNorm(value(p)), 0.0f, 1.0f); };
        s.set = [this, p, law](float n) {
            const auto& info = synth_->parameters()[(size_t) p];
            const float v = law.fromNorm(std::clamp(n, 0.0f, 1.0f));
            set(p, info.choices.empty() && ! info.isToggle && ! integral(p) ? v : std::round(v));
        };
        s.defaultValue = std::clamp(law.toNorm(synth_->parameters()[(size_t) p].defaultValue), 0.0f, 1.0f);
        s.format = [this, p, law](float n) { // UI-93: the text at any position
            const auto& info = synth_->parameters()[(size_t) p];
            const float v = std::clamp(law.fromNorm(std::clamp(n, 0.0f, 1.0f)), info.minValue, info.maxValue);
            return operatorValueText(*synth_, p, info.choices.empty() && ! info.isToggle && ! integral(p) ? v : std::round(v));
        };
        auto* widget = add<Dial>(std::move(name), std::move(s), unitFn(), sizeFn());
        widget->style = Knob::Style::Device;
        widget->dim = std::move(dim);
        widget->bipolar = bipolar;
        tag(widget, p);
        return placed(widget, cx - Dial::kRingX, cy - Dial::kRingY, cx + 92, cy + 28, page, std::move(when));
    }
    Readout* readout(int p, std::string name, float x0, float y0, float x1, int page, std::function<SkColor(const Theme&)> ink, std::function<bool()> when = {})
    {
        auto* widget = add<Readout>(synth_, p, std::move(name), lawFor(p), formatFn(p), unitFn(), sizeFn());
        widget->ink = std::move(ink);
        widget->toggle = synth_->parameters()[(size_t) p].isToggle;
        tag(widget, p);
        return placed(widget, x0, y0, x1, y0 + 54, page, std::move(when));
    }
    // A control the engine does not drive yet: drawn grey, no input, not mappable.
    Readout* inertReadout(int p, std::string name, float x0, float y0, float x1, int page, float arrowX = 0)
    {
        auto* widget = add<Readout>(synth_, p, std::move(name), lawFor(p), formatFn(p), unitFn(), sizeFn());
        widget->enabled = [] { return false; };
        widget->arrowX = arrowX;
        widget->tooltip = "Not available yet: the reference's modulation destinations are not identified (DEV-28a2)";
        return placed(widget, x0, y0, x1, y0 + 54, page);
    }
    DisplayButton* displayButton(int p, std::string label, float x0, float y0, float x1, float y1, int page, std::function<bool()> enabled = {})
    {
        auto* widget = add<DisplayButton>(synth_, p, std::move(label), unitFn(), sizeFn());
        widget->enabled = std::move(enabled);
        tag(widget, p);
        return placed(widget, x0, y0, x1, y1, page);
    }
    Check* check(int p, std::string label, float x0, float y0, float x1, float y1, float box, int page = -1, std::function<bool()> when = {})
    {
        auto* widget = add<Check>(synth_, p, std::move(label), box, unitFn(), sizeFn());
        tag(widget, p);
        return placed(widget, x0, y0, x1, y1, page, std::move(when));
    }
    LightChooser* lightChooser(int p, float x0, float y0, float x1, float y1, std::function<bool()> when = {})
    {
        auto* widget = add<LightChooser>(synth_, p, unitFn(), sizeFn());
        tag(widget, p);
        return placed(widget, x0, y0, x1, y1, -1, std::move(when));
    }
    PanelButton* panelButton(int p, std::string label, bool bold, float x0, float y0, float x1, float y1, std::function<bool()> isOn, std::function<void()> click,
                             std::function<bool()> dim = {})
    {
        auto* widget = add<PanelButton>(std::move(label), bold, unitFn(), sizeFn());
        widget->on = std::move(isOn);
        widget->dim = std::move(dim);
        widget->click = std::move(click);
        widget->tooltip = synth_->parameters()[(size_t) p].name;
        tag(widget, p);
        return placed(widget, x0, y0, x1, y1);
    }

    // ---- left: oscillator strips (Coarse 43, Fine 161, Fixed box 242..260, Level 311, letter 394..420; knobs 53 px
    // below the strip's top)
    void buildStrip(int op)
    {
        // The bottom strip is 84 px tall (to the body's end) and its controls sit 2 px lower than in the others.
        const float top = stripTop(op) + (op == 0 ? 2 : 0), cy = top + 53;
        const auto off = [this, op] { return ! on(O::opParam(op, O::OpOn)); };
        const auto fixed = [this, op] { return on(O::opParam(op, O::OpFixed)); };
        const auto notFixed = [this, op] { return ! on(O::opParam(op, O::OpFixed)); };
        dial(O::opParam(op, O::OpCoarse), "Coarse", 43, cy, off, false, -1, notFixed);
        dial(O::opParam(op, O::OpFine), "Fine", 161, cy, off, false, -1, notFixed);
        dial(O::opParam(op, O::OpFixedFreq), "Freq", 43, cy, off, false, -1, fixed);
        dial(O::opParam(op, O::OpFixedMulti), "Multi", 161, cy, off, false, -1, fixed);
        check(O::opParam(op, O::OpFixed), "", 242, top + 42, 260, top + 60, 18);
        dial(O::opParam(op, O::OpVolume), "Level", 311, cy, off);
        auto* square = add<LetterSquare>(synth_, op, unitFn());
        tag(square, O::opParam(op, O::OpOn));
        placed(square, 394, top + 28, 420, top + 54);
    }

    // ---- right: LFO, Filter, Pitch and Global panels (knobs at x 1155 / 1263 / 1377)
    void buildPanels()
    {
        const auto lfoOff = [this] { return ! on(O::LfoOn); };
        check(O::LfoOn, "LFO", 1054, 54, 1150, 80, 26);
        lightChooser(O::LfoType, 1054, 88, 1152, 114);
        lightChooser(O::LfoRange, 1158, 88, 1196, 114);
        panelButton(O::LfoRetrigger, "R", true, 1202, 88, 1228, 114, [this] { return on(O::LfoRetrigger); },
                    [this] { set(O::LfoRetrigger, on(O::LfoRetrigger) ? 0.0f : 1.0f); }, lfoOff);
        const auto synced = [this] { return std::lround(value(O::LfoRange)) == 2; };
        dial(O::LfoRate, "Rate", 1263, 97, lfoOff, false, -1, [synced] { return ! synced(); });
        dial(O::LfoRateSynced, "Rate", 1263, 97, lfoOff, false, -1, synced);
        dial(O::LfoAmount, "Amount", 1377, 97, lfoOff);

        const auto filterOff = [this] { return ! on(O::FilterOn); };
        check(O::FilterOn, "Filter", 1054, 140, 1142, 166, 26);
        lightChooser(O::FilterType, 1148, 140, 1228, 166)->glyph = [this](SkCanvas& c, Rect r, int type, SkColor ink) {
            filterGlyph(c, r, type, ink, 2.4f * unit());
        };
        panelButton(O::FilterSlope, "12", false, 1054, 174, 1092, 200, [this] { return ! on(O::FilterSlope); }, [this] { set(O::FilterSlope, 0); }, filterOff);
        panelButton(O::FilterSlope, "24", false, 1098, 174, 1136, 200, [this] { return on(O::FilterSlope); }, [this] { set(O::FilterSlope, 1); }, filterOff);
        const auto lpHp = [this] { return std::lround(value(O::FilterType)) <= 1; };
        lightChooser(O::FilterCircuitLpHp, 1148, 174, 1228, 200, lpHp);
        lightChooser(O::FilterCircuitBpNoMo, 1148, 174, 1228, 200, [lpHp] { return ! lpHp(); });
        dial(O::FilterFreq, "Freq", 1263, 183, filterOff);
        dial(O::FilterRes, "Res", 1377, 183, filterOff);

        check(O::PitchEnvOn, "", 1054, 250, 1080, 276, 26);
        dial(O::PitchEnvAmount, "Pitch Env", 1155, 269, [this] { return ! on(O::PitchEnvOn); }, true);
        dial(O::Spread, "Spread", 1263, 269);
        dial(O::Transpose, "Transpose", 1375, 269, {}, true);

        auto* icon = add<AlgorithmIcon>(synth_, unitFn());
        tag(icon, O::Algorithm);
        placed(icon, 1052, 306, 1126, 382);
        dial(O::EnvTime, "Time", 1155, 357, {}, true);
        dial(O::Tone, "Tone", 1263, 357);
        dial(O::Volume, "Volume", 1377, 357);
    }

    // ---- display rows: label tops at y 194 / 254 / 315 (labels centred 12 px below, values 41 px below)
    static constexpr float kRows[3] = {194, 254, 315};
    std::function<SkColor(const Theme&)> cyanWhen(std::function<bool()> active)
    {
        return [active](const Theme& t) { return ! active || active() ? t.color("RangeDefault") : t.color("SurfaceBackground"); };
    }
    std::function<SkColor(const Theme&)> amberWhen(std::function<bool()> active)
    {
        return [active](const Theme& t) { return ! active || active() ? t.color("ChosenDefault") : t.color("SurfaceBackground"); };
    }
    // The envelope's Loop chooser and, while it loops, Repeat (Beat / Sync) or the loop time (Loop / Trigger).
    void loopRow(int first, int page, std::function<SkColor(const Theme&)> ink)
    {
        auto* loop = readout(first + O::LoopMode, "Loop", 453, kRows[2], 549, page, ink);
        loop->arrowX = 84;
        const auto mode = [this, first] { return (int) std::lround(value(first + O::LoopMode)); };
        auto* repeat = readout(first + O::RepeatTime, "Repeat", 553, kRows[2], 635, page, ink, [mode] { return mode() == 2 || mode() == 3; });
        repeat->arrowX = 70;
        readout(first + O::LoopTime, "Time", 553, kRows[2], 635, page, ink, [mode] { return mode() == 1 || mode() == 4; });
    }

    // ---- the oscillator page (the capture): Envelope columns x 453 / 553 / 639 / 724, Oscillator columns 828 / 928
    void buildOscillatorPage(int op)
    {
        const int env = O::envParam(op, 0);
        const auto active = [this, op] { return on(O::opParam(op, O::OpOn)); };
        const auto cyan = cyanWhen(active), amber = amberWhen(active);
        const char* stages[3][3] = {{"Attack", "Decay", "Release"}, {"Initial", "Peak", "Sustain"}, {}};
        const int fields[2][3] = {{O::AttackTime, O::DecayTime, O::ReleaseTime}, {O::AttackLevel, O::DecayLevel, O::SustainLevel}};
        const float columns[3][2] = {{453, 551}, {553, 637}, {639, 722}};
        for (int row = 0; row < 2; ++row)
            for (int col = 0; col < 3; ++col)
                readout(env + fields[row][col], stages[row][col], columns[col][0], kRows[row], columns[col][1], op, cyan);
        readout(env + O::TimeVelScale, "Time<Vel", 724, kRows[0], 818, op, cyan);
        readout(O::opParam(op, O::OpVelScale), "Vel", 724, kRows[1], 818, op, cyan);
        loopRow(env, op, cyan);
        readout(O::opParam(op, O::OpKeyScale), "Key", 724, kRows[2], 818, op, cyan);

        // Oscillator: Wave (amber, its picture to the right), Feedback (active only where no oscillator modulates this
        // one), Repeat (the User wave's expansion), Phase with R, Osc<Vel with Q.
        auto* wave = readout(O::opParam(op, O::OpWave), "Wave", 828, kRows[0], 925, op, amber);
        wave->arrowX = 71;
        readout(O::opParam(op, O::OpFeedback), "Feedback", 828, kRows[1], 925, op,
                amberWhen([this, op, active] { return active() && O::modulators((int) std::lround(value(O::Algorithm)), op) == 0; }));
        auto* repeat = readout(O::expansionParam(op), "Repeat", 928, kRows[1], 1022, op,
                               amberWhen([this, op, active] { return active() && std::lround(value(O::opParam(op, O::OpWave))) == O::kWaveforms - 1; }));
        repeat->arrowX = 71;
        readout(O::opParam(op, O::OpPhase), "Phase", 828, kRows[2], 925, op, amber)->valueX = 33;
        displayButton(O::opParam(op, O::OpRetrigger), "R", 828, 344, 850, 366, op);
        readout(O::opParam(op, O::OpVelCoarse), "Osc<Vel", 928, kRows[2], 1022, op, amber);
        displayButton(O::opParam(op, O::OpVelCoarseQuantized), "Q", 990, 344, 1012, 366, op);
    }

    // ---- LFO, Filter and Pitch pages (laid out from the manual, awaiting a capture, DEV-28a): Envelope columns 453 /
    // 553 / 639, the page's own section at 724 / 828 / 928.
    void buildEnvelopePage(int page)
    {
        const int env = O::otherEnvParam(page == PageLfo ? O::EnvLfo : page == PageFilter ? O::EnvFilter : O::EnvPitch, 0);
        const int onParam = page == PageLfo ? O::LfoOn : page == PageFilter ? O::FilterOn : O::PitchEnvOn;
        const auto active = [this, onParam] { return on(onParam); };
        const auto cyan = cyanWhen(active), amber = amberWhen(active);
        const char* stages[2][3] = {{"Attack", "Decay", "Release"}, {"Initial", "Peak", "Sustain"}};
        const int fields[2][3] = {{O::AttackTime, O::DecayTime, O::ReleaseTime}, {O::AttackLevel, O::DecayLevel, O::SustainLevel}};
        const float columns[3][2] = {{453, 551}, {553, 637}, {639, 722}};
        for (int row = 0; row < 2; ++row)
            for (int col = 0; col < 3; ++col)
                readout(env + fields[row][col], stages[row][col], columns[col][0], kRows[row], columns[col][1], page, cyan);
        loopRow(env, page, cyan);
        if (page != PageLfo) readout(env + O::ReleaseLevel, "End", 639, kRows[2], 722, page, cyan);

        if (page == PageFilter)
        {
            readout(O::FilterEnvAmount, "Envelope", 724, kRows[0], 824, page, amber);
            readout(O::FilterFreqVel, "Freq<Vel", 828, kRows[0], 924, page, amber);
            readout(O::FilterFreqKey, "Freq<Key", 928, kRows[0], 1022, page, amber);
            readout(env + O::TimeVelScale, "Time<Vel", 724, kRows[1], 824, page, cyan);
            readout(O::FilterDrive, "Drive", 828, kRows[1], 924, page, amber);
            readout(O::FilterMorph, "Morph", 928, kRows[1], 1022, page,
                    amberWhen([this, active] { return active() && std::lround(value(O::FilterType)) == 4; }));
            readout(O::ShaperType, "Shaper", 724, kRows[2], 824, page, amber)->arrowX = 84;
            const auto shaping = [this] { return std::lround(value(O::ShaperType)) != 0; };
            readout(O::ShaperDrive, "Drive", 828, kRows[2], 924, page, amberWhen(shaping));
            readout(O::ShaperDryWet, "Dry/Wet", 928, kRows[2], 1022, page, amberWhen(shaping));
            return;
        }
        // LFO / Pitch: amounts, the Dest A targets (oscillators A-D and the filter or the LFO), Dest B (disabled).
        const bool lfo = page == PageLfo;
        readout(env + O::TimeVelScale, "Time<Vel", 724, kRows[0], 824, page, cyan);
        if (lfo)
        {
            readout(O::LfoRateKey, "Rate<Key", 828, kRows[0], 924, page, amber);
            readout(O::LfoAmountVel, "Amt<Vel", 928, kRows[0], 1022, page, amber);
        }
        // Dest. A: the targets (label painted by paintDisplay) and the amount.
        for (int op = 0; op < O::kOperators; ++op)
            displayButton(O::opParam(op, lfo ? O::OpLfoOn : O::OpPitchEnvOn), letter(op), 726 + 38.0f * (float) op, 284, 758 + 38.0f * (float) op, 306, page,
                          active);
        displayButton(lfo ? O::FilterLfoOn : O::LfoPitchEnvOn, lfo ? "Fil" : "LFO", 878, 284, 920, 306, page, active);
        readout(lfo ? O::LfoAmountA : O::PitchAmountA, "Amount", 928, kRows[1], 1022, page, amber);
        inertReadout(lfo ? O::LfoDestB : O::PitchDestB, "Dest. B", 724, kRows[2], 920, page, 172);
        inertReadout(lfo ? O::LfoDestBAmount : O::PitchDestBAmount, "Amount", 928, kRows[2], 1022, page);
    }

    // ---- Global page (laid out from the manual, awaiting a capture, DEV-28a): algorithms in the graph, voices / glide /
    // pan / time in the Global section, the MIDI routing (disabled, DEV-28a2) at the right.
    void buildGlobalPage()
    {
        const auto cyan = cyanWhen({}), amber = amberWhen({});
        constexpr int pg = PageGlobal;
        // Four columns of about 95 px (the labels here are longer than the oscillator page's).
        const float x[5] = {452, 553, 646, 739, 832};
        const auto cell = [&](int p, const char* name, int col, int row, std::function<SkColor(const Theme&)> ink) {
            return readout(p, name, x[col], kRows[row], x[col + 1] - 2, pg, std::move(ink));
        };
        cell(O::NumVoices, "Voices", 0, 0, cyan)->arrowX = 74;
        cell(O::RetriggerMode, "Retrigger", 1, 0, cyan);
        cell(O::GlideOn, "Glide", 2, 0, cyan);
        cell(O::GlideTime, "Time", 3, 0, cyanWhen([this] { return on(O::GlideOn); }));
        cell(O::Pan, "Pan", 0, 1, cyan);
        cell(O::PanKey, "Pan<Key", 1, 1, cyan);
        cell(O::PanRandom, "Pan<Rnd", 2, 1, cyan);
        cell(O::PitchBendRange, "PB Range", 3, 1, cyan);
        cell(O::EnvTimeKey, "Time<Key", 0, 2, cyan);
        cell(O::EnvTimeIncludeAttack, "Attack", 1, 2, cyan);
        cell(O::Interpolation, "Interpol", 2, 2, amber);
        cell(O::AntiAlias, "Antialias", 3, 2, amber);
        const char* sources[3] = {"Velocity", "Key", "Ctrl 1"};
        const int sourceIndex[3] = {1, 0, 2};
        for (int i = 0; i < 3; ++i)
        {
            inertReadout(O::midiModParam(sourceIndex[i], 0, true), sources[i], 840, kRows[i], 930, pg, 76);
            inertReadout(O::midiModParam(sourceIndex[i], 0, false), "Amount", 934, kRows[i], 1022, pg);
        }
    }

    // ---- painting
    // A strip (left column): square at the body's left edge, rounded at the right; a panel (right column): the mirror.
    void panelLeft(SkCanvas& c, const Theme& t, float y0, float y1) const
    {
        panel(c, t, 12, y0, 436, y1);
        paint::fill(c, at(12, y0, 20, y1), t.color("SurfaceBackground"));
    }
    void panelRight(SkCanvas& c, const Theme& t, float y0, float y1) const
    {
        panel(c, t, 1038, y0, 1462, y1);
        paint::fill(c, at(1452, y0, 1462, y1), t.color("SurfaceBackground"));
    }

    int envelopeFirst() const
    {
        if (page_ <= PageD) return O::envParam(page_, 0);
        if (page_ == PageLfo) return O::otherEnvParam(O::EnvLfo, 0);
        if (page_ == PageFilter) return O::otherEnvParam(O::EnvFilter, 0);
        return O::otherEnvParam(O::EnvPitch, 0);
    }
    enum class Levels { Gain, Unipolar, Bipolar };
    Levels levels() const { return page_ == PageFilter ? Levels::Unipolar : page_ == PagePitch ? Levels::Bipolar : Levels::Gain; }
    // Level y between 150 (zero / -inf / -48 st) and 34.5 (full / 0 dB / +48 st); the pitch envelope's 0 st in the middle.
    float levelY(float v) const
    {
        constexpr float bottom = 150, top = 34.5f;
        switch (levels())
        {
            case Levels::Gain: return bottom - (bottom - top) * (v <= kFloor ? 0.0f : std::pow(std::clamp(v, 0.0f, 1.0f), kGraphGain));
            case Levels::Unipolar: return bottom - (bottom - top) * std::clamp(v, 0.0f, 1.0f);
            case Levels::Bipolar: break;
        }
        return (bottom + top) * 0.5f - (bottom - top) * 0.5f * std::clamp(v / 48.0f, -1.0f, 1.0f);
    }
    float levelAt(float y) const
    {
        constexpr float bottom = 150, top = 34.5f;
        const float n = std::clamp((bottom - y) / (bottom - top), 0.0f, 1.0f);
        switch (levels())
        {
            case Levels::Gain: return n <= 0 ? 0.0f : std::pow(n, 1.0f / kGraphGain);
            case Levels::Unipolar: return n;
            case Levels::Bipolar: break;
        }
        return std::round((n * 2 - 1) * 48.0f);
    }
    // Stage widths: kEnvPx per e-fold above 1 ms (the capture: Decay 1.00 s spans 114 px, Release 400 ms 99 px); the
    // release always starts at x 829. The Filter page keeps the envelope in the display's left half.
    Geometry geometry() const
    {
        Geometry g{};
        const bool half = page_ == PageFilter;
        g.k = half ? kEnvPx * 0.52f : kEnvPx;
        const int first = envelopeFirst();
        const auto width = [&](int field) { return g.k * std::log(std::max(1.0f, value(first + field))); };
        g.x[0] = 460;
        g.x[1] = g.x[0] + width(O::AttackTime);
        g.x[2] = g.x[1] + width(O::DecayTime);
        g.x[3] = std::max(half ? 640.0f : 829.0f, g.x[2]);
        g.x[4] = g.x[3] + width(O::ReleaseTime);
        g.y[0] = levelY(value(first + O::AttackLevel));
        g.y[1] = levelY(value(first + O::DecayLevel));
        g.y[2] = g.y[3] = levelY(value(first + O::SustainLevel));
        g.y[4] = levelY(value(first + O::ReleaseLevel));
        return g;
    }

    void brackets(SkCanvas& c, const Theme& t, float x0, float y0, float x1, float y1) const
    {
        const float u = unit(), arm = 18, w = 2;
        const auto ink = t.color("RangeDefault");
        for (const auto [x, dx] : {std::pair{x0, 1.0f}, std::pair{x1, -1.0f}})
            for (const auto [y, dy] : {std::pair{y0, 1.0f}, std::pair{y1, -1.0f}})
            {
                const float ex = dx > 0 ? x : x - w, ey = dy > 0 ? y : y - w;
                paint::fill(c, at(std::min(ex, ex + dx * arm), ey, std::max(ex + w, ex + dx * arm), ey + w), ink);
                paint::fill(c, at(ex, std::min(ey, ey + dy * arm), ex + w, std::max(ey + w, ey + dy * arm)), ink);
            }
        (void) u;
    }
    void header(SkCanvas& c, const Theme& t, const char* name, float x, float sepEnd, bool filled) const
    {
        const float u = unit();
        const auto cyan = t.color("RangeDefault");
        const auto square = at(x, 170, x + 14, 184);
        paint::fill(c, square, cyan);
        if (! filled) paint::fill(c, square.reduced(2 * u), t.color("RetroDisplayBackground"));
        label(c, t, name, x + 19, 165, sepEnd, 189, Align::Left, false, t.color("SurfaceBackground"));
        paint::fill(c, at(x, 188, sepEnd, 190), t.color("SurfaceBackground"));
    }

    void paintDisplay(SkCanvas& c, const Theme& t)
    {
        displayPanel(c, t, kDisplayX0, kDisplayY0, kDisplayX1, kDisplayY1);
        const bool osc = page_ <= PageD;
        if (page_ == PageGlobal)
        {
            paintAlgorithms(c, t);
            header(c, t, "Global", 450, 828, true);
            header(c, t, "MIDI", 836, 1024, false);
            return;
        }
        if (osc && harmonics_) paintHarmonics(c, t);
        else paintEnvelope(c, t);
        if (page_ == PageFilter) paintResponse(c, t);
        if (osc)
        {
            header(c, t, "Envelope", 450, 818, ! harmonics_);
            header(c, t, "Oscillator", 826, 1024, harmonics_);
            paintWave(c, t);
            return;
        }
        header(c, t, "Envelope", 450, 718, true);
        header(c, t, page_ == PageLfo ? "LFO" : page_ == PageFilter ? "Filter" : "Pitch", 725, 1024, false);
        if (page_ != PageFilter) label(c, t, "Dest. A", 724, 254, 920, 278, Align::Left, false, t.color("SurfaceBackground"));
    }

    bool envelopeActive() const
    {
        if (page_ <= PageD) return on(O::opParam(page_, O::OpOn));
        return on(page_ == PageLfo ? O::LfoOn : page_ == PageFilter ? O::FilterOn : O::PitchEnvOn);
    }
    void paintEnvelope(SkCanvas& c, const Theme& t)
    {
        const bool half = page_ == PageFilter;
        brackets(c, t, 450, 24, half ? 730 : 1024, 160);
        const auto g = geometry();
        const float u = unit();
        const int first = envelopeFirst();
        const bool active = envelopeActive();
        const auto ink = active ? t.color("RangeDefault") : t.color("SurfaceBackground");
        if (levels() == Levels::Bipolar) paint::fill(c, at(456, 91.5f, 1018, 92.5f), paint::withAlpha(t.color("SurfaceBackground"), 90));
        SkPathBuilder path;
        const auto to = [&](float x, float y, bool start = false) {
            const auto pt = at(x, y, x, y);
            if (start) path.moveTo(pt.x, pt.y); else path.lineTo(pt.x, pt.y);
        };
        to(g.x[0], g.y[0], true);
        const int slopes[4] = {O::AttackSlope, O::DecaySlope, -1, O::ReleaseSlope};
        for (int stage = 0; stage < 4; ++stage)
        {
            const float x0 = g.x[stage], x1 = g.x[stage + 1], y0 = g.y[stage], y1 = g.y[stage + 1];
            if (slopes[stage] < 0 || x1 - x0 < 0.5f) { to(x1, y1); continue; }
            const float slope = value(first + slopes[stage]);
            for (int i = 1; i <= 32; ++i)
            {
                const float f = (float) i / 32.0f, k = O::slopeCurve(f, slope);
                to(x0 + (x1 - x0) * f, y0 + (y1 - y0) * k);
            }
        }
        stroke(c, path, ink, 2.6f * u);
        const auto handle = [&](float x, float y) {
            const auto q = at(x - 6, y - 6, x + 6, y + 6);
            paint::stroke(c, q.reduced(1 * u), active ? t.color("ChosenDefault") : t.color("SurfaceBackground"), 2 * u);
        };
        handle(g.x[0], g.y[0]);
        handle(g.x[1], g.y[1]);
        handle(g.x[2], g.y[2]);
        handle(g.x[4], g.y[4]);
    }

    int harmonicCount(int op) const { return 16 << std::clamp((int) std::lround(value(O::numHarmonicsParam(op))), 0, 2); }
    // The oscillator's partials: the User wave's own, or the classic series of the chosen shape (sine, saw, square,
    // triangle); drawing makes the wave User.
    float partial(int op, int h) const
    {
        const int wave = (int) std::lround(value(O::opParam(op, O::OpWave)));
        const int n = h + 1;
        if (wave == O::kWaveforms - 1) return value(O::harmonicParam(op, h));
        if (wave <= 2) return n == 1 ? 1.0f : 0.0f;
        static const int sawCount[] = {3, 4, 6, 8, 16, 32, 64, 64};
        if (wave >= 3 && wave <= 10) return n <= sawCount[wave - 3] ? 1.0f / (float) n : 0.0f;
        if (wave >= 11 && wave <= 18) return n % 2 == 1 && n <= sawCount[wave - 11] * 2 ? 1.0f / (float) n : 0.0f;
        if (wave == 19) return n % 2 == 1 ? 1.0f / (float) (n * n) : 0.0f;
        return 0.5f; // noise: flat
    }
    void paintHarmonics(SkCanvas& c, const Theme& t)
    {
        brackets(c, t, 450, 24, 1024, 160);
        const int op = page_, count = harmonicCount(op);
        const float w = 574.0f / (float) count;
        const auto ink = on(O::opParam(op, O::OpOn)) ? t.color("RangeDefault") : t.color("SurfaceBackground");
        for (int h = 0; h < count; ++h)
        {
            const float amp = std::clamp(partial(op, h), 0.0f, 1.0f), x0 = 450 + w * (float) h;
            if (amp > 0.001f) paint::fill(c, at(x0 + w * 0.2f, 156 - 128 * amp, x0 + w * 0.8f, 156), ink);
            else paint::fill(c, at(x0 + w * 0.2f, 154, x0 + w * 0.8f, 156), paint::withAlpha(ink, 90));
        }
    }

    // The waveform picture right of the Wave chooser (x 932..1006, y 203..242): one cycle of the chosen wave.
    void paintWave(SkCanvas& c, const Theme& t)
    {
        const int op = page_, wave = (int) std::lround(value(O::opParam(op, O::OpWave)));
        constexpr int points = 96;
        float samples[points + 1];
        float peak = 1e-6f;
        uint32_t noise = 0x2545f491u;
        for (int i = 0; i <= points; ++i)
        {
            const double phase = (double) i / points;
            float s = 0;
            if (wave == O::kWaveforms - 1)
                for (int h = 0; h < harmonicCount(op); ++h) s += value(O::harmonicParam(op, h)) * (float) std::sin(2 * M_PI * (h + 1) * phase);
            else if (wave == 20 || wave == 21)
            {
                noise ^= noise << 13; noise ^= noise >> 17; noise ^= noise << 5;
                s = (float) (noise & 0xffff) / 32767.5f - 1.0f;
            }
            else s = O::waveSample(wave, phase);
            samples[i] = s;
            peak = std::max(peak, std::abs(s));
        }
        SkPathBuilder path;
        for (int i = 0; i <= points; ++i)
        {
            const float x = 933 + 72.0f * (float) i / points, y = 222.5f - 18.5f * samples[i] / peak;
            const auto pt = at(x, y, x, y);
            if (i == 0) path.moveTo(pt.x, pt.y); else path.lineTo(pt.x, pt.y);
        }
        stroke(c, path, on(O::opParam(op, O::OpOn)) ? t.color("RangeDefault") : t.color("SurfaceBackground"), 2.6f * unit());
    }

    // Filter page: the response (30 Hz .. 18.5 kHz over x 750..1016, 0 dB at y 70, 1.4 px per dB) from the device's own
    // stages, and the amber cutoff / resonance handle (resonance 0 .. 125 % over y 140..40).
    void paintResponse(SkCanvas& c, const Theme& t)
    {
        brackets(c, t, 740, 24, 1024, 160);
        const bool active = on(O::FilterOn);
        const float u = unit();
        devices::BiquadCoeffs stages[2];
        const int count = synth_->filterStages(value(O::FilterFreq), stages);
        const double rate = synth_->sampleRate() > 0 ? synth_->sampleRate() : 44100.0;
        const auto clip = at(742, 26, 1022, 158);
        c.save();
        c.clipRect(clip.sk());
        SkPathBuilder path;
        for (int i = 0; i <= 133; ++i)
        {
            const float x = 750 + 2.0f * (float) i, hz = 30.0f * std::pow(18500.0f / 30.0f, (x - 750) / 266);
            const std::complex<double> z = std::polar(1.0, -2 * M_PI * std::min((double) hz, rate * 0.49) / rate);
            double magnitude = 1;
            for (int s = 0; s < std::min(count, 2); ++s)
            {
                const auto& b = stages[s];
                magnitude *= std::abs((b.b0 + b.b1 * z + b.b2 * z * z) / (1.0 + b.a1 * z + b.a2 * z * z));
            }
            const float db = (float) (20 * std::log10(std::max(magnitude, 1e-6))), y = std::clamp(70 - 1.4f * db, 20.0f, 170.0f);
            const auto pt = at(x, y, x, y);
            if (i == 0) path.moveTo(pt.x, pt.y); else path.lineTo(pt.x, pt.y);
        }
        stroke(c, path, active ? t.color("RangeDefault") : t.color("SurfaceBackground"), 2.6f * u);
        const float hx = 750 + 266 * std::log(value(O::FilterFreq) / 30.0f) / std::log(18500.0f / 30.0f);
        const float hy = 140 - 100 * std::clamp(value(O::FilterRes) / 1.25f, 0.0f, 1.0f);
        const auto h = at(hx, hy, hx, hy);
        SkPaint ring;
        ring.setAntiAlias(true);
        ring.setStyle(SkPaint::kStroke_Style);
        ring.setStrokeWidth(2.4f * u);
        ring.setColor(active ? t.color("ChosenDefault") : t.color("SurfaceBackground"));
        c.drawCircle(h.x, h.y, 9 * u, ring);
        c.restore();
    }

    // Global page: the eleven algorithms in the graph (click one to choose it), the chosen one bracketed in cyan.
    static Rect algorithmCell(int a) { return {454 + 51.8f * (float) a, 26, 48, 132}; }
    void paintAlgorithms(SkCanvas& c, const Theme& t)
    {
        const int chosen = std::clamp((int) std::lround(value(O::Algorithm)), 0, O::kAlgorithms - 1);
        const float u = unit();
        for (int a = 0; a < O::kAlgorithms; ++a)
        {
            const auto cell = algorithmCell(a);
            const auto r = at(cell.x, cell.y, cell.right(), cell.bottom() - 22);
            const float pitch = std::min(16.0f, 44.0f / algorithmLayout(a).cols), size = std::min(11.0f, pitch * 0.72f);
            paintAlgorithm(c, t, r, a, size * u, pitch * u, 1.6f * u, a == chosen ? 1.0f : 0.55f, true);
            label(c, t, std::to_string(a + 1), cell.x, cell.bottom() - 22, cell.right(), cell.bottom(), Align::Centre, false,
                  a == chosen ? t.color("RangeDefault") : t.color("SurfaceBackground"));
            if (a == chosen) brackets(c, t, cell.x - 2, cell.y - 2, cell.right() + 2, cell.bottom() + 2);
        }
    }

    O* synth_;
    DisplayCap* cap_ = nullptr;
    int page_ = PageA;
    bool harmonics_ = false;
    Drag drag_ = Drag::None;
    int slope_ = 0;
    float startY_ = 0, startSlope_ = 0;
};
} // namespace

std::string operatorValueText(const devices::DeviceProcessor& p, int index, float v)
{
    const auto& info = p.parameters()[(size_t) index];
    if (index >= O::UserFirst)
    {
        const int field = (index - O::UserFirst) % (O::kHarmonics + 3);
        if (field < O::kHarmonics) return percent(v * 100);
        if (field == O::kHarmonics) return choice(p, index, v);
        if (field == O::kHarmonics + 1) return repeatName((int) std::lround(v));
        return v > 0.5f ? "On" : "Off";
    }
    if (index >= O::MidiModFirst) return (index - O::MidiModFirst) % 2 == 1 ? choice(p, index, v) : percent(v);
    if (index < O::GlobalFirst && index % O::kOpFields == O::OpWave) return O::waveShortName((int) std::lround(v));
    if (! info.choices.empty()) return choice(p, index, v);
    if (info.isToggle && index != O::FilterSlope) return v > 0.5f ? "On" : "Off";
    // Envelope fields (oscillators and the filter / pitch / LFO envelopes).
    int envField = -1;
    bool pitch = false, filter = false;
    if (index < O::GlobalFirst)
    {
        const int field = index % O::kOpFields;
        if (field >= O::OpEnv) envField = field - O::OpEnv;
        else
            switch (field)
            {
                case O::OpCoarse: return v < 0.5f ? "0.5" : printed(std::round(v), "%.0f");
                case O::OpFine: return printed(clean(std::round(v), 0.5), "%.0f");
                case O::OpFixedFreq: return hertz(v);
                case O::OpVelCoarse: return printed(clean(std::round(v), 0.5), "%.0f");
                case O::OpVolume: return decibels(v);
                default: return percent(v); // Phase, Feedback, Vel, Key
            }
    }
    else if (index < O::Algorithm)
    {
        const int block = (index - O::OtherEnvFirst) / O::kEnvFields;
        envField = (index - O::OtherEnvFirst) % O::kEnvFields;
        pitch = block == O::EnvPitch;
        filter = block == O::EnvFilter;
    }
    if (envField >= 0)
        switch (envField)
        {
            case O::AttackTime: case O::DecayTime: case O::ReleaseTime: case O::LoopTime: return milliseconds(v, info.minValue);
            case O::AttackLevel: case O::DecayLevel: case O::SustainLevel: case O::ReleaseLevel:
                return pitch ? printed(clean(std::round(v), 0.5), "%.0f st") : filter ? percent(v * 100) : decibels(v);
            default: return percent(v); // slopes / Time<Vel
        }
    switch (index)
    {
        case O::Transpose: case O::PitchBendRange: return printed(clean(std::round(v), 0.5), "%.0f st");
        case O::Volume: return decibels(v);
        case O::Pan:
        {
            const int amount = (int) std::lround(v * 50);
            return amount == 0 ? "C" : std::to_string(std::abs(amount)) + (amount < 0 ? "L" : "R");
        }
        case O::FilterRes: // the reference shows a decimal under 10 % ("0.0 %", "28 %"; live-operator-fixed-alg11.png)
            return v * 100 < 9.95f ? printed(clean(v * 100.0, 0.05), "%.1f %%") : percent(v * 100);
        case O::Tone: case O::LfoAmount: case O::LfoRateKey: case O::LfoAmountVel: case O::PitchEnvAmount: case O::FilterMorph:
            return percent(v * 100);
        case O::GlideTime: return milliseconds(v, 0);
        case O::LfoRate: return printed(v, "%.2f");
        case O::FilterSlope: return v > 0.5f ? "24" : "12";
        case O::FilterFreq: return hertz(v);
        case O::FilterDrive: case O::ShaperDrive: return printed(clean(v, 0.05), "%.1f dB");
        case O::FilterLegacyQ: return ReferenceFace::significant(v);
        default: return info.unit == "%" ? percent(v) : ReferenceFace::significant(v);
    }
}

ReferenceFace* addOperatorFace(Widget& parent, devices::DeviceProcessor* processor, ReferenceFace::Tag tag)
{
    if (auto* synth = dynamic_cast<O*>(processor)) return parent.add<OperatorFace>(parent, synth, std::move(tag));
    return nullptr;
}

int operatorFacePage(const ReferenceFace& face)
{
    if (const auto* op = dynamic_cast<const OperatorFace*>(&face)) return op->page();
    return -1;
}
} // namespace sonora::ui
