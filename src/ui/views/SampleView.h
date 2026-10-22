#pragma once

#include "ui/Widget.h"
#include "core/Edit.h"

namespace sonora::ui
{

// Audio clip editor (UI-10c, UI-17), laid out like Live 12's sample editor: a header with the file name and format, a
// ruler (bars when warped, minutes:seconds otherwise), three marker rows (clip start/end, loop, warp markers with the
// detected transients) and the stereo waveform. Drag the start/end triangles; double-click the warp row or the
// waveform to add a warp marker, drag it (Shift: its beat), double-click it to remove it. In the loop row, drag the
// position marker, the loop brace's end or the brace itself (warped clips snap to 1/16 notes; Alt: no snap). Cmd+wheel zooms, wheel scrolls,
// Follow (UiContext::sampleFollow) keeps the playhead in view.
class SampleView : public Widget
{
public:
    static constexpr float kHeaderHeight = 18, kRulerHeight = 18, kStartRowHeight = 10, kLoopRowHeight = 11, kWarpRowHeight = 13;
    static constexpr float kSide = 8; // waveform inset from the left and right edges

    void paint(SkCanvas& c, const Theme& t) override;
    void mouseDown(const MouseEvent&) override;
    void mouseDrag(const MouseEvent&) override;
    void mouseUp(const MouseEvent&) override;
    bool mouseWheel(const MouseEvent& e) override;

    // Geometry in this view's coordinates (tests locate the rows through these).
    float startRowY() const { return kHeaderHeight + kRulerHeight; }
    float loopRowY() const { return startRowY() + kStartRowHeight; }
    float warpRowY() const { return loopRowY() + kLoopRowHeight; }
    Rect waveArea() const;
    // Source position of the playhead in seconds, < 0 when the clip is not playing. Arrangement clips follow the
    // transport; Session clips the playing slot's clock (UI-17c).
    double playheadSeconds(const model::Clip& clip) const;

private:
    int lastUid_ = -1;
    double scrollSeconds_ = 0, secondsPerPixel_ = 0.001;
    float verticalZoom_ = 1.0f; // Alt+Cmd+wheel; amplitude is drawn this many times larger (clipped at full scale)
    model::ClipLocation location_;
    model::Clip original_, working_;
    int marker_ = -1;
    int edge_ = 0; // dragging: clip start (1) / end (2) marker; loop row: position (3), loop end (4), whole brace (5)
    float grabX_ = 0, grabY_ = 0;
    bool changed_ = false;
    void commit();
    void showContextMenu(const MouseEvent& e, const model::Clip& clip);
    double secondsToUnits(const model::Clip&, double seconds) const;
    double xToSeconds(float x) const { return scrollSeconds_ + (x - kSide) * secondsPerPixel_; }
    float secondsToX(double seconds) const { return kSide + (float) ((seconds - scrollSeconds_) / secondsPerPixel_); }
};

} // namespace sonora::ui
