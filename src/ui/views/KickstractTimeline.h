#pragma once
// Sonora Kickstract's timeline (PLG-17e): Input, Kick and Removed as three lanes scrolling in real time, one line per
// detected hit across them. Freeze stops the scrolling (the device keeps filtering) so the last ~10 s can be studied:
// drag to move back in time, the wheel zooms the time span, the pointer shows the time under it. Shared by the app's
// window (KickstractTimelineWindowView) and the VST3's window.

#include "devices/native/Kickstract.h"
#include "ui/RootWidget.h"
#include "ui/UiContext.h"
#include "ui/Widget.h"

#include <functional>
#include <string>
#include <vector>

namespace sonora::ui
{
class TextButton;

class KickstractTimeline final : public Widget
{
public:
    static constexpr float kPreferredWidth = 1100, kPreferredHeight = 460;
    explicit KickstractTimeline(devices::Kickstract* processor);
    void setProcessor(devices::Kickstract* processor); // after a graph rebuild (null: nothing to show)

    void layout() override;
    void paint(SkCanvas&, const Theme&) override;
    void tick() override;
    void mouseDown(const MouseEvent&) override;
    void mouseDrag(const MouseEvent&) override;
    void mouseMove(const MouseEvent&) override;
    void mouseExit() override;
    void mouseDoubleClick(const MouseEvent&) override;
    bool mouseWheel(const MouseEvent&) override;
    bool keyPressed(const KeyEvent&) override;

    void setFrozen(bool frozen);
    bool frozen() const { return frozen_; }
    void setSpanSeconds(double seconds);
    double spanSeconds() const;
    // Bins back from the newest one shown at the right edge (only while frozen).
    int offsetBins() const { return offset_; }
    // Geometry and content, exposed for tests.
    Rect laneArea(int lane) const;
    int bins() const { return (int) bins_.size(); }
    int hitsShown() const;

private:
    void pull();
    void clampOffset();
    double secondsAt(float x) const; // time relative to the newest bin (<= 0)
    void paintLane(SkCanvas&, const Theme&, int lane);

    devices::Kickstract* processor_;
    std::vector<devices::Kickstract::TimelineBin> bins_; // oldest first
    uint64_t pulled_ = 0;
    bool frozen_ = false;
    int span_ = 1600;   // bins across the lanes (4 s)
    int offset_ = 0;    // bins back from the newest (frozen only)
    float dragX_ = 0;
    int dragOffset_ = 0;
    float hoverX_ = -1;
    TextButton *freeze_ = nullptr, *live_ = nullptr;
    TextButton* spans_[3]{};
};

// Root of the app's timeline window: follows its built-in Kickstract by key through graph rebuilds, like the Hush window.
class KickstractTimelineWindowView final : public RootWidget
{
public:
    KickstractTimelineWindowView(UiContext&, UiContext::WavetableKey);
    const UiContext::WavetableKey& key() const { return key_; }
    void setKey(const UiContext::WavetableKey&);
    void sync();
    KickstractTimeline* timeline() const { return timeline_; }
    std::function<void(const std::string&)> onTitleChanged;
    std::function<void()> onGone; // the device was deleted

    void layout() override;
    void paint(SkCanvas&, const Theme&) override;
    void tick() override;

    struct Site
    {
        int track = -1;
        const model::Device* model = nullptr;
        devices::Kickstract* processor = nullptr;
    };
    static std::optional<Site> find(const UiContext&, const UiContext::WavetableKey&);

private:
    bool stale() const;
    void rebuild();

    UiContext::WavetableKey key_;
    KickstractTimeline* timeline_ = nullptr;
    bool built_ = false, goneSent_ = false;
    const model::Song* builtSong_ = nullptr;
    const void* builtGraph_ = nullptr;
    uint64_t builtStructure_ = 0;
    std::string title_;
};
} // namespace sonora::ui
