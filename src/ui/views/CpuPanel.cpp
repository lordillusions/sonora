#include "ui/views/CpuPanel.h"

#include "ui/Controls.h"
#include "ui/Painter.h"
#include "ui/UiContext.h"
#include "ui/widgets/Fader.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace sonora::ui
{

namespace
{
constexpr float kPad = 10.0f, kRow = 18.0f, kLabelW = 62.0f, kValueW = 46.0f;

std::string percent(float load)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%d %%", (int) std::lround(std::max(0.0f, load) * 100.0f));
    return buf;
}

std::string decimal(double value, int places)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.*f", places, value);
    return buf;
}

// A level bar like the mixer meters: the fill turns yellow past half the buffer time and red when it is all used.
void loadBar(SkCanvas& c, const Theme& t, const Rect& r, float load)
{
    paint::fill(c, r, t.color("MeterBackground"));
    const float shown = std::clamp(load, 0.0f, 1.0f);
    if (shown > 0.0f) paint::fill(c, r.withWidth(std::max(1.0f, r.w * shown)), Fader::meterColour(load, false));
    paint::stroke(c, r, t.color("ControlContrastFrame"));
}
} // namespace

Rect CpuPanel::panelRect() const
{
    const auto& x = ctx();
    const int rows = std::min<int>(kMaxTrackRows, std::max<int>(1, (int) x.cpu.tracks.size()));
    const float more = (int) x.cpu.tracks.size() > kMaxTrackRows ? 12.0f : 0.0f; // the "1-12 of N" line
    const float h = kPad + 22 + 4 * kRow + 10 + 3 * kRow + 10 + 20 + (float) rows * kRow + more + kPad;
    const auto anchor = x.cpuAnchor;
    const float w = std::min(kWidth, width() - 16);
    // Right edge under the CPU box (the panel opens towards the window's centre), clamped into the window.
    float left = anchor.w > 0 ? anchor.right() - w : width() - w - 8;
    left = std::clamp(left, 8.0f, std::max(8.0f, width() - w - 8));
    const float top = anchor.h > 0 ? anchor.bottom() + 4 : 40.0f;
    return {left, top, w, std::min(h, std::max(60.0f, height() - top - 8))};
}

void CpuPanel::close()
{
    auto& x = ctx();
    x.cpuPanelOpen = false;
    firstTrack_ = 0;
    if (x.modelChanged) x.modelChanged();
    if (x.requestRepaint) x.requestRepaint();
}

void CpuPanel::tick()
{
    if (visible && ++ticks_ % 6 == 0) repaint(); // the figures change about ten times a second
}

void CpuPanel::paint(SkCanvas& c, const Theme& t)
{
    hits_.clear();
    const auto& x = ctx();
    const auto& v = x.cpu;
    const auto panel = panelRect();
    paint::fillRounded(c, panel.translated(0, 2), 4.0f, paint::withAlpha(t.color("ShadowDark"), 90)); // drop shadow
    paint::fillRounded(c, panel, 4.0f, t.color("SurfaceBackground"));
    paint::stroke(c, panel, t.color("ControlContrastFrame"));
    const SkColor ink = t.color("ControlForeground"), dim = t.color("TextDisabled");
    const float fs = t.metrics.fontSize, small = t.metrics.smallFontSize;

    auto r = panel.reduced(kPad, kPad);
    {
        auto title = r.removeFromTop(18);
        paint::text(c, "CPU Load", title, ink, fs + 1, Align::Left, true);
        const Rect reset{title.right() - 48, title.y, 48, 16};
        ctl::toggle(c, t, reset, "Reset", false, t.color("ChosenDefault"));
        hits_.push_back({reset, [&x] { if (x.resetCpuStats) x.resetCpuStats(); }});
        r.removeFromTop(4);
    }
    auto meterRow = [&](const std::string& label, float load) {
        auto line = r.removeFromTop(kRow);
        paint::text(c, label, line.removeFromLeft(kLabelW), dim, fs);
        paint::text(c, percent(load), line.removeFromRight(kValueW), ink, fs, Align::Right, load >= 1.0f); // the red bar says it
        line.removeFromRight(6);
        loadBar(c, t, line.reduced(0, 5), load);
    };
    meterRow("Current", v.current);
    meterRow("Average", v.average);
    meterRow("Peak", v.peak);
    {
        auto line = r.removeFromTop(kRow);
        paint::text(c, "Overruns", line.removeFromLeft(kLabelW), dim, fs);
        std::string text = std::to_string(v.overruns) + (v.overruns == 1 ? " block" : " blocks") + " over the buffer time";
        if (v.deviceDropouts >= 0) text += ", driver " + std::to_string(v.deviceDropouts);
        paint::text(c, text, line, ink, fs, Align::Left, v.overloaded);
    }
    r.removeFromTop(4);
    paint::fill(c, r.removeFromTop(1), t.color("ControlContrastFrame"));
    r.removeFromTop(5);
    {
        auto line = r.removeFromTop(kRow);
        paint::text(c, "Buffer", line.removeFromLeft(kLabelW), dim, fs);
        std::string text = v.bufferSize > 0 ? std::to_string(v.bufferSize) + " samples" : std::string("-");
        if (v.sampleRate > 0) text += "  at " + decimal(v.sampleRate / 1000.0, 1) + " kHz";
        if (v.bufferMicros > 0) text += "  (" + decimal(v.bufferMicros / 1000.0, 1) + " ms)";
        paint::text(c, text, line, ink, fs);
    }
    {
        auto line = r.removeFromTop(kRow);
        paint::text(c, "Latency", line.removeFromLeft(kLabelW), dim, fs);
        paint::text(c, "in " + decimal(v.inputLatencyMs, 1) + " ms, out " + decimal(v.outputLatencyMs, 1) + " ms", line, ink, fs);
    }
    {
        auto line = r.removeFromTop(kRow);
        paint::text(c, "Threads", line.removeFromLeft(kLabelW), dim, fs);
        const Rect toggle{line.right() - 70, line.y + 1, 70, kRow - 2};
        ctl::toggle(c, t, toggle, "Multi-Core", v.multiCore, t.color("ChosenDefault"));
        hits_.push_back({toggle, [&x, on = v.multiCore] { if (x.setMultiCore) x.setMultiCore(! on); }});
        line.removeFromRight(76);
        std::string text;
        if (! v.multiCore) text = "1 (multi-core off)";
        else if (v.helpers == 0) text = "1 (no helper threads)";
        else text = std::to_string(v.parallel ? v.threads : 1) + " of " + std::to_string(v.helpers + 1)
                    + (v.parallel ? "" : ", light load") + (v.workgroup ? ", realtime group" : "");
        paint::text(c, text, line, ink, fs);
    }
    r.removeFromTop(4);
    paint::fill(c, r.removeFromTop(1), t.color("ControlContrastFrame"));
    r.removeFromTop(5);
    {
        auto line = r.removeFromTop(20);
        paint::text(c, "Performance Impact", line, ink, fs, Align::Left, true);
        paint::text(c, "share of the buffer time", line, dim, small, Align::Right);
    }
    const int count = (int) v.tracks.size();
    const int rows = std::max(1, std::min(kMaxTrackRows, (int) std::floor((r.h - (count > kMaxTrackRows ? 12.0f : 0.0f)) / kRow)));
    firstTrack_ = std::clamp(firstTrack_, 0, std::max(0, count - rows));
    if (count == 0) paint::text(c, "No set loaded", r.removeFromTop(kRow), dim, fs);
    const float budget = v.bufferMicros > 0 ? v.bufferMicros : 1.0f;
    for (int i = firstTrack_; i < count && i < firstTrack_ + rows; ++i)
    {
        const auto& track = v.tracks[(size_t) i];
        auto line = r.removeFromTop(kRow);
        const auto chip = line.removeFromLeft(4).reduced(0, 3);
        paint::fill(c, chip, Theme::paletteColor(track.colorIndex));
        line.removeFromLeft(6);
        const float share = track.micros / budget;
        paint::text(c, share >= 0.0005f ? decimal(100.0 * share, 1) + " %" : std::string("0 %"), line.removeFromRight(kValueW),
                    ink, fs, Align::Right);
        line.removeFromRight(6);
        const auto bar = line.removeFromRight(std::min(96.0f, line.w * 0.45f));
        loadBar(c, t, bar.reduced(0, 5), share);
        paint::text(c, track.name, line.withWidth(std::max(0.0f, line.w - 6)), ink, fs);
    }
    if (count > rows)
    {
        const std::string more = std::to_string(firstTrack_ + 1) + "-" + std::to_string(std::min(count, firstTrack_ + rows)) + " of "
                                 + std::to_string(count) + " (scroll)";
        paint::text(c, more, {panel.x + kPad, panel.bottom() - kPad - 10, panel.w - 2 * kPad, 10}, dim, small, Align::Right);
    }
}

void CpuPanel::mouseDown(const MouseEvent& e)
{
    for (const auto& h : hits_)
        if (h.area.contains(e.x, e.y))
        {
            auto action = h.action; // may rebuild hits_
            action();
            repaint();
            return;
        }
    if (! panelRect().contains(e.x, e.y)) close(); // a click anywhere else closes it, the CPU box included
}

bool CpuPanel::mouseWheel(const MouseEvent& e)
{
    if (! panelRect().contains(e.x, e.y)) return true;
    firstTrack_ += e.wheelDeltaY < 0 ? 1 : e.wheelDeltaY > 0 ? -1 : 0;
    repaint();
    return true;
}

bool CpuPanel::keyPressed(const KeyEvent& k)
{
    if (! ctx().cpuPanelOpen) return false;
    if (k.keyCode == juce::KeyPress::escapeKey) { close(); return true; }
    return false;
}

} // namespace sonora::ui
