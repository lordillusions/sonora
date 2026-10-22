#pragma once
// Drawing helpers shared by all widgets. Thin wrappers over SkCanvas so widget code
// stays short and consistent (pixel snapping, text baseline, etc.).

#include "ui/Rect.h"

#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"

#include <string>
#include <string_view>

namespace sonora::ui
{

enum class Align
{
    Left,
    Centre,
    Right
};

namespace paint
{
// Removes the last UTF-8 code point (not just the last byte) so truncated text never ends in a broken sequence.
inline void popLastCodePoint(std::string& s)
{
    while (! s.empty() && (static_cast<unsigned char>(s.back()) & 0xC0) == 0x80) // continuation bytes
        s.pop_back();
    if (! s.empty())
        s.pop_back(); // the lead byte (or a plain ASCII character)
}

void fill(SkCanvas& c, const Rect& r, SkColor color);
void fillRounded(SkCanvas& c, const Rect& r, float radius, SkColor color);
void stroke(SkCanvas& c, const Rect& r, SkColor color, float width = 1.0f);
void line(SkCanvas& c, float x0, float y0, float x1, float y1, SkColor color, float width = 1.0f);

// Draws single-line text vertically centred in `r`, truncated with "…" when too wide.
void text(SkCanvas& c, std::string_view s, const Rect& r, SkColor color, float size, Align align = Align::Left,
          bool bold = false);

// Small triangles / icons used across the UI.
void playTriangle(SkCanvas& c, const Rect& r, SkColor color);
void stopSquare(SkCanvas& c, const Rect& r, SkColor color);
void recordCircle(SkCanvas& c, const Rect& r, SkColor color);
// TRK-5: two chain links marking a linked (alias) clip, centred in `r`.
void linkGlyph(SkCanvas& c, const Rect& r, SkColor color);

// Picks black or white text for a coloured background (clip names on clip colours).
SkColor contrastText(SkColor background);
SkColor withAlpha(SkColor c, uint8_t a);
SkColor mix(SkColor a, SkColor b, float t);
} // namespace paint

} // namespace sonora::ui
