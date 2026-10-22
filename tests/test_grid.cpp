#include "TestFramework.h"

#include "ui/GridModel.h"

#include <algorithm>

using namespace sonora;

namespace
{
std::vector<std::string> labels(const std::vector<ui::RulerMark>& marks)
{
    std::vector<std::string> out;
    for (const auto& m : marks)
        if (m.labelled)
            out.push_back(m.text);
    return out;
}
bool contains(const std::vector<std::string>& v, const std::string& s) { return std::find(v.begin(), v.end(), s) != v.end(); }
} // namespace

TEST_CASE("grid: the adaptive grid follows the zoom like Live (measured at 3.25, 8, 34 and 141 px per beat)")
{
    ui::GridSettings g;
    CHECK_NEAR(ui::gridBeats(g, 3.25, 4.0), 4.0, 1e-9);   // 1/1 (one bar)
    CHECK_NEAR(ui::gridBeats(g, 8.2, 4.0), 2.0, 1e-9);    // 1/2
    CHECK_NEAR(ui::gridBeats(g, 34.0, 4.0), 0.5, 1e-9);   // 1/8
    CHECK_NEAR(ui::gridBeats(g, 141.0, 4.0), 0.125, 1e-9); // 1/32
    CHECK(ui::gridLabel(4.0, 4.0) == "1/1");
    CHECK(ui::gridLabel(0.5, 4.0) == "1/8");
    CHECK(ui::gridLabel(0.125, 4.0) == "1/32");
    CHECK(ui::gridLabel(16.0, 4.0) == "4 Bars");
    CHECK(ui::gridLabel(3.0, 3.0) == "1 Bar");

    // Zoomed far out the grid is a multiple of bars.
    CHECK(ui::gridBeats(g, 0.8, 4.0) >= 16.0);
    // Narrow / Wide change how fine the grid gets for the same zoom
    ui::GridSettings narrow = g, wide = g;
    narrow.density = ui::GridSettings::Density::Narrow;
    wide.density = ui::GridSettings::Density::Wide;
    CHECK(ui::gridBeats(narrow, 34.0, 4.0) < ui::gridBeats(g, 34.0, 4.0));
    CHECK(ui::gridBeats(wide, 34.0, 4.0) > ui::gridBeats(g, 34.0, 4.0));
}

TEST_CASE("grid: fixed grid ignores zoom, triplets are 2/3 of sub-bar sizes, narrow/widen step the size")
{
    ui::GridSettings g;
    g.fixed = true;
    g.fixedIndex = 7; // 1/16
    CHECK_NEAR(ui::gridBeats(g, 3.0, 4.0), 0.25, 1e-9);
    CHECK_NEAR(ui::gridBeats(g, 400.0, 4.0), 0.25, 1e-9);
    g.fixedIndex = 3;
    CHECK_NEAR(ui::gridBeats(g, 50.0, 4.0), 4.0, 1e-9);
    CHECK_NEAR(ui::gridBeats(g, 50.0, 3.0), 3.0, 1e-9); // 1 Bar of 3/4
    g.fixedIndex = 6; // 1/8
    g.triplet = true;
    CHECK_NEAR(ui::gridBeats(g, 50.0, 4.0), 0.5 * 2.0 / 3.0, 1e-9);
    CHECK(ui::gridLabel(ui::gridBeats(g, 50.0, 4.0), 4.0) == "1/8T");
    g.triplet = false;
    ui::narrowGrid(g);
    CHECK(g.fixedIndex == 7);
    ui::widenGrid(g);
    ui::widenGrid(g);
    CHECK(g.fixedIndex == 5);
    for (int i = 0; i < 20; ++i) ui::narrowGrid(g);
    CHECK(g.fixedIndex == ui::kFixedGridCount - 1);

    CHECK_NEAR(ui::snapSize(g, 50.0, 4.0, false), 0.0625, 1e-9);
    CHECK_NEAR(ui::snapSize(g, 50.0, 4.0, true), 0.0, 1e-9);   // modifier override
    g.snap = false;
    CHECK_NEAR(ui::snapSize(g, 50.0, 4.0, false), 0.0, 1e-9);  // Snap to Grid off
}

TEST_CASE("ruler: graduation labels match Live (1, 1.2, 1.3 ... and 1.1.3 when zoomed in)")
{
    CHECK(ui::rulerText(0.0, 4.0, 1.0) == "1");
    CHECK(ui::rulerText(4.0, 4.0, 1.0) == "2");
    CHECK(ui::rulerText(1.0, 4.0, 1.0) == "1.2");
    CHECK(ui::rulerText(6.0, 4.0, 1.0) == "2.3");
    CHECK(ui::rulerText(0.5, 4.0, 1.0) == "1.1.3");
    CHECK(ui::rulerText(0.25, 4.0, 1.0) == "1.1.2");
    CHECK(ui::rulerText(4.75, 4.0, 1.0) == "2.1.4");
    CHECK(ui::rulerText(0.0625, 4.0, 1.0) == "1.1.1.2");

    // 3.25 px/beat: every fourth bar, 1 5 9 ... (Live shows 1, 5, 9)
    const auto far = labels(ui::rulerMarks(0, 80, 3.25, 4.0, 1.0));
    REQUIRE(far.size() >= 5);
    CHECK(far[0] == "1");
    CHECK(far[1] == "5");
    CHECK(far[2] == "9");
    // 8 px/beat: every bar
    const auto bars = labels(ui::rulerMarks(0, 40, 8.2, 4.0, 1.0));
    CHECK(contains(bars, "2"));
    CHECK(contains(bars, "3"));
    CHECK(! contains(bars, "1.2"));
    // 34 px/beat: bars and beats
    const auto beats = labels(ui::rulerMarks(0, 12, 34.0, 4.0, 1.0));
    CHECK(contains(beats, "1.2"));
    CHECK(contains(beats, "1.3"));
    CHECK(contains(beats, "2"));
    CHECK(! contains(beats, "1.1.3"));
    // 141 px/beat: eighths are labelled too
    const auto eighths = labels(ui::rulerMarks(0, 4, 141.0, 4.0, 1.0));
    CHECK(contains(eighths, "1.1.3"));
    CHECK(contains(eighths, "1.2"));
    CHECK(! contains(eighths, "1.1.2"));

    // unlabelled ticks fill in the finer levels while they are at least 9 px apart (1/32 at 141 px per beat)
    const auto marks = ui::rulerMarks(0, 1, 141.0, 4.0, 1.0);
    int ticks = 0;
    for (const auto& m : marks) if (m.beat >= 0.0 && m.beat < 1.0 - 1e-9) ++ticks;
    CHECK(ticks == 8);
}

TEST_CASE("ruler: grid lines carry bar / beat / sub-beat strength and a fixed coarse grid still shows bars")
{
    const auto lines = ui::gridLines(0, 4, 100.0, 4.0, 1.0, 0.5);
    REQUIRE(lines.size() >= 8);
    CHECK(lines[0].strength == 2);
    CHECK(lines[1].strength == 0);
    CHECK(lines[2].strength == 1);
    int bars = 0;
    for (const auto& l : ui::gridLines(0, 40, 100.0, 4.0, 1.0, 32.0)) bars += l.strength == 2;
    CHECK(bars >= 10);
}
