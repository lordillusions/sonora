#include "ui/views/MonitorPanel.h"

#include "include/core/SkPaint.h"
#include "include/core/SkPath.h"

#include "dsp/Analyzers.h"
#include "ui/Controls.h"
#include "ui/Painter.h"
#include "ui/UiContext.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace sonora::ui
{

namespace
{
constexpr float kSpectrumTopDb = 0.0f, kSpectrumBottomDb = -80.0f;
constexpr float kLufsTop = 0.0f, kLufsBottom = -60.0f;

std::string number(float v, const char* format, const char* silence = "-inf")
{
    if (v <= -150.0f)
        return silence;
    char buf[48];
    std::snprintf(buf, sizeof buf, format, (double) v);
    return buf;
}

float frequencyPosition(float hz) // 0 at 20 Hz .. 1 at 20 kHz
{
    return std::clamp(std::log(hz / 20.0f) / std::log(1000.0f), 0.0f, 1.0f);
}

std::string frequencyLabel(float hz)
{
    char buf[16];
    if (hz >= 1000.0f)
        std::snprintf(buf, sizeof buf, "%gk", (double) (hz / 1000.0f));
    else
        std::snprintf(buf, sizeof buf, "%g", (double) hz);
    return buf;
}
} // namespace

double MonitorPanel::scopeWindowMs(int index)
{
    static const double windows[kScopeWindows] = {10.0, 20.0, 50.0, 100.0, 200.0};
    return windows[std::clamp(index, 0, kScopeWindows - 1)];
}

Rect MonitorPanel::loudnessArea() const { return {8, 6, 206, height() - 12}; }
Rect MonitorPanel::stereoArea() const { return {width() - 8 - 220, 6, 220, height() - 12}; }

Rect MonitorPanel::displayArea() const
{
    const float left = loudnessArea().right() + 10, right = stereoArea().x - 10;
    return {left, 30, std::max(40.0f, right - left), std::max(20.0f, height() - 36)};
}

Rect MonitorPanel::viewButton(int index) const
{
    return {displayArea().x + (float) index * 62.0f, 6, 58, 18};
}

Rect MonitorPanel::windowButton() const { return {viewButton(3).x + 6, 6, 62, 18}; }
Rect MonitorPanel::triggerButton() const { return {windowButton().right() + 4, 6, 48, 18}; }
Rect MonitorPanel::resetButton() const { return {loudnessArea().x, loudnessArea().bottom() - 18, 74, 18}; }

void MonitorPanel::tick()
{
    auto& x = ctx();
    if (x.monitor == nullptr)
        return;
    x.monitor->setScope(scopeWindowMs(x.monitorScopeWindow), x.monitorScopeTrigger);
    if (const auto serial = x.monitor->snapshot().serial; serial != shownSerial_)
    {
        shownSerial_ = serial;
        repaint();
    }
}

void MonitorPanel::mouseDown(const MouseEvent& e)
{
    auto& x = ctx();
    for (int i = 0; i < 3; ++i)
        if (viewButton(i).contains(e.x, e.y))
        {
            x.monitorView = i;
            repaint();
            return;
        }
    if (x.monitorView == 2)
    {
        if (windowButton().contains(e.x, e.y))
        {
            x.monitorScopeWindow = (x.monitorScopeWindow + 1) % kScopeWindows;
            repaint();
            return;
        }
        if (triggerButton().contains(e.x, e.y))
        {
            x.monitorScopeTrigger = ! x.monitorScopeTrigger;
            repaint();
            return;
        }
    }
    if (resetButton().contains(e.x, e.y) && x.monitor != nullptr)
    {
        x.monitor->resetLoudness();
        repaint();
    }
}

void MonitorPanel::paint(SkCanvas& c, const Theme& t)
{
    auto& x = ctx();
    paint::fill(c, localBounds(), t.color("DetailViewBackground"));
    if (x.monitor == nullptr)
    {
        paint::text(c, "No audio engine: nothing to monitor.", localBounds(), t.color("TextDisabled"), t.metrics.fontSize + 1, Align::Centre);
        return;
    }
    const auto s = x.monitor->snapshot();
    paintLoudness(c, t, s);

    static const char* views[3] = {"Bars", "Curve", "Scope"};
    for (int i = 0; i < 3; ++i)
        ctl::toggle(c, t, viewButton(i), views[i], x.monitorView == i, t.color("ChosenDefault"));
    if (x.monitorView == 2)
    {
        char buf[24];
        std::snprintf(buf, sizeof buf, "%g ms", scopeWindowMs(x.monitorScopeWindow));
        ctl::toggle(c, t, windowButton(), buf, false, t.color("ChosenDefault"));
        ctl::toggle(c, t, triggerButton(), "Trig", x.monitorScopeTrigger, t.color("ChosenDefault"));
        paintScope(c, t, s);
    }
    else
        paintSpectrum(c, t, s, x.monitorView == 0);
    paintStereo(c, t, s);
}

void MonitorPanel::paintLoudness(SkCanvas& c, const Theme& t, const engine::MonitorSnapshot& s)
{
    const auto area = loudnessArea();
    const SkColor ink = t.color("ControlForeground"), dim = t.color("TextDisabled");
    const float fs = t.metrics.fontSize;

    // vertical LUFS bar: momentary fill, short-term tick
    const Rect bar{area.x, area.y + 2, 14, area.h - 28};
    paint::fill(c, bar, t.color("RetroDisplayBackground"));
    auto yFor = [&](float lufs) { return bar.bottom() - std::clamp((lufs - kLufsBottom) / (kLufsTop - kLufsBottom), 0.0f, 1.0f) * bar.h; };
    if (s.momentaryLufs > kLufsBottom)
        paint::fill(c, {bar.x + 1, yFor(s.momentaryLufs), bar.w - 2, bar.bottom() - yFor(s.momentaryLufs)},
                    s.momentaryLufs > -9.0f ? t.color("ChosenAlert") : t.color("ChosenDefault"));
    if (s.shortTermLufs > kLufsBottom)
        paint::line(c, bar.x, yFor(s.shortTermLufs), bar.right(), yFor(s.shortTermLufs), t.color("RetroDisplayForeground"), 2.0f);
    for (const float mark : {-60.0f, -40.0f, -30.0f, -23.0f, -14.0f, -9.0f, 0.0f})
    {
        const float y = yFor(mark);
        paint::line(c, bar.right(), y, bar.right() + 3, y, dim, 1.0f);
        paint::text(c, number(mark, "%.0f"), {bar.right() + 4, y - 6, 24, 12}, dim, fs - 2, Align::Left);
    }

    struct Row { const char* caption; std::string value; const char* unit; bool big, alert; };
    const Row rows[] = {
        {"INTEGRATED", number(s.integratedLufs, "%.1f"), "LUFS", true, false},
        {"MOMENTARY", number(s.momentaryLufs, "%.1f"), "LUFS", false, false},
        {"SHORT-TERM", number(s.shortTermLufs, "%.1f"), "LUFS", false, false},
        {"LOUDNESS RANGE", s.integratedLufs <= -150.0f ? "-" : number(s.loudnessRange, "%.1f"), "LU", false, false},
        {"TRUE PEAK", number(s.truePeakDb, "%.1f"), "dBTP", false, s.truePeakDb > -1.0f},
        {"SAMPLE PEAK", number(s.samplePeakDb, "%.1f"), "dBFS", false, s.samplePeakDb >= -0.01f},
    };
    const float left = area.x + 44, rowH = std::min(38.0f, (area.h - 26) / 6.0f);
    float y = area.y;
    for (const auto& row : rows)
    {
        paint::text(c, row.caption, {left, y, area.right() - left, 11}, dim, fs - 2, Align::Left, true);
        paint::text(c, row.value, {left, y + 10, 100, rowH - 10}, row.alert ? t.color("ChosenAlert") : ink, row.big ? fs + 9 : fs + 4, Align::Left, row.big);
        paint::text(c, row.unit, {left + 100, y + 10, 44, rowH - 10}, dim, fs - 1, Align::Left);
        y += rowH;
    }
    ctl::toggle(c, t, resetButton(), "Reset", false, t.color("ChosenDefault"));
    char seconds[40];
    std::snprintf(seconds, sizeof seconds, "%d:%02d", (int) s.integratedSeconds / 60, (int) s.integratedSeconds % 60);
    paint::text(c, seconds, {resetButton().right() + 6, resetButton().y, 60, 18}, dim, fs - 1, Align::Left);
}

void MonitorPanel::paintSpectrum(SkCanvas& c, const Theme& t, const engine::MonitorSnapshot& s, bool bars)
{
    const auto d = displayArea();
    paint::fill(c, d, t.color("RetroDisplayBackground"));
    const SkColor scale = t.color("RetroDisplayScaleText"), fg = t.color("RetroDisplayForeground");
    const float fs = t.metrics.fontSize - 2;
    const Rect plot{d.x + 26, d.y + 6, d.w - 32, d.h - 20};
    auto yFor = [&](float db) { return plot.y + std::clamp((kSpectrumTopDb - db) / (kSpectrumTopDb - kSpectrumBottomDb), 0.0f, 1.0f) * plot.h; };
    auto xFor = [&](float hz) { return plot.x + frequencyPosition(hz) * plot.w; };

    for (float db = 0.0f; db >= kSpectrumBottomDb; db -= 20.0f)
    {
        paint::line(c, plot.x, yFor(db), plot.right(), yFor(db), paint::withAlpha(scale, 70), 1.0f);
        paint::text(c, number(db, "%.0f"), {d.x + 2, yFor(db) - 6, 22, 12}, scale, fs, Align::Right);
    }
    for (const float hz : {50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f})
    {
        paint::line(c, xFor(hz), plot.y, xFor(hz), plot.bottom(), paint::withAlpha(scale, 70), 1.0f);
        paint::text(c, frequencyLabel(hz), {xFor(hz) - 16, plot.bottom() + 1, 32, 12}, scale, fs, Align::Centre);
    }

    c.save();
    c.clipRect(plot.sk());
    if (bars)
    {
        const float slot = plot.w / (float) dsp::SpectrumAnalyzer::kBars;
        for (int i = 0; i < dsp::SpectrumAnalyzer::kBars; ++i)
        {
            const float top = yFor(s.bars[(size_t) i]);
            if (top < plot.bottom() - 0.5f)
                paint::fill(c, {plot.x + (float) i * slot + 0.5f, top, std::max(1.0f, slot - 1.0f), plot.bottom() - top}, paint::withAlpha(fg, 200));
            const float peak = yFor(s.barPeaks[(size_t) i]);
            if (peak < plot.bottom() - 1.0f)
                paint::fill(c, {plot.x + (float) i * slot + 0.5f, peak - 1.0f, std::max(1.0f, slot - 1.0f), 1.5f}, fg);
        }
    }
    else
    {
        SkPath line, area;
        for (int i = 0; i < dsp::SpectrumAnalyzer::kCurvePoints; ++i)
        {
            const float px = xFor(dsp::SpectrumAnalyzer::curveFrequency(i)), py = yFor(s.curve[(size_t) i]);
            if (i == 0)
            {
                line.moveTo(px, py);
                area.moveTo(px, plot.bottom());
                area.lineTo(px, py);
            }
            else
            {
                line.lineTo(px, py);
                area.lineTo(px, py);
            }
        }
        area.lineTo(plot.right(), plot.bottom());
        area.close();
        SkPaint fill;
        fill.setAntiAlias(true);
        fill.setColor(paint::withAlpha(fg, 60));
        c.drawPath(area, fill);
        SkPaint stroke;
        stroke.setAntiAlias(true);
        stroke.setStyle(SkPaint::kStroke_Style);
        stroke.setStrokeWidth(1.6f);
        stroke.setColor(fg);
        c.drawPath(line, stroke);
    }
    c.restore();
}

void MonitorPanel::paintScope(SkCanvas& c, const Theme& t, const engine::MonitorSnapshot& s)
{
    const auto d = displayArea();
    paint::fill(c, d, t.color("RetroDisplayBackground"));
    const SkColor scale = t.color("RetroDisplayScaleText"), fg = t.color("RetroDisplayForeground");
    const float fs = t.metrics.fontSize - 2;
    const Rect plot = d.reduced(6, 4);
    const float laneH = plot.h * 0.5f;
    for (int lane = 0; lane < 2; ++lane)
    {
        const float mid = plot.y + laneH * ((float) lane + 0.5f), half = laneH * 0.46f;
        paint::line(c, plot.x, mid, plot.right(), mid, paint::withAlpha(scale, 90), 1.0f);
        paint::line(c, plot.x, mid - half, plot.right(), mid - half, paint::withAlpha(scale, 40), 1.0f);
        paint::line(c, plot.x, mid + half, plot.right(), mid + half, paint::withAlpha(scale, 40), 1.0f);
        paint::text(c, lane == 0 ? "L" : "R", {plot.x + 2, mid - half, 14, 12}, scale, fs, Align::Left, true);
        const auto columns = (int) s.scope.size();
        for (int i = 0; i < columns; ++i)
        {
            const auto& col = s.scope[(size_t) i];
            const float lo = lane == 0 ? col.minL : col.minR, hi = lane == 0 ? col.maxL : col.maxR;
            const float px = plot.x + ((float) i + 0.5f) * plot.w / (float) columns;
            const float y0 = mid - std::clamp(hi, -1.0f, 1.0f) * half, y1 = mid - std::clamp(lo, -1.0f, 1.0f) * half;
            paint::line(c, px, y0, px, std::max(y1, y0 + 1.0f), fg, std::max(1.0f, plot.w / (float) columns));
        }
    }
}

void MonitorPanel::paintStereo(SkCanvas& c, const Theme& t, const engine::MonitorSnapshot& s)
{
    const auto area = stereoArea();
    const SkColor ink = t.color("ControlForeground"), dim = t.color("TextDisabled");
    const float fs = t.metrics.fontSize;

    paint::text(c, "CORRELATION", {area.x, area.y, 100, 11}, dim, fs - 2, Align::Left, true);
    paint::text(c, number(s.correlation, "%+.2f"), {area.right() - 60, area.y - 2, 60, 14}, ink, fs + 2, Align::Right, true);
    const Rect meter{area.x, area.y + 16, area.w, 12};
    paint::fill(c, meter, t.color("RetroDisplayBackground"));
    const float centre = meter.centreX();
    const float value = std::clamp(s.correlation, -1.0f, 1.0f);
    const SkColor good = t.color("ChosenDefault"), bad = t.color("ChosenAlert");
    if (value >= 0.0f)
        paint::fill(c, {centre, meter.y + 1, value * meter.w * 0.5f, meter.h - 2}, good);
    else
        paint::fill(c, {centre + value * meter.w * 0.5f, meter.y + 1, -value * meter.w * 0.5f, meter.h - 2}, bad);
    paint::line(c, centre, meter.y, centre, meter.bottom(), t.color("RetroDisplayScaleText"), 1.0f);
    paint::text(c, "-1", {meter.x, meter.bottom() + 1, 20, 11}, dim, fs - 2, Align::Left);
    paint::text(c, "0", {centre - 10, meter.bottom() + 1, 20, 11}, dim, fs - 2, Align::Centre);
    paint::text(c, "+1", {meter.right() - 20, meter.bottom() + 1, 20, 11}, dim, fs - 2, Align::Right);

    // goniometer (mid up, left on the upper left diagonal)
    const float bottomRows = 44.0f;
    const float side = std::max(40.0f, std::min(area.w - 24.0f, area.h - 48.0f - bottomRows));
    const Rect gon{area.centreX() - side * 0.5f, meter.bottom() + 16, side, side};
    paint::fill(c, gon, t.color("RetroDisplayBackground"));
    const SkColor scale = t.color("RetroDisplayScaleText");
    paint::line(c, gon.centreX(), gon.y, gon.centreX(), gon.bottom(), paint::withAlpha(scale, 90), 1.0f);
    paint::line(c, gon.x, gon.centreY(), gon.right(), gon.centreY(), paint::withAlpha(scale, 60), 1.0f);
    paint::line(c, gon.x, gon.y, gon.right(), gon.bottom(), paint::withAlpha(scale, 50), 1.0f);
    paint::line(c, gon.right(), gon.y, gon.x, gon.bottom(), paint::withAlpha(scale, 50), 1.0f);
    paint::text(c, "L", {gon.x + 2, gon.y + 1, 12, 11}, scale, fs - 2, Align::Left, true);
    paint::text(c, "R", {gon.right() - 14, gon.y + 1, 12, 11}, scale, fs - 2, Align::Right, true);
    paint::text(c, "M", {gon.centreX() + 3, gon.y + 1, 12, 11}, scale, fs - 2, Align::Left, true);
    const float scaleXY = side * 0.5f / 1.4142f;
    std::vector<SkPoint> points;
    points.reserve(s.goniometer.size());
    for (const auto& p : s.goniometer)
        points.push_back({gon.centreX() - std::clamp(p.first, -1.4142f, 1.4142f) * scaleXY,
                          gon.centreY() - std::clamp(p.second, -1.4142f, 1.4142f) * scaleXY});
    SkPaint dots;
    dots.setAntiAlias(true);
    dots.setStrokeWidth(1.6f);
    dots.setStrokeCap(SkPaint::kRound_Cap);
    dots.setColor(paint::withAlpha(t.color("RetroDisplayForeground"), 170));
    c.save();
    c.clipRect(gon.sk());
    c.drawPoints(SkCanvas::kPoints_PointMode, SkSpan<const SkPoint>(points.data(), points.size()), dots);
    c.restore();

    // balance and width
    const float rowY = gon.bottom() + 6;
    paint::text(c, "BALANCE", {area.x, rowY, 70, 11}, dim, fs - 2, Align::Left, true);
    const Rect bal{area.x + 64, rowY, area.w - 64, 11};
    paint::fill(c, bal, t.color("RetroDisplayBackground"));
    const float b = std::clamp(s.balance, -1.0f, 1.0f);
    paint::fill(c, b >= 0 ? Rect{bal.centreX(), bal.y + 1, b * bal.w * 0.5f, bal.h - 2} : Rect{bal.centreX() + b * bal.w * 0.5f, bal.y + 1, -b * bal.w * 0.5f, bal.h - 2},
                t.color("ChosenDefault"));
    paint::line(c, bal.centreX(), bal.y, bal.centreX(), bal.bottom(), t.color("RetroDisplayScaleText"), 1.0f);
    paint::text(c, "L", {bal.x + 2, bal.bottom() + 1, 10, 10}, dim, fs - 3, Align::Left);
    paint::text(c, "R", {bal.right() - 12, bal.bottom() + 1, 10, 10}, dim, fs - 3, Align::Right);
    char width[32];
    std::snprintf(width, sizeof width, "%d %%", (int) std::lround(s.width * 100.0f));
    paint::text(c, "WIDTH", {area.x, rowY + 24, 60, 11}, dim, fs - 2, Align::Left, true);
    paint::text(c, width, {area.x + 64, rowY + 22, 60, 14}, ink, fs + 1, Align::Left);
}

} // namespace sonora::ui
