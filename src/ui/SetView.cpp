#include "ui/SetView.h"

#include "ui/UiContext.h"

#include <algorithm>
#include <cmath>

namespace sonora::ui
{
namespace
{
enum class Channel { Track, Group, ReturnOrMain };

// Every channel with its control index: tracks, returns, Main.
template <typename SongT, typename Fn>
void forEachChannel(SongT& song, Fn&& fn)
{
    int index = 0;
    for (auto& t : song.tracks) fn(index++, t, t.kind == model::TrackKind::Group ? Channel::Group : Channel::Track);
    for (auto& t : song.returns) fn(index++, t, Channel::ReturnOrMain);
    fn(index, song.main, Channel::ReturnOrMain);
}

bool contains(const std::vector<int>& v, int value) { return std::find(v.begin(), v.end(), value) != v.end(); }

// The reference's sizes as Sonora shows them, within Sonora's own limits. A saved value the UI still shows this way was
// not changed, so it is written back as it was read (the reference allows narrower Session tracks and deeper zoom).
float uiLaneHeight(int reference) { return std::clamp((float) reference + kReferenceSeparator, 18.0f, 420.0f); }
float uiSessionWidth(int reference) { return std::clamp((float) reference + kReferenceSeparator, 92.0f, 400.0f); }
double uiPixelsPerBeat(double beatsPerPixel) { return std::clamp(1.0 / beatsPerPixel, 0.01, 2400.0); }
} // namespace

void applySetView(UiContext& x)
{
    if (x.song == nullptr) return;
    const auto& song = *x.song;
    const auto count = (size_t) x.numControlTracks();
    x.laneHeights.assign(count, 0.0f);
    x.sessionColumnWidths.assign(count, 0.0f);
    x.collapsedTracks.clear();
    forEachChannel(song, [&](int i, const model::Track& t, Channel channel) {
        // A folded return / Main is the single row Sonora shows by default; a folded track collapses; a group's fold is
        // model state (Track::folded) and its own height still applies.
        if (channel == Channel::Track && t.folded) x.collapsedTracks.push_back(i);
        if (t.laneHeight > 0 && ! (channel == Channel::ReturnOrMain && t.folded))
            x.laneHeights[(size_t) i] = uiLaneHeight(t.laneHeight);
        if (t.sessionWidth > 0)
            x.sessionColumnWidths[(size_t) i] = uiSessionWidth(t.sessionWidth);
    });

    const auto& v = song.view;
    x.mainView = v.sessionVisible ? MainView::Session : MainView::Arrangement;
    if (v.present)
    {
        x.pixelsPerBeat = uiPixelsPerBeat(v.beatsPerPixel);
        x.scrollBeats = std::max(0.0, v.arrangementScrollX * v.beatsPerPixel);
        x.scrollTracksY = (float) std::max(0, v.arrangementScrollY);
    }
    x.sessionScrollX = (float) std::max(0, v.sessionScrollX);
    x.sessionScrollY = (float) std::max(0, v.sessionScrollY);
    x.detailVisible = v.clipDetailOpen || v.deviceDetailOpen;
    if (v.clipDetailOpen) x.detailTab = DetailTab::Clip;
    else if (v.deviceDetailOpen) x.detailTab = DetailTab::Devices;
}

void captureSetView(const UiContext& x, model::Song& song)
{
    forEachChannel(song, [&](int i, model::Track& t, Channel channel) {
        const bool collapsed = contains(x.collapsedTracks, i);
        const float height = (size_t) i < x.laneHeights.size() ? x.laneHeights[(size_t) i] : 0.0f;
        const float width = (size_t) i < x.sessionColumnWidths.size() ? x.sessionColumnWidths[(size_t) i] : 0.0f;
        if (channel == Channel::Track) t.folded = collapsed;
        if (channel == Channel::ReturnOrMain)
        {
            if (collapsed) t.folded = true;
            else if (height > 0.0f) t.folded = false;
        }
        if (height > 0.0f && (t.laneHeight <= 0 || std::fabs(height - uiLaneHeight(t.laneHeight)) > 0.5f))
        {
            const double steps = std::round((height - kReferenceSeparator) / kReferenceLaneStep);
            t.laneHeight = std::max(1, (int) steps) * kReferenceLaneStep;
        }
        if (width > 0.0f && (t.sessionWidth <= 0 || std::fabs(width - uiSessionWidth(t.sessionWidth)) > 0.5f))
            t.sessionWidth = std::max(1, (int) std::lround(width - kReferenceSeparator));
    });

    auto& v = song.view;
    v.sessionVisible = x.mainView == MainView::Session;
    if (x.pixelsPerBeat > 0.0)
    {
        // Keep the read zoom when the UI still shows it (1 / (1 / z) can differ from z in the last bit).
        if (std::fabs(x.pixelsPerBeat - uiPixelsPerBeat(v.beatsPerPixel)) > 1e-9 * x.pixelsPerBeat) v.beatsPerPixel = 1.0 / x.pixelsPerBeat;
        v.arrangementScrollX = (int) std::lround(x.scrollBeats / v.beatsPerPixel); // the reference's pixels at its zoom
    }
    v.arrangementScrollY = (int) std::lround(x.scrollTracksY);
    v.sessionScrollX = (int) std::lround(x.sessionScrollX);
    v.sessionScrollY = (int) std::lround(x.sessionScrollY);
    v.clipDetailOpen = x.detailVisible && x.detailTab == DetailTab::Clip;
    v.deviceDetailOpen = x.detailVisible && x.detailTab != DetailTab::Clip;
}

} // namespace sonora::ui
