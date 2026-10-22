#pragma once
// Pure edits on audio clips used by the Clip panel (warp on/off, tempo scaling, reversing). They return a modified
// copy; the caller commits it through model::modifyClip so everything is undoable and saved.

#include "core/Model.h"

namespace sonora::ui::audioedit
{

// Tempo the warp markers describe, in BPM (project tempo when the clip has fewer than two markers).
double clipBpm(const model::Clip& clip, double songTempo);

// Warp on/off. Start/loop positions keep pointing at the same source times while their units change
// (beats <-> seconds); turning warp on creates two anchors when the clip has none.
model::Clip toggleWarp(const model::Clip& clip, double songTempo);
// First activation: build a source-tempo grid and convert seconds using it. Later toggles preserve edits.
model::Clip enableWarpWithTempo(const model::Clip&, double songTempo, double detectedBpm);

// Multiplies the clip's BPM by `factor` (the "/2" and "x2" buttons, or typing a BPM): warp-marker beats, loop positions
// and the clip length scale with it so the same part of the sample stays in use.
model::Clip scaleTempo(const model::Clip& clip, double factor);

// Mirrors markers, loop and start around the sample's centre so a reversed copy of the file plays the same region.
model::Clip mirrorForReverse(const model::Clip& clip, double sampleSeconds);

// Source-file interval (seconds) that Crop keeps: the loop region plus the start marker if it lies before the loop.
struct SourceRange
{
    double start = 0.0, end = 0.0;
};
SourceRange cropRange(const model::Clip& clip, double songTempo);

// The clip after its sample was replaced by the interval starting at `startSeconds` of the old file: markers and loop
// positions that are expressed in source seconds shift so the same audio plays.
model::Clip cropped(const model::Clip& clip, double startSeconds);

// "bar. beat. sixteenth" like Live's displays; oneBased for positions, zero-based for lengths.
std::string formatBeats(double beats, int beatsPerBar, bool oneBased);
// "m: s.mmm"
std::string formatSeconds(double seconds);

} // namespace sonora::ui::audioedit
