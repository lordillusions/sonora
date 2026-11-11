#include "gfx/Brand.h"

#include <cstring>

#include "gfx/Fonts.h"

#include "include/core/SkCanvas.h"
#include "include/core/SkBlurTypes.h"
#include "include/core/SkMaskFilter.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPathBuilder.h"
#include "include/core/SkPathUtils.h"
#include "include/core/SkRRect.h"
#include "include/core/SkSurface.h"
#include "include/effects/SkGradientShader.h"
#include "include/utils/SkParsePath.h"

#include <algorithm>
#include <cmath>

namespace sonora::gfx::brand
{

namespace
{
constexpr float pi = 3.14159265358979f;

// Wordmark geometry, in cap heights. Every curve is a half (or quarter) circle joined to straight bars,
// the same construction as the mark, so the letters read as one family.
constexpr float strokeW = 0.165f;   // stroke width
constexpr float xHeight = 0.74f;
constexpr float letterGap = 0.105f;

SkPath ribbonS(float x, float top, float width, float height, float s)
{
    // Centerlines: top bar, left half circle, middle bar, right half circle, bottom bar.
    const float yTop = top + s / 2, yBottom = top + height - s / 2, yMid = (yTop + yBottom) / 2;
    const float r = (yMid - yTop) / 2;
    const float leftCx = x + s / 2 + r, rightCx = x + width - s / 2 - r;
    SkPathBuilder b;
    b.moveTo(x + width, yTop);
    b.lineTo(leftCx, yTop);
    b.arcTo(SkRect::MakeLTRB(leftCx - r, yTop, leftCx + r, yMid), -90, -180, false);
    b.lineTo(rightCx, yMid);
    b.arcTo(SkRect::MakeLTRB(rightCx - r, yMid, rightCx + r, yBottom), -90, 180, false);
    b.lineTo(x, yBottom);
    return b.detach();
}

SkPath stadium(float x, float width, float s)
{
    // An "o" ring: rounded rectangle whose sides are half circles; baseline 0, top -xHeight.
    const SkRect r = SkRect::MakeLTRB(x + s / 2, -xHeight + s / 2, x + width - s / 2, -s / 2);
    const float radius = r.height() / 2;
    SkPathBuilder b;
    b.addRRect(SkRRect::MakeRectXY(r, radius, radius));
    return b.detach();
}

SkPath line(float x0, float y0, float x1, float y1)
{
    SkPathBuilder b;
    b.moveTo(x0, y0).lineTo(x1, y1);
    return b.detach();
}

// The app icon tile: the macOS icon grid's rounded square (corner radius 185.4 on an 824 body). macOS 26 shows an
// icon whose outline differs from this shape shrunk inside a grey tile, so the tile must match it (a superellipse
// did not; checked with NSWorkspace iconForFile, UI-51c).
SkPath iconTile(const SkRect& r)
{
    const float radius = r.width() * 185.4f / 824.0f;
    SkPathBuilder b;
    b.addRRect(SkRRect::MakeRectXY(r, radius, radius));
    return b.detach();
}

SkFont font(float size, bool bold)
{
    auto f = uiFont(size, bold);
    f.setEdging(SkFont::Edging::kAntiAlias);
    f.setSubpixel(true);
    return f;
}

void drawText(SkCanvas& c, std::string text, float x, float baseline, float size, SkColor color, bool bold = false,
              bool right = false, float maxWidth = 0)
{
    const auto f = font(size, bold);
    if (maxWidth > 0 && f.measureText(text.data(), text.size(), SkTextEncoding::kUTF8) > maxWidth)
    {
        // Trim whole UTF-8 characters until the text and an ellipsis fit.
        const std::string ellipsis = "\xE2\x80\xA6";
        while (! text.empty())
        {
            while (! text.empty() && (static_cast<unsigned char>(text.back()) & 0xC0) == 0x80) text.pop_back(); // continuation bytes
            if (! text.empty()) text.pop_back(); // the lead byte, or one ASCII character
            const auto candidate = text + ellipsis;
            if (f.measureText(candidate.data(), candidate.size(), SkTextEncoding::kUTF8) <= maxWidth) { text = candidate; break; }
        }
    }
    SkPaint p;
    p.setAntiAlias(true);
    p.setColor(color);
    if (right) x -= f.measureText(text.data(), text.size(), SkTextEncoding::kUTF8);
    c.drawSimpleText(text.data(), text.size(), SkTextEncoding::kUTF8, x, baseline, f, p);
}

SkRRect panelShape(const SkRect& bounds) { return SkRRect::MakeRectXY(bounds, panel::cornerRadius, panel::cornerRadius); }

std::string hex(SkColor c)
{
    char buf[8];
    std::snprintf(buf, sizeof buf, "#%02x%02x%02x", SkColorGetR(c), SkColorGetG(c), SkColorGetB(c));
    return buf;
}

std::string pathsSvg(const std::vector<Stroke>& strokes, const std::string& fill)
{
    std::string out;
    for (const auto& s : strokes)
        out += "  <path fill=\"" + fill + "\" d=\"" + std::string(SkParsePath::ToSVGString(outline(s)).c_str()) + "\"/>\n";
    return out;
}
} // namespace

// The splash's panel (UI-51), shared by the prompts (UI-44), About (COM-8) and Settings (UI-63): black, lit slightly from
// the top-left, rounded like a macOS window, with a hairline border drawn last.
void drawPanel(SkCanvas& canvas, const SkRect& bounds)
{
    SkPaint bg;
    bg.setAntiAlias(true);
    const SkPoint pts[2] = {{bounds.left(), bounds.top()}, {bounds.left() + bounds.width() * 0.35f, bounds.bottom()}};
    const SkColor colors[2] = {panel::backgroundTop, panel::backgroundBottom};
    bg.setShader(SkGradientShader::MakeLinear(pts, colors, nullptr, 2, SkTileMode::kClamp));
    canvas.drawRRect(panelShape(bounds), bg);
}

void drawPanelBorder(SkCanvas& canvas, const SkRect& bounds)
{
    SkPaint border;
    border.setAntiAlias(true);
    border.setStyle(SkPaint::kStroke_Style);
    border.setStrokeWidth(1);
    border.setColor(panel::border);
    const float r = panel::cornerRadius - 0.5f;
    canvas.drawRRect(SkRRect::MakeRectXY(bounds.makeInset(0.5f, 0.5f), r, r), border);
}

void drawGhostMark(SkCanvas& canvas, const SkRect& box, float fadeBottom, uint8_t topAlpha, uint8_t bottomAlpha)
{
    SkPaint ghost;
    const SkPoint g[2] = {{0, box.top()}, {0, fadeBottom}};
    const SkColor gc[2] = {SkColorSetARGB(topAlpha, 255, 255, 255), SkColorSetARGB(bottomAlpha, 255, 255, 255)};
    ghost.setShader(SkGradientShader::MakeLinear(g, gc, nullptr, 2, SkTileMode::kClamp));
    drawStrokes(canvas, markStrokes(box), ghost);
}

void drawButtonFrame(SkCanvas& canvas, const SkRect& rect, const ButtonLook& look)
{
    const float radius = std::min(panel::buttonRadius, rect.height() * 0.25f);
    SkPaint fill;
    fill.setAntiAlias(true);
    if (look.primary)
        fill.setColor(look.down ? panel::primaryDown : look.hot ? panel::primaryHot : panel::primary);
    else
        fill.setColor(look.down ? panel::buttonDown : look.hot ? panel::buttonHot : panel::button);
    if (! look.enabled) fill.setAlphaf(0.45f);
    canvas.drawRRect(SkRRect::MakeRectXY(rect, radius, radius), fill);
    SkPaint edge;
    edge.setAntiAlias(true);
    edge.setStyle(SkPaint::kStroke_Style);
    edge.setStrokeWidth(1);
    if (! look.primary)
    {
        edge.setColor(panel::hairline);
        canvas.drawRRect(SkRRect::MakeRectXY(rect.makeInset(0.5f, 0.5f), radius - 0.5f, radius - 0.5f), edge);
    }
    if (look.focused && ! look.primary)
    {
        edge.setStrokeWidth(2);
        edge.setColor(panel::focusRing);
        canvas.drawRRect(SkRRect::MakeRectXY(rect.makeOutset(2, 2), radius + 2, radius + 2), edge);
    }
}

SkColor buttonTextColor(const ButtonLook& look)
{
    if (! look.enabled) return panel::dim;
    return look.primary ? panel::primaryText : panel::buttonText;
}

void drawButton(SkCanvas& canvas, const SkRect& rect, const std::string& label, const ButtonLook& look, float textSize)
{
    drawButtonFrame(canvas, rect, look);
    const auto f = font(textSize, true);
    const float tw = f.measureText(label.data(), label.size(), SkTextEncoding::kUTF8);
    drawText(canvas, label, rect.centerX() - std::min(tw, rect.width() - 8) / 2, rect.centerY() + textSize * 0.36f, textSize,
             buttonTextColor(look), true, false, rect.width() - 8);
}

void drawSwitch(SkCanvas& canvas, const SkRect& rect, bool on, bool hot, bool enabled)
{
    // The track is a primary button when on and a secondary one when off; the knob is the other one's colour.
    const float r = rect.height() / 2;
    SkPaint p;
    p.setAntiAlias(true);
    p.setColor(on ? (hot ? panel::primaryHot : panel::primary) : (hot ? panel::buttonHot : panel::button));
    if (! enabled) p.setAlphaf(0.45f);
    canvas.drawRRect(SkRRect::MakeRectXY(rect, r, r), p);
    if (! on)
    {
        SkPaint edge;
        edge.setAntiAlias(true);
        edge.setStyle(SkPaint::kStroke_Style);
        edge.setColor(panel::hairline);
        canvas.drawRRect(SkRRect::MakeRectXY(rect.makeInset(0.5f, 0.5f), r - 0.5f, r - 0.5f), edge);
    }
    const float knob = r - 2.5f;
    p.setColor(on ? panel::primaryText : (enabled ? panel::message : panel::dim));
    canvas.drawCircle(on ? rect.right() - r : rect.left() + r, rect.centerY(), knob, p);
}

void drawField(SkCanvas& canvas, const SkRect& rect)
{
    const float radius = std::min(panel::buttonRadius, rect.height() * 0.25f);
    SkPaint p;
    p.setAntiAlias(true);
    p.setColor(panel::well);
    canvas.drawRRect(SkRRect::MakeRectXY(rect, radius, radius), p);
    p.setStyle(SkPaint::kStroke_Style);
    p.setColor(panel::hairline);
    canvas.drawRRect(SkRRect::MakeRectXY(rect.makeInset(0.5f, 0.5f), radius - 0.5f, radius - 0.5f), p);
}

std::vector<Stroke> markStrokes(const SkRect& box)
{
    // The mark is heavier than the wordmark S: a 21 % stroke keeps it legible at 16 px.
    const float h = box.height(), w = h * markAspect;
    const float x = box.centerX() - w / 2;
    const float s = h * 0.21f;
    return {{ribbonS(x, box.top(), w, h, s), s}};
}

std::vector<Stroke> wordmarkStrokes(float& width)
{
    std::vector<Stroke> out;
    const float s = strokeW;
    float x = 0;
    auto add = [&](SkPath p) { out.push_back({std::move(p), s}); };

    // S
    const float sWidth = 0.74f;
    add(ribbonS(x, -1.0f, sWidth, 1.0f, s));
    x += sWidth + letterGap;

    // o
    const float oWidth = 0.80f;
    auto o = [&] { add(stadium(x, oWidth, s)); x += oWidth + letterGap; };
    o();

    // n: stem, then a half-circle arch that comes down as the right stem.
    {
        const float w = 0.70f, r = (w - s) / 2, top = -xHeight + s / 2, cx = x + s / 2 + r, cy = top + r;
        add(line(x + s / 2, 0, x + s / 2, -xHeight));
        SkPathBuilder b;
        b.moveTo(x + s / 2, cy);
        b.arcTo(SkRect::MakeLTRB(cx - r, top, cx + r, top + 2 * r), 180, 180, false);
        b.lineTo(x + w - s / 2, 0);
        add(b.detach());
        x += w + letterGap;
    }
    o();

    // r: stem and a quarter circle ending in a short arm.
    {
        const float r = 0.25f, top = -xHeight + s / 2, cx = x + s / 2 + r;
        add(line(x + s / 2, 0, x + s / 2, -xHeight));
        SkPathBuilder b;
        b.moveTo(x + s / 2, top + r);
        b.arcTo(SkRect::MakeLTRB(cx - r, top, cx + r, top + 2 * r), 180, 90, false);
        b.lineTo(cx + 0.07f, top);
        add(b.detach());
        x += s / 2 + r + 0.07f + letterGap * 0.55f; // the arm leaves room under it
    }

    // a: single-storey, a ring with a stem on its right.
    {
        const float w = 0.76f;
        add(stadium(x, w, s));
        add(line(x + w - s / 2, 0, x + w - s / 2, -xHeight));
        x += w;
    }
    width = x;
    return out;
}

SkPath outline(const Stroke& stroke)
{
    SkPaint p;
    p.setStyle(SkPaint::kStroke_Style);
    p.setStrokeWidth(stroke.width);
    p.setStrokeCap(SkPaint::kButt_Cap);
    p.setStrokeJoin(SkPaint::kRound_Join);
    return skpathutils::FillPathWithPaint(stroke.centerline, p);
}

void drawStrokes(SkCanvas& canvas, const std::vector<Stroke>& strokes, const SkPaint& paint)
{
    for (const auto& s : strokes)
    {
        SkPaint p = paint;
        p.setAntiAlias(true);
        p.setStyle(SkPaint::kStroke_Style);
        p.setStrokeWidth(s.width);
        p.setStrokeCap(SkPaint::kButt_Cap);
        p.setStrokeJoin(SkPaint::kRound_Join);
        canvas.drawPath(s.centerline, p);
    }
}

void drawStrokes(SkCanvas& canvas, const std::vector<Stroke>& strokes, SkColor color)
{
    SkPaint p;
    p.setColor(color);
    drawStrokes(canvas, strokes, p);
}

void drawAppIcon(SkCanvas& canvas, float size)
{
    const float k = size / 1024.0f;
    const SkRect body = SkRect::MakeXYWH(100 * k, 100 * k, 824 * k, 824 * k);
    const auto shape = iconTile(body);

    // Soft shadow under the tile (part of the macOS icon grid).
    SkPaint shadow;
    shadow.setAntiAlias(true);
    shadow.setColor(SkColorSetARGB(90, 0, 0, 0));
    shadow.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, 10 * k));
    canvas.save();
    canvas.translate(0, 12 * k);
    canvas.drawPath(shape, shadow);
    canvas.restore();

    // Tile: near black, a touch lighter at the top.
    SkPaint tile;
    tile.setAntiAlias(true);
    const SkPoint pts[2] = {{0, body.top()}, {0, body.bottom()}};
    const SkColor tileColors[2] = {SkColorSetRGB(0x2c, 0x2c, 0x2c), SkColorSetRGB(0x0c, 0x0c, 0x0c)};
    tile.setShader(SkGradientShader::MakeLinear(pts, tileColors, nullptr, 2, SkTileMode::kClamp));
    canvas.drawPath(shape, tile);

    // Hairline rim so the tile separates from a black Dock.
    SkPaint rim;
    rim.setAntiAlias(true);
    rim.setStyle(SkPaint::kStroke_Style);
    rim.setStrokeWidth(std::max(1.0f, 3 * k));
    const SkColor rimColors[2] = {SkColorSetARGB(70, 255, 255, 255), SkColorSetARGB(10, 255, 255, 255)};
    rim.setShader(SkGradientShader::MakeLinear(pts, rimColors, nullptr, 2, SkTileMode::kClamp));
    canvas.save();
    canvas.clipPath(shape, true);
    canvas.drawPath(shape, rim);
    canvas.restore();

    // The S: white fading to light grey.
    const float h = 470 * k;
    const SkRect markBox = SkRect::MakeXYWH(body.centerX() - h * markAspect / 2, body.centerY() - h / 2, h * markAspect, h);
    SkPaint ink;
    const SkPoint inkPts[2] = {{0, markBox.top()}, {0, markBox.bottom()}};
    const SkColor inkColors[2] = {SkColorSetRGB(0xff, 0xff, 0xff), SkColorSetRGB(0xb8, 0xb8, 0xb8)};
    ink.setShader(SkGradientShader::MakeLinear(inkPts, inkColors, nullptr, 2, SkTileMode::kClamp));
    drawStrokes(canvas, markStrokes(markBox), ink);
}

float drawWordmark(SkCanvas& canvas, float x, float baseline, float capHeight, SkColor color)
{
    float width = 0;
    const auto word = wordmarkStrokes(width);
    canvas.save();
    canvas.translate(x, baseline);
    canvas.scale(capHeight, capHeight);
    drawStrokes(canvas, word, color);
    canvas.restore();
    return width * capHeight;
}

void drawSplash(SkCanvas& canvas, float width, float height, const SplashState& state)
{
    const SkRect bounds = SkRect::MakeWH(width, height);
    canvas.clear(SK_ColorTRANSPARENT);

    drawPanel(canvas, bounds);
    canvas.save();
    canvas.clipRRect(panelShape(bounds), true);

    // Large mark on the right, fading out towards the bottom: the splash's one graphic gesture.
    {
        const float h = height * 0.74f;
        const SkRect box = SkRect::MakeXYWH(width - 52 - h * markAspect, height * 0.11f, h * markAspect, h);
        drawGhostMark(canvas, box, box.bottom(), 30, 6);
    }

    // Wordmark, left aligned. Its S is the mark.
    const float left = 44, capH = 46, baseline = 150;
    drawWordmark(canvas, left, baseline, capH, panel::title);
    drawText(canvas, "Music production, open.", left + 2, baseline + 36, 15, SkColorSetRGB(0x9a, 0x9a, 0x9a));

    // Status and version.
    const float footer = height - 40;
    drawText(canvas, state.status.empty() ? std::string("Starting...") : state.status, left + 2, footer, 12.5f,
             SkColorSetRGB(0xc8, 0xc8, 0xc8), false, false, width - left - 150);
    drawText(canvas, state.version, width - 28, footer, 11.5f, SkColorSetRGB(0x6e, 0x6e, 0x6e), false, true);

    // Progress: a hairline across the bottom; a soft highlight sweeps it while the length is unknown.
    const SkRect track = SkRect::MakeXYWH(left + 2, height - 24, width - left - 30, 2);
    SkPaint p;
    p.setAntiAlias(true);
    p.setColor(SkColorSetRGB(0x2e, 0x2e, 0x2e));
    canvas.drawRoundRect(track, 1, 1, p);
    if (state.progress >= 0)
    {
        p.setColor(SkColorSetRGB(0xee, 0xee, 0xee));
        canvas.drawRoundRect(SkRect::MakeXYWH(track.x(), track.y(), track.width() * std::clamp(state.progress, 0.0f, 1.0f), track.height()), 1, 1, p);
    }
    else
    {
        const float phase = (float) std::fmod(state.timeSeconds / 1.6, 1.0);
        const float seg = track.width() * 0.32f;
        const float x0 = track.x() - seg + (track.width() + seg) * (0.5f - 0.5f * std::cos(phase * pi));
        const SkPoint g[2] = {{x0, 0}, {x0 + seg, 0}};
        const SkColor gc[3] = {SkColorSetARGB(0, 255, 255, 255), SkColorSetARGB(235, 255, 255, 255), SkColorSetARGB(0, 255, 255, 255)};
        p.setShader(SkGradientShader::MakeLinear(g, gc, nullptr, 3, SkTileMode::kDecal));
        canvas.save();
        canvas.clipRect(track);
        canvas.drawRoundRect(track, 1, 1, p);
        canvas.restore();
    }
    canvas.restore();
    drawPanelBorder(canvas, bounds);
}

namespace
{
// Prompt layout (UI-44), in points.
constexpr float promptPad = 26, promptIcon = 64, promptTextX = promptPad + promptIcon + 16;
constexpr float promptTitleSize = panel::titleSize, promptMessageSize = panel::textSize, promptLine = 17.5f;
constexpr float promptButtonH = panel::buttonHeight, promptButtonGap = 10, promptButtonMinW = 84;

std::vector<std::string> wrapText(const std::string& text, float size, float maxWidth)
{
    const auto f = font(size, false);
    std::vector<std::string> lines;
    std::string line;
    size_t i = 0;
    while (i <= text.size())
    {
        const size_t end = std::min(text.find_first_of(" \n", i), text.size());
        const auto word = text.substr(i, end - i);
        const auto candidate = line.empty() ? word : line + " " + word;
        if (! line.empty() && f.measureText(candidate.data(), candidate.size(), SkTextEncoding::kUTF8) > maxWidth)
        {
            lines.push_back(line);
            line = word;
        }
        else
            line = candidate;
        if (end < text.size() && text[end] == '\n') { lines.push_back(line); line.clear(); }
        i = end + 1;
    }
    if (! line.empty()) lines.push_back(line);
    return lines;
}

float promptMessageTop() { return promptPad + 20 + 22; } // first message baseline
float promptButtonsTop(const PromptState& state)
{
    const auto lines = wrapText(state.message, promptMessageSize, promptWidth - promptTextX - promptPad);
    const float lastBaseline = promptMessageTop() + promptLine * (float) std::max<size_t>(1, lines.size()) - promptLine;
    return std::max(lastBaseline + 24, promptPad + promptIcon + 14);
}
} // namespace

namespace
{
// The buttons' rects relative to the top of the button area, and that area's height. One row like a macOS alert: the
// primary rightmost, the other trailing buttons leftwards in index order, the leading ones from the text column. When
// the labels are too long for that, the leading ones move under the icon; when they still collide, the buttons stack
// in a column under the text, primary on top and the leading ones last (macOS's vertical alert).
struct PromptButtonLayout
{
    std::vector<SkRect> rects;
    float height = promptButtonH;
};
PromptButtonLayout layoutPromptButtons(const PromptState& state, float width)
{
    const auto f = font(promptMessageSize, true);
    PromptButtonLayout layout;
    layout.rects.resize(state.buttons.size());
    std::vector<size_t> trailing, leading;
    for (size_t i = 0; i < state.buttons.size(); ++i)
        if (state.buttons[i].primary && ! state.buttons[i].leading) trailing.push_back(i);
    for (size_t i = 0; i < state.buttons.size(); ++i)
        if (! state.buttons[i].primary && ! state.buttons[i].leading) trailing.push_back(i);
    for (size_t i = 0; i < state.buttons.size(); ++i)
        if (state.buttons[i].leading) leading.push_back(i);
    auto widthOf = [&](size_t i) {
        const auto& label = state.buttons[i].label;
        return std::max(promptButtonMinW, f.measureText(label.data(), label.size(), SkTextEncoding::kUTF8) + 30);
    };
    float right = width - promptPad;
    for (const auto i : trailing)
    {
        const float w = widthOf(i);
        layout.rects[i] = SkRect::MakeXYWH(right - w, 0, w, promptButtonH);
        right -= w + promptButtonGap;
    }
    float leadingWidth = 0;
    for (const auto i : leading) leadingWidth += widthOf(i) + promptButtonGap;
    // `right` is now one gap left of the trailing row; leadingWidth already counts one gap after the last leading one.
    const float trailingLeft = right + promptButtonGap;
    for (const float start : {promptTextX, promptPad})
        if (start + leadingWidth <= trailingLeft + 0.5f)
        {
            float left = start;
            for (const auto i : leading)
            {
                const float w = widthOf(i);
                layout.rects[i] = SkRect::MakeXYWH(left, 0, w, promptButtonH);
                left += w + promptButtonGap;
            }
            return layout;
        }
    std::vector<size_t> column = trailing;
    column.insert(column.end(), leading.begin(), leading.end());
    float y = 0;
    for (const auto i : column)
    {
        layout.rects[i] = SkRect::MakeLTRB(promptTextX, y, width - promptPad, y + promptButtonH);
        y += promptButtonH + promptButtonGap;
    }
    layout.height = std::max(promptButtonH, y - promptButtonGap);
    return layout;
}
} // namespace

float promptHeight(const PromptState& state)
{
    return promptButtonsTop(state) + layoutPromptButtons(state, promptWidth).height + 22;
}

std::vector<SkRect> promptButtonRects(const PromptState& state, float width, float height)
{
    auto layout = layoutPromptButtons(state, width);
    const float top = height - 22 - layout.height;
    for (auto& r : layout.rects) r.offset(0, top);
    return layout.rects;
}

void drawPrompt(SkCanvas& canvas, float width, float height, const PromptState& state)
{
    const SkRect bounds = SkRect::MakeWH(width, height);
    canvas.clear(SK_ColorTRANSPARENT);
    drawPanel(canvas, bounds);
    canvas.save();
    canvas.clipRRect(panelShape(bounds), true);

    // The splash's ghost S, running off the bottom-right corner behind the text and buttons.
    {
        const float h = height * 1.25f;
        const SkRect box = SkRect::MakeXYWH(width - h * markAspect * 0.8f, height * 0.1f, h * markAspect, h);
        drawGhostMark(canvas, box, box.top() + height, 20, 4);
    }

    // App icon (the 1024 grid has a 100-unit margin, so the tile is drawn slightly larger than promptIcon).
    canvas.save();
    const float iconCanvas = promptIcon * 1024.0f / 824.0f;
    canvas.translate(promptPad - (iconCanvas - promptIcon) / 2, promptPad - (iconCanvas - promptIcon) / 2);
    drawAppIcon(canvas, iconCanvas);
    canvas.restore();

    const float textW = width - promptTextX - promptPad;
    drawText(canvas, state.title, promptTextX, promptPad + 20, promptTitleSize, panel::title, true, false, textW);
    float baseline = promptMessageTop();
    for (const auto& line : wrapText(state.message, promptMessageSize, textW))
    {
        drawText(canvas, line, promptTextX, baseline, promptMessageSize, panel::message, false, false, textW);
        baseline += promptLine;
    }

    // Buttons: the primary is the splash's white on black, the others dark with a hairline; hover lifts, press sinks.
    const auto rects = promptButtonRects(state, width, height);
    for (size_t i = 0; i < rects.size(); ++i)
        drawButton(canvas, rects[i], state.buttons[i].label,
                   {.primary = state.buttons[i].primary, .hot = state.hovered == (int) i,
                    .down = state.pressed == (int) i && state.hovered == (int) i, .focused = state.focused == (int) i});
    canvas.restore();
    drawPanelBorder(canvas, bounds);
}

namespace
{
// About window layout (COM-8), in points.
constexpr float aboutPad = 34, aboutTop = 66;
constexpr float aboutNameSize = 12.5f, aboutLine = 18, aboutTitleSize = 11;
constexpr float aboutStatusSize = 11.5f, aboutStatusLine = 14, aboutStatusGap = 12; // the update row's status (COM-4)
constexpr size_t aboutUpdateRowBefore = 2; // the update row sits between the credit (line 1) and the supporters
const SkColor aboutDim = SkColorSetRGB(0x8e, 0x8e, 0x8e);

float aboutTextWidth(float width) { return width - 2 * aboutPad; }

// Content below the header (credits, supporters) as drawn lines: (text, size, colour, bold, gap before).
struct AboutLine { std::string text; float size; SkColor color; bool bold; float gap; };
std::vector<AboutLine> aboutLines(const AboutState& state, float width)
{
    std::vector<AboutLine> lines;
    const SkColor light = panel::text;
    lines.push_back({state.version + "  \u00B7  Free and open source (AGPL-3.0-or-later)", 12, aboutDim, false, 0});
    lines.push_back({state.credit, 12.5f, light, false, 4});
    lines.push_back({"Thank you, supporters", 14, panel::title, true, 26});
    if (state.groups.empty())
        for (const auto& line : wrapText(state.emptyText, aboutNameSize, aboutTextWidth(width)))
            lines.push_back({line, aboutNameSize, aboutDim, false, lines.size() == 3 ? 6.0f : 0.0f});
    for (const auto& [title, names] : state.groups)
    {
        lines.push_back({title, aboutTitleSize, aboutDim, true, 12});
        std::string joined;
        for (size_t i = 0; i < names.size(); ++i) joined += (i ? "   " : "") + names[i];
        for (const auto& line : wrapText(joined, aboutNameSize, aboutTextWidth(width))) lines.push_back({line, aboutNameSize, light, false, 0});
    }
    return lines;
}

float aboutButtonWidth(const std::string& label)
{
    const auto f = font(promptMessageSize, true);
    return std::max(promptButtonMinW, f.measureText(label.data(), label.size(), SkTextEncoding::kUTF8) + 30);
}

// Each content line's baseline, the update button (between the credit and the supporters) and the content's bottom.
struct AboutLayout
{
    std::vector<AboutLine> lines;
    std::vector<float> baselines;
    SkRect updateButton = SkRect::MakeEmpty();
    float bottom = 0;
};
AboutLayout aboutLayout(const AboutState& state, float width)
{
    AboutLayout layout;
    layout.lines = aboutLines(state, width);
    float y = aboutTop + 70;
    for (size_t i = 0; i < layout.lines.size(); ++i)
    {
        if (i == aboutUpdateRowBefore)
        {
            layout.updateButton = SkRect::MakeXYWH(aboutPad, y + 2, aboutButtonWidth(state.updates.label), promptButtonH);
            y = layout.updateButton.bottom() + aboutLine - 12;
        }
        y += layout.lines[i].gap;
        layout.baselines.push_back(y);
        y += aboutLine;
    }
    layout.bottom = y;
    return layout;
}
} // namespace

float aboutHeight(const AboutState& state) { return aboutLayout(state, aboutWidth).bottom + 26 + promptButtonH + 24; }

std::array<SkRect, aboutButtonCount> aboutButtonRects(const AboutState& state, float width, float height)
{
    std::array<SkRect, aboutButtonCount> rects;
    const float y = height - 24 - promptButtonH;
    float left = aboutPad, right = width - aboutPad;
    for (int i : {AboutWebsite, AboutGitHub})
    {
        const float w = aboutButtonWidth(aboutButtonLabels[(size_t) i]);
        rects[(size_t) i] = SkRect::MakeXYWH(left, y, w, promptButtonH);
        left += w + promptButtonGap;
    }
    for (int i : {AboutSupport, AboutClose})
    {
        const float w = aboutButtonWidth(aboutButtonLabels[(size_t) i]);
        rects[(size_t) i] = SkRect::MakeXYWH(right - w, y, w, promptButtonH);
        right -= w + promptButtonGap;
    }
    rects[AboutUpdates] = aboutLayout(state, width).updateButton;
    return rects;
}

void drawAbout(SkCanvas& canvas, float width, float height, const AboutState& state)
{
    const SkRect bounds = SkRect::MakeWH(width, height);
    canvas.clear(SK_ColorTRANSPARENT);
    drawPanel(canvas, bounds);
    canvas.save();
    canvas.clipRRect(panelShape(bounds), true);
    {
        // The splash's ghost mark, top right.
        const float h = 230;
        const SkRect box = SkRect::MakeXYWH(width - 40 - h * markAspect, 24, h * markAspect, h);
        drawGhostMark(canvas, box, box.bottom(), 26, 0);
    }
    drawWordmark(canvas, aboutPad, aboutTop, 34, panel::title);
    drawText(canvas, "Music production, open.", aboutPad + 2, aboutTop + 28, 14, SkColorSetRGB(0x9a, 0x9a, 0x9a));
    const auto layout = aboutLayout(state, width);
    for (size_t i = 0; i < layout.lines.size(); ++i)
    {
        const auto& line = layout.lines[i];
        drawText(canvas, line.text, aboutPad + 2, layout.baselines[i], line.size, line.color, line.bold, false, aboutTextWidth(width));
    }
    {
        // The update row: its status beside the button, centred on it, two lines at most (the rest ends in an ellipsis).
        const auto& button = layout.updateButton;
        const float x = button.right() + aboutStatusGap, maxWidth = width - aboutPad - x;
        auto status = wrapText(state.updates.status, aboutStatusSize, maxWidth);
        if (status.size() > 2)
        {
            for (size_t i = 2; i < status.size(); ++i) status[1] += " " + status[i];
            status.resize(2);
        }
        float baseline = button.centerY() + aboutStatusSize * 0.36f - (float) (status.size() > 1 ? aboutStatusLine / 2 : 0);
        for (const auto& line : status)
        {
            drawText(canvas, line, x, baseline, aboutStatusSize, aboutDim, false, false, maxWidth);
            baseline += aboutStatusLine;
        }
    }
    const auto rects = aboutButtonRects(state, width, height);
    for (int i = 0; i < aboutButtonCount; ++i)
    {
        const bool updates = i == AboutUpdates, enabled = ! updates || state.updates.enabled;
        drawButton(canvas, rects[(size_t) i], updates ? state.updates.label : std::string(aboutButtonLabels[(size_t) i]),
                   {.primary = i == AboutSupport, .hot = enabled && state.hovered == i,
                    .down = enabled && state.pressed == i && state.hovered == i, .enabled = enabled});
    }
    canvas.restore();
    drawPanelBorder(canvas, bounds);
}

void drawActivity(SkCanvas& canvas, const SkRect& box, double timeSeconds, SkColor color)
{
    constexpr int bars = 4;
    const float gap = box.width() / (bars * 2.0f - 1.0f) * 0.8f;
    const float barW = (box.width() - gap * (bars - 1)) / bars;
    SkPaint p;
    p.setAntiAlias(true);
    p.setColor(color);
    for (int i = 0; i < bars; ++i)
    {
        // Each bar swings on its own phase, like a meter following a busy signal.
        const double phase = timeSeconds * 2 * (double) pi * 1.15 - i * 0.95;
        const float level = 0.28f + 0.72f * (float) (0.5 + 0.5 * std::sin(phase) * std::cos(phase * 0.37 + i));
        const float h = std::max(barW, box.height() * level);
        const SkRect bar = SkRect::MakeXYWH(box.x() + (float) i * (barW + gap), box.centerY() - h / 2, barW, h);
        canvas.drawRoundRect(bar, barW / 2, barW / 2, p);
    }
}

void drawBusyCursor(SkCanvas& canvas, double timeSeconds)
{
    // Arrow: black with a white rim, hot spot at its tip.
    SkPathBuilder b;
    const float o = (float) busyCursorHotSpot;
    b.moveTo(o, o).lineTo(o, o + 16.5f).lineTo(o + 4.1f, o + 12.7f).lineTo(o + 6.9f, o + 19.2f).lineTo(o + 9.4f, o + 18.1f)
        .lineTo(o + 6.7f, o + 11.8f).lineTo(o + 12.2f, o + 11.8f).close();
    const auto arrow = b.detach();
    SkPaint p;
    p.setAntiAlias(true);
    p.setStyle(SkPaint::kStroke_Style);
    p.setStrokeJoin(SkPaint::kRound_Join);
    p.setStrokeWidth(2.2f);
    p.setColor(SK_ColorWHITE);
    canvas.drawPath(arrow, p);
    p.setStyle(SkPaint::kFill_Style);
    p.setColor(SkColorSetRGB(0x10, 0x10, 0x10));
    canvas.drawPath(arrow, p);

    // Badge: a dark rounded square with the activity bars, below-right of the tip.
    const SkRect badge = SkRect::MakeXYWH(15.5f, 16.5f, 14, 14);
    p.setColor(SK_ColorWHITE);
    canvas.drawRoundRect(badge.makeOutset(1.2f, 1.2f), 4.6f, 4.6f, p);
    p.setColor(SkColorSetRGB(0x14, 0x14, 0x14));
    canvas.drawRoundRect(badge, 3.6f, 3.6f, p);
    drawActivity(canvas, badge.makeInset(3.2f, 3.0f), timeSeconds, SK_ColorWHITE);
}

void drawScrubCursor(SkCanvas& canvas)
{
    const float cy = (float) scrubCursorHotSpotY;
    SkPathBuilder b; // box with the cone opening to the right
    b.moveTo(3.0f, cy - 3.2f).lineTo(7.0f, cy - 3.2f).lineTo(11.6f, cy - 7.4f).lineTo(11.6f, cy + 7.4f)
        .lineTo(7.0f, cy + 3.2f).lineTo(3.0f, cy + 3.2f).close();
    const auto speaker = b.detach();
    SkPathBuilder w; // two sound waves
    w.addArc(SkRect::MakeLTRB(9.6f, cy - 4.4f, 18.4f, cy + 4.4f), -50, 100);
    w.addArc(SkRect::MakeLTRB(9.2f, cy - 8.4f, 22.0f, cy + 8.4f), -50, 100);
    const auto waves = w.detach();

    SkPaint p;
    p.setAntiAlias(true);
    p.setStyle(SkPaint::kStroke_Style);
    p.setStrokeJoin(SkPaint::kRound_Join);
    p.setStrokeCap(SkPaint::kRound_Cap);
    p.setColor(SK_ColorWHITE);
    p.setStrokeWidth(2.2f);
    canvas.drawPath(speaker, p);
    p.setStrokeWidth(3.6f);
    canvas.drawPath(waves, p);
    p.setColor(SkColorSetRGB(0x10, 0x10, 0x10));
    p.setStrokeWidth(1.5f);
    canvas.drawPath(waves, p);
    p.setStyle(SkPaint::kFill_Style);
    canvas.drawPath(speaker, p);
}

void drawZoomCursor(SkCanvas& canvas)
{
    const float cx = (float) zoomCursorHotSpotX, cy = (float) zoomCursorHotSpotY, r = 6.0f;
    SkPathBuilder b;
    b.addCircle(cx, cy, r);
    b.moveTo(cx + r * 0.72f, cy + r * 0.72f).lineTo(21.0f, 21.0f); // handle
    const auto glass = b.detach();
    SkPaint p;
    p.setAntiAlias(true);
    p.setStyle(SkPaint::kStroke_Style);
    p.setStrokeCap(SkPaint::kRound_Cap);
    p.setColor(SK_ColorWHITE);
    p.setStrokeWidth(4.4f);
    canvas.drawPath(glass, p);
    p.setColor(SkColorSetRGB(0x10, 0x10, 0x10));
    p.setStrokeWidth(2.0f);
    canvas.drawPath(glass, p);
}

std::string markSvg(bool onIcon)
{
    if (! onIcon)
    {
        const auto strokes = markStrokes(SkRect::MakeXYWH(0, 0, 100 * markAspect, 100));
        char viewBox[48];
        std::snprintf(viewBox, sizeof viewBox, "0 0 %g 100", (double) (100 * markAspect));
        return std::string("<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"") + viewBox + "\">\n" + pathsSvg(strokes, "currentColor")
               + "</svg>\n";
    }
    // App-icon tile as a vector (for the website): the icon-grid rounded square with the S.
    const SkRect body = SkRect::MakeXYWH(0, 0, 824, 824);
    const float h = 470;
    const auto strokes = markStrokes(SkRect::MakeXYWH(412 - h * markAspect / 2, 412 - h / 2, h * markAspect, h));
    return std::string("<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 824 824\">\n")
           + "  <defs>\n"
             "    <linearGradient id=\"tile\" x1=\"0\" y1=\"0\" x2=\"0\" y2=\"1\"><stop offset=\"0\" stop-color=\"#2c2c2c\"/>"
             "<stop offset=\"1\" stop-color=\"#0c0c0c\"/></linearGradient>\n"
             "    <linearGradient id=\"ink\" gradientUnits=\"userSpaceOnUse\" x1=\"0\" y1=\"177\" x2=\"0\" y2=\"647\">"
             "<stop offset=\"0\" stop-color=\"#ffffff\"/><stop offset=\"1\" stop-color=\"#b8b8b8\"/></linearGradient>\n"
             "  </defs>\n"
             "  <path fill=\"url(#tile)\" d=\""
           + std::string(SkParsePath::ToSVGString(iconTile(body)).c_str()) + "\"/>\n" + pathsSvg(strokes, "url(#ink)")
           + "</svg>\n";
}

std::string wordmarkSvg(SkColor color)
{
    float width = 0;
    auto strokes = wordmarkStrokes(width);
    // Shift to a 0-based viewBox in units of 1/100 cap height.
    for (auto& s : strokes)
    {
        s.centerline = s.centerline.makeTransform(SkMatrix::Scale(100, 100).postTranslate(0, 100));
        s.width *= 100;
    }
    char viewBox[64];
    std::snprintf(viewBox, sizeof viewBox, "0 0 %.1f 100", (double) (width * 100));
    return std::string("<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"") + viewBox + "\">\n" + pathsSvg(strokes, hex(color))
           + "</svg>\n";
}

juce::Image renderImage(int pixelWidth, int pixelHeight, float scale, const std::function<void(SkCanvas&)>& draw)
{
    juce::Image image(juce::Image::ARGB, std::max(1, pixelWidth), std::max(1, pixelHeight), true, juce::SoftwareImageType());
    juce::Image::BitmapData data(image, juce::Image::BitmapData::writeOnly);
    // JUCE ARGB images are BGRA premultiplied in memory (see SkiaCanvasComponent::paint).
    const auto info = SkImageInfo::Make(image.getWidth(), image.getHeight(), kBGRA_8888_SkColorType, kPremul_SkAlphaType);
    if (auto surface = SkSurfaces::WrapPixels(info, data.data, (size_t) data.lineStride))
    {
        auto* canvas = surface->getCanvas();
        canvas->clear(SK_ColorTRANSPARENT);
        canvas->scale(scale, scale);
        draw(*canvas);
    }
    return image;
}

} // namespace sonora::gfx::brand
