// UI-60: a dragged clip edge draws a line up to the ruler with its position (Live); Shift while moving snaps the clip's
// end instead of its start; the status bar reads out the selected clip's start, end and length.
#include "TestFramework.h"
#include "gfx/SkiaCanvasComponent.h"
#include "io/als/AlsReader.h"
#include "ui/ClipEditing.h"
#include "ui/views/MainLayout.h"

#include <juce_core/juce_core.h>
#include <juce_graphics/juce_graphics.h>

#include <cstdlib>

using namespace sonora;

TEST_CASE("drag guide: bar.beat formatting, end-aligned moves and the guide edge")
{
    CHECK(ui::formatBarBeat(0.0, 4, 4, false) == "1.1.1");
    CHECK(ui::formatBarBeat(37.25, 4, 4, false) == "10.2.2");
    CHECK(ui::formatBarBeat(8.0, 4, 4, true) == "2.0.0");
    CHECK(ui::formatBarBeat(2.75, 4, 4, true) == "0.2.3");
    CHECK(ui::formatBarBeat(3.0, 3, 4, false) == "2.1.1"); // 3/4: bar 2 starts at beat 3
    CHECK(ui::formatBarBeat(3.0, 6, 8, false) == "2.1.1"); // 6/8: eighth-note beats, 3 quarter notes per bar
    CHECK(ui::rangeStatusText(4.0, 12.0, 4, 4) == "Start 2.1.1   End 4.1.1   Length 2.0.0");

    model::Clip c;
    c.kind = model::Clip::Kind::Midi;
    c.currentStart = 4.0;
    c.currentEnd = 7.5;
    c.loop = {0.0, 3.5, 0.0, true};
    auto byStart = ui::dragClip(c, ui::DragMode::Move, 2.3, 1.0, true, 120.0);
    CHECK_NEAR(byStart.currentStart, 6.0, 1e-9);
    CHECK_NEAR(byStart.currentEnd, 9.5, 1e-9);
    auto byEnd = ui::dragClip(c, ui::DragMode::Move, 2.3, 1.0, true, 120.0, true);
    CHECK_NEAR(byEnd.currentEnd, 10.0, 1e-9); // 9.8 snaps to 10, the length stays 3.5
    CHECK_NEAR(byEnd.currentStart, 6.5, 1e-9);
    auto early = ui::dragClip(c, ui::DragMode::Move, -10.0, 1.0, true, 120.0, true);
    CHECK_NEAR(early.currentStart, 0.0, 1e-9); // never before the timeline start
    CHECK_NEAR(ui::dragGuideBeat(byEnd, ui::DragMode::Move, true), 10.0, 1e-9);
    CHECK_NEAR(ui::dragGuideBeat(byEnd, ui::DragMode::Move, false), 6.5, 1e-9);
    CHECK_NEAR(ui::dragGuideBeat(byEnd, ui::DragMode::ResizeEnd, false), 10.0, 1e-9);
    CHECK_NEAR(ui::dragGuideBeat(byEnd, ui::DragMode::TrimStart, false), 6.5, 1e-9);
    CHECK(ui::dragGuideBeat(byEnd, ui::DragMode::FadeInCurve, false) < 0);
}

TEST_CASE("drag guide: real mouse drags snap by the end with Shift and the status bar reads out the clip")
{
    auto loaded = io::loadAls(juce::File(SONORA_TEST_FIXTURES).getChildFile("two_tracks.xml"));
    REQUIRE(loaded);
    model::Song song = *loaded.song;
    REQUIRE(! song.tracks[0].arrangementClips.empty());
    song.tracks[0].arrangementClips[0] = model::withEnd(song.tracks[0].arrangementClips[0], 11.5); // 4 .. 11.5, off the grid
    model::EditHistory history;
    ui::UiContext ctx;
    ctx.song = &song;
    ctx.ioVisible = false;
    ctx.browserVisible = false;
    ctx.detailVisible = false;
    ctx.mainView = ui::MainView::Arrangement;
    ctx.pixelsPerBeat = 14.0;
    ctx.applyEdit = [&](model::EditCommand cmd) { history.perform(song, std::move(cmd)); };
    ctx.undo = [&] { history.undo(song); };
    ui::MainLayout root(ctx);
    root.setBounds({0, 0, 1200, 700});
    root.layout();

    const float laneTop = ctx.theme.metrics.controlBarHeight + ctx.theme.metrics.arrangementRulerHeight;
    auto beatX = [&](double b) { return ctx.theme.metrics.outerMargin + (float) (b * ctx.pixelsPerBeat); };
    auto mouse = [](float x, float y, bool shift = false) { ui::MouseEvent e; e.x = x; e.y = y; e.shift = shift; return e; };
    const auto clipUid = song.tracks[0].arrangementClips[0].uid;

    // A click selects the clip: the status bar's readout is its range.
    root.dispatchMouseDown(mouse(beatX(8.0), laneTop + 30));
    root.dispatchMouseUp(mouse(beatX(8.0), laneTop + 30));
    REQUIRE(ctx.selectedClipUid == clipUid);
    REQUIRE(ctx.statusRange().has_value());
    CHECK_NEAR(ctx.statusRange()->first, 4.0, 1e-9);
    CHECK_NEAR(ctx.statusRange()->second, 11.5, 1e-9);

    // Moving by the title bar with Shift held: the end lands on the grid (11.5 + 2.3 = 13.8 -> 14).
    root.dispatchMouseDown(mouse(beatX(8.0), laneTop + 6));
    root.dispatchMouseDrag(mouse(beatX(10.3), laneTop + 6, true));
    REQUIRE(ctx.dragRange.has_value());
    CHECK_NEAR(ctx.dragRange->second, 14.0, 1e-9);
    CHECK_NEAR(ctx.statusRange()->first, 6.5, 1e-9); // the readout follows the drag
    const auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("sonora_tests");
    dir.createDirectory();
    CHECK(gfx::renderToPng(root, 1200, 700, 2, dir.getChildFile("drag_guide_end.png").getFullPathName().toStdString()));
    root.dispatchMouseUp(mouse(beatX(10.3), laneTop + 6, true));
    CHECK(! ctx.dragRange.has_value());
    CHECK_NEAR(song.tracks[0].arrangementClips[0].currentStart, 6.5, 1e-9);
    CHECK_NEAR(song.tracks[0].arrangementClips[0].currentEnd, 14.0, 1e-9);

    // Without Shift the start snaps (6.5 + 2.3 = 8.8 -> 9).
    root.dispatchMouseDown(mouse(beatX(10.0), laneTop + 6));
    root.dispatchMouseDrag(mouse(beatX(12.3), laneTop + 6));
    CHECK(gfx::renderToPng(root, 1200, 700, 2, dir.getChildFile("drag_guide_start.png").getFullPathName().toStdString()));
    root.dispatchMouseUp(mouse(beatX(12.3), laneTop + 6));
    CHECK_NEAR(song.tracks[0].arrangementClips[0].currentStart, 9.0, 1e-9);

    // Resizing the end shows the guide at the end (16.5 + 1.2 = 17.7 -> 18).
    const float rightEdge = beatX(16.5) - 2;
    root.dispatchMouseDown(mouse(rightEdge, laneTop + 6));
    root.dispatchMouseDrag(mouse(rightEdge + (float) (1.2 * ctx.pixelsPerBeat), laneTop + 6));
    REQUIRE(ctx.dragRange.has_value());
    CHECK_NEAR(ctx.dragRange->second, 18.0, 1e-9);
    CHECK(gfx::renderToPng(root, 1200, 700, 2, dir.getChildFile("drag_guide_resize.png").getFullPathName().toStdString()));
    root.dispatchMouseUp(mouse(rightEdge + (float) (1.2 * ctx.pixelsPerBeat), laneTop + 6));
    CHECK_NEAR(song.tracks[0].arrangementClips[0].currentEnd, 18.0, 1e-9);
}

TEST_CASE("deactivated clip: faded in a white frame; Main shows no empty name field")
{
    auto loaded = io::loadAls(juce::File(SONORA_TEST_FIXTURES).getChildFile("two_tracks.xml"));
    REQUIRE(loaded);
    model::Song song = *loaded.song;
    REQUIRE(! song.tracks[0].arrangementClips.empty());
    ui::UiContext ctx;
    ctx.song = &song;
    ctx.ioVisible = ctx.browserVisible = ctx.detailVisible = false;
    ctx.mainView = ui::MainView::Arrangement;
    ctx.pixelsPerBeat = 14.0;
    ui::MainLayout root(ctx);
    root.setBounds({0, 0, 1200, 700});
    root.layout();
    const auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("sonora_tests");
    dir.createDirectory();
    const float laneTop = ctx.theme.metrics.controlBarHeight + ctx.theme.metrics.arrangementRulerHeight;
    const int px = (int) (ctx.theme.metrics.outerMargin + 8.0 * ctx.pixelsPerBeat), py = (int) laneTop + 40;
    auto pixelAt = [&](const char* name) {
        const auto file = dir.getChildFile(name);
        REQUIRE(gfx::renderToPng(root, 1200, 700, 1, file.getFullPathName().toStdString()));
        return juce::ImageFileFormat::loadFrom(file).getPixelAt(px, py);
    };
    const auto active = pixelAt("clip_active.png");
    song.tracks[0].arrangementClips[0].disabled = true;
    root.repaint();
    const auto faded = pixelAt("clip_deactivated.png");
    const auto clipColour = ui::Theme::paletteColor(song.tracks[0].arrangementClips[0].colorIndex);
    CHECK(std::abs((int) active.getGreen() - (int) SkColorGetG(clipColour)) <= 2); // drawn in its colour while active
    const int change = std::abs((int) active.getRed() - (int) faded.getRed()) + std::abs((int) active.getGreen() - (int) faded.getGreen())
                     + std::abs((int) active.getBlue() - (int) faded.getBlue());
    CHECK(change > 60); // the lane shows through a deactivated clip
}
