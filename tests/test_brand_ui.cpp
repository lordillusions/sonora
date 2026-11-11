#include "TestFramework.h"
#include <cmath>

#include "app/AboutUpdates.h"
#include "gfx/Brand.h"
#include "ui/UiContext.h"

#include "include/core/SkCanvas.h"
#include "include/core/SkPixmap.h"
#include "include/core/SkSurface.h"

using namespace sonora;

TEST_CASE("brand: busy tasks stack, update and end by id (UI-51)")
{
    ui::UiContext ctx;
    CHECK(ctx.currentBusy() == nullptr);
    CHECK(! ctx.waitingForBusy());
    const int scan = ctx.beginBusy("Scanning plug-ins...", true);
    CHECK(ctx.currentBusy() != nullptr);
    CHECK(! ctx.waitingForBusy()); // background work keeps the normal cursor
    const int open = ctx.beginBusy("Opening a.als...");
    CHECK(ctx.waitingForBusy());
    CHECK(ctx.currentBusy()->text == "Opening a.als...");
    ctx.updateBusy(open, "Starting plug-ins 1 of 2: X", 0.0f);
    CHECK(ctx.currentBusy()->progress == 0.0f);
    ctx.endBusy(open);
    CHECK(! ctx.waitingForBusy());
    CHECK(ctx.currentBusy()->id == scan); // the older task shows again
    ctx.endBusy(open); // ending twice is harmless
    ctx.endBusy(scan);
    CHECK(ctx.currentBusy() == nullptr);
}

TEST_CASE("brand: mark and wordmark geometry stay inside their boxes")
{
    const auto box = SkRect::MakeXYWH(10, 20, 100 * gfx::brand::markAspect, 100);
    for (const auto& stroke : gfx::brand::markStrokes(box))
    {
        const auto bounds = gfx::brand::outline(stroke).getBounds();
        CHECK(bounds.left() >= box.left() - 0.01f);
        CHECK(bounds.right() <= box.right() + 0.01f);
        CHECK(bounds.top() >= box.top() - 0.01f);
        CHECK(bounds.bottom() <= box.bottom() + 0.01f);
    }
    float width = 0;
    const auto word = gfx::brand::wordmarkStrokes(width);
    CHECK(word.size() >= 6); // S o n(2) o r(2) a(2)
    CHECK(width > 4.0f && width < 6.0f);
    SkRect all = SkRect::MakeEmpty();
    for (const auto& stroke : word) all.join(gfx::brand::outline(stroke).getBounds());
    CHECK_NEAR(all.top(), -1.0f, 0.01);   // cap height
    CHECK_NEAR(all.bottom(), 0.0f, 0.01); // baseline
    CHECK(gfx::brand::markSvg(false).find("<path") != std::string::npos);
    CHECK(gfx::brand::wordmarkSvg(SK_ColorBLACK).find("viewBox=\"0 0 ") != std::string::npos);
}

TEST_CASE("brand: icon and busy cursor draw opaque pixels where expected")
{
    auto surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(64, 64));
    surface->getCanvas()->clear(SK_ColorTRANSPARENT);
    gfx::brand::drawAppIcon(*surface->getCanvas(), 64);
    SkPixmap pixels;
    CHECK(surface->peekPixels(&pixels));
    CHECK(SkColorGetA(pixels.getColor(1, 1)) == 0);     // outside the tile
    CHECK(SkColorGetA(pixels.getColor(32, 12)) == 255); // tile body
    const auto cursor = gfx::brand::renderImage(64, 64, 2.0f, [](SkCanvas& c) { gfx::brand::drawBusyCursor(c, 0.3); });
    CHECK(cursor.getPixelAt(gfx::brand::busyCursorHotSpot * 2 + 2, gfx::brand::busyCursorHotSpot * 2 + 6).getAlpha() == 255); // arrow
    CHECK(cursor.getPixelAt(63, 0).getAlpha() == 0);
    const auto scrub = gfx::brand::renderImage(48, 48, 2.0f, [](SkCanvas& c) { gfx::brand::drawScrubCursor(c); });
    CHECK(scrub.getPixelAt(gfx::brand::scrubCursorHotSpotX * 2, gfx::brand::scrubCursorHotSpotY * 2).getAlpha() == 255); // speaker body
    CHECK(scrub.getPixelAt(0, 0).getAlpha() == 0);
}

TEST_CASE("brand: save prompt buttons sit inside the panel like a macOS alert (UI-44)")
{
    gfx::brand::PromptState state;
    state.title = "Save changes to \u201CA very long set name that the title trims with an ellipsis\u201D?";
    state.message = "Your changes will be lost if you don\u2019t save them. A second sentence makes the message wrap onto more lines.";
    state.buttons = {{"Save", true, false}, {"Don\u2019t Save", false, true}, {"Cancel", false, false}};
    const float w = gfx::brand::promptWidth, h = gfx::brand::promptHeight(state);
    gfx::brand::PromptState shortMessage = state;
    shortMessage.message = "One line.";
    CHECK(h > gfx::brand::promptHeight(shortMessage)); // the panel grows with the wrapped message
    const auto rects = gfx::brand::promptButtonRects(state, w, h);
    CHECK(rects.size() == 3);
    for (const auto& r : rects)
    {
        CHECK(r.left() > 0 && r.right() < w && r.top() > 0 && r.bottom() < h);
        CHECK(r.width() >= 84);
    }
    CHECK(rects[0].right() > rects[2].right()); // Save rightmost, Cancel left of it
    CHECK(rects[2].right() < rects[0].left());
    CHECK(rects[1].right() < rects[2].left()); // Don't Save at the left
    auto surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul((int) w, (int) h));
    gfx::brand::drawPrompt(*surface->getCanvas(), w, h, state);
    SkPixmap pixels;
    CHECK(surface->peekPixels(&pixels));
    CHECK(SkColorGetA(pixels.getColor((int) w / 2, (int) h / 2)) == 255); // opaque panel
    CHECK(SkColorGetA(pixels.getColor(0, 0)) < 255);                      // rounded corner
    const auto save = pixels.getColor((int) rects[0].left() + 6, (int) rects[0].centerY());
    CHECK(SkColorGetR(save) > 200); // the primary button is the light one
    const auto cancel = pixels.getColor((int) rects[2].left() + 6, (int) rects[2].centerY());
    CHECK(SkColorGetR(cancel) < 80);
}

TEST_CASE("brand: long prompt button labels never overlap (UI-44c)")
{
    // The Duplicate Tracks question: two long trailing labels pushed Cancel's slot under "Without Automation".
    auto disjoint = [](const std::vector<SkRect>& rects) {
        for (size_t a = 0; a < rects.size(); ++a)
            for (size_t b = a + 1; b < rects.size(); ++b)
                if (SkRect::Intersects(rects[a].makeOutset(4, 4), rects[b])) return false;
        return true;
    };
    gfx::brand::PromptState state;
    state.title = "Duplicate the automation too?";
    state.message = "The selected tracks have automation. Should the copies get it as well?";
    state.buttons = {{"With Automation", true, false}, {"Without Automation", false, false}, {"Cancel", false, true}};
    float w = gfx::brand::promptWidth, h = gfx::brand::promptHeight(state);
    auto rects = gfx::brand::promptButtonRects(state, w, h);
    CHECK(disjoint(rects));
    for (const auto& r : rects) CHECK(r.left() > 0 && r.right() < w && r.top() > 0 && r.bottom() < h);
    CHECK(rects[0].right() > rects[1].right() && rects[1].left() > rects[2].right()); // still one row: Cancel at the left
    CHECK(std::abs(rects[0].top() - rects[2].top()) < 0.01f);

    // Labels that fit no row stack in a column, primary on top, Cancel last, and the panel grows to hold them.
    gfx::brand::PromptState column = state;
    column.buttons = {{"Replace Every Instance in the Set", true, false}, {"Keep the Plug-ins as They Are", false, false},
                      {"Cancel", false, true}};
    h = gfx::brand::promptHeight(column);
    CHECK(h > gfx::brand::promptHeight(state));
    rects = gfx::brand::promptButtonRects(column, w, h);
    CHECK(disjoint(rects));
    for (const auto& r : rects) CHECK(r.left() > 0 && r.right() < w && r.top() > 0 && r.bottom() < h);
    CHECK(rects[0].top() < rects[1].top() && rects[1].top() < rects[2].top());
}

TEST_CASE("brand: About window buttons stay inside the panel, apart, and the height follows the supporters (COM-8)")
{
    gfx::brand::AboutState empty;
    empty.version = "Version 0.1.2";
    empty.credit = "Created by Wellington Chaves";
    empty.emptyText = "No supporters listed yet.";
    auto many = empty;
    many.groups = {{"Studio", {"Example Studio"}}, {"Supporters", std::vector<std::string>(40, "Somebody Long")}};
    CHECK(gfx::brand::aboutHeight(many) > gfx::brand::aboutHeight(empty)); // names wrap onto more lines
    const float w = gfx::brand::aboutWidth, h = gfx::brand::aboutHeight(many);
    const auto rects = gfx::brand::aboutButtonRects(many, w, h);
    for (size_t i = 0; i < rects.size(); ++i)
    {
        CHECK(rects[i].left() > 0 && rects[i].right() < w && rects[i].bottom() < h);
        for (size_t j = i + 1; j < rects.size(); ++j) CHECK(! SkRect::Intersects(rects[i], rects[j]));
    }
    auto surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul((int) w, (int) h));
    gfx::brand::drawAbout(*surface->getCanvas(), w, h, many);
    SkPixmap pixels;
    REQUIRE(surface->peekPixels(&pixels));
    const auto& support = rects[gfx::brand::AboutSupport];
    CHECK(SkColorGetR(pixels.getColor((int) support.left() + 6, (int) support.centerY())) > 200); // the primary button
}

namespace
{
// Only what About's update row reads (COM-4).
class FakeUpdater final : public app::Updater
{
public:
    FakeUpdater() : Updater(Host{}) {}
    bool on = true, idle = true, automatic = true, pending = false;
    app::UpdateChannel current = app::UpdateChannel::Stable;
    bool available() const override { return on; }
    std::string unavailableReason() const override { return on ? "" : "Updates are off in this build (no update feed or signing key configured)"; }
    std::string backendName() const override { return "fake"; }
    std::string currentVersion() const override { return "0.1.2 (1)"; }
    bool canCheckNow() const override { return idle; }
    void checkNow() override {}
    bool automaticChecks() const override { return automatic; }
    void setAutomaticChecks(bool enabled) override { automatic = enabled; }
    app::UpdateChannel channel() const override { return current; }
    void setChannel(app::UpdateChannel channel) override { current = channel; }
    std::vector<app::UpdateChannel> channels() const override { return {app::UpdateChannel::Stable, app::UpdateChannel::Beta}; }
    bool installPending() const override { return pending; }
    void installPendingNow() override {}
};

bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }
} // namespace

TEST_CASE("brand: About's update row follows the updater and sits between the credit and the supporters (COM-4)")
{
    CHECK(! app::aboutUpdateRow(nullptr).enabled);
    CHECK(app::aboutUpdateRow(nullptr).status == "This build has no updater");

    FakeUpdater updater;
    auto row = app::aboutUpdateRow(&updater);
    CHECK(row.enabled);
    CHECK(row.label == "Check for Updates...");
    CHECK(contains(row.status, "Checks automatically every day") && contains(row.status, "Stable channel"));
    updater.automatic = false;
    updater.current = app::UpdateChannel::Beta;
    row = app::aboutUpdateRow(&updater);
    CHECK(contains(row.status, "Automatic checks are off") && contains(row.status, "Beta channel"));
    updater.idle = false; // a check is running: the button waits
    CHECK(! app::aboutUpdateRow(&updater).enabled);
    updater.pending = true; // downloaded, restart postponed: the same button installs it
    row = app::aboutUpdateRow(&updater);
    CHECK(row.enabled);
    CHECK(row.label == "Install and Restart...");
    updater.on = false;
    row = app::aboutUpdateRow(&updater);
    CHECK(! row.enabled);
    CHECK(contains(row.status, "Updates are off in this build"));

    gfx::brand::AboutState state;
    state.version = "Version 0.1.2";
    state.credit = "Created by Wellington Chaves";
    state.emptyText = "No supporters listed yet.";
    state.updates = row; // the longest status: two lines beside the button
    const float w = gfx::brand::aboutWidth, h = gfx::brand::aboutHeight(state);
    const auto rects = gfx::brand::aboutButtonRects(state, w, h);
    const auto& button = rects[gfx::brand::AboutUpdates];
    CHECK(button.left() > 0 && button.width() > 100 && button.top() > 130);
    CHECK(button.bottom() + 40 < rects[gfx::brand::AboutWebsite].top()); // the supporters text stays between them
    for (size_t i = 0; i < rects.size(); ++i)
        if (i != gfx::brand::AboutUpdates) CHECK(! SkRect::Intersects(button, rects[i]));

    // Disabled: the label is drawn dim; enabled: light.
    auto labelBrightness = [&](const gfx::brand::AboutState& s) {
        auto surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul((int) w, (int) h));
        gfx::brand::drawAbout(*surface->getCanvas(), w, h, s);
        SkPixmap pixels;
        REQUIRE(surface->peekPixels(&pixels));
        unsigned brightest = 0;
        for (int y = (int) button.top() + 6; y < (int) button.bottom() - 6; ++y)
            for (int x = (int) button.left() + 10; x < (int) button.right() - 10; ++x)
                brightest = std::max(brightest, SkColorGetR(pixels.getColor(x, y)));
        return brightest;
    };
    const unsigned off = labelBrightness(state);
    state.updates = app::aboutUpdateRow(nullptr);
    state.updates.enabled = true;
    CHECK(off < 140);
    CHECK(labelBrightness(state) > 200);
}
