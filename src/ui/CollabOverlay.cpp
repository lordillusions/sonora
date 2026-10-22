#include "ui/CollabOverlay.h"

#include "ui/Painter.h"

#include <algorithm>

namespace sonora::ui::collab
{

uint8_t alpha(float age)
{
    if (age < 8.0f) return 255;
    return static_cast<uint8_t>(std::max(90.0f, 255.0f - (age - 8.0f) * 15.0f));
}

SkColor colour(const CollabView::Cursor& cursor) { return paint::withAlpha(collabColor(cursor.color), alpha(cursor.age)); }

void tag(SkCanvas& c, float x, float y, const CollabView::Cursor& cursor, float fontSize)
{
    const float size = fontSize - 1.0f;
    const float w = std::min(140.0f, 10.0f + size * 0.58f * static_cast<float>(cursor.name.size()));
    const Rect r{x, y, w, size + 5.0f};
    paint::fillRounded(c, r, 3.0f, colour(cursor));
    paint::text(c, cursor.name, r.reduced(4, 0), paint::withAlpha(SK_ColorWHITE, alpha(cursor.age)), size, Align::Left, true);
}

void pointer(SkCanvas& c, float x, float top, float bottom, const CollabView::Cursor& cursor, float fontSize)
{
    paint::line(c, x, top, x, bottom, colour(cursor), 2.0f);
    tag(c, x + 2.0f, top + 1.0f, cursor, fontSize);
}

} // namespace sonora::ui::collab
