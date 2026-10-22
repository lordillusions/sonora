#include "ui/Painter.h"

#include "gfx/Fonts.h"

#include "include/core/SkPaint.h"
#include "include/core/SkPath.h"
#include "include/core/SkPathBuilder.h"
#include "include/core/SkRRect.h"

#include <string>

namespace sonora::ui::paint
{

namespace
{
SkPaint solid(SkColor c)
{
    SkPaint p;
    p.setColor(c);
    p.setAntiAlias(true);
    return p;
}
} // namespace

void fill(SkCanvas& c, const Rect& r, SkColor color)
{
    SkPaint p;
    p.setColor(color);
    c.drawRect(r.sk(), p);
}

void fillRounded(SkCanvas& c, const Rect& r, float radius, SkColor color)
{
    c.drawRoundRect(r.sk(), radius, radius, solid(color));
}

void stroke(SkCanvas& c, const Rect& r, SkColor color, float width)
{
    auto p = solid(color);
    p.setStyle(SkPaint::kStroke_Style);
    p.setStrokeWidth(width);
    c.drawRect(r.reduced(width * 0.5f).sk(), p);
}

void linkGlyph(SkCanvas& c, const Rect& r, SkColor color)
{
    SkPaint p;
    p.setAntiAlias(true);
    p.setStyle(SkPaint::kStroke_Style);
    p.setStrokeWidth(1.2f);
    p.setColor(color);
    const float h = std::min(r.h, 8.0f), w = h * 1.1f, cy = r.centreY();
    const float cx = r.centreX();
    c.drawRRect(SkRRect::MakeRectXY(SkRect::MakeXYWH(cx - w * 0.85f, cy - h * 0.35f, w, h * 0.7f), h * 0.35f, h * 0.35f), p);
    c.drawRRect(SkRRect::MakeRectXY(SkRect::MakeXYWH(cx - w * 0.15f, cy - h * 0.35f, w, h * 0.7f), h * 0.35f, h * 0.35f), p);
}

void line(SkCanvas& c, float x0, float y0, float x1, float y1, SkColor color, float width)
{
    auto p = solid(color);
    p.setStrokeWidth(width);
    p.setAntiAlias(false);
    c.drawLine(x0, y0, x1, y1, p);
}

void text(SkCanvas& c, std::string_view s, const Rect& r, SkColor color, float size, Align align, bool bold)
{
    if (s.empty() || r.w <= 2)
        return;
    const auto font = gfx::uiFont(size, bold);
    std::string str(s);
    float width = font.measureText(str.data(), str.size(), SkTextEncoding::kUTF8);

    if (width > r.w)
    {
        // Truncate one code point at a time (UTF-8 safe, UI-9).
        const std::string ell = "\xE2\x80\xA6";
        while (! str.empty() && width > r.w)
        {
            popLastCodePoint(str);
            const auto t = str + ell;
            width = font.measureText(t.data(), t.size(), SkTextEncoding::kUTF8);
        }
        str += ell;
    }

    SkFontMetrics m;
    font.getMetrics(&m);
    const float baseline = r.centreY() - (m.fAscent + m.fDescent) * 0.5f;
    float x = r.x;
    if (align == Align::Centre)
        x = r.centreX() - width * 0.5f;
    else if (align == Align::Right)
        x = r.right() - width;

    c.drawSimpleText(str.data(), str.size(), SkTextEncoding::kUTF8, x, std::round(baseline), font, solid(color));
}

void playTriangle(SkCanvas& c, const Rect& r, SkColor color)
{
    SkPathBuilder b;
    b.moveTo(r.x, r.y);
    b.lineTo(r.right(), r.centreY());
    b.lineTo(r.x, r.bottom());
    b.close();
    c.drawPath(b.detach(), solid(color));
}

void stopSquare(SkCanvas& c, const Rect& r, SkColor color)
{
    c.drawRect(r.sk(), solid(color));
}

void recordCircle(SkCanvas& c, const Rect& r, SkColor color)
{
    c.drawCircle(r.centreX(), r.centreY(), std::min(r.w, r.h) * 0.5f, solid(color));
}

SkColor contrastText(SkColor bg)
{
    const float lum = 0.299f * SkColorGetR(bg) + 0.587f * SkColorGetG(bg) + 0.114f * SkColorGetB(bg);
    return lum > 140.0f ? SkColorSetRGB(0x07, 0x07, 0x07) : SkColorSetRGB(0xe6, 0xe6, 0xe6);
}

SkColor withAlpha(SkColor c, uint8_t a)
{
    return SkColorSetA(c, a);
}

SkColor mix(SkColor a, SkColor b, float t)
{
    auto lerp = [t](unsigned x, unsigned y) { return (U8CPU) std::lround(float(x) + (float(y) - float(x)) * t); };
    return SkColorSetARGB(lerp(SkColorGetA(a), SkColorGetA(b)), lerp(SkColorGetR(a), SkColorGetR(b)),
                          lerp(SkColorGetG(a), SkColorGetG(b)), lerp(SkColorGetB(a), SkColorGetB(b)));
}

} // namespace sonora::ui::paint
