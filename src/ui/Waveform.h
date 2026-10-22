#pragma once
// Draws a sample's overview inside an audio clip (UI-4). Follows the clip's warp markers, loop and
// transposition, so what is drawn is what the engine plays at that position.

#include "core/Model.h"
#include "engine/PeakCache.h"
#include "ui/Painter.h"

namespace sonora::ui
{

// `body` is the area inside the clip rectangle that receives the waveform; only the columns between
// visibleX0 and visibleX1 (same coordinate space as `body`) are drawn. `origin` is the x of the clip start.
void paintWaveform(SkCanvas& c, const engine::Peaks& peaks, const model::Clip& clip, const Rect& body, float origin,
                   float pixelsPerBeat, double tempo, float visibleX0, float visibleX1, SkColor color, float displayGain = 1.0f);

} // namespace sonora::ui
