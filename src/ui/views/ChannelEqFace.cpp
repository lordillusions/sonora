// DEV-30: Channel EQ (`ChannelEq`) matched to a 2000 x 231 capture of the reference device inside the "Ac Piano
// Upright" rack (default light theme): the device spans x 1068..1357, its body starts at y 29 and is 198 tall (0.83x the
// 239-px body of the UI-42 capture). Every box below is in that capture's pixels.
//
// Display (x 1072..1354, y 33..154): 20 Hz..20 kHz on a log axis (100 Hz at x 1137.5, 93.75 px per decade), 0 dB at
// y 80.5 with 3.146 px per dB (+-12 dB grid lines and labels), the live response curve over the output spectrum, then a
// bar under a rule at y 128.5 with the HP 80 Hz switch (amber, a high-pass glyph) and the Mid frequency readout, whose
// pointer marks the Mid knob below. Knobs Low / Mid / High, a rule, Output. Knobs that follow a rack macro are drawn as
// the reference draws them (grey ring, green dot) and ignore hand edits.

#include "ui/views/ReferenceFace.h"

#include "devices/native/ChannelEq.h"
#include "dsp/DeviceSpectrum.h"
#include "dsp/Dsp.h"
#include "gfx/Fonts.h"
#include "ui/Controls.h"
#include "ui/widgets/Knob.h"

#include "include/core/SkPaint.h"
#include "include/core/SkPathBuilder.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <set>

namespace sonora::ui
{
namespace
{
using E = devices::ChannelEq;
using Law = ReferenceFace::Law;

std::string decibels(float gain)
{
    char text[32];
    std::snprintf(text, sizeof(text), "%.1f dB", (double) (std::round(dsp::gainToDb(gain) * 10.0f) / 10.0f + 0.0f));
    return text;
}
std::string hertz(float hz)
{
    return hz >= 1000.0f ? ReferenceFace::significant(hz / 1000.0f) + " kHz" : ReferenceFace::significant(hz) + " Hz";
}
// A gain stored linear, shown and turned in dB (the knob's travel is dB-linear, like the reference's).
Law decibelLaw(float range)
{
    return {[=](float gain) { return (dsp::gainToDb(std::max(gain, 1e-6f)) + range) / (2 * range); },
            [=](float n) { return dsp::dbToGain(-range + 2 * range * n); }};
}

// The HP 80 Hz switch: an amber key with a high-pass glyph while on, a dark key with a grey glyph while off.
class HighpassSwitch final : public Widget
{
public:
    explicit HighpassSwitch(E* eq) : eq_(eq) { tooltip = "HP 80 Hz: high-pass filter"; }
    void paint(SkCanvas& c, const Theme& t) override
    {
        const auto r = localBounds();
        const bool on = eq_->getParameter(E::HighpassOn) > 0.5f;
        const auto scale = t.color("RetroDisplayScaleText");
        paint::fill(c, r, on ? t.color("ChosenDefault") : paint::withAlpha(scale, 40));
        // Measured glyph: from (4, 12) rising through a bend to (22, 3.5) in the 25 x 16.5 key.
        const float sx = r.w / 25.0f, sy = r.h / 16.5f;
        SkPathBuilder glyph;
        glyph.moveTo(r.x + 4.0f * sx, r.y + 12.5f * sy).quadTo(r.x + 9.5f * sx, r.y + 4.0f * sy, r.x + 22.0f * sx, r.y + 3.8f * sy);
        SkPaint pen;
        pen.setAntiAlias(true);
        pen.setStyle(SkPaint::kStroke_Style);
        pen.setStrokeWidth(std::max(1.0f, 1.5f * sy));
        pen.setStrokeCap(SkPaint::kRound_Cap);
        pen.setColor(on ? t.color("RetroDisplayBackground") : paint::withAlpha(scale, 150));
        c.drawPath(glyph.detach(), pen);
    }
    void mouseDown(const MouseEvent& e) override
    {
        if (e.right) return;
        eq_->setParameter(E::HighpassOn, eq_->getParameter(E::HighpassOn) > 0.5f ? 0.0f : 1.0f);
        repaint();
    }

private:
    E* eq_;
};

// The Mid frequency readout (amber, bold): drag or wheel on a log law over 120 Hz..7.5 kHz, double-click resets.
class FrequencyReadout final : public Widget
{
public:
    FrequencyReadout(E* eq, std::function<float()> size) : eq_(eq), size_(std::move(size)) { tooltip = "Mid frequency"; }
    void paint(SkCanvas& c, const Theme& t) override
    {
        paint::text(c, hertz(eq_->getParameter(E::MidFrequency)), localBounds(), t.color("ChosenDefault"), size_(), Align::Centre, true);
    }
    void mouseDown(const MouseEvent& e) override { pressed_ = ! e.right; startY_ = e.y; start_ = norm(); }
    void mouseDrag(const MouseEvent& e) override
    {
        if (pressed_) set(start_ + (startY_ - e.y) / (e.shift ? 1000.0f : 200.0f));
    }
    void mouseUp(const MouseEvent&) override { pressed_ = false; }
    void mouseDoubleClick(const MouseEvent& e) override
    {
        if (e.right) return;
        eq_->setParameter(E::MidFrequency, eq_->parameters()[E::MidFrequency].defaultValue);
        repaint();
    }
    bool mouseWheel(const MouseEvent& e) override
    {
        if (e.right || std::abs(e.wheelDeltaX) > std::abs(e.wheelDeltaY)) return false;
        set(norm() + (e.wheelDeltaY > 0 ? 1.0f : -1.0f) * (e.shift ? 0.002f : 0.02f));
        return true;
    }

private:
    static constexpr float kLow = 120.0f, kHigh = 7500.0f;
    float norm() const { return std::log(std::max(eq_->getParameter(E::MidFrequency), kLow) / kLow) / std::log(kHigh / kLow); }
    void set(float n)
    {
        eq_->setParameter(E::MidFrequency, kLow * std::pow(kHigh / kLow, std::clamp(n, 0.0f, 1.0f)));
        repaint();
    }
    E* eq_;
    std::function<float()> size_;
    float startY_ = 0, start_ = 0;
    bool pressed_ = false;
};

class ChannelEqFace final : public ReferenceFace
{
public:
    ChannelEqFace(E* eq, const model::Device& model, Tag tagger) : ReferenceFace(eq, 290, kLeft, kTop, std::move(tagger), kBody), eq_(eq)
    {
        // Parameters that follow one of the enclosing rack's macros (the rack's Tone macro maps both shelves).
        std::set<std::string> mapped;
        for (const auto& p : model.parameters)
            if (p.macroIndex >= 0) mapped.insert(p.path);
        const std::array<std::pair<int, float>, 4> knobs{{{E::LowShelfGain, 1111.5f}, {E::MidGain, 1169.5f}, {E::HighShelfGain, 1229}, {E::Gain, 1316}}};
        for (const auto& [parameter, centre] : knobs)
        {
            const float range = parameter == E::LowShelfGain || parameter == E::HighShelfGain ? 15.0f : 12.0f;
            auto* k = knob(parameter, eq->parameters()[(size_t) parameter].name, centre - 28, 163, centre + 28, 225.5f, decibelLaw(range), decibels);
            k->marker = 0.5f;
            k->markerGap = 16.0f; // measured: the ring's slot at 0 dB and a 9.5-unit triangle
            k->markerWidth = 9.5f;
            if (mapped.count(eq->parameters()[(size_t) parameter].id) > 0) k->mapped = [] { return true; };
        }
        auto* hp = add<HighpassSwitch>(eq);
        tag(hp, E::HighpassOn);
        place(hp, 1076.5f, 133.5f, 1101.5f, 150);
        auto* readout = add<FrequencyReadout>(eq, [this] { return text(); });
        tag(readout, E::MidFrequency);
        place(readout, 1136, 133.5f, 1201, 150);
    }

    Rect graphBounds() const override { return at(1072, 33, 1354, 128); }

    void tick() override
    {
        auto clip = boundsInRoot().sk();
        for (auto* widget = this->parent(); widget != nullptr; widget = widget->parent())
            if (! widget->visible || ! clip.intersect(widget->boundsInRoot().sk())) { lastTick_ = 0; return; }
        const double now = juce::Time::getMillisecondCounterHiRes() * 0.001;
        if (lastTick_ > 0 && now - lastTick_ < 0.030) return;
        const bool changed = spectrum_.update(eq_->displayTap(), lastTick_ > 0 ? now - lastTick_ : 1.0, true);
        lastTick_ = now;
        if (changed) repaint();
    }

    void paint(SkCanvas& c, const Theme& t) override
    {
        fillBody(c, t, false);
        paintDisplay(c, t);
        // Pointer from the readout down to the Mid knob, and the rule before Output.
        const auto tip = at(1169, 157.5f, 1169, 157.5f), base = at(1165, 153.5f, 1173, 153.5f);
        SkPathBuilder pointer;
        pointer.moveTo(base.x, base.y).lineTo(base.right(), base.y).lineTo(tip.x, tip.y).close();
        SkPaint fill;
        fill.setAntiAlias(true);
        fill.setColor(t.color("RetroDisplayBackground"));
        c.drawPath(pointer.detach(), fill);
        paint::fill(c, at(1271, 159, 1272.5f, 224.5f), t.color("TextDisabled"));
    }

    // Display mapping in capture pixels (measured).
    static constexpr float kLeft = 1068, kTop = 29, kBody = 198;
    static constexpr float kKiloX = 1231.75f, kDecade = 93.75f, kZeroY = 80.5f, kPerDb = 3.146f;

private:
    float xOf(float hz) const { return at(kKiloX + kDecade * std::log10(hz / 1000.0f), 0, 0, 0).x; }
    float yOf(float db) const { return at(0, kZeroY - kPerDb * db, 0, 0).y; }

    void updateCurve(Rect r)
    {
        const auto s = eq_->settings();
        const double rate = eq_->sampleRate();
        const std::array<double, 8> key{(double) s.highpass, s.lowDb, s.midDb, s.midHz, s.highDb, s.outputDb, rate, (double) r.w * 1e4 + r.h};
        if (key == curveKey_) return;
        curveKey_ = key;
        SkPathBuilder path;
        for (int i = 0; i <= (int) r.w; ++i)
        {
            const float px = r.x + (float) i;
            const float hz = 1000.0f * std::pow(10.0f, ((px / unit() + kLeft) - kKiloX) / kDecade);
            const float db = (float) juce::Decibels::gainToDecibels(E::response(s, hz, rate), -60.0);
            const float y = yOf(std::clamp(db, -30.0f, 30.0f));
            if (i == 0) path.moveTo(px, y); else path.lineTo(px, y);
        }
        curve_ = path.detach();
    }

    void paintDisplay(SkCanvas& c, const Theme& t)
    {
        displayPanel(c, t, 1072, 33, 1354, 154);
        const float u = unit();
        const auto scale = t.color("RetroDisplayScaleText");
        const auto r = graphBounds();
        c.save();
        c.clipRect(r.sk());
        // Output spectrum (DEV-2b's analyzer, as EQ Eight draws it): -90..0 dBFS over the graph's height.
        SkPathBuilder spectrum;
        spectrum.moveTo(r.x, r.bottom());
        const auto& values = spectrum_.curve();
        for (size_t i = 0; i < values.size(); ++i)
        {
            const float level = std::clamp((values[i] - dsp::SpectrumAnalyzer::kFloorDb) / -dsp::SpectrumAnalyzer::kFloorDb, 0.0f, 1.0f);
            spectrum.lineTo(xOf(dsp::SpectrumAnalyzer::curveFrequency((int) i)), r.bottom() - r.h * level);
        }
        spectrum.lineTo(r.right(), r.bottom());
        spectrum.close();
        SkPaint shade;
        shade.setAntiAlias(true);
        shade.setColor(paint::withAlpha(scale, 70));
        c.drawPath(spectrum.detach(), shade);
        // Grid: +12 / 0 / -12 dB right of their labels, decade lines above the frequency labels.
        const auto grid = paint::withAlpha(scale, 49);
        for (int db : {12, 0, -12})
            paint::fill(c, {at(1097, 0, 1097, 0).x, yOf((float) db) - 0.5f * u, r.right() - at(1097, 0, 1097, 0).x, u}, grid);
        for (float hz : {100.0f, 1000.0f, 10000.0f}) paint::fill(c, {xOf(hz) - 0.5f * u, r.y, u, at(0, 33, 0, 115).h}, grid);
        c.restore();
        const auto labels = paint::withAlpha(scale, 117);
        for (int db : {12, 0, -12})
            paint::text(c, std::to_string(db), {at(1072, 0, 1072, 0).x, yOf((float) db) - 7 * u, 22 * u, 14 * u}, labels, text(), Align::Right);
        for (const auto& [name, hz] : std::array<std::pair<const char*, float>, 3>{{{"100", 100}, {"1k", 1000}, {"10k", 10000}}})
            paint::text(c, name, {xOf(hz) - 20 * u, at(0, 115, 0, 129).y, 40 * u, 14 * u}, labels, text(), Align::Centre);
        paint::fill(c, at(1073, 128, 1353, 129.5f), paint::withAlpha(scale, 45)); // the bar's rule

        // Response.
        c.save();
        c.clipRect(r.sk());
        updateCurve(r);
        SkPaint pen;
        pen.setAntiAlias(true);
        pen.setStyle(SkPaint::kStroke_Style);
        pen.setStrokeWidth(1.8f * u);
        pen.setStrokeJoin(SkPaint::kRound_Join);
        pen.setColor(t.color("RangeDefault"));
        c.drawPath(curve_, pen);
        c.restore();
    }

    E* eq_;
    dsp::DeviceSpectrum spectrum_;
    double lastTick_ = 0;
    std::array<double, 8> curveKey_{};
    SkPath curve_;
};
} // namespace

ReferenceFace* addChannelEqFace(Widget& parent, const model::Device& model, devices::DeviceProcessor* processor, ReferenceFace::Tag tag)
{
    if (auto* eq = dynamic_cast<E*>(processor)) return parent.add<ChannelEqFace>(eq, model, std::move(tag));
    return nullptr;
}
} // namespace sonora::ui
