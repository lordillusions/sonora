// UI-62: the Arrangement ruler's bar-number row is Live's beat-time ruler: a magnifying-glass cursor, drag down/up zooms
// in/out around the grabbed beat, sideways scrolls, double-click zooms to the time selection or the whole Arrangement,
// Shift-click still scrubs. The Overview zooms with a vertical drag too. Below the numbers stays the scrub area.
#include "TestFramework.h"
#include "io/als/AlsReader.h"
#include "ui/views/MainLayout.h"

#include <juce_core/juce_core.h>

using namespace sonora;

TEST_CASE("ruler zoom: the beat-time ruler zooms and scrolls with real mouse drags")
{
    auto loaded = io::loadAls(juce::File(SONORA_TEST_FIXTURES).getChildFile("two_tracks.xml"));
    REQUIRE(loaded);
    model::Song song = *loaded.song;
    ui::UiContext ctx;
    ctx.song = &song;
    ctx.ioVisible = false;
    ctx.browserVisible = false;
    ctx.detailVisible = false;
    ctx.mainView = ui::MainView::Arrangement;
    ctx.pixelsPerBeat = 14.0;
    ui::MainLayout root(ctx);
    root.setBounds({0, 0, 1200, 700});
    root.layout();

    const float top = ctx.theme.metrics.controlBarHeight, x0 = ctx.theme.metrics.outerMargin;
    const float numbersY = top + 22, scrubY = top + 34, overviewY = top + 8;
    auto beatX = [&](double b) { return x0 + (float) ((b - ctx.scrollBeats) * ctx.pixelsPerBeat); };
    auto mouse = [](float x, float y, int clicks = 1, bool shift = false) {
        ui::MouseEvent e; e.x = x; e.y = y; e.clicks = clicks; e.shift = shift; return e;
    };
    auto beatUnder = [&](float x) { return ctx.scrollBeats + (x - x0) / ctx.pixelsPerBeat; };

    root.dispatchMouseMove(mouse(beatX(8.0), numbersY));
    CHECK(root.cursor() == ui::Cursor::Zoom);
    root.dispatchMouseMove(mouse(beatX(40.0), scrubY));
    CHECK(root.cursor() == ui::Cursor::Scrub);

    // Drag down 48 px: twice the zoom, beat 8 stays under the pointer; sideways then scrolls; up 96 px halves the start zoom.
    const float grabX = beatX(8.0);
    const double cursorBefore = ctx.arrangementCursor;
    root.dispatchMouseDown(mouse(grabX, numbersY));
    root.dispatchMouseDrag(mouse(grabX, numbersY + 48));
    CHECK_NEAR(ctx.pixelsPerBeat, 28.0, 1e-6);
    CHECK_NEAR(beatUnder(grabX), 8.0, 1e-6);
    root.dispatchMouseDrag(mouse(grabX - 56, numbersY + 48));
    CHECK_NEAR(ctx.pixelsPerBeat, 28.0, 1e-6);
    CHECK_NEAR(beatUnder(grabX - 56), 8.0, 1e-6);
    root.dispatchMouseDrag(mouse(grabX, numbersY - 48));
    CHECK_NEAR(ctx.pixelsPerBeat, 7.0, 1e-6);
    CHECK_NEAR(ctx.scrollBeats, 0.0, 1e-9); // never scrolls before the Arrangement start
    root.dispatchMouseUp(mouse(grabX, numbersY - 48));
    CHECK_NEAR(ctx.arrangementCursor, cursorBefore, 1e-9); // zooming never moves the play position

    // Double-click without a time selection: the whole Arrangement from its start.
    root.dispatchMouseDown(mouse(beatX(4.0), numbersY, 2));
    root.dispatchMouseUp(mouse(beatX(4.0), numbersY, 2));
    CHECK_NEAR(ctx.scrollBeats, 0.0, 1e-9);
    const double whole = ctx.pixelsPerBeat;
    CHECK(whole > 1.0);

    // Double-click with a time selection: it fills the view.
    ctx.arrangementSelection = {true, 8.0, 16.0, 0, 0};
    root.dispatchMouseDown(mouse(beatX(10.0), numbersY, 2));
    root.dispatchMouseUp(mouse(beatX(10.0), numbersY, 2));
    CHECK_NEAR(ctx.scrollBeats, 8.0, 1e-9);
    CHECK(ctx.pixelsPerBeat * 8.0 > 600.0);

    // The Overview: a vertical drag zooms as well.
    ctx.arrangementSelection = {};
    const double before = ctx.pixelsPerBeat;
    root.dispatchMouseDown(mouse(x0 + 100, overviewY));
    root.dispatchMouseDrag(mouse(x0 + 100, overviewY - 48));
    CHECK_NEAR(ctx.pixelsPerBeat, before / 2, 1e-6);
    root.dispatchMouseUp(mouse(x0 + 100, overviewY - 48));

    // Shift-click on the numbers still plays from there (the scrub path), without zooming.
    const double zoom = ctx.pixelsPerBeat;
    root.dispatchMouseDown(mouse(beatX(ctx.scrollBeats + 6.0), numbersY, 1, true));
    root.dispatchMouseUp(mouse(beatX(ctx.scrollBeats + 6.0), numbersY, 1, true));
    CHECK_NEAR(ctx.pixelsPerBeat, zoom, 1e-9);
}
