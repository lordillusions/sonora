#pragma once
// Pure helpers for editing clips with the mouse (arrangement view, UI-3): grid, snapping and the
// geometry of move / resize / trim drags. No drawing and no state, so it is unit tested.

#include "core/ClipGeometry.h"
#include "core/Edit.h"
#include "core/TimeUtils.h"
#include "ui/AudioClipEdits.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

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
    StretchEnd,
    Gain // UI-87: audio clips, vertical drag of the title-bar gain handle (or Cmd+Option on the body)
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

// The clip as it looks while dragged by `deltaBeats` from its original state (docs/CLIP_EDITING.md). With `snap` the
// moving edge lands on the grid or on one of the magnetic `targets` (other clips' edges, locators), whichever is nearer:
// Move snaps the start, keeping the length (with `alignEnd` the end); the edges resize like Live (core/ClipGeometry: an
// unlooped clip's end marker follows, an unlooped audio clip stops at its sample's end, a looped clip repeats) and never get
// shorter than one grid step.
inline model::Clip dragClip(const model::Clip& original, DragMode mode, double deltaBeats, double grid, bool snap, double tempo,
                            bool alignEnd = false, const std::vector<double>& targets = {})
{
    const double minLength = snap && grid > 0.0 ? grid : 1.0 / 64.0;
    switch (mode)
    {
        case DragMode::FadeInCurve:
        case DragMode::FadeOutCurve:
        case DragMode::Gain:
            return original; // vertical gestures (fade shape, gain) are handled in ArrangementView
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
            // UI-86: Beats mode plays each transient slice at its own speed and only loops the slice's last 25 ms to fill,
            // so a stretched one-shot or any sample with few transients kept its length while the clip and its waveform
            // grew. Complex stretches the audio to the edge. Re-Pitch (pitch follows, like Live) and the granular modes
            // stay as they are.
            if (clip.warpMode == model::WarpMode::Beats && std::fabs(length - oldLength) > 1e-9)
                clip.warpMode = model::WarpMode::Complex;
            return clip;
        }
        case DragMode::Move:
        {
            const double length = original.currentEnd - original.currentStart;
            const double start = model::snapClipStart(original.currentStart + deltaBeats, length, snap ? grid : 0.0, targets, alignEnd);
            return model::movedClip(original, start);
        }
        case DragMode::ResizeEnd:
        {
            double end = original.currentEnd + deltaBeats;
            if (snap)
                end = model::snapWithTargets(end, grid, targets);
            return model::resizedEnd(original, end, tempo, minLength);
        }
        case DragMode::FadeIn:
        case DragMode::FadeOut:
        {
            // Fade lengths are in beats from the clip's own edge; they cannot overlap each other.
            // UI-85: from the fades as they fit the clip, so a stale longer fade can neither push the other one out nor
            // reach outside the clip.
            const double length = original.currentEnd - original.currentStart;
            const bool in = mode == DragMode::FadeIn;
            const auto fitted = model::fittedClipFades(original.fadeIn, original.fadeOut, length);
            double fade = (in ? fitted.in + deltaBeats : fitted.out - deltaBeats);
            if (snap)
                fade = snapToGrid(fade, grid);
            fade = std::clamp(fade, 0.0, std::max(0.0, length - (in ? fitted.out : fitted.in)));
            auto c = original;
            (in ? c.fadeOut : c.fadeIn) = in ? fitted.out : fitted.in;
            if (fade < 0.01) fade = 0.0; // back at the automatic edge fade
            (in ? c.fadeIn : c.fadeOut) = fade;
            (in ? c.fadeInDefault : c.fadeOutDefault) = fade == 0.0;
            return c;
        }
        case DragMode::TrimStart:
        {
            double start = original.currentStart + deltaBeats;
            if (snap)
                start = model::snapWithTargets(start, grid, targets);
            return model::trimmedStart(original, start, tempo, minLength);
        }
    }
    return original;
}

// The edge a drag places (the position the guide line and the ruler label show), -1 for the fade curve gestures.
inline double dragGuideBeat(const model::Clip& preview, DragMode mode, bool alignEnd)
{
    switch (mode)
    {
        case DragMode::Move: return alignEnd ? preview.currentEnd : preview.currentStart;
        case DragMode::ResizeEnd:
        case DragMode::StretchEnd: return preview.currentEnd;
        case DragMode::TrimStart:
        case DragMode::StretchStart: return preview.currentStart;
        case DragMode::FadeIn: return preview.currentStart + preview.fadeIn;
        case DragMode::FadeOut: return preview.currentEnd - preview.fadeOut;
        case DragMode::FadeInCurve:
        case DragMode::FadeOutCurve:
        case DragMode::Gain: break;
    }
    return -1.0;
}

// UI-87: clip gain (<SampleVolume>, the Clip view's Gain fader) dragged vertically: up is louder, 0.2 dB per pixel (`fine`:
// 0.02 dB), in the fader's range: -70 dB and below is silence (-inf), +24 dB at most. Returns the linear gain.
constexpr double kClipGainMinDb = -70.0, kClipGainMaxDb = 24.0;
inline double dragClipGain(double originalGain, float deltaY, bool fine)
{
    const double startDb = originalGain > 0.0 ? std::clamp(20.0 * std::log10(originalGain), kClipGainMinDb, kClipGainMaxDb) : kClipGainMinDb;
    const double db = std::min(kClipGainMaxDb, startDb - (double) deltaY * (fine ? 0.02 : 0.2));
    return db <= kClipGainMinDb ? 0.0 : std::pow(10.0, db / 20.0);
}

// "+3.0 dB", "0.0 dB", "-inf dB": the readout shown while the clip gain is dragged.
inline std::string clipGainText(double gain)
{
    if (gain <= 0.0 || 20.0 * std::log10(gain) <= kClipGainMinDb)
        return "-inf dB";
    const double db = 20.0 * std::log10(gain);
    char text[24];
    std::snprintf(text, sizeof(text), "%s%.1f dB", db >= 0.05 ? "+" : "", std::fabs(db) < 0.05 ? 0.0 : db);
    return text;
}

// "bar.beat.sixteenth" (1-based) of an Arrangement position, or "bars.beats.sixteenths" (0-based) of a length.
inline std::string formatBarBeat(double beats, int numerator, int denominator, bool length)
{
    const double beatUnit = 4.0 / std::max(1, denominator), beatsPerBar = std::max(1, numerator) * beatUnit;
    const bool negative = beats < -1e-9;
    const double value = std::fabs(beats) + 1e-6; // 3.9999999 is still beat 4
    const int bar = (int) std::floor(value / beatsPerBar);
    const double inBar = value - bar * beatsPerBar;
    const int beat = (int) std::floor(inBar / beatUnit);
    const int sixteenth = (int) std::floor((inBar - beat * beatUnit) / 0.25);
    const int base = length ? 0 : 1;
    char text[48];
    std::snprintf(text, sizeof(text), "%s%d.%d.%d", negative ? "-" : "", bar + base, beat + base, sixteenth + base);
    return text;
}

// The status bar's range readout (Live): where a clip or time selection starts, ends and how long it is.
inline std::string rangeStatusText(double start, double end, int numerator, int denominator)
{
    return "Start " + formatBarBeat(start, numerator, denominator, false) + "   End " + formatBarBeat(end, numerator, denominator, false)
         + "   Length " + formatBarBeat(end - start, numerator, denominator, true);
}

} // namespace sonora::ui
