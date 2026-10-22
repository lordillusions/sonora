#pragma once
// COL-5: drawing other participants' pointers and selections over the views (Arrangement, Session, piano roll).

#include "ui/Rect.h"
#include "ui/UiContext.h"

#include "include/core/SkCanvas.h"

#include <string>

namespace sonora::ui::collab
{
// Opacity of a cursor that has not moved for `age` seconds: full while active, then fading to a faint marker.
uint8_t alpha(float age);
SkColor colour(const CollabView::Cursor&);
// A small name tag with its left edge at x, top at y.
void tag(SkCanvas& c, float x, float y, const CollabView::Cursor& cursor, float fontSize);
// A remote pointer: a 2 px line from (x, top) to (x, bottom) and the name tag beside it.
void pointer(SkCanvas& c, float x, float top, float bottom, const CollabView::Cursor& cursor, float fontSize);
} // namespace sonora::ui::collab
