// UI-43b: Auto Filter (`AutoFilter2`, the reference's Live 12.4 design) matched to a 914 x 382 capture of the
// reference device (default light theme, a retina zoom: the body starts at y 40 and is 342 tall, 1.43x the 239-px body
// of the UI-42 capture). Every box below is in that capture's pixels. Controls the processor does not implement (the
// sidechain, the LFO's stereo phase / spin / offset / quantisation and morph, the envelope's hold and S&H) are drawn
// disabled with their task ID (DEV-9c).

#include "ui/views/ReferenceFace.h"

#include "devices/native/AutoFilter.h"
#include "dsp/Dsp.h"
#include "ui/Controls.h"
#include "ui/widgets/Buttons.h"
#include "ui/widgets/Knob.h"

#include "include/core/SkPaint.h"
#include "include/core/SkPathBuilder.h"

#include <cmath>
#include <complex>
#include <cstdio>

namespace sonora::ui
{
namespace
{
using A = devices::AutoFilter2;
using Law = ReferenceFace::Law;
constexpr const char* kDegree = "\xC2\xB0";
constexpr const char* kPhi = "\xC3\xB8";

std::string printed(float value, const char* format)
{
    char text[48];
    std::snprintf(text, sizeof(text), format, (double) value);
    return text;
}
// The reference writes percentages with one decimal below 100 ("0.0 %", "100 %").
std::string percent(float v)
{
    const float p = v * 100.0f;
    return printed(p, std::abs(p) >= 99.95f ? "%.0f %%" : "%.1f %%");
}
Rect trimmed(Rect r, float left) { return {r.x + left, r.y, std::max(0.0f, r.w - left), r.h}; }
std::string seconds(float s) { return s >= 1.0f ? ReferenceFace::significant(s) + " s" : ReferenceFace::significant(s * 1000.0f) + " ms"; }

void stroke(SkCanvas& c, SkPathBuilder& path, SkColor ink, float width)
{
    SkPaint pen;
    pen.setAntiAlias(true);
    pen.setStyle(SkPaint::kStroke_Style);
    pen.setStrokeWidth(width);
    pen.setStrokeJoin(SkPaint::kRound_Join);
    pen.setColor(ink);
    c.drawPath(path.detach(), pen);
}

// Filter type glyphs as in the reference's type chooser: a 30 x 14 glyph 8 px into a 30-px chooser (band-pass measured;
// the others drawn in the same vocabulary). `r` is the chooser's icon box, which scales with the chooser height.
void filterIcon(SkCanvas& c, const Theme& t, Rect r, int type, SkColor ink)
{
    const float h = r.h / 0.52f, x = 0.267f * h, y = 0.267f * h, w = h, gh = 0.467f * h, bottom = y + gh, k = gh * 0.45f;
    SkPathBuilder p;
    switch (type)
    {
        case 0: // Lowpass: flat, then a rounded fall
            p.moveTo(x, y).lineTo(x + w * 0.6f, y).quadTo(x + w * 0.85f, y, x + w * 0.85f, y + k).lineTo(x + w * 0.85f, bottom);
            break;
        case 1: // Highpass
            p.moveTo(x + w * 0.15f, bottom).lineTo(x + w * 0.15f, y + k).quadTo(x + w * 0.15f, y, x + w * 0.4f, y).lineTo(x + w, y);
            break;
        case 2: // Bandpass: rounded rise, flat top, rounded fall
        case 7: // Vowel runs the band-pass here
            p.moveTo(x, bottom).lineTo(x, y + k).quadTo(x, y, x + k, y).lineTo(x + w - k, y).quadTo(x + w, y, x + w, y + k).lineTo(x + w, bottom);
            if (type == 7) p.moveTo(x + w * 0.5f, y).lineTo(x + w * 0.5f, y + gh * 0.55f);
            break;
        case 3: // Notch
            p.moveTo(x, y).lineTo(x + w * 0.35f, y).quadTo(x + w * 0.5f, y, x + w * 0.5f, bottom).quadTo(x + w * 0.5f, y, x + w * 0.65f, y).lineTo(x + w, y);
            break;
        case 4: // Morph: low-pass and high-pass flanks meeting
            p.moveTo(x, y).quadTo(x + w * 0.4f, y, x + w * 0.45f, bottom);
            p.moveTo(x + w * 0.55f, bottom).quadTo(x + w * 0.6f, y, x + w, y);
            break;
        case 5: // DJ
            paint::text(c, "DJ", {x, y - gh * 0.3f, w, gh * 1.6f}, ink, gh * 1.25f, Align::Left, true);
            return;
        default: // Comb: evenly spaced teeth
            p.moveTo(x, bottom);
            for (int i = 0; i < 4; ++i) p.lineTo(x + w * (0.125f + 0.25f * (float) i), y).lineTo(x + w * 0.25f * (float) (i + 1), bottom);
            break;
    }
    (void) t;
    stroke(c, p, ink, std::max(1.0f, gh * 0.2f));
}

// LFO waveform glyphs of the Wave chooser: a 30 x 16 glyph 6 px into a 26-px chooser (sine measured).
void waveIcon(SkCanvas& c, const Theme&, Rect r, int wave, SkColor ink)
{
    const float h = r.h / 0.52f, x = 0.23f * h, y = 0.2f * h, w = 1.15f * h, gh = 0.6f * h, mid = y + gh * 0.5f, bottom = y + gh;
    SkPathBuilder p;
    switch (wave)
    {
        case 0: // Sine: one period, starting at the middle going up
            p.moveTo(x, mid);
            for (int i = 1; i <= 24; ++i)
            {
                const float f = (float) i / 24.0f;
                p.lineTo(x + w * f, mid - std::sin(f * 6.2831853f) * gh * 0.5f);
            }
            break;
        case 1: p.moveTo(x, mid).lineTo(x + w * 0.25f, y).lineTo(x + w * 0.75f, bottom).lineTo(x + w, mid); break;
        case 2: p.moveTo(x, bottom).lineTo(x + w * 0.5f, y).lineTo(x + w * 0.5f, bottom).lineTo(x + w, y).lineTo(x + w, bottom); break;
        case 3: p.moveTo(x, y).lineTo(x + w * 0.5f, bottom).lineTo(x + w * 0.5f, y).lineTo(x + w, bottom).lineTo(x, y).moveTo(x + w, bottom); break;
        case 4: p.moveTo(x, bottom).lineTo(x, y).lineTo(x + w * 0.5f, y).lineTo(x + w * 0.5f, bottom).lineTo(x + w, bottom).lineTo(x + w, y); break;
        case 5: // S&H: held steps
        {
            const float level[] = {0.3f, 0.9f, 0.1f, 0.6f, 0.2f};
            p.moveTo(x, y + gh * level[0]);
            for (int i = 0; i < 5; ++i)
            {
                p.lineTo(x + w * 0.2f * (float) i, y + gh * level[i]).lineTo(x + w * 0.2f * (float) (i + 1), y + gh * level[i]);
            }
            break;
        }
        default: // Wander: smooth random
            p.moveTo(x, mid).cubicTo(x + w * 0.2f, y, x + w * 0.3f, bottom, x + w * 0.55f, mid).cubicTo(x + w * 0.75f, y - gh * 0.1f, x + w * 0.85f, y + gh * 0.8f, x + w, y + gh * 0.3f);
            break;
    }
    stroke(c, p, ink, std::max(1.0f, gh * 0.16f));
}

// A drawn control the processor does not implement: painted by the face's callback, explains itself in a tooltip.
class Inert final : public Widget
{
public:
    using Painter = std::function<void(SkCanvas&, const Theme&, Rect)>;
    Inert(Painter painter, std::string why) : painter_(std::move(painter)) { tooltip = std::move(why); }
    void paint(SkCanvas& c, const Theme& t) override { painter_(c, t, localBounds()); }

private:
    Painter painter_;
};

class AutoFilterFace final : public ReferenceFace
{
public:
    enum class Section { Lfo, Envelope }; // the modulator the display bar shows (the one last touched)

    explicit AutoFilterFace(A* filter, Tag tagger) : ReferenceFace(filter, 886, 8, 40, std::move(tagger), 342), filter_(filter)
    {
        const auto type = [filter] { return (int) std::lround(filter->getParameter(A::FilterType)); };
        const auto mode = [filter] { return (int) std::lround(filter->getParameter(A::LfoTimeMode)); };

        // Sidechain strip (folded): expand and listen circles, caption and LED.
        inert([](SkCanvas& c, const Theme& t, Rect r) { circleButton(c, t, r, true); }, 18, 48, 44, 74, "The sidechain input is not implemented yet (DEV-9c)");
        inert([](SkCanvas& c, const Theme& t, Rect r) { circleButton(c, t, r, false); }, 18, 80, 44, 106, "Sidechain listen is not implemented yet (DEV-9c)");

        // Filter column. Freq is the large knob (ring 73.5 across, 2.04 units per knob unit); DJ replaces it for type DJ.
        auto* freq = knob(A::Frequency, "Freq", 58, 46.5f, 167, 176.1f, Law::logarithmic(20, 20000));
        freq->deviceStroke = 2.45f;
        showWhen(freq, [type] { return type() != 5; });
        auto* dj = knob(A::DjControl, "DJ", 58, 46.5f, 167, 176.1f, Law::linear(-1, 1), percent);
        dj->deviceStroke = 2.45f;
        dj->marker = 0.5f;
        dj->markerTriangle = false;
        showWhen(dj, [type] { return type() == 5; });
        chooser(A::FilterType, 60, 192, 166, 222, filterIcon);
        for (auto* b : {option(A::Slope, 0, "12", 62, 226, 110, 252), option(A::Slope, 1, "24", 116, 226, 164, 252)})
            showWhen(b, [type] { return type() != 4; });
        showWhen(slider(A::Morph, 62, 226, 164, 252, Law::linear(0, 1), percent), [type] { return type() == 4; });
        knob(A::Resonance, "Res", 62, 277.7f, 163, 368.6f, Law::linear(0, 1), percent);

        // LFO section. The Rate knob follows Mode: Hz, seconds, or the synced rate.
        lfo(knob(A::LfoAmount, "LFO Amt", 192, 277.7f, 293, 368.6f, Law::linear(0, 1), percent));
        const float rate[] = {300, 277.7f, 401, 368.6f};
        auto* hz = lfo(knob(A::LfoFrequency, "Rate", rate[0], rate[1], rate[2], rate[3], Law::logarithmic(0.1f, 60)));
        showWhen(hz, [mode] { return mode() == 0; });
        auto* time = lfo(knob(A::LfoTime, "Time", rate[0], rate[1], rate[2], rate[3], Law::logarithmic(0.1f, 200), seconds));
        showWhen(time, [mode] { return mode() == 1; });
        auto* synced = lfo(knob(A::LfoSyncedRate, "Rate", rate[0], rate[1], rate[2], rate[3], Law::logarithmic(0.25f, 64),
                                [](float v) { return ReferenceFace::significant(v); }));
        showWhen(synced, [mode] { return mode() >= 2; });
        lfo(chooser(A::LfoWaveform, 492, 272, 594, 298, waveIcon));
        lfo(inert([this](SkCanvas& c, const Theme& t, Rect r) {
                ctl::framed(c, t, r, t.color("ControlTextBack"));
                paint::text(c, "0.0 %", r, t.color("TextDisabled"), text(), Align::Centre);
            }, 492, 308, 594, 334, "LFO morph is not implemented yet (DEV-9c)"));
        lfo(chooser(A::LfoTimeMode, 492, 344, 594, 370));

        // Envelope section.
        auto* envelope = knob(A::EnvAmount, "Envelope", 636, 277.7f, 737, 368.6f, Law::linear(-1, 1), percent);
        envelope->marker = 0.5f;
        envelope->markerTriangle = false;
        envelopeWidgets_.push_back(envelope);

        // Output column.
        knob(A::Drive, "Drive", 780, 51.6f, 881, 142.5f, Law::linear(0, 1), percent);
        chooser(A::Circuit, 780, 150, 882, 176);
        toggle(A::SoftClip, "Clip", 780, 194, 860, 220);
        slider(A::Output, 780, 226, 882, 252,
               {[](float g) { return g <= 0.0000317f ? 0.0f : std::clamp((dsp::gainToDb(g) + 36.0f) / 36.0f, 0.0f, 1.0f); },
                [](float n) { return n <= 0.002f ? 0.0f : dsp::dbToGain(-36.0f + 36.0f * n); }},
               [](float g) { return g <= 0.0000317f ? std::string("-inf dB") : printed(dsp::gainToDb(g) + 0.0f, "%.1f dB"); });
        knob(A::DryWet, "Dry/Wet", 780, 277.7f, 881, 368.6f, Law::linear(0, 1), percent);

        // Display bar, LFO: stereo mode, stereo phase, offset and quantisation (Live's defaults, drawn disabled).
        const auto dim = [](const Theme& t) { return paint::mix(t.color("RetroDisplayBackground"), t.color("RetroDisplayScaleText"), 0.76f); };
        onBar(Section::Lfo, inertButton("Phase", true, 254, 228, 324, 250, "The LFO's stereo phase / spin modes are not implemented yet (DEV-9c)"));
        onBar(Section::Lfo, inert([this, dim](SkCanvas& c, const Theme& t, Rect r) {
                  paint::text(c, kPhi, r, dim(t), text());
                  paint::text(c, std::string("4.0") + kDegree, trimmed(r, 25 * unit()), dim(t), text());
              }, 355, 228, 440, 250, "The LFO's stereo phase is not implemented yet (DEV-9c)"));
        onBar(Section::Lfo, inert([this, dim](SkCanvas& c, const Theme& t, Rect r) {
                  offsetIcon(c, {r.x, r.y + 2 * unit(), 18 * unit(), 17 * unit()}, dim(t), 2 * unit());
                  paint::text(c, std::string("0.0") + kDegree, trimmed(r, 28 * unit()), dim(t), text());
              }, 468, 228, 560, 250, "The LFO's phase offset is not implemented yet (DEV-9c)"));
        onBar(Section::Lfo, inert([this, dim](SkCanvas& c, const Theme& t, Rect r) {
                  const float u = unit();
                  quantiseIcon(c, {r.x, r.y + 2 * u, 16 * u, 17 * u}, dim(t), 2 * u);
                  paint::text(c, "None", trimmed(r, 28 * u), dim(t), text());
                  ctl::triangle(c, r.x + 94 * u, r.centreY(), 10 * u, 5 * u, ctl::Dir::Down, dim(t));
                  paint::text(c, "8", trimmed(r, 116 * u), dim(t), text());
              }, 583, 228, 760, 250, "LFO quantisation is not implemented yet (DEV-9c)"));

        // Display bar, envelope: attack and release (layout not measured yet: the capture shows the LFO bar).
        const auto attack = Law::logarithmic(0.0001f, 0.1f), release = Law::logarithmic(0.001f, 3);
        onBar(Section::Envelope, displayValue(A::EnvAttack, "Attack", 300, 228, 460, 250, attack, seconds, false, true));
        onBar(Section::Envelope, displayValue(A::EnvRelease, "Release", 470, 228, 640, 250, release, seconds, false, true));
        onBar(Section::Envelope, inertButton("Hold", false, 652, 228, 704, 250, "The envelope's hold is not implemented yet (DEV-9c)"));
        onBar(Section::Envelope, inertButton("S&H", false, 710, 228, 760, 250, "The envelope's sample and hold is not implemented yet (DEV-9c)"));
    }

    Rect graphBounds() const override { return at(176, 40, 768, 216); }
    Section section() const { return section_; }
    void select(Section s)
    {
        if (section_ == s) return;
        section_ = s;
        layout();
        repaint();
    }

    void paint(SkCanvas& c, const Theme& t) override
    {
        fillBody(c, t, true);
        panel(c, t, 12, 40, 50, 378);
        panel(c, t, 54, 40, 172, 378);
        panel(c, t, 772, 40, 890, 378);
        if (section_ == Section::Lfo) panel(c, t, 606, 264, 768, 378);
        else panel(c, t, 176, 264, 602, 378);
        label(c, t, "Wave", 413, 272, 490, 298, Align::Left);
        label(c, t, "Morph", 413, 308, 490, 334, Align::Left);
        label(c, t, "Mode", 413, 344, 490, 370, Align::Left);

        // Sidechain caption and activity LED.
        const auto caption = at(21, 250, 40, 350);
        c.save();
        c.translate(caption.centreX(), caption.centreY());
        c.rotate(-90);
        paint::text(c, "Sidechain", {-caption.h * 0.5f, -caption.w * 0.5f, caption.h, caption.w}, t.color("ControlForeground"), text(), Align::Centre);
        c.restore();
        led(c, t, 30.5f, 360.5f, 6, false);
        led(c, t, 874.5f, 206.5f, 7, false); // soft-clip activity

        paintDisplay(c, t);
    }

    void mouseDown(const MouseEvent& e) override
    {
        dragging_ = ! e.right && graphBounds().contains(e.x, e.y) && std::lround(filter_->getParameter(A::FilterType)) != 5;
        if (dragging_) { mouseDrag(e); return; }
        if (at(172, 260, 606, 382).contains(e.x, e.y)) select(Section::Lfo);
        else if (at(606, 260, 772, 382).contains(e.x, e.y)) select(Section::Envelope);
    }
    void mouseDrag(const MouseEvent& e) override
    {
        if (! dragging_) return;
        // Horizontal: frequency on the display's log axis; vertical: resonance (0 at the measured handle row).
        const float x = captureX(e.x), y = captureY(e.y);
        filter_->setParameter(A::Frequency, std::clamp(1000.0f * std::pow(10.0f, (x - kKiloX) / kDecade), 20.0f, 20000.0f));
        filter_->setParameter(A::Resonance, std::clamp((kHandleY - y) / kHandleTravel, 0.0f, 1.0f));
        repaint();
    }
    void mouseUp(const MouseEvent&) override { dragging_ = false; }
    void descendantPressed(Widget* leaf) override
    {
        for (auto* w : lfoWidgets_) if (w == leaf) select(Section::Lfo);
        for (auto* w : envelopeWidgets_) if (w == leaf) select(Section::Envelope);
    }

    // Display mapping in capture pixels (measured): 1 kHz at x 510.5, 196 px per decade, 0 dB at y 128.5, 3.67 px per dB.
    static constexpr float kKiloX = 510.5f, kDecade = 196.0f, kZeroY = 128.5f, kPerDb = 3.667f;
    static constexpr float kHandleY = 202.5f, kHandleTravel = 150.0f;

private:
    template <typename W> W* lfo(W* widget) { lfoWidgets_.push_back(widget); return widget; }
    Widget* inert(Inert::Painter painter, float x0, float y0, float x1, float y1, std::string why)
    {
        auto* widget = add<Inert>(std::move(painter), std::move(why));
        place(widget, x0, y0, x1, y1);
        return widget;
    }
    void onBar(Section s, Widget* widget) { showWhen(widget, [this, s] { return section_ == s; }); }
    float captureX(float x) const { return x / unit() + 8; }
    float captureY(float y) const { return y / unit() + 40; }

    static void circleButton(SkCanvas& c, const Theme& t, Rect r, bool fold)
    {
        const float d = std::min(r.w, r.h), cx = r.centreX(), cy = r.centreY();
        SkPaint pen;
        pen.setAntiAlias(true);
        pen.setColor(t.color("ControlTextBack"));
        c.drawCircle(cx, cy, d * 0.5f, pen);
        pen.setStyle(SkPaint::kStroke_Style);
        pen.setStrokeWidth(d * 0.09f);
        pen.setColor(t.color("ControlContrastFrame"));
        c.drawCircle(cx, cy, d * 0.5f - d * 0.045f, pen);
        const auto ink = t.color("TextDisabled");
        if (fold)
        {
            ctl::triangle(c, cx, cy + d * 0.02f, d * 0.52f, d * 0.42f, ctl::Dir::Down, ink);
            return;
        }
        // Headphones: a band over two ear cups.
        pen.setColor(ink);
        pen.setStrokeWidth(d * 0.1f);
        c.drawArc(SkRect::MakeLTRB(cx - d * 0.27f, cy - d * 0.27f, cx + d * 0.27f, cy + d * 0.27f), 180, 180, false, pen);
        SkPaint fill;
        fill.setAntiAlias(true);
        fill.setColor(ink);
        c.drawRect(SkRect::MakeXYWH(cx - d * 0.32f, cy - d * 0.02f, d * 0.16f, d * 0.26f), fill);
        c.drawRect(SkRect::MakeXYWH(cx + d * 0.16f, cy - d * 0.02f, d * 0.16f, d * 0.26f), fill);
    }
    static void offsetIcon(SkCanvas& c, Rect r, SkColor ink, float width)
    {
        // A sine period with a start line through it (the LFO offset).
        SkPathBuilder p;
        p.moveTo(r.x + r.w * 0.25f, r.y).lineTo(r.x + r.w * 0.25f, r.bottom());
        p.moveTo(r.x, r.centreY());
        for (int i = 1; i <= 16; ++i)
        {
            const float f = (float) i / 16.0f;
            p.lineTo(r.x + r.w * f, r.centreY() - std::sin(f * 6.2831853f) * r.h * 0.4f);
        }
        stroke(c, p, ink, width);
    }
    static void quantiseIcon(SkCanvas& c, Rect r, SkColor ink, float width)
    {
        // A metronome: an open trapezoid with its swinging arm.
        SkPathBuilder p;
        p.moveTo(r.x + r.w * 0.4f, r.y).lineTo(r.x, r.bottom()).lineTo(r.right(), r.bottom()).lineTo(r.x + r.w * 0.75f, r.y + r.h * 0.35f);
        p.moveTo(r.x + r.w * 0.4f, r.bottom() - r.h * 0.2f).lineTo(r.right(), r.y + r.h * 0.1f);
        stroke(c, p, ink, width);
    }

    // Magnitude in dB of the processor's filter at `hz` (analog prototypes of its RBJ stages; DEV-22b laws).
    float responseDb(float hz) const
    {
        using C = std::complex<double>;
        const int type = std::clamp((int) std::lround(filter_->getParameter(A::FilterType)), 0, 7);
        const double res = filter_->getParameter(A::Resonance), dj = filter_->getParameter(A::DjControl);
        double fc = filter_->getParameter(A::Frequency);
        if (type == 5) fc = dj >= 0 ? 20.0 * std::exp2(9.2 * dj) : 20000.0 * std::exp2(9.2 * dj);
        if (type == 6)
        {
            const double fb = std::min(0.995, 0.5 * (1.0 + res)), phase = -2.0 * M_PI * hz / (2.0 * std::max(20.0, fc));
            return (float) (20.0 * std::log10(std::max(1e-9, (1.0 - fb) / std::abs(C(1.0) - fb * std::polar(1.0, phase)))));
        }
        const double d = A::damping((float) res), w = hz / fc;
        const int stages = filter_->getParameter(A::Slope) > 0.5f ? 2 : 1;
        C lp = 1, hp = 1, bp = 1, notch = 1;
        for (int k = 0; k < stages; ++k)
        {
            const double q = k == 0 ? 1.0 / (2.0 * d) : 1.0 / (1.0 + d);
            const C den(1.0 - w * w, w / q);
            lp *= 1.0 / den;
            hp *= -w * w / den;
            bp *= C(0, w) / den;
            notch *= (1.0 - w * w) / den;
        }
        C h;
        switch (type)
        {
            case 1: h = hp; break;
            case 2: case 7: h = bp; break;
            case 3: h = notch; break;
            case 4:
            {
                const double m = filter_->getParameter(A::Morph);
                h = m < 0.5 ? lp + (bp - lp) * (m * 2.0) : bp + (hp - bp) * ((m - 0.5) * 2.0);
                break;
            }
            case 5: h = dj >= 0 ? hp : lp; break;
            default: h = lp; break;
        }
        return (float) (20.0 * std::log10(std::max(1e-9, std::abs(h))));
    }

    void paintDisplay(SkCanvas& c, const Theme& t)
    {
        displayPanel(c, t, 176, 40, 768, 260);
        const float u = unit();
        const auto scale = t.color("RetroDisplayScaleText");
        // Grid: decades at 100 Hz / 1 kHz / 10 kHz and the 0 dB line (2 px), the bar's top rule.
        for (float x : {314.5f, 510.5f, 706.5f}) paint::fill(c, at(x - 1, 40, x + 1, 216), paint::withAlpha(scale, 49));
        paint::fill(c, at(176, kZeroY - 1, 768, kZeroY + 1), paint::withAlpha(scale, 49));
        paint::fill(c, at(176, 216, 768, 218), paint::withAlpha(scale, 68));
        paint::text(c, section_ == Section::Lfo ? "LFO" : "Envelope", at(189, 228, 330, 250), scale, text(), Align::Left, true);

        const auto r = graphBounds();
        c.save();
        c.clipRect(r.sk());
        SkPathBuilder curve;
        for (int i = 0; i <= (int) r.w; ++i)
        {
            const float px = r.x + (float) i, hz = 1000.0f * std::pow(10.0f, (captureX(px) - kKiloX) / kDecade);
            const float y = at(0, kZeroY - kPerDb * std::clamp(responseDb(hz), -40.0f, 30.0f), 0, 0).y;
            if (i == 0) curve.moveTo(px, y); else curve.lineTo(px, y);
        }
        stroke(c, curve, t.color("RangeDefault"), 3 * u);
        // Handle: frequency across, resonance up (a 25-px amber ring). DJ sets its corner from the DJ knob instead.
        if (std::lround(filter_->getParameter(A::FilterType)) == 5) { c.restore(); return; }
        const float hz = filter_->getParameter(A::Frequency);
        const auto centre = at(kKiloX + kDecade * std::log10(hz / 1000.0f), kHandleY - kHandleTravel * filter_->getParameter(A::Resonance), 0, 0);
        SkPaint ring;
        ring.setAntiAlias(true);
        ring.setStyle(SkPaint::kStroke_Style);
        ring.setStrokeWidth(3 * u);
        ring.setColor(t.color("ChosenDefault"));
        c.drawCircle(centre.x, centre.y, 11 * u, ring);
        c.restore();
    }

    A* filter_;
    Section section_ = Section::Lfo;
    std::vector<Widget*> lfoWidgets_, envelopeWidgets_;
    bool dragging_ = false;
};
} // namespace

ReferenceFace* addAutoFilterFace(Widget& parent, devices::DeviceProcessor* processor, ReferenceFace::Tag tag)
{
    if (auto* filter = dynamic_cast<A*>(processor)) return parent.add<AutoFilterFace>(filter, std::move(tag));
    return nullptr;
}
} // namespace sonora::ui
