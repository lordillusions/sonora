// UI-76: lane edits (delete / copy / paste a range, move points and segments, curves, Simplify, Insert Shape) and the
// track automation that goes along with a moved or copied Arrangement clip.
#include "TestFramework.h"

#include "core/AutomationEdits.h"
#include "core/Edit.h"

#include <cmath>

using namespace sonora;

namespace
{
model::AutomationEnvelope ramp()
{
    // 0 until beat 4, a ramp to 1 at beat 8, back to 0 at beat 12.
    model::AutomationEnvelope e;
    e.targetId = 7;
    e.points = {{-63072000.0, 0.0}, {4, 0.0}, {8, 1.0}, {12, 0.0}};
    return e;
}
const model::LaneScale linear{};
} // namespace

TEST_CASE("automation edits (UI-76): deleting a range pins the edges and keeps the lane outside")
{
    auto e = ramp();
    model::eraseEnvelopeRange(e, 6, 10, linear);
    CHECK_NEAR(model::laneValueAt(e, 5, linear), 0.25, 1e-9);  // unchanged before
    CHECK_NEAR(model::laneValueAt(e, 11, linear), 0.25, 1e-9); // unchanged after
    CHECK_NEAR(model::laneValueAt(e, 8, linear), 0.5, 1e-9);   // straight between the edge values (0.5 and 0.5)
    for (const auto& p : e.points) CHECK(! (p.time > 6 + 1e-9 && p.time < 10 - 1e-9));
    CHECK(e.points.front().time < 0); // the pre-roll stays
}

TEST_CASE("automation edits (UI-76): copy and paste a range, onto the same lane and onto a fader-law lane")
{
    auto e = ramp();
    const auto clip = model::copyEnvelopeRange(e, 4, 8, linear);
    CHECK_NEAR(clip.length, 4, 1e-12);
    REQUIRE(clip.points.size() == 2);
    CHECK_NEAR(clip.points.back().value, 1.0, 1e-6);
    model::pasteEnvelopeClip(e, 12, clip, linear);
    CHECK_NEAR(model::laneValueAt(e, 14, linear), 0.5, 1e-9);
    CHECK_NEAR(model::laneValueAt(e, 11.5, linear), 0.125, 1e-9); // the original falling ramp before the paste is kept
    CHECK_NEAR(model::laneValueAt(e, 16.5, linear), 0.0, 1e-9);   // after the paste: the value the lane had there
    // Another parameter: values map by lane position (linear 0..1 -> the fader's positions).
    model::AutomationEnvelope volume;
    volume.points = {{-63072000.0, 1.0}};
    const model::LaneScale fader{model::AutomationLaw::Fader, 0, 1.995262};
    model::pasteEnvelopeClip(volume, 0, clip, fader);
    CHECK_NEAR(fader.position(model::laneValueAt(volume, 2, fader)), 0.5, 1e-4);
}

TEST_CASE("automation edits (UI-76): moving points drops the breakpoints they pass over; segments move vertically")
{
    auto e = ramp();
    const auto moved = model::moveEnvelopePoints(e, {8}, 5.0, 0.0, linear); // past the point at 12
    REQUIRE(moved.size() == 1);
    CHECK_NEAR(moved.front(), 13, 1e-12);
    CHECK(e.points.size() == 3);
    CHECK_NEAR(e.points.back().time, 13, 1e-12);
    // A segment: both ends up by a quarter of the lane.
    auto f = ramp();
    const auto seg = model::segmentAt(f, 6);
    CHECK(seg.first == 1); CHECK(seg.second == 2);
    model::moveEnvelopePoints(f, {4, 8}, 0.0, 0.25, linear);
    CHECK_NEAR(model::laneValueAt(f, 4, linear), 0.25, 1e-9);
    CHECK_NEAR(model::laneValueAt(f, 8, linear), 1.0, 1e-9); // clamped at the top
    // Never before the song start.
    auto g = ramp();
    const auto early = model::moveEnvelopePoints(g, {4}, -10.0, 0.0, linear);
    CHECK_NEAR(early.front(), 0, 1e-12);
}

TEST_CASE("automation edits (UI-76): curves, Simplify Envelope and Insert Shape")
{
    auto e = ramp();
    model::curveSegment(e, 1, 0.8);
    CHECK(e.points[1].curved());
    CHECK(model::laneValueAt(e, 6, linear) > 0.6); // bent up: rises fast
    CHECK_NEAR(model::segmentCurvature(e.points[1]), 0.8, 1e-9);
    model::curveSegment(e, 1, 0.0);
    CHECK(! e.points[1].curved());

    model::AutomationEnvelope line;
    line.points = {{-63072000.0, 0.0}, {0, 0.0}, {1, 0.25}, {2, 0.5}, {3, 0.75}, {4, 1.0}, {5, 0.2}};
    model::simplifyEnvelope(line, 0, 100, linear);
    CHECK(line.points.size() == 4); // pre-roll, 0, 4 and the turn at 5
    CHECK_NEAR(model::laneValueAt(line, 2, linear), 0.5, 1e-9);

    auto s = ramp();
    model::insertEnvelopeShape(s, 16, 20, model::EnvelopeShape::Square, 2, 0.0, 1.0, linear);
    CHECK_NEAR(model::laneValueAt(s, 16.5, linear), 1.0, 1e-9);
    CHECK_NEAR(model::laneValueAt(s, 17.5, linear), 0.0, 1e-9);
    CHECK_NEAR(model::laneValueAt(s, 18.5, linear), 1.0, 1e-9);
    CHECK_NEAR(model::laneValueAt(s, 21, linear), 0.0, 1e-9); // after: unchanged
    auto r = ramp();
    model::insertEnvelopeShape(r, 0, 4, model::EnvelopeShape::RampUp, 1, 0.0, 1.0, linear);
    CHECK_NEAR(model::laneValueAt(r, 2, linear), 0.5, 1e-9);
}

TEST_CASE("automation edits (UI-76): value readouts and typed values")
{
    const model::LaneScale fader{model::AutomationLaw::Fader, 0, 1.995262};
    CHECK(model::formatLaneValue(1.0, fader, false, false) == "0.0 dB");
    CHECK(model::formatLaneValue(-0.5, linear, true, false) == "25L");
    CHECK(model::formatLaneValue(0.0, linear, true, false) == "C");
    CHECK(model::formatLaneValue(1.0, linear, false, true) == "On");
    double v = 0;
    CHECK(model::parseLaneValue("-6 dB", fader, false, false, v));
    CHECK_NEAR(v, std::pow(10.0, -6.0 / 20.0), 1e-9);
    CHECK(model::parseLaneValue("-inf", fader, false, false, v)); CHECK_NEAR(v, 0, 1e-12);
    CHECK(model::parseLaneValue("20R", {model::AutomationLaw::Linear, -1, 1}, true, false, v)); CHECK_NEAR(v, 0.4, 1e-9);
    CHECK(model::parseLaneValue("2.5k", {model::AutomationLaw::Log, 20, 20000}, false, false, v)); CHECK_NEAR(v, 2500, 1e-9);
    CHECK(! model::parseLaneValue("abc", linear, false, false, v));
}

TEST_CASE("clip automation (UI-76): a clip's automation moves or copies with it, to its own or another track")
{
    model::Song song;
    song.tracks.resize(2);
    for (int i = 0; i < 2; ++i)
    {
        auto& t = song.tracks[(size_t) i];
        t.kind = model::TrackKind::Audio;
        t.mixer.volumeTargetId = 100 + i * 10;
        t.mixer.panTargetId = 101 + i * 10;
    }
    model::AutomationEnvelope pan;
    pan.targetId = 101;
    pan.points = {{-63072000.0, 0.0}, {0, 0.0}, {2, 1.0}, {4, 0.0}};
    song.tracks[0].automation.push_back(pan);
    CHECK(model::trackAutomationMovesIn(song, 0, 0, 4));
    CHECK(! model::trackAutomationMovesIn(song, 0, 8, 12));
    CHECK(! model::trackAutomationMovesIn(song, 1, 0, 4));

    // Move to bar 3 of the same track: the source holds its start value, the destination gets the shape.
    model::EditHistory history;
    auto move = model::transferClipAutomation(song, 0, 0, 4, 0, 8, false);
    REQUIRE(move.apply);
    history.perform(song, std::move(move));
    CHECK_NEAR(model::laneValueAt(song.tracks[0].automation[0], 2, linear), 0.0, 1e-9);
    CHECK_NEAR(model::laneValueAt(song.tracks[0].automation[0], 10, linear), 1.0, 1e-9);
    history.undo(song);
    CHECK_NEAR(model::laneValueAt(song.tracks[0].automation[0], 2, linear), 1.0, 1e-9);

    // Copy to the other track: its pan lane is created from track 0's.
    int skipped = -1;
    auto copy = model::transferClipAutomation(song, 0, 0, 4, 1, 0, true, &skipped);
    REQUIRE(copy.apply);
    CHECK(skipped == 0);
    history.perform(song, std::move(copy));
    REQUIRE(song.tracks[1].automation.size() == 1);
    CHECK(song.tracks[1].automation[0].targetId == 111);
    CHECK_NEAR(model::laneValueAt(song.tracks[1].automation[0], 2, linear), 1.0, 1e-9);
    CHECK_NEAR(model::laneValueAt(song.tracks[0].automation[0], 2, linear), 1.0, 1e-9); // a copy keeps the source

    // Nothing moves where there is no automation.
    CHECK(! model::transferClipAutomation(song, 0, 8, 12, 0, 20, false).apply);
}
