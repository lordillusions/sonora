#pragma once
// UI-57: the view state a set is saved with (model::Song::View plus each track's lane height, fold and Session width),
// as the reference app stores it, so a set reopens the way it was left: same main view, Arrangement zoom and scroll,
// Session scroll, track heights and widths, detail view. Live opens sets saved by Sonora the same way.

namespace sonora::model { struct Song; }

namespace sonora::ui
{
struct UiContext;

// Puts the view state of the freshly loaded `ctx.song` into the context (main view, zoom, scroll, heights, widths).
void applySetView(UiContext& ctx);

// Writes the context's current view state into `song` (the set about to be saved). Values the user did not change are
// left exactly as they were read, so the reference's own numbers survive a load/save round trip.
void captureSetView(const UiContext& ctx, model::Song& song);

// The reference's LaneHeight / ViewStateSessionTrackWidth leave out the 2-point separator Sonora's sizes include
// (Live 85 ↔ 87, 93 ↔ 95, measured from captures). Live snaps lane heights to 17-point steps.
inline constexpr float kReferenceSeparator = 2.0f;
inline constexpr int kReferenceLaneStep = 17;

} // namespace sonora::ui
