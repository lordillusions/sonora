#pragma once
// UI-82: the CPU meter details, opened by clicking the CPU box in the control bar (like the reference's CPU meter).
//
// A popover under the box: current / average / peak load of the audio callback (share of the buffer time), blocks over
// their buffer time and the driver's own dropouts since Reset, buffer size / sample rate / latency, the threads that
// render (ENG-14e multi-core, with its switch), and every track's cost as a share of the buffer time, highest first
// (the reference's "Performance Impact"). Shows UiContext::cpu only; AppController fills it.
// The widget covers the whole window while open: a click outside the popover closes it, Escape too.

#include "ui/Widget.h"

#include <functional>
#include <vector>

namespace sonora::ui
{

class CpuPanel : public Widget
{
public:
    void paint(SkCanvas& c, const Theme& t) override;
    void mouseDown(const MouseEvent& e) override;
    bool mouseWheel(const MouseEvent& e) override;
    bool keyPressed(const KeyEvent& k) override;
    void tick() override;

    Rect panelRect() const;
    static constexpr float kWidth = 300.0f;
    static constexpr int kMaxTrackRows = 12;

private:
    struct Hit
    {
        Rect area;
        std::function<void()> action;
    };
    void close();
    std::vector<Hit> hits_;
    int ticks_ = 0;
    int firstTrack_ = 0; // scrolled track list
};

} // namespace sonora::ui
