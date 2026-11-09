#include "ui/views/KickstractFace.h"

#include "ui/Painter.h"
#include "ui/widgets/Buttons.h"
#include "ui/widgets/Knob.h"

#include "include/core/SkCanvas.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPathBuilder.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace sonora::ui
{
namespace
{
using K = devices::Kickstract;
constexpr float kGap = 6, kTitle = 15, kDetectWidth = 112, kBandWidth = 72, kFinishWidth = 96;
constexpr float kMinHz = 20, kMaxHz = 20000, kTopDb = 0, kFloorDb = -84;
// The displays keep their own dark palette in every theme: the colours carry the meaning (bands, result, input).
constexpr SkColor kScreen = 0xff15181c, kGrid = 0xff272c33, kScale = 0xff7d8590, kInk = 0xffeef1f4;
constexpr const char* kBandTitle[K::kBands] = {"LOW", "MID", "HIGH"};
constexpr float kAttackMs = (float) (K::kAttackSeconds * 1000.0); // the gate ramp ends at the hit

std::string format(const devices::ParameterInfo& info, float v)
{
    char buffer[32];
    if (! info.choices.empty()) return info.choices[(size_t) std::clamp((int) std::lround(v), 0, (int) info.choices.size() - 1)];
    if (info.unit == "Hz")
    {
        if (v >= 1000) std::snprintf(buffer, sizeof buffer, "%.2f kHz", v / 1000);
        else std::snprintf(buffer, sizeof buffer, "%.0f Hz", v);
    }
    else if (info.unit == "ms") std::snprintf(buffer, sizeof buffer, v < 10 ? "%.1f ms" : "%.0f ms", v);
    else if (info.unit == "dB") std::snprintf(buffer, sizeof buffer, "%.1f dB", v);
    else if (info.unit == "%") std::snprintf(buffer, sizeof buffer, "%.0f %%", v * 100);
    else std::snprintf(buffer, sizeof buffer, "%.2f", v);
    return buffer;
}

std::string hzText(float hz)
{
    char buffer[24];
    if (hz >= 1000) std::snprintf(buffer, sizeof buffer, hz >= 10000 ? "%.0fk" : "%.1fk", hz / 1000);
    else std::snprintf(buffer, sizeof buffer, "%.0f", hz);
    return buffer;
}

void strokePath(SkCanvas& c, SkPathBuilder& path, SkColor colour, float width)
{
    SkPaint pen;
    pen.setAntiAlias(true);
    pen.setStyle(SkPaint::kStroke_Style);
    pen.setStrokeWidth(width);
    pen.setStrokeJoin(SkPaint::kRound_Join);
    pen.setColor(colour);
    c.drawPath(path.detach(), pen);
}

void fillPath(SkCanvas& c, SkPathBuilder& path, SkColor colour)
{
    SkPaint brush;
    brush.setAntiAlias(true);
    brush.setColor(colour);
    c.drawPath(path.detach(), brush);
}

void dot(SkCanvas& c, float x, float y, float radius, SkColor fill, SkColor ring)
{
    SkPaint p;
    p.setAntiAlias(true);
    p.setColor(fill);
    c.drawCircle(x, y, radius, p);
    p.setStyle(SkPaint::kStroke_Style);
    p.setStrokeWidth(1.2f);
    p.setColor(ring);
    c.drawCircle(x, y, radius, p);
}
} // namespace

KickstractFace::KickstractFace(devices::Kickstract* processor, std::function<void(Widget*, int)> tag)
    : processor_(processor), tag_(std::move(tag))
{
    threshold_ = knob(K::Threshold);
    retrigger_ = knob(K::Retrigger);
    input_ = knob(K::Input);
    chop_ = toggle("Chop", K::Chop, (int) TextButton::Style::Toggle);
    chop_->tooltip = "Chop: cut each band short after every kick (off: the bands play through, filters only)";
    for (int b = 0; b < K::kBands; ++b)
    {
        bandOn_[b] = toggle(std::string(kBandTitle[b]), K::bandParam(b, K::BandOn), (int) TextButton::Style::Activator);
        bandOn_[b]->tooltip = std::string(kBandTitle[b]) + " band on/off";
        bandSolo_[b] = toggle("S", K::bandParam(b, K::BandSolo), (int) TextButton::Style::Solo);
        bandSolo_[b]->tooltip = "Solo this band";
        bandKnobs_[b][0] = knob(K::bandParam(b, K::BandLength));
        bandKnobs_[b][1] = knob(K::bandParam(b, K::BandFade));
        bandKnobs_[b][2] = knob(K::bandParam(b, K::BandGain));
    }
    punch_ = knob(K::Punch);
    drive_ = knob(K::Drive);
    output_ = knob(K::Output);
    static constexpr const char* kListen[3] = {"Kick", "Rem", "In"};
    static constexpr const char* kListenTip[3] = {"Listen to the extracted kick", "Listen to what the gates removed",
                                                  "Listen to the (delayed) input"};
    for (int i = 0; i < 3; ++i)
    {
        auto* b = add<TextButton>(kListen[i]);
        b->isOn = [this, i] { return (int) std::lround(processor_->getParameter(K::Listen)) == i; };
        b->onClick = [this, i] { setParameter(K::Listen, (float) i); };
        b->tooltip = kListenTip[i];
        if (tag_) tag_(b, K::Listen);
        listen_[i] = b;
    }
    timeline_ = add<TextButton>("Timeline", TextButton::Style::Plain);
    timeline_->tooltip = "Open the timeline: input, kick and what was removed, live; Freeze to study it";
    timeline_->onClick = [this] { if (onOpenTimeline) onOpenTimeline(); };
}

void KickstractFace::setParameter(int parameter, float value)
{
    processor_->setParameter(parameter, value);
    repaint();
}

Knob* KickstractFace::knob(int parameter, const std::string& label)
{
    const auto& info = processor_->parameters()[(size_t) parameter];
    Knob::Source source;
    source.get = [this, parameter] { return processor_->getParameter(parameter); };
    source.set = [this, parameter](float v) { processor_->setParameter(parameter, v); };
    source.minValue = info.minValue;
    source.maxValue = info.maxValue;
    source.defaultValue = info.defaultValue;
    source.bipolar = info.minValue < 0 && info.maxValue > 0;
    source.logarithmic = info.unit == "Hz" || info.unit == "ms";
    source.format = [info](float v) { return format(info, v); };
    auto* k = add<Knob>(label.empty() ? info.name : label, std::move(source));
    k->style = Knob::Style::Compact;
    k->tooltip = info.name + ": double-click to reset; Shift-drag for fine adjustment.";
    if (tag_) tag_(k, parameter);
    return k;
}

TextButton* KickstractFace::toggle(const std::string& label, int parameter, int style)
{
    auto* b = add<TextButton>(label, (TextButton::Style) style);
    b->isOn = [this, parameter] { return processor_->getParameter(parameter) > 0.5f; };
    b->onClick = [this, parameter] { setParameter(parameter, processor_->getParameter(parameter) > 0.5f ? 0.0f : 1.0f); };
    if (tag_) tag_(b, parameter);
    return b;
}

// ---------------------------------------------------------------------------------------------------------------------
// Layout

void KickstractFace::layout()
{
    const float w = width(), h = height();
    const float fixed = kDetectWidth + K::kBands * kBandWidth + kFinishWidth + 5 * kGap;
    const float displays = std::max(240.0f, w - fixed);
    detect_ = {0, kDetectWidth};
    spectrum_ = {detect_.x + detect_.w + kGap, displays * 0.5f - kGap * 0.5f};
    kickColumn_ = {spectrum_.x + spectrum_.w + kGap, displays * 0.5f - kGap * 0.5f};
    float x = kickColumn_.x + kickColumn_.w + kGap;
    for (auto& band : bands_) { band = {x, kBandWidth - 2}; x += kBandWidth; }
    finish_ = {x + kGap - 2, kFinishWidth};

    const float body = std::max(40.0f, h - kTitle);
    const float row = std::clamp(body / 4.0f, 26.0f, 44.0f);
    auto place = [&](Widget* widget, float cx, float cw, int index, float rowHeight) {
        widget->setBounds({cx, kTitle + (float) index * rowHeight + 2, cw, std::min(rowHeight - 4, 34.0f)});
    };
    // Detect: three knobs and Chop; the meter takes the column's right edge.
    place(threshold_, detect_.x + 2, detect_.w - 22, 0, row);
    place(retrigger_, detect_.x + 2, detect_.w - 22, 1, row);
    place(input_, detect_.x + 2, detect_.w - 22, 2, row);
    chop_->setBounds({detect_.x + 2, kTitle + 3 * row + 4, 52, 16});

    const float bandRow = std::clamp((body - 20) / 3.0f, 26.0f, 48.0f);
    for (int b = 0; b < K::kBands; ++b)
    {
        const auto& col = bands_[b];
        bandOn_[b]->setBounds({col.x, 0, col.w - 20, 15});
        bandSolo_[b]->setBounds({col.x + col.w - 18, 0, 18, 15});
        for (int k = 0; k < 3; ++k)
            bandKnobs_[b][k]->setBounds({col.x + 4, 20 + (float) k * bandRow, col.w - 6, std::min(bandRow - 4, 34.0f)});
    }
    const float finishRow = std::clamp((body - 22) / 3.0f, 26.0f, 44.0f);
    place(punch_, finish_.x + 2, finish_.w - 4, 0, finishRow);
    place(drive_, finish_.x + 2, finish_.w - 4, 1, finishRow);
    place(output_, finish_.x + 2, finish_.w - 4, 2, finishRow);
    const float listenY = std::min(h - 18, kTitle + 3 * finishRow + 4), third = (finish_.w - 4) / 3.0f;
    for (int i = 0; i < 3; ++i) listen_[i]->setBounds({finish_.x + 2 + (float) i * third, listenY, third - 2, 16});
    timeline_->visible = static_cast<bool>(onOpenTimeline);
    timeline_->setBounds({kickColumn_.x + kickColumn_.w - 64, 0, 64, kTitle - 2});
}

Rect KickstractFace::spectrumArea() const { return {spectrum_.x, kTitle, spectrum_.w, std::max(20.0f, height() - kTitle - 2)}; }
Rect KickstractFace::kickArea() const { return {kickColumn_.x, kTitle, kickColumn_.w, std::max(20.0f, height() - kTitle - 2)}; }

float KickstractFace::xForHz(float hz) const
{
    const auto r = spectrumArea();
    return r.x + r.w * std::log(std::clamp(hz, kMinHz, kMaxHz) / kMinHz) / std::log(kMaxHz / kMinHz);
}

float KickstractFace::hzForX(float x) const
{
    const auto r = spectrumArea();
    return kMinHz * std::pow(kMaxHz / kMinHz, std::clamp((x - r.x) / std::max(1.0f, r.w), 0.0f, 1.0f));
}

float KickstractFace::windowMs() const
{
    if (kick_.bins > 0 && kick_.windowMs > 0) return kick_.windowMs;
    float longest = 0;
    for (int b = 0; b < K::kBands; ++b)
        longest = std::max(longest, processor_->getParameter(K::bandParam(b, K::BandLength)) + processor_->getParameter(K::bandParam(b, K::BandFade)));
    return std::clamp(longest * 1.15f + 15.0f, 60.0f, 1500.0f);
}

float KickstractFace::xForMs(float ms) const
{
    const auto r = kickArea().reduced(4, 0);
    return r.x + r.w * std::clamp(ms / windowMs(), 0.0f, 1.0f);
}

// The gate's level `ms` after it opened (capture time 0): the ramp up to the onset, Length from the onset, Fade.
float KickstractFace::gateLevelAt(int band, float ms) const
{
    const float attack = kAttackMs;
    if (processor_->getParameter(K::Chop) < 0.5f) return 1.0f;
    const float holdEnd = attack + processor_->getParameter(K::bandParam(band, K::BandLength));
    const float fade = std::max(0.01f, processor_->getParameter(K::bandParam(band, K::BandFade)));
    if (ms < attack) return std::max(0.0f, ms / attack);
    if (ms <= holdEnd) return 1.0f;
    const float r = std::clamp(1.0f - (ms - holdEnd) / fade, 0.0f, 1.0f);
    return r * r;
}

int KickstractFace::handleAt(float x, float y) const
{
    const auto s = spectrumArea();
    if (s.contains(x, y))
    {
        const float low = xForHz(processor_->lowSplitHz()), high = xForHz(processor_->highSplitHz());
        const float dl = std::abs(x - low), dh = std::abs(x - high);
        if (std::min(dl, dh) <= 5) return dl <= dh ? 0 : 1;
        return -1;
    }
    const auto k = kickArea();
    if (! k.contains(x, y) || processor_->getParameter(K::Chop) < 0.5f) return -1;
    const float attack = kAttackMs;
    int best = -1;
    float bestDistance = 7;
    for (int b = 0; b < K::kBands; ++b)
    {
        const float holdEnd = attack + processor_->getParameter(K::bandParam(b, K::BandLength));
        const float fadeEnd = holdEnd + processor_->getParameter(K::bandParam(b, K::BandFade));
        const float levelY = k.y + 8 + (float) b * 7; // the three Length handles sit on staggered rows near the top
        const float hx = xForMs(holdEnd), fx = xForMs(fadeEnd), cy = k.centreY();
        if (const float d = std::hypot(x - hx, y - levelY); d < bestDistance) { bestDistance = d; best = 10 + b; }
        if (const float d = std::hypot(x - fx, y - cy); d < bestDistance) { bestDistance = d; best = 20 + b; }
    }
    return best;
}

// ---------------------------------------------------------------------------------------------------------------------
// Input

void KickstractFace::mouseDown(const MouseEvent& e)
{
    drag_ = handleAt(e.x, e.y);
    mouseDrag(e);
}

void KickstractFace::mouseDrag(const MouseEvent& e)
{
    if (drag_ < 0) return;
    const float attack = kAttackMs;
    const auto msAt = [&](float x) {
        const auto r = kickArea().reduced(4, 0);
        return std::clamp((x - r.x) / std::max(1.0f, r.w), 0.0f, 1.0f) * windowMs();
    };
    if (drag_ == 0) setParameter(K::LowSplit, hzForX(e.x));
    else if (drag_ == 1) setParameter(K::HighSplit, hzForX(e.x));
    else if (drag_ >= 10 && drag_ < 20)
    {
        const int b = drag_ - 10;
        setParameter(K::bandParam(b, K::BandLength), std::max(1.0f, msAt(e.x) - attack));
    }
    else if (drag_ >= 20)
    {
        const int b = drag_ - 20;
        const float holdEnd = attack + processor_->getParameter(K::bandParam(b, K::BandLength));
        setParameter(K::bandParam(b, K::BandFade), std::max(1.0f, msAt(e.x) - holdEnd));
    }
}

void KickstractFace::mouseUp(const MouseEvent&) { drag_ = -1; }

void KickstractFace::mouseMove(const MouseEvent& e)
{
    const int hover = handleAt(e.x, e.y);
    if (hover != hover_) { hover_ = hover; repaint(); }
}

void KickstractFace::mouseExit()
{
    if (hover_ != -1) { hover_ = -1; repaint(); }
}

void KickstractFace::mouseDoubleClick(const MouseEvent& e)
{
    const int handle = handleAt(e.x, e.y);
    int parameter = -1;
    if (handle == 0) parameter = K::LowSplit;
    else if (handle == 1) parameter = K::HighSplit;
    else if (handle >= 10 && handle < 20) parameter = K::bandParam(handle - 10, K::BandLength);
    else if (handle >= 20) parameter = K::bandParam(handle - 20, K::BandFade);
    if (parameter >= 0) setParameter(parameter, processor_->parameters()[(size_t) parameter].defaultValue);
}

Cursor KickstractFace::cursorAt(float x, float y) const
{
    return handleAt(x, y) >= 0 ? Cursor::ResizeHorizontal : Cursor::Default;
}

void KickstractFace::tick()
{
    const double now = juce::Time::getMillisecondCounterHiRes() / 1000.0;
    const double seconds = lastTick_ > 0 ? now - lastTick_ : 1.0 / 30.0;
    lastTick_ = now;
    bool changed = inSpectrum_.update(processor_->inputTap, seconds);
    changed |= outSpectrum_.update(processor_->outputTap, seconds);
    changed |= processor_->takeKick(kick_, kick_.serial);
    const float peak = processor_->takeDetectorPeak();
    const float meter = std::max(peak, meter_ * (float) std::pow(0.05, seconds));
    if (std::abs(meter - meter_) > 1e-5f) { meter_ = meter < 1e-5f ? 0 : meter; changed = true; }
    if (const auto triggers = processor_->triggerCount(); triggers != triggers_)
    {
        triggers_ = triggers;
        flashUntil_ = now + 0.12;
        changed = true;
    }
    for (int b = 0; b < K::kBands; ++b)
    {
        const float level = std::max(processor_->bandLevel(b), bandLevels_[b] * (float) std::pow(0.02, seconds));
        if (std::abs(level - bandLevels_[b]) > 1e-3f) { bandLevels_[b] = level; changed = true; }
    }
    if (changed || now < flashUntil_ + 0.05) repaint();
}

// ---------------------------------------------------------------------------------------------------------------------
// Painting

void KickstractFace::paint(SkCanvas& c, const Theme& t)
{
    paintColumnTitles(c, t);
    paintDetector(c, t);
    paintSpectrum(c, t);
    paintKick(c, t);
}

void KickstractFace::paintColumnTitles(SkCanvas& c, const Theme& t)
{
    const auto fg = t.color("ControlForeground"), dim = t.color("TextDisabled");
    const float size = t.metrics.smallFontSize;
    paint::text(c, "DETECT", {detect_.x + 2, 0, detect_.w, kTitle - 2}, fg, size, Align::Left, true);
    paint::text(c, "SPECTRUM", {spectrum_.x + 2, 0, spectrum_.w, kTitle - 2}, fg, size, Align::Left, true);
    paint::text(c, "drag the lines to move the bands", {spectrum_.x, 0, spectrum_.w - 2, kTitle - 2}, dim, size - 1, Align::Right);
    paint::text(c, "LAST KICK", {kickColumn_.x + 2, 0, kickColumn_.w, kTitle - 2}, fg, size, Align::Left, true);
    paint::text(c, "drag the dots: Length / Fade", {kickColumn_.x, 0, kickColumn_.w - (timeline_->visible ? 70.0f : 2.0f), kTitle - 2},
                dim, size - 1, Align::Right);
    paint::text(c, "FINISH", {finish_.x + 2, 0, finish_.w, kTitle - 2}, fg, size, Align::Left, true);
    // Band strips: a coloured bar under each band's On button, lit by its gate.
    for (int b = 0; b < K::kBands; ++b)
    {
        const auto& col = bands_[b];
        const bool on = processor_->getParameter(K::bandParam(b, K::BandOn)) > 0.5f;
        paint::fill(c, {col.x, 16, col.w, 2}, paint::withAlpha(kBandColour[b], on ? 110 : 40));
        paint::fill(c, {col.x, 16, col.w * std::clamp(bandLevels_[b], 0.0f, 1.0f), 2}, kBandColour[b]);
        if (b > 0) paint::line(c, col.x - 2, 2, col.x - 2, height() - 2, paint::withAlpha(dim, 70));
    }
}

void KickstractFace::paintDetector(SkCanvas& c, const Theme& t)
{
    // Vertical meter of the detector (low band) with the threshold line; the lamp lights on every detected hit.
    const Rect meter{detect_.x + detect_.w - 14, kTitle + 14, 10, std::max(20.0f, height() - kTitle - 18)};
    paint::fill(c, meter, kScreen);
    const auto yForDb = [&](float db) { return meter.y + meter.h * std::clamp(db / -60.0f, 0.0f, 1.0f); };
    const float level = 20.0f * std::log10(std::max(meter_, 1e-6f));
    const float top = yForDb(level);
    paint::fill(c, {meter.x + 1, top, meter.w - 2, meter.bottom() - top}, kBandColour[K::Low]);
    const float thresholdY = yForDb(processor_->getParameter(K::Threshold));
    paint::line(c, meter.x - 3, thresholdY, meter.right() + 1, thresholdY, kInk, 1.5f);
    const bool lit = juce::Time::getMillisecondCounterHiRes() / 1000.0 < flashUntil_;
    dot(c, meter.centreX(), kTitle + 5, 4, lit ? 0xff5cff7a : kScreen, paint::withAlpha(t.color("ControlForeground"), 120));
}

void KickstractFace::paintSpectrum(SkCanvas& c, const Theme& t)
{
    const auto r = spectrumArea();
    paint::fillRounded(c, r, 3, kScreen);
    c.save();
    c.clipRect(r.sk());
    const float size = t.metrics.smallFontSize - 1;
    const float lowX = xForHz(processor_->lowSplitHz()), highX = xForHz(processor_->highSplitHz());
    const float edges[K::kBands + 1] = {r.x, lowX, highX, r.right()};
    bool anySolo = false;
    for (int b = 0; b < K::kBands; ++b) anySolo |= processor_->getParameter(K::bandParam(b, K::BandSolo)) > 0.5f;
    for (int b = 0; b < K::kBands; ++b) // band regions, brighter while their gate is open
    {
        const bool on = processor_->getParameter(K::bandParam(b, K::BandOn)) > 0.5f
                        && (! anySolo || processor_->getParameter(K::bandParam(b, K::BandSolo)) > 0.5f);
        const auto alpha = (uint8_t) (on ? 12 + 48 * std::clamp(bandLevels_[b], 0.0f, 1.0f) : 6);
        paint::fill(c, {edges[b], r.y, edges[b + 1] - edges[b], r.h}, paint::withAlpha(kBandColour[b], alpha));
        paint::text(c, kBandTitle[b], {edges[b] + 3, r.y + 2, std::max(0.0f, edges[b + 1] - edges[b] - 6), 12},
                    paint::withAlpha(kBandColour[b], on ? 230 : 90), size, Align::Left, true);
    }
    for (const float hz : {50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f})
    {
        const float x = xForHz(hz);
        paint::line(c, x, r.y, x, r.bottom() - 11, kGrid);
        paint::text(c, hzText(hz), {x - 15, r.bottom() - 11, 30, 10}, kScale, size - 1, Align::Centre);
    }
    for (const float db : {-24.0f, -48.0f, -72.0f})
    {
        const float y = r.y + r.h * (kTopDb - db) / (kTopDb - kFloorDb);
        paint::line(c, r.x, y, r.right(), y, paint::withAlpha(kGrid, 160));
    }
    const auto yForDb = [&](float db) { return r.y + r.h * std::clamp((kTopDb - db) / (kTopDb - kFloorDb), 0.0f, 1.0f); };
    const auto curvePath = [&](const auto& curve, bool closed) {
        SkPathBuilder path;
        bool started = false;
        for (int i = 0; i < dsp::SpectrumAnalyzer::kCurvePoints; ++i)
        {
            const float x = xForHz(dsp::SpectrumAnalyzer::curveFrequency(i)), y = yForDb(curve[(size_t) i]);
            if (! started) { path.moveTo(x, closed ? r.bottom() : y); if (closed) path.lineTo(x, y); started = true; }
            else path.lineTo(x, y);
        }
        if (closed) { path.lineTo(r.right(), r.bottom()); path.close(); }
        return path;
    };
    auto input = curvePath(inSpectrum_.curve(), true);
    fillPath(c, input, 0x30c8d0da);
    auto inputLine = curvePath(inSpectrum_.curve(), false);
    strokePath(c, inputLine, 0x60c8d0da, 1.0f);
    for (int b = 0; b < K::kBands; ++b) // the result, in each band's colour
    {
        c.save();
        c.clipRect(SkRect::MakeLTRB(edges[b], r.y, edges[b + 1], r.bottom()));
        auto out = curvePath(outSpectrum_.curve(), true);
        fillPath(c, out, paint::withAlpha(kBandColour[b], 70));
        auto outLine = curvePath(outSpectrum_.curve(), false);
        strokePath(c, outLine, kBandColour[b], 1.6f);
        c.restore();
    }
    for (int s = 0; s < 2; ++s) // split lines and their frequency tags
    {
        const float x = s == 0 ? lowX : highX;
        const bool hot = hover_ == s || drag_ == s;
        paint::line(c, x, r.y, x, r.bottom(), paint::withAlpha(kInk, hot ? 255 : 170), hot ? 2.5f : 1.5f);
        const auto& info = processor_->parameters()[(size_t) (s == 0 ? K::LowSplit : K::HighSplit)];
        const std::string label = format(info, s == 0 ? processor_->lowSplitHz() : processor_->highSplitHz());
        const Rect tagRect{std::clamp(x - 26, r.x + 1, r.right() - 53), r.y + 15, 52, 12};
        paint::fillRounded(c, tagRect, 2, hot ? kInk : 0xd0272c33);
        paint::text(c, label, tagRect, hot ? kScreen : kInk, size, Align::Centre);
    }
    c.restore();
}

void KickstractFace::paintKick(SkCanvas& c, const Theme& t)
{
    const auto area = kickArea();
    paint::fillRounded(c, area, 3, kScreen);
    c.save();
    c.clipRect(area.sk());
    const auto r = area.reduced(4, 0);
    const float size = t.metrics.smallFontSize - 1;
    const float mid = r.centreY(), half = r.h * 0.5f - 12;
    const float window = windowMs();
    const float step = window > 600 ? 100.0f : window > 250 ? 50.0f : 20.0f;
    for (float ms = step; ms < window; ms += step) // time grid
    {
        const float x = xForMs(ms);
        paint::line(c, x, r.y, x, r.bottom() - 11, kGrid);
        paint::text(c, std::to_string((int) ms), {x - 16, r.bottom() - 11, 32, 10}, kScale, size - 1, Align::Centre);
    }
    paint::line(c, r.x, mid, r.right(), mid, kGrid);
    const float hitX = xForMs(kAttackMs);
    paint::line(c, hitX, r.y, hitX, r.bottom(), paint::withAlpha(kInk, 50));

    if (kick_.bins > 0)
    {
        float peak = 0.05f;
        for (int i = 0; i < kick_.bins; ++i)
            peak = std::max({peak, kick_.inPeak[(size_t) i], kick_.outMax[(size_t) i], -kick_.outMin[(size_t) i]});
        const float scale = half / peak;
        const float binWidth = r.w / (float) K::kCaptureBins;
        const auto mirrored = [&](const std::array<float, K::kCaptureBins>& values, SkColor colour) {
            SkPathBuilder path;
            path.moveTo(r.x, mid);
            for (int i = 0; i < kick_.bins; ++i) path.lineTo(r.x + ((float) i + 0.5f) * binWidth, mid - values[(size_t) i] * scale);
            for (int i = kick_.bins - 1; i >= 0; --i) path.lineTo(r.x + ((float) i + 0.5f) * binWidth, mid + values[(size_t) i] * scale);
            path.close();
            fillPath(c, path, colour);
        };
        mirrored(kick_.inPeak, 0x26c8d0da); // the input in that window (what the kick came out of)
        for (int b = 0; b < K::kBands; ++b) mirrored(kick_.band[(size_t) b], paint::withAlpha(kBandColour[b], 85));
        SkPaint wave; // the result's waveform: one min/max line per bin
        wave.setAntiAlias(true);
        wave.setStrokeWidth(std::max(1.0f, binWidth * 0.8f));
        wave.setColor(kInk);
        for (int i = 0; i < kick_.bins; ++i)
        {
            const float x = r.x + ((float) i + 0.5f) * binWidth;
            c.drawLine(x, mid - kick_.outMax[(size_t) i] * scale, x, mid - std::min(kick_.outMin[(size_t) i] * scale, kick_.outMax[(size_t) i] * scale - 0.6f), wave);
        }
    }
    else
        paint::text(c, "play something with a kick", {r.x, mid - 8, r.w, 16}, kScale, size + 1, Align::Centre);

    // Each band's gate envelope (where the chop happens), with its Length and Fade handles.
    const bool chop = processor_->getParameter(K::Chop) > 0.5f;
    const float attack = kAttackMs;
    for (int b = 0; b < K::kBands; ++b)
    {
        const bool on = processor_->getParameter(K::bandParam(b, K::BandOn)) > 0.5f;
        SkPathBuilder envelope;
        const int steps = 160;
        for (int i = 0; i <= steps; ++i)
        {
            const float ms = window * (float) i / (float) steps;
            const float y = mid - gateLevelAt(b, ms) * (half + 4) + (float) b * 2;
            if (i == 0) envelope.moveTo(xForMs(ms), y);
            else envelope.lineTo(xForMs(ms), y);
        }
        strokePath(c, envelope, paint::withAlpha(kBandColour[b], on ? 230 : 70), 1.4f);
        if (! chop) continue;
        const float holdEnd = attack + processor_->getParameter(K::bandParam(b, K::BandLength));
        const float fadeEnd = holdEnd + processor_->getParameter(K::bandParam(b, K::BandFade));
        const float hx = xForMs(holdEnd), fx = xForMs(fadeEnd), hy = area.y + 8 + (float) b * 7;
        paint::line(c, hx, hy, hx, mid - (half + 4) + (float) b * 2, paint::withAlpha(kBandColour[b], 90));
        const bool hotLength = hover_ == 10 + b || drag_ == 10 + b, hotFade = hover_ == 20 + b || drag_ == 20 + b;
        dot(c, hx, hy, hotLength ? 4.5f : 3.2f, kBandColour[b], kScreen);
        dot(c, fx, mid, hotFade ? 4.5f : 3.2f, kScreen, kBandColour[b]);
    }
    c.restore();
}
} // namespace sonora::ui
