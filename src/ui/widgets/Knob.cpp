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
        const bool enabled = ! isEnabled || isEnabled();
        const SkColor ink = t.color(enabled ? "ControlForeground" : "TextDisabled"), cyan = t.color("RangeDefault");
        const float size = fontSize > 0 ? fontSize : t.metrics.smallFontSize;
        if (showLabel) paint::text(c, label_, {r.x, r.y, r.w, 14 * q}, ink, size, Align::Centre);
        const float stroke = 3 * q, d = 33 * q; // stroke centre line
        const float cx = r.centreX(), cy = r.y + 31.5f * q + shift;
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
        if (enabled && sweep > 0.1f)
        {
            pen.setColor(cyan);
            c.drawArc(oval, kStartDeg + kSweepDeg * std::min(from, norm), sweep, false, pen);
        }
        if (triangle)
        {
            SkPathBuilder tri;
            const float ty = r.y + 17.0f * q;
            tri.moveTo(cx - 3.5f * q, ty - 2.25f * q).lineTo(cx + 3.5f * q, ty - 2.25f * q).lineTo(cx, ty + 2.25f * q).close();
            SkPaint fill;
            fill.setAntiAlias(true);
            fill.setColor(enabled && sweep > 0.1f ? cyan : ink);
            c.drawPath(tri.detach(), fill);
        }
        const float angle = (kStartDeg + kSweepDeg * norm) * (float) M_PI / 180.0f;
        pen.setColor(ink);
        pen.setStrokeCap(SkPaint::kSquare_Cap);
        const float reach = d / 2 + stroke * 0.5f;
        c.drawLine(cx, cy, cx + std::cos(angle) * reach, cy + std::sin(angle) * reach, pen);
        if (showValue)
            paint::text(c, src_.format ? src_.format(value) : std::to_string(value), {r.x, r.y + 49.5f * q + shift, r.w, 14 * q}, ink,
                        size, Align::Centre);
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

bool Knob::keyPressed(const KeyEvent& key)
{
    if (key.cmd || key.alt || key.ctrl || ! src_.set) return false;
    if (key.keyCode == juce::KeyPress::deleteKey || key.keyCode == juce::KeyPress::backspaceKey)
        src_.set(src_.defaultValue);
    else if (key.keyCode == juce::KeyPress::upKey || key.keyCode == juce::KeyPress::downKey)
    {
        const float step = (key.shift ? 0.001f : 0.01f) * (key.keyCode == juce::KeyPress::upKey ? 1.0f : -1.0f);
        src_.set(fromNorm(std::clamp(toNorm(src_.get ? src_.get() : src_.defaultValue) + step, 0.0f, 1.0f)));
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
    dragStartY_ = e.y;
    dragStartNorm_ = std::clamp(toNorm(src_.get ? src_.get() : src_.defaultValue), 0.0f, 1.0f);
}

void Knob::mouseUp(const MouseEvent& e)
{
    if (labelPressed_ && ! e.right && localBounds().contains(e.x, e.y) && onLabelClick)
        onLabelClick();
    labelPressed_ = false;
    repaint();
}

void Knob::mouseDrag(const MouseEvent& e)
{
    if (e.right || labelPressed_) return;
    const float sensitivity = e.shift ? 1000.0f : 200.0f; // pixels for full range
    const float n = dragStartNorm_ + (dragStartY_ - e.y) / sensitivity;
    if (src_.set)
        src_.set(fromNorm(n));
    repaint();
}

void Knob::mouseDoubleClick(const MouseEvent& event)
{
    if (event.right) { mouseDown(event); return; }
    if (src_.set)
        src_.set(src_.defaultValue);
    repaint();
}

bool Knob::mouseWheel(const MouseEvent& e)
{
    if (! src_.set || ! src_.get)
        return false;
    if (std::abs(e.wheelDeltaX) > std::abs(e.wheelDeltaY))
        return false; // a sideways swipe over a knob scrolls the device chain, it does not turn the knob
    src_.set(fromNorm(toNorm(src_.get()) + e.wheelDeltaY * 0.05f));
    repaint();
    return true;
}

} // namespace sonora::ui
