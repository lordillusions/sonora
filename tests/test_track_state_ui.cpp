// TRK-1: hidden-track filtering and the track menu toggles.
#include "TestFramework.h"
#include "core/Edit.h"
#include "io/als/AlsNewSet.h"
#include "io/als/AlsReader.h"
#include "io/als/AlsWriter.h"
#include "ui/TrackMenu.h"
#include "ui/UiContext.h"

using namespace sonora;

namespace
{
struct TrackUiRig
{
    juce::ScopedJuceInitialiser_GUI gui;
    juce::File folder = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("sonora-track-ui-" + juce::Uuid().toString());
    std::unique_ptr<model::Song> song;
    TrackUiRig()
    {
        REQUIRE(folder.createDirectory()); auto root = io::createNewSetDom(); std::string error;
        REQUIRE(io::writeAlsDom(*root, folder.getChildFile("Source.als"), error));
        auto loaded = io::loadAls(folder.getChildFile("Source.als")); REQUIRE(loaded); song = std::move(loaded.song);
        REQUIRE(song->tracks.size() >= 3); REQUIRE(! song->returns.empty());
    }
    ~TrackUiRig() { folder.deleteRecursively(); }
};
} // namespace

TEST_CASE("track state: views hide hidden tracks and members of hidden groups unless shown")
{
    TrackUiRig rig; auto& s = *rig.song;
    ui::UiContext ctx; ctx.song = &s;
    s.tracks[0].kind = model::TrackKind::Group;
    s.tracks[1].groupId = s.tracks[0].id;
    const int ret = (int) s.tracks.size();
    CHECK(! ui::trackHiddenInViews(ctx, 0)); CHECK(! ui::trackHiddenInViews(ctx, 1));
    s.tracks[0].hidden = true; s.returns[0].hidden = true;
    CHECK(ui::trackHiddenInViews(ctx, 0)); CHECK(ui::trackHiddenInViews(ctx, 1)); CHECK(! ui::trackHiddenInViews(ctx, 2));
    CHECK(ui::trackHiddenInViews(ctx, ret));
    ctx.showHiddenTracks = true;
    CHECK(! ui::trackHiddenInViews(ctx, 0)); CHECK(! ui::trackHiddenInViews(ctx, 1)); CHECK(! ui::trackHiddenInViews(ctx, ret));
}

TEST_CASE("track state: the track menu toggles hide and deactivate on the selection")
{
    TrackUiRig rig; auto& s = *rig.song;
    ui::UiContext ctx; ctx.song = &s;
    model::EditHistory history;
    ctx.applyEdit = [&](model::EditCommand c) { history.perform(s, std::move(c)); };
    ctx.selectedTrack = 0; ctx.trackSelection = {0, 1};
    ui::toggleSelectedTracksHidden(ctx);
    CHECK(s.tracks[0].hidden); CHECK(s.tracks[1].hidden); CHECK(! s.tracks[2].hidden);
    CHECK(ctx.statusText.find("View > Show Hidden Tracks") != std::string::npos);
    ui::toggleSelectedTracksHidden(ctx);
    CHECK(! s.tracks[0].hidden); CHECK(! s.tracks[1].hidden);
    ui::toggleSelectedTracksDeactivated(ctx);
    CHECK(s.tracks[0].deactivated); CHECK(s.tracks[1].deactivated);
    ctx.recording = true; ui::toggleSelectedTracksDeactivated(ctx); CHECK(s.tracks[0].deactivated); // refused while recording
}
