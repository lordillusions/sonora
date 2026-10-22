// UI-42: EQ Eight, EQ Three, Compressor and Limiter faces matched to a capture of the reference's Live 12 devices
// (2000 x 278). Every box below is in that capture's
// pixels: the device bodies start at y 33 and are 239 tall. Controls the processors do not implement are drawn
// disabled (with a tooltip) instead of pretending to work.

#include "ui/views/ReferenceFace.h"

#include "devices/native/Bands.h"
#include "devices/native/Compressor.h"
#include "devices/native/EqEight.h"
#include "devices/native/Limiter.h"
#include "dsp/Dsp.h"
#include "gfx/Fonts.h"
#include "ui/Controls.h"
#include "ui/UiContext.h"
#include "ui/widgets/Buttons.h"
#include "ui/widgets/Knob.h"

#include "include/core/SkPaint.h"
#include "include/core/SkPathBuilder.h"

#include <array>
#include <cmath>
#include <cstdio>

namespace sonora::ui
{
namespace
{
constexpr float kTop = 33; // capture y of the device bodies
using Law = ReferenceFace::Law;

std::string printed(float value, const char* format)
{
    char text[48];
    std::snprintf(text, sizeof(text), format, (double) value);
    return text;
}
std::string decibels(float db) { return ReferenceFace::significant(db) + " dB"; }
SkColor gridInk(const Theme& t, uint8_t alpha) { return paint::withAlpha(t.color("RetroDisplayScaleText"), alpha); }

// A gain in linear amplitude shown on a dB scale; the bottom of the travel is -inf (EQ Three's kill).
Law gainLaw(float lowDb, float highDb)
{
    return {[=](float gain) { return gain <= 0.0000317f ? 0.0f : (dsp::gainToDb(gain) - lowDb) / (highDb - lowDb); },
            [=](float n) { return n <= 0.002f ? 0.0f : dsp::dbToGain(lowDb + n * (highDb - lowDb)); }};
}

// ================================================================ EQ Eight
// Small switch under each band chooser: cyan square while the band is on, then the band number. Selects the band.
class BandSwitch final : public Widget
{
public:
    BandSwitch(devices::EqEight* eq, int band, std::function<bool()> setB, std::function<void(int)> select, std::function<float()> size)
        : eq_(eq), band_(band), setB_(std::move(setB)), select_(std::move(select)), size_(std::move(size)) {}
    void paint(SkCanvas& c, const Theme& t) override
    {
        using E = devices::EqEight;
        const bool on = eq_->getParameter(E::bandParamSet(setB_(), band_, E::On)) > 0.5f;
        const float h = height(), side = h;
        const Rect box{0, 0, side, side};
        ctl::framed(c, t, box, on ? t.color("RangeDefault") : t.color("ControlTextBack"));
        paint::text(c, std::to_string(band_ + 1), {side, 0, width() - side, h}, t.color("ControlForeground"), size_(), Align::Centre);
    }
    void mouseUp(const MouseEvent& e) override
    {
        using E = devices::EqEight;
        if (e.right || ! localBounds().contains(e.x, e.y)) return;
        const int index = E::bandParamSet(setB_(), band_, E::On);
        if (e.x < height()) eq_->setParameter(index, eq_->getParameter(index) > 0.5f ? 0.0f : 1.0f);
        select_(band_);
    }

private:
    devices::EqEight* eq_;
    int band_;
    std::function<bool()> setB_;
    std::function<void(int)> select_;
    std::function<float()> size_;
};

class EqEightFace final : public ReferenceFace
{
public:
    using E = devices::EqEight;
    EqEightFace(E* eq, Tag tagger) : ReferenceFace(eq, 675, 10, kTop, std::move(tagger)), eq_(eq)
    {
        // Selected band: Freq / Gain / Q in the left column (label rows centred at 47, 126.5, 213).
        const auto param = [this](E::BandParam which) { return [this, which] { return E::bandParamSet(setB_, band_, which); }; };
        bandKnobs_[0] = knob(param(E::Freq), "Freq", 14, 40, 79, 103.5f, Law::logarithmic(10, 22000));
        bandKnobs_[1] = knob(param(E::GainDb), "Gain", 14, 119.5f, 79, 191, Law::linear(-15, 15));
        bandKnobs_[1]->marker = 0.5f;
        bandKnobs_[2] = knob(param(E::Q), "Q", 14, 206, 79, 269.5f, Law::logarithmic(0.1f, 18));
        for (int b = 0; b < E::kBands; ++b)
        {
            const float x = cellX(b);
            types_[(size_t) b] = chooser([this, b] { return E::bandParamSet(setB_, b, E::Type); }, x + 5, 227, x + 57, 245.6f,
                    [](SkCanvas& c, const Theme&, Rect r, int type, SkColor ink) { eqBandIcon(c, r, type, ink, std::max(1.0f, r.h * 0.16f)); });
            switches_[(size_t) b] = add<BandSwitch>(eq, b, [this] { return setB_; }, [this](int band) { selectBand(band); }, [this] { return text(); });
            place(switches_[(size_t) b], x + 15, 250, x + 47, 265);
        }
        // Right column.
        inertButton("", false, 614, 41, 643, 58, "Band audition is not implemented yet (UI-42b)");
        inertButton("", false, 649, 41, 678, 58, "The spectrum analyzer is not implemented yet (DEV-2b)");
        chooser(E::kMode, 614, 80, 678, 98);
        edit_ = button("L", 614, 120, 678, 137, [this] { setB_ = ! setB_; retag(); repaint(); });
        edit_->isEnabled = [this] { return mode() != 0; };
        adaptive_ = toggle(E::kAdaptiveQ, "On", 614, 161, 678, 178);
        slider(E::kScale, 614, 201, 678, 218, Law::linear(-1.5f, 1.5f));
        slider(E::kGlobalGain, 614, 243, 678, 260, Law::linear(-12, 12));
        retag();
    }

    int selectedBand() const { return band_; }
    void selectBand(int band)
    {
        band_ = std::clamp(band, 0, E::kBands - 1);
        retag();
        repaint();
    }
    Rect graphBounds() const override { return at(83, 33, 607, 220); }

    void paint(SkCanvas& c, const Theme& t) override
    {
        if (mode() == 0 && setB_) { setB_ = false; retag(); }
        edit_->setLabel(mode() == 2 ? (setB_ ? "S" : "M") : (setB_ ? "R" : "L"));
        adaptive_->setLabel(eq_->getParameter(E::kAdaptiveQ) > 0.5f ? "On" : "Off");
        fillBody(c, t, true);
        panel(c, t, 13, 33, 80, 269);
        panel(c, t, 610, 33, 682, 269);
        for (int b = 0; b < E::kBands; ++b) panel(c, t, cellX(b), 223, cellX(b) + 63, 269);
        for (const auto& [text, y] : std::array<std::pair<const char*, float>, 5>{{{"Mode", 64}, {"Edit", 98}, {"Adapt. Q", 144}, {"Scale", 184}, {"Output", 226}}})
            label(c, t, text, 614, y, 682, y + 15, Align::Left, true);
        paintDisplay(c, t);
    }
    void paintOverChildren(SkCanvas& c, const Theme& t) override { paintSwitchGlyphs(c, t); }

    void mouseDown(const MouseEvent& e) override
    {
        dragBand_ = -1;
        if (e.right || ! graphBounds().contains(e.x, e.y)) return;
        float best = 14 * unit();
        for (int b = 0; b < E::kBands; ++b)
        {
            if (! bandOn(b)) continue;
            const auto h = handle(b);
            if (const float d = std::hypot(h[0] - e.x, h[1] - e.y); d < best) { best = d; dragBand_ = b; }
        }
        if (dragBand_ < 0) return;
        if (e.clicks >= 2) // double-click a handle: band off, as in the reference
        {
            eq_->setParameter(E::bandParamSet(setB_, dragBand_, E::On), 0.0f);
            dragBand_ = -1;
            repaint();
            return;
        }
        selectBand(dragBand_);
        startY_ = e.y;
        startQ_ = eq_->getParameter(E::bandParamSet(setB_, dragBand_, E::Q));
    }
    void mouseDoubleClick(const MouseEvent& e) override
    {
        auto twice = e;
        twice.clicks = 2;
        mouseDown(twice);
    }
    void mouseDrag(const MouseEvent& e) override
    {
        if (dragBand_ < 0 || e.right) return;
        const auto r = graphBounds();
        if (e.alt) // Q: drag up for a narrower band
            eq_->setParameter(E::bandParamSet(setB_, dragBand_, E::Q), std::clamp(startQ_ * std::pow(18.0f / 0.1f, (startY_ - e.y) / (r.h * 2)), 0.1f, 18.0f));
        else
        {
            const float n = std::clamp((e.x - r.x) / r.w, 0.0f, 1.0f);
            eq_->setParameter(E::bandParamSet(setB_, dragBand_, E::Freq), 10.0f * std::pow(2200.0f, n));
            if (hasGain(dragBand_))
                eq_->setParameter(E::bandParamSet(setB_, dragBand_, E::GainDb), std::clamp((r.centreY() - e.y) / dbPixels(), -15.0f, 15.0f));
        }
        repaint();
    }

private:
    static float cellX(int band) { return 83 + 65.75f * (float) band; }
    int mode() const { return (int) std::lround(eq_->getParameter(E::kMode)); }
    bool bandOn(int b) const { return eq_->getParameter(E::bandParamSet(setB_, b, E::On)) > 0.5f; }
    bool hasGain(int b) const
    {
        const int type = (int) std::lround(eq_->getParameter(E::bandParamSet(setB_, b, E::Type)));
        return type == 2 || type == 3 || type == 5;
    }
    float dbPixels() const { return graphBounds().h / 30.0f; } // the display spans +-15 dB
    float freqX(float hz) const
    {
        const auto r = graphBounds();
        return r.x + r.w * std::log(std::max(hz, 10.0f) / 10.0f) / std::log(2200.0f);
    }
    std::array<float, 2> handle(int b) const
    {
        const auto r = graphBounds();
        const float gain = hasGain(b) ? eq_->getParameter(E::bandParamSet(setB_, b, E::GainDb)) : 0.0f;
        return {freqX(eq_->getParameter(E::bandParamSet(setB_, b, E::Freq))), r.centreY() - gain * dbPixels()};
    }
    // MIDI mapping follows the selected band and the edited set (L/R, M/S).
    void retag()
    {
        for (int i = 0; i < 3; ++i)
            tag(bandKnobs_[(size_t) i], E::bandParamSet(setB_, band_, i == 0 ? E::Freq : i == 1 ? E::GainDb : E::Q));
        for (int b = 0; b < E::kBands; ++b)
        {
            tag(types_[(size_t) b], E::bandParamSet(setB_, b, E::Type));
            tag(switches_[(size_t) b], E::bandParamSet(setB_, b, E::On));
        }
    }
    void paintSwitchGlyphs(SkCanvas& c, const Theme& t) const
    {
        // Headphones and spectrum glyphs on the two disabled buttons.
        const auto ink = t.color("TextDisabled");
        const float u = unit();
        SkPaint pen;
        pen.setAntiAlias(true);
        pen.setStyle(SkPaint::kStroke_Style);
        pen.setStrokeWidth(1.6f * u);
        pen.setColor(ink);
        const auto phones = at(614, 41, 643, 58);
        const float cx = phones.centreX(), cy = phones.centreY(), s = 5.5f * u;
        c.drawArc(SkRect::MakeLTRB(cx - s, cy - s, cx + s, cy + s), 180, 180, false, pen);
        paint::fill(c, {cx - s - 0.8f * u, cy, 2.6f * u, 4.5f * u}, ink);
        paint::fill(c, {cx + s - 1.8f * u, cy, 2.6f * u, 4.5f * u}, ink);
        const auto bars = at(649, 41, 678, 58);
        const float heights[] = {0.35f, 0.75f, 0.55f, 0.9f, 0.5f, 0.3f, 0.45f};
        for (int i = 0; i < 7; ++i)
        {
            const float h = bars.h * 0.55f * heights[i];
            paint::fill(c, {bars.x + bars.w * 0.2f + (float) i * bars.w * 0.09f, bars.y + bars.h * 0.78f - h, 1.6f * u, h}, ink);
        }
    }
    void paintDisplay(SkCanvas& c, const Theme& t)
    {
        displayPanel(c, t, 83, 33, 607, 220);
        const auto r = graphBounds();
        const float u = unit(), zero = r.centreY();
        // Grid: every 6 dB, frequency lines per decade step (decades stronger), labels inside the display.
        const float gridLeft = at(118, 0, 118, 0).x;
        for (int db = -12; db <= 12; db += 6)
            paint::line(c, gridLeft, zero - (float) db * dbPixels(), r.right(), zero - (float) db * dbPixels(), gridInk(t, 38), u);
        for (float decade = 10; decade <= 10000; decade *= 10)
            for (int step = 1; step <= 9; ++step)
            {
                const float hz = decade * (float) step;
                if (hz < 20 || hz > 22000) continue;
                paint::line(c, freqX(hz), r.y, freqX(hz), r.bottom(), gridInk(t, step == 1 ? 55 : 24), u);
            }
        const auto scale = paint::withAlpha(t.color("RetroDisplayScaleText"), 150);
        for (int db = -12; db <= 12; db += 6)
        {
            const float y = zero - (float) db * dbPixels();
            paint::text(c, std::to_string(db), {at(86, 0, 86, 0).x, y - 8 * u, 24 * u, 16 * u}, scale, text(), Align::Right);
        }
        for (const auto& [name, hz] : std::array<std::pair<const char*, float>, 3>{{{"100", 100}, {"1k", 1000}, {"10k", 10000}}})
            paint::text(c, name, {freqX(hz) - 20 * u, at(0, 205, 0, 219).y, 40 * u, 14 * u}, scale, text(), Align::Centre);

        // Response, then the handles of the enabled bands (selected one filled).
        SkPathBuilder curve;
        for (float x = 0; x <= r.w; x += 1.0f)
        {
            const double hz = 10.0 * std::pow(2200.0, x / r.w);
            const float db = (float) juce::Decibels::gainToDecibels(eq_->responseAt(hz, setB_), -60.0);
            const float y = std::clamp(zero - db * dbPixels(), r.y, r.bottom());
            if (x <= 0) curve.moveTo(r.x + x, y); else curve.lineTo(r.x + x, y);
        }
        SkPaint pen;
        pen.setAntiAlias(true);
        pen.setStyle(SkPaint::kStroke_Style);
        pen.setStrokeWidth(2.2f * u);
        pen.setColor(t.color("RangeDefault"));
        c.save();
        c.clipRect(r.sk());
        c.drawPath(curve.detach(), pen);
        const auto amber = t.color("ChosenDefault");
        for (int b = 0; b < E::kBands; ++b)
        {
            if (! bandOn(b)) continue;
            const auto h = handle(b);
            const float radius = 7.5f * u;
            SkPaint dot;
            dot.setAntiAlias(true);
            dot.setColor(b == band_ ? amber : t.color("RetroDisplayBackground"));
            c.drawCircle(h[0], h[1], radius, dot);
            dot.setStyle(SkPaint::kStroke_Style);
            dot.setStrokeWidth(2 * u);
            dot.setColor(amber);
            c.drawCircle(h[0], h[1], radius, dot);
            paint::text(c, std::to_string(b + 1), {h[0] - radius, h[1] - radius, radius * 2, radius * 2},
                        b == band_ ? t.color("RetroDisplayBackground") : amber, 10.5f * u, Align::Centre, true);
        }
        c.restore();
    }

    E* eq_;
    std::array<Knob*, 3> bandKnobs_{};
    std::array<Widget*, E::kBands> types_{}, switches_{};
    TextButton *edit_ = nullptr, *adaptive_ = nullptr;
    int band_ = 2, dragBand_ = -1;
    bool setB_ = false;
    float startY_ = 0, startQ_ = 1;
};

// ================================================================ EQ Three
class EqThreeFace final : public ReferenceFace
{
public:
    using Q = devices::EqThree;
    explicit EqThreeFace(Q* eq, Tag tagger) : ReferenceFace(eq, 210, 698, kTop, std::move(tagger))
    {
        const std::array<float, 3> centre{738, 804, 870};
        const char* names[] = {"GainLow", "GainMid", "GainHi"};
        const char* letters[] = {"L", "M", "H"};
        for (int band = 0; band < 3; ++band)
        {
            knob(Q::GainLow + band, names[band], centre[(size_t) band] - 33, 57, centre[(size_t) band] + 33, 120.5f, gainLaw(-54, 6));
            toggle(Q::LowOn + band, letters[band], centre[(size_t) band] - 20, 129, centre[(size_t) band] + 20, 147.5f);
        }
        knob(Q::FreqLow, "FreqLow", 705, 198, 771, 261.5f, Law::logarithmic(50, 500));
        knob(Q::FreqHigh, "FreqHi", 837, 198, 903, 261.5f, Law::logarithmic(500, 18000));
        option(Q::Slope, 0, "24", 784, 205, 822, 222.5f);
        option(Q::Slope, 1, "48", 784, 227, 822, 245);
    }
    void paint(SkCanvas& c, const Theme& t) override
    {
        fillBody(c, t, false);
        for (float x : {738.0f, 804.0f, 870.0f}) led(c, t, x, 44, 6, false); // band activity LEDs (not metered yet)
    }
};

// ================================================================ Compressor
// Display legend switch: a small square (filled while shown) and its name.
class LegendSwitch final : public Widget
{
public:
    LegendSwitch(std::string name, bool* on, std::function<float()> size) : name_(std::move(name)), on_(on), size_(std::move(size)) {}
    void paint(SkCanvas& c, const Theme& t) override
    {
        const float side = height() * 0.55f;
        const Rect box{0.5f, (height() - side) * 0.5f, side, side};
        const auto cyan = t.color("RangeDefault");
        if (*on_) paint::fill(c, box, cyan);
        else paint::stroke(c, box, t.color("RetroDisplayScaleText"), std::max(1.0f, side * 0.14f));
        paint::text(c, name_, {side * 1.9f, 0, width() - side * 1.9f, height()}, t.color("RetroDisplayScaleText"), size_());
    }
    void mouseUp(const MouseEvent& e) override
    {
        if (e.right || ! localBounds().contains(e.x, e.y)) return;
        *on_ = ! *on_;
        repaint();
    }

private:
    std::string name_;
    bool* on_;
    std::function<float()> size_;
};

// Display footer view button (Collapsed / Transfer curve / Activity) and the Env. Lin/Log switch.
class DisplayIconButton final : public TextButton
{
public:
    enum class Glyph { Bars, Curve, Activity, LogBox };
    DisplayIconButton(Glyph glyph, std::function<float()> size) : TextButton("", Style::Plain), glyph_(glyph), size_(std::move(size)) {}
    void paint(SkCanvas& c, const Theme& t) override
    {
        const bool on = isOn && isOn(), enabled = ! isEnabled || isEnabled();
        const auto r = localBounds();
        if (glyph_ == Glyph::LogBox)
        {
            const auto cyan = t.color("RangeDefault");
            if (on) paint::fill(c, r, cyan);
            else paint::stroke(c, r.reduced(0.5f), cyan, 1);
            paint::text(c, on ? "Log" : "Lin", r, on ? t.color("RetroDisplayBackground") : cyan, size_(), Align::Centre);
            return;
        }
        const auto ink = ! enabled ? paint::withAlpha(t.color("RetroDisplayScaleText"), 70)
                         : on ? t.color("ChosenDefault") : t.color("RetroDisplayScaleText");
        const float cx = r.centreX(), cy = r.centreY(), s = r.h * 0.22f;
        SkPaint pen;
        pen.setAntiAlias(true);
        pen.setStyle(SkPaint::kStroke_Style);
        pen.setStrokeWidth(std::max(1.0f, r.h * 0.05f));
        pen.setColor(ink);
        if (glyph_ == Glyph::Bars)
            for (int i = -1; i <= 1; ++i) paint::fill(c, {cx + (float) i * s * 0.75f - 0.6f, cy - s * (i == 0 ? 1.0f : 0.55f) + s * 0.4f, 1.3f, s * (i == 0 ? 1.6f : 1.15f)}, ink);
        else if (glyph_ == Glyph::Curve)
        {
            SkPathBuilder path;
            path.moveTo(cx - s, cy + s).lineTo(cx - s * 0.1f, cy - s * 0.5f).lineTo(cx + s, cy - s * 0.85f);
            c.drawPath(path.detach(), pen);
        }
        else
        {
            paint::line(c, cx - s * 1.3f, cy, cx + s * 1.3f, cy, ink, pen.getStrokeWidth());
            for (int i = -2; i <= 2; ++i) paint::line(c, cx + (float) i * s * 0.55f, cy - s * 0.8f, cx + (float) i * s * 0.55f, cy + s * 0.8f, ink, pen.getStrokeWidth());
        }
    }

private:
    Glyph glyph_;
    std::function<float()> size_;
};

// The sidechain strip's round fold switch (down: section shown) and its disabled listen switch.
class StripButton final : public TextButton
{
public:
    explicit StripButton(bool fold) : TextButton("", Style::Plain), fold_(fold) {}
    void paint(SkCanvas& c, const Theme& t) override
    {
        const bool enabled = ! isEnabled || isEnabled();
        const auto ink = t.color(enabled ? "ControlForeground" : "TextDisabled");
        const float d = std::min(width(), height()), cx = width() * 0.5f, cy = height() * 0.5f;
        SkPaint pen;
        pen.setAntiAlias(true);
        pen.setColor(t.color("ControlTextBack"));
        c.drawCircle(cx, cy, d * 0.45f, pen);
        pen.setStyle(SkPaint::kStroke_Style);
        pen.setStrokeWidth(d * 0.09f);
        pen.setColor(ink);
        c.drawCircle(cx, cy, d * 0.45f, pen);
        if (fold_) ctl::triangle(c, cx, cy + d * 0.03f, d * 0.42f, d * 0.32f, isOn && isOn() ? ctl::Dir::Down : ctl::Dir::Right, ink);
        else
        {
            const float s = d * 0.22f;
            c.drawArc(SkRect::MakeLTRB(cx - s, cy - s, cx + s, cy + s), 180, 180, false, pen);
            paint::fill(c, {cx - s - d * 0.04f, cy, d * 0.12f, d * 0.2f}, ink);
            paint::fill(c, {cx + s - d * 0.08f, cy, d * 0.12f, d * 0.2f}, ink);
        }
    }

private:
    bool fold_;
};

class CompressorFace final : public ReferenceFace
{
public:
    using C = devices::Compressor;
    enum class View { Collapsed, Transfer, Activity };
    CompressorFace(C* compressor, const model::Device& model, Tag tagger)
        : ReferenceFace(compressor, 657, 921, kTop, std::move(tagger)), compressor_(compressor), model_(model)
    {
        animated_ = true;
        for (auto& h : history_) h = {-120.0f, 0.0f, -120.0f};
        collapseColumns(951, 1034, true); // the sidechain section starts folded
        // Sidechain strip and section. Sonora has no sidechain filter yet (DEV-7b): those controls are disabled.
        auto* fold = add<StripButton>(true);
        fold->isOn = [this] { return ! collapsed_; };
        fold->onClick = [this] { collapseColumns(951, 1034, ! collapsed_); relayoutChain(); };
        place(fold, 929, 39, 945.5f, 55.5f);
        auto* listen = add<StripButton>(false);
        listen->isEnabled = [] { return false; };
        listen->tooltip = "Sidechain listen is not implemented yet (DEV-7b)";
        place(listen, 929, 61, 945.5f, 77);
        const std::string why = "The sidechain filter is not implemented yet (DEV-7b)";
        inertButton("SC Filter", false, 957.5f, 38, 1029, 55.5f, why);
        inertKnob("Freq", 0.13f, "80.0 Hz", 960, 137, 1025, 200.5f);

        // Ratio / Attack / Release column (label rows centred at 46, 115, 185). Expand uses the expansion ratio.
        const auto expand = [compressor] { return std::lround(compressor->getParameter(C::Model)) == 2; };
        auto* ratio = knob(C::Ratio, "Ratio", 1046, 39, 1112, 102.5f,
                           {[](float r) { return 1.0f - std::pow(std::max(r, 1.0f), -1.16f); },
                            [](float n) { return std::min(100.0f, std::pow(std::max(1.0f - n, 1e-6f), -1.0f / 1.16f)); }},
                           [](float r) { return r >= 99.99f ? std::string("inf : 1") : significant(r) + " : 1"; });
        showWhen(ratio, [expand] { return ! expand(); });
        auto* expansion = knob(C::ExpansionRatio, "Ratio", 1046, 39, 1112, 102.5f, Law::linear(1, 2),
                               [](float r) { return "1 : " + significant(r); });
        showWhen(expansion, expand);
        knob(C::Attack, "Attack", 1046, 108, 1112, 171.5f, Law::logarithmic(0.01f, 1000));
        knob(C::Release, "Release", 1046, 178, 1112, 241.5f, Law::power(1, 3000, 0.5f));
        toggle(C::AutoRelease, "Auto", 1043, 246, 1114, 264);

        // Display header and footer.
        const auto thresholdLaw = Law{[](float g) { return (dsp::gainToDb(g) + 60) / 60; }, [](float n) { return dsp::dbToGain(n * 60 - 60); }};
        displayValue(C::Threshold, "Thresh", 1132, 37, 1262, 55, thresholdLaw, [](float g) { return decibels(dsp::gainToDb(g)); });
        place(add<LegendSwitch>("GR", &showReduction_, [this] { return text(); }), 1265.5f, 37, 1305, 55);
        place(add<LegendSwitch>("Output", &showOutput_, [this] { return text(); }), 1309, 37, 1380, 55);
        displayValue(C::Gain, "Out", 1392, 37, 1486, 55, Law::linear(-36, 36), decibels);
        const DisplayIconButton::Glyph glyphs[] = {DisplayIconButton::Glyph::Bars, DisplayIconButton::Glyph::Curve, DisplayIconButton::Glyph::Activity};
        for (int i = 0; i < 3; ++i)
        {
            auto* view = add<DisplayIconButton>(glyphs[i], [this] { return text(); });
            view->isOn = [this, i] { return (int) view_ == i; };
            if (i == 0)
            {
                view->isEnabled = [] { return false; };
                view->tooltip = "The collapsed display is not implemented yet (UI-42b)";
            }
            else view->onClick = [this, i] { view_ = (View) i; repaint(); };
            place(view, 1122 + 28.5f * (float) i, 243, 1150 + 28.5f * (float) i, 269);
        }
        displayValue(C::Knee, "Knee", 1217, 248, 1305, 264, Law::linear(0, 18), [](float db) { return printed(db, "%.1f dB"); });
        displayValue(C::LookAhead, "Look.", 1309, 248, 1408, 264, Law::linear(0, 2), {}, true);
        auto* env = add<DisplayIconButton>(DisplayIconButton::Glyph::LogBox, [this] { return text(); });
        env->isOn = [compressor] { return compressor->getParameter(C::LogEnvelope) > 0.5f; };
        env->onClick = [compressor] { compressor->setParameter(C::LogEnvelope, compressor->getParameter(C::LogEnvelope) > 0.5f ? 0.0f : 1.0f); };
        tag(env, C::LogEnvelope);
        place(env, 1443, 249, 1479, 263);

        // Right column.
        toggle(C::GainCompensation, "Makeup", 1498, 38, 1569, 56);
        option(C::Model, 0, "Peak", 1498, 96, 1569, 113);
        option(C::Model, 1, "RMS", 1498, 118, 1569, 136);
        option(C::Model, 2, "Expand", 1498, 141, 1569, 158);
        knob(C::DryWet, "Dry/Wet", 1500, 199, 1567, 262.5f, Law::linear(0, 1));
    }

    Rect graphBounds() const override { return at(1122, 60, 1490, 242); }
    View view() const { return view_; }

    void tick() override
    {
        history_[(size_t) write_] = {compressor_->inputPeakDb(), compressor_->gainReductionDb(), compressor_->outputPeakDb()};
        write_ = (write_ + 1) % (int) history_.size();
        ReferenceFace::tick();
    }

    void paint(SkCanvas& c, const Theme& t) override
    {
        fillBody(c, t, true);
        panel(c, t, 924, 33, 1034, 269);
        panel(c, t, 1037, 33, 1120, 269);
        panel(c, t, 1493, 33, 1575, 269);
        const float u = unit();
        // Sidechain strip: divider, vertical caption and the activity LED (lit while an external input is routed).
        const auto divider = at(950, 37, 951, 265);
        paint::fill(c, divider, t.color("TextDisabled"));
        const auto caption = at(929, 175, 946, 248);
        c.save();
        c.translate(caption.centreX(), caption.centreY());
        c.rotate(-90);
        paint::text(c, "Sidechain", {-caption.h * 0.5f, -caption.w * 0.5f, caption.h, caption.w}, t.color("ControlForeground"), text(), Align::Centre);
        c.restore();
        led(c, t, 937, 255.5f, 5, model_.sidechain.routed());
        if (! collapsed_)
        {
            const auto dim = t.color("TextDisabled");
            label(c, t, "Type", 957, 77, 1029, 89, Align::Centre, false, dim);
            const auto type = at(957.5f, 93, 1029, 110);
            ctl::dropdown(c, t, type, "", false, text());
            eqBandIcon(c, {type.x + type.h * 0.16f, type.y + type.h * 0.24f, type.h * 1.3f, type.h * 0.52f}, 1, dim, std::max(1.0f, type.h * 0.1f));
            label(c, t, "Res", 957, 232, 1029, 242, Align::Centre, false, dim);
            const auto res = at(957.5f, 246, 1029, 264);
            ctl::framed(c, t, res, t.color("SurfaceHighlight"));
            paint::text(c, "0.71", res, dim, text(), Align::Centre);
        }
        paintDisplay(c, t, u);
    }

    void mouseDown(const MouseEvent& e) override
    {
        dragging_ = ! e.right && graphBounds().contains(e.x, e.y) && view_ != View::Collapsed;
        if (dragging_) mouseDrag(e);
    }
    void mouseDrag(const MouseEvent& e) override
    {
        if (! dragging_) return;
        const auto r = graphBounds();
        float db;
        if (view_ == View::Transfer) db = -60 + 60 * std::clamp((e.x - r.x) / r.w, 0.0f, 1.0f);
        else db = std::clamp(dbAt(e.y), -60.0f, 0.0f);
        compressor_->setParameter(C::Threshold, dsp::dbToGain(db));
        repaint();
    }
    void mouseUp(const MouseEvent&) override { dragging_ = false; }

private:
    static constexpr float kTopDb = 6.5f, kBottomDb = -36.0f; // activity scale: 0 dB sits at capture y 88
    float yOf(float db) const
    {
        const auto r = graphBounds();
        return r.y + r.h * (kTopDb - std::clamp(db, kBottomDb, kTopDb)) / (kTopDb - kBottomDb);
    }
    float dbAt(float y) const
    {
        const auto r = graphBounds();
        return kTopDb - (y - r.y) / r.h * (kTopDb - kBottomDb);
    }
    void paintDisplay(SkCanvas& c, const Theme& t, float u)
    {
        displayPanel(c, t, 1122, 33, 1490, 269);
        const auto rule = gridInk(t, 40);
        const auto head = at(1122, 59, 1490, 60), foot = at(1122, 242, 1490, 243);
        paint::fill(c, {head.x, head.y, head.w, u}, rule);
        paint::fill(c, {foot.x, foot.y, foot.w, u}, rule);
        for (float x : {1151.0f, 1179.5f, 1208.0f}) paint::fill(c, {at(x, 0, x, 0).x, foot.y, u, at(0, 243, 0, 269).h}, rule);
        paint::text(c, "Env.", at(1410, 248, 1442, 264), t.color("RetroDisplayScaleText"), text());

        const auto r = graphBounds();
        const float threshold = dsp::gainToDb(compressor_->getParameter(C::Threshold));
        const auto cyan = t.color("RangeDefault");
        c.save();
        c.clipRect(r.sk());
        if (view_ == View::Activity)
        {
            // Input level (grey area from the bottom), gain reduction (cyan, hanging from the top), output level (light line).
            const size_t n = history_.size();
            const float step = r.w / (float) (n - 1);
            const auto input = paint::mix(t.color("RetroDisplayBackground"), t.color("RetroDisplayScaleText"), 0.18f);
            for (size_t i = 0; i < n; ++i)
            {
                const auto& h = history_[((size_t) write_ + i) % n];
                const float x = r.x + (float) i * step, y = yOf(h[0]);
                paint::fill(c, {x, y, step + 0.5f, r.bottom() - y}, input);
            }
            SkPathBuilder reduction, output;
            for (size_t i = 0; i < n; ++i)
            {
                const auto& h = history_[((size_t) write_ + i) % n];
                const float x = r.x + (float) i * step;
                const float gy = r.y - std::min(0.0f, h[1]) * r.h / (kTopDb - kBottomDb);
                if (i == 0) { reduction.moveTo(x, gy); output.moveTo(x, yOf(h[2])); }
                else { reduction.lineTo(x, gy); output.lineTo(x, yOf(h[2])); }
            }
            SkPaint pen;
            pen.setAntiAlias(true);
            pen.setStyle(SkPaint::kStroke_Style);
            pen.setStrokeWidth(1.6f * u);
            if (showOutput_) { pen.setColor(paint::withAlpha(t.color("ControlTextBack"), 220)); c.drawPath(output.detach(), pen); }
            if (showReduction_) { pen.setColor(cyan); c.drawPath(reduction.detach(), pen); }
            paint::line(c, r.x, yOf(threshold), r.right(), yOf(threshold), cyan, 2 * u);
        }
        else
        {
            for (int db = -48; db <= -12; db += 12)
            {
                const float x = r.x + (float) (db + 60) / 60 * r.w, y = r.bottom() - (float) (db + 60) / 60 * r.h;
                paint::line(c, x, r.y, x, r.bottom(), gridInk(t, 30), u);
                paint::line(c, r.x, y, r.right(), y, gridInk(t, 30), u);
            }
            SkPathBuilder curve;
            for (int x = 0; x <= (int) r.w; ++x)
            {
                const float in = -60 + 60 * (float) x / r.w;
                const float out = devices::compressorCurveDb(in, threshold, compressor_->getParameter(C::Ratio), 2 * compressor_->getParameter(C::Knee));
                const float y = r.bottom() - (out + 60) / 60 * r.h;
                if (x == 0) curve.moveTo(r.x, y); else curve.lineTo(r.x + (float) x, y);
            }
            SkPaint pen;
            pen.setAntiAlias(true);
            pen.setStyle(SkPaint::kStroke_Style);
            pen.setStrokeWidth(2 * u);
            pen.setColor(cyan);
            c.drawPath(curve.detach(), pen);
            const float tx = r.x + (threshold + 60) / 60 * r.w;
            paint::line(c, tx, r.y, tx, r.bottom(), paint::withAlpha(cyan, 160), u);
        }
        c.restore();
        paint::text(c, printed(compressor_->gainReductionDb() + 0.0f, "%.2f dB"), at(1395, 212, 1474, 227), t.color("ChosenDefault"), text(), Align::Right);
    }

    C* compressor_;
    const model::Device& model_;
    View view_ = View::Activity;
    bool showReduction_ = true, showOutput_ = false, dragging_ = false;
    std::array<std::array<float, 3>, 180> history_{}; // ~6 s of {input, reduction, output} dB at the tick rate
    int write_ = 0;
};

// ================================================================ Limiter
// Lookahead: a stepped knob with its three settings written around the ring (1.5 / 3 / 6 ms), no value arc.
class StepKnob final : public Widget
{
public:
    StepKnob(devices::DeviceProcessor* p, int index, std::function<float()> size) : p_(p), index_(index), size_(std::move(size)) {}
    void paint(SkCanvas& c, const Theme& t) override
    {
        // Capture geometry (76 x 71 box): label centred at 7; a 240-degree ring centred at (37.5, 51), 29 across the
        // stroke centre, open at the bottom; the settings sit at its ends and top: (8, 63), (37.5, 19), (67, 63).
        const float q = width() / 76.0f, size = size_();
        const auto ink = t.color("ControlForeground");
        paint::text(c, "Lookahead", {0, 0, width(), 14 * q}, ink, size, Align::Centre);
        const float cx = 37.5f * q, cy = 51 * q, d = 29 * q;
        SkPaint pen;
        pen.setAntiAlias(true);
        pen.setStyle(SkPaint::kStroke_Style);
        pen.setStrokeWidth(3 * q);
        pen.setColor(ink);
        c.drawArc(SkRect::MakeXYWH(cx - d / 2, cy - d / 2, d, d), 150, 240, false, pen);
        const char* labels[] = {"1.5", "3", "6"};
        const float labelX[] = {8, 37.5f, 67}, labelY[] = {63, 19, 63};
        const int value = std::clamp((int) std::lround(p_->getParameter(index_)), 0, 2);
        for (int i = 0; i < 3; ++i)
        {
            const float a = (150.0f + 120.0f * (float) i) * (float) M_PI / 180.0f;
            const float r0 = d / 2 + 3.5f * q, r1 = d / 2 + 7 * q;
            paint::line(c, cx + std::cos(a) * r0, cy + std::sin(a) * r0, cx + std::cos(a) * r1, cy + std::sin(a) * r1, ink, 1.2f * q);
            paint::text(c, labels[i], {(labelX[i] - 12) * q, (labelY[i] - 6) * q, 24 * q, 12 * q}, ink, size * 0.78f, Align::Centre);
        }
        const float a = (150.0f + 120.0f * (float) value) * (float) M_PI / 180.0f;
        pen.setStrokeCap(SkPaint::kSquare_Cap);
        c.drawLine(cx, cy, cx + std::cos(a) * (d / 2 + 1.5f * q), cy + std::sin(a) * (d / 2 + 1.5f * q), pen);
    }
    void mouseDown(const MouseEvent& e) override { startY_ = e.y; start_ = p_->getParameter(index_); }
    void mouseDrag(const MouseEvent& e) override
    {
        if (e.right) return;
        p_->setParameter(index_, std::clamp(std::round(start_ + (startY_ - e.y) / 25.0f), 0.0f, 2.0f));
        repaint();
    }
    bool mouseWheel(const MouseEvent& e) override
    {
        if (e.right || std::abs(e.wheelDeltaX) > std::abs(e.wheelDeltaY)) return false;
        p_->setParameter(index_, std::clamp(std::round(p_->getParameter(index_)) + (e.wheelDeltaY > 0 ? 1.0f : -1.0f), 0.0f, 2.0f));
        repaint();
        return true;
    }

private:
    devices::DeviceProcessor* p_;
    int index_;
    std::function<float()> size_;
    float startY_ = 0, start_ = 0;
};

// "Link 100 %": the stereo link as a display value (the processor links fully or not at all). Click switches it.
class LinkValue final : public Widget
{
public:
    LinkValue(devices::DeviceProcessor* p, int index, std::function<float()> size) : p_(p), index_(index), size_(std::move(size)) {}
    void paint(SkCanvas& c, const Theme& t) override
    {
        const float size = size_(), x = gfx::measureText("Link", size) + size * 0.5f;
        paint::text(c, "Link", localBounds(), t.color("RetroDisplayScaleText"), size);
        paint::text(c, p_->getParameter(index_) > 0.5f ? "100 %" : "0 %", {x, 0, width() - x, height()}, t.color("RangeDefault"), size);
    }
    void mouseUp(const MouseEvent& e) override
    {
        if (e.right || ! localBounds().contains(e.x, e.y)) return;
        p_->setParameter(index_, p_->getParameter(index_) > 0.5f ? 0.0f : 1.0f);
        repaint();
    }

private:
    devices::DeviceProcessor* p_;
    int index_;
    std::function<float()> size_;
};

class LimiterFace final : public ReferenceFace
{
public:
    using L = devices::Limiter;
    explicit LimiterFace(L* limiter, Tag tagger) : ReferenceFace(limiter, 397, 1591, kTop, std::move(tagger)), limiter_(limiter)
    {
        animated_ = true;
        auto* input = knob(L::Gain, "Input", 1605, 44, 1671, 107.5f, Law::linear(-24, 24), [](float db) { return printed(db, "%.1f dB"); });
        input->marker = 0.5f;
        input->markerTriangle = false;
        inertButton("Maximize", false, 1598, 117, 1677, 133, "Maximize is not implemented yet (DEV-12c)");
        knob(L::Release, "Release", 1605, 176, 1671, 239.5f, Law::logarithmic(1, 3000));
        toggle(L::AutoRelease, "Auto", 1598, 248, 1677, 264);
        displayValue(L::Ceiling, "", 1713, 244, 1775, 260, Law::linear(-12, 0), [](float db) { return printed(db, "%.1f dB"); });
        auto* link = add<LinkValue>(limiter, L::LinkChannels, [this] { return text(); });
        tag(link, L::LinkChannels);
        place(link, 1806, 244, 1888, 260);
        auto* lookahead = add<StepKnob>(limiter, L::Lookahead, [this] { return text(); });
        tag(lookahead, L::Lookahead);
        place(lookahead, 1905, 44, 1981, 115);
        const std::string mode = "Soft Clip and True Peak modes are not implemented yet (DEV-12c)";
        inertButton("Standard", true, 1902, 147, 1981, 163, "The limiter runs its standard mode");
        inertButton("Soft Clip", false, 1902, 169, 1981, 186, mode);
        inertButton("True Peak", false, 1902, 192, 1981, 208, mode);
        inertButton("L/R", true, 1902, 248, 1939, 264, "The limiter processes left / right");
        inertButton("M/S", false, 1944, 248, 1981, 264, "Mid / side routing is not implemented yet (DEV-12c)");
    }

    Rect graphBounds() const override { return at(1685, 38, 1894, 267); }

    void paint(SkCanvas& c, const Theme& t) override
    {
        fillBody(c, t, false);
        label(c, t, "Mode", 1902, 130, 1981, 144);
        label(c, t, "Routing", 1902, 231, 1981, 245);
        displayPanel(c, t, 1685, 38, 1894, 267);
        const float u = unit();
        const auto scale = t.color("RetroDisplayScaleText");
        label(c, t, "Ceiling", 1706, 46, 1766, 60, Align::Centre, false, scale);
        label(c, t, "GR", 1824, 46, 1864, 60, Align::Centre, false, scale);
        const auto track = paint::mix(t.color("RetroDisplayBackground"), scale, 0.1f);
        const auto level = paint::withAlpha(scale, 200), cyan = t.color("RangeDefault");
        for (int ch = 0; ch < 2; ++ch)
        {
            const float lx = ch == 0 ? 1716 : 1737.3f, gx = ch == 0 ? 1823 : 1844;
            const auto meter = at(lx, 67, lx + 19.5f, 237), gr = at(gx, 67, gx + 20, 237);
            paint::fill(c, meter, track);
            paint::fill(c, gr, track);
            const float out = -limiter_->outputPeakDb(ch);
            if (out < 30) { const float y = scaleY(out); paint::fill(c, {meter.x, y, meter.w, meter.bottom() - y}, level); }
            const float reduction = -limiter_->gainReductionDb(ch);
            if (reduction > 0.05f) paint::fill(c, {gr.x, gr.y, gr.w, scaleY(reduction) - gr.y}, cyan);
        }
        // Scale: dB under the ceiling, as written between the meters (0 3 6 12 24) with minor ticks.
        for (int mark : {0, 1, 2, 3, 4, 5, 6, 9, 12, 18, 24})
        {
            const bool major = mark == 0 || mark == 3 || mark == 6 || mark == 12 || mark == 24;
            const float db = (float) mark;
            const float y = scaleY(db);
            const float a = at(1760, 0, 1760, 0).x, b = at(1768, 0, 1768, 0).x, e = at(1812, 0, 1812, 0).x, f = at(1820, 0, 1820, 0).x;
            paint::line(c, major ? a : a + 3 * u, y, b, y, paint::withAlpha(scale, 140), u);
            paint::line(c, e, y, major ? f : f - 3 * u, y, paint::withAlpha(scale, 140), u);
            if (major) paint::text(c, printed(db, "%.0f"), {at(1775, 0, 1775, 0).x, y - 7 * u, 30 * u, 14 * u}, scale, text(), Align::Centre);
        }
        // Ceiling pointer.
        const float y = scaleY(-limiter_->getParameter(L::Ceiling)), x = at(1757.7f, 0, 1757.7f, 0).x;
        SkPathBuilder arrow;
        arrow.moveTo(x, y).lineTo(x + 14 * u, y - 6.5f * u).lineTo(x + 14 * u, y + 6.5f * u).close();
        SkPaint fill;
        fill.setAntiAlias(true);
        fill.setColor(cyan);
        c.drawPath(arrow.detach(), fill);
    }

    void mouseDown(const MouseEvent& e) override
    {
        dragging_ = ! e.right && graphBounds().contains(e.x, e.y) && e.y < at(0, 240, 0, 240).y;
        if (dragging_) mouseDrag(e);
    }
    void mouseDrag(const MouseEvent& e) override
    {
        if (! dragging_) return;
        limiter_->setParameter(L::Ceiling, std::clamp(-dbAt(e.y), -12.0f, 0.0f));
        repaint();
    }
    void mouseUp(const MouseEvent&) override { dragging_ = false; }
    float scaleY(float db) const
    {
        // Measured label rows: 0 at 72, 3 at 115, 6 at 154, 12 at 206, 24 at 233 (piecewise linear between them).
        static constexpr std::array<std::array<float, 2>, 5> marks{{{0, 72}, {3, 115}, {6, 154}, {12, 206}, {24, 233}}};
        db = std::clamp(db, 0.0f, 24.0f);
        for (size_t i = 1; i < marks.size(); ++i)
            if (db <= marks[i][0])
            {
                const float f = (db - marks[i - 1][0]) / (marks[i][0] - marks[i - 1][0]);
                return at(0, marks[i - 1][1] + f * (marks[i][1] - marks[i - 1][1]), 0, 0).y;
            }
        return at(0, 233, 0, 0).y;
    }
    float dbAt(float y) const
    {
        float best = 0, distance = 1e9f;
        for (float db = 0; db <= 24.0f; db += 0.05f)
            if (const float d = std::abs(scaleY(db) - y); d < distance) { distance = d; best = db; }
        return best;
    }

private:
    L* limiter_;
    bool dragging_ = false;
};
} // namespace

ReferenceFace* addReferenceFace(Widget& parent, const model::Device& model, devices::DeviceProcessor* processor, ReferenceFace::Tag tag)
{
    if (auto* eq = dynamic_cast<devices::EqEight*>(processor)) return parent.add<EqEightFace>(eq, std::move(tag));
    if (auto* eq = dynamic_cast<devices::EqThree*>(processor)) return parent.add<EqThreeFace>(eq, std::move(tag));
    if (auto* compressor = dynamic_cast<devices::Compressor*>(processor)) return parent.add<CompressorFace>(compressor, model, std::move(tag));
    if (auto* limiter = dynamic_cast<devices::Limiter*>(processor)) return parent.add<LimiterFace>(limiter, std::move(tag));
    return nullptr;
}
} // namespace sonora::ui
