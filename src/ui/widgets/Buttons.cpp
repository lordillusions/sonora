#include "ui/widgets/Buttons.h"
#include "ui/TrackMenu.h"

#include "include/core/SkPath.h"
#include "include/effects/SkDashPathEffect.h"

#include "ui/Controls.h"
#include "ui/Painter.h"

#include "include/core/SkPaint.h"

namespace sonora::ui
{

void TextButton::paint(SkCanvas& c, const Theme& t)
{
    const bool enabled = ! isEnabled || isEnabled();
    const bool on = enabled && ((isOn ? isOn() : false) || pressed_);
    const auto r = localBounds();

    SkColor onColour = t.color("ChosenDefault");
    switch (style_)
    {
        case Style::Solo: onColour = t.color("ChosenPreListen"); break;
        case Style::Arm: onColour = t.color("ChosenRecord"); break;
        case Style::Toggle:
        case Style::Activator:
        case Style::Plain: break;
    }

    if (roundStyle)
    {
        SkPaint ring;
        ring.setAntiAlias(true);
        ring.setColor(on ? onColour : t.color("ControlBackground"));
        c.drawCircle(r.centreX(), r.centreY(), std::min(r.w, r.h) * 0.5f - 0.5f, ring);
        ring.setStyle(SkPaint::kStroke_Style);
        ring.setStrokeWidth(1.0f);
        // Lit, the edge is a darker shade of the fill (the reference's device activator), not the grey frame.
        ring.setColor(on ? paint::mix(onColour, t.color("ControlForeground"), 0.45f) : t.color("ControlContrastFrame"));
        c.drawCircle(r.centreX(), r.centreY(), std::min(r.w, r.h) * 0.5f - 0.5f, ring);
        paint::text(c, label_, r, t.color("ControlForeground"), t.metrics.smallFontSize, Align::Centre);
        return;
    }
    if (barStyle)
    {
        ctl::barBox(c, t, r, label_, Align::Centre, on, onColour);
        return;
    }

    const bool dimmed = style_ == Style::Activator && ! on;
    ctl::toggle(c, t, r, style_ == Style::Arm && label_.empty() ? std::string_view{} : std::string_view{label_}, on, onColour,
                ! dimmed && enabled, style_ == Style::Activator);
    if (style_ == Style::Arm && label_.empty())
    {
        const auto ink = on ? t.color("ControlOnForeground") : t.color("ClipSlotButton");
        if (isMidiArm && isMidiArm())
        {
            SkPaint pen; pen.setAntiAlias(true); pen.setColor(ink);
            const float x = r.centreX(), y = r.centreY();
            c.drawCircle(x, y, 3.6f, pen);
            pen.setColor(on ? onColour : t.color("ControlBackground"));
            c.drawOval(SkRect::MakeXYWH(x - 2.2f, y + 0.2f, 2.8f, 2.0f), pen);
            c.drawRect(SkRect::MakeXYWH(x - 0.2f, y - 2.8f, 1.0f, 4.0f), pen);
        }
        else paint::recordCircle(c, Rect{r.centreX() - 3.5f, r.centreY() - 3.5f, 7, 7}, ink);
    }
}

void TextButton::mouseDown(const MouseEvent& event)
{
    if (event.right) { if (midiTarget) showControlContextMenu(*this, ctx()); return; }
    pressed_ = (! isEnabled || isEnabled()) && style_ == Style::Plain;
    repaint();
}

void TextButton::mouseUp(const MouseEvent& e)
{
    pressed_ = false;
    if (! e.right && localBounds().contains(e.x, e.y) && (! isEnabled || isEnabled()))
    {
        if (onClickWithModifiers) { auto callback = onClickWithModifiers; callback(e); }
        else if (onClick) { auto callback = onClick; callback(); }
    }
    if (Widget::isAlive(this)) repaint();
}

void IconButton::paint(SkCanvas& c, const Theme& t)
{
    const bool on = isOn ? isOn() : false;
    const auto r = localBounds();
    SkColor fg = t.color(flat ? "ControlForeground" : "TransportOffForeground");
    if (isEnabled && ! isEnabled())
        fg = t.color("TextDisabled");
    const bool transport = icon_ == Icon::Play || icon_ == Icon::Stop || icon_ == Icon::Record;
    if (icon_ == Icon::SessionView || icon_ == Icon::ArrangementView)
    {
        const auto ink = t.color(on ? "ChosenDefault" : "TextDisabled");
        for (int i = 0; i < 3; ++i)
        {
            const float d = (float) (i - 1) * 4;
            if (icon_ == Icon::SessionView) paint::line(c, r.centreX() + d, r.centreY() - 6, r.centreX() + d, r.centreY() + 6, ink, 1.8f);
            else paint::line(c, r.centreX() - 6, r.centreY() + d, r.centreX() + 6, r.centreY() + d, ink, 1.8f);
        }
        return;
    }
    if (icon_ == Icon::Browser)
    {
        const SkColor col = on ? t.color("ChosenDefault") : t.color("TransportOffForeground");
        const float bw = 5, bh = 13;
        paint::fill(c, {r.centreX() - bw - 1.5f, r.centreY() - bh * 0.5f, bw, bh}, col);
        paint::fill(c, {r.centreX() + 1.5f, r.centreY() - bh * 0.5f, bw, bh}, col);
        return;
    }
    if (on)
    {
        paint::fill(c, r, icon_ == Icon::Play ? t.color("ChosenPlay") : icon_ == Icon::Record ? t.color("ChosenRecord")
                                                                                              : t.color("ChosenDefault"));
        fg = t.color("ControlOnForeground");
    }
    else if (! transport && ! flat)
        paint::fill(c, r, t.color("TransportOffBackground"));

    const float s = std::min(r.w, r.h) * (transport ? 0.62f : 0.5f);
    const Rect icon{r.centreX() - s * 0.5f, r.centreY() - s * 0.5f, s, s};
    switch (icon_)
    {
        case Icon::Play: paint::playTriangle(c, icon.translated(s * 0.08f, 0), fg); break;
        case Icon::Stop: paint::stopSquare(c, icon.reduced(s * 0.04f), fg); break;
        case Icon::Record: paint::recordCircle(c, icon, fg); break;
        case Icon::Loop:
        {
            SkPaint p;
            p.setAntiAlias(true);
            p.setColor(fg);
            p.setStyle(SkPaint::kStroke_Style);
            p.setStrokeWidth(1.4f);
            c.drawRoundRect(icon.reduced(-2, 1).sk(), 3, 3, p);
            break;
        }
        case Icon::Metronome:
        {
            // Open circle + filled circle, like Live's metronome glyph.
            SkPaint p;
            p.setAntiAlias(true);
            p.setColor(fg);
            p.setStyle(SkPaint::kStroke_Style);
            p.setStrokeWidth(1.2f);
            const float d = s * 0.42f;
            c.drawCircle(r.centreX() - d, r.centreY(), d * 0.8f, p);
            p.setStyle(SkPaint::kFill_Style);
            c.drawCircle(r.centreX() + d, r.centreY(), d * 0.8f, p);
            break;
        }
        case Icon::Browser: case Icon::SessionView: case Icon::ArrangementView: break;
        case Icon::Overdub:
        {
            paint::line(c, r.centreX() - s * 0.5f, r.centreY(), r.centreX() + s * 0.5f, r.centreY(), fg, 1.6f);
            paint::line(c, r.centreX(), r.centreY() - s * 0.5f, r.centreX(), r.centreY() + s * 0.5f, fg, 1.6f);
            break;
        }
        case Icon::AutomationArm:
        {
            // two breakpoints joined by a segment
            SkPaint p;
            p.setAntiAlias(true);
            p.setColor(fg);
            p.setStyle(SkPaint::kStroke_Style);
            p.setStrokeWidth(1.5f);
            const float d = s * 0.42f, rad = s * 0.2f;
            c.drawLine(r.centreX() - d + rad * 0.7f, r.centreY() + d - rad * 0.7f, r.centreX() + d - rad * 0.7f, r.centreY() - d + rad * 0.7f, p);
            c.drawCircle(r.centreX() - d, r.centreY() + d, rad, p);
            c.drawCircle(r.centreX() + d, r.centreY() - d, rad, p);
            break;
        }
        case Icon::ReEnable:
        {
            const float h = s * 0.5f;
            paint::line(c, r.centreX() - h, r.centreY(), r.centreX() + h, r.centreY(), fg, 1.5f);
            paint::line(c, r.centreX() - h, r.centreY(), r.centreX() - h * 0.1f, r.centreY() - h * 0.8f, fg, 1.5f);
            paint::line(c, r.centreX() - h, r.centreY(), r.centreX() - h * 0.1f, r.centreY() + h * 0.8f, fg, 1.5f);
            break;
        }
        case Icon::Capture:
        {
            SkPaint p;
            p.setAntiAlias(true);
            p.setColor(fg);
            p.setStyle(SkPaint::kStroke_Style);
            p.setStrokeWidth(1.3f);
            const SkScalar dashes[] = {2.2f, 2.2f};
            p.setPathEffect(SkDashPathEffect::Make(dashes, 0.0f));
            c.drawRect(icon.reduced(-1.5f, -1.5f).sk(), p);
            break;
        }
        case Icon::SessionRecord:
        {
            SkPaint p;
            p.setAntiAlias(true);
            p.setColor(fg);
            p.setStyle(SkPaint::kStroke_Style);
            p.setStrokeWidth(1.6f);
            c.drawCircle(r.centreX(), r.centreY(), s * 0.55f, p);
            break;
        }
        case Icon::PunchIn:
        case Icon::PunchOut:
        {
            // a level change at the punch point: high-then-low for In, low-then-high for Out
            const bool in = icon_ == Icon::PunchIn;
            const float hw = r.w * 0.28f, hh = r.h * 0.22f;
            const float yTop = r.centreY() - hh, yBottom = r.centreY() + hh;
            const float yA = in ? yTop : yBottom, yB = in ? yBottom : yTop;
            SkPaint p;
            p.setAntiAlias(true);
            p.setColor(fg);
            p.setStyle(SkPaint::kStroke_Style);
            p.setStrokeWidth(1.5f);
            SkPath path;
            path.moveTo(r.centreX() - hw, yA);
            path.lineTo(r.centreX() - hw * 0.25f, yA);
            path.lineTo(r.centreX() + hw * 0.25f, yB);
            path.lineTo(r.centreX() + hw, yB);
            c.drawPath(path, p);
            break;
        }
        case Icon::Draw:
        {
            // a pencil pointing down-left with a baseline
            SkPaint p;
            p.setAntiAlias(true);
            p.setColor(fg);
            p.setStyle(SkPaint::kFill_Style);
            const float u = s * 0.5f;
            SkPath body;
            body.moveTo(r.centreX() + u * 0.9f, r.centreY() - u * 0.4f);
            body.lineTo(r.centreX() + u * 0.4f, r.centreY() - u * 0.9f);
            body.lineTo(r.centreX() - u * 0.6f, r.centreY() + u * 0.1f);
            body.lineTo(r.centreX() - u * 0.9f, r.centreY() + u * 0.7f);
            body.lineTo(r.centreX() - u * 0.3f, r.centreY() + u * 0.4f);
            body.close();
            c.drawPath(body, p);
            paint::line(c, r.centreX() - u * 0.2f, r.centreY() + u * 0.95f, r.centreX() + u, r.centreY() + u * 0.95f, fg, 1.6f);
            break;
        }
        case Icon::Keyboard:
        {
            SkPaint p;
            p.setAntiAlias(true);
            p.setColor(fg);
            p.setStyle(SkPaint::kStroke_Style);
            p.setStrokeWidth(1.3f);
            const float w = s * 1.15f, h = s * 1.1f;
            const Rect keys{r.centreX() - w * 0.5f, r.centreY() - h * 0.5f, w, h};
            c.drawRect(keys.sk(), p);
            p.setStyle(SkPaint::kFill_Style);
            for (int i = 0; i < 3; ++i)
                c.drawRect(SkRect::MakeXYWH(keys.x + w * (0.2f + 0.23f * (float) i), keys.y, w * 0.14f, h * 0.6f), p);
            break;
        }
        case Icon::FloatWindow:
        {
            const float cx = r.centreX(), cy = r.centreY();
            paint::stroke(c, {cx - 5.5f, cy - 2.5f, 8, 7}, fg);
            paint::line(c, cx - 1, cy + 1, cx + 5, cy - 5, fg, 1.3f);
            paint::line(c, cx + 1.5f, cy - 5, cx + 5, cy - 5, fg, 1.3f);
            paint::line(c, cx + 5, cy - 5, cx + 5, cy - 1.5f, fg, 1.3f);
            break;
        }
        case Icon::Wrench:
        {
            // Solid open-end wrench, handle to the lower left (the reference's plug-in title glyph).
            const float cx = r.centreX(), cy = r.centreY(), k = flat ? 0.8f : 1.0f;
            SkPaint p;
            p.setAntiAlias(true);
            p.setColor(fg);
            p.setStyle(SkPaint::kStroke_Style);
            p.setStrokeCap(SkPaint::kRound_Cap);
            p.setStrokeWidth(2.2f * k);
            c.drawLine(cx - 3.6f * k, cy + 3.6f * k, cx + 1.0f * k, cy - 1.0f * k, p);
            p.setStyle(SkPaint::kFill_Style);
            c.drawCircle(cx + 1.9f * k, cy - 1.9f * k, 2.7f * k, p);
            p.setStyle(SkPaint::kStroke_Style); // the jaw: a notch cut out of the head toward the upper right
            p.setStrokeWidth(1.6f * k);
            p.setColor(t.color(flat ? "SurfaceHighlight" : "TransportOffBackground"));
            c.drawLine(cx + 2.2f * k, cy - 2.2f * k, cx + 4.8f * k, cy - 4.8f * k, p);
            break;
        }
    }
}

void IconButton::mouseUp(const MouseEvent& e)
{
    if (e.right) return;
    if (localBounds().contains(e.x, e.y) && onClick && (! isEnabled || isEnabled()))
        onClick();
    repaint();
}

void IconButton::mouseDoubleClick(const MouseEvent& event)
{
    if (event.right) return;
    if (onDoubleClick)
        onDoubleClick();
    else if (onClick)
        onClick();
    repaint();
}

} // namespace sonora::ui
