#include "ui/views/CollabVideo.h"

#include "gfx/Fonts.h"
#include "ui/Painter.h"
#include "ui/UiContext.h"

#include "include/core/SkCanvas.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPixmap.h"
#include "include/core/SkRRect.h"

namespace sonora::ui
{

void CollabVideoTiles::paint(SkCanvas& c, const Theme& t)
{
    const auto& tiles = ctx().collab.videoTiles;
    std::map<int, Cached> kept;
    float y = 0.0f;
    for (size_t i = 0; i < tiles.size(); ++i, y += tileH + gap)
    {
        const auto& tile = tiles[i];
        const SkRect dst = SkRect::MakeXYWH(0, y, tileW, tileH);
        const auto rounded = SkRRect::MakeRectXY(dst, 8, 8);
        SkPaint p;
        p.setAntiAlias(true);
        p.setColor(SkColorSetRGB(20, 20, 22));
        c.drawRRect(rounded, p);
        // The frame as a Skia image, made again only when a new one arrived.
        auto cached = images_[tile.id];
        if (tile.pixels && tile.width > 0 && tile.height > 0 && tile.pixels->size() >= (size_t) tile.width * (size_t) tile.height
            && (cached.serial != tile.serial || ! cached.image))
        {
            const SkPixmap pixmap(SkImageInfo::Make(tile.width, tile.height, kBGRA_8888_SkColorType, kOpaque_SkAlphaType), tile.pixels->data(),
                                  (size_t) tile.width * 4);
            cached.image = SkImages::RasterFromPixmapCopy(pixmap);
            cached.serial = tile.serial;
        }
        if (cached.image)
        {
            c.save();
            c.clipRRect(rounded, true);
            // Cover the tile (crop the longer side), mirrored for this person's own preview.
            const float sx = tileW / (float) cached.image->width(), sy = tileH / (float) cached.image->height(), s = std::max(sx, sy);
            const float w = (float) cached.image->width() * s, h = (float) cached.image->height() * s;
            if (tile.self)
            {
                c.translate(dst.right(), 0);
                c.scale(-1.0f, 1.0f);
                c.drawImageRect(cached.image, SkRect::MakeXYWH((tileW - w) * 0.5f, y + (tileH - h) * 0.5f, w, h), SkSamplingOptions(SkFilterMode::kLinear));
            }
            else
                c.drawImageRect(cached.image, SkRect::MakeXYWH((tileW - w) * 0.5f, y + (tileH - h) * 0.5f, w, h), SkSamplingOptions(SkFilterMode::kLinear));
            c.restore();
        }
        else paint::text(c, "Waiting for video...", {0, y, tileW, tileH}, SkColorSetRGB(170, 170, 175), t.metrics.fontSize - 1, Align::Centre);
        kept[tile.id] = cached;

        // Name chip in the person's colour; a green frame while they talk.
        const auto name = tile.self ? std::string("You") : tile.name;
        const float chipW = std::min(tileW - 12, gfx::measureText(name, t.metrics.fontSize - 1, true) + (tile.micOn ? 26.0f : 14.0f));
        const Rect chip{6, y + tileH - 24, chipW, 18};
        p.setColor(paint::withAlpha(collabColor(tile.color), 220));
        c.drawRRect(SkRRect::MakeRectXY(SkRect::MakeXYWH(chip.x, chip.y, chip.w, chip.h), 5, 5), p);
        paint::text(c, name, {chip.x + 7, chip.y, chip.w - 7, chip.h}, SK_ColorWHITE, t.metrics.fontSize - 1, Align::Left, true);
        if (tile.micOn)
        {
            const float mx = chip.right() - 14, cy = chip.centreY();
            p.setColor(SK_ColorWHITE);
            c.drawRRect(SkRRect::MakeRectXY(SkRect::MakeXYWH(mx + 2, cy - 6, 5, 8), 2.5f, 2.5f), p);
            p.setStyle(SkPaint::kStroke_Style);
            p.setStrokeWidth(1.0f);
            c.drawArc(SkRect::MakeXYWH(mx, cy - 3, 9, 7), 0, 180, false, p);
            p.setStyle(SkPaint::kFill_Style);
        }
        if (tile.talking)
        {
            p.setStyle(SkPaint::kStroke_Style);
            p.setStrokeWidth(3.0f);
            p.setColor(SkColorSetRGB(39, 174, 96));
            c.drawRRect(SkRRect::MakeRectXY(dst.makeInset(1.5f, 1.5f), 7, 7), p);
            p.setStyle(SkPaint::kFill_Style);
        }
        if (i == 0)
        {
            // Hide all tiles on this machine.
            hideButton_ = {tileW - 24, y + 6, 18, 18};
            p.setColor(paint::withAlpha(SkColorSetRGB(0, 0, 0), 150));
            c.drawCircle(hideButton_.centreX(), hideButton_.centreY(), 9, p);
            p.setColor(SK_ColorWHITE);
            p.setStrokeWidth(1.6f);
            p.setStyle(SkPaint::kStroke_Style);
            c.drawLine(hideButton_.centreX() - 3.5f, hideButton_.centreY() - 3.5f, hideButton_.centreX() + 3.5f, hideButton_.centreY() + 3.5f, p);
            c.drawLine(hideButton_.centreX() + 3.5f, hideButton_.centreY() - 3.5f, hideButton_.centreX() - 3.5f, hideButton_.centreY() + 3.5f, p);
        }
    }
    images_ = std::move(kept); // people who left take their images with them
}

void CollabVideoTiles::mouseDown(const MouseEvent& e)
{
    auto& x = ctx();
    if (hideButton_.contains(e.x, e.y) && x.collabShowVideo) x.collabShowVideo(false);
}

} // namespace sonora::ui
