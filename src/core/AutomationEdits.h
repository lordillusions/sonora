#pragma once
// UI-76: envelope edits on one automation lane, as the reference's lanes offer them (Live 12 manual, "Automation and Editing
// Envelopes"): delete / copy / paste / duplicate a time range of the lane, move a segment or a selection, curve a segment,
// Simplify Envelope and Insert Shape. Pure model code (tested in tests/test_automation_edits.cpp); the editor in
// ui/views/AutomationEditor drives it.
//
// Values live where the control puts them on its lane (core/AutomationLaw.h): every function that interpolates takes the
// lane's scale, so an edge value pinned by an edit is exactly what the engine plays there. The pre-roll point (time < 0)
// is never removed or moved.

#include "core/AutomationLaw.h"
#include "core/Model.h"

#include <string>
#include <vector>

namespace sonora::model
{

struct LaneScale
{
    AutomationLaw law = AutomationLaw::Linear;
    double min = 0.0, max = 1.0;
    double position(double value) const { return automationPosition(law, value, min, max); }
    double value(double position) const { return automationValue(law, position, min, max); }
};

// The lane's value at `beat`, as engine::AutomationLane plays it (curves, steps, jumps, the law's position space).
double laneValueAt(const AutomationEnvelope& envelope, Beats beat, const LaneScale& scale);

// A copied stretch of one lane (the automation clipboard): points relative to the range start, the first at 0 and the
// last at `length` (the edge values are always included). Pasting onto another parameter maps by lane position.
struct EnvelopeClip
{
    double length = 0.0;
    std::vector<AutomationPoint> points;
    bool stepped = false;
    LaneScale scale;
    bool empty() const { return length <= 0.0 || points.empty(); }
};
EnvelopeClip copyEnvelopeRange(const AutomationEnvelope& envelope, Beats start, Beats end, const LaneScale& scale);

// Live's Delete on a time selection in a lane: the breakpoints inside [start, end] go, the values just outside both edges
// are pinned there so the lane outside is unchanged, and inside the line runs straight between them.
void eraseEnvelopeRange(AutomationEnvelope& envelope, Beats start, Beats end, const LaneScale& scale);

// Paste: [at, at + clip.length] is replaced by the clip (values mapped through lane positions when the scales differ),
// with the lane's own values pinned at both edges (a jump where they differ, nothing extra where they agree).
void pasteEnvelopeClip(AutomationEnvelope& envelope, Beats at, const EnvelopeClip& clip, const LaneScale& scale);

// Moves the points at `times` (exact breakpoint times, each moved once; the pre-roll stays) by `deltaBeats` and
// `deltaPosition` on the lane (clamped to 0..1; stepped lanes round to whole values). Breakpoints the moved block passes
// over horizontally are removed, as in the reference. Returns the new times of the moved points.
std::vector<Beats> moveEnvelopePoints(AutomationEnvelope& envelope, const std::vector<Beats>& times, double deltaBeats,
                                      double deltaPosition, const LaneScale& scale);

// The segment that holds `beat` (between breakpoints a and b = a + 1): its indices, or {-1, -1} before the first / after
// the last breakpoint (there the lane is flat to the pre-roll / end, and "the segment" is that flat run).
std::pair<int, int> segmentAt(const AutomationEnvelope& envelope, Beats beat);

// Live's Option-drag on a segment: bends the segment that starts at breakpoint `index` (amount -1..1; 0 straight).
void curveSegment(AutomationEnvelope& envelope, int index, double amount);
double segmentCurvature(const AutomationPoint& start);

// Simplify Envelope: removes breakpoints inside [start, end] that the line through their neighbours already passes
// within `tolerance` (lane position units) of. Curved segments and jumps are kept.
void simplifyEnvelope(AutomationEnvelope& envelope, Beats start, Beats end, const LaneScale& scale, double tolerance = 0.004);

// Insert Shape over [start, end]. The ramps and ADSR run from the lane's value before the selection; the waveforms swing
// between `low` and `high` lane positions, `cycles` times.
enum class EnvelopeShape
{
    Sine, Triangle, Sawtooth, InverseSawtooth, Square, RampUp, RampDown, Adsr
};
const char* envelopeShapeName(EnvelopeShape);
void insertEnvelopeShape(AutomationEnvelope& envelope, Beats start, Beats end, EnvelopeShape shape, int cycles, double low,
                         double high, const LaneScale& scale);

// Breakpoints (time >= 0) inside [start, end].
std::vector<Beats> envelopePointTimes(const AutomationEnvelope& envelope, Beats start, Beats end);
// Whether the lane has a breakpoint (time >= 0) in [start, end) or its value changes there.
bool envelopeMovesIn(const AutomationEnvelope& envelope, Beats start, Beats end);

// A short text for a lane value: dB for fader-law lanes, L/C/R for pan, On/Off for switches, else 3 significant digits.
std::string formatLaneValue(double value, const LaneScale& scale, bool pan, bool toggle, const std::string& unit = {});
// The value typed into Edit Value (the same notations; "-inf" for silence). False when the text is not a number.
bool parseLaneValue(const std::string& text, const LaneScale& scale, bool pan, bool toggle, double& value);

} // namespace sonora::model
