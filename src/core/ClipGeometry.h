#pragma once
// UI-71: clip geometry as the reference app defines it (docs/CLIP_EDITING.md). Pure functions on model::Clip, unit tested
// in tests/test_clip_geometry.cpp; the Arrangement, the clip editor and the Clip panel all go through them.
//
// Marker storage (verified on the 65 real sets: 35,000 unlooped and 9,000 looped Arrangement MIDI clips):
//  - Loop off: LoopStart is the start marker, LoopEnd the end marker, StartRelative 0, and the Arrangement length
//    CurrentEnd - CurrentStart equals LoopEnd - LoopStart (in content units).
//  - Loop on:  LoopStart/LoopEnd are the loop brace, LoopStart + StartRelative the start marker (always inside the brace);
//    the Arrangement length is independent of the loop (the loop repeats).
// Content units are beats for MIDI and warped audio clips, seconds for unwarped audio clips.

#include "core/Model.h"

#include <optional>
#include <utility>
#include <vector>

namespace sonora::model
{

// Content units per Arrangement beat: 1, or seconds per beat for unwarped audio (0 when the tempo is unknown).
double contentUnitsPerBeat(const Clip& clip, double tempo);

// Start / end / loop markers in content units. `arrangement` clips with loop off end where their Arrangement length
// says (the end marker follows the clip's right edge); Session clips and looped clips end at LoopEnd.
struct ClipMarkers
{
    double start = 0.0, end = 4.0, loopStart = 0.0, loopEnd = 4.0;
    bool looping = true;
};
ClipMarkers clipMarkers(const Clip& clip, bool arrangement, double tempo);

// The content range the clip editor fits when a clip opens: from the earlier of the start marker and the loop start to
// the loop end (loop on) or from the start to the end marker (loop off). Never the Arrangement length of a looped clip.
std::pair<double, double> clipEditorRange(const Clip& clip, bool arrangement, double tempo);

// The view the reference app saved with the clip (<ScrollerTimePreserver LeftTime/RightTime>, content beats): it reopens a
// clip's editor there. 1,604 of 2,453 MIDI clips in the real sets hold exactly clipEditorRange (the default), 141 hold
// 0..0 (never shown), the rest a view the user zoomed to. Empty when absent, empty or no longer overlapping the clip.
std::optional<std::pair<double, double>> savedEditorRange(const Clip& clip, bool arrangement, double tempo);
// The range the clip editor shows when the clip opens: the saved view, else clipEditorRange.
std::pair<double, double> openingEditorRange(const Clip& clip, bool arrangement, double tempo);

// Writes start / end / loop markers back. Loop off: StartRelative becomes 0, LoopStart the start marker and LoopEnd the
// end marker; an Arrangement clip's right edge then follows (length = end - start), its left edge stays put. Loop on:
// the start marker is kept inside the brace and the Arrangement length does not change.
Clip withMarkers(Clip clip, const ClipMarkers& markers, bool arrangement, double tempo);

// Re-expresses an unlooped Arrangement clip the way the reference stores it (StartRelative folded into LoopStart,
// LoopEnd = start + length). Playback does not change. No-op for looped clips and unknown units.
void normaliseUnloopedMarkers(Clip& clip, double tempo);

// Audio clips: the sample's first and last position in content units (seconds, or beats through the warp markers).
// False for MIDI clips or when the sample length is unknown.
bool sampleContentRange(const Clip& clip, double tempo, double& first, double& last);

// Edge drags in the Arrangement (Live, "Moving and Resizing Clips"): the right edge sets the clip's end, the left edge
// trims its start while the content stays where it is on the timeline. An unlooped audio clip cannot reveal more than
// its sample; a looped clip repeats its loop in both directions. `minLength` is the shortest the clip may become.
Clip resizedEnd(const Clip& clip, Beats newEnd, double tempo, Beats minLength);
Clip trimmedStart(const Clip& clip, Beats newStart, double tempo, Beats minLength);

// Snapping with magnetic objects (Live: "Clips snap to the editing grid, as well as various objects in the Arrangement",
// i.e. other clips' edges and locators): the nearest of the grid line and the targets. `grid` 0 = free.
double snapWithTargets(double position, double grid, const std::vector<double>& targets);
// Where a moved clip of `length` starts: its start snaps to the grid, and either edge to the targets. `alignEnd`
// (Shift, UI-60) snaps its end to the grid instead.
double snapClipStart(double rawStart, double length, double grid, const std::vector<double>& targets, bool alignEnd);

} // namespace sonora::model
