// UI-57: the view state a set is saved with (main view, Arrangement zoom/scroll, Session scroll, lane heights, folds,
// Session widths, detail view) opens in the UI and is written back into the reference's own elements.
#include "TestFramework.h"
#include "io/als/AlsNewSet.h"
#include "io/als/AlsReader.h"
#include "io/als/AlsWriter.h"
#include "ui/SetView.h"
#include "ui/UiContext.h"

#include <algorithm>

using namespace sonora;

namespace
{
juce::XmlElement* path(juce::XmlElement* e, const char* p)
{
    juce::StringArray parts;
    parts.addTokens(p, "/", "");
    for (const auto& part : parts)
        if (e != nullptr) e = e->getChildByName(part);
    return e;
}

juce::XmlElement* track(juce::XmlElement& liveSet, int index) { return liveSet.getChildByName("Tracks")->getChildElement(index); }

juce::XmlElement* firstLane(juce::XmlElement* t)
{
    auto* lanes = path(t, "DeviceChain/AutomationLanes/AutomationLanes");
    return lanes ? lanes->getFirstChildElement() : nullptr;
}

bool collapsed(const ui::UiContext& ctx, int index)
{
    return std::find(ctx.collapsedTracks.begin(), ctx.collapsedTracks.end(), index) != ctx.collapsedTracks.end();
}
} // namespace

TEST_CASE("set view: the reference's zoom, scroll, main view, heights and widths open and save back (UI-57)")
{
    auto dom = io::createNewSetDom();
    auto& liveSet = *dom->getChildByName("LiveSet");
    path(&liveSet, "SequencerNavigator/BeatTimeHelper/CurrentZoom")->setAttribute("Value", "0.0170445269855049944");
    path(&liveSet, "SequencerNavigator/ScrollerPos")->setAttribute("X", -10660);
    path(&liveSet, "SequencerNavigator/ScrollerPos")->setAttribute("Y", -19);
    liveSet.getChildByName("SessionScrollPos")->setAttribute("X", -120);
    liveSet.getChildByName("SessionScrollPos")->setAttribute("Y", -17);
    liveSet.getChildByName("SelectedDocumentViewInMainWindow")->setAttribute("Value", 0);
    liveSet.getChildByName("ViewStateMainWindowClipDetailOpen")->setAttribute("Value", "true");
    liveSet.getChildByName("ViewStateMainWindowDeviceDetailOpen")->setAttribute("Value", "false");
    firstLane(track(liveSet, 0))->getChildByName("LaneHeight")->setAttribute("Value", 51);
    path(track(liveSet, 0), "DeviceChain/Mixer/ViewStateSessionTrackWidth")->setAttribute("Value", 150);
    track(liveSet, 1)->getChildByName("TrackUnfolded")->setAttribute("Value", "false");
    path(track(liveSet, 2), "DeviceChain/Mixer/ViewStateSessionTrackWidth")->setAttribute("Value", 74); // narrower than Sonora allows

    juce::TemporaryFile source(".als");
    std::string error;
    REQUIRE(io::writeAlsDom(*dom, source.getFile(), error));
    auto loaded = io::loadAls(source.getFile());
    REQUIRE(loaded);
    auto& song = *loaded.song;
    CHECK(song.view.present);
    CHECK(! song.view.sessionVisible);
    CHECK_NEAR(song.view.beatsPerPixel, 0.0170445269855049944, 1e-15);
    CHECK(song.view.arrangementScrollX == 10660); CHECK(song.view.arrangementScrollY == 19);
    CHECK(song.view.sessionScrollX == 120); CHECK(song.view.sessionScrollY == 17);
    CHECK(song.view.clipDetailOpen); CHECK(! song.view.deviceDetailOpen);
    CHECK(song.tracks[0].laneHeight == 51); CHECK(song.tracks[1].laneHeight == 68);
    CHECK(song.tracks[0].sessionWidth == 150);

    ui::UiContext ctx;
    ctx.song = &song;
    ctx.mainView = ui::MainView::Session;
    ctx.collapsedTracks = {3};          // left over from another set
    ui::applySetView(ctx);
    CHECK(ctx.mainView == ui::MainView::Arrangement);
    CHECK_NEAR(ctx.pixelsPerBeat, 1.0 / 0.0170445269855049944, 1e-9);
    CHECK_NEAR(ctx.scrollBeats, 10660 * 0.0170445269855049944, 1e-9); // bar 46, where the reference was left
    CHECK_NEAR(ctx.scrollTracksY, 19.0f, 1e-6);
    CHECK_NEAR(ctx.sessionScrollX, 120.0f, 1e-6); CHECK_NEAR(ctx.sessionScrollY, 17.0f, 1e-6);
    CHECK(ctx.detailVisible); CHECK(ctx.detailTab == ui::DetailTab::Clip);
    CHECK_NEAR(ctx.laneHeights[0], 53.0f, 1e-6); CHECK_NEAR(ctx.laneHeights[1], 70.0f, 1e-6);
    CHECK(collapsed(ctx, 1)); CHECK(! collapsed(ctx, 0)); CHECK(! collapsed(ctx, 3));
    CHECK_NEAR(ctx.sessionColumnWidths[0], 152.0f, 1e-6); CHECK_NEAR(ctx.sessionColumnWidths[2], 92.0f, 1e-6);

    // Saved untouched: the reference's own numbers come back verbatim.
    auto untouched = song;
    ui::captureSetView(ctx, untouched);
    juce::TemporaryFile same(".xml");
    REQUIRE(io::saveAls(untouched, same.getFile()).ok);
    auto sameXml = juce::XmlDocument::parse(same.getFile());
    REQUIRE(sameXml != nullptr);
    auto* sameSet = sameXml->getChildByName("LiveSet");
    CHECK(path(sameSet, "SequencerNavigator/BeatTimeHelper/CurrentZoom")->getStringAttribute("Value") == "0.0170445269855049944");
    CHECK(path(sameSet, "SequencerNavigator/ScrollerPos")->getIntAttribute("X") == -10660);
    CHECK(path(sameSet, "SequencerNavigator/ScrollerPos")->getIntAttribute("Y") == -19);
    CHECK(sameSet->getChildByName("SessionScrollPos")->getIntAttribute("X") == -120);
    CHECK(firstLane(track(*sameSet, 0))->getChildByName("LaneHeight")->getIntAttribute("Value") == 51);
    CHECK(track(*sameSet, 1)->getChildByName("TrackUnfolded")->getStringAttribute("Value") == "false");
    CHECK(path(track(*sameSet, 2), "DeviceChain/Mixer/ViewStateSessionTrackWidth")->getIntAttribute("Value") == 74);

    // Changed in Sonora: Session view, another zoom/scroll, a taller lane (snapped to 17-point steps), the fold opened,
    // the detail view closed.
    ctx.mainView = ui::MainView::Session;
    ctx.pixelsPerBeat = 20.0;
    ctx.scrollBeats = 8.0;
    ctx.scrollTracksY = 40.0f;
    ctx.sessionScrollX = 0.0f;
    ctx.laneHeights[0] = 90.0f;
    ctx.collapsedTracks.clear();
    ctx.sessionColumnWidths[0] = 110.0f;
    ctx.detailVisible = false;
    ui::captureSetView(ctx, song);
    juce::TemporaryFile saved(".als");
    REQUIRE(io::saveAls(song, saved.getFile()).ok);
    auto again = io::loadAls(saved.getFile());
    REQUIRE(again);
    const auto& v = again.song->view;
    CHECK(v.sessionVisible);
    CHECK_NEAR(v.beatsPerPixel, 0.05, 1e-12);
    CHECK(v.arrangementScrollX == 160); CHECK(v.arrangementScrollY == 40);
    CHECK(v.sessionScrollX == 0); CHECK(v.sessionScrollY == 17);
    CHECK(! v.clipDetailOpen); CHECK(! v.deviceDetailOpen);
    CHECK(again.song->tracks[0].laneHeight == 85);
    CHECK(! again.song->tracks[1].folded);
    CHECK(again.song->tracks[0].sessionWidth == 108);

    ui::UiContext reopened;
    reopened.song = again.song.get();
    ui::applySetView(reopened);
    CHECK(reopened.mainView == ui::MainView::Session);
    CHECK_NEAR(reopened.pixelsPerBeat, 20.0, 1e-9); CHECK_NEAR(reopened.scrollBeats, 8.0, 1e-9);
    CHECK(! reopened.detailVisible);
    CHECK_NEAR(reopened.laneHeights[0], 87.0f, 1e-6);
}

TEST_CASE("set view: a set without view elements keeps Sonora's zoom and gains none on save (UI-57)")
{
    auto dom = io::createNewSetDom();
    auto& liveSet = *dom->getChildByName("LiveSet");
    for (const char* tag : {"SequencerNavigator", "SessionScrollPos", "SelectedDocumentViewInMainWindow"})
        liveSet.removeChildElement(liveSet.getChildByName(tag), true);
    juce::TemporaryFile source(".als");
    std::string error;
    REQUIRE(io::writeAlsDom(*dom, source.getFile(), error));
    auto loaded = io::loadAls(source.getFile());
    REQUIRE(loaded);
    CHECK(! loaded.song->view.present);

    ui::UiContext ctx;
    ctx.song = loaded.song.get();
    ui::applySetView(ctx);
    CHECK_NEAR(ctx.pixelsPerBeat, 14.0, 1e-9);
    CHECK(ctx.mainView == ui::MainView::Arrangement);
    ctx.pixelsPerBeat = 30.0;
    ui::captureSetView(ctx, *loaded.song);
    juce::TemporaryFile saved(".xml");
    REQUIRE(io::saveAls(*loaded.song, saved.getFile()).ok);
    auto xml = juce::XmlDocument::parse(saved.getFile());
    REQUIRE(xml != nullptr);
    CHECK(xml->getChildByName("LiveSet")->getChildByName("SequencerNavigator") == nullptr);
    CHECK(xml->getChildByName("LiveSet")->getChildByName("SelectedDocumentViewInMainWindow") == nullptr);
}
