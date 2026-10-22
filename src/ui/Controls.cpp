#include "ui/Controls.h"

#include "include/core/SkPaint.h"
#include "include/core/SkPathBuilder.h"

namespace sonora::ui::ctl
{

void midiMeter(SkCanvas& canvas, const Theme& theme, const Rect& bounds, bool active)
{
    SkPaint pen;
    pen.setAntiAlias(true);
    pen.setColor(theme.color(active ? "ChosenDefault" : "MeterBackground"));
    const float radius = 2.2f, spacing = 7.0f;
    for (float y = bounds.y + radius; y + radius <= bounds.bottom(); y += spacing)
        canvas.drawCircle(bounds.centreX(), y, radius, pen);
}
void loopBrace(SkCanvas& c, const Theme& t, const Rect& r, bool enabled)
{
    if (r.w <= 0 || r.h <= 0) return;
    const auto ink = t.color("ControlForeground");
    paint::fill(c, r, t.color(enabled ? "SurfaceHighlight" : "SurfaceBackground"));
    paint::stroke(c, r, ink);
    const float end = std::min(r.h * 0.8f, r.w * 0.3f);
    for (const bool right : {false, true})
    {
        const float edge = right ? r.right() : r.x, tip = edge + (right ? -end : end);
        paint::line(c, edge, r.y, tip, r.centreY(), ink);
        paint::line(c, tip, r.centreY(), edge, r.bottom(), ink);
    }
}

void triangle(SkCanvas& c, float cx, float cy, float w, float h, Dir dir, SkColor color)
{
    SkPathBuilder b;
    switch (dir)
    {
        case Dir::Down:
            b.moveTo(cx - w * 0.5f, cy - h * 0.5f);
            b.lineTo(cx + w * 0.5f, cy - h * 0.5f);
            b.lineTo(cx, cy + h * 0.5f);
            break;
        case Dir::Up:
            b.moveTo(cx - w * 0.5f, cy + h * 0.5f);
            b.lineTo(cx + w * 0.5f, cy + h * 0.5f);
            b.lineTo(cx, cy - h * 0.5f);
            break;
        case Dir::Right:
            b.moveTo(cx - h * 0.5f, cy - w * 0.5f);
            b.lineTo(cx - h * 0.5f, cy + w * 0.5f);
            b.lineTo(cx + h * 0.5f, cy);
            break;
        case Dir::Left:
            b.moveTo(cx + h * 0.5f, cy - w * 0.5f);
            b.lineTo(cx + h * 0.5f, cy + w * 0.5f);
            b.lineTo(cx - h * 0.5f, cy);
            break;
    }
    b.close();
    SkPaint p;
    p.setAntiAlias(true);
    p.setColor(color);
    c.drawPath(b.detach(), p);
}

void framed(SkCanvas& c, const Theme& t, const Rect& r, SkColor fill)
{
    paint::fill(c, r, fill);
    paint::stroke(c, r, t.color("ControlContrastFrame"));
}

void dropdown(SkCanvas& c, const Theme& t, const Rect& r, std::string_view text, bool enabled, float size)
{
    framed(c, t, r, enabled ? t.color("ControlTextBack") : t.color("SurfaceHighlight"));
    const SkColor fg = enabled ? t.color("ControlForeground") : t.color("TextDisabled");
    const float k = size > 0 ? size / t.metrics.smallFontSize : 1.0f;
    paint::text(c, text, Rect{r.x + 3 * k, r.y, r.w - 14 * k, r.h}, fg, size > 0 ? size : t.metrics.smallFontSize);
    triangle(c, r.right() - 6.5f * k, r.centreY(), 6 * k, 4 * k, Dir::Down, fg);
}

void valueBox(SkCanvas& c, const Theme& t, const Rect& r, std::string_view text, Align align, bool enabled, float size)
{
    framed(c, t, r, enabled ? t.color("ControlTextBack") : t.color("SurfaceHighlight"));
    paint::text(c, text, r.reduced(3, 0), enabled ? t.color("ControlForeground") : t.color("TextDisabled"),
                size > 0 ? size : t.metrics.smallFontSize, align);
}

void emptyBox(SkCanvas& c, const Theme& t, const Rect& r)
{
    framed(c, t, r, t.color("SurfaceHighlight"));
}

void toggle(SkCanvas& c, const Theme& t, const Rect& r, std::string_view label, bool on, SkColor onColour, bool enabled,
            bool bold, float size)
{
    SkColor fill = on ? onColour : (enabled ? t.color("ControlTextBack") : t.color("SurfaceHighlight"));
    framed(c, t, r, fill);
    const SkColor fg = on ? t.color("ControlOnForeground") : (enabled ? t.color("ControlOffForeground") : t.color("TextDisabled"));
    paint::text(c, label, r.reduced(1, 0), fg, size > 0 ? size : t.metrics.smallFontSize, Align::Centre, bold);
}

void barBox(SkCanvas& c, const Theme& t, const Rect& r, std::string_view text, Align align, bool on, SkColor onColour)
{
    paint::fill(c, r, on ? onColour : t.color("TransportOffBackground"));
    paint::text(c, text, r.reduced(5, 0), t.color(on ? "ControlOnForeground" : "TransportOffForeground"), t.metrics.fontSize,
                align);
}

void caption(SkCanvas& c, const Theme& t, const Rect& r, std::string_view text, Align align)
{
    paint::text(c, text, r, t.color("ControlForeground"), t.metrics.smallFontSize, align, true);
}

} // namespace sonora::ui::ctl
