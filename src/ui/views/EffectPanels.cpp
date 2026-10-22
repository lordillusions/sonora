#include "ui/views/EffectPanels.h"

#include "devices/native/Delay.h"
#include "devices/native/Reverb.h"
#include "ui/Controls.h"
#include "ui/UiContext.h"
#include "ui/widgets/Knob.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPathBuilder.h"

#include <juce_gui_basics/juce_gui_basics.h>
#include <cmath>
#include <cstdio>

namespace sonora::ui
{
namespace
{
using D = devices::Delay;
using R = devices::Reverb;
using Processor = devices::DeviceProcessor;

std::string number(float v, const char* format)
{
    char buffer[48];
    std::snprintf(buffer, sizeof(buffer), format, (double) v);
    return buffer;
}
std::string parameterText(Processor* p, int index)
{
    const auto& info = p->parameters()[(size_t) index];
    const float v = p->getParameter(index);
    if (! info.choices.empty()) return info.choices[(size_t) std::clamp((int) std::lround(v), 0, (int) info.choices.size() - 1)];
    if (dynamic_cast<R*>(p))
    {
        if (index == R::Diffusion || index == R::Scale) return number(v * 100, "%.0f %%");
        if (index == R::Shape) return number(v, "%.2f");
    }
    if (info.unit == "Hz") return v >= 1000 ? number(v / 1000, "%.2f kHz") : number(v, v < 10 ? "%.2f Hz" : "%.0f Hz");
    if (info.unit == "ms") return v >= 1000 ? number(v / 1000, "%.2f s") : number(v, "%.2f ms");
    if (info.unit == "s") return number(v, "%.2f s");
    if (info.unit == "%") return number(v * 100, "%.1f %%");
    if (info.unit == "pct") return number(v, "%.0f %%");
    if (info.unit == "lin") return v <= 0 ? "-inf dB" : number(20 * std::log10(v), "%.1f dB");
    return number(v, "%.2f");
}
void set(Processor* p, int index, float value)
{
    const auto& info = p->parameters()[(size_t) index];
    value = std::clamp(value, info.minValue, info.maxValue);
    if (! info.choices.empty()) value = std::round(value);
    if (auto* delay = dynamic_cast<D*>(p)) delay->setParameterLinked(index, value);
    else p->setParameter(index, value);
}
void choose(Widget* owner, const std::vector<std::string>& labels, int current, std::function<void(int)> pick)
{
    std::vector<bool> ticked(labels.size());
    if (current >= 0 && (size_t) current < ticked.size()) ticked[(size_t) current] = true;
    // An async native menu may outlive a device-chain rebuild.
    auto safePick = [owner, onPick = std::move(pick)](int index) {
        if (Widget::isAlive(owner) && index >= 0) { onPick(index); owner->repaint(); }
    };
    if (owner->ctx().showChoices) owner->ctx().showChoices(labels, ticked, std::move(safePick));
    else
    {
        juce::PopupMenu menu;
        for (size_t i = 0; i < labels.size(); ++i) menu.addItem((int) i + 1, labels[i], true, ticked[i]);
        menu.showMenuAsync(juce::PopupMenu::Options(), [onResult = std::move(safePick)](int result) { if (result > 0) onResult(result - 1); });
    }
}
// Value readouts are draggable controls, including the numbers inside the dark displays.
class Value : public Widget
{
public:
    Value(Processor* p, int index, std::string label, bool display, int choice)
        : p_(p), index_(index), label_(std::move(label)), display_(display), choice_(choice) {}
    void paint(SkCanvas& c, const Theme& t) override
    {
        const auto& info = p_->parameters()[(size_t) index_];
        const bool button = info.isToggle || choice_ >= 0;
        const bool on = info.isToggle ? p_->getParameter(index_) > 0.5f : choice_ >= 0 && (int) std::lround(p_->getParameter(index_)) == choice_;
        const bool cyan = dynamic_cast<R*>(p_) ? index_ == R::SpinOn || index_ == R::ChorusOn : index_ == D::FilterOn;
        const SkColor accent = t.color(cyan ? "RangeDefault" : "ChosenDefault");
        const auto text = button ? label_ : label_ + parameterText(p_, index_);
        if (dynamic_cast<R*>(p_) && index_ == R::HighFilterType)
        {
            ctl::framed(c, t, localBounds(), t.color("ChosenDefault"));
            const float mid = height() * 0.5f;
            paint::line(c, 3, mid, width() * 0.45f, mid, t.color("ControlForeground"));
            const float endY = p_->getParameter(index_) > 0.5f ? height() - 3 : 3;
            paint::line(c, width() * 0.45f, mid, width() * 0.65f, endY, t.color("ControlForeground"));
            paint::line(c, width() * 0.65f, endY, width() - 3, endY, t.color("ControlForeground"));
            return;
        }
        if (button) ctl::toggle(c, t, localBounds(), text, on, accent);
        else if (! display_) ctl::valueBox(c, t, localBounds(), text);
        else paint::text(c, text, localBounds(), t.color(dynamic_cast<D*>(p_) || index_ == R::ShelfHiFreq || index_ == R::ShelfHiGain
                                                                 || index_ == R::Diffusion || index_ == R::Scale || index_ == R::BandFreq || index_ == R::BandWidth
                                                                 ? "ChosenDefault" : "RangeDefault"), t.metrics.smallFontSize, Align::Centre);
    }
    void mouseDown(const MouseEvent& e) override { startY_ = e.y; start_ = p_->getParameter(index_); }
    void mouseDrag(const MouseEvent& e) override
    {
        const auto& info = p_->parameters()[(size_t) index_];
        if (info.isToggle || choice_ >= 0 || ! info.choices.empty()) return;
        const float delta = (startY_ - e.y) / (e.shift ? 1500.0f : 200.0f);
        if (info.unit == "Hz" && info.minValue > 0) set(p_, index_, start_ * std::pow(info.maxValue / info.minValue, delta));
        else set(p_, index_, start_ + delta * (info.maxValue - info.minValue));
        repaint();
    }
    void mouseUp(const MouseEvent& e) override
    {
        if (! localBounds().contains(e.x, e.y) || std::abs(startY_ - e.y) > 3) return;
        const auto& info = p_->parameters()[(size_t) index_];
        if (info.isToggle) set(p_, index_, p_->getParameter(index_) > 0.5f ? 0.0f : 1.0f);
        else if (choice_ >= 0) set(p_, index_, (float) choice_);
        else if (! info.choices.empty()) choose(this, info.choices, (int) std::lround(p_->getParameter(index_)), [this](int i) { set(p_, index_, (float) i); });
        repaint();
    }
    void mouseDoubleClick(const MouseEvent&) override { set(p_, index_, p_->parameters()[(size_t) index_].defaultValue); repaint(); }
private:
    Processor* p_;
    int index_;
    std::string label_;
    bool display_;
    int choice_;
    float start_ = 0, startY_ = 0;
};

class Graph : public Widget
{
public:
    enum Kind { DelayFilter, InputFilter, Reflections, Diffusion };
    Graph(Processor* p, Kind kind) : p_(p), kind_(kind) {}
    void paint(SkCanvas& c, const Theme& t) override
    {
        const auto r = localBounds();
        paint::fill(c, r, t.color("RetroDisplayBackground"));
        const auto grid = paint::withAlpha(t.color("RetroDisplayScaleText"), 75);
        for (float f : {20.0f, 50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f})
            paint::line(c, freqX(f), 0, freqX(f), height(), grid, 0.5f);
        for (int i = 1; i < 5; ++i) paint::line(c, 0, height() * (float) i / 5, width(), height() * (float) i / 5, grid, 0.5f);
        SkPathBuilder path;
        const float centre = kind_ == DelayFilter ? p_->getParameter(D::FilterFrequency) : kind_ == InputFilter ? p_->getParameter(R::BandFreq) : p_->getParameter(R::ShelfHiFreq);
        const float bandwidth = kind_ == DelayFilter ? p_->getParameter(D::FilterBandwidth) : kind_ == InputFilter ? p_->getParameter(R::BandWidth) : 1.0f;
        for (float x = 0; x <= width(); x += 1)
        {
            const float f = xFreq(x);
            float gain = 1;
            if (kind_ == DelayFilter || kind_ == InputFilter)
            {
                const float lo = centre / std::pow(2.0f, bandwidth * 0.5f), hi = centre * std::pow(2.0f, bandwidth * 0.5f);
                // Reverb (measured, DEV-22b): BandHighOn switches the low cut, BandLowOn the high cut.
                const bool low = kind_ == DelayFilter ? p_->getParameter(D::FilterOn) > 0.5f : p_->getParameter(R::BandHighOn) > 0.5f;
                const bool high = kind_ == DelayFilter ? low : p_->getParameter(R::BandLowOn) > 0.5f;
                if (low) gain *= 1 / std::sqrt(1 + std::pow(lo / f, 4.0f));
                if (high) gain *= 1 / std::sqrt(1 + std::pow(f / hi, 4.0f));
            }
            else if (kind_ == Diffusion)
            {
                if (p_->getParameter(R::ShelfHighOn) > 0.5f)
                {
                    if (p_->getParameter(R::HighFilterType) > 0.5f) gain *= 1 / std::sqrt(1 + std::pow(f / centre, 4.0f));
                    else gain *= 1 - (1 - p_->getParameter(R::ShelfHiGain)) / (1 + std::pow(centre / f, 4.0f));
                }
                if (p_->getParameter(R::ShelfLowOn) > 0.5f) gain *= 1 - (1 - p_->getParameter(R::ShelfLoGain)) / (1 + std::pow(f / p_->getParameter(R::ShelfLoFreq), 4.0f));
            }
            else gain = std::pow(x / std::max(1.0f, width()), 0.4f + p_->getParameter(R::Shape) * 2.0f) * std::exp(-5 * x / std::max(1.0f, width()));
            const float y = height() * (0.9f - 0.6f * gain);
            if (x < 0.5f) path.moveTo(x, y); else path.lineTo(x, y);
        }
        SkPaint pen;
        pen.setAntiAlias(true); pen.setStyle(SkPaint::kStroke_Style); pen.setStrokeWidth(1.2f); pen.setColor(t.color("RangeDefault"));
        c.drawPath(path.detach(), pen);
        if (kind_ != Reflections)
        {
            const float y = kind_ == Diffusion ? height() * (1 - p_->getParameter(R::ShelfHiGain))
                                               : height() * (1 - (bandwidth - 0.5f) / 8.5f);
            pen.setColor(t.color("ChosenDefault"));
            c.drawCircle(freqX(centre), y, 5.5f, pen);
            if (kind_ == Diffusion)
            {
                paint::text(c, "2", {freqX(centre) - 4, y - 5, 8, 10}, pen.getColor(), 8, Align::Centre);
                pen.setColor(t.color("TextDisabled"));
                const float lowX = freqX(p_->getParameter(R::ShelfLoFreq)), lowY = height() * (1 - p_->getParameter(R::ShelfLoGain));
                c.drawCircle(lowX, lowY, 5, pen);
                paint::text(c, "1", {lowX - 4, lowY - 5, 8, 10}, pen.getColor(), 8, Align::Centre);
            }
        }
    }
    void mouseDown(const MouseEvent& e) override { mouseDrag(e); }
    void mouseDrag(const MouseEvent& e) override
    {
        const float f = xFreq(std::clamp(e.x, 0.0f, width()));
        const float amount = std::clamp(1 - e.y / std::max(1.0f, height()), 0.0f, 1.0f);
        if (kind_ == DelayFilter) { set(p_, D::FilterFrequency, f); set(p_, D::FilterBandwidth, 0.5f + amount * 8.5f); }
        else if (kind_ == InputFilter) { set(p_, R::BandFreq, f); set(p_, R::BandWidth, 0.5f + amount * 8.5f); }
        else if (kind_ == Diffusion)
        {
            if (e.shift) { set(p_, R::ShelfLoFreq, f); set(p_, R::ShelfLoGain, amount); }
            else { set(p_, R::ShelfHiFreq, f); set(p_, R::ShelfHiGain, amount); }
        }
        else set(p_, R::Shape, amount);
        repaint();
    }
private:
    float freqX(float f) const { return std::clamp(std::log(f / 20) / std::log(1000.0f), 0.0f, 1.0f) * width(); }
    float xFreq(float x) const { return 20 * std::pow(1000.0f, x / std::max(1.0f, width())); }
    Processor* p_;
    Kind kind_;
};
} // namespace

EffectPanel::EffectPanel(Processor* processor, int track, int device, bool nested)
    : processor_(processor), track_(track), device_(device), nested_(nested), delay_(dynamic_cast<D*>(processor) != nullptr)
{
    if (delay_) delayControls(); else reverbControls();
}
float EffectPanel::preferredWidth() const { return delay_ ? 392.0f : 500.0f; }
Widget* EffectPanel::control(int param, std::string label, Rect rect, bool knob, bool display, int choice)
{
    Widget* w;
    if (knob)
    {
        const auto& info = processor_->parameters()[(size_t) param];
        Knob::Source s;
        s.get = [this, param] { return processor_->getParameter(param); };
        s.set = [this, param](float v) { set(processor_, param, v); };
        s.minValue = info.minValue; s.maxValue = info.maxValue; s.defaultValue = info.defaultValue;
        s.logarithmic = info.unit == "Hz"; s.bipolar = info.minValue < 0;
        s.format = [this, param](float) {
            if (! delay_ && (param == R::RoomSize || param == R::StereoSeparation)) return number(processor_->getParameter(param), "%.2f");
            return parameterText(processor_, param);
        };
        w = add<Knob>(std::move(label), std::move(s));
    }
    else w = add<Value>(processor_, param, std::move(label), display, choice);
    if (! nested_) w->midiTarget = midi::ControlTarget{midi::ControlTarget::Kind::DeviceParameter, track_, device_, 0, processor_->parameters()[(size_t) param].id};
    items_.push_back({w, rect});
    return w;
}
void EffectPanel::delayControls()
{
    for (int side = 0; side < 2; ++side)
    {
        const float x = side == 0 ? 4.0f : 73.0f;
        control(side == 0 ? D::SyncL : D::SyncR, "Sync", {x, 14, 45, 13});
        constexpr int beats[] = {1, 2, 3, 4, 5, 6, 8, 16};
        for (int i = 0; i < 8; ++i) control(side == 0 ? D::SyncedL : D::SyncedR, std::to_string(beats[i]), {x + (float) (i % 2) * 24, 30 + (float) (i / 2) * 15, 21, 12}, false, false, beats[i] - 1);
        control(side == 0 ? D::TimeL : D::TimeR, "", {x, 30, 45, 57});
        control(side == 0 ? D::OffsetL : D::OffsetR, "", {x, 91, 45, 13});
    }
    control(D::Link, "↔", {54, 53, 15, 12});
    control(D::Feedback, "Feedback", {35, 114, 55, 48}, true);
    control(D::Freeze, "*", {90, 137, 13, 13});
    auto* graph = add<Graph>(processor_, Graph::DelayFilter);
    items_.push_back({graph, {125, 1, 200, 119}});
    control(D::FilterOn, "Filter", {128, 125, 37, 13});
    control(D::FilterFrequency, "Freq ", {167, 125, 83, 13}, false, true);
    control(D::FilterBandwidth, "Width ", {251, 125, 72, 13}, false, true);
    control(D::ModTimeMode, "LFO ", {128, 145, 48, 13}, false, true);
    control(D::ModAmountTime, "Delay ", {178, 145, 74, 13}, false, true);
    control(D::ModAmountFilter, "Filter ", {252, 145, 71, 13}, false, true);
    for (int i = 0; i < 3; ++i) control(D::SmoothingMode, i == 0 ? "Repitch" : i == 1 ? "Fade" : "Jump", {332, 14 + (float) i * 16, 54, 13}, false, false, i);
    control(D::PingPong, "Ping Pong", {332, 91, 54, 13});
    control(D::DryWet, "Dry/Wet", {332, 114, 54, 48}, true);
}
void EffectPanel::reverbControls()
{
    control(R::BandHighOn, "Lo Cut", {4, 16, 43, 13}); // measured: BandHighOn = high-pass (low cut)
    control(R::BandLowOn, "Hi Cut", {50, 16, 43, 13});  // BandLowOn = low-pass (high cut)
    items_.push_back({add<Graph>(processor_, Graph::InputFilter), {4, 34, 89, 54}});
    control(R::BandFreq, "", {3, 91, 46, 13}, false, true);
    control(R::BandWidth, "", {49, 91, 44, 13}, false, true);
    control(R::SpinOn, "Spin", {122, 27, 40, 13});
    control(R::SpinAmount, "", {104, 59, 43, 13}, false, true);
    control(R::SpinRate, "", {149, 59, 44, 13}, false, true);
    items_.push_back({add<Graph>(processor_, Graph::Reflections), {104, 77, 89, 13}});
    control(R::Shape, "Shape ", {104, 91, 89, 13}, false, true);
    control(R::ShelfHighOn, "High", {202, 16, 29, 13});
    control(R::HighFilterType, "", {233, 16, 23, 13});
    control(R::ShelfHiFreq, "", {257, 16, 52, 13}, false, true);
    control(R::ShelfHiGain, "", {312, 16, 30, 13}, false, true);
    items_.push_back({add<Graph>(processor_, Graph::Diffusion), {202, 34, 130, 54}});
    control(R::ShelfLowOn, "Low", {202, 91, 29, 13});
    control(R::ShelfLoFreq, "", {239, 91, 53, 13}, false, true);
    control(R::ShelfLoGain, "", {298, 91, 34, 13}, false, true);
    control(R::Diffusion, "", {335, 43, 43, 13}, false, true);
    control(R::Scale, "", {335, 77, 43, 13}, false, true);
    control(R::ChorusOn, "Chorus", {387, 1, 43, 13});
    control(R::SizeModDepth, "", {386, 43, 44, 13}, false, true);
    control(R::SizeModFreq, "", {386, 77, 44, 13}, false, true);
    control(R::MixReflect, "Reflect", {440, 0, 54, 52}, true);
    control(R::MixDiffuse, "Diffuse", {440, 56, 54, 52}, true);
    control(R::MixDirect, "Dry/Wet", {440, 113, 54, 49}, true);
    control(R::PreDelay, "Predelay", {20, 114, 58, 48}, true);
    control(R::SizeSmoothing, "", {108, 138, 38, 13});
    control(R::RoomSize, "Size", {151, 114, 48, 48}, true);
    control(R::DecayTime, "Decay", {205, 114, 52, 48}, true);
    control(R::FreezeOn, "Freeze", {276, 124, 48, 13});
    control(R::FlatOn, "Flat", {276, 141, 22, 13});
    control(R::CutOn, "Cut", {301, 141, 23, 13});
    control(R::StereoSeparation, "Stereo", {342, 114, 51, 48}, true);
    control(R::Density, "", {402, 138, 35, 13});
}
void EffectPanel::layout()
{
    const float sx = width() / preferredWidth();
    // Displays take the extra height; the bottom controls retain their measured sizes.
    const float extra = height() - 166;
    for (auto& item : items_)
    {
        auto r = item.rect;
        r.x *= sx; r.w *= sx;
        if (r.y >= 114) r.y += extra;
        else if (delay_ && r.x >= 125 * sx && r.x < 326 * sx)
        {
            if (r.y < 10) r.h += extra;
            else r.y += extra;
        }
        else if (! delay_)
        {
            if (((int) r.y == 34) || ((int) r.y == 77 && (int) r.h == 13 && dynamic_cast<Graph*>(item.widget))) r.h += extra;
            else if (r.y >= 77 && r.x < 380 * sx) r.y += extra;
        }
        item.widget->setBounds(r);
        if (delay_)
            if (auto* v = dynamic_cast<Value*>(item.widget); v)
            {
                // The two timing selectors exchange their grid for a free-time value field.
                if (item.rect.x < 119 && item.rect.y >= 30 && item.rect.y <= 87)
                {
                    const bool sync = processor_->getParameter(item.rect.x < 60 ? D::SyncL : D::SyncR) > 0.5f;
                    item.widget->visible = (int) item.rect.h == 57 ? ! sync : sync;
                }
            }
    }
}
void EffectPanel::tick() { layout(); }
void EffectPanel::paint(SkCanvas& c, const Theme& t)
{
    paint::fill(c, localBounds(), t.color("SurfaceBackground"));
    const float sx = width() / preferredWidth(), extra = height() - 166;
    auto text = [&](std::string_view label, Rect r, bool dark = false) {
        r.x *= sx; r.w *= sx;
        if (r.y >= 114) r.y += extra;
        paint::text(c, label, r, t.color(dark ? "RetroDisplayScaleText" : "ControlForeground"), t.metrics.smallFontSize, Align::Centre);
    };
    if (delay_)
    {
        text("Left", {4, 0, 45, 12}); text("Right", {73, 0, 45, 12}); text("Smoothing", {328, 0, 62, 12});
        paint::fillRounded(c, {125 * sx, 1, 200 * sx, 161 + extra}, 2, t.color("RetroDisplayBackground"));
    }
    else
    {
        for (Rect r : {Rect{0, 0, 97, 108}, Rect{100, 0, 97, 108}, Rect{199, 0, 183, 108}, Rect{385, 0, 47, 108}})
        { r.x *= sx; r.w *= sx; r.h += extra; paint::fillRounded(c, r, 2, t.color("RetroDisplayBackground")); }
        text("Input Filter", {3, 1, 90, 12}, true); text("Early Reflections", {102, 1, 93, 12}, true); text("Diffusion Network", {201, 1, 177, 12}, true);
        text("Amount", {104, 47, 43, 12}, true); text("Rate", {149, 47, 44, 12}, true);
        text("Diffusion", {334, 31, 46, 12}, true); text("Scale", {334, 65, 46, 12}, true);
        text("Amount", {385, 31, 47, 12}, true); text("Rate", {385, 65, 47, 12}, true);
        text("Smooth", {105, 125, 43, 12}); text("Density", {398, 125, 42, 12});
        paint::line(c, 435 * sx, 0, 435 * sx, height(), t.color("ControlContrastFrame"));
    }
}
} // namespace sonora::ui
