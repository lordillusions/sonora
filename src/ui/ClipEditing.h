#pragma once
// Pure helpers for editing clips with the mouse (arrangement view, UI-3): grid, snapping and the
// geometry of move / resize / trim drags. No drawing and no state, so it is unit tested.

#include "core/Edit.h"
#include "ui/AudioClipEdits.h"

#include <algorithm>
#include <cmath>

namespace sonora::ui
{

enum class DragMode
{
    Move,
    ResizeEnd, // right edge
    TrimStart, // left edge
    FadeIn,    // audio clips: handle at the top of the left fade
    FadeOut,
    FadeInCurve,
    FadeOutCurve,
    StretchStart,
    StretchEnd
};

// Grid (in beats) matching the zoom level, like Live's adaptive grid: finer when zoomed in.
inline double gridBeatsFor(double pixelsPerBeat, double beatsPerBar)
{
    if (pixelsPerBeat >= 160.0)
        return 0.125;
    if (pixelsPerBeat >= 80.0)
        return 0.25;
    if (pixelsPerBeat >= 40.0)
        return 0.5;
    if (pixelsPerBeat >= 14.0)
        return 1.0;
    return beatsPerBar; // zoomed far out: whole bars
}

inline double snapToGrid(double beat, double grid)
{
    return grid > 0.0 ? std::round(beat / grid) * grid : beat;
}

// Which part of a clip is under x (0 .. clip width in pixels): edges are 5 px wide (less on tiny clips).
inline DragMode hitZone(float xInClip, float clipWidthPx)
{
    const float edge = std::min(9.0f, clipWidthPx * 0.25f);
    if (xInClip >= clipWidthPx - edge)
        return DragMode::ResizeEnd;
    if (xInClip < edge)
        return DragMode::TrimStart;
    return DragMode::Move;
}

// The clip as it looks while dragged by `deltaBeats` from its original state. With `snap` the
// moving edge lands on the grid (Move snaps the start, keeping the length).
inline model::Clip dragClip(const model::Clip& original, DragMode mode, double deltaBeats, double grid, bool snap, double tempo)
{
    switch (mode)
    {
        case DragMode::FadeInCurve:
        case DragMode::FadeOutCurve:
            return original; // vertical shape gesture is handled in ArrangementView
        case DragMode::StretchStart:
        case DragMode::StretchEnd:
        {
            const bool left = mode == DragMode::StretchStart;
            double edge = (left ? original.currentStart : original.currentEnd) + deltaBeats;
            if (snap) edge = snapToGrid(edge, grid);
            edge = left ? std::clamp(edge, 0.0, original.currentEnd - 0.0625) : std::max(original.currentStart + 0.0625, edge);
            const double oldLength = original.currentEnd - original.currentStart;
            if (oldLength <= 0.0 || original.kind != model::Clip::Kind::Audio) return original;
            auto clip = original.warped ? original : audioedit::toggleWarp(original, tempo);
            if (! original.warped)
            {
                const double ratio = std::pow(2.0, (original.pitchCoarse + original.pitchFine / 100.0) / 12.0);
                for (auto& marker : clip.warpMarkers) marker.beats /= ratio;
                clip.loop.loopStart /= ratio; clip.loop.loopEnd /= ratio; clip.loop.startRelative /= ratio;
            }
            const double length = left ? original.currentEnd - edge : edge - original.currentStart;
            clip = audioedit::scaleTempo(clip, length / oldLength);
            clip = model::movedClip(clip, left ? edge : original.currentStart);
            clip.fadeIn *= length / oldLength; clip.fadeOut *= length / oldLength;
            return clip; // the warp mode stays (Live): a Re-Pitch clip changes pitch as it stretches
        }
        case DragMode::Move:
        {
            double start = original.currentStart + deltaBeats;
            if (snap)
                start = snapToGrid(start, grid);
            return model::movedClip(original, start);
        }
        case DragMode::ResizeEnd:
        {
            double end = original.currentEnd + deltaBeats;
            if (snap)
                end = snapToGrid(end, grid);
            return model::withEnd(original, end);
        }
        case DragMode::FadeIn:
        case DragMode::FadeOut:
        {
            // Fade lengths are in beats from the clip's own edge; they cannot overlap each other.
            const double length = original.currentEnd - original.currentStart;
            const bool in = mode == DragMode::FadeIn;
            double fade = (in ? original.fadeIn + deltaBeats : original.fadeOut - deltaBeats);
            if (snap)
                fade = snapToGrid(fade, grid);
            fade = std::clamp(fade, 0.0, std::max(0.0, length - (in ? original.fadeOut : original.fadeIn)));
            auto c = original;
            if (fade < 0.01) fade = 0.0; // back at the automatic edge fade
            (in ? c.fadeIn : c.fadeOut) = fade;
            (in ? c.fadeInDefault : c.fadeOutDefault) = fade == 0.0;
            return c;
        }
        case DragMode::TrimStart:
        {
            double start = original.currentStart + deltaBeats;
            if (snap)
                start = snapToGrid(start, grid);
            return model::withStart(original, start, tempo);
        }
    }
    return original;
}

} // namespace sonora::ui
