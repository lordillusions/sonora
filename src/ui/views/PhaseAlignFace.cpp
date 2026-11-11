#include "ui/views/PhaseAlignFace.h"

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
using P = devices::PhaseAlign;
constexpr float kGap = 6, kTitle = 15, kDialWidth = 150, kKnobColumn = 78, kOutWidth = 96, kHistoryWidth = 150;
constexpr SkColor kScreen = 0xff15181c, kGrid = 0xff272c33, kScale = 0xff7d8590, kInk = 0xffeef1f4;
constexpr float kPi = 3.14159265f;

std::string format(const devices::ParameterInfo& info, float v)
{
    char buffer[32];
    if (! info.choices.empty()) return info.choices[(size_t) std::clamp((int) std::lround(v), 0, (int) info.choices.size() - 1)];
    if (info.unit == "Hz") std::snprintf(buffer, sizeof buffer, "%.0f Hz", v);
    else if (info.unit == "ms") std::snprintf(buffer, sizeof buffer, v < 10 ? "%.1f ms" : "%.0f ms", v);
    else if (info.unit == "dB") std::snprintf(buffer, sizeof buffer, "%.1f dB", v);
    else if (info.unit == "deg") std::snprintf(buffer, sizeof buffer, "%+.0f\xc2\xb0", v);
    else if (info.unit == "%") std::snprintf(buffer, sizeof buffer, "%.0f %%", v * 100);
    else std::snprintf(buffer, sizeof buffer, "%.2f", v);
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

PhaseAlignFace::PhaseAlignFace(devices::PhaseAlign* processor, std::function<void(Widget*, int)> tag)
    : processor_(processor), tag_(std::move(tag))
{
    angle_ = knob(P::Angle);
    speed_ = knob(P::Speed);
    crossover_ = knob(P::Crossover);
    threshold_ = knob(P::Threshold);
    window_ = knob(P::Window);
    coherence_ = knob(P::MinCoherence);
    output_ = knob(P::Output);
    angle_->tooltip = "Angle: the rotation in Manual mode (180 = polarity flipped)";
    speed_->tooltip = "Speed: how long a half turn takes when the angle changes (slower = smoother)";
    crossover_->tooltip = "Crossover: only the bass below this is rotated; set it just above where kick and bass overlap";
    window_->tooltip = "Window: how much of each kick hit is compared with the bass";
    coherence_->tooltip = "Min. Coherence: hits where kick and bass share less than this are ignored";
    static constexpr const char* kModes[3] = {"Auto", "Hold", "Manual"};
    static constexpr const char* kModeTips[3] = {"Auto: follows the kick, hit by hit", "Hold: keeps the angle learned so far",
                                                 "Manual: the Angle control"};
    for (int m = 0; m < 3; ++m)
    {
        auto* b = add<TextButton>(kModes[m]);
        b->isOn = [this, m] { return (int) std::lround(processor_->getParameter(P::ModeParam)) == m; };
        b->onClick = [this, m] { processor_->setParameter(P::ModeParam, (float) m); repaint(); };
        b->tooltip = kModeTips[m];
        if (tag_) tag_(b, P::ModeParam);
        modes_[m] = b;
    }
    static constexpr const char* kListen[3] = {"Bass", "Sum", "Orig."};
    static constexpr const char* kListenTips[3] = {"Listen to the aligned bass (normal)", "Listen to the aligned bass + the kick",
                                                   "Listen to the bass as it came in + the kick (A/B)"};
    for (int l = 0; l < 3; ++l)
    {
        auto* b = add<TextButton>(kListen[l]);
        b->isOn = [this, l] { return (int) std::lround(processor_->getParameter(P::Listen)) == l; };
        b->onClick = [this, l] { processor_->setParameter(P::Listen, (float) l); repaint(); };
        b->tooltip = kListenTips[l];
        if (tag_) tag_(b, P::Listen);
        listen_[l] = b;
    }
    sidechain_ = add<TextButton>("Sidechain", TextButton::Style::Plain);
    sidechain_->onClick = [this] { if (onChooseSidechain) onChooseSidechain(); };
}

Knob* PhaseAlignFace::knob(int parameter)
{
    const auto& info = processor_->parameters()[(size_t) parameter];
    Knob::Source source;
    source.get = [this, parameter] { return processor_->getParameter(parameter); };
    source.set = [this, parameter](float v) { processor_->setParameter(parameter, v); };
    source.minValue = info.minValue;
    source.maxValue = info.maxValue;
    source.defaultValue = info.defaultValue;
    source.bipolar = info.minValue < 0 && info.maxValue > 0 && info.unit != "dB";
    source.logarithmic = info.unit == "Hz" || info.unit == "ms";
    source.format = [info](float v) { return format(info, v); };
    auto* k = add<Knob>(info.name, std::move(source));
    k->style = Knob::Style::Compact;
    k->tooltip = info.name + ": double-click to reset; Shift-drag for fine adjustment.";
    if (tag_) tag_(k, parameter);
    return k;
}

void PhaseAlignFace::layout()
{
    const float w = width(), h = height();
    const float fixed = kDialWidth + kHistoryWidth + 2 * kKnobColumn + kOutWidth + 5 * kGap;
    const float hitWidth = std::max(200.0f, w - fixed);
    phase_ = {0, kDialWidth};
    hit_ = {phase_.x + phase_.w + kGap, hitWidth};
    history_ = {hit_.x + hit_.w + kGap, kHistoryWidth};
    align_ = {history_.x + history_.w + kGap, kKnobColumn};
    detect_ = {align_.x + align_.w + kGap, kKnobColumn};
    out_ = {detect_.x + detect_.w + kGap, kOutWidth};

    const float body = std::max(40.0f, h - kTitle);
    const float row = std::clamp(body / 3.0f, 26.0f, 48.0f);
    const auto place = [&](Widget* widget, const Column& col, int index) {
        widget->setBounds({col.x + 2, kTitle + (float) index * row + 2, col.w - 4, std::min(row - 4, 34.0f)});
    };
    place(angle_, align_, 0);
    place(speed_, align_, 1);
    place(crossover_, align_, 2);
    place(threshold_, detect_, 0);
    place(window_, detect_, 1);
    place(coherence_, detect_, 2);
    place(output_, out_, 0);
    const float third = (out_.w - 4) / 3.0f;
    for (int l = 0; l < 3; ++l) listen_[l]->setBounds({out_.x + 2 + (float) l * third, kTitle + row + 6, third - 2, 16});
    const float modeWidth = (phase_.w - 4) / 3.0f;
    for (int m = 0; m < 3; ++m) modes_[m]->setBounds({phase_.x + (float) m * (modeWidth + 2), h - 17, modeWidth, 16});
    sidechain_->setBounds({hit_.x + hit_.w - 170, 0, 170, kTitle - 2});
}

Rect PhaseAlignFace::dialArea() const { return {phase_.x, kTitle, phase_.w, std::max(20.0f, height() - kTitle - 21)}; }
Rect PhaseAlignFace::hitArea() const { return {hit_.x, kTitle, hit_.w, std::max(20.0f, height() - kTitle - 2)}; }
Rect PhaseAlignFace::historyArea() const { return {history_.x, kTitle, history_.w, std::max(20.0f, height() - kTitle - 2)}; }

void PhaseAlignFace::tick()
{
    bool changed = processor_->takeShape(shape_, shape_.serial);
    if (const auto hits = processor_->hitCount(); hits != hits_) { hits_ = hits; changed = true; }
    if (const float a = processor_->currentAngle(); std::abs(a - shownAngle_) > 0.05f) { shownAngle_ = a; changed = true; }
    const std::string source = sidechainName ? sidechainName() : std::string();
    std::string label = "Kick: " + source;
    if (source.empty()) // the app names the track; the VST3 only knows whether the host sends something
        label = onChooseSidechain ? "Choose the kick track..."
                : processor_->sidechainPresent() ? "Kick: host sidechain" : "Route the kick to the sidechain";
    if (label != shownSource_ || sidechain_->label() != label)
    {
        shownSource_ = label;
        sidechain_->setLabel(label);
        changed = true;
    }
    if (changed) repaint();
}

void PhaseAlignFace::paint(SkCanvas& c, const Theme& t)
{
    const auto fg = t.color("ControlForeground");
    const float size = t.metrics.smallFontSize;
    paint::text(c, "PHASE", {phase_.x + 2, 0, phase_.w, kTitle - 2}, fg, size, Align::Left, true);
    paint::text(c, "LAST HIT", {hit_.x + 2, 0, hit_.w, kTitle - 2}, fg, size, Align::Left, true);
    paint::text(c, "HITS", {history_.x + 2, 0, history_.w, kTitle - 2}, fg, size, Align::Left, true);
    paint::text(c, "ALIGN", {align_.x + 2, 0, align_.w, kTitle - 2}, fg, size, Align::Left, true);
    paint::text(c, "DETECT", {detect_.x + 2, 0, detect_.w, kTitle - 2}, fg, size, Align::Left, true);
    paint::text(c, "OUT", {out_.x + 2, 0, out_.w, kTitle - 2}, fg, size, Align::Left, true);
    paintDial(c, t);
    paintHit(c, t);
    paintHistory(c, t);
}

void PhaseAlignFace::paintDial(SkCanvas& c, const Theme& t)
{
    const auto r = dialArea();
    paint::fillRounded(c, r, 3, kScreen);
    const float cx = r.centreX(), cy = r.centreY() - 4, radius = std::max(10.0f, std::min(r.w, r.h - 22) * 0.5f - 12);
    SkPaint ring;
    ring.setAntiAlias(true);
    ring.setStyle(SkPaint::kStroke_Style);
    ring.setStrokeWidth(4);
    ring.setColor(kGrid);
    c.drawCircle(cx, cy, radius, ring);
    // Coherence of the last hit: how much of the ring is lit.
    const float coherence = std::clamp(processor_->lastCoherence(), 0.0f, 1.0f);
    if (hits_ > 0)
    {
        ring.setColor(paint::withAlpha(kAfter, 200));
        c.drawArc(SkRect::MakeLTRB(cx - radius, cy - radius, cx + radius, cy + radius), -90, 360 * coherence, false, ring);
    }
    // 0 at the top, clockwise; 180 = polarity flip at the bottom.
    const auto point = [&](float degrees, float length) {
        const float a = (degrees - 90.0f) * kPi / 180.0f;
        return std::pair<float, float>{cx + std::cos(a) * length, cy + std::sin(a) * length};
    };
    for (const float tick : {0.0f, 90.0f, 180.0f, -90.0f})
    {
        const auto [x0, y0] = point(tick, radius - 7);
        const auto [x1, y1] = point(tick, radius - 2);
        paint::line(c, x0, y0, x1, y1, kScale, 1.2f);
    }
    const auto [tx, ty] = point(processor_->targetAngle(), radius - 3);
    paint::fillRounded(c, {tx - 3, ty - 3, 6, 6}, 3, kKick);
    const auto [nx, ny] = point(shownAngle_, radius - 9);
    paint::line(c, cx, cy, nx, ny, kInk, 2.2f);
    char text[32];
    std::snprintf(text, sizeof text, "%+.0f\xc2\xb0", shownAngle_);
    paint::text(c, text, {r.x, r.bottom() - 19, r.w, 16}, kInk, t.metrics.fontSize + 1, Align::Centre, true);
    const int mode = (int) std::lround(processor_->getParameter(P::ModeParam));
    const char* state = mode == P::Manual ? "manual" : mode == P::Hold ? "holding" : processor_->sidechainPresent() ? "following the kick" : "waiting for the kick";
    paint::text(c, state, {r.x + 4, r.y + 2, r.w - 8, 12}, kScale, t.metrics.smallFontSize - 1, Align::Left);
}

void PhaseAlignFace::paintHit(SkCanvas& c, const Theme& t)
{
    const auto r = hitArea();
    paint::fillRounded(c, r, 3, kScreen);
    c.save();
    c.clipRect(r.sk());
    const float mid = r.centreY(), half = r.h * 0.40f;
    paint::line(c, r.x, mid, r.right(), mid, kGrid);
    const float size = t.metrics.smallFontSize - 1;
    if (shape_.bins > 1)
    {
        float peak = 1e-4f;
        for (int b = 0; b < shape_.bins; ++b)
            peak = std::max({peak, std::abs(shape_.kick[(size_t) b]), std::abs(shape_.before[(size_t) b]), std::abs(shape_.after[(size_t) b])});
        const auto curve = [&](const std::array<float, P::kShapeBins>& v, SkColor colour, float width) {
            SkPathBuilder path;
            for (int b = 0; b < shape_.bins; ++b)
            {
                const float x = r.x + 4 + (r.w - 8) * (float) b / (float) (shape_.bins - 1), y = mid - v[(size_t) b] / peak * half;
                if (b == 0) path.moveTo(x, y);
                else path.lineTo(x, y);
            }
            stroke(c, path, colour, width);
        };
        curve(shape_.before, paint::withAlpha(kBefore, 200), 1.2f);
        curve(shape_.kick, kKick, 1.6f);
        curve(shape_.after, kAfter, 1.8f);
        char gain[48];
        std::snprintf(gain, sizeof gain, "low end %+.1f dB", processor_->lastGainDb());
        paint::text(c, gain, {r.x + 6, r.bottom() - 15, r.w - 12, 13}, kInk, size + 1, Align::Right, true);
    }
    else
        paint::text(c, processor_->sidechainPresent() ? "play the kick and the bass together" : "route the kick track to the sidechain",
                    {r.x + 6, mid - 7, r.w - 12, 14}, kScale, size, Align::Centre);
    // Legend.
    float x = r.x + 6;
    for (const auto& [label, colour] : {std::pair<const char*, SkColor>{"kick", kKick}, {"bass before", kBefore}, {"bass after", kAfter}})
    {
        paint::fill(c, {x, r.y + 7, 10, 2}, colour);
        paint::text(c, label, {x + 13, r.y + 2, 80, 12}, colour, size, Align::Left);
        x += 82;
    }
    c.restore();
}

void PhaseAlignFace::paintHistory(SkCanvas& c, const Theme& t)
{
    const auto r = historyArea();
    paint::fillRounded(c, r, 3, kScreen);
    const auto yFor = [&](float degrees) { return r.y + 8 + (r.h - 16) * (0.5f - degrees / 360.0f); };
    for (const float d : {180.0f, 90.0f, 0.0f, -90.0f, -180.0f})
    {
        const float y = yFor(d);
        paint::line(c, r.x + 22, y, r.right() - 3, y, d == 0.0f ? paint::withAlpha(kScale, 120) : kGrid);
        char label[8];
        std::snprintf(label, sizeof label, "%.0f", d);
        paint::text(c, label, {r.x + 1, y - 5, 20, 10}, kScale, t.metrics.smallFontSize - 2, Align::Right);
    }
    const float step = (r.w - 30) / (float) P::kHistory;
    for (int i = 0; i < P::kHistory; ++i)
    {
        const auto hit = processor_->history(i);
        if (i >= (int) hits_) break;
        const float x = r.right() - 6 - (float) i * step, y = yFor(hit.angle);
        SkPaint dotPaint;
        dotPaint.setAntiAlias(true);
        dotPaint.setColor(paint::withAlpha(kAfter, (uint8_t) (60 + 195 * std::clamp(hit.coherence, 0.0f, 1.0f))));
        if (! hit.used)
        {
            dotPaint.setStyle(SkPaint::kStroke_Style);
            dotPaint.setStrokeWidth(1);
        }
        c.drawCircle(x, y, 2.6f, dotPaint);
    }
}
} // namespace sonora::ui
