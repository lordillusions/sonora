#include "ui/widgets/Fader.h"
#include "ui/TrackMenu.h"
#include <juce_gui_basics/juce_gui_basics.h>

#include "core/FaderCurve.h"
#include "ui/Controls.h"
#include "ui/MeterBallistics.h"
#include "ui/Painter.h"

#include "include/core/SkPaint.h"
#include "include/core/SkPathBuilder.h"

#include <cmath>
#include <cstdio>

namespace sonora::ui
{

float Fader::gainToPosition(float gain)
{
    return model::faderGainToPosition(gain);
}

float Fader::positionToGain(float pos)
{
    return model::faderPositionToGain(pos);
}

void Fader::tick()
{
    const float l = getPeakL ? getPeakL() : 0.0f;
    const float r = getPeakR ? getPeakR() : 0.0f;
    const float nl = meterDecay(l, shownL_);
    const float nr = meterDecay(r, shownR_);
    if (std::abs(nl - shownL_) > 0.0f || std::abs(nr - shownR_) > 0.0f)
    {
        shownL_ = nl;
        shownR_ = nr;
        repaint();
    }
}

namespace
{
constexpr float kHandleW = 11.5f, kMeterW = 12.0f, kHandleH = 13.0f;
} // namespace

Rect Fader::meterRect() const
{
    return {kHandleW + 1.0f, 0.0f, kMeterW, height()};
}

void Fader::paint(SkCanvas& c, const Theme& t)
{
    const Rect meter = meterRect();
    paint::fill(c, meter, t.color("MeterBackground"));
    paint::fill(c, {meter.centreX() - 0.5f, meter.y, 1, meter.h}, t.color("DisplayBackground"));

    auto bar = [&](float level, Rect area) {
        const float pos = gainToPosition(level);
        if (pos <= 0)
            return;
        Rect lit{area.x, area.bottom() - area.h * pos, area.w, area.h * pos};
        const SkColor col = level >= 1.0f ? SkColorSetRGB(0xff, 0x0a, 0x0a)
                            : level > 0.5f ? SkColorSetRGB(0xff, 0xd1, 0x00)
                                           : SkColorSetRGB(0x00, 0xf7, 0x58);
        paint::fill(c, lit, col);
    };
    const float half = (meter.w - 1) * 0.5f;
    bar(shownL_, {meter.x, meter.y, half, meter.h});
    bar(shownR_, {meter.x + half + 1, meter.y, half, meter.h});

    // 0 dB line across the meter, scale ticks and labels (0, 12 ... 60 dB below unity) to its right.
    const float zeroY = meter.bottom() - meter.h * gainToPosition(1.0f);
    paint::line(c, meter.x, zeroY, meter.right(), zeroY, paint::withAlpha(t.color("ControlDisabled"), 180));
    const float labelX = meter.right() + 4;
    for (int db : {0, 12, 24, 36, 48, 60})
    {
        const float y = meter.bottom() - meter.h * gainToPosition(std::pow(10.0f, -(float) db / 20.0f));
        paint::line(c, meter.right() + 0.5f, std::round(y) + 0.5f, meter.right() + 3.5f, std::round(y) + 0.5f,
                    paint::withAlpha(t.color("ControlForeground"), 150));
        paint::text(c, std::to_string(db), Rect{labelX, y - 6, width() - labelX, 12}, t.color("ControlForeground"),
                    t.metrics.smallFontSize);
    }

    // Handle: a solid triangle pointing at the meter from the left.
    const float g = getGain ? getGain() : 1.0f;
    const float y = meter.bottom() - meter.h * gainToPosition(g);
    ctl::triangle(c, kHandleW * 0.5f, y, kHandleH, kHandleW, ctl::Dir::Right, t.color("ControlForeground"));
}

bool Fader::keyPressed(const KeyEvent& key)
{
    if (key.cmd || key.alt || key.ctrl || ! setGain) return false;
    if (key.keyCode == juce::KeyPress::deleteKey || key.keyCode == juce::KeyPress::backspaceKey) setGain(1.0f);
    else if (key.keyCode == juce::KeyPress::upKey || key.keyCode == juce::KeyPress::downKey)
    {
        const float step = (key.shift ? 0.001f : 0.01f) * (key.keyCode == juce::KeyPress::upKey ? 1.0f : -1.0f);
        setGain(positionToGain(std::clamp(gainToPosition(getGain ? getGain() : 1.0f) + step, 0.0f, 1.0f)));
    }
    else return false;
    repaint(); return true;
}

void Fader::mouseDown(const MouseEvent& e)
{
    if (e.right) { showControlContextMenu(*this, ctx(), [this] { if (setGain) setGain(1.0f); repaint(); }); return; }
    dragStartY_ = e.y;
    dragStartPos_ = gainToPosition(getGain ? getGain() : 1.0f);
}

void Fader::mouseDrag(const MouseEvent& e)
{
    if (e.right) return;
    const float h = std::max(20.0f, height());
    const float pos = dragStartPos_ + (dragStartY_ - e.y) / (e.shift ? h * 5 : h);
    if (setGain)
        setGain(positionToGain(pos));
    repaint();
}

void Fader::mouseDoubleClick(const MouseEvent& event)
{
    if (event.right) { mouseDown(event); return; }
    if (setGain)
        setGain(1.0f);
    repaint();
}

} // namespace sonora::ui
