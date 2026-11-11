#include "ui/views/SweepFace.h"

#include "ui/Painter.h"
#include "ui/widgets/Buttons.h"
#include "ui/widgets/Knob.h"

#include "include/core/SkCanvas.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPathBuilder.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace sonora::ui
{
namespace
{
using W = devices::Sweep;
constexpr float kGap = 6, kTitle = 15, kSourceWidth = 170, kKnobColumn = 78;
constexpr SkColor kScreen = 0xff15181c, kGrid = 0xff272c33, kInk = 0xffeef1f4;

std::string format(const devices::ParameterInfo& info, float v)
{
    char buffer[32];
    if (! info.choices.empty()) return info.choices[(size_t) std::clamp((int) std::lround(v), 0, (int) info.choices.size() - 1)];
    if (info.unit == "Hz")
    {
        if (v >= 1000) std::snprintf(buffer, sizeof buffer, "%.1f kHz", v / 1000);
        else std::snprintf(buffer, sizeof buffer, "%.0f Hz", v);
    }
    else if (info.unit == "dB") std::snprintf(buffer, sizeof buffer, "%.1f dB", v);
    else if (info.unit == "st") std::snprintf(buffer, sizeof buffer, "%.0f st", v);
    else if (info.unit == "%") std::snprintf(buffer, sizeof buffer, "%.0f %%", v * 100);
    else std::snprintf(buffer, sizeof buffer, "%+.2f", v);
    return buffer;
}

void stroke(SkCanvas& c, SkPathBuilder& path, SkColor colour, float width)
{
    SkPaint pen;
    pen.setAntiAlias(true);
    pen.setStyle(SkPaint::kStroke_Style);
    pen.setStrokeWidth(width);
    pen.setStrokeJoin(SkPaint::kRound_Join);
    pen.setColor(colour);
    c.drawPath(path.detach(), pen);
}
} // namespace

SweepFace::SweepFace(devices::Sweep* processor, std::function<void(Widget*, int)> tag)
    : processor_(processor), tag_(std::move(tag))
{
    static constexpr const char* kSources[3] = {"Noise", "Tone", "Sample"};
    for (int s = 0; s < 3; ++s) sources_[s] = choice(kSources[s], W::Source, s);
    directions_[0] = choice("Up", W::Direction, W::Up);
    directions_[1] = choice("Down", W::Direction, W::Down);
    directions_[0]->tooltip = "Riser: the filter opens, the pitch rises, the level swells in";
    directions_[1]->tooltip = "Downlifter: the filter closes, the pitch falls, the level fades out";
    length_ = knob(W::LengthParam);
    length_->tooltip = "Length: how long one sweep lasts, in time with the track (it ends exactly there)";
    load_ = add<TextButton>("Load Sample...", TextButton::Style::Plain);
    load_->tooltip = "Load a sample (a chord, a crash, a vocal...): Up plays it reversed with a tail, rising into its attack";
    load_->onClick = [this] { if (onLoadSample) onLoadSample(); };
    load_->isEnabled = [this] { return static_cast<bool>(onLoadSample); };
    low_ = knob(W::LowHz);
    high_ = knob(W::HighHz);
    resonance_ = knob(W::Resonance);
    pitch_ = knob(W::Pitch);
    curveKnob_ = knob(W::Curve);
    curveKnob_->tooltip = "Curve: left = most of the change early, right = most of it at the end";
    swell_ = knob(W::Swell);
    width_ = knob(W::Width);
    detune_ = knob(W::Detune);
    rhythm_ = knob(W::Rhythm);
    rhythm_->tooltip = "Rhythm: a gate that speeds up over the sweep";
    tail_ = knob(W::Tail);
    tail_->tooltip = "Tail: reverb added to the sample before it is reversed (Sample source)";
    output_ = knob(W::Output);
    reverse_ = add<TextButton>("Reverse", TextButton::Style::Toggle);
    reverse_->isOn = [this] { return processor_->getParameter(W::Reverse) > 0.5f; };
    reverse_->onClick = [this] {
        processor_->setParameter(W::Reverse, processor_->getParameter(W::Reverse) > 0.5f ? 0.0f : 1.0f);
        processor_->refreshSample();
        repaint();
    };
    reverse_->tooltip = "Reverse: an Up sweep plays the sample backwards (a reverse swell)";
    if (tag_) tag_(reverse_, W::Reverse);
}

TextButton* SweepFace::choice(const char* label, int parameter, int value)
{
    auto* b = add<TextButton>(label);
    b->isOn = [this, parameter, value] { return (int) std::lround(processor_->getParameter(parameter)) == value; };
    b->onClick = [this, parameter, value] {
        processor_->setParameter(parameter, (float) value);
        if (parameter == W::Direction) processor_->refreshSample(); // reversal follows the direction
        repaint();
    };
    if (tag_) tag_(b, parameter);
    return b;
}

Knob* SweepFace::knob(int parameter)
{
    const auto& info = processor_->parameters()[(size_t) parameter];
    const bool stepped = ! info.choices.empty() || info.unit == "st";
    Knob::Source source;
    source.get = [this, parameter] { return processor_->getParameter(parameter); };
    source.set = [this, parameter, stepped](float v) {
        processor_->setParameter(parameter, stepped ? std::round(v) : v);
        if (parameter == W::Tail) processor_->refreshSample();
    };
    source.minValue = info.minValue;
    source.maxValue = info.maxValue;
    source.defaultValue = info.defaultValue;
    source.bipolar = info.minValue < 0 && info.maxValue > 0 && info.unit != "dB";
    source.logarithmic = info.unit == "Hz";
    source.format = [info](float v) { return format(info, v); };
    auto* k = add<Knob>(info.name, std::move(source));
    k->style = Knob::Style::Compact;
    k->tooltip = info.name + ": double-click to reset; Shift-drag for fine adjustment.";
    if (tag_) tag_(k, parameter);
    return k;
}

void SweepFace::layout()
{
    const float w = width(), h = height();
    const float fixed = kSourceWidth + 2 * kKnobColumn + 2 * kKnobColumn + 1.2f * kKnobColumn + 4 * kGap;
    source_ = {0, kSourceWidth};
    curve_ = {source_.x + source_.w + kGap, std::max(220.0f, w - fixed)};
    filter_ = {curve_.x + curve_.w + kGap, 2 * kKnobColumn};
    motion_ = {filter_.x + filter_.w + kGap, 2 * kKnobColumn};
    sound_ = {motion_.x + motion_.w + kGap, 1.2f * kKnobColumn};

    const float third = (source_.w - 4) / 3.0f;
    for (int s = 0; s < 3; ++s) sources_[s]->setBounds({source_.x + (float) s * (third + 2), kTitle + 2, third, 18});
    const float half = (source_.w - 2) / 2.0f;
    for (int d = 0; d < 2; ++d) directions_[d]->setBounds({source_.x + (float) d * (half + 2), kTitle + 24, half, 18});
    length_->setBounds({source_.x + 2, kTitle + 48, source_.w - 4, 34});
    load_->setBounds({source_.x, h - 38, source_.w, 18});

    const float body = std::max(40.0f, h - kTitle);
    const float row = std::clamp(body / 3.0f, 26.0f, 48.0f);
    const auto place = [&](Widget* widget, const Column& col, int column, int index) {
        widget->setBounds({col.x + 2 + (float) column * kKnobColumn, kTitle + (float) index * row + 2, kKnobColumn - 4,
                           std::min(row - 4, 34.0f)});
    };
    place(low_, filter_, 0, 0);
    place(high_, filter_, 0, 1);
    place(resonance_, filter_, 0, 2);
    place(pitch_, filter_, 1, 0);
    place(curveKnob_, filter_, 1, 1);
    place(swell_, motion_, 0, 0);
    place(width_, motion_, 0, 1);
    place(rhythm_, motion_, 0, 2);
    place(detune_, motion_, 1, 0);
    place(tail_, sound_, 0, 0);
    place(output_, sound_, 0, 1);
    reverse_->setBounds({sound_.x + 4, kTitle + 2 * row + 8, kKnobColumn - 8, 16});
}

Rect SweepFace::curveArea() const { return {curve_.x, kTitle, curve_.w, std::max(20.0f, height() - kTitle - 2)}; }

void SweepFace::tick()
{
    const float p = processor_->progress();
    bool changed = std::abs(p - progress_) > 1e-4f;
    progress_ = p;
    if (processor_->sampleName() != sampleShown_) { sampleShown_ = processor_->sampleName(); changed = true; }
    if (changed) repaint();
}

void SweepFace::paint(SkCanvas& c, const Theme& t)
{
    const auto fg = t.color("ControlForeground"), dim = t.color("TextDisabled");
    const float size = t.metrics.smallFontSize;
    paint::text(c, "SWEEP", {curve_.x + 2, 0, curve_.w, kTitle - 2}, fg, size, Align::Left, true);
    char length[48];
    std::snprintf(length, sizeof length, "%.2f s at the track's tempo", processor_->lengthSeconds());
    paint::text(c, length, {curve_.x, 0, curve_.w - 2, kTitle - 2}, dim, size - 1, Align::Right);
    paint::text(c, "FILTER / PITCH", {filter_.x + 2, 0, filter_.w, kTitle - 2}, fg, size, Align::Left, true);
    paint::text(c, "MOTION", {motion_.x + 2, 0, motion_.w, kTitle - 2}, fg, size, Align::Left, true);
    paint::text(c, "SOUND", {sound_.x + 2, 0, sound_.w, kTitle - 2}, fg, size, Align::Left, true);
    paint::text(c, "SOURCE", {source_.x + 2, 0, source_.w, kTitle - 2}, fg, size, Align::Left, true);
    const bool sampleSource = std::lround(processor_->getParameter(W::Source)) == W::Sample;
    const std::string sample = processor_->hasSample() ? processor_->sampleName() : sampleSource ? "no sample loaded" : "";
    paint::text(c, sample, {source_.x + 2, height() - 18, source_.w - 4, 16}, processor_->hasSample() ? fg : dim, size - 1, Align::Left);

    // The sweep: cutoff (log scale), pitch and level over the progress, the gate's openings, the play head.
    const auto r = curveArea();
    paint::fillRounded(c, r, 3, kScreen);
    c.save();
    c.clipRect(r.sk());
    const auto inner = r.reduced(6, 14);
    for (int q = 1; q < 4; ++q)
        paint::line(c, inner.x + inner.w * (float) q / 4, r.y, inner.x + inner.w * (float) q / 4, r.bottom(), kGrid);
    const auto curve = [&](auto valueAt, SkColor colour, float widthPx) {
        SkPathBuilder path;
        constexpr int kPoints = 96;
        for (int i = 0; i <= kPoints; ++i)
        {
            const float p = (float) i / kPoints, x = inner.x + inner.w * p, y = inner.bottom() - inner.h * std::clamp(valueAt(p), 0.0f, 1.0f);
            if (i == 0) path.moveTo(x, y);
            else path.lineTo(x, y);
        }
        stroke(c, path, colour, widthPx);
    };
    const float maxPitch = std::max(1.0f, processor_->getParameter(W::Pitch));
    curve([&](float p) { return std::log(processor_->cutoffAt(p) / 20.0f) / std::log(1000.0f); }, kCutoff, 2.0f);
    curve([&](float p) { return 0.5f + 0.5f * processor_->pitchAt(p) / maxPitch; }, kPitch, 1.6f);
    curve([&](float p) { return processor_->levelAt(p); }, paint::withAlpha(kLevel, 170), 1.2f);
    if (processor_->gateRateAt(0) > 0) // gate openings along the bottom
    {
        const double beats = processor_->lengthSeconds() * 2.0; // only the shape matters here; any tempo works
        double phase = 0;
        constexpr int kSteps = 600;
        for (int i = 0; i < kSteps; ++i)
        {
            const float p = (float) i / kSteps;
            phase += processor_->gateRateAt(p) * beats / kSteps;
            if (phase - std::floor(phase) < 0.55)
                paint::fill(c, {inner.x + inner.w * p, r.bottom() - 5, inner.w / kSteps + 0.5f, 3}, paint::withAlpha(kInk, 110));
        }
    }
    if (progress_ >= 0)
    {
        const float x = inner.x + inner.w * progress_;
        paint::line(c, x, r.y, x, r.bottom(), kInk, 1.5f);
    }
    float lx = r.x + 6;
    for (const auto& [label, colour] : {std::pair<const char*, SkColor>{"filter", kCutoff}, {"pitch", kPitch}, {"level", kLevel}})
    {
        paint::fill(c, {lx, r.y + 7, 10, 2}, colour);
        paint::text(c, label, {lx + 13, r.y + 2, 60, 12}, colour, size - 1, Align::Left);
        lx += 62;
    }
    c.restore();
}
} // namespace sonora::ui
