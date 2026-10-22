#pragma once
// Grid, snapping and ruler model shared by the Arrangement, the MIDI note editor and the envelope editors. It mirrors
// Live's Options menu: Narrow/Widen Grid, Triplet Grid, Snap to Grid and Fixed Grid, plus the adaptive grid whose
// resolution follows the zoom level (Narrow / Medium / Wide). Pure functions, unit tested; nothing here draws.

#include <cmath>
#include <string>
#include <vector>

namespace sonora::ui
{

struct GridSettings
{
    enum class Density { Narrow = 0, Medium = 1, Wide = 2, Narrowest = 3, Widest = 4 };

    bool fixed = false;               // Fixed Grid: always `fixedIndex`, independent of zoom
    Density density = Density::Medium; // adaptive grid: how fine the grid gets for a given zoom
    int fixedIndex = 7;               // into kFixedGridCount sizes: 0 = 8 Bars ... 9 = 1/64 (7 = 1/16)
    bool triplet = false;             // Triplet Grid: sub-bar sizes are 2/3 as long
    bool snap = true;                 // Snap to Grid
    // An editor-wide override: hold Cmd (or Alt) while dragging to ignore the grid, like Live.
};

constexpr int kFixedGridCount = 10;

// Compact persistence for the application settings file.
int encodeGrid(const GridSettings& settings);
GridSettings decodeGrid(int value);

// Length in beats of the fixed-grid entry `index` (0 8 Bars, 1 4 Bars, 2 2 Bars, 3 1 Bar, 4 1/2, ... 9 1/64).
double fixedGridBeats(int index, double beatsPerBar);

// Grid size in beats that the settings give at this zoom (pixelsPerBeat), triplet included.
double gridBeats(const GridSettings& settings, double pixelsPerBeat, double beatsPerBar);

// "1/16", "1/8T", "1 Bar", "4 Bars" ... for the grid indicator in the corner of an editor.
std::string gridLabel(double gridBeats, double beatsPerBar);

// Narrow Grid (finer) / Widen Grid (coarser): moves the fixed size one step, or the adaptive density.
void narrowGrid(GridSettings& settings);
void widenGrid(GridSettings& settings);

// The grid to snap to given the settings and the modifier override; 0 means free (no snapping).
inline double snapSize(const GridSettings& settings, double pixelsPerBeat, double beatsPerBar, bool overrideSnap)
{
    return settings.snap && ! overrideSnap ? gridBeats(settings, pixelsPerBeat, beatsPerBar) : 0.0;
}

// ---- ruler

struct RulerMark
{
    double beat = 0.0;
    int level = 0;        // 0 bar, 1 beat, 2 and up: successively finer subdivisions
    bool labelled = false;
    std::string text;     // "3", "3.2", "3.2.3" like Live's time ruler; empty when not labelled
};

// "bar", "bar.beat", "bar.beat.sixteenth" (and a fourth part for finer positions), parts that are zero-offset omitted:
// 1, 1.2, 1.1.3. `beatUnit` is the length of a beat in quarter notes (4 / denominator).
std::string rulerText(double beat, double beatsPerBar, double beatUnit);

// Marks to draw for the visible range: labelled marks at the finest level whose text fits, unlabelled ticks at finer
// levels while they stay at least `minTickPixels` apart.
std::vector<RulerMark> rulerMarks(double firstBeat, double lastBeat, double pixelsPerBeat, double beatsPerBar, double beatUnit,
                                  float charWidth = 6.0f, float minTickPixels = 9.0f);

// Vertical grid lines for the lanes / note area: the grid size (and coarser multiples) with a strength of
// 2 bar line, 1 beat line, 0 sub-beat line. `grid` is the effective grid in beats.
struct GridLine
{
    double beat = 0.0;
    int strength = 0;
};
std::vector<GridLine> gridLines(double firstBeat, double lastBeat, double pixelsPerBeat, double beatsPerBar, double beatUnit,
                                double grid);

} // namespace sonora::ui
