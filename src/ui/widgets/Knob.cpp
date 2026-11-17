#include "ui/widgets/Knob.h"
#include "ui/TrackMenu.h"
#include <juce_gui_basics/juce_gui_basics.h>

#include "ui/Painter.h"

#include "include/core/SkPaint.h"
#include "include/core/SkPath.h"
#include "include/core/SkPathBuilder.h"

#include <cmath>
#include <cstdio>

namespace sonora::ui
{

namespace
{
constexpr float kStartDeg = 135.0f; // Skia angles: 0 == 3 o'clock, clockwise
constexpr float kSweepDeg = 270.0f;
} // namespace

float Knob::toNorm(float v) const
{
    if (src_.logarithmic && src_.minValue > 0)
        return std::log(v / src_.minValue) / std::log(src_.maxValue / src_.minValue);
    return (v - src_.minValue) / (src_.maxValue - src_.minValue);
}

float Knob::fromNorm(float n) const
{
    n = std::clamp(n, 0.0f, 1.0f);
    if (src_.logarithmic && src_.minValue > 0)
        return src_.minValue * std::pow(src_.maxValue / src_.minValue, n);
    return src_.minValue + n * (src_.maxValue - src_.minValue);
}

void Knob::paint(SkCanvas& c, const Theme& t)
{
    auto r = localBounds();
    const float value = src_.get ? src_.get() : src_.defaultValue;
    const float norm = std::clamp(toNorm(value), 0.0f, 1.0f);

    if (style == Style::Poti)
    {
        const float d = std::min(r.w, r.h) - 3;
        const SkRect oval = SkRect::MakeXYWH(r.centreX() - d / 2, r.centreY() - d / 2, d, d);
        SkPaint ring;
        ring.setAntiAlias(true);
        ring.setStyle(SkPaint::kStroke_Style);
        ring.setStrokeWidth(2.2f);
        ring.setStrokeCap(SkPaint::kButt_Cap);
        ring.setColor(t.color("Poti"));
        c.drawArc(oval, kStartDeg, kSweepDeg, false, ring);
        const float angle = (kStartDeg + kSweepDeg * norm) * (float) M_PI / 180.0f;
        SkPaint needle = ring;
        needle.setStrokeCap(SkPaint::kRound_Cap);
        const float cx = oval.centerX(), cy = oval.centerY(), rad = d / 2;
        c.drawLine(cx + std::cos(angle) * rad * 0.15f, cy + std::sin(angle) * rad * 0.15f, cx + std::cos(angle) * rad,
                   cy + std::sin(angle) * rad, needle);
        if (showLabel && ! label_.empty())
            paint::text(c, label_, Rect{r.centreX() - 8, cy + rad * 0.45f, 16, 10}, t.color("ControlForeground"),
                        t.metrics.smallFontSize - 0.5f, Align::Centre, true);
        return;
    }

    if (style == Style::Compact)
    {
        const bool enabled = ! isEnabled || isEnabled();
        if (showLabel) paint::text(c, label_, {r.x, r.y, r.w, 12}, t.color("ControlForeground"), t.metrics.smallFontSize);
        const float d = std::min(13.0f, r.h - 14);
        const SkRect oval = SkRect::MakeXYWH(r.x + 1.5f, r.bottom() - d - 1.5f, d, d);
        SkPaint arc;
        arc.setAntiAlias(true);
        arc.setStyle(SkPaint::kStroke_Style);
        arc.setStrokeWidth(2.0f);
        arc.setStrokeCap(SkPaint::kButt_Cap);
        arc.setColor(t.color("Poti"));
        c.drawArc(oval, kStartDeg, kSweepDeg, false, arc);
        const float from = src_.bipolar ? 0.5f : 0.0f;
        const float sweep = kSweepDeg * std::abs(norm - from);
        if (enabled && sweep > 0.1f)
        {
            arc.setColor(t.color("RangeDefault"));
            c.drawArc(oval, kStartDeg + kSweepDeg * std::min(from, norm), sweep, false, arc);
        }
        const float angle = (kStartDeg + kSweepDeg * norm) * (float) M_PI / 180.0f;
        SkPaint needle;
        needle.setAntiAlias(true);
        needle.setStrokeWidth(1.5f);
        needle.setStrokeCap(SkPaint::kRound_Cap);
        needle.setColor(t.color("PotiNeedle"));
        const float cx = oval.centerX(), cy = oval.centerY(), rad = d / 2;
        c.drawLine(cx, cy, cx + std::cos(angle) * rad, cy + std::sin(angle) * rad, needle);
        if (showValue)
            paint::text(c, src_.format ? src_.format(value) : std::to_string(value), {r.x + d + 3, r.bottom() - 11, r.w - d - 3, 11},
                        t.color(enabled ? "ControlForeground" : "TextDisabled"), t.metrics.smallFontSize - 0.5f);
        return;
    }

    if (style == Style::Ring)
    {
        // Measured from the reference (units of a 51-tall knob): label row 0-11.4, triangle 11.6-14.9,
        // ring 15.8-41.2 outside (2.6 thick), value row 40.5-51.5.
        const float k = r.h / 51.0f;
        const SkColor ink = t.color("ControlForeground"), cyan = t.color("RangeDefault");
        const float size = fontSize > 0 ? fontSize : t.metrics.smallFontSize;
        if (showLabel) paint::text(c, label_, {r.x, r.y, r.w, 11.4f * k}, ink, size, Align::Centre);
        const float stroke = 2.6f * k, d = std::min(25.4f * k, r.w - 2) - stroke;
        const float cx = r.centreX(), cy = r.y + 28.5f * k;
        const SkRect oval = SkRect::MakeXYWH(cx - d / 2, cy - d / 2, d, d);
        SkPaint pen;
        pen.setAntiAlias(true);
        pen.setStyle(SkPaint::kStroke_Style);
        pen.setStrokeWidth(stroke);
        pen.setStrokeCap(SkPaint::kButt_Cap);
        pen.setColor(ink);
        c.drawArc(oval, kStartDeg, kSweepDeg, false, pen);
        const float from = marker >= 0 ? marker : 0.0f;
        const float sweep = kSweepDeg * std::abs(norm - from);
        if (sweep > 0.1f)
        {
            pen.setColor(cyan);
            c.drawArc(oval, kStartDeg + kSweepDeg * std::min(from, norm), sweep, false, pen);
        }
        if (marker >= 0)
        {
            const float angle = (kStartDeg + kSweepDeg * marker) * (float) M_PI / 180.0f;
            const float tx = cx + std::cos(angle) * (d / 2 + stroke + 2.6f * k), ty = cy + std::sin(angle) * (d / 2 + stroke + 2.6f * k);
            SkPathBuilder tri;
            tri.moveTo(tx - 3.35f * k, ty - 1.65f * k).lineTo(tx + 3.35f * k, ty - 1.65f * k).lineTo(tx, ty + 1.65f * k).close();
            SkPaint fill;
            fill.setAntiAlias(true);
            fill.setColor(sweep > 0.1f ? cyan : ink);
            c.drawPath(tri.detach(), fill);
        }
        const float angle = (kStartDeg + kSweepDeg * norm) * (float) M_PI / 180.0f;
        pen.setColor(ink);
        pen.setStrokeCap(SkPaint::kSquare_Cap);
        const float reach = d / 2 + stroke * 0.5f;
        c.drawLine(cx, cy, cx + std::cos(angle) * reach, cy + std::sin(angle) * reach, pen);
        if (showValue)
            paint::text(c, src_.format ? src_.format(value) : std::to_string(value), {r.x, r.y + 40.5f * k, r.w, 11 * k}, ink,
                        size, Align::Centre);
        return;
    }

    if (style == Style::Device)
    {
        // Measured from the reference's EQ Eight / EQ Three / Compressor captures, in units of a 63.5-tall knob:
        // label row centred at 7, ring centre 31.5 (36 across outside, 3 thick), value row centred at 56.5. The
        // marker triangle (7 x 4.5, centred at 17) sits under the label and moves the ring and value 8 units down.
        const bool triangle = marker >= 0 && markerTriangle;
        const float q = r.h / (triangle ? 71.5f : 63.5f), shift = triangle ? 8.0f * q : 0.0f;
        const bool follows = mapped && mapped();
        const bool enabled = (! isEnabled || isEnabled()) && ! follows;
        const SkColor ink = t.color(enabled ? "ControlForeground" : "TextDisabled"), cyan = t.color("RangeDefault");
        const SkColor text = follows ? t.color("ControlForeground") : ink;
        const float size = fontSize > 0 ? fontSize : t.metrics.smallFontSize;
        if (showLabel) paint::text(c, label_, {r.x, r.y, r.w, 14 * q}, text, size, Align::Centre);
        const float stroke = deviceStroke * q, d = (36 - deviceStroke) * q; // outside 36 across; d = stroke centre line
        const float cx = r.centreX(), cy = r.y + 31.5f * q + shift;
        const SkRect oval = SkRect::MakeXYWH(cx - d / 2, cy - d / 2, d, d);
        SkPaint pen;
        pen.setAntiAlias(true);
        pen.setStyle(SkPaint::kStroke_Style);
        pen.setStrokeWidth(stroke);
        pen.setStrokeCap(SkPaint::kButt_Cap);
        pen.setColor(ink);
        const float from = marker >= 0 ? marker : 0.0f;
        const float slot = marker >= 0 ? markerGap : 0.0f, slotAt = kStartDeg + kSweepDeg * from;
        if (slot > 0) // two arcs either side of the marker's slot
        {
            c.drawArc(oval, kStartDeg, std::max(0.0f, slotAt - slot - kStartDeg), false, pen);
            c.drawArc(oval, slotAt + slot, std::max(0.0f, kStartDeg + kSweepDeg - slotAt - slot), false, pen);
        }
        else
            c.drawArc(oval, kStartDeg, kSweepDeg, false, pen);
        const float sweep = std::max(0.0f, kSweepDeg * std::abs(norm - from) - slot);
        if (enabled && sweep > 0.1f)
        {
            pen.setColor(cyan);
            c.drawArc(oval, norm >= from ? slotAt + slot : slotAt - slot - sweep, sweep, false, pen);
        }
        if (triangle)
        {
            SkPathBuilder tri;
            const float ty = r.y + 17.0f * q;
            const float half = markerWidth * 0.5f * q;
            tri.moveTo(cx - half, ty - 2.25f * q).lineTo(cx + half, ty - 2.25f * q).lineTo(cx, ty + 2.25f * q).close();
            SkPaint fill;
            fill.setAntiAlias(true);
            fill.setColor(follows ? t.color("ControlTextBack") : enabled && sweep > 0.1f ? cyan : ink);
            c.drawPath(tri.detach(), fill);
        }
        const float angle = (kStartDeg + kSweepDeg * norm) * (float) M_PI / 180.0f;
        pen.setColor(ink);
        pen.setStrokeCap(SkPaint::kSquare_Cap);
        const float reach = d / 2 + stroke * 0.5f;
        c.drawLine(cx, cy, cx + std::cos(angle) * reach, cy + std::sin(angle) * reach, pen);
        if (showValue)
            paint::text(c, src_.format ? src_.format(value) : std::to_string(value), {r.x, r.y + 49.5f * q + shift, r.w, 14 * q}, text,
                        size, Align::Centre);
        if (follows) // the macro mark: measured 15 x 14.3 units up-left of the ring centre, 3.1 across with a dark rim
        {
            if (slot > 0) // a light square in the marker's slot
            {
                const float a = slotAt * (float) M_PI / 180.0f, side = stroke * 0.9f;
                paint::fill(c, {cx + std::cos(a) * d / 2 - side / 2, cy + std::sin(a) * d / 2 - side / 2, side, side}, t.color("ControlTextBack"));
            }
            SkPaint dot;
            dot.setAntiAlias(true);
            dot.setColor(t.color("ControlForeground"));
            c.drawCircle(cx - 15.1f * q, cy - 14.3f * q, 3.8f * q, dot);
            dot.setColor(t.color("MacroAssignment"));
            c.drawCircle(cx - 15.1f * q, cy - 14.3f * q, 3.0f * q, dot);
        }
        return;
    }

    if (showLabel)
        paint::text(c, label_, r.removeFromTop(12), t.color("ControlForeground"), t.metrics.smallFontSize, Align::Centre);
    Rect valueArea;
    if (showValue)
        valueArea = r.removeFromBottom(12);

    const float d = std::min(r.w, r.h) - 4;
    const SkRect oval = SkRect::MakeXYWH(r.centreX() - d / 2, r.centreY() - d / 2, d, d);

    SkPaint arc;
    arc.setAntiAlias(true);
    arc.setStyle(SkPaint::kStroke_Style);
    arc.setStrokeWidth(2.5f);
    arc.setStrokeCap(SkPaint::kButt_Cap);
    arc.setColor(t.color("ControlFillHandle"));
    c.drawArc(oval, kStartDeg, kSweepDeg, false, arc);

    arc.setColor(t.color("RangeDefault"));
    const float from = src_.bipolar ? 0.5f : 0.0f;
    const float a0 = kStartDeg + kSweepDeg * std::min(from, norm);
    const float sweep = kSweepDeg * std::abs(norm - from);
    if (sweep > 0.1f)
        c.drawArc(oval, a0, sweep, false, arc);

    // Needle
    const float angle = (kStartDeg + kSweepDeg * norm) * (float) M_PI / 180.0f;
    SkPaint needle;
    needle.setAntiAlias(true);
    needle.setStrokeWidth(1.6f);
    needle.setColor(t.color("ControlForeground"));
    const float cx = oval.centerX(), cy = oval.centerY(), rad = d / 2 - 1;
    c.drawLine(cx + std::cos(angle) * rad * 0.25f, cy + std::sin(angle) * rad * 0.25f, cx + std::cos(angle) * rad,
               cy + std::sin(angle) * rad, needle);

    if (showValue)
    {
        std::string text;
        if (src_.format)
            text = src_.format(value);
        else
        {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.2f", value);
            text = buf;
        }
        paint::text(c, text, valueArea, t.color("ControlForeground"), t.metrics.smallFontSize, Align::Centre);
    }
}

std::string Knob::formatValue(float v) const
{
    if (src_.format) return src_.format(v);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f", v);
    return buf;
}

ValueLaw Knob::valueLaw() const
{
    DialRange range;
    range.format = [this](float v) { return formatValue(v); };
    range.minValue = src_.minValue;
    range.maxValue = src_.maxValue;
    range.logarithmic = src_.logarithmic;
    range.hint = src_.unit;
    return ValueLaw::detect(range);
}

Rect Knob::valueBounds() const
{
    const auto r = localBounds();
    if (! showValue && style != Style::Poti) return {};
    switch (style)
    {
        case Style::Poti: return r; // no value text: the field opens over the ring
        case Style::Compact:
        {
            const float d = std::min(13.0f, r.h - 14);
            return {d + 3, r.h - 11, std::max(0.0f, r.w - d - 3), 11};
        }
        case Style::Ring:
        {
            const float k = r.h / 51.0f;
            return {0, 40.5f * k, r.w, 11 * k};
        }
        case Style::Device:
        {
            const bool triangle = marker >= 0 && markerTriangle;
            const float q = r.h / (triangle ? 71.5f : 63.5f), shift = triangle ? 8.0f * q : 0.0f;
            return {0, 49.5f * q + shift, r.w, 14 * q};
        }
        case Style::Arc: break;
    }
    return {0, r.h - 12, r.w, 12};
}

bool Knob::canType() const
{
    return typeable && src_.set && ! (mapped && mapped()) && (! isEnabled || isEnabled());
}

void Knob::openValueEntry(uint32_t firstChar)
{
    if (! canType()) return;
    auto field = valueBounds();
    if (field.w <= 0 || field.h <= 0) field = localBounds();
    const auto shown = formatValue(current());
    float size = fontSize > 0 ? fontSize : 0.0f;
    if (style == Style::Device) size = std::max(size, field.h * 0.62f);
    beginValueEntry(*this, field, shown, [this](const std::string& text) { applyTypedValue(text); }, firstChar, size);
}

void Knob::applyTypedValue(const std::string& text)
{
    if (! canType()) return;
    if (const auto v = valueLaw().fromText(text, current()))
        src_.set(*v);
    repaint();
}

bool Knob::wantsKey(const KeyEvent& key) const
{
    return canType() && startsValueEntry(key);
}

bool Knob::keyPressed(const KeyEvent& key)
{
    if (key.cmd || key.alt || key.ctrl || ! src_.set || (mapped && mapped())) return false;
    if (startsValueEntry(key) && canType())
    {
        openValueEntry(key.textChar);
        return true;
    }
    if (key.keyCode == juce::KeyPress::returnKey && canType())
    {
        openValueEntry();
        return true;
    }
    if (key.keyCode == juce::KeyPress::deleteKey || key.keyCode == juce::KeyPress::backspaceKey)
        src_.set(src_.defaultValue);
    else if (key.keyCode == juce::KeyPress::upKey || key.keyCode == juce::KeyPress::downKey)
    {
        const int direction = key.keyCode == juce::KeyPress::upKey ? 1 : -1;
        const float stepped = valueLaw().nudge(current(), direction, key.shift);
        if (std::isfinite(stepped)) src_.set(stepped);
        else
        {
            const float step = (key.shift ? 0.001f : 0.01f) * (float) direction;
            src_.set(fromNorm(std::clamp(toNorm(current()) + step, 0.0f, 1.0f)));
        }
    }
    else return false;
    repaint(); return true;
}

void Knob::mouseDown(const MouseEvent& e)
{
    if (e.right)
    {
        if (onRightClick) onRightClick();
        else showControlContextMenu(*this, ctx(), [this] { if (src_.set) src_.set(src_.defaultValue); repaint(); });
        return;
    }
    labelPressed_ = style == Style::Ring && onLabelClick && e.y < height() * 11.4f / 51.0f;
    if (labelPressed_)
        return;
    const auto field = style == Style::Poti ? Rect{} : valueBounds();
    pressInValue_ = canType() && field.w > 0 && field.contains(e.x, e.y);
    moved_ = false;
    wheeling_ = false;
    dragLaw_ = valueLaw();
    dragStartY_ = e.y;
    dragStartNorm_ = std::clamp(toNorm(current()), 0.0f, 1.0f);
}

void Knob::mouseUp(const MouseEvent& e)
{
    if (labelPressed_ && ! e.right && localBounds().contains(e.x, e.y) && onLabelClick)
        onLabelClick();
    const bool open = pressInValue_ && ! moved_ && ! e.right && localBounds().contains(e.x, e.y);
    labelPressed_ = pressInValue_ = false;
    if (open) openValueEntry();
    repaint();
}

void Knob::mouseDrag(const MouseEvent& e)
{
    if (e.right || labelPressed_ || (mapped && mapped())) return;
    // A press on the value text only turns the knob once it has really moved (a click there opens the field).
    if (! moved_ && pressInValue_ && std::abs(e.y - dragStartY_) < 3) return;
    moved_ = true;
    const float sensitivity = e.shift ? 1000.0f : 200.0f; // pixels for full range
    const float n = dragStartNorm_ + (dragStartY_ - e.y) / sensitivity;
    if (src_.set)
        src_.set(dragLaw_.quantize(fromNorm(n), e.shift));
    repaint();
}

void Knob::mouseDoubleClick(const MouseEvent& event)
{
    if (event.right) { mouseDown(event); return; }
    if (src_.set && ! (mapped && mapped()))
        src_.set(src_.defaultValue);
    repaint();
}

bool Knob::mouseWheel(const MouseEvent& e)
{
    if (! src_.set || ! src_.get || (mapped && mapped()))
        return false;
    if (std::abs(e.wheelDeltaX) > std::abs(e.wheelDeltaY))
        return false; // a sideways swipe over a knob scrolls the device chain, it does not turn the knob
    // The wheel keeps its own unrounded position so small trackpad deltas add up to a step instead of rounding away.
    const float now = src_.get();
    if (! wheeling_ || std::abs(now - wheelSet_) > 1e-6f * std::max(1.0f, std::abs(now))) wheelNorm_ = toNorm(now);
    wheelNorm_ = std::clamp(wheelNorm_ + e.wheelDeltaY * 0.05f, 0.0f, 1.0f);
    src_.set(valueLaw().quantize(fromNorm(wheelNorm_), e.shift));
    wheelSet_ = src_.get();
    wheeling_ = true;
    repaint();
    return true;
}

} // namespace sonora::ui
