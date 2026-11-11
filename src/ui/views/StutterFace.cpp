#include "ui/views/StutterFace.h"

#include "ui/Painter.h"
#include "ui/widgets/Buttons.h"
#include "ui/widgets/Knob.h"

#include "include/core/SkCanvas.h"
#include "include/core/SkPaint.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace sonora::ui
{
namespace
{
using S = devices::Stutter;
constexpr float kGap = 6, kTitle = 15, kPadWidth = 150, kKnobColumn = 76, kOutWidth = 84, kPatternHeight = 24;
// The displays keep their own dark palette in every theme: the colours carry the meaning (the modes).
constexpr SkColor kScreen = 0xff15181c, kGrid = 0xff272c33, kScale = 0xff7d8590, kInk = 0xffeef1f4;

std::string format(const devices::ParameterInfo& info, float v)
{
    char buffer[32];
    if (! info.choices.empty()) return info.choices[(size_t) std::clamp((int) std::lround(v), 0, (int) info.choices.size() - 1)];
    if (info.unit == "ms") std::snprintf(buffer, sizeof buffer, v < 10 ? "%.1f ms" : "%.0f ms", v);
    else if (info.unit == "dB") std::snprintf(buffer, sizeof buffer, "%.1f dB", v);
    else if (info.unit == "st") std::snprintf(buffer, sizeof buffer, "%+.0f st", v);
    else if (info.unit == "%") std::snprintf(buffer, sizeof buffer, "%.0f %%", v * 100);
    else if (info.id == "Tone")
    {
        if (std::abs(v) < 0.01f) return "Open";
        std::snprintf(buffer, sizeof buffer, v < 0 ? "LP %.0f" : "HP %.0f", std::abs(v) * 100);
    }
    else std::snprintf(buffer, sizeof buffer, "%.2f", v);
    return buffer;
}
} // namespace

StutterFace::StutterFace(devices::Stutter* processor, std::function<void(Widget*, int)> tag)
    : processor_(processor), tag_(std::move(tag))
{
    rate_ = knob(S::Rate);
    length_ = knob(S::LengthMs);
    shrink_ = knob(S::Shrink);
    spacing_ = knob(S::Spacing);
    quantize_ = knob(S::Quantize);
    smooth_ = knob(S::Smooth);
    pitchKnob_ = knob(S::Pitch);
    ramp_ = knob(S::Ramp);
    decay_ = knob(S::Decay);
    wide_ = knob(S::Wide);
    chance_ = knob(S::Chance);
    tone_ = knob(S::Tone);
    mix_ = knob(S::Mix);
    output_ = knob(S::Output);
    rate_->tooltip = "Rate: the slice length in note values (it follows the tempo)";
    shrink_->tooltip = "Shrink: each repeat shorter than the one before (a roll that speeds up)";
    spacing_->tooltip = "Spacing: how much of each repeat sounds; the rest is silence (Gate: the open part)";
    ramp_->tooltip = "Ramp: semitones added on every repeat (rising or falling stutters)";
    chance_->tooltip = "Chance: how likely each pattern step fires";
    sync_ = add<TextButton>("Sync", TextButton::Style::Toggle);
    sync_->isOn = [this] { return processor_->getParameter(S::Sync) > 0.5f; };
    sync_->onClick = [this] { setParameter(S::Sync, processor_->getParameter(S::Sync) > 0.5f ? 0.0f : 1.0f); };
    sync_->tooltip = "Sync: slices in note values (off: Length in ms)";
    if (tag_) tag_(sync_, S::Sync);
    pattern_ = add<TextButton>("Pattern", TextButton::Style::Toggle);
    pattern_->isOn = [this] { return processor_->getParameter(S::PatternOn) > 0.5f; };
    pattern_->onClick = [this] { setParameter(S::PatternOn, processor_->getParameter(S::PatternOn) > 0.5f ? 0.0f : 1.0f); };
    pattern_->tooltip = "Pattern: play the 16 steps below while the transport runs (one bar of 1/16 steps)";
    if (tag_) tag_(pattern_, S::PatternOn);
}

void StutterFace::setParameter(int parameter, float value)
{
    processor_->setParameter(parameter, value);
    repaint();
}

Knob* StutterFace::knob(int parameter)
{
    const auto& info = processor_->parameters()[(size_t) parameter];
    const bool stepped = ! info.choices.empty() || info.unit == "st";
    Knob::Source source;
    source.get = [this, parameter] { return processor_->getParameter(parameter); };
    source.set = [this, parameter, stepped](float v) { processor_->setParameter(parameter, stepped ? std::round(v) : v); };
    source.minValue = info.minValue;
    source.maxValue = info.maxValue;
    source.defaultValue = info.defaultValue;
    source.bipolar = info.minValue < 0 && info.maxValue > 0;
    source.logarithmic = info.unit == "ms";
    source.format = [info](float v) { return format(info, v); };
    auto* k = add<Knob>(info.name, std::move(source));
    k->style = Knob::Style::Compact;
    if (k->tooltip.empty()) k->tooltip = info.name + ": double-click to reset; Shift-drag for fine adjustment.";
    if (tag_) tag_(k, parameter);
    return k;
}

// ---------------------------------------------------------------------------------------------------------------------
// Layout

void StutterFace::layout()
{
    const float w = width(), h = height();
    const float fixed = kPadWidth + 4 * kKnobColumn + kOutWidth + 4 * kGap;
    const float displayWidth = std::max(240.0f, w - fixed);
    pads_ = {0, kPadWidth};
    display_ = {pads_.x + pads_.w + kGap, displayWidth};
    slice_ = {display_.x + display_.w + kGap, 2 * kKnobColumn};
    pitch_ = {slice_.x + slice_.w + kGap, 2 * kKnobColumn};
    out_ = {pitch_.x + pitch_.w + kGap, kOutWidth};

    const float body = std::max(40.0f, h - kTitle);
    const float row = std::clamp(body / 3.0f, 26.0f, 48.0f);
    const auto place = [&](Widget* widget, const Column& col, int column, int index, float columnWidth) {
        widget->setBounds({col.x + 2 + (float) column * columnWidth, kTitle + (float) index * row + 2, columnWidth - 4,
                           std::min(row - 4, 34.0f)});
    };
    const bool synced = processor_->getParameter(S::Sync) > 0.5f;
    rate_->visible = synced;
    length_->visible = ! synced;
    place(rate_, slice_, 0, 0, kKnobColumn);
    place(length_, slice_, 0, 0, kKnobColumn);
    place(shrink_, slice_, 0, 1, kKnobColumn);
    place(spacing_, slice_, 0, 2, kKnobColumn);
    place(quantize_, slice_, 1, 0, kKnobColumn);
    place(smooth_, slice_, 1, 1, kKnobColumn);
    sync_->setBounds({slice_.x + kKnobColumn + 4, kTitle + 2 * row + 6, 52, 16});
    place(pitchKnob_, pitch_, 0, 0, kKnobColumn);
    place(ramp_, pitch_, 0, 1, kKnobColumn);
    place(decay_, pitch_, 0, 2, kKnobColumn);
    place(wide_, pitch_, 1, 0, kKnobColumn);
    place(chance_, pitch_, 1, 1, kKnobColumn);
    place(tone_, out_, 0, 0, kOutWidth);
    place(mix_, out_, 0, 1, kOutWidth);
    place(output_, out_, 0, 2, kOutWidth);
    pattern_->setBounds({display_.x + display_.w - 58, 0, 58, kTitle - 2});
}

Rect StutterFace::padArea(int index) const
{
    // Two columns, three rows: Repeat Reverse / Freeze Stop / Gate Latch.
    const float body = std::max(40.0f, height() - kTitle);
    const float cellW = (pads_.w - 4) / 2.0f, cellH = (body - 4) / 3.0f;
    const int column = index % 2, rowIndex = index / 2;
    return {pads_.x + (float) column * (cellW + 4), kTitle + (float) rowIndex * (cellH + 2), cellW, cellH - 2};
}

Rect StutterFace::displayArea() const
{
    return {display_.x, kTitle, display_.w, std::max(20.0f, height() - kTitle - kPatternHeight - 4)};
}

Rect StutterFace::stepArea(int step) const
{
    const float top = height() - kPatternHeight, groupGap = 4;
    const float cell = (display_.w - 3 * groupGap - (float) (S::kSteps - 1) * 2) / (float) S::kSteps;
    const float x = display_.x + (float) step * (cell + 2) + (float) (step / 4) * groupGap;
    return {x, top, cell, kPatternHeight - 2};
}

int StutterFace::padAt(float x, float y) const
{
    for (int i = 0; i <= S::kModes; ++i)
        if (padArea(i).contains(x, y)) return i;
    return -1;
}

int StutterFace::stepAt(float x, float y) const
{
    for (int s = 0; s < S::kSteps; ++s)
        if (stepArea(s).contains(x, y)) return s;
    return -1;
}

// ---------------------------------------------------------------------------------------------------------------------
// Input

void StutterFace::mouseDown(const MouseEvent& e)
{
    if (const int pad = padAt(e.x, e.y); pad >= 0)
    {
        if (pad == S::kModes) // Latch = the Hold control
        {
            setParameter(S::Hold, processor_->getParameter(S::Hold) > 0.5f ? 0.0f : 1.0f);
            return;
        }
        setParameter(S::ModeParam, (float) pad);
        processor_->press(pad); // momentary: stutters while the mouse is down
        pressedPad_ = pad;
        return;
    }
    if (const int step = stepAt(e.x, e.y); step >= 0)
    {
        const int mode = (int) std::lround(processor_->getParameter(S::ModeParam)) + 1;
        const int current = (int) std::lround(processor_->getParameter(S::stepParam(step)));
        paintValue_ = e.alt || e.right || current == mode ? 0 : mode;
        lastPainted_ = step;
        setParameter(S::stepParam(step), (float) paintValue_);
    }
}

void StutterFace::mouseDrag(const MouseEvent& e)
{
    if (paintValue_ < 0) return;
    const int step = stepAt(e.x, std::clamp(e.y, height() - kPatternHeight + 1, height() - 3));
    if (step < 0 || step == lastPainted_) return;
    // Every cell between the last painted one and this one: a fast drag skips cells.
    const int from = lastPainted_ < 0 ? step : lastPainted_;
    for (int s = std::min(from, step); s <= std::max(from, step); ++s) processor_->setParameter(S::stepParam(s), (float) paintValue_);
    lastPainted_ = step;
    repaint();
}

void StutterFace::mouseUp(const MouseEvent&)
{
    if (pressedPad_ >= 0) processor_->press(-1);
    pressedPad_ = paintValue_ = lastPainted_ = -1;
    repaint();
}

void StutterFace::tick()
{
    const auto before = view_;
    processor_->readView(view_);
    const bool synced = processor_->getParameter(S::Sync) > 0.5f;
    if (rate_->visible != synced) layout();
    if (view_.engaged || before.engaged != view_.engaged || before.serial != view_.serial || before.step != view_.step
        || lastEngaged_ != view_.engaged)
        repaint();
    lastEngaged_ = view_.engaged;
}

// ---------------------------------------------------------------------------------------------------------------------
// Painting

void StutterFace::paint(SkCanvas& c, const Theme& t)
{
    const auto fg = t.color("ControlForeground"), dim = t.color("TextDisabled");
    const float size = t.metrics.smallFontSize;
    paint::text(c, "TRIGGER", {pads_.x + 2, 0, pads_.w, kTitle - 2}, fg, size, Align::Left, true);
    paint::text(c, "hold a pad", {pads_.x, 0, pads_.w - 2, kTitle - 2}, dim, size - 1, Align::Right);
    paint::text(c, "SLICE", {display_.x + 2, 0, display_.w, kTitle - 2}, fg, size, Align::Left, true);
    paint::text(c, "TIME", {slice_.x + 2, 0, slice_.w, kTitle - 2}, fg, size, Align::Left, true);
    paint::text(c, "PITCH", {pitch_.x + 2, 0, pitch_.w, kTitle - 2}, fg, size, Align::Left, true);
    paint::text(c, "OUT", {out_.x + 2, 0, out_.w, kTitle - 2}, fg, size, Align::Left, true);
    for (const auto* col : {&slice_, &pitch_, &out_})
        paint::line(c, col->x - kGap * 0.5f, 2, col->x - kGap * 0.5f, height() - 2, paint::withAlpha(dim, 70));
    paintPads(c, t);
    paintDisplay(c, t);
    paintPattern(c, t);
}

void StutterFace::paintPads(SkCanvas& c, const Theme& t)
{
    const int selected = (int) std::lround(processor_->getParameter(S::ModeParam));
    const bool hold = processor_->getParameter(S::Hold) > 0.5f;
    const float size = t.metrics.smallFontSize;
    for (int i = 0; i < S::kModes; ++i)
    {
        const auto r = padArea(i);
        const bool playing = view_.engaged && view_.mode == i;
        const SkColor colour = kModeColour[i];
        paint::fillRounded(c, r, 4, playing ? colour : paint::mix(kScreen, colour, i == selected ? 0.28f : 0.10f));
        if (i == selected) paint::stroke(c, r.reduced(0.5f, 0.5f), colour, 1.2f);
        paint::text(c, S::kModeNames[i], r, playing ? kScreen : paint::withAlpha(kInk, i == selected ? 255 : 170), size + 1,
                    Align::Centre, true);
        static constexpr const char* kKeys[S::kModes] = {"C1", "D1", "E1", "F1", "G1"};
        paint::text(c, kKeys[i], {r.x + 4, r.bottom() - 12, r.w - 8, 11}, paint::withAlpha(playing ? kScreen : kScale, 200),
                    size - 2, Align::Right);
    }
    const auto latch = padArea(S::kModes);
    const SkColor accent = selected >= 0 && selected < S::kModes ? kModeColour[selected] : kInk;
    paint::fillRounded(c, latch, 4, hold ? paint::mix(kScreen, accent, 0.55f) : kScreen);
    paint::stroke(c, latch.reduced(0.5f, 0.5f), paint::withAlpha(accent, hold ? 255 : 90), 1.2f);
    paint::text(c, hold ? "LATCHED" : "Latch", latch, hold ? kInk : paint::withAlpha(kInk, 170), size + 1, Align::Centre, true);
}

void StutterFace::paintDisplay(SkCanvas& c, const Theme& t)
{
    const auto r = displayArea();
    paint::fillRounded(c, r, 3, kScreen);
    c.save();
    c.clipRect(r.sk());
    const float size = t.metrics.smallFontSize - 1;
    const SkColor colour = kModeColour[std::clamp(view_.mode, 0, S::kModes - 1)];
    const float mid = r.centreY(), half = r.h * 0.42f;
    paint::line(c, r.x, mid, r.right(), mid, kGrid);
    for (int q = 1; q < 4; ++q) paint::line(c, r.x + r.w * (float) q / 4, r.y, r.x + r.w * (float) q / 4, r.bottom(), paint::withAlpha(kGrid, 150));
    if (view_.bins > 0)
    {
        float peak = 1e-4f;
        for (int b = 0; b < view_.bins; ++b) peak = std::max({peak, -view_.min[(size_t) b], view_.max[(size_t) b]});
        const float scale = half / std::max(peak, 0.05f);
        const float binWidth = r.w / (float) view_.bins;
        SkPaint pen;
        pen.setAntiAlias(true);
        pen.setColor(view_.engaged ? colour : paint::withAlpha(colour, 90));
        for (int b = 0; b < view_.bins; ++b)
        {
            const float x = r.x + ((float) b + 0.5f) * binWidth;
            c.drawRect(SkRect::MakeLTRB(x - binWidth * 0.42f, mid - view_.max[(size_t) b] * scale, x + binWidth * 0.42f,
                                        mid - view_.min[(size_t) b] * scale + 1.0f),
                       pen);
        }
    }
    if (view_.engaged)
    {
        const float x = r.x + r.w * std::clamp(view_.position, 0.0f, 1.0f);
        paint::line(c, x, r.y, x, r.bottom(), kInk, 1.5f);
        char status[64];
        if (view_.mode == S::Repeat || view_.mode == S::Reverse)
            std::snprintf(status, sizeof status, "%s  x%d", S::kModeNames[view_.mode], view_.repeat + 1);
        else std::snprintf(status, sizeof status, "%s", S::kModeNames[std::clamp(view_.mode, 0, S::kModes - 1)]);
        paint::text(c, status, {r.x + 6, r.y + 3, r.w - 12, 13}, colour, size + 1, Align::Left, true);
    }
    else
        paint::text(c, view_.bins > 0 ? "last slice" : "hold a pad, play C1-G1 (C2+ pitched) or turn Pattern on",
                    {r.x + 6, r.y + 3, r.w - 12, 13}, kScale, size, Align::Left);
    c.restore();
}

void StutterFace::paintPattern(SkCanvas& c, const Theme& t)
{
    const bool on = processor_->getParameter(S::PatternOn) > 0.5f;
    const float size = t.metrics.smallFontSize - 2;
    for (int s = 0; s < S::kSteps; ++s)
    {
        const auto r = stepArea(s);
        const int v = (int) std::lround(processor_->getParameter(S::stepParam(s)));
        const bool current = on && view_.step == s;
        SkColor fill = s % 4 == 0 ? 0xff20252b : 0xff1a1e23;
        if (v > 0) fill = paint::withAlpha(kModeColour[v - 1], on ? (current ? 255 : 200) : 90);
        paint::fillRounded(c, r, 2, fill);
        if (current) paint::stroke(c, r.reduced(0.5f, 0.5f), kInk, 1.4f);
        if (v > 0 && r.w > 18)
        {
            static constexpr const char* kShort[S::kModes] = {"RPT", "REV", "FRZ", "STP", "GTE"};
            paint::text(c, kShort[v - 1], r, kScreen, size, Align::Centre, true);
        }
    }
}
} // namespace sonora::ui
