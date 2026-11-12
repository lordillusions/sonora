#include "ui/views/ProFace.h"

#include "gfx/Fonts.h"

#include "include/core/SkPathBuilder.h"
#include "include/core/SkRRect.h"
#include "include/effects/SkGradientShader.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace sonora::ui
{
namespace pro
{
SkPaint fill(SkColor c)
{
    SkPaint p;
    p.setAntiAlias(true);
    p.setColor(c);
    return p;
}

SkPaint stroke(SkColor c, float width)
{
    SkPaint p = fill(c);
    p.setStyle(SkPaint::kStroke_Style);
    p.setStrokeWidth(width);
    p.setStrokeCap(SkPaint::kRound_Cap);
    p.setStrokeJoin(SkPaint::kRound_Join);
    return p;
}

void rounded(SkCanvas& c, const Rect& r, float radius, SkColor colour)
{
    c.drawRRect(SkRRect::MakeRectXY(SkRect::MakeXYWH(r.x, r.y, r.w, r.h), radius, radius), fill(colour));
}

void roundedStroke(SkCanvas& c, const Rect& r, float radius, SkColor colour, float width)
{
    const float h = width * 0.5f;
    c.drawRRect(SkRRect::MakeRectXY(SkRect::MakeXYWH(r.x + h, r.y + h, r.w - width, r.h - width), radius, radius),
                stroke(colour, width));
}

void text(SkCanvas& c, std::string_view s, const Rect& r, SkColor colour, float size, Align align, bool bold)
{
    paint::text(c, s, r, colour, size, align, bold);
}

float textWidth(std::string_view s, float size, bool bold) { return gfx::measureText(s, size, bold); }

void panel(SkCanvas& c, const Rect& r, float radius)
{
    SkPaint p = fill(kPanel);
    const SkPoint ends[2] = {{r.x, r.y}, {r.x, r.y + r.h}};
    const SkColor colours[2] = {0xFF1C2027, 0xFF16191E};
    p.setShader(SkGradientShader::MakeLinear(ends, colours, nullptr, 2, SkTileMode::kClamp));
    c.drawRRect(SkRRect::MakeRectXY(SkRect::MakeXYWH(r.x, r.y, r.w, r.h), radius, radius), p);
    roundedStroke(c, r, radius, kLine, 1.0f);
}

void well(SkCanvas& c, const Rect& r, float radius)
{
    rounded(c, r, radius, kWell);
    roundedStroke(c, r, radius, 0xFF1E222A, 1.0f);
}

void sectionTitle(SkCanvas& c, std::string_view s, float x, float y, SkColor accent)
{
    rounded(c, {x, y + 4, 3, 10}, 1.5f, accent);
    text(c, s, {x + 9, y, 300, 18}, kDim, 11.0f, Align::Left, true);
}

void knob(SkCanvas& c, float cx, float cy, float radius, float t, SkColor accent, bool bipolar, bool hot, bool driven)
{
    t = std::clamp(t, 0.0f, 1.0f);
    const float ring = std::max(2.5f, radius * 0.11f);
    const float arcR = radius - ring * 0.5f;
    const SkRect arc = SkRect::MakeXYWH(cx - arcR, cy - arcR, 2 * arcR, 2 * arcR);
    c.drawArc(arc, 135.0f, 270.0f, false, stroke(kTrack, ring));
    const SkColor valueColour = driven ? paint::withAlpha(accent, 150) : accent;
    if (bipolar)
    {
        const float from = 270.0f, sweep = (t - 0.5f) * 270.0f;
        if (std::abs(sweep) > 0.5f) c.drawArc(arc, from, sweep, false, stroke(valueColour, ring));
    }
    else if (t > 0.002f)
        c.drawArc(arc, 135.0f, 270.0f * t, false, stroke(valueColour, ring));
    // Body: a shaded disc with a soft shadow and a thin rim.
    const float body = radius - ring - std::max(2.0f, radius * 0.08f);
    c.drawCircle(cx, cy + radius * 0.06f + 1.0f, body, fill(0x50000000));
    SkPaint shade = fill(kKnobBottom);
    const SkPoint ends[2] = {{cx, cy - body}, {cx, cy + body}};
    const SkColor colours[2] = {hot ? 0xFF525967 : kKnobTop, kKnobBottom};
    shade.setShader(SkGradientShader::MakeLinear(ends, colours, nullptr, 2, SkTileMode::kClamp));
    c.drawCircle(cx, cy, body, shade);
    c.drawCircle(cx, cy, body - 0.5f, stroke(hot ? 0x40FFFFFF : 0x22FFFFFF, 1.0f));
    if (driven) c.drawCircle(cx, cy, body + 1.5f, stroke(paint::withAlpha(accent, 90), 1.0f));
    // Pointer.
    const float angle = (135.0f + 270.0f * t) * 3.14159265f / 180.0f;
    const float inner = body * 0.55f, outer = body * 0.9f;
    c.drawLine(cx + std::cos(angle) * inner, cy + std::sin(angle) * inner, cx + std::cos(angle) * outer,
               cy + std::sin(angle) * outer, stroke(0xFFF4F6FA, std::max(1.6f, radius * 0.06f)));
}

void wordmark(SkCanvas& c, std::string_view name, float x, float y, float size, SkColor accent, std::string_view version)
{
    text(c, name, {x, y, 400, size * 1.2f}, kText, size, Align::Left, true);
    const float w = textWidth(name, size, true);
    c.drawCircle(x + w + size * 0.22f, y + size * 0.86f, size * 0.11f, fill(accent));
    std::string under = "sonora";
    if (! version.empty()) under += "  " + std::string(version);
    text(c, under, {x + 1, y + size * 1.12f, 300, size * 0.5f}, kFaint, size * 0.42f, Align::Left, true);
}

void waveform(SkCanvas& c, const float* lo, const float* hi, int bins, const Rect& r, SkColor colour, float gain)
{
    if (bins < 2) return;
    const float mid = r.y + r.h * 0.5f, half = r.h * 0.5f;
    SkPathBuilder p;
    for (int i = 0; i < bins; ++i)
    {
        const float x = r.x + r.w * (float) i / (float) (bins - 1);
        const float y = mid - std::clamp(hi[i] * gain, -1.0f, 1.0f) * half;
        if (i == 0) p.moveTo(x, y);
        else p.lineTo(x, y);
    }
    for (int i = bins - 1; i >= 0; --i)
    {
        const float x = r.x + r.w * (float) i / (float) (bins - 1);
        const float lowest = std::min(lo[i] * gain, hi[i] * gain - 0.004f);
        p.lineTo(x, mid - std::clamp(lowest, -1.0f, 1.0f) * half);
    }
    p.close();
    c.drawPath(p.detach(), fill(colour));
}
} // namespace pro

// ---------------------------------------------------------------------------------------------------------------------
ProFace::ProFace(devices::DeviceProcessor& device, Size full, Size compact, SkColor accent)
    : device_(device), full_(full), compact_(compact), accent_(accent)
{
}

float ProFace::scale() const
{
    const auto d = designSize();
    if (width() <= 0 || height() <= 0) return 1.0f;
    return std::max(0.05f, std::min(width() / d.w, height() / d.h));
}

MouseEvent ProFace::toDesign(const MouseEvent& e) const
{
    auto d = e;
    const float s = scale();
    d.x = e.x / s;
    d.y = e.y / s;
    return d;
}

MouseEvent ProFace::designToLocal(float x, float y) const
{
    MouseEvent e;
    const float s = scale();
    e.x = x * s;
    e.y = y * s;
    return e;
}

void ProFace::layout()
{
    const auto mode = height() < kCompactBelow ? Layout::Compact : Layout::Full;
    if (! built_ || mode != mode_)
    {
        mode_ = mode;
        rebuild();
    }
}

void ProFace::rebuild()
{
    controls_.clear();
    hover_ = active_ = pressedPad_ = -1;
    built_ = true;
    build(mode_);
    repaint();
}

ProFace::Control& ProFace::addKnob(const Rect& r, int param, std::string label, std::string tip)
{
    Control c;
    c.kind = Kind::Knob;
    c.r = r;
    c.param = param;
    c.label = std::move(label);
    c.tip = std::move(tip);
    controls_.push_back(std::move(c));
    return controls_.back();
}

ProFace::Control& ProFace::addToggle(const Rect& r, int param, std::string label, std::string tip)
{
    Control c;
    c.kind = Kind::Toggle;
    c.r = r;
    c.param = param;
    c.label = std::move(label);
    c.tip = std::move(tip);
    controls_.push_back(std::move(c));
    return controls_.back();
}

ProFace::Control& ProFace::addSegment(const Rect& r, int param, float v, std::string label, std::string tip)
{
    Control c;
    c.kind = Kind::Segment;
    c.r = r;
    c.param = param;
    c.value = v;
    c.label = std::move(label);
    c.tip = std::move(tip);
    controls_.push_back(std::move(c));
    return controls_.back();
}

void ProFace::addSegments(const Rect& r, int param, std::vector<std::string> labels, bool vertical, float gap, const std::string& tip)
{
    const auto& choices = info(param).choices;
    const int n = (int) (labels.empty() ? choices.size() : labels.size());
    if (n <= 0) return;
    const float span = vertical ? r.h : r.w;
    const float cell = (span - gap * (float) (n - 1)) / (float) n;
    for (int i = 0; i < n; ++i)
    {
        const float at = (cell + gap) * (float) i;
        const Rect cr = vertical ? Rect{r.x, r.y + at, r.w, cell} : Rect{r.x + at, r.y, cell, r.h};
        addSegment(cr, param, info(param).minValue + (float) i, labels.empty() ? choices[(size_t) i] : labels[(size_t) i], tip);
    }
}

ProFace::Control& ProFace::addButton(const Rect& r, std::string label, std::function<void()> click, std::string tip)
{
    Control c;
    c.kind = Kind::Button;
    c.r = r;
    c.label = std::move(label);
    c.click = std::move(click);
    c.tip = std::move(tip);
    controls_.push_back(std::move(c));
    return controls_.back();
}

ProFace::Control& ProFace::addPad(const Rect& r, std::string label, std::function<void(bool)> press, std::string tip)
{
    Control c;
    c.kind = Kind::Pad;
    c.r = r;
    c.label = std::move(label);
    c.press = std::move(press);
    c.tip = std::move(tip);
    controls_.push_back(std::move(c));
    return controls_.back();
}

ProFace::Control& ProFace::addArea(const Rect& r, std::string tip)
{
    Control c;
    c.kind = Kind::Area;
    c.r = r;
    c.tip = std::move(tip);
    controls_.push_back(std::move(c));
    return controls_.back();
}

float ProFace::value(int param) const { return device_.getParameter(param); }

void ProFace::setValue(int param, float v)
{
    const auto& i = info(param);
    v = std::clamp(v, i.minValue, i.maxValue);
    if (i.isToggle) v = v >= 0.5f ? 1.0f : 0.0f;
    else if (! i.choices.empty() || i.unit == "int") v = std::round(v);
    device_.setParameter(param, v);
    repaint();
}

float ProFace::normalised(const Control& c) const
{
    const float v = c.shown ? c.shown() : value(c.param);
    if (c.toNorm) return std::clamp(c.toNorm(v), 0.0f, 1.0f);
    const auto& i = info(c.param);
    if ((i.unit == "ms" || i.unit == "Hz") && i.minValue > 0)
        return std::clamp(std::log(std::max(v, i.minValue) / i.minValue) / std::log(i.maxValue / i.minValue), 0.0f, 1.0f);
    const float span = i.maxValue - i.minValue;
    return span > 0 ? std::clamp((v - i.minValue) / span, 0.0f, 1.0f) : 0.0f;
}

void ProFace::setNormalised(const Control& c, float t)
{
    t = std::clamp(t, 0.0f, 1.0f);
    if (c.fromNorm) { setValue(c.param, c.fromNorm(t)); return; }
    const auto& i = info(c.param);
    if ((i.unit == "ms" || i.unit == "Hz") && i.minValue > 0) setValue(c.param, i.minValue * std::pow(i.maxValue / i.minValue, t));
    else setValue(c.param, i.minValue + t * (i.maxValue - i.minValue));
}

std::string ProFace::valueText(int param, float v) const
{
    const auto& i = info(param);
    char buffer[64];
    if (i.isToggle) return v >= 0.5f ? "On" : "Off";
    if (! i.choices.empty())
    {
        const int index = std::clamp((int) std::lround(v - i.minValue), 0, (int) i.choices.size() - 1);
        return i.choices[(size_t) index];
    }
    if (i.unit == "%") std::snprintf(buffer, sizeof buffer, "%d%%", (int) std::lround(v * 100.0f));
    else if (i.unit == "dB")
    {
        if (v <= -69.5f) return "-inf dB";
        std::snprintf(buffer, sizeof buffer, std::abs(v) < 0.05f ? "0.0 dB" : "%+.1f dB", v);
    }
    else if (i.unit == "Hz")
    {
        if (v >= 1000.0f) std::snprintf(buffer, sizeof buffer, v >= 9950.0f ? "%.0f kHz" : "%.1f kHz", v / 1000.0f);
        else std::snprintf(buffer, sizeof buffer, "%.0f Hz", v);
    }
    else if (i.unit == "ms") std::snprintf(buffer, sizeof buffer, v < 9.95f ? "%.1f ms" : "%.0f ms", v);
    else if (i.unit == "st") std::snprintf(buffer, sizeof buffer, std::abs(v) < 0.05f ? "0 st" : "%+.0f st", v);
    else if (i.unit == "int") std::snprintf(buffer, sizeof buffer, "%d", (int) std::lround(v));
    else std::snprintf(buffer, sizeof buffer, "%.2f", v);
    return buffer;
}

int ProFace::hit(float x, float y) const
{
    for (int i = (int) controls_.size() - 1; i >= 0; --i)
    {
        const auto& r = controls_[(size_t) i].r;
        if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h) return i;
    }
    return -1;
}

int ProFace::parameterAt(float x, float y) const
{
    const int i = hit(x, y);
    return i < 0 ? -1 : controls_[(size_t) i].param;
}

std::optional<std::pair<float, float>> ProFace::controlCentre(const std::string& label) const
{
    for (const auto& c : controls_)
        if (c.label == label) return std::make_pair(c.r.x + c.r.w * 0.5f, c.r.y + c.r.h * 0.5f);
    return std::nullopt;
}

void ProFace::hoverDesign(float x, float y)
{
    mouseX_ = x;
    mouseY_ = y;
    hover_ = hit(x, y);
}

std::string ProFace::helpLine() const
{
    if (hover_ >= 0 && (size_t) hover_ < controls_.size())
    {
        const auto& c = controls_[(size_t) hover_];
        if (! c.tip.empty()) return c.tip;
    }
    return idleHelp();
}

// ------------------------------------------------------------------ input
void ProFace::mouseDown(const MouseEvent& raw)
{
    const auto e = toDesign(raw);
    mouseX_ = e.x;
    mouseY_ = e.y;
    lastY_ = e.y;
    const int index = hit(e.x, e.y);
    if (index < 0) return;
    const auto& c = controls_[(size_t) index];
    switch (c.kind)
    {
        case Kind::Knob:
            if (e.right) return;
            if (e.alt)
            {
                willEdit(c.param);
                setValue(c.param, info(c.param).defaultValue);
                return;
            }
            willEdit(c.param);
            active_ = index;
            dragNorm_ = normalised(c);
            return;
        case Kind::Toggle:
            setValue(c.param, value(c.param) >= 0.5f ? 0.0f : 1.0f);
            return;
        case Kind::Segment:
            willEdit(c.param);
            setValue(c.param, c.value);
            return;
        case Kind::Button:
            if (! e.right && c.click) c.click();
            repaint();
            return;
        case Kind::Pad:
            pressedPad_ = index;
            if (c.press) c.press(true);
            repaint();
            return;
        case Kind::Area:
            active_ = index;
            if (c.down) c.down(e);
            return;
    }
}

void ProFace::mouseDrag(const MouseEvent& raw)
{
    const auto e = toDesign(raw);
    mouseX_ = e.x;
    mouseY_ = e.y;
    const float dy = e.y - lastY_;
    lastY_ = e.y;
    if (active_ < 0 || (size_t) active_ >= controls_.size()) return;
    const auto& c = controls_[(size_t) active_];
    if (c.kind == Kind::Knob)
    {
        dragNorm_ = std::clamp(dragNorm_ - dy / (e.shift ? 900.0f : 220.0f), 0.0f, 1.0f);
        setNormalised(c, dragNorm_);
    }
    else if (c.kind == Kind::Area && c.drag)
        c.drag(e);
}

void ProFace::mouseUp(const MouseEvent& raw)
{
    const auto e = toDesign(raw);
    if (pressedPad_ >= 0 && (size_t) pressedPad_ < controls_.size())
    {
        const auto& c = controls_[(size_t) pressedPad_];
        if (c.press) c.press(false);
    }
    if (active_ >= 0 && (size_t) active_ < controls_.size())
    {
        const auto& c = controls_[(size_t) active_];
        if (c.kind == Kind::Area && c.up) c.up(e);
    }
    pressedPad_ = -1;
    active_ = -1;
    repaint();
}

void ProFace::mouseMove(const MouseEvent& raw)
{
    const auto e = toDesign(raw);
    mouseX_ = e.x;
    mouseY_ = e.y;
    const int h = hit(e.x, e.y);
    if (h != hover_)
    {
        hover_ = h;
        repaint();
    }
}

void ProFace::mouseExit()
{
    if (hover_ >= 0) repaint();
    hover_ = -1;
    mouseX_ = mouseY_ = -1;
}

void ProFace::mouseDoubleClick(const MouseEvent& raw)
{
    const auto e = toDesign(raw);
    const int index = hit(e.x, e.y);
    if (index >= 0 && controls_[(size_t) index].kind == Kind::Knob)
    {
        const auto& c = controls_[(size_t) index];
        willEdit(c.param);
        setValue(c.param, info(c.param).defaultValue);
        return;
    }
    mouseDown(raw);
}

bool ProFace::mouseWheel(const MouseEvent& raw)
{
    const auto e = toDesign(raw);
    const int index = hit(e.x, e.y);
    if (index < 0 || controls_[(size_t) index].kind != Kind::Knob) return false;
    const auto& c = controls_[(size_t) index];
    willEdit(c.param);
    setNormalised(c, normalised(c) + e.wheelDeltaY * (e.shift ? 0.02f : 0.08f));
    return true;
}

std::optional<midi::ControlTarget> ProFace::mappingTargetAt(float x, float y) const
{
    const float s = scale();
    const int param = parameterAt(x / s, y / s);
    if (param < 0 || ! mappingTarget) return std::nullopt;
    return mappingTarget(param);
}

void ProFace::tick()
{
    ++ticks_;
    bool dirty = animate();
    const auto& infos = device_.parameters();
    if (lastValues_.size() != infos.size()) { lastValues_.assign(infos.size(), 0.0f); dirty = true; }
    for (size_t i = 0; i < infos.size(); ++i)
    {
        const float v = device_.getParameter((int) i);
        if (std::abs(v - lastValues_[i]) > 0.0f) { lastValues_[i] = v; dirty = true; }
    }
    if (dirty) repaint();
}

// ------------------------------------------------------------------ paint
void ProFace::paint(SkCanvas& c, const Theme&)
{
    if (! built_) layout();
    const float s = scale();
    const auto d = designSize();
    c.save();
    c.scale(s, s);
    pro::rounded(c, {0, 0, d.w, d.h}, mode_ == Layout::Compact ? 5.0f : 0.0f, pro::kBackground);
    paintBack(c, mode_);
    for (size_t i = 0; i < controls_.size(); ++i) paintControl(c, controls_[i], (int) i);
    paintFront(c, mode_);
    c.restore();
}

void ProFace::paintControl(SkCanvas& c, const Control& control, int index)
{
    switch (control.kind)
    {
        case Kind::Knob: paintKnob(c, control); break;
        case Kind::Segment: paintSegment(c, control); break;
        case Kind::Button:
        case Kind::Pad: paintButton(c, control); break;
        case Kind::Toggle: paintToggle(c, control); break;
        case Kind::Area: break;
    }
    (void) index;
}

void ProFace::paintKnob(SkCanvas& c, const Control& k)
{
    const auto& r = k.r;
    const bool small = k.small || r.h < 70.0f;
    const float textBlock = small ? 26.0f : 36.0f;
    const float radius = std::max(8.0f, std::min(r.w * 0.5f - 3.0f, (r.h - textBlock) * 0.5f));
    const float cx = r.x + r.w * 0.5f, cy = r.y + radius + 1.0f;
    const int index = (int) (&k - controls_.data());
    const bool hot = index == hover_ || index == active_;
    const bool driven = k.driven && k.driven();
    const SkColor accent = k.colour != 0 ? k.colour : accent_;
    pro::knob(c, cx, cy, radius, normalised(k), accent, k.bipolar, hot, driven);
    const float v = k.shown ? k.shown() : value(k.param);
    const std::string valueStr = valueText(k.param, v);
    float y = cy + radius + 3.0f;
    if (! small && radius >= 19.0f)
    {
        const float size = std::clamp(radius * 0.36f, 10.0f, 17.0f);
        pro::text(c, valueStr, {cx - radius * 0.86f, cy - size * 0.7f, radius * 1.72f, size * 1.4f}, pro::kText, size, Align::Centre, true);
        pro::text(c, k.label, {r.x - 6, y, r.w + 12, 14}, hot ? pro::kText : pro::kDim, 11.0f, Align::Centre, true);
        if (k.caption)
            pro::text(c, k.caption(), {r.x - 10, y + 14, r.w + 20, 13}, driven ? paint::withAlpha(accent, 220) : pro::kFaint, 9.5f);
        return;
    }
    pro::text(c, k.label, {r.x - 4, y, r.w + 8, 12}, hot ? pro::kText : pro::kDim, 9.5f, Align::Centre, true);
    pro::text(c, valueStr, {r.x - 6, y + 11, r.w + 12, 13}, driven ? paint::withAlpha(accent, 230) : pro::kText, 10.5f, Align::Centre, true);
}

void ProFace::paintSegment(SkCanvas& c, const Control& s)
{
    const int index = (int) (&s - controls_.data());
    const bool on = std::abs(value(s.param) - s.value) < 0.5f;
    const bool hot = index == hover_;
    const SkColor accent = s.colour != 0 ? s.colour : accent_;
    const float radius = std::min(6.0f, s.r.h * 0.3f);
    if (on) pro::rounded(c, s.r, radius, accent);
    else
    {
        pro::rounded(c, s.r, radius, hot ? 0xFF2B303A : pro::kRaised);
        pro::roundedStroke(c, s.r, radius, pro::kLine, 1.0f);
    }
    const float size = s.textSize > 0 ? s.textSize : std::clamp(s.r.h * 0.42f, 9.0f, 13.0f);
    pro::text(c, s.label, s.r, on ? paint::contrastText(accent) : (hot ? pro::kText : pro::kDim), size, Align::Centre, true);
}

void ProFace::paintButton(SkCanvas& c, const Control& b)
{
    const int index = (int) (&b - controls_.data());
    const bool on = (b.lit && b.lit()) || index == pressedPad_;
    const bool hot = index == hover_;
    const SkColor accent = b.colour != 0 ? b.colour : accent_;
    const float radius = b.kind == Kind::Pad ? 10.0f : std::min(6.0f, b.r.h * 0.3f);
    if (on)
    {
        if (b.kind == Kind::Pad)
        {
            for (int g = 3; g >= 1; --g) // glow
                pro::rounded(c, {b.r.x - (float) g * 2, b.r.y - (float) g * 2, b.r.w + (float) g * 4, b.r.h + (float) g * 4},
                             radius + (float) g * 2, paint::withAlpha(accent, (uint8_t) (22 * (4 - g))));
        }
        pro::rounded(c, b.r, radius, accent);
    }
    else
    {
        pro::rounded(c, b.r, radius, hot ? 0xFF2B303A : pro::kRaised);
        pro::roundedStroke(c, b.r, radius, b.kind == Kind::Pad ? paint::withAlpha(accent, 120) : pro::kLine, b.kind == Kind::Pad ? 1.5f : 1.0f);
    }
    const float size = b.textSize > 0 ? b.textSize : std::clamp(b.r.h * 0.4f, 9.0f, b.kind == Kind::Pad ? 22.0f : 13.0f);
    pro::text(c, b.label, b.r, on ? paint::contrastText(accent) : (hot ? pro::kText : (b.kind == Kind::Pad ? accent : pro::kDim)), size,
              Align::Centre, true);
}

void ProFace::paintToggle(SkCanvas& c, const Control& t)
{
    const int index = (int) (&t - controls_.data());
    const bool on = value(t.param) >= 0.5f;
    const bool hot = index == hover_;
    const SkColor accent = t.colour != 0 ? t.colour : accent_;
    const float radius = t.r.h * 0.5f;
    pro::rounded(c, t.r, radius, hot ? 0xFF2B303A : pro::kRaised);
    pro::roundedStroke(c, t.r, radius, on ? paint::withAlpha(accent, 160) : pro::kLine, 1.0f);
    const float dot = t.r.h * 0.22f;
    const float dx = t.r.x + t.r.h * 0.5f + 2.0f, dy = t.r.y + t.r.h * 0.5f;
    if (on) c.drawCircle(dx, dy, dot + 3.0f, pro::fill(paint::withAlpha(accent, 60)));
    c.drawCircle(dx, dy, dot, pro::fill(on ? accent : 0xFF3A404B));
    const float size = t.textSize > 0 ? t.textSize : std::clamp(t.r.h * 0.42f, 9.0f, 12.0f);
    pro::text(c, t.label, {t.r.x + t.r.h + 2, t.r.y, t.r.w - t.r.h - 6, t.r.h}, on ? pro::kText : pro::kDim, size, Align::Left, true);
}

void ProFace::paintHelp(SkCanvas& c, const Rect& r)
{
    const auto help = helpLine();
    if (help.empty()) return;
    pro::text(c, help, r, pro::kDim, std::min(12.0f, r.h * 0.75f), Align::Left);
}

void ProFace::paintWindowButton(SkCanvas& c, const Rect& r)
{
    const float x = r.x + r.w * 0.5f, y = r.y + r.h * 0.5f, s = std::min(r.w, r.h) * 0.3f;
    const auto pen = pro::stroke(pro::kDim, 1.4f);
    c.drawRect(SkRect::MakeXYWH(x - s, y - s * 0.6f, s * 1.4f, s * 1.4f), pen);
    c.drawLine(x - s * 0.1f, y + s * 0.1f, x + s, y - s, pen);
    c.drawLine(x + s * 0.35f, y - s, x + s, y - s, pen);
    c.drawLine(x + s, y - s, x + s, y - s * 0.35f, pen);
}
} // namespace sonora::ui
