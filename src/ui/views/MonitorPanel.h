#pragma once
// Monitor tab of the Device View: loudness (LUFS momentary / short-term / integrated / range, true peak),
// spectrum (bars or curve) or oscilloscope, and the stereo meters (correlometer, goniometer, balance, width)
// for the Main output. Reads engine::MasterMonitor::snapshot(); the analysis runs on its own thread.

#include "engine/MasterMonitor.h"
#include "ui/Widget.h"

namespace sonora::ui
{

class MonitorPanel : public Widget
{
public:
    void paint(SkCanvas& c, const Theme& t) override;
    void tick() override;
    void mouseDown(const MouseEvent& e) override;

    static constexpr int kScopeWindows = 5;
    static double scopeWindowMs(int index);

private:
    Rect loudnessArea() const;
    Rect displayArea() const;
    Rect stereoArea() const;
    Rect viewButton(int index) const;
    Rect windowButton() const;
    Rect triggerButton() const;
    Rect resetButton() const;

    void paintLoudness(SkCanvas& c, const Theme& t, const engine::MonitorSnapshot& s);
    void paintSpectrum(SkCanvas& c, const Theme& t, const engine::MonitorSnapshot& s, bool bars);
    void paintScope(SkCanvas& c, const Theme& t, const engine::MonitorSnapshot& s);
    void paintStereo(SkCanvas& c, const Theme& t, const engine::MonitorSnapshot& s);

    uint64_t shownSerial_ = 0;
};

} // namespace sonora::ui
