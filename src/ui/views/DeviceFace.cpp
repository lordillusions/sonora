#include "ui/views/DeviceFace.h"

#include "devices/native/FmSynth.h"
#include "devices/native/Simpler.h"
#include "devices/native/VectorSynth.h"
#include "devices/native/Impulse.h"
#include "devices/native/Echo.h"
#include "devices/native/Redux.h"
#include "devices/native/SwissArmyMeter.h"
#include "devices/native/Utility.h"
#include "devices/native/Tuner.h"
#include "devices/native/ClassicEffects.h"
#include "engine/PeakCache.h"
#include "gfx/Fonts.h"
#include "ui/views/NativeEditors.h"
#include "ui/Controls.h"
#include "ui/UiContext.h"
#include "ui/widgets/Buttons.h"
#include "ui/widgets/Knob.h"
#include "include/core/SkPathBuilder.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <cstdio>

namespace sonora::ui
{
namespace
{
using P = devices::DeviceProcessor;
using F = devices::FmSynth;
using S = devices::Simpler;
using V = devices::VectorSynth;
using I = devices::Impulse;
using E = devices::Echo;
using R = devices::Redux;
using T = devices::Tuner;
using G = devices::Gate;
using AP = devices::AutoPan;
using SAM = devices::SwissArmyMeter;
using U = devices::Utility;

std::string number(float value, const char* format)
{
    char text[48]; std::snprintf(text, sizeof(text), format, (double) value); return text;
}
// Three significant digits, as the reference's sampler displays values ("0.00 ms", "50.0 ms", "600 ms", "22.0 kHz").
std::string significant(float v)
{
    const float a = std::abs(v);
    return number(v, a >= 99.95f ? "%.0f" : a >= 9.995f ? "%.1f" : "%.2f");
}
std::string formatted(P* p, int index)
{
    const auto& info = p->parameters()[(size_t) index];
    const float v = p->getParameter(index);
    if (! info.choices.empty()) return info.choices[(size_t) std::clamp((int) std::lround(v), 0, (int) info.choices.size() - 1)];
    if (dynamic_cast<S*>(p) && index == S::ConstPowerFade) return v > 0.5f ? "Power" : "Linear";
    if (dynamic_cast<S*>(p) && ! info.isToggle)
    {
        // Per-control precision as the reference's Controls view shows it ("Vel 0.0 %", "Vel>Vol 35 %", "Amount 0").
        switch (index)
        {
            case S::FilterVelMod: case S::FilterKeyTrack: case S::PanRandom: case S::LfoKeyScale: case S::FilterSustain: case S::PitchSustain:
            case S::VolKeyScale: case S::PanKeyScale:
                return number(v * 100, v >= 0.9995f ? "%.0f %%" : "%.1f %%");
            case S::VelToVolume: return number(v * 100, "%.0f %%");
            case S::Spread: return number(v, "%.0f %%");
            case S::FilterEnvAmount: return number(v, std::abs(v - std::round(v)) < 0.05f ? "%.0f" : "%.1f");
            case S::LfoAttack: return v >= 1000 ? significant(v / 1000) + " s" : significant(v) + " ms";
            case S::LfoOffset: return number(v, "%.2f") + "\xc2\xb0";
            default: break;
        }
        if (info.unit == "ms") return v <= 0.1001f && info.minValue >= 0.1f ? "0.00 ms" : v >= 1000 ? significant(v / 1000) + " s" : significant(v) + " ms";
        if (info.unit == "Hz") return v >= 1000 ? significant(v / 1000) + " kHz" : significant(v) + " Hz";
        if (info.unit == "dB") return significant(v) + " dB";
        if (info.unit == "lin") return v <= 0.0003f ? "-inf dB" : number(dsp::gainToDb(v), "%.1f dB");
        if (info.unit == "%") return significant(v * 100) + " %";
        if (info.unit == "st") return number(v, "%.0f st");
        if (info.unit == "ct") return number(v, "%.0f ct");
        if (index == S::FilterRes) return number(v * 100, "%.1f %%");
        if (index == S::Pan) return std::abs(v) < 0.005f ? "C" : number(std::abs(v) * 50, "%.0f") + (v < 0 ? "L" : "R");
        if (info.minValue <= -0.99f && info.maxValue >= 0.99f && info.maxValue <= 1.01f) return significant(v * 100) + " %"; // slopes
    }
    if (info.unit == "lin") return v <= 0 ? "-inf dB" : number(dsp::gainToDb(v), "%.1f dB");
    if (dynamic_cast<F*>(p) && index >= F::OscStart && (index - F::OscStart) % F::OscParams == F::Level)
        return v <= 0 ? "-inf dB" : number(dsp::gainToDb(v), "%.1f dB");
    if (info.unit == "%") return number(v * 100, "%.0f %%");
    if (info.unit == "Hz") return v >= 1000 ? number(v / 1000, "%.2f kHz") : number(v, v < 10 ? "%.2f Hz" : "%.0f Hz");
    if (info.unit == "ms") return v >= 1000 ? number(v / 1000, "%.2f s") : number(v, "%.1f ms");
    if (info.unit == "dB") return number(v, "%.1f dB");
    if (info.unit == "st" || info.unit == "ct") return number(v, "%.0f") + " " + info.unit;
    return number(v, "%.2f");
}
bool logarithmic(const devices::ParameterInfo& info)
{
    return info.minValue > 0 && (info.unit == "Hz" || info.unit == "ms" || info.unit == "lin");
}
class ChoiceKnob final : public Knob
{
public:
    ChoiceKnob(std::string label, Source source, P* processor, int parameter)
        : Knob(std::move(label), std::move(source)), processor_(processor), parameter_(parameter) {}
    bool mouseWheel(const MouseEvent& e) override
    {
        if (e.right || std::abs(e.wheelDeltaX) > std::abs(e.wheelDeltaY)) return false;
        processor_->setParameter(parameter_, std::round(processor_->getParameter(parameter_)) + (e.wheelDeltaY > 0 ? 1.0f : -1.0f));
        repaint(); return true;
    }
private:
    P* processor_;
    int parameter_;
};
// Small icons of the reference's filter-type and LFO-wave choosers.
void filterIcon(SkCanvas& c, Rect r, int type, SkColor ink)
{
    SkPathBuilder path;
    const float x = r.x, y = r.y, w = r.w, h = r.h;
    if (type == 1) path.moveTo(x, y + h).lineTo(x + w * 0.35f, y + h * 0.15f).lineTo(x + w, y + h * 0.15f);
    else if (type == 2) path.moveTo(x, y + h).lineTo(x + w * 0.5f, y + h * 0.1f).lineTo(x + w, y + h);
    else if (type == 3) path.moveTo(x, y + h * 0.15f).lineTo(x + w * 0.4f, y + h * 0.15f).lineTo(x + w * 0.5f, y + h).lineTo(x + w * 0.6f, y + h * 0.15f).lineTo(x + w, y + h * 0.15f);
    else if (type == 4) path.moveTo(x, y + h * 0.15f).lineTo(x + w * 0.3f, y + h * 0.15f).lineTo(x + w * 0.5f, y + h * 0.6f).lineTo(x + w * 0.7f, y + h * 0.15f).lineTo(x + w, y + h * 0.15f);
    else path.moveTo(x, y + h * 0.15f).lineTo(x + w * 0.65f, y + h * 0.15f).lineTo(x + w, y + h);
    SkPaint pen; pen.setAntiAlias(true); pen.setStyle(SkPaint::kStroke_Style); pen.setStrokeWidth(1.1f); pen.setColor(ink);
    c.drawPath(path.detach(), pen);
}
void waveIcon(SkCanvas& c, Rect r, int shape, SkColor ink)
{
    SkPathBuilder path;
    const float x = r.x, y = r.y, w = r.w, h = r.h, mid = y + h * 0.5f;
    if (shape == 0) { path.moveTo(x, mid); for (int i = 1; i <= 16; ++i) path.lineTo(x + w * (float) i / 16, mid - h * 0.5f * std::sin(6.2831853f * (float) i / 16)); }
    else if (shape == 1) path.moveTo(x, y + h).lineTo(x, y).lineTo(x + w * 0.5f, y).lineTo(x + w * 0.5f, y + h).lineTo(x + w, y + h).lineTo(x + w, y);
    else if (shape == 2) path.moveTo(x, y + h).lineTo(x + w * 0.5f, y).lineTo(x + w, y + h);
    else if (shape == 3) path.moveTo(x, y).lineTo(x + w, y + h).moveTo(x + w, y).lineTo(x + w, y + h);
    else if (shape == 4) path.moveTo(x, y + h).lineTo(x + w, y).lineTo(x + w, y + h);
    else path.moveTo(x, mid).lineTo(x + w * 0.25f, mid).lineTo(x + w * 0.25f, y).lineTo(x + w * 0.5f, y).lineTo(x + w * 0.5f, y + h).lineTo(x + w * 0.75f, y + h).lineTo(x + w * 0.75f, y + h * 0.3f).lineTo(x + w, y + h * 0.3f);
    SkPaint pen; pen.setAntiAlias(true); pen.setStyle(SkPaint::kStroke_Style); pen.setStrokeWidth(1.1f); pen.setColor(ink);
    c.drawPath(path.detach(), pen);
}
void noteIcon(SkCanvas& c, float cx, float cy, SkColor ink)
{
    SkPaint pen; pen.setAntiAlias(true); pen.setColor(ink);
    c.drawOval(SkRect::MakeXYWH(cx - 3.5f, cy + 1, 4, 3), pen);
    paint::line(c, cx + 0.3f, cy + 2.5f, cx + 0.3f, cy - 4, ink, 1);
    paint::line(c, cx + 0.3f, cy - 4, cx + 3, cy - 2, ink, 1);
}
class Readout final : public Widget
{
public:
    // Looks of the reference's sampler controls (DEV-22 visual parity).
    enum Look { Plain, Row, CyanToggle, Checkbox, Field = 5, DarkChooser, RetrigBox, SlopePair, SyncPair, FilterType = 11, WaveType, SmallChooser, LightChooser, InlineChooser,
                Pair, AmberCheck, AmberBox, Box };
    int look = Plain;
    bool cyanValue = false, valueRight = false, chooser = false, dim = false, backing = false; // Pair: value colour / alignment, drop-down mark
    bool compact = false; // Row: smaller text (Sonora-only fields squeezed into the sample row)
    float textSize = 0;   // toggles, choosers and Box: 0 == the theme's small font size
    Readout(P* p, int index, std::string label, bool dark = false) : p_(p), index_(index), label_(std::move(label)), dark_(dark) {}
    void paint(SkCanvas& canvas, const Theme& theme) override
    {
        const auto& info = p_->parameters()[(size_t) index_];
        if (look != Plain && paintLook(canvas, theme, info)) return;
        if (dynamic_cast<T*>(p_))
        {
            paint::fill(canvas, localBounds(), theme.color("RetroDisplayBackground"));
            const bool on = p_->getParameter(index_) > 0.5f;
            if (info.isToggle)
                ctl::toggle(canvas, theme, localBounds().reduced(1), index_ == T::Strobe ? (on ? "Strobe" : "Target") : (on ? "Hz" : "ct"), true, theme.color("ChosenDefault"));
            else if (! info.choices.empty())
                paint::text(canvas, formatted(p_, index_), localBounds(), theme.color("RetroDisplayScaleText"), theme.metrics.smallFontSize, Align::Centre);
            else
            {
                paint::text(canvas, label_, {1, 0, width() * 0.46f, height()}, theme.color("RetroDisplayScaleText"), theme.metrics.smallFontSize);
                paint::text(canvas, formatted(p_, index_), {width() * 0.46f, 0, width() * 0.54f - 2, height()}, theme.color("RangeDefault"), theme.metrics.smallFontSize, Align::Right);
            }
            return;
        }
        if (dark_)
        {
            paint::fill(canvas, localBounds(), theme.color("RetroDisplayBackground"));
            paint::text(canvas, label_, {0, 0, width(), height() * 0.48f}, theme.color("RetroDisplayScaleText"), theme.metrics.smallFontSize, Align::Centre);
            paint::text(canvas, formatted(p_, index_), {0, height() * 0.48f, width(), height() * 0.52f}, theme.color("RangeDefault"), theme.metrics.smallFontSize, Align::Centre);
        }
        else if (info.isToggle)
        {
            const bool on = p_->getParameter(index_) > 0.5f;
            const auto label = dynamic_cast<S*>(p_) && index_ == S::FilterSlope ? (on ? std::string("24 dB") : std::string("12 dB")) : label_;
            ctl::toggle(canvas, theme, localBounds(), label, on, theme.color("ChosenDefault"), true, false, textSize);
        }
        else if (! info.choices.empty()) ctl::dropdown(canvas, theme, localBounds(), formatted(p_, index_), true, textSize);
        else
        {
            paint::text(canvas, label_, {1, 0, width() * 0.43f, height()}, theme.color("TextDisabled"), theme.metrics.smallFontSize);
            paint::text(canvas, formatted(p_, index_), {width() * 0.43f, 0, width() * 0.57f - 2, height()}, theme.color("ControlForeground"), theme.metrics.smallFontSize, Align::Right);
            paint::line(canvas, 0, height() - 1, width(), height() - 1, theme.color("ControlContrastFrame"), 0.5f);
        }
    }
    bool paintLook(SkCanvas& c, const Theme& t, const devices::ParameterInfo& info)
    {
        const float v = p_->getParameter(index_);
        const bool on = v > 0.5f;
        const auto r = localBounds();
        const SkColor cyan = t.color("RangeDefault"), dark = t.color("RetroDisplayBackground"), label = t.color("RetroDisplayScaleText");
        const float size = t.metrics.smallFontSize;
        switch (look)
        {
            case Row:
            {
                const float rowSize = compact ? size - 1.5f : size;
                paint::fill(c, r, dark);
                paint::text(c, label_, {0, 0, r.w, r.h * 0.5f}, label, rowSize);
                paint::text(c, formatted(p_, index_), {0, r.h * 0.5f, r.w, r.h * 0.5f}, cyan, rowSize);
                return true;
            }
            case CyanToggle:
                paint::fill(c, r, on ? cyan : dark);
                if (! on) paint::stroke(c, r.reduced(0.5f), cyan, 1);
                paint::text(c, label_, r, on ? dark : cyan, size - 1.5f, Align::Centre);
                return true;
            case Checkbox:
            {
                const Rect box{0, (r.h - 11) * 0.5f, 11, 11};
                paint::fill(c, box, on ? t.color("ChosenDefault") : t.color("ControlBackground"));
                paint::stroke(c, box.reduced(0.5f), t.color("ControlContrastFrame"), 1);
                paint::text(c, label_, {15, 0, r.w - 15, r.h}, t.color("ControlForeground"), size + 0.5f, Align::Left, true);
                return true;
            }
            case Field:
            {
                ctl::framed(c, t, r, t.color("ControlBackground"));
                const float norm = info.choices.empty() ? (logarithmic(info) ? std::log(v / info.minValue) / std::log(info.maxValue / info.minValue)
                                                                              : (v - info.minValue) / (info.maxValue - info.minValue))
                                                         : v / std::max(1.0f, info.maxValue);
                paint::fill(c, {r.x + 1, r.y + 1, (r.w - 2) * std::clamp(norm, 0.0f, 1.0f), r.h - 2}, t.color("RangeEditField"));
                paint::text(c, formatted(p_, index_), r, t.color("ControlForeground"), size, Align::Centre);
                return true;
            }
            case DarkChooser:
                paint::fill(c, r, dark);
                paint::text(c, label_, {0, 0, r.w, r.h * 0.5f}, label, size);
                paint::text(c, formatted(p_, index_), {0, r.h * 0.5f, r.w * 0.5f, r.h * 0.5f}, cyan, size);
                ctl::triangle(c, r.w * 0.62f, r.h * 0.75f, 6, 3.5f, ctl::Dir::Down, cyan);
                return true;
            case RetrigBox:
            {
                paint::fill(c, r, dark);
                paint::text(c, label_, {0, 0, r.w, r.h * 0.5f}, label, size);
                const Rect box{r.w * 0.5f - 7, r.h * 0.5f + 1, 9, 9};
                paint::fill(c, box, on ? cyan : dark);
                paint::stroke(c, box.reduced(0.5f), cyan, 1);
                paint::text(c, "R", box, on ? dark : cyan, size - 2, Align::Centre, true);
                return true;
            }
            case SlopePair:
            case SyncPair:
                for (int half = 0; half < 2; ++half)
                {
                    const Rect box{half * r.w * 0.5f + (half ? 1.0f : 0.0f), 0, r.w * 0.5f - 1, r.h};
                    const bool lit = (half == 1) == on;
                    ctl::framed(c, t, box, lit ? t.color("ChosenDefault") : t.color("ControlBackground"));
                    if (look == SlopePair) paint::text(c, half ? "24" : "12", box, t.color("ControlForeground"), size, Align::Centre);
                    else if (half == 0) paint::text(c, "Hz", box, t.color("ControlForeground"), size, Align::Centre);
                    else noteIcon(c, box.centreX(), box.centreY(), t.color("ControlForeground"));
                }
                return true;
            case LightChooser:
                paint::text(c, label_, {0, 0, r.w, r.h * 0.5f}, t.color("ControlForeground"), size);
                ctl::dropdown(c, t, {0, r.h * 0.5f, r.w, r.h * 0.5f}, "");
                paint::text(c, formatted(p_, index_), {3, r.h * 0.5f, r.w - 13, r.h * 0.5f}, t.color("ControlForeground"), size - 1);
                return true;
            case InlineChooser:
            {
                paint::text(c, label_, {1, 0, r.w * 0.45f, r.h}, t.color("TextDisabled"), size - 1);
                const Rect box{r.w * 0.45f, 0, r.w * 0.55f, r.h};
                ctl::dropdown(c, t, box, "");
                paint::text(c, formatted(p_, index_), {box.x + 2, 0, box.w - 11, r.h}, t.color("ControlForeground"), size - 1.5f);
                return true;
            }
            case SmallChooser:
                ctl::dropdown(c, t, r, "", ! isEnabled || isEnabled());
                paint::text(c, formatted(p_, index_), {r.x + 3, r.y, r.w - 13, r.h}, t.color(! isEnabled || isEnabled() ? "ControlForeground" : "TextDisabled"), size - 1);
                return true;
            case FilterType:
            case WaveType:
            {
                const bool enabled = ! isEnabled || isEnabled();
                ctl::dropdown(c, t, r, "", enabled);
                const Rect icon{r.x + 4, r.y + 3, r.w - 18, r.h - 6};
                if (enabled)
                {
                    if (look == FilterType) filterIcon(c, icon, (int) std::lround(v), t.color("ControlForeground"));
                    else waveIcon(c, icon, (int) std::lround(v), t.color("ControlForeground"));
                }
                return true;
            }
            case Pair:
            {
                // The reference's display fields: grey label, then the value in amber (settings) or cyan (amounts).
                const auto value = formatted(p_, index_);
                const SkColor ink = paint::withAlpha(cyanValue ? cyan : t.color("ChosenDefault"), dim ? 170 : 255);
                const float mark = chooser ? 10.0f : 0.0f;
                if (backing) paint::fill(c, r, dark); // keeps graph overlays legible over the curve
                paint::text(c, label_, r, dim ? paint::withAlpha(label, 150) : label, size);
                if (valueRight) paint::text(c, value, {0, 0, r.w - mark, r.h}, ink, size, Align::Right);
                else
                {
                    const float x = label_.empty() ? 0.0f : gfx::measureText(label_, size) + 5.0f;
                    paint::text(c, value, {x, 0, r.w - x - mark, r.h}, ink, size);
                }
                if (chooser) ctl::triangle(c, r.w - 4, r.h * 0.5f, 6, 3.5f, ctl::Dir::Down, t.color("ChosenDefault"));
                return true;
            }
            case AmberCheck:
            {
                const float side = 8;
                const Rect box{0, (r.h - side) * 0.5f, side, side};
                if (on) paint::fill(c, box, t.color("ChosenDefault"));
                else paint::stroke(c, box.reduced(0.5f), label, 1);
                paint::text(c, label_, {side + 6, 0, r.w - side - 6, r.h}, label, size + 0.5f);
                return true;
            }
            case Box: // the reference's plain number box ("120 Hz")
                ctl::valueBox(c, t, r, formatted(p_, index_), Align::Centre, ! isEnabled || isEnabled(), textSize);
                return true;
            case AmberBox:
                paint::fill(c, r, on ? t.color("ChosenDefault") : dark);
                if (! on) paint::stroke(c, r.reduced(0.5f), t.color("ChosenDefault"), 1);
                paint::text(c, label_, r, on ? dark : t.color("ChosenDefault"), size - 1.5f, Align::Centre, true);
                return true;
            default: return false;
        }
    }
    std::function<bool()> isEnabled;
    void mouseDown(const MouseEvent& e) override { pressed_ = ! e.right; startY_ = e.y; start_ = p_->getParameter(index_); }
    void mouseDrag(const MouseEvent& e) override
    {
        const auto& info = p_->parameters()[(size_t) index_];
        if (! pressed_ || e.right || info.isToggle || ! info.choices.empty()) return;
        const float delta = (startY_ - e.y) / (e.shift ? 1800 : 180);
        p_->setParameter(index_, logarithmic(info) ? start_ * std::pow(info.maxValue / info.minValue, delta) : start_ + delta * (info.maxValue - info.minValue));
        repaint();
    }
    void mouseUp(const MouseEvent& e) override
    {
        if (! pressed_) return;
        pressed_ = false;
        if (e.right || ! localBounds().contains(e.x, e.y) || std::abs(e.y - startY_) > 3) return;
        const auto& info = p_->parameters()[(size_t) index_];
        if (look == SlopePair || look == SyncPair) p_->setParameter(index_, e.x >= width() * 0.5f ? 1.0f : 0.0f);
        else if (look == Checkbox || look == CyanToggle || look == RetrigBox || look == AmberCheck || look == AmberBox) p_->setParameter(index_, p_->getParameter(index_) > 0.5f ? 0 : 1);
        else if (info.isToggle) p_->setParameter(index_, p_->getParameter(index_) > 0.5f ? 0 : 1);
        else if (! info.choices.empty())
        {
            const auto labels = info.choices;
            const int current = (int) std::lround(p_->getParameter(index_));
            std::vector<bool> ticks(labels.size()); ticks[(size_t) std::clamp(current, 0, (int) ticks.size() - 1)] = true;
            auto pick = [this](int chosen) { if (Widget::isAlive(this) && chosen >= 0) { p_->setParameter(index_, (float) chosen); repaint(); } };
            if (ctx().showChoices) ctx().showChoices(labels, ticks, std::move(pick));
            else
            {
                juce::PopupMenu menu;
                for (size_t i = 0; i < labels.size(); ++i) menu.addItem((int) i + 1, labels[i], true, ticks[i]);
                menu.showMenuAsync(juce::PopupMenu::Options(), [selection = std::move(pick)](int choice) { if (choice > 0) selection(choice - 1); });
            }
        }
        repaint();
    }
    void mouseDoubleClick(const MouseEvent& e) override { if (! e.right) { p_->setParameter(index_, p_->parameters()[(size_t) index_].defaultValue); repaint(); } }
    bool mouseWheel(const MouseEvent& e) override
    {
        if (e.right || std::abs(e.wheelDeltaX) > std::abs(e.wheelDeltaY)) return false;
        const auto& info = p_->parameters()[(size_t) index_];
        const float direction = e.wheelDeltaY > 0 ? 1.0f : -1.0f;
        if (info.isToggle || ! info.choices.empty()) p_->setParameter(index_, std::round(p_->getParameter(index_)) + direction);
        else if (logarithmic(info)) p_->setParameter(index_, p_->getParameter(index_) * std::pow(info.maxValue / info.minValue, direction * (e.shift ? 0.001f : 0.01f)));
        else p_->setParameter(index_, p_->getParameter(index_) + direction * (info.maxValue - info.minValue) * (e.shift ? 0.001f : 0.01f));
        repaint(); return true;
    }
private:
    P* p_; int index_; std::string label_; float startY_ = 0, start_ = 0; bool pressed_ = false, dark_ = false;
};
class SampleReadout final : public Widget
{
public:
    std::function<float()> get;
    std::function<void(float)> commit;
    std::string label;
    float low = 0, high = 1;
    bool percent = true;
    void paint(SkCanvas& c, const Theme& t) override
    {
        paint::fill(c, localBounds(), t.color("RetroDisplayBackground"));
        paint::text(c, label, {0, 0, width(), height() * 0.5f}, t.color("RetroDisplayScaleText"), t.metrics.smallFontSize);
        const float v = pressed_ ? pending_ : get();
        paint::text(c, percent ? number(v * 100, "%.1f %%") : number(v, "%.1f dB"), {0, height() * 0.5f, width(), height() * 0.5f},
                    t.color("RangeDefault"), t.metrics.smallFontSize);
    }
    void mouseDown(const MouseEvent& e) override { if (! e.right) { pressed_ = true; y_ = e.y; start_ = pending_ = get(); } }
    void mouseDrag(const MouseEvent& e) override
    {
        if (! pressed_) return;
        pending_ = std::clamp(start_ + (y_ - e.y) * (high - low) / (e.shift ? 1800 : 180), low, high); repaint();
    }
    void mouseUp(const MouseEvent&) override
    {
        if (! pressed_) return;
        pressed_ = false;
        if (std::abs(pending_ - start_) > 1e-7f && commit) { auto callback = commit; callback(pending_); }
    }
private:
    bool pressed_ = false;
    float y_ = 0, start_ = 0, pending_ = 0;
};
class SampleModeButton final : public TextButton
{
public:
    SampleModeButton(int mode, std::string label) : TextButton(std::move(label), Style::Plain), mode_(mode) {}
    void paint(SkCanvas& c, const Theme& t) override
    {
        const SkColor ink = t.color(isOn && isOn() ? "ChosenDefault" : "RetroDisplayScaleText");
        paint::fill(c, localBounds(), t.color("RetroDisplayBackground"));
        const float x = width() * 0.5f, y = height() * 0.32f;
        SkPaint pen; pen.setAntiAlias(true); pen.setColor(ink); pen.setStrokeWidth(1); pen.setStyle(SkPaint::kStroke_Style);
        if (mode_ == 0)
        {
            c.drawArc(SkRect::MakeXYWH(x - 5, y - 4, 10, 8), 30, 300, false, pen);
            ctl::triangle(c, x + 4, y + 3, 4, 4, ctl::Dir::Down, ink);
        }
        else if (mode_ == 1)
        {
            paint::line(c, x - 5, y, x + 5, y, ink); paint::line(c, x - 5, y - 4, x - 5, y + 4, ink);
            ctl::triangle(c, x + 4, y, 5, 5, ctl::Dir::Right, ink);
        }
        else for (int i = 0; i < 4; ++i) paint::line(c, x - 5 + (float) i * 3, y - 4, x - 5 + (float) i * 3, y + 4, ink);
        paint::text(c, label(), {0, height() * 0.52f, width(), height() * 0.4f}, ink, t.metrics.smallFontSize - 1, Align::Centre);
    }
private:
    int mode_;
};
class DisplayButton final : public TextButton
{
public:
    explicit DisplayButton(std::string label) : TextButton(std::move(label)) {}
    void paint(SkCanvas& c, const Theme& t) override
    {
        const auto ink = t.color(isOn && isOn() ? "RangeDefault" : "RetroDisplayScaleText");
        paint::fill(c, localBounds(), t.color("RetroDisplayBackground"));
        paint::stroke(c, localBounds().reduced(1), ink, 0.7f);
        paint::text(c, label(), localBounds(), ink, t.metrics.smallFontSize, Align::Centre);
    }
};
// The reference's view switches inside a display ("Frequency / Envelope", "Amplitude / Pitch"): cyan square + label.
class ViewToggle final : public TextButton
{
public:
    explicit ViewToggle(std::string label) : TextButton(std::move(label), Style::Plain) {}
    void paint(SkCanvas& c, const Theme& t) override
    {
        const float side = 7;
        const Rect box{0, (height() - side) * 0.5f, side, side};
        if (isOn && isOn()) paint::fill(c, box, t.color("RangeDefault"));
        else paint::stroke(c, box.reduced(0.5f), t.color("RetroDisplayScaleText"), 1);
        paint::text(c, label(), {side + 5, 0, width() - side - 5, height()}, t.color("RetroDisplayScaleText"), t.metrics.smallFontSize);
    }
};
// The reference's Bass Mono audition switch: a ring with a headphone glyph, filled amber while on.
class AuditionButton final : public TextButton
{
public:
    AuditionButton() : TextButton("", Style::Toggle) {}
    void paint(SkCanvas& c, const Theme& t) override
    {
        const bool on = isOn && isOn(), enabled = ! isEnabled || isEnabled();
        const float d = std::min(width(), height()) - 1, cx = width() * 0.5f, cy = height() * 0.5f;
        const SkColor ink = enabled ? t.color("ControlForeground") : t.color("TextDisabled");
        SkPaint pen; pen.setAntiAlias(true);
        if (on) { pen.setColor(t.color("ChosenDefault")); c.drawCircle(cx, cy, d * 0.5f, pen); }
        pen.setStyle(SkPaint::kStroke_Style); pen.setStrokeWidth(1.2f); pen.setColor(ink);
        c.drawCircle(cx, cy, d * 0.5f - 0.6f, pen);
        // Headband and two ear pads.
        const float s = d * 0.5f;
        c.drawArc(SkRect::MakeLTRB(cx - s * 0.52f, cy - s * 0.5f, cx + s * 0.52f, cy + s * 0.5f), 180, 180, false, pen);
        pen.setStyle(SkPaint::kFill_Style);
        c.drawRoundRect(SkRect::MakeLTRB(cx - s * 0.6f, cy - s * 0.02f, cx - s * 0.3f, cy + s * 0.48f), 1, 1, pen);
        c.drawRoundRect(SkRect::MakeLTRB(cx + s * 0.3f, cy - s * 0.02f, cx + s * 0.6f, cy + s * 0.48f), 1, 1, pen);
    }
};
// The Swiss Army Meter's reset: a thin grey ring (the original's bottom-right circle).
class ResetCircle final : public TextButton
{
public:
    ResetCircle() : TextButton("", Style::Plain) {}
    void paint(SkCanvas& c, const Theme&) override
    {
        SkPaint pen; pen.setAntiAlias(true); pen.setStyle(SkPaint::kStroke_Style);
        pen.setStrokeWidth(std::max(1.0f, width() * 0.1f)); pen.setColor(SkColorSetRGB(0x92, 0x92, 0x92));
        c.drawCircle(width() * 0.5f, height() * 0.5f, std::min(width(), height()) * 0.5f - pen.getStrokeWidth() * 0.5f, pen);
    }
};
void grid(SkCanvas& c, const Theme& t, Rect r)
{
    paint::fill(c, r, t.color("RetroDisplayBackground"));
    const auto color = paint::withAlpha(t.color("RetroDisplayScaleText"), 55);
    for (int i = 1; i < 4; ++i)
    {
        paint::line(c, r.x + r.w * (float) i / 4, r.y, r.x + r.w * (float) i / 4, r.bottom(), color, 0.5f);
        paint::line(c, r.x, r.y + r.h * (float) i / 4, r.right(), r.y + r.h * (float) i / 4, color, 0.5f);
    }
}
void stroke(SkCanvas& c, SkPathBuilder& path, SkColor color, float thickness = 1.3f)
{
    SkPaint pen; pen.setAntiAlias(true); pen.setStyle(SkPaint::kStroke_Style); pen.setStrokeWidth(thickness); pen.setColor(color);
    c.drawPath(path.detach(), pen);
}
// Simpler Controls displays (DEV-22 visual parity, geometry from the reference capture).
void corners(SkCanvas& c, Rect r, SkColor ink)
{
    const float n = 7;
    for (const auto& [x, y, dx, dy] : std::array<std::array<float, 4>, 4>{{{r.x, r.y, 1, 1}, {r.right(), r.y, -1, 1}, {r.x, r.bottom(), 1, -1}, {r.right(), r.bottom(), -1, -1}}})
    {
        paint::line(c, x, y, x + dx * n, y, ink, 1);
        paint::line(c, x, y, x, y + dy * n, ink, 1);
    }
}
float slopeFraction(float u, float slope) // drawn curve of an envelope stage: linear at 0, exponential towards +-1
{
    constexpr float k = 6;
    if (slope >= 0) return (1 - slope) * u + slope * (1 - std::exp(-k * u)) / (1 - std::exp(-k));
    return (1 + slope) * u - slope * (std::exp(k * u) - 1) / (std::exp(k) - 1);
}
void simplerEnvelope(SkCanvas& c, const Theme& t, P* p, Rect r, const std::array<int, 7>& params, bool bipolar = false)
{
    corners(c, r, paint::withAlpha(t.color("RangeDefault"), 140));
    // Measured layout: stage widths grow with log1p(ms / 1.5) (600 ms = 18 % of the width); sustain ends at 66.5 %.
    const auto span = [&](int parameter) { return r.w * 0.0305f * (float) std::log1p(p->getParameter(parameter) / 1.5); };
    // The pitch envelope is drawn around its zero line (the middle), the others from the floor.
    const float x0 = r.x + r.w * 0.017f, top = r.y + r.h * 0.1f, bottom = bipolar ? r.y + r.h * 0.51f : r.y + r.h * 0.92f;
    const float xa = x0 + span(params[0]), xd = xa + span(params[1]);
    const float xs = std::max(xd, r.x + r.w * 0.665f), xr = std::min(xs + span(params[3]), r.right() - 2);
    const float ys = bottom - (bottom - top) * std::clamp(p->getParameter(params[2]), 0.0f, 1.0f);
    SkPathBuilder curve;
    curve.moveTo(x0, bottom);
    const auto segment = [&](float ax, float ay, float bx, float by, float slope) {
        for (int i = 1; i <= 24; ++i)
        {
            const float u = (float) i / 24;
            curve.lineTo(ax + (bx - ax) * u, ay + (by - ay) * slopeFraction(u, slope));
        }
    };
    segment(x0, bottom, xa, top, p->getParameter(params[4]));
    segment(xa, top, xd, ys, p->getParameter(params[5]));
    curve.lineTo(xs, ys);
    segment(xs, ys, xr, bottom, p->getParameter(params[6]));
    stroke(c, curve, t.color("RangeDefault"));
    for (const auto& point : std::array<std::array<float, 2>, 3>{{{xa, top}, {xd, ys}, {xr, bottom}}})
        paint::stroke(c, {point[0] - 2.5f, point[1] - 2.5f, 5, 5}, t.color("ChosenDefault"), 1);
}
void filterResponse(SkCanvas& c, const Theme& t, const S& simpler, Rect r)
{
    // 30 Hz - 22 kHz, log; +-24 dB around the middle. Same coefficients as the audio (Simpler::filterStages).
    const auto scale = t.color("RetroDisplayScaleText");
    const auto xOf = [&](double hz) { return r.x + r.w * (float) (std::log(hz / 30.0) / std::log(22000.0 / 30.0)); };
    for (double decade : {10.0, 100.0, 1000.0, 10000.0})
        for (int k = 1; k < 10; ++k)
            if (const double hz = decade * k; hz >= 30 && hz <= 22000)
                paint::line(c, xOf(hz), r.y, xOf(hz), r.bottom(), paint::withAlpha(scale, k == 1 ? 60 : 24), 0.5f);
    const float zero = r.y + r.h * 0.5f, perDb = r.h * 0.5f / 24.0f;
    for (int db : {-24, -12, 12, 24}) paint::line(c, r.x, zero - (float) db * perDb, r.right(), zero - (float) db * perDb, paint::withAlpha(scale, 24), 0.5f);
    for (const auto& [hz, label] : std::array<std::pair<double, const char*>, 3>{{{100.0, "100"}, {1000.0, "1k"}, {10000.0, "10k"}}})
        paint::text(c, label, {xOf(hz) - 15, r.bottom() - 12, 30, 10}, paint::withAlpha(scale, 150), t.metrics.smallFontSize, Align::Centre);
    const bool on = simpler.getParameter(S::FilterOn) > 0.5f;
    devices::BiquadCoeffs stages[2];
    const double cutoff = simpler.getParameter(S::FilterFreq);
    const int count = simpler.filterStages(cutoff, stages);
    const auto yOf = [&](double hz) {
        double gain = 1;
        if (on) for (int i = 0; i < count; ++i) gain *= stages[i].magnitudeAt(hz, simpler.sampleRate());
        return std::clamp(zero - (float) (20 * std::log10(std::max(gain, 1e-6))) * perDb, r.y + 1, r.bottom() - 1);
    };
    SkPathBuilder curve;
    for (float x = 0; x <= r.w; x += 1)
    {
        const double hz = 30 * std::pow(22000.0 / 30.0, x / r.w);
        if (x <= 0) curve.moveTo(r.x, yOf(hz)); else curve.lineTo(r.x + x, yOf(hz));
    }
    stroke(c, curve, paint::withAlpha(t.color("RangeDefault"), on ? 255 : 110));
    SkPaint ring; ring.setAntiAlias(true); ring.setStyle(SkPaint::kStroke_Style); ring.setStrokeWidth(1.4f); ring.setColor(t.color("ChosenDefault"));
    c.drawCircle(xOf(std::clamp(cutoff, 30.0, 22000.0)), yOf(cutoff), 4.5f, ring);
}
void lfoWave(SkCanvas& c, const Theme& t, P* p, Rect r)
{
    const int shape = std::clamp((int) std::lround(p->getParameter(S::LfoShape)), 0, 5);
    const double offset = p->getParameter(S::LfoOffset) / 360.0;
    SkPathBuilder curve;
    for (float x = 0; x <= r.w; x += 1)
    {
        const double phase = 3.0 * x / r.w + offset, ph = phase - std::floor(phase);
        float v = 0;
        switch (shape) // reference order: Sine, Square, Triangle, Saw Down, Saw Up, Random
        {
            case 0: v = (float) std::sin(2 * dsp::kPi * ph); break;
            case 1: v = ph < 0.5 ? 1.0f : -1.0f; break;
            case 2: v = 1 - 4 * (float) std::abs(ph - 0.5); break;
            case 3: v = 1 - 2 * (float) ph; break;
            case 4: v = 2 * (float) ph - 1; break;
            default: { const unsigned step = (unsigned) (phase * 4) * 2654435761u; v = (float) (step >> 8 & 0xffff) / 32767.5f - 1; break; }
        }
        const float y = r.centreY() - v * r.h * 0.45f;
        if (x <= 0) curve.moveTo(r.x, y); else curve.lineTo(r.x + x, y);
    }
    stroke(c, curve, paint::withAlpha(t.color("RangeDefault"), p->getParameter(S::LfoOn) > 0.5f ? 255 : 110));
}
}

constexpr float kUtilityText = 9.9f, kUtilityHeader = 10.7f; // face units, measured ("Bass Mono" 49.6 wide, "Input" 31)
bool DeviceFace::supports(P* p)
{
    return dynamic_cast<F*>(p) || dynamic_cast<S*>(p)
        || dynamic_cast<V*>(p) || dynamic_cast<I*>(p) || dynamic_cast<E*>(p) || dynamic_cast<R*>(p) || dynamic_cast<T*>(p) || dynamic_cast<G*>(p) || dynamic_cast<AP*>(p)
        || dynamic_cast<SAM*>(p) || dynamic_cast<U*>(p);
}
DeviceFace::DeviceFace(const model::Device& model, P* p, int track, int device, int chain, int child, std::function<void(Widget*, int)> tag)
    : model_(model), processor_(p), track_(track), device_(device), chain_(chain), child_(child), tag_(std::move(tag))
{
    if (dynamic_cast<F*>(p)) kind_ = Kind::Fm;
    else if (dynamic_cast<S*>(p)) kind_ = model.type == "MultiSampler" ? Kind::Sampler : Kind::Simpler;
    else if (dynamic_cast<V*>(p)) kind_ = Kind::Vector;
    else if (dynamic_cast<I*>(p)) kind_ = Kind::Impulse;
    else if (dynamic_cast<G*>(p)) kind_ = Kind::Gate;
    else if (dynamic_cast<AP*>(p)) kind_ = Kind::AutoPan;
    else if (dynamic_cast<R*>(p)) kind_ = Kind::Redux;
    else if (dynamic_cast<T*>(p)) kind_ = Kind::Tuner;
    else if (dynamic_cast<SAM*>(p)) kind_ = Kind::Meter;
    else if (dynamic_cast<U*>(p)) kind_ = Kind::Utility;
    else kind_ = Kind::Echo;

    if (kind_ == Kind::Fm)
    {
        graph_ = {202, 24, 242, 79};
        for (int osc = 0; osc < 4; ++osc)
        {
            const float y = (float) (3 - osc) * 41;
            tab(std::string(1, (char) ('A' + osc)), {174, y + 12, 16, 18}, osc, true);
            control(F::oscParam(osc, F::Ratio), "Coarse", {0, y, 54, 39});
            control(F::oscParam(osc, F::Fine), "Fine", {58, y, 50, 39});
            control(F::oscParam(osc, F::Level), "Level", {116, y, 50, 39});
            control(F::oscParam(osc, F::Wave), "Wave", {202, 0, 112, 18}, false, 100 + osc);
            int col = 0;
            for (int parameter : {F::Attack, F::Decay, F::Sustain, F::Release})
                control(F::oscParam(osc, (F::OscParam) parameter), p->parameters()[(size_t) F::oscParam(osc, (F::OscParam) parameter)].name.substr(2),
                        {202 + (float) col++ * 61, 110, 58, 54}, true, 100 + osc);
        }
        control(F::Algorithm, "Algorithm", {452, 0, 116, 18}, false);
        control(F::Feedback, "Feedback", {452, 90, 56, 55}); control(F::Volume, "Volume", {512, 90, 56, 55});
        control(F::Transpose, "Transpose", {452, 146, 116, 18}, false);
        control(F::BendRange, "Bend", {452, 70, 116, 18}, false); control(F::Pan, "Pan", {326, 0, 118, 18}, false);
    }
    else if (kind_ == Kind::Simpler)
    {
        // Geometry measured from the reference's Simpler (660 px wide; y scaled to this 164-unit face). Page 0 is the
        // Sample view (display, sample row); page 1 is the Controls view; both share the Filter / LFO / amplitude row.
        // Page codes: 30 Classic only, 31 One-Shot only, 32 not One-Shot; 20 / 21 LFO beat / rate; 33-37 shared row
        // (conditional circuit / drive / morph); 40-43 Controls display views; 44 / 45 loop time / beat repeat.
        const auto look = [](Widget* widget, int value) { static_cast<Readout*>(widget)->look = value; return widget; };
        const auto knob = [this](int parameter, std::string label, Rect rect, int page) {
            auto* widget = static_cast<Knob*>(control(parameter, std::move(label), rect, true, page));
            widget->style = Knob::Style::Compact;
            return widget;
        };
        graph_ = {46, 4, 606, 81};
        for (int mode = 0; mode < 3; ++mode)
        {
            auto* button = add<SampleModeButton>(mode, mode == 0 ? "Classic" : mode == 1 ? "1-Shot" : "Slice");
            button->isOn = [this, mode] { return (int) dynamic_cast<S*>(processor_)->playbackMode() == mode; };
            button->onClick = [this, mode] { edit([mode](auto& d) { d.plain["Globals/PlaybackMode"] = mode; }, "Sample Playback Mode"); };
            controls_.push_back({button, {4, (float) mode * 39.5f, 36, 39}, 0});
        }
        sampleControl("Gain", {46, 88, 40, 26}, 0);
        look(control(S::SampleStartMod, "Start", {87, 88, 41, 26}, false, 0), Readout::Row);
        look(control(S::LoopLengthMod, "Loop", {129, 88, 41, 26}, false, 30), Readout::Row);
        look(control(S::SampleLengthMod, "Length", {170, 88, 41, 26}, false, 0), Readout::Row);
        look(control(S::LoopFadeMod, "Fade", {211, 88, 41, 26}, false, 30), Readout::Row);
        look(control(S::FadeIn, "Fade In", {129, 88, 41, 26}, false, 31), Readout::Row);
        look(control(S::FadeOut, "Fade Out", {211, 88, 41, 26}, false, 31), Readout::Row);
        look(control(S::LoopOn, "LOOP", {253, 92, 34, 9}, false, 30), Readout::CyanToggle);
        look(control(S::Snap, "SNAP", {253, 105, 34, 9}, false, 0), Readout::CyanToggle);
        look(control(S::Voices, "Voices", {296, 88, 36, 26}, false, 32), Readout::DarkChooser);
        look(control(S::Retrigger, "Retrig", {334, 88, 30, 26}, false, 32), Readout::RetrigBox);
        look(control(S::OneShotGate, "Mode", {296, 88, 50, 26}, false, 31), Readout::DarkChooser);

        // Sonora-only voice settings (the reference keeps them in menus) in the free part of the sample row.
        for (const auto& [parameter, label, x, w] : std::array<std::tuple<int, const char*, float, float>, 5>{
                 {{S::KeyZoneShift, "Zone", 368, 24}, {S::VolKeyScale, "Vol<Key", 392, 35}, {S::PanKeyScale, "Pan<Key", 427, 35},
                  {S::ConstPowerFade, "X-Fade", 462, 32}, {S::Interpolation, "Interp", 494, 35}}})
            static_cast<Readout*>(look(control(parameter, label, {x, 88, w, 26}, false, 0), Readout::Row))->compact = true;

        // Filter / LFO / amplitude row, on both pages (33). Circuit choosers: 34 low/high-pass, 35 band/notch/morph;
        // 36 Drive (non-Clean circuits), 37 Morph (Morph type) share the slot after Res, as in the reference.
        look(control(S::FilterOn, "Filter", {5, 124, 40, 12}, false, 33), Readout::Checkbox);
        auto* type = static_cast<Readout*>(look(control(S::FilterType, "Type", {48, 124, 37, 12}, false, 33), Readout::FilterType));
        type->isEnabled = [this] { return processor_->getParameter(S::FilterOn) > 0.5f; };
        look(control(S::FilterSlope, "Slope", {5, 142, 34, 11}, false, 33), Readout::SlopePair);
        const auto filterOn = [this] { return processor_->getParameter(S::FilterOn) > 0.5f; };
        static_cast<Readout*>(look(control(S::FilterCircuit, "Circuit", {48, 142, 37, 11}, false, 34), Readout::SmallChooser))->isEnabled = filterOn;
        static_cast<Readout*>(look(control(S::FilterCircuitBp, "Circuit", {48, 142, 37, 11}, false, 35), Readout::SmallChooser))->isEnabled = filterOn;
        knob(S::FilterFreq, "Frequency", {91, 120, 56, 38}, 33)->isEnabled = filterOn;
        knob(S::FilterRes, "Res", {148, 120, 46, 38}, 33)->isEnabled = filterOn;
        knob(S::FilterDrive, "Drive", {196, 120, 46, 38}, 36)->isEnabled = filterOn;
        knob(S::FilterMorph, "Morph", {196, 120, 46, 38}, 37)->isEnabled = filterOn;
        look(control(S::LfoOn, "LFO", {260, 124, 36, 12}, false, 33), Readout::Checkbox);
        look(control(S::LfoShape, "Wave", {260, 142, 40, 11}, false, 33), Readout::WaveType);
        look(control(S::LfoSync, "Sync", {313, 124, 47, 12}, false, 33), Readout::SyncPair);
        look(control(S::LfoRate, "Rate", {313, 142, 47, 11}, false, 21), Readout::Field);
        look(control(S::LfoBeat, "Beat", {313, 142, 47, 11}, false, 20), Readout::Field);
        int col = 0;
        for (int parameter : {S::Attack, S::Decay, S::Sustain, S::Release, S::Volume})
            knob(parameter, p->parameters()[(size_t) parameter].name, {386 + (float) col++ * 55, 120, 54, 38}, 33);

        // Controls page (measured from a capture of the reference): three
        // displays above the shared row. Filter: Frequency (40) / Envelope (41) views; LFO; Amplitude (42) / Pitch (43)
        // envelope views. selected_ bit 0 = filter Envelope view, bit 1 = Pitch view. Envelope slopes (set by dragging
        // the curves in the reference) are small dim fields inside each graph.
        const auto pair = [this](int parameter, std::string label, Rect rect, int page, bool cyanValue = false, bool right = false) {
            auto* readout = static_cast<Readout*>(control(parameter, std::move(label), rect, false, page));
            readout->look = Readout::Pair; readout->cyanValue = cyanValue; readout->valueRight = right;
            return readout;
        };
        const auto view = [this](std::string label, Rect rect, int bit, bool on) {
            auto* button = add<ViewToggle>(std::move(label));
            button->isOn = [this, bit, on] { return ((selected_ >> bit) & 1) == (on ? 1 : 0); };
            button->onClick = [this, bit, on] { selected_ = on ? selected_ | (1 << bit) : selected_ & ~(1 << bit); layout(); repaint(); };
            controls_.push_back({button, rect, 1});
        };
        const auto slopes = [&](int a, int d, int r, float x, float y, int page) {
            for (int i = 0; i < 3; ++i)
            {
                auto* field = pair(i == 0 ? a : i == 1 ? d : r, i == 0 ? "Slope A" : i == 1 ? "D" : "R", {x + (i == 0 ? 0.0f : 26.0f + (float) i * 48), y, i == 0 ? 72.0f : 46.0f, 9}, page, true);
                field->dim = true; field->backing = true;
            }
        };
        view("Frequency", {7.5f, 106, 61, 11}, 0, false);
        view("Envelope", {69.5f, 106, 62, 11}, 0, true);
        pair(S::FilterVelMod, "Vel", {138.5f, 106, 46, 11}, 1, true);
        pair(S::FilterKeyTrack, "Key", {188.5f, 106, 52, 11}, 1, true);
        look(control(S::FilterEnvOn, "Filter Envelope", {7.5f, 6.5f, 90, 11}, false, 41), Readout::AmberCheck);
        pair(S::FilterEnvAmount, "Amount", {181, 6.5f, 58, 11}, 41);
        pair(S::FilterAttack, "A", {7.5f, 22.5f, 52, 11}, 41);
        pair(S::FilterDecay, "D", {71, 22.5f, 50, 11}, 41);
        pair(S::FilterSustain, "S", {133, 22.5f, 46, 11}, 41);
        pair(S::FilterRelease, "R", {196, 22.5f, 46, 11}, 41);
        slopes(S::FilterAttackSlope, S::FilterDecaySlope, S::FilterReleaseSlope, 70, 37, 41);

        pair(S::LfoAttack, "Attack", {249, 8.2f, 70, 11}, 1);
        pair(S::LfoKeyScale, "Key", {322, 8.2f, 52, 11}, 1);
        pair(S::LfoOffset, "Offset", {249, 23.4f, 66, 11}, 1);
        look(control(S::LfoRetrigger, "R", {322.4f, 24.3f, 9.2f, 9.2f}, false, 1), Readout::AmberBox);
        pair(S::LfoVolume, "Vol", {249, 90.5f, 56, 11}, 1, true);
        pair(S::LfoPitch, "Pitch", {307.6f, 90.5f, 66, 11}, 1, true);
        pair(S::LfoPan, "Pan", {249, 106, 56, 11}, 1, true);
        pair(S::FilterLfoMod, "Filter", {307.6f, 106, 66, 11}, 1, true);

        pair(S::AmpLoopTime, "Time", {512.4f, 6.5f, 64, 11}, 44);
        pair(S::AmpRepeatTime, "Time", {512.4f, 6.5f, 64, 11}, 45)->chooser = true; // Beat / Sync: the repeat division
        pair(S::AmpLoopMode, "Mode", {580, 6.5f, 72, 11}, 42)->chooser = true;
        slopes(S::AttackSlope, S::DecaySlope, S::ReleaseSlope, 390, 66, 42);
        look(control(S::PitchEnvOn, "Pitch Env", {561.3f, 6.5f, 58, 11}, false, 43), Readout::AmberCheck);
        pair(S::PitchEnvAmount, "", {620, 6.5f, 34, 11}, 43);
        const int pitchStages[4] = {S::PitchAttack, S::PitchDecay, S::PitchSustain, S::PitchRelease};
        const char* stageNames[4] = {"Attack", "Decay", "Sustain", "Release"};
        for (int row = 0; row < 4; ++row) pair(pitchStages[row], stageNames[row], {571.5f, 23.8f + (float) row * 13.8f, 82, 11}, 43, false, true);
        slopes(S::PitchAttackSlope, S::PitchDecaySlope, S::PitchReleaseSlope, 398, 66, 43);
        view("Amplitude", {386, 78.5f, 62, 11}, 1, false);
        view("Pitch", {522.6f, 78.5f, 42, 11}, 1, true);
        pair(S::Pan, "Pan", {385, 92.4f, 44, 11}, 1, true);
        pair(S::PanRandom, "Ran>Pan", {450.6f, 92.4f, 72, 11}, 1, true);
        pair(S::Transpose, "Transp", {523.5f, 92.4f, 56, 11}, 1);
        pair(S::GlideMode, "Glide", {591.8f, 92.4f, 62, 11}, 1)->chooser = true;
        pair(S::Spread, "Spread", {385, 106, 56, 11}, 1, true);
        pair(S::VelToVolume, "Vel>Vol", {450.6f, 106, 64, 11}, 1, true);
        pair(S::Detune, "Detune", {523.5f, 106, 58, 11}, 1);
        pair(S::GlideTime, "Time", {591.8f, 106, 62, 11}, 1);
    }
    else if (kind_ == Kind::Sampler)
    {
        tab("Sample", {0, 0, 70, 18}, 0); tab("Filter / Env", {74, 0, 90, 18}, 1); tab("Zones", {168, 0, 58, 18}, 2); tab("Modulation", {230, 0, 86, 18}, 3);
        zones_ = add<NativeDevicePanel>(model, track, device, chain, child);
        graph_ = {kind_ == Kind::Simpler ? 66.0f : 0.0f, 24, kind_ == Kind::Simpler ? 438.0f : 504.0f, 78};
        int col = 0;
        for (int parameter : {S::Transpose, S::Detune, S::Attack, S::Decay, S::Sustain, S::Release, S::VelToVolume, S::Pan})
            control(parameter, p->parameters()[(size_t) parameter].name, {(float) col++ * 63, 110, 60, 54}, true, 0);
        control(S::Volume, "Volume", {514, 34, 60, 58});
        control(S::KeyZoneShift, "Key Shift", {514, 112, 60, 52});
        int mod = 0;
        for (int parameter : {S::Voices, S::Retrigger, S::LfoOn, S::LfoRate, S::LfoShape, S::LfoPitch, S::LfoVolume, S::LfoPan, S::LfoSync, S::LfoBeat})
            control(parameter, p->parameters()[(size_t) parameter].name, {(float) (mod % 5) * 100, 30 + (float) (mod / 5) * 64, 96, 56}, true, 3), ++mod;
        control(S::FilterOn, "Filter", {0, 25, 60, 18}, false, 1);
        control(S::FilterType, "Type", {66, 25, 103, 18}, false, 1);
        control(S::FilterSlope, "Slope", {176, 25, 72, 18}, false, 1);
        int x = 0;
        for (int parameter : {S::FilterFreq, S::FilterRes, S::FilterKeyTrack, S::FilterEnvAmount, S::FilterAttack, S::FilterDecay, S::FilterSustain, S::FilterRelease})
            control(parameter, p->parameters()[(size_t) parameter].name, {(float) x++ * 63, 110, 60, 54}, true, 1);
        control(S::FilterEnvOn, "Envelope", {266, 25, 80, 18}, false, 1);
        control(S::FadeIn, "In", {365, 25, 67, 18}, false, 1); control(S::FadeOut, "Out", {438, 25, 66, 18}, false, 1);
        // Shared voice engine settings (DEV-22). Simpler's region / loop fractions do not apply to zones (see applies()).
        tab("Voice", {320, 0, 58, 18}, 4);
        int slot = 0;
        for (int parameter : {S::AttackSlope, S::DecaySlope, S::ReleaseSlope, S::FilterAttackSlope, S::FilterDecaySlope, S::FilterReleaseSlope,
                              S::FilterMorph, S::FilterVelMod, S::FilterLfoMod, S::FilterDrive, S::GlideTime, S::VolKeyScale, S::PanKeyScale,
                              S::PanRandom, S::PitchEnvAmount, S::PitchAttack, S::PitchDecay, S::PitchSustain, S::PitchRelease,
                              S::PitchAttackSlope, S::PitchDecaySlope, S::PitchReleaseSlope, S::LfoAttack, S::Spread})
        {
            auto* knob = static_cast<Knob*>(control(parameter, p->parameters()[(size_t) parameter].name, {(float) (slot % 10) * 58, 22 + (float) (slot / 10) * 40, 56, 38}, true, 4));
            knob->style = Knob::Style::Compact;
            ++slot;
        }
        slot = 0;
        for (int parameter : {S::GlideMode, S::PitchEnvOn, S::ConstPowerFade, S::Interpolation, S::LfoRetrigger, S::OneShotGate, S::FilterCircuit, S::FilterCircuitBp})
            control(parameter, p->parameters()[(size_t) parameter].name, {(float) slot++ * 72, 146, 70, 13}, false, 4);
    }
    else if (kind_ == Kind::Vector)
    {
        graph_ = {0, 24, 246, 78};
        tab("Osc 1", {0, 0, 56, 18}, 0, true); tab("Osc 2", {60, 0, 56, 18}, 1, true);
        for (int osc = 0; osc < 2; ++osc)
        {
            int col = 0;
            for (int parameter : {V::Position1 + osc, V::Level1 + osc, V::Tune1 + osc, V::Detune1 + osc})
                control(parameter, p->parameters()[(size_t) parameter].name, {(float) col++ * 62, 110, 58, 54}, true, 100 + osc);
        }
        control(V::FilterOn, "Filter", {256, 0, 54, 18}, false); control(V::FilterType, "Type", {316, 0, 98, 18}, false);
        control(V::Cutoff, "Frequency", {256, 110, 76, 54}); control(V::Resonance, "Resonance", {336, 110, 76, 54});
        tab("Amp", {424, 0, 54, 18}, 0); tab("LFO", {482, 0, 54, 18}, 1);
        int col = 0;
        for (int parameter : {V::Attack, V::Decay, V::Sustain, V::Release})
            control(parameter, p->parameters()[(size_t) parameter].name, {424 + (float) col++ * 56, 110, 54, 54}, true, 0);
        control(V::LfoRate, "Rate", {424, 110, 106, 54}, true, 1); control(V::LfoAmount, "Position Amt", {536, 110, 106, 54}, true, 1);
        control(V::SubLevel, "Sub", {660, 0, 60, 51}); control(V::Volume, "Volume", {660, 55, 60, 51});
        control(V::Pan, "Pan", {660, 110, 60, 51}); control(V::Transpose, "Transpose", {124, 0, 122, 18}, false);
    }
    else if (kind_ == Kind::Impulse)
    {
        graph_ = {0, 46, 504, 10};
        for (int slot = 0; slot < 8; ++slot)
        {
            const auto* zone = dynamic_cast<I*>(processor_)->zone(slot);
            auto* button = add<TextButton>(zone ? zone->part.name : "Drop Sample");
            button->isOn = [this, slot] { return selected_ == slot; };
            button->onClick = [this, slot] { selected_ = slot; dynamic_cast<I*>(processor_)->audition(slot); layout(); repaint(); };
            controls_.push_back({button, {(float) slot * 65, 0, 61, 42}, -1});
            int col = 0;
            for (int parameter : {I::Start, I::Tune, I::Drive, I::Cutoff, I::Resonance, I::Decay, I::Pan, I::Level})
                control(I::slotParam(slot, (I::SlotParam) parameter), p->parameters()[(size_t) I::slotParam(slot, (I::SlotParam) parameter)].name,
                        {(float) col++ * 65, 64, 61, 64}, true, 100 + slot);
            control(I::slotParam(slot, I::FilterOn), "Filter", {196, 142, 61, 18}, false, 100 + slot);
        }
        control(I::Volume, "Master", {528, 0, 60, 64});
    }
    else if (kind_ == Kind::Gate)
    {
        graph_ = {72, 2, 136, 126};
        control(G::Attack, "Attack", {0, 0, 64, 52}); control(G::Hold, "Hold", {0, 55, 64, 52}); control(G::Release, "Release", {0, 110, 64, 52});
        control(G::Threshold, "Threshold", {72, 136, 136, 24}, false);
        control(G::Return, "Return", {218, 0, 68, 56}); control(G::Floor, "Floor", {218, 61, 68, 56});
        control(G::Flip, "Flip", {218, 122, 68, 18}, false); control(G::Listen, "Listen", {218, 144, 68, 18}, false);
        control(G::Lookahead, "Lookahead", {72, 113, 136, 18}, false);
    }
    else if (kind_ == Kind::AutoPan)
    {
        graph_ = {0, 0, 276, 74};
        control(AP::On, "LFO", {234, 2, 40, 16}, false);
        control(AP::Amount, "Amount", {0, 80, 64, 59}); control(AP::Rate, "Rate", {70, 80, 64, 59});
        control(AP::Phase, "Phase", {140, 80, 64, 59}); control(AP::Offset, "Offset", {210, 80, 64, 59});
        control(AP::Shape, "Wave", {0, 144, 92, 18}, false); control(AP::Sync, "Sync", {98, 144, 40, 18}, false);
        control(AP::Beat, "Beat", {144, 144, 84, 18}, false); control(AP::Invert, "Invert", {234, 144, 42, 18}, false);
    }
    else if (kind_ == Kind::Redux)
    {
        graph_ = {8, 4, 238, 46};
        control(R::Rate, "Rate", {4, 55, 76, 68}); control(R::Jitter, "Jitter", {84, 55, 76, 68});
        control(R::Bits, "Bits", {164, 55, 76, 68}); control(R::Shape, "Shape", {248, 4, 64, 62});
        control(R::DryWet, "Dry/Wet", {248, 91, 64, 68});
        control(R::PreFilter, "Pre", {4, 129, 46, 18}, false); control(R::PostFilter, "Post", {54, 129, 46, 18}, false);
        control(R::PostOctave, "Oct", {104, 129, 68, 18}, false); control(R::Dc, "DC Shift", {176, 129, 64, 18}, false);
    }
    else if (kind_ == Kind::Tuner)
    {
        graph_ = {0, 0, 300, 140};
        control(T::ViewMode, "View", {0, 144, 62, 18}, false);
        control(T::Strobe, "Strobe", {66, 144, 52, 18}, false);
        control(T::DisplayHz, "Hz", {122, 144, 32, 18}, false);
        control(T::Reference, "Reference", {158, 144, 142, 18}, false);
    }
    else if (kind_ == Kind::Utility)
    {
        // Geometry measured from the reference's Utility (face units: x from the face's left edge, y of 164): Input
        // column (phase, channel, Width / Mid-Side, Mono, Bass Mono + frequency + audition) | Output column (Gain,
        // Balance, Mute, DC). Headers, the divider and its arrow are painted.
        auto toggle = [this](int parameter, std::string label, Rect rect) {
            auto* readout = dynamic_cast<Readout*>(control(parameter, std::move(label), rect, false));
            return readout;
        };
        toggle(U::PhaseInvertL, "\xc3\x98 L", {2.1f, 18.6f, 24.9f, 11.7f});
        toggle(U::PhaseInvertR, "\xc3\x98 R", {30.7f, 18.6f, 25.2f, 11.7f});
        control(U::ChannelMode, "Channel", {2.1f, 34.1f, 53.8f, 12.1f}, false);
        utilityKnob(0, {0, 53.7f, 58, 51});
        toggle(U::Mono, "Mono", {2.1f, 110.3f, 53.8f, 11.4f});
        toggle(U::BassMono, "Bass Mono", {2.1f, 131.1f, 53.8f, 11.8f});
        auto* frequency = dynamic_cast<Readout*>(control(U::BassMonoFrequency, "Bass Freq", {2.1f, 146.4f, 38.5f, 11.7f}, false));
        frequency->look = Readout::Box;
        auto* audition = add<AuditionButton>();
        audition->isOn = [p] { return p->getParameter(U::BassMonoAudition) > 0.5f; };
        audition->onClick = [p] { p->setParameter(U::BassMonoAudition, p->getParameter(U::BassMonoAudition) > 0.5f ? 0.0f : 1.0f); };
        if (tag_) tag_(audition, U::BassMonoAudition);
        controls_.push_back({audition, {43.8f, 146.5f, 12.1f, 12.1f}, -1});
        utilityKnob(1, {67.3f, 23.9f, 58, 51});
        utilityKnob(2, {67.3f, 85.8f, 58, 51});
        toggle(U::Mute, "Mute", {70.8f, 145.7f, 29.2f, 11.6f});
        toggle(U::DcFilter, "DC", {103.5f, 145.7f, 17.7f, 11.6f});
    }
    else if (kind_ == Kind::Meter)
    {
        // DEV-23: Swiss Army Meter, measured from a capture of the original (face units, x from the face's left edge,
        // y of 164, scaled uniformly): dark body, a rounded readout panel, an 8-segment meter each side, the
        // correlation bar at the bottom of the panel and the reset circle bottom right. The original's logo is left out.
        graph_ = {24.9f, 7.5f, 66.6f, 149.8f}; // readout panel
        auto* reset = add<ResetCircle>();
        reset->onClick = [p] { if (auto* m = dynamic_cast<SAM*>(p)) m->requestReset(); };
        controls_.push_back({reset, {100.5f, 144.5f, 11.2f, 11.2f}, -1});
    }
    else
    {
        graph_ = {136, 24, 232, 104};
        control(E::TimeL, "Left", {0, 25, 58, 56}, true, 10); control(E::TimeR, "Right", {64, 25, 58, 56}, true, 11);
        control(E::DivisionL, "Left", {0, 25, 58, 56}, true, 12); control(E::DivisionR, "Right", {64, 25, 58, 56}, true, 13);
        control(E::SyncL, "Sync", {0, 4, 58, 18}, false); control(E::SyncR, "Sync", {64, 4, 58, 18}, false);
        control(E::Input, "Input", {0, 100, 58, 60}); control(E::Feedback, "Feedback", {64, 100, 62, 60});
        tab("Echo", {136, 0, 72, 18}, 0); tab("Modulation", {212, 0, 80, 18}, 1); tab("Character", {296, 0, 72, 18}, 2);
        control(E::HighPass, "HP", {136, 138, 110, 20}, false, 0); control(E::LowPass, "LP", {254, 138, 114, 20}, false, 0);
        control(E::ModRate, "Rate", {148, 91, 88, 64}, true, 1); control(E::ModAmount, "Amount", {260, 91, 88, 64}, true, 1);
        control(E::Duck, "Ducking", {148, 91, 88, 64}, true, 2); control(E::Invert, "Invert Feedback", {254, 112, 108, 18}, false, 2);
        control(E::FilterOn, "Filter", {376, 2, 68, 18}, false); control(E::Mode, "Mode", {376, 28, 94, 20}, false);
        control(E::Output, "Output", {382, 55, 78, 54}); control(E::DryWet, "Dry/Wet", {382, 110, 78, 54});
    }
}
bool DeviceFace::applies(const std::string& parameterId) const
{
    // Simpler's sample region and loop are not part of a Sampler zone (zones carry their own region and loop); the
    // Simpler-only LFO offset / key scale and envelope loop settings are not read for the Sampler either.
    if (kind_ != Kind::Sampler) return true;
    for (int parameter : {S::LoopOn, S::SampleStartMod, S::SampleLengthMod, S::LoopLengthMod, S::LoopFadeMod, S::Snap,
                          S::LfoOffset, S::LfoKeyScale, S::AmpLoopMode, S::AmpLoopTime, S::AmpRepeatTime})
        if (processor_->parameters()[(size_t) parameter].id == parameterId) return false;
    return true;
}
float DeviceFace::preferredWidth() const
{
    switch (kind_)
    {
        case Kind::Fm: return 570;
        case Kind::Simpler: return 660;
        case Kind::Sampler: return 580;
        case Kind::Vector: return 724;
        case Kind::Impulse: return 592;
        case Kind::Echo: return 472;
        case Kind::Redux: return 316;
        case Kind::Tuner: return 300;
        case Kind::Gate: return 292;
        case Kind::AutoPan: return 276;
        case Kind::Meter: return 117.4f * (height() > 0 ? height() / 164 : 1.0f);
        case Kind::Utility: return 125.4f * (height() > 0 ? height() / 164 : 1.0f);
    }
    return 580;
}
Rect DeviceFace::scaled(Rect r) const
{
    const float sy = height() / 164;
    if (kind_ == Kind::Utility || kind_ == Kind::Meter) return {r.x * sy, r.y * sy, r.w * sy, r.h * sy}; // measured as a whole: keep its proportions
    return {r.x, r.y * sy, r.w, r.h * sy};
}
Widget* DeviceFace::control(int index, std::string label, Rect rect, bool knob, int page, bool dark)
{
    Widget* widget;
    const auto& info = processor_->parameters()[(size_t) index];
    if (knob)
    {
        Knob::Source source;
        source.get = [p = processor_, index] { return p->getParameter(index); };
        source.set = [p = processor_, index, discrete = ! info.choices.empty()](float v) { p->setParameter(index, discrete ? std::round(v) : v); };
        source.minValue = info.minValue; source.maxValue = info.maxValue; source.defaultValue = info.defaultValue;
        source.logarithmic = logarithmic(info); source.bipolar = info.minValue < 0 && info.maxValue > 0;
        source.format = [p = processor_, index](float) { return formatted(p, index); };
        Knob* dial;
        if (! info.choices.empty()) dial = add<ChoiceKnob>(std::move(label), std::move(source), processor_, index);
        else dial = add<Knob>(std::move(label), std::move(source));
        dial->style = Knob::Style::Device; // UI-42: the reference's device knob (faces may pick another style after)
        widget = dial;
    }
    else widget = add<Readout>(processor_, index, std::move(label), dark);
    if (tag_) tag_(widget, index);
    controls_.push_back({widget, rect, page});
    return widget;
}
void DeviceFace::sampleControl(std::string label, Rect rect, int field)
{
    auto* readout = add<SampleReadout>(); readout->label = std::move(label); readout->percent = field != 0;
    if (field == 0) { readout->low = -36; readout->high = 36; }
    readout->get = [this, field] {
        if (model_.sampleParts.empty()) return 0.0f;
        const auto& p = model_.sampleParts[0];
        const double frames = (double) std::max<int64_t>(1, p.lengthSamples);
        const double end = p.sampleEnd > 0 ? (double) p.sampleEnd : frames;
        if (field == 0) return dsp::gainToDb((float) p.volume);
        if (field == 1) return (float) ((double) p.sampleStart / frames);
        if (field == 2) return (float) ((double) (p.loopStart - p.sampleStart) / std::max(1.0, end - (double) p.sampleStart));
        if (field == 3) return (float) ((double) (p.loopEnd - p.loopStart) / std::max(1.0, end - (double) p.loopStart));
        return (float) ((double) p.loopCrossfade / std::max(1.0, (double) (p.loopEnd - p.loopStart)));
    };
    readout->commit = [this, field](float value) {
        edit([field, value](auto& d) {
            if (d.sampleParts.empty()) return;
            auto& p = d.sampleParts[0];
            const auto frames = std::max<int64_t>(1, p.lengthSamples), end = p.sampleEnd > 0 ? p.sampleEnd : frames;
            if (field == 0) p.volume = dsp::dbToGain(value);
            else if (field == 1) { p.sampleStart = std::clamp<int64_t>((int64_t) (value * (double) frames), 0, end - 1); p.loopStart = std::max(p.loopStart, p.sampleStart); }
            else if (field == 2) p.loopStart = std::clamp<int64_t>(p.sampleStart + (int64_t) (value * (double) (end - p.sampleStart)), p.sampleStart, end - 1);
            else if (field == 3) p.loopEnd = std::clamp<int64_t>(p.loopStart + (int64_t) (value * (double) (end - p.loopStart)), p.loopStart + 1, end);
            else p.loopCrossfade = (int64_t) (value * (double) std::max<int64_t>(0, p.loopEnd - p.loopStart));
            p.loopEnd = std::clamp(p.loopEnd, p.loopStart + 1, end);
            p.loopCrossfade = std::clamp<int64_t>(p.loopCrossfade, 0, (p.loopEnd - p.loopStart) / 2);
            d.samplePartsEdited = true;
        }, "Sample Region");
    };
    controls_.push_back({readout, rect, 0});
}
void DeviceFace::tab(std::string label, Rect rect, int value, bool selection)
{
    auto* button = add<TextButton>(std::move(label));
    button->isOn = [this, value, selection] { return (selection ? selected_ : page_) == value; };
    button->onClick = [this, value, selection] { (selection ? selected_ : page_) = value; layout(); repaint(); };
    controls_.push_back({button, rect, -1});
}
void DeviceFace::layout()
{
    for (auto& control : controls_)
    {
        const int page = control.page;
        bool show = page < 0 || (page >= 100 ? selected_ == page - 100 : page_ == page);
        if (kind_ == Kind::Echo && page >= 10 && page <= 13)
        {
            const bool right = page == 11 || page == 13;
            show = (processor_->getParameter(right ? E::SyncR : E::SyncL) > 0.5f) == (page >= 12);
        }
        if (kind_ == Kind::Simpler)
        {
            const int mode = (int) dynamic_cast<S*>(processor_)->playbackMode();
            const bool sync = processor_->getParameter(S::LfoSync) > 0.5f;
            const int type = (int) std::lround(processor_->getParameter(S::FilterType));
            const bool lowHigh = type <= 1, circuit = processor_->getParameter(lowHigh ? S::FilterCircuit : S::FilterCircuitBp) > 0.5f;
            const bool shared = page_ == 0 || page_ == 1;
            if (page == 20 || page == 21) show = shared && sync == (page == 20);
            else if (page >= 30 && page <= 32) show = page_ == 0 && (page == 30 ? mode == 0 : page == 31 ? mode == 1 : mode != 1);
            else if (page == 33) show = shared;
            else if (page == 34 || page == 35) show = shared && lowHigh == (page == 34);
            else if (page == 36) show = shared && circuit && type != 4;
            else if (page == 37) show = shared && type == 4;
            else if (page >= 40 && page <= 43) show = page_ == 1 && ((selected_ >> (page >= 42 ? 1 : 0)) & 1) == (page % 2);
            else if (page == 44 || page == 45)
            {
                const int loopMode = (int) std::lround(processor_->getParameter(S::AmpLoopMode));
                show = page_ == 1 && (selected_ & 2) == 0 && (loopMode == 2 || loopMode == 3) == (page == 45);
            }
        }
        control.widget->visible = show;
        control.widget->setBounds(scaled(control.bounds));
    }
    if (zones_) { zones_->visible = page_ == 2; zones_->setBounds(scaled({0, 24, 504, 140})); }
    if (kind_ == Kind::Utility)
    {
        // Text measured against the reference grows with the face, like its geometry.
        const float size = kUtilityText * height() / 164;
        for (auto& control : controls_)
        {
            if (auto* readout = dynamic_cast<Readout*>(control.widget)) readout->textSize = size;
            if (auto* knob = dynamic_cast<Knob*>(control.widget)) knob->fontSize = size;
        }
    }
}
void DeviceFace::tick()
{
    if (kind_ == Kind::Echo) layout();
    if (kind_ == Kind::Tuner)
    {
        const float hz = dynamic_cast<T*>(processor_)->frequency();
        const float pitch = hz > 0 ? 69 + 12 * std::log2(hz / processor_->getParameter(T::Reference)) : 0;
        history_[(size_t) historyWrite_] = pitch; historyWrite_ = (historyWrite_ + 1) % (int) history_.size();
        strobePhase_ = std::fmod(strobePhase_ + (pitch - std::round(pitch)) * 20 + 300, 300.0);
    }
    if (kind_ == Kind::Meter)
    {
        const auto reading = dynamic_cast<SAM*>(processor_)->reading();
        const float levels[2] = {reading.momentaryL, reading.momentaryR};
        for (size_t ch = 0; ch < 2; ++ch)
        {
            // -60 .. 0 LUFS, each channel read as loud as the pair (+3 dB), as the original's two meters agree with its
            // readout; the bar rises at once and falls ~20 LU/s at 30 ticks/s; the hold mark stays ~1.5 s.
            const float target = std::clamp((levels[ch] + 3.01f + 60.0f) / 60.0f, 0.0f, 1.0f);
            meterLevel_[ch] = target >= meterLevel_[ch] ? target : std::max(target, meterLevel_[ch] - 0.011f);
            if (meterLevel_[ch] >= meterHold_[ch]) { meterHold_[ch] = meterLevel_[ch]; meterHoldAge_[ch] = 0; }
            else if (++meterHoldAge_[ch] > 45) meterHold_[ch] = std::max(meterLevel_[ch], meterHold_[ch] - 0.011f);
        }
    }
    repaint();
}
void DeviceFace::envelope(SkCanvas& c, const Theme& t, Rect r, int a, int d, int s, int release)
{
    grid(c, t, r);
    const auto widthOf = [this](int parameter) { return std::log1p(processor_->getParameter(parameter)); };
    const float attack = widthOf(a), decay = widthOf(d), rel = widthOf(release), sum = attack + decay + rel + 4;
    const float x1 = r.x + r.w * attack / sum, x2 = x1 + r.w * decay / sum, x3 = x2 + r.w * 4 / sum;
    const float top = r.y + 7, bottom = r.bottom() - 7, sustain = bottom - (r.h - 14) * processor_->getParameter(s);
    SkPathBuilder curve;
    curve.moveTo(r.x + 2, bottom).lineTo(x1, top).lineTo(x2, sustain).lineTo(x3, sustain).lineTo(r.right() - 2, bottom);
    stroke(c, curve, t.color("RangeDefault"));
    for (const auto& point : std::array<std::array<float, 2>, 3>{{{x1, top}, {x2, sustain}, {x3, sustain}}})
        paint::fill(c, {point[0] - 2, point[1] - 2, 4, 4}, t.color("ChosenDefault"));
}
void DeviceFace::filter(SkCanvas& c, const Theme& t, Rect r, int frequency, int resonance, int type)
{
    grid(c, t, r);
    const float cutoff = processor_->getParameter(frequency), res = processor_->getParameter(resonance);
    const int mode = type < 0 ? 0 : (int) std::lround(processor_->getParameter(type));
    SkPathBuilder curve;
    for (float x = 0; x <= r.w; ++x)
    {
        const float f = 20 * std::pow(1000.0f, x / std::max(1.0f, r.w));
        const float ratio = f / cutoff;
        float gain = 1 / std::sqrt(1 + std::pow(ratio, 4.0f));
        if (mode == 1) gain = 1 / std::sqrt(1 + std::pow(1 / ratio, 4.0f));
        else if (mode == 2) gain = std::exp(-2 * std::pow(std::log2(ratio), 2.0f));
        else if (mode == 3) gain = 1 - std::exp(-4 * std::pow(std::log2(ratio), 2.0f));
        const float y = r.bottom() - 6 - (r.h - 18) * gain - res * 12 * std::exp(-8 * std::pow(std::log2(ratio), 2.0f));
        if (x <= 0) curve.moveTo(r.x + x, y); else curve.lineTo(r.x + x, y);
    }
    stroke(c, curve, t.color("RangeDefault"));
    const float x = r.x + r.w * std::clamp(std::log(cutoff / 20) / std::log(1000.0f), 0.0f, 1.0f);
    paint::fill(c, {x - 2, r.centreY() - 2, 5, 5}, t.color("ChosenDefault"));
}
void DeviceFace::waveform(SkCanvas& c, const Theme& t, Rect r)
{
    const bool simpler = kind_ == Kind::Simpler;
    paint::fill(c, r, t.color(simpler ? "SampleDisplayBackground" : "RetroDisplayBackground"));
    const S::Zone* zone = nullptr;
    if (const auto* sampler = dynamic_cast<S*>(processor_); sampler && ! sampler->zones().empty()) zone = &sampler->zones()[0];
    if (const auto* impulse = dynamic_cast<I*>(processor_)) zone = impulse->zone(selected_);
    const auto* sample = zone && zone->slot ? zone->slot->get() : nullptr;
    if (! sample || sample->audio.getNumSamples() < 2)
    {
        paint::text(c, zone ? "Sample unavailable" : "Drop Sample Here", r, t.color("RetroDisplayScaleText"), t.metrics.fontSize, Align::Centre);
        return;
    }
    const int count = sample->audio.getNumSamples();
    if (previewSource_ != sample) { preview_.reset(); previewSource_ = sample; }
    if (! preview_)
    {
        if (ctx().peaks)
        {
            model::Clip clip; clip.kind = model::Clip::Kind::Audio; clip.sample = zone->part.sample;
            preview_ = ctx().peaks(clip);
        }
        else preview_ = engine::computePeaks(sample->audio, sample->sampleRate);
    }
    const float centre = r.y + (r.h - (simpler ? 16 : 12)) * 0.5f, amplitude = (r.h - (simpler ? 24 : 18)) * 0.48f;
    SkPathBuilder upper, lower, trace;
    const bool close = count <= (int) (r.w * 2);
    bool first = true;
    for (float x = 0; x <= r.w; x += 0.5f)
    {
        const double frame = (double) x / r.w * (count - 1);
        float lo = 0, hi = 0;
        if (close)
        {
            const int i = (int) frame, j = std::min(count - 1, i + 1);
            const float f = (float) (frame - i);
            const float v = sample->audio.getSample(0, i) * (1 - f) + sample->audio.getSample(0, j) * f;
            if (first) trace.moveTo(r.x + x, centre - v * amplitude); else trace.lineTo(r.x + x, centre - v * amplitude);
        }
        else if (preview_)
        {
            preview_->range(frame / sample->sampleRate, std::min((double) count, frame + (double) count / r.w * 0.5) / sample->sampleRate, lo, hi, 0);
            if (first) { upper.moveTo(r.x + x, centre - hi * amplitude); lower.moveTo(r.x + x, centre - lo * amplitude); }
            else { upper.lineTo(r.x + x, centre - hi * amplitude); lower.lineTo(r.x + x, centre - lo * amplitude); }
        }
        first = false;
    }
    if (close) stroke(c, trace, t.color("RetroDisplayForeground"), 1);
    else if (preview_) { stroke(c, upper, t.color("RetroDisplayForeground"), 1); stroke(c, lower, t.color("RetroDisplayForeground"), 1); }
    if (simpler)
    {
        // Reference ruler: ticks every 1/2/5 x 10^n ms with "m:ss:mmm" labels inside the display, right of each tick.
        const double seconds = count / sample->sampleRate;
        double step = 0.001;
        for (double base = 0.001; base < 1000; base *= 10)
            for (double m : {1.0, 2.0, 5.0})
                if (seconds / (base * m) <= 5.5 && step < base * m * 0.999) { step = base * m; goto chosen; }
        chosen:
        for (int tick = 0; tick * step < seconds; ++tick)
        {
            const double at = tick * step;
            const float x = r.x + (float) (at / seconds) * r.w;
            const int ms = (int) std::lround(at * 1000);
            char text[32];
            if (ms % 1000 == 0) std::snprintf(text, sizeof text, "%d:%02d", ms / 60000, ms / 1000 % 60);
            else std::snprintf(text, sizeof text, "%d:%02d:%03d", ms / 60000, ms / 1000 % 60, ms % 1000);
            const auto ink = paint::withAlpha(t.color("RetroDisplayScaleText"), 150);
            paint::line(c, x, r.bottom() - 11, x, r.bottom() - 2, ink, 0.6f);
            paint::text(c, text, {x + 2, r.bottom() - 12, 60, 10}, ink, t.metrics.smallFontSize - 1);
        }
    }
    else for (int tick = 0; tick <= 4; ++tick)
    {
        const float x = r.x + r.w * (float) tick / 4;
        paint::line(c, x, r.bottom() - 13, x, r.bottom() - 10, t.color("RetroDisplayScaleText"), 0.5f);
        paint::text(c, number((float) ((double) tick * count / sample->sampleRate / 4), "%.3f"),
                    {std::clamp(x - 16, r.x + 2, r.right() - 38), r.bottom() - 11, 36, 10},
                    t.color("RetroDisplayScaleText"), t.metrics.smallFontSize - 1, Align::Centre);
    }
    if (kind_ != Kind::Impulse)
    {
        const auto marker = [&](int64_t frame, bool end, SkColor color) {
            const float x = r.x + std::clamp((float) ((double) frame / count), 0.0f, 1.0f) * r.w;
            paint::line(c, x, r.y + 2, x, r.bottom() - 2, color);
            ctl::triangle(c, x + (end ? -3.0f : 3.0f), r.y + 5, 6, 7, end ? ctl::Dir::Left : ctl::Dir::Right, color);
        };
        const auto boundary = t.color(simpler ? "RetroDisplayScaleText" : "ChosenDefault");
        marker(dragHandle_ == 0 ? dragFrame_ : zone->part.sampleStart, false, boundary);
        marker(dragHandle_ == 1 ? dragFrame_ : zone->part.sampleEnd > 0 ? zone->part.sampleEnd : count, true, boundary);
        if (simpler && processor_->getParameter(S::LoopOn) > 0.5f && processor_->getParameter(S::LoopLengthMod) < 0.999f)
        {
            const double start = (double) zone->part.sampleStart, end = zone->part.sampleEnd > 0 ? (double) zone->part.sampleEnd : (double) count;
            const double regionStart = start + processor_->getParameter(S::SampleStartMod) * (end - start);
            const double regionEnd = std::min(end, regionStart + processor_->getParameter(S::SampleLengthMod) * (end - start));
            marker((int64_t) (regionEnd - processor_->getParameter(S::LoopLengthMod) * (regionEnd - regionStart)), false, t.color("RangeDefault"));
        }
        if (! simpler && zone->part.loopMode > 0)
        {
            marker(zone->part.loopStart, false, t.color("RangeDefault")); marker(zone->part.loopEnd, true, t.color("RangeDefault"));
        }
    }
    if (kind_ != Kind::Simpler) paint::text(c, zone->part.name, {r.x + 8, r.y + 2, r.w - 16, 12}, t.color("RetroDisplayScaleText"), t.metrics.smallFontSize);
}
void DeviceFace::routing(SkCanvas& c, const Theme& t, Rect r)
{
    paint::fill(c, r, t.color("RetroDisplayBackground"));
    const int algorithm = (int) std::lround(processor_->getParameter(F::Algorithm));
    static constexpr int parents[8][4] = {{-1,0,1,2}, {-1,0,0,0}, {-1,0,-1,2}, {-1,0,1,-1}, {-1,0,0,1}, {-1,-1,-1,2}, {-1,-1,-1,0}, {-1,-1,-1,-1}};
    // Nodes sit in four fixed columns; connecting rails show the actual selected algorithm.
    const float y = r.y + 14;
    for (int i = 0; i < 4; ++i)
    {
        const float x = r.x + 8 + (float) i * 25;
        const int parent = parents[std::clamp(algorithm, 0, 7)][i];
        if (parent >= 0)
        {
            const float target = r.x + 16 + (float) parent * 25;
            paint::line(c, x + 8, y + 16, x + 8, r.bottom() - 12 - (float) i * 3, t.color("RangeDefault"));
            paint::line(c, x + 8, r.bottom() - 12 - (float) i * 3, target, r.bottom() - 12 - (float) i * 3, t.color("RangeDefault"));
            paint::line(c, target, r.bottom() - 12 - (float) i * 3, target, y + 16, t.color("RangeDefault"));
        }
        ctl::toggle(c, t, {x, y, 17, 16}, std::string(1, (char) ('A' + i)), selected_ == i, t.color("ChosenDefault"));
    }
}
Widget* DeviceFace::utilityKnob(int which, Rect rect)
{
    // The reference's knob laws: 0 dB, 100 % width, centre balance and mid/side all point straight up.
    // Gain: +-35 dB linear in dB around the top, the last 5 % of the left side fading to -inf (approximation).
    // Width: 0-100 % on the left half, 100-400 % on the right. Positions are 0..1.
    auto* p = processor_;
    Knob::Source source;
    source.minValue = 0; source.maxValue = 1; source.defaultValue = 0.5f;
    std::string label;
    int parameter = U::Gain;
    if (which == 0)
    {
        label = "Width"; parameter = U::StereoWidth;
        source.get = [p] {
            if (p->getParameter(U::MidSideOn) > 0.5f) return p->getParameter(U::MidSideBalance) * 0.5f;
            const float w = p->getParameter(U::StereoWidth);
            return w <= 1 ? w * 0.5f : 0.5f + (w - 1) / 6;
        };
        source.set = [p](float pos) {
            if (p->getParameter(U::MidSideOn) > 0.5f) p->setParameter(U::MidSideBalance, pos * 2);
            else p->setParameter(U::StereoWidth, pos <= 0.5f ? pos * 2 : 1 + (pos - 0.5f) * 6);
        };
        source.format = [p](float) {
            if (p->getParameter(U::MidSideOn) > 0.5f)
            {
                const float v = p->getParameter(U::MidSideBalance) - 1;
                return std::abs(v) < 0.005f ? std::string("C") : number(std::abs(v) * 100, "%.0f") + (v < 0 ? "M" : "S");
            }
            return number(p->getParameter(U::StereoWidth) * 100, "%.0f %%");
        };
    }
    else if (which == 1)
    {
        label = "Gain";
        source.get = [p] {
            const float g = p->getParameter(U::Gain);
            if (g <= 0) return 0.0f;
            const float db = dsp::gainToDb(g);
            if (db >= 0) return 0.5f + std::min(db, 35.0f) / 70;
            if (db >= -33.25f) return 0.5f + db / 70;
            return 0.025f * g / dsp::dbToGain(-33.25f); // last 5 %: linear amplitude down to silence
        };
        source.set = [p](float pos) {
            pos = std::clamp(pos, 0.0f, 1.0f);
            p->setParameter(U::Gain, pos >= 0.025f ? dsp::dbToGain((pos - 0.5f) * 70) : dsp::dbToGain(-33.25f) * pos / 0.025f);
        };
        source.format = [p](float) {
            const float g = p->getParameter(U::Gain);
            return g <= 0.0001f ? std::string("-inf dB") : number(dsp::gainToDb(g), "%.2f dB");
        };
    }
    else
    {
        label = "Balance"; parameter = U::Balance;
        source.get = [p] { return (p->getParameter(U::Balance) + 1) * 0.5f; };
        source.set = [p](float pos) { p->setParameter(U::Balance, pos * 2 - 1); };
        source.format = [p](float) {
            const float v = p->getParameter(U::Balance);
            return std::abs(v) < 0.01f ? std::string("C") : number(std::abs(v) * 50, "%.0f") + (v < 0 ? "L" : "R");
        };
    }
    auto* knob = add<Knob>(label, std::move(source));
    knob->style = Knob::Style::Ring;
    knob->marker = 0.5f;
    if (which == 0)
        knob->onLabelClick = [this, p, knob] {
            const bool midSide = p->getParameter(U::MidSideOn) < 0.5f;
            p->setParameter(U::MidSideOn, midSide ? 1.0f : 0.0f);
            knob->setLabel(midSide ? "Mid/Side" : "Width");
            if (tag_) tag_(knob, midSide ? U::MidSideBalance : U::StereoWidth);
            repaint();
        };
    if (which == 0 && p->getParameter(U::MidSideOn) > 0.5f) { knob->setLabel("Mid/Side"); parameter = U::MidSideBalance; }
    if (tag_) tag_(knob, parameter);
    controls_.push_back({knob, rect, -1});
    return knob;
}
void DeviceFace::meter(SkCanvas& c, const Theme&)
{
    // The original's own colours (it is dark in every theme), sampled from the capture.
    const SkColor panel = SkColorSetRGB(0x25, 0x25, 0x25), edge = SkColorSetRGB(0x1d, 0x1d, 0x1d);
    const SkColor label = SkColorSetRGB(0x9c, 0x9d, 0xa0), ink = SkColorSetRGB(0xc3, 0xfd, 0xfb), icon = SkColorSetRGB(0x92, 0x92, 0x92);
    const SkColor slot = SkColorSetRGB(0x2d, 0x2c, 0x2d), unlit = SkColorSetRGB(0x25, 0x25, 0x25);
    const float sy = height() / 164;
    const auto reading = dynamic_cast<SAM*>(processor_)->reading();
    const auto value = [](float v, const char* format) { return v <= dsp::kSilenceDb + 1 ? std::string("-inf") : number(v, format); };
    // Text sizes fitted to the widths measured in the capture, so another font keeps the same footprint.
    const auto fitted = [sy](std::string_view sample, float width, bool bold) { return 10.0f * width * sy / gfx::measureText(sample, 10.0f, bold); };
    const float labelSize = fitted("Integrated", 44.3f, false), bigSize = fitted("-17.4", 51.7f, true), valueSize = fitted("-15.20", 35.3f, true);

    const auto box = graphBounds();
    SkPaint fill; fill.setAntiAlias(true); fill.setColor(panel);
    const SkRect rounded = SkRect::MakeXYWH(box.x, box.y, box.w, box.h);
    c.drawRoundRect(rounded, 4 * sy, 4 * sy, fill);
    fill.setStyle(SkPaint::kStroke_Style); fill.setStrokeWidth(1); fill.setColor(edge);
    c.drawRoundRect(rounded.makeInset(0.5f, 0.5f), 4 * sy, 4 * sy, fill);
    const float column = box.x, width = box.w;
    const auto row = [&](float centre, float h) { const auto r = scaled({0, centre - h / 2, 0, h}); return Rect{column, r.y, width, r.h}; };
    paint::text(c, "LUFS", row(18.5f, 12), label, labelSize, Align::Centre);
    paint::text(c, value(reading.shortTerm, "%.1f"), row(37.7f, 20), ink, bigSize, Align::Centre, true);
    paint::text(c, "Integrated", row(69.8f, 12), label, labelSize, Align::Centre);
    paint::text(c, value(reading.integrated, "%.2f"), row(85.1f, 14), ink, valueSize, Align::Centre, true);
    paint::text(c, "True Peak", row(111.9f, 12), label, labelSize, Align::Centre);
    paint::text(c, value(reading.truePeak, "%.2f"), row(125.2f, 14), ink, valueSize, Align::Centre, true);

    // Correlation from the panel centre: dark amber bar ending in a bright mark; a single mark at 0.
    const auto bar = scaled({0, 147.4f, 0, 8.3f});
    const float centre = box.centreX(), half = 30.6f * sy, mark = 2.5f * sy;
    const float x = centre + std::clamp(reading.correlation, -1.0f, 1.0f) * half;
    if (std::abs(x - centre) > mark)
        paint::fill(c, {std::min(centre, x), bar.y, std::abs(x - centre), bar.h}, SkColorSetRGB(0xa9, 0x7f, 0x0c));
    const float markX = std::abs(x - centre) > mark ? (x > centre ? x - mark : x) : centre - 0.75f * sy;
    paint::fill(c, {markX, bar.y, std::abs(x - centre) > mark ? mark : 1.5f * sy, bar.h}, SkColorSetRGB(0xe5, 0xa7, 0x00));

    // Per-channel momentary meters: 8 segments, teal at the bottom to green at the top, a green hold line.
    for (int ch = 0; ch < 2; ++ch)
    {
        const Rect outer = scaled({ch == 0 ? 4.5f : 99.7f, 36.9f, 12.4f, 101.1f});
        paint::fill(c, outer, slot);
        constexpr int kSegments = 8;
        const float gap = 1.3f * sy, inset = 1.6f * sy, step = (outer.h - gap) / kSegments;
        for (int s = 0; s < kSegments; ++s)
        {
            const Rect cell{outer.x + inset, outer.bottom() - (float) (s + 1) * step, outer.w - 2 * inset, step - gap};
            paint::fill(c, cell, unlit);
            const float lit = std::clamp(meterLevel_[(size_t) ch] * kSegments - (float) s, 0.0f, 1.0f);
            const float mix = (float) s / (kSegments - 1);
            const SkColor colour = SkColorSetRGB((U8CPU) (0x5c + (0x88 - 0x5c) * mix), (U8CPU) (0xdb + (0xe6 - 0xdb) * mix), (U8CPU) (0xd6 + (0x96 - 0xd6) * mix));
            if (lit > 0) paint::fill(c, {cell.x, cell.y + cell.h * (1 - lit), cell.w, cell.h * lit}, colour);
        }
        if (meterHold_[(size_t) ch] > 0.002f)
        {
            const float y = std::round(outer.bottom() - gap - (outer.h - gap) * meterHold_[(size_t) ch]) + 0.5f;
            paint::line(c, outer.x + inset, y, outer.right() - inset, y, SkColorSetRGB(0x88, 0xe6, 0x96), std::max(1.0f, sy));
        }
    }

    // Expand chevron (top right) and the two-line menu mark (bottom left), drawn as in the original.
    SkPaint pen; pen.setAntiAlias(true); pen.setStyle(SkPaint::kStroke_Style); pen.setColor(icon);
    pen.setStrokeWidth(1.3f * sy); pen.setStrokeCap(SkPaint::kRound_Cap); pen.setStrokeJoin(SkPaint::kRound_Join);
    SkPathBuilder chevron;
    chevron.moveTo(98.9f * sy, 15 * sy).lineTo(106.2f * sy, 21.8f * sy).lineTo(113.6f * sy, 15 * sy);
    c.drawPath(chevron.detach(), pen);
    pen.setStrokeWidth(1.7f * sy);
    c.drawLine(7.7f * sy, 148.5f * sy, 14.1f * sy, 148.5f * sy, pen);
    c.drawLine(3.2f * sy, 154 * sy, 18.2f * sy, 154 * sy, pen);
}
void DeviceFace::paint(SkCanvas& c, const Theme& t)
{
    if (kind_ == Kind::Meter)
    {
        meter(c, t);
        return;
    }
    if (kind_ == Kind::Utility)
    {
        const SkColor ink = t.color("ControlForeground"), divider = t.color("TextDisabled");
        const float header = kUtilityHeader * height() / 164;
        paint::text(c, "Input", scaled({0, 1.7f, 58, 12.8f}), ink, header, Align::Centre, true);
        paint::text(c, "Output", scaled({67.3f, 1.7f, 58, 12.8f}), ink, header, Align::Centre, true);
        const auto top = scaled({62, 2.5f, 1, 72.1f}), bottom = scaled({62, 87.8f, 1, 71.2f}), arrow = scaled({0, 81.4f, 0, 0});
        paint::fill(c, {top.x, top.y, 1, top.h}, divider);
        paint::fill(c, {bottom.x, bottom.y, 1, bottom.h}, divider);
        const float sy = height() / 164;
        ctl::triangle(c, 63.1f * sy, arrow.y, 6.4f * sy, 3.8f * sy, ctl::Dir::Right, divider);
        return;
    }
    if (kind_ == Kind::Fm)
    {
        envelope(c, t, graphBounds(), F::oscParam(selected_, F::Attack), F::oscParam(selected_, F::Decay), F::oscParam(selected_, F::Sustain), F::oscParam(selected_, F::Release));
        routing(c, t, scaled({452, 24, 116, 40}));
        for (int row = 1; row < 4; ++row) paint::line(c, 0, scaled({0, (float) row * 41, 0, 0}).y, 190, scaled({0, (float) row * 41, 0, 0}).y, t.color("ControlContrastFrame"));
    }
    else if (kind_ == Kind::Simpler)
    {
        const auto separator = t.color("TextDisabled");
        if (page_ == 0)
        {
            paint::fill(c, scaled({0, 0, 660, 118}), t.color("RetroDisplayBackground"));
            for (int row = 1; row < 3; ++row)
                paint::line(c, 4, scaled({0, (float) row * 39.5f - 0.5f, 0, 0}).y, 40, scaled({0, (float) row * 39.5f - 0.5f, 0, 0}).y, t.color("ControlContrastFrame"));
            waveform(c, t, graphBounds());
            // Warp is not implemented (DEV-4b): drawn like the reference's unwarped state, without controls.
            const auto dim = paint::withAlpha(t.color("ChosenDefault"), 150), muted = t.color("TextDisabled");
            const auto warp = scaled({531, 90, 49, 10});
            paint::stroke(c, warp.reduced(0.5f), dim, 1);
            paint::text(c, "WARP", warp, dim, t.metrics.smallFontSize - 1.5f, Align::Centre);
            paint::text(c, "as", scaled({584, 90, 14, 10}), muted, t.metrics.smallFontSize, Align::Centre);
            const auto beats = scaled({601, 89, 53, 12});
            paint::stroke(c, beats.reduced(0.5f), muted, 1);
            // The reference's length guess: the sample's length at the song tempo, rounded to a power of two beats.
            const auto& zones = dynamic_cast<S*>(processor_)->zones();
            if (const auto* sample = ! zones.empty() && zones[0].slot ? zones[0].slot->get() : nullptr; sample && ctx().song)
            {
                const double length = sample->audio.getNumSamples() / sample->sampleRate * ctx().song->tempo / 60.0;
                const double rounded = std::exp2(std::round(std::log2(std::max(length, 1.0 / 16))));
                const auto text = rounded >= 4 ? number((float) (rounded / 4), "%.0f") + (rounded >= 8 ? " Bars" : " Bar")
                                               : rounded >= 1 ? number((float) rounded, "%.0f") + (rounded >= 2 ? " Beats" : " Beat") : number((float) rounded, "%g") + " Beat";
                paint::text(c, text, beats, dim, t.metrics.smallFontSize, Align::Centre);
            }
            paint::text(c, "Beats", scaled({533, 103, 46, 10}), muted, t.metrics.smallFontSize);
            for (int i = 0; i < 2; ++i)
            {
                const auto pill = scaled({600 + (float) i * 28, 103, 25, 11});
                paint::stroke(c, pill.reduced(0.5f), muted, 1);
                paint::text(c, i == 0 ? ":2" : "*2", pill, muted, t.metrics.smallFontSize, Align::Centre);
            }
        }
        else
        {
            const auto scale = t.color("RetroDisplayScaleText"), amber = t.color("ChosenDefault");
            const float size = t.metrics.smallFontSize;
            for (const Rect& r : {Rect{2.7f, 4, 240, 115}, Rect{245.8f, 4, 132.4f, 115}, Rect{381, 4, 275.8f, 115}})
                paint::fill(c, scaled(r), t.color("RetroDisplayBackground"));
            if (selected_ & 1)
                simplerEnvelope(c, t, processor_, scaled({7.8f, 35.3f, 231.2f, 68}), {S::FilterAttack, S::FilterDecay, S::FilterSustain, S::FilterRelease,
                                                                                    S::FilterAttackSlope, S::FilterDecaySlope, S::FilterReleaseSlope});
            else filterResponse(c, t, *dynamic_cast<S*>(processor_), scaled({2.7f, 4, 240, 100}));
            lfoWave(c, t, processor_, scaled({250, 37.6f, 124, 46}));
            if (selected_ & 2)
            {
                // Pitch view header: bend ranges are plain set values (read only here).
                const auto plain = [this](const char* key, double fallback) { const auto it = model_.plain.find(key); return it != model_.plain.end() ? it->second : fallback; };
                const auto header = [&](const std::string& label, const std::string& value, float x) {
                    const auto r = scaled({x, 6.5f, 80, 11});
                    paint::text(c, label, r, scale, size);
                    paint::text(c, value, {r.x + gfx::measureText(label, size) + 5, r.y, 60, r.h}, amber, size);
                };
                header("PB Range", number((float) dynamic_cast<S*>(processor_)->pitchBendRange(), "%+.0f st"), 385);
                header("Note PB", number((float) plain("Globals/MpePitchBendRange", 48), "%+.0f st"), 473.7f);
                simplerEnvelope(c, t, processor_, scaled({384.6f, 20.6f, 180, 57}), {S::PitchAttack, S::PitchDecay, S::PitchSustain, S::PitchRelease,
                                                                                    S::PitchAttackSlope, S::PitchDecaySlope, S::PitchReleaseSlope}, true);
            }
            else
            {
                paint::text(c, "Amplitude Envelope", scaled({386, 6.5f, 120, 11}), scale, size + 0.5f, Align::Centre);
                const auto bar = scaled({509.2f, 6.8f, 0, 10});
                paint::line(c, bar.x, bar.y, bar.x, bar.bottom(), paint::withAlpha(scale, 120), 1);
                const float rule = scaled({0, 19.7f, 0, 0}).y;
                paint::line(c, 384.6f, rule, 656.8f, rule, paint::withAlpha(scale, 90), 0.7f);
                simplerEnvelope(c, t, processor_, scaled({384.6f, 22, 272.2f, 55.6f}), {S::Attack, S::Decay, S::Sustain, S::Release,
                                                                                       S::AttackSlope, S::DecaySlope, S::ReleaseSlope});
            }
        }
        for (float x : {244.0f, 377.5f}) paint::line(c, x, scaled({0, 121, 0, 0}).y, x, height() - 2, separator);
    }
    else if (kind_ == Kind::Sampler)
    {
        if (page_ == 0) waveform(c, t, graphBounds());
        else if (page_ == 1)
        {
            filter(c, t, scaled({0, 50, 246, 52}), S::FilterFreq, S::FilterRes, S::FilterType);
            envelope(c, t, scaled({256, 50, 248, 52}), S::FilterAttack, S::FilterDecay, S::FilterSustain, S::FilterRelease);
        }
    }
    else if (kind_ == Kind::Vector)
    {
        const auto r = graphBounds(); grid(c, t, r);
        auto* vector = dynamic_cast<V*>(processor_);
        const float position = processor_->getParameter(V::Position1 + selected_);
        for (int frame = 0; frame < 9; ++frame)
        {
            const float pos = (float) frame / 8;
            SkPathBuilder wave;
            for (int x = 0; x <= (int) r.w - 20; ++x)
            {
                const float y = r.bottom() - 12 - pos * r.h * 0.6f - vector->waveform(pos, (double) x / (r.w - 20)) * r.h * 0.17f;
                const float xx = r.x + (float) x + pos * 18;
                if (x == 0) wave.moveTo(xx, y); else wave.lineTo(xx, y);
            }
            stroke(c, wave, paint::withAlpha(t.color("RetroDisplayForeground"), 65), 0.7f);
        }
        SkPathBuilder selected;
        for (int x = 0; x <= (int) r.w - 20; ++x)
        {
            const float y = r.bottom() - 12 - position * r.h * 0.6f - vector->waveform(position, (double) x / (r.w - 20)) * r.h * 0.17f;
            if (x == 0) selected.moveTo(r.x + position * 18, y); else selected.lineTo(r.x + (float) x + position * 18, y);
        }
        stroke(c, selected, t.color("ChosenDefault"));
        filter(c, t, scaled({256, 24, 158, 78}), V::Cutoff, V::Resonance, V::FilterType);
        if (page_ == 0) envelope(c, t, scaled({424, 24, 224, 78}), V::Attack, V::Decay, V::Sustain, V::Release);
        else
        {
            auto lfo = scaled({424, 24, 224, 59}); grid(c, t, lfo); SkPathBuilder curve;
            for (int x = 0; x < (int) lfo.w; ++x)
            {
                const float y = lfo.centreY() - (float) std::sin(4 * dsp::kPi * x / lfo.w) * lfo.h * 0.35f;
                if (x == 0) curve.moveTo(lfo.x, y); else curve.lineTo(lfo.x + (float) x, y);
            }
            stroke(c, curve, t.color("RetroDisplayForeground"));
        }
    }
    else if (kind_ == Kind::Impulse)
    {
        auto* impulse = dynamic_cast<I*>(processor_);
        for (int slot = 0; slot < 8; ++slot)
        {
            const auto* zone = impulse->zone(slot);
            static constexpr const char* notes[]{"C3", "D3", "E3", "F3", "G3", "A3", "B3", "C4"};
            paint::text(c, notes[slot], scaled({(float) slot * 65 + 2, 44, 59, 12}), t.color(zone ? "ControlForeground" : "TextDisabled"), t.metrics.smallFontSize, Align::Centre);
            if (impulse->activePads() & (1u << (unsigned) slot)) paint::fill(c, scaled({(float) slot * 65 + 4, 48, 4, 4}), t.color("ChosenDefault"));
        }
    }
    else if (kind_ == Kind::Gate)
    {
        const auto r = graphBounds(); grid(c, t, r);
        const float threshold = dsp::gainToDb(processor_->getParameter(G::Threshold));
        const float floor = processor_->getParameter(G::Floor); // dB, -75 == -inf
        SkPathBuilder response;
        for (int x = 0; x <= (int) r.w; ++x)
        {
            const float input = -80 + 80 * (float) x / r.w;
            const bool open = (input >= threshold) != (processor_->getParameter(G::Flip) > 0.5f);
            const float output = std::max(-80.0f, input + (open ? 0 : floor));
            const float y = r.bottom() - (output + 80) / 80 * r.h;
            if (x == 0) response.moveTo(r.x, y); else response.lineTo(r.x + (float) x, y);
        }
        stroke(c, response, t.color("RangeDefault"));
        paint::text(c, number(dynamic_cast<G*>(processor_)->reduction(), "%.1f dB"), {r.x + 2, r.y + 2, r.w - 4, 14}, t.color("RetroDisplayScaleText"), t.metrics.smallFontSize, Align::Right);
    }
    else if (kind_ == Kind::AutoPan)
    {
        const auto r = graphBounds(); grid(c, t, r);
        for (int ch = 0; ch < 2; ++ch)
        {
            SkPathBuilder wave;
            for (int x = 0; x <= (int) r.w; ++x)
            {
                const double phase = (double) x / r.w + processor_->getParameter(AP::Offset) / 360.0 + (ch == 1 ? processor_->getParameter(AP::Phase) / 360.0 : 0);
                const float modulation = AP::waveform(phase, (int) processor_->getParameter(AP::Shape)) * (processor_->getParameter(AP::Invert) > 0.5f ? -1 : 1) * (processor_->getParameter(AP::On) > 0.5f ? 1 : 0);
                const float y = r.centreY() - modulation * processor_->getParameter(AP::Amount) * r.h * 0.42f;
                if (x == 0) wave.moveTo(r.x, y); else wave.lineTo(r.x + (float) x, y);
            }
            stroke(c, wave, t.color(ch == 0 ? "RangeDefault" : "RetroDisplayForeground"));
        }
    }
    else if (kind_ == Kind::Redux)
    {
        const auto r = graphBounds(); grid(c, t, r);
        SkPathBuilder curve;
        const float levels = std::pow(2.0f, std::min(8.0f, processor_->getParameter(R::Bits)) - 1);
        for (int x = 0; x <= (int) r.w; ++x)
        {
            const float value = std::round(std::sin((float) x / r.w * 6.28f) * levels) / levels;
            const float y = r.centreY() - value * r.h * 0.42f;
            if (x == 0) curve.moveTo(r.x, y); else curve.lineTo(r.x + (float) x, y);
        }
        stroke(c, curve, t.color("RetroDisplayForeground"));
    }
    else if (kind_ == Kind::Tuner)
    {
        paint::fill(c, localBounds(), t.color("RetroDisplayBackground"));
        const auto r = graphBounds();
        const auto* tuner = dynamic_cast<T*>(processor_); const float hz = tuner->frequency();
        const float pitch = hz > 0 ? 69 + 12 * std::log2(hz / processor_->getParameter(T::Reference)) : 0;
        const int nearest = (int) std::lround(pitch); const float cents = (pitch - (float) nearest) * 100;
        const SkColor ink = std::abs(cents) < 5 ? SkColorSetRGB(100, 232, 127) : SkColorSetRGB(245, 110, 80);
        if (processor_->getParameter(T::ViewMode) > 0.5f)
        {
            for (int row = 0; row < 7; ++row) paint::line(c, r.x, r.y + 12 + (float) row * 16, r.right(), r.y + 12 + (float) row * 16, t.color("ControlContrastFrame"), 0.5f);
            SkPathBuilder path; bool first = true;
            for (size_t i = 0; i < history_.size(); ++i)
            {
                const float value = history_[(size_t) ((historyWrite_ + (int) i) % (int) history_.size())];
                if (value <= 0) { first = true; continue; }
                const float x = r.x + (float) i / (float) history_.size() * r.w;
                const float y = r.centreY() - (value - (float) nearest) * 16;
                if (first) path.moveTo(x, y); else path.lineTo(x, y); first = false;
            }
            c.save(); c.clipRect(SkRect::MakeXYWH(r.x, r.y, r.w, r.h)); stroke(c, path, ink); c.restore();
        }
        else
        {
            SkPathBuilder arc; arc.moveTo(r.x + 22, r.y + 72).quadTo(r.centreX(), r.y + 14, r.right() - 22, r.y + 72);
            stroke(c, arc, t.color("ControlContrastFrame"), 1);
            SkPaint ring; ring.setAntiAlias(true); ring.setStyle(SkPaint::kStroke_Style); ring.setStrokeWidth(1); ring.setColor(t.color("ControlContrastFrame"));
            c.drawCircle(r.centreX(), r.y + 43, 11, ring);
            if (hz > 0)
            {
                if (processor_->getParameter(T::Strobe) > 0.5f)
                {
                    for (int i = 0; i < 15; ++i)
                    {
                        const float x = r.x + std::fmod((float) i * 21 + (float) strobePhase_, r.w);
                        paint::fill(c, {x, r.y + 37, 6, 18}, ink);
                    }
                }
                else
                {
                    const float x = r.centreX() + std::clamp(cents / 50, -1.0f, 1.0f) * (r.w * 0.5f - 22);
                    ring.setColor(ink); c.drawCircle(x, r.y + 43 + (x - r.centreX()) * (x - r.centreX()) / (r.w * 2), 6, ring);
                }
            }
            for (int i = 0; i < 3; ++i) paint::text(c, i == 0 ? "-50" : i == 1 ? "0" : "+50", {r.x + (float) i * (r.w - 32) / 2, r.y + (i == 1 ? 8.0f : 78.0f), 32, 12}, t.color("RetroDisplayScaleText"), t.metrics.smallFontSize, Align::Centre);
        }
        if (hz > 0)
        {
            static constexpr const char* sharp[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
            static constexpr const char* flat[] = {"C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B"};
            const int note = (nearest % 12 + 12) % 12, spelling = (int) processor_->getParameter(T::Spelling);
            std::string label = spelling == 1 ? flat[note] : sharp[note];
            if (spelling == 2 && label != flat[note]) label += std::string("/") + flat[note];
            paint::text(c, label, {r.x, r.y + 66, r.w, 38}, ink, 27, Align::Centre);
            paint::text(c, processor_->getParameter(T::DisplayHz) > 0.5f ? number(hz, "%.1f Hz") : number(cents, "%+.1f ct"), {r.x, r.bottom() - 25, r.w, 22}, ink, t.metrics.fontSize, Align::Centre);
        }
    }
    else
    {
        auto* echo = dynamic_cast<E*>(processor_);
        const auto r = graphBounds(); grid(c, t, r);
        if (page_ == 0)
        {
            SkPaint pen; pen.setAntiAlias(true); pen.setStyle(SkPaint::kStroke_Style); pen.setStrokeWidth(1);
            const Rect tunnel{r.x, r.y, r.w, r.h * 0.62f};
            c.save(); c.clipRect(SkRect::MakeXYWH(tunnel.x, tunnel.y, tunnel.w, tunnel.h));
            for (int side = 0; side < 2; ++side)
            {
                c.save();
                c.clipRect(SkRect::MakeXYWH(tunnel.x + (float) side * tunnel.w * 0.5f, tunnel.y, tunnel.w * 0.5f, tunnel.h));
                const float spacing = (float) std::clamp(echo->delayMilliseconds(side) / 500, 0.2, 3.0);
                for (int ring = 0; ring < 15; ++ring)
                {
                    const float scale = std::exp(-(float) ring * 0.18f * spacing);
                    pen.setColor(paint::withAlpha(t.color("ChosenDefault"), (uint8_t) std::clamp(230 - ring * 12, 20, 230)));
                    c.drawOval(SkRect::MakeXYWH(tunnel.centreX() - tunnel.w * 0.49f * scale, tunnel.bottom() - tunnel.h * 0.98f * scale,
                                               tunnel.w * 0.98f * scale, tunnel.h * 1.96f * scale), pen);
                }
                c.restore();
            }
            c.restore();
            const Rect filterRect{r.x, tunnel.bottom() + 1, r.w, r.h * 0.38f - 1}; grid(c, t, filterRect);
            SkPathBuilder response;
            for (int x = 0; x <= (int) r.w; ++x)
            {
                const float hz = 20 * std::pow(1000.0f, (float) x / r.w);
                const float hp = processor_->getParameter(E::HighPass), lp = processor_->getParameter(E::LowPass);
                const float gain = processor_->getParameter(E::FilterOn) > 0.5f
                    ? 1 / std::sqrt((1 + std::pow(hp / hz, 4.0f)) * (1 + std::pow(hz / lp, 4.0f))) : 1;
                const float y = filterRect.bottom() - 4 - gain * (filterRect.h - 9);
                if (x == 0) response.moveTo(r.x, y); else response.lineTo(r.x + (float) x, y);
            }
            stroke(c, response, t.color("RangeDefault"));
        }
        else paint::text(c, page_ == 1 ? "Delay-time modulation" : "Feedback polarity / ducking", scaled({136, 28, 232, 35}), t.color("RetroDisplayScaleText"), t.metrics.fontSize, Align::Centre);
    }
}
void DeviceFace::edit(const std::function<void(model::Device&)>& operation, const std::string& label)
{
    if (! ctx().editDevice) return;
    const int chain = chain_, child = child_;
    ctx().editDevice(track_, device_, [operation, chain, child](auto& root) {
        if (chain < 0) operation(root);
        else if ((size_t) chain < root.chains.size() && child >= 0 && (size_t) child < root.chains[(size_t) chain].devices.size())
        { root.chainsEdited = true; operation(root.chains[(size_t) chain].devices[(size_t) child]); }
    }, label);
}
void DeviceFace::mouseDown(const MouseEvent& e)
{
    dragHandle_ = -1;
    if (e.right)
    {
        if (kind_ == Kind::Tuner)
        {
            auto pick = [this](int choice) { if (Widget::isAlive(this) && choice >= 0 && choice <= 2) { processor_->setParameter(T::Spelling, (float) choice); repaint(); } };
            const std::vector<std::string> labels{"Sharps", "Flats", "Sharps and Flats"};
            if (ctx().showChoices) ctx().showChoices(labels, {}, std::move(pick));
            else
            {
                juce::PopupMenu menu;
                for (size_t i = 0; i < labels.size(); ++i) menu.addItem((int) i + 1, labels[i]);
                menu.showMenuAsync(juce::PopupMenu::Options(), [selection = std::move(pick)](int choice) { if (choice > 0) selection(choice - 1); });
            }
        }
        return;
    }
    if ((kind_ == Kind::Simpler || kind_ == Kind::Sampler) && page_ == 0 && graphBounds().contains(e.x, e.y))
    {
        const auto* sampler = dynamic_cast<S*>(processor_);
        if (sampler->zones().empty()) return;
        const auto& z = sampler->zones()[0]; const auto* data = z.slot ? z.slot->get() : nullptr;
        if (! data || model_.sampleParts.empty()) return;
        const auto r = graphBounds();
        const float start = r.x + (float) z.part.sampleStart / (float) data->audio.getNumSamples() * r.w;
        const float end = r.x + (float) (z.part.sampleEnd > 0 ? z.part.sampleEnd : data->audio.getNumSamples()) / (float) data->audio.getNumSamples() * r.w;
        if (std::min(std::abs(e.x - start), std::abs(e.x - end)) <= 9) dragHandle_ = std::abs(e.x - start) < std::abs(e.x - end) ? 0 : 1;
    }
    else dragHandle_ = 2;
    mouseDrag(e);
}
void DeviceFace::mouseDrag(const MouseEvent& e)
{
    if (e.right || dragHandle_ < 0) return;
    const auto r = graphBounds();
    const float x = std::clamp((e.x - r.x) / std::max(1.0f, r.w), 0.0f, 1.0f);
    const float y = std::clamp(1 - (e.y - r.y) / std::max(1.0f, r.h), 0.0f, 1.0f);
    if (kind_ == Kind::Vector)
    {
        if (r.contains(e.x, e.y)) processor_->setParameter(V::Position1 + selected_, y);
        const auto filterRect = scaled({256, 24, 158, 78});
        if (filterRect.contains(e.x, e.y))
        {
            processor_->setParameter(V::Cutoff, 20 * std::pow(1000.0f, (e.x - filterRect.x) / filterRect.w));
            processor_->setParameter(V::Resonance, 1 - (e.y - filterRect.y) / filterRect.h);
        }
        const auto envelopeRect = scaled({424, 24, 224, 78});
        if (page_ == 0 && envelopeRect.contains(e.x, e.y))
        {
            const float amount = (e.x - envelopeRect.x) / envelopeRect.w;
            processor_->setParameter(V::Decay, 1 + amount * amount * 19999);
            processor_->setParameter(V::Sustain, 1 - (e.y - envelopeRect.y) / envelopeRect.h);
        }
    }
    else if ((kind_ == Kind::Simpler || kind_ == Kind::Sampler) && page_ == 0 && dragHandle_ < 2)
    {
        const auto& z = dynamic_cast<S*>(processor_)->zones()[0]; const auto* data = z.slot->get();
        const int64_t end = z.part.sampleEnd > 0 ? z.part.sampleEnd : data->audio.getNumSamples();
        dragFrame_ = std::clamp<int64_t>((int64_t) (x * (float) data->audio.getNumSamples()), dragHandle_ == 0 ? 0 : z.part.sampleStart + 1, dragHandle_ == 0 ? end - 1 : data->audio.getNumSamples());
    }
    else if (kind_ == Kind::Echo && page_ == 0 && r.contains(e.x, e.y))
    {
        const bool right = e.shift;
        processor_->setParameter(right ? E::SyncR : E::SyncL, 0);
        processor_->setParameter(right ? E::TimeR : E::TimeL, 1 + x * 3999); layout();
    }
    else if (kind_ == Kind::Fm && r.contains(e.x, e.y))
    {
        processor_->setParameter(F::oscParam(selected_, F::Decay), 1 + x * x * 19999);
        processor_->setParameter(F::oscParam(selected_, F::Sustain), y);
    }
    else if (kind_ == Kind::Sampler && page_ == 1)
    {
        const auto filterRect = scaled(Rect{0, 50, 246, 52});
        if (filterRect.contains(e.x, e.y))
        {
            processor_->setParameter(S::FilterFreq, 20 * std::pow(1000.0f, std::clamp(e.x / filterRect.w, 0.0f, 1.0f)));
            processor_->setParameter(S::FilterRes, std::clamp(1 - (e.y - filterRect.y) / filterRect.h, 0.0f, 1.0f));
        }
    }
    repaint();
}
void DeviceFace::mouseUp(const MouseEvent& e)
{
    const int handle = dragHandle_; dragHandle_ = -1;
    if (e.right || handle < 0 || handle > 1 || (kind_ != Kind::Simpler && kind_ != Kind::Sampler)) return;
    const int64_t frame = dragFrame_;
    // Structural edit may synchronously destroy this face. No member access after commit.
    edit([handle, frame](auto& d) {
        if (d.sampleParts.empty()) return;
        auto& z = d.sampleParts[0]; if (handle == 0) z.sampleStart = frame; else z.sampleEnd = frame;
        z.loopStart = std::max(z.loopStart, z.sampleStart); z.loopEnd = std::min(z.loopEnd, z.sampleEnd);
        if (z.loopEnd <= z.loopStart) z.loopMode = 0;
        d.samplePartsEdited = true;
    }, "Sample Boundary");
}
bool DeviceFace::browserDrop(const BrowserDrag& payload, const MouseEvent& e)
{
    if (payload.kind != BrowserDrag::Kind::Sample) return false;
    if (kind_ != Kind::Sampler && kind_ != Kind::Simpler && kind_ != Kind::Impulse) return false;
    int zone = 0;
    if (kind_ == Kind::Impulse) zone = e.y < scaled({0, 0, 0, 50}).h ? std::clamp((int) (e.x / 65), 0, 7) : selected_;
    if (ctx().dropDeviceItem) ctx().dropDeviceItem(payload, track_, device_, chain_, zone, child_);
    return true;
}
}
