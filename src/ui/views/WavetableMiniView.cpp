#include "ui/views/WavetableMiniView.h"

#include "devices/native/WavetableModRouting.h"
#include "ui/Painter.h"
#include "ui/Theme.h"

#include "include/core/SkCanvas.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPathBuilder.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace sonora::ui
{
namespace
{
using W = devices::WavetableSynth;
// The dashboard's palette (WavetableEditor): Prism keeps its dark face in every theme, folded or not.
constexpr SkColor kBackground = 0xff11171f, kPanel = 0xff202a35, kRaised = 0xff2d3946, kLine = 0xff3b4b5c;
constexpr SkColor kText = 0xffe5edf7, kDim = 0xff96a9bd, kCyan = 0xff43d9e7, kPlot = 0xff111d29, kFrame = 0xffffd23f;
constexpr SkColor kMeter = 0xff4aa3ff, kMeterHot = 0xffff6b5b, kMeterBack = 0xff0b1118;
constexpr float kMargin = 4, kGap = 3, kMeterWidth = 9, kBadge = 15, kPlotWidth = 92;

std::string number(const char* format, double value)
{
    char text[48];
    std::snprintf(text, sizeof(text), format, value);
    return text;
}
bool tableMode(const W& synth, int oscillator)
{
    return (int) std::lround(synth.getParameter(W::mediaParam(oscillator, W::MediaMode))) == W::WavetableMode;
}
bool oscillatorOn(const W& synth, int oscillator) { return synth.getParameter(W::oscParam(oscillator, W::On)) > 0.5f; }
} // namespace

Rect WavetableMiniView::oscillatorRow(int oscillator) const
{
    const float rowHeight = std::max(16.0f, (height() - 2 * kMargin - (float) (W::kOscillators - 1) * kGap) / (float) W::kOscillators);
    return {kMargin, kMargin + (float) oscillator * (rowHeight + kGap), std::max(40.0f, width() - 2 * kMargin - kMeterWidth - kGap), rowHeight};
}
Rect WavetableMiniView::oscillatorPlot(int oscillator) const
{
    const auto row = oscillatorRow(oscillator);
    return {row.x + kBadge + 4, row.y, std::min(kPlotWidth, row.w * 0.5f), row.h};
}
Rect WavetableMiniView::meterBar(int channel) const
{
    const float top = kMargin, h = std::max(10.0f, height() - 2 * kMargin);
    return {width() - kMargin - kMeterWidth + (float) channel * 5, top, 4, h};
}

float WavetableMiniView::liveAmount(int destination) const
{
    if (! live_.active) return 0;
    float sum = 0;
    for (const auto& route : devices::wavetable_routing::routesTo(*processor_, destination)) sum += route.amount * live_.route[(size_t) route.slot];
    return sum;
}
float WavetableMiniView::livePosition(int oscillator) const
{
    return processor_->getParameter(W::oscParam(oscillator, W::Position)) + liveAmount(W::Osc1Position + oscillator);
}
float WavetableMiniView::markerPosition(int oscillator) const
{
    if (! tableMode(*processor_, oscillator))
        return std::clamp(processor_->getParameter(W::mediaParam(oscillator, W::MediaStartPosition)), 0.0f, 1.0f);
    const int frames = std::max(1, processor_->tableFrameCount(oscillator));
    return frames > 1 ? std::clamp(processor_->tableFramePosition(oscillator, livePosition(oscillator)) / (float) (frames - 1), 0.0f, 1.0f) : 0.0f;
}

float WavetableMiniView::signature() const
{
    float sum = live_.active ? 0.5f : 0.0f;
    for (int o = 0; o < W::kOscillators; ++o)
    {
        const float k = (float) (o + 1);
        sum += (oscillatorOn(*processor_, o) ? 1.0f : 0.0f) * 3 * k + processor_->getParameter(W::oscParam(o, W::Table)) * 5 * k
             + processor_->getParameter(W::mediaParam(o, W::MediaMode)) * 7 * k + markerPosition(o) * 11 * k
             + processor_->getParameter(W::oscParam(o, W::Level)) * 13 * k
             + (processor_->getParameter(W::oscParam(o, W::Warp1Amount)) + liveAmount(W::Osc1Warp1 + o)) * 17 * k
             + (processor_->getParameter(W::oscParam(o, W::Warp2Amount)) + liveAmount(W::Osc1Warp2 + o)) * 19 * k
             + (float) processor_->tableFrameCount(o) * 0.01f * k;
    }
    return sum + meter_[0] * 23 + meter_[1] * 29;
}

void WavetableMiniView::tick()
{
    live_ = processor_->liveState();
    for (size_t channel = 0; channel < 2; ++channel)
    {
        meter_[channel] = std::max(live_.outputPeak[channel], meter_[channel] * 0.82f); // the dashboard's falling hold
        if (meter_[channel] < 1e-5f) meter_[channel] = 0;
    }
    const float now = signature();
    if (std::abs(now - signature_) > 1e-6f)
    {
        signature_ = now;
        repaint();
    }
}

void WavetableMiniView::mouseUp(const MouseEvent& e)
{
    if (! e.right && localBounds().contains(e.x, e.y) && onClick) onClick();
}

void WavetableMiniView::paint(SkCanvas& c, const Theme&)
{
    paint::fillRounded(c, localBounds(), 2, kBackground);
    for (int o = 0; o < W::kOscillators; ++o)
    {
        const auto row = oscillatorRow(o), plot = oscillatorPlot(o);
        const bool on = oscillatorOn(*processor_, o), table = tableMode(*processor_, o);
        paint::fillRounded(c, row, 2, kPanel);

        // Oscillator letter: lit while the oscillator plays.
        const Rect badge{row.x + 2, row.centreY() - kBadge * 0.5f, kBadge, kBadge};
        paint::fillRounded(c, badge, 2, on ? kCyan : kRaised);
        paint::text(c, std::string(1, (char) ('A' + o)), badge, on ? kBackground : kDim, 9, Align::Centre, true);

        // Current waveform: the frame the live position selects (with warps), or the sample preview.
        paint::fill(c, plot, kPlot);
        paint::line(c, plot.x, plot.centreY(), plot.right(), plot.centreY(), paint::withAlpha(kLine, 120));
        const float amplitude = plot.h * 0.36f, centre = plot.centreY() - 2;
        const auto colour = on ? kCyan : paint::mix(kDim, kPlot, 0.45f);
        SkPathBuilder line, area;
        area.moveTo(plot.x, centre);
        constexpr int points = 72;
        const auto preview = table ? std::array<float, W::kPreviewSamples>{} : processor_->wavePreview(o);
        const float position = livePosition(o);
        const float warp1 = processor_->getParameter(W::oscParam(o, W::Warp1Amount)) + liveAmount(W::Osc1Warp1 + o);
        const float warp2 = processor_->getParameter(W::oscParam(o, W::Warp2Amount)) + liveAmount(W::Osc1Warp2 + o);
        for (int i = 0; i <= points; ++i)
        {
            const float phase = (float) i / points;
            const float v = table ? processor_->waveformAt(o, phase, position, warp1, warp2)
                                  : preview[(size_t) std::min(W::kPreviewSamples - 1, (int) std::lround(phase * (W::kPreviewSamples - 1)))];
            const float x = plot.x + 2 + phase * (plot.w - 4), y = centre - std::clamp(v, -1.2f, 1.2f) * amplitude;
            if (i == 0) line.moveTo(x, y); else line.lineTo(x, y);
            area.lineTo(x, y);
        }
        area.lineTo(plot.right() - 2, centre); area.close();
        SkPaint pen; pen.setAntiAlias(true);
        pen.setColor(paint::withAlpha(colour, on ? 34 : 14)); c.drawPath(area.detach(), pen);
        pen.setColor(colour); pen.setStrokeWidth(1.4f); pen.setStyle(SkPaint::kStroke_Style); pen.setStrokeJoin(SkPaint::kRound_Join);
        c.drawPath(line.detach(), pen);

        // Position marker along the bottom of the plot: through the table's frames, or the sample's start.
        const Rect track{plot.x + 3, plot.bottom() - 4, plot.w - 6, 2};
        paint::fill(c, track, paint::withAlpha(kLine, 200));
        const float markerX = track.x + markerPosition(o) * track.w;
        paint::fill(c, {markerX - 1.5f, track.y - 1, 3, 4}, on ? kFrame : paint::mix(kFrame, kPlot, 0.5f));

        // Name, position and level.
        const Rect label{plot.right() + 5, row.y + 2, std::max(10.0f, row.right() - plot.right() - 7), std::min(12.0f, (row.h - 4) / 3)};
        const int tableIndex = (int) std::lround(processor_->getParameter(W::oscParam(o, W::Table)));
        std::string name = ! table ? processor_->oscillatorMediaName(o) : tableIndex == W::Custom ? processor_->customTableName(o) : W::tableName(tableIndex);
        if (name.empty()) name = table ? "Wavetable" : "Sample";
        const float lineStep = std::max(label.h, (row.h - 4) / 3);
        paint::text(c, name, label, on ? kText : kDim, 8.5f, Align::Left, true);
        std::string where;
        if (table)
        {
            const int frames = std::max(1, processor_->tableFrameCount(o));
            where = frames > 1 ? "Frame " + std::to_string((int) std::lround(processor_->tableFramePosition(o, position)) + 1) + " / " + std::to_string(frames)
                               : "Single frame";
        }
        else where = number("Start %.0f %%", markerPosition(o) * 100);
        paint::text(c, where, label.translated(0, lineStep), kDim, 8);
        paint::text(c, on ? number("Level %.0f %%", processor_->getParameter(W::oscParam(o, W::Level)) * 100) : std::string("Off"),
                    label.translated(0, 2 * lineStep), on ? kDim : paint::mix(kDim, kPanel, 0.4f), 8);
    }

    // Output meter: block peaks with the falling hold, -48..0 dB, as beside the dashboard's MAIN.
    for (int channel = 0; channel < 2; ++channel)
    {
        const auto bar = meterBar(channel);
        paint::fill(c, bar, kMeterBack);
        const float db = 20 * std::log10(std::max(meter_[(size_t) channel], 1e-5f));
        const float fill = std::clamp((db + 48) / 48, 0.0f, 1.0f) * bar.h;
        paint::fill(c, {bar.x, bar.bottom() - fill, bar.w, fill}, db > -1 ? kMeterHot : kMeter);
    }
}
} // namespace sonora::ui
