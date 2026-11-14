#include "ui/views/CorrelationFace.h"

#include "include/core/SkPathBuilder.h"
#include "include/core/SkRRect.h"
#include "include/effects/SkGradientShader.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace sonora::ui
{
namespace
{
using C = devices::Correlation;
constexpr float kSqrt2 = 1.41421356f;

std::string format(const char* pattern, double value)
{
    char buffer[128];
    std::snprintf(buffer, sizeof buffer, pattern, value);
    return buffer;
}

// "+0.62", "0.00", "-0.41"
std::string signedValue(float v) { return std::abs(v) < 0.005f ? std::string("0.00") : format("%+.2f", v); }
std::string decibels(float d) { return format(std::abs(d) < 0.05f ? "%.1f dB" : "%+.1f dB", d); }

SkColor mix(SkColor a, SkColor b, float t)
{
    t = std::clamp(t, 0.0f, 1.0f);
    const auto channel = [t](unsigned x, unsigned y) { return (unsigned) std::lround((float) x + ((float) y - (float) x) * t); };
    return SkColorSetARGB(channel(SkColorGetA(a), SkColorGetA(b)), channel(SkColorGetR(a), SkColorGetR(b)), channel(SkColorGetG(a), SkColorGetG(b)),
                          channel(SkColorGetB(a), SkColorGetB(b)));
}

} // namespace

CorrelationFace::CorrelationFace(devices::Correlation& device)
    : ProFace(device, kFull, kCompact, kAccent), meter_(device), x_((size_t) kScopeRead), y_((size_t) kScopeRead), pts_((size_t) kScopeRead)
{
}

SkColor CorrelationFace::colourFor(float c)
{
    if (c >= 0.5f) return kGood;
    if (c >= 0.0f) return mix(kWarn, kGood, c / 0.5f);
    if (c >= -0.5f) return mix(kBad, kWarn, (c + 0.5f) / 0.5f);
    return kBad;
}

float CorrelationFace::zoomDb() const { return value(C::ScopeAuto) >= 0.5f ? autoZoomDb_ : value(C::ScopeZoom); }

float CorrelationFace::correlationAbove(float hz) const
{
    // The bands' correlations weighted by their energy: close to the correlation of everything above `hz`.
    double sum = 0, weight = 0;
    for (int i = 0; i < C::kBands; ++i)
    {
        const auto& b = bands_[(size_t) i];
        if (! b.signal || C::bandCentre(i) <= hz * 1.01f) continue;
        const double w = std::pow(10.0, (double) b.levelDb / 10.0);
        sum += w * (double) b.correlation;
        weight += w;
    }
    return weight > 0 ? (float) (sum / weight) : 1.0f;
}

int CorrelationFace::worstBand(bool lowEndOnly) const
{
    float loudest = -200.0f;
    for (const auto& b : bands_)
        if (b.signal) loudest = std::max(loudest, b.levelDb);
    int worst = -1;
    for (int i = 0; i < C::kBands; ++i)
    {
        const auto& b = bands_[(size_t) i];
        if (! b.signal || b.levelDb < loudest - 40.0f) continue;
        if (lowEndOnly && C::bandCentre(i) > C::kLowEndHz * 1.01f) break;
        if (worst < 0 || b.correlation < bands_[(size_t) worst].correlation) worst = i;
    }
    return worst;
}

std::string CorrelationFace::bandReadout(int index) const
{
    if (index < 0 || index >= C::kBands) return {};
    const auto& b = bands_[(size_t) index];
    const std::string name = C::bandLabel(index) + " Hz";
    if (! b.active) return name + ": above this sample rate's range.";
    if (! b.signal) return name + ": no signal in this band.";
    std::string s = name + ": correlation " + signedValue(b.correlation);
    if (b.minHeld < b.correlation - 0.02f) s += " (held low " + signedValue(b.minHeld) + ")";
    s += ", level " + format("%.0f dBFS", b.levelDb) + ". ";
    s += b.correlation >= 0.5f ? "Safe in mono." : b.correlation >= 0.0f ? "Wide: partly lost in mono." : "Out of phase: cancels in mono.";
    return s;
}

CorrelationFace::Verdict CorrelationFace::verdict() const
{
    const auto& r = reading_;
    const std::string mono = decibels(r.monoDb);
    if (bypassed_) return {"Correlation is off", "Turn the device on to measure the stereo image.", pro::kDim};
    if (! r.signal)
        return {"Waiting for audio\xE2\x80\xA6",
                "Play something through it: the meter, the scope and the bands follow the input, with or without the transport.",
                pro::kDim};
    if (r.levelL < -60.0f && r.levelR > r.levelL + 30.0f)
        return {"Only the right side has sound", "The left channel is silent: in mono this plays " + mono + ". Check the panning or the routing.", kWarn};
    if (r.levelR < -60.0f && r.levelL > r.levelR + 30.0f)
        return {"Only the left side has sound", "The right channel is silent: in mono this plays " + mono + ". Check the panning or the routing.", kWarn};
    if (r.correlation >= 0.97f && r.width < 0.05f)
        return {"Mono", "Left and right carry the same signal: nothing is lost in mono, and there is no stereo image either.", kAccent};
    const int worst = worstBand();
    const std::string worstText = worst >= 0 ? " (worst: " + C::bandLabel(worst) + " Hz at " + signedValue(bands_[(size_t) worst].correlation) + ")" : "";
    // The whole mix inverted, or only its low end (an inverted bass pulls the wideband reading down with it)?
    const bool lowEndProblem = r.lowSignal && r.lowEnd < 0.2f;
    if (r.correlation < -0.2f && (! lowEndProblem || correlationAbove(C::kLowEndHz) < 0.0f))
        return {"Out of phase: it cancels in mono",
                "The mono sum is " + mono + " against the stereo mix" + worstText + ". One side is probably inverted: flip the polarity of one channel.",
                kBad};
    if (lowEndProblem)
    {
        const int low = worstBand(true);
        const std::string where = low >= 0 ? C::bandLabel(low) + " Hz at " + signedValue(bands_[(size_t) low].correlation) : "the low end";
        return {r.lowEnd < -0.1f ? "Low end out of phase (" + signedValue(r.lowEnd) + ")" : "Low end too wide (" + signedValue(r.lowEnd) + ")",
                "Below " + format("%.0f Hz", C::kLowEndHz) + " the sides disagree (worst: " + where +
                    "): the bass thins out in mono, on club systems and phones. Keep the low end mono.",
                r.lowEnd < -0.1f ? kBad : kWarn};
    }
    if (r.correlation < 0.0f)
        return {"Phasey (" + signedValue(r.correlation) + ")",
                "More of the sides disagree than agree: the mono sum is " + mono + worstText + ". Check it with Listen > Mono.", kWarn};
    if (r.correlation < 0.15f)
        return {"Very wide (" + signedValue(r.correlation) + ")",
                "Left and right barely share anything: the mono sum is " + mono + worstText + ". Check that the lead parts still read in mono.", kWarn};
    std::string detail = "Wide and safe in mono: the mono sum is " + mono + " against the stereo mix";
    if (r.lowSignal) detail += ", the low end " + signedValue(r.lowEnd);
    return {"Mono-compatible (" + signedValue(r.correlation) + ")", detail + ".", kGood};
}

std::string CorrelationFace::valueText(int param, float v) const
{
    if (param == C::Integration && v >= 995.0f) return format("%.1f s", v / 1000.0f);
    return ProFace::valueText(param, v);
}

Rect CorrelationFace::bandsPlot() const
{
    return layoutMode() == Layout::Full ? Rect{380, 262, 712, 254} : Rect{178, 70, 314, 86};
}

int CorrelationFace::bandAt(float x, float y) const
{
    const auto r = bandsPlot();
    const float left = layoutMode() == Layout::Full ? 30.0f : 4.0f, bottom = layoutMode() == Layout::Full ? 20.0f : 4.0f;
    const Rect p{r.x + left, r.y + 6, r.w - left - 6, r.h - 6 - bottom};
    if (x < p.x || x >= p.x + p.w || y < r.y || y >= r.y + r.h) return -1;
    return std::clamp((int) ((x - p.x) / (p.w / (float) C::kBands)), 0, C::kBands - 1);
}

std::string CorrelationFace::idleHelp() const
{
    if (const int band = bandAt(designMouseX(), designMouseY()); band >= 0) return bandReadout(band);
    return verdict().detail;
}

void CorrelationFace::willEdit(int param)
{
    if (param == C::ScopeZoom && value(C::ScopeAuto) >= 0.5f)
    {
        setValue(C::ScopeZoom, autoZoomDb_); // the knob takes over from where auto left it
        setValue(C::ScopeAuto, 0.0f);
    }
}

bool CorrelationFace::animate()
{
    bool dirty = false;
    if (const bool bypassed = meter_.isBypassed(); bypassed != bypassed_) { bypassed_ = bypassed; dirty = true; }
    const uint64_t seen = meter_.samplesSeen();
    const auto reading = meter_.reading();
    if (seen != seen_ || reading.frozen != reading_.frozen || std::abs(reading.minHeld - reading_.minHeld) > 0.0f || std::abs(reading.maxHeld - reading_.maxHeld) > 0.0f)
    {
        seen_ = seen;
        dirty = true;
        reading_ = reading;
        for (int b = 0; b < C::kBands; ++b) bands_[(size_t) b] = meter_.band(b);
        points_ = meter_.scope(x_.data(), y_.data(), kScopeRead);
        if (reading_.signal && points_ > 0)
        {
            float peak = 0;
            for (int i = 0; i < points_; ++i) peak = std::max(peak, (std::abs(x_[(size_t) i]) + std::abs(y_[(size_t) i])) / kSqrt2);
            const float target = std::clamp(-20.0f * std::log10(std::max(peak, 1.0e-6f)) - 1.0f, 0.0f, 30.0f);
            // Louder: at once (nothing leaves the scope); quieter: grows back at 15 dB a second.
            autoZoomDb_ = target < autoZoomDb_ ? target : std::min(target, autoZoomDb_ + 0.5f);
        }
    }
    return dirty;
}

void CorrelationFace::addListen(const Rect& r, const std::vector<std::string>& labels, bool vertical, float gap, const std::string& tip)
{
    // Like addSegments, with the check modes lit amber: the output is not the input while one of them is on.
    const int n = (int) labels.size();
    const float cell = ((vertical ? r.h : r.w) - gap * (float) (n - 1)) / (float) n;
    for (int i = 0; i < n; ++i)
    {
        const float at = (cell + gap) * (float) i;
        auto& s = addSegment(vertical ? Rect{r.x, r.y + at, r.w, cell} : Rect{r.x + at, r.y, cell, r.h}, C::Listen, (float) i, labels[(size_t) i], tip);
        if (i != C::ListenStereo) s.colour = kWarn;
    }
}

void CorrelationFace::build(Layout layout)
{
    const std::string listenTip =
        "Listen: Stereo passes the input untouched. Mono, Side, Left and Right replace the output to check the mix by ear; turn it back to Stereo.";
    const auto freeze = [this] { meter_.setFrozen(! meter_.frozen()); };
    const auto reset = [this] { meter_.requestReset(); };
    if (layout == Layout::Full)
    {
        addListen({600, 16, 324, 30}, {"Stereo", "Mono", "Side", "L", "R"}, false, 4, listenTip);
        addButton({936, 16, 80, 30}, "Freeze", freeze, "Freeze: holds every reading on screen (the audio keeps passing). Click again to follow the input.")
            .lit = [this] { return meter_.frozen(); };
        addButton({1024, 16, 80, 30}, "Reset", reset, "Reset: clears the held extremes, the band minimums and the history.");
        addArea({28, 260, 316, 226}, "The scope: every sample of the last 85 ms. Mono is a vertical line, a side alone leans to it, unrelated sides "
                                     "fill a disc, and anything horizontal cancels in mono.");
        addSegments({28, 492, 150, 26}, C::ScopeMode, {"Lissajous", "Polar"}, false, 4,
                    "Lissajous: the classic goniometer. Polar: the same folded into a half disc (in phase up, out of phase to the sides).");
        addToggle({186, 492, 158, 26}, C::ScopeAuto, "Auto zoom", "Auto zoom: scales the scope to the signal (the Zoom knob takes over when you turn it).");
        addArea(bandsPlot(), ""); // the pointer reads the band under it (help line)
        const float row = 558, kw = 76, kh = 108;
        addKnob({870, row, kw, kh}, C::Integration, "INTEGRATION", "Integration: how long the readings average (short = lively, long = steady).")
            .caption = [this] {
            const float v = value(C::Integration);
            return std::string(v <= 120.0f ? "fast" : v <= 700.0f ? "averaging" : "slow average");
        };
        addKnob({870 + kw, row, kw, kh}, C::Hold, "HOLD", "Hold: how long the lowest and highest readings stay marked (Infinite: until Reset).")
            .caption = [] { return std::string("extremes"); };
        auto& zoom = addKnob({870 + 2 * kw, row, kw, kh}, C::ScopeZoom, "ZOOM", "Zoom: the scope's gain. Turning it switches Auto zoom off.");
        zoom.driven = [this] { return value(C::ScopeAuto) >= 0.5f; };
        zoom.shown = [this] { return zoomDb(); };
        zoom.caption = [this] { return std::string(value(C::ScopeAuto) >= 0.5f ? "auto" : "scope gain"); };
        return;
    }
    // Compact: the device chain.
    addArea({8, 8, 154, 154}, "The scope: mono is vertical, unrelated sides fill a disc, horizontal cancels in mono.");
    addArea(bandsPlot(), "");
    addListen({508, 8, 92, 154}, {"Stereo", "Mono", "Side", "Left", "Right"}, true, 3, listenTip);
    addKnob({608, 4, 74, 80}, C::Integration, "INTEGR.", "Integration: how long the readings average.");
    addKnob({684, 4, 74, 80}, C::Hold, "HOLD", "Hold: how long the extremes stay marked.");
    addButton({610, 92, 70, 30}, "Freeze", freeze, "Freeze: holds every reading on screen.").lit = [this] { return meter_.frozen(); };
    addButton({686, 92, 70, 30}, "Reset", reset, "Reset: clears the held extremes and the history.");
    addSegments({610, 130, 146, 30}, C::ScopeMode, {"Lissajous", "Polar"}, false, 3, "The scope: Lissajous or Polar.");
}

// ------------------------------------------------------------------ paint
void CorrelationFace::paintScope(SkCanvas& c, const Rect& r, bool compact)
{
    pro::well(c, r, 6);
    const bool polar = std::lround(value(C::ScopeMode)) == C::Polar;
    const float gain = std::pow(10.0f, zoomDb() / 20.0f);
    const float pad = compact ? 8.0f : 14.0f;
    const SkColor grid = 0xFF232832, gridSoft = 0xFF1A1E25;
    const float label = compact ? 8.5f : 10.0f;
    float cx = r.x + r.w * 0.5f, cy = 0, unit = 0, base = 0;
    if (! polar)
    {
        const float h = std::min(r.w, r.h) * 0.5f - pad;
        cy = r.y + r.h * 0.5f;
        unit = h / kSqrt2 * gain;
        // The full-scale diamond (every sample of a -1..1 signal lands inside), half scale and the axes.
        for (const int ring : {0, 1})
        {
            const float s = ring == 0 ? 1.0f : 0.5f;
            SkPathBuilder d;
            d.moveTo(cx, cy - h * s);
            d.lineTo(cx + h * s, cy);
            d.lineTo(cx, cy + h * s);
            d.lineTo(cx - h * s, cy);
            d.close();
            c.drawPath(d.detach(), pro::stroke(ring == 0 ? grid : gridSoft, 1.0f));
        }
        c.drawLine(cx, cy - h, cx, cy + h, pro::stroke(grid, 1.0f));
        c.drawLine(cx - h, cy, cx + h, cy, pro::stroke(gridSoft, 1.0f));
        const float d = h * 0.5f;
        c.drawLine(cx - d, cy - d, cx + d, cy + d, pro::stroke(gridSoft, 1.0f));
        c.drawLine(cx + d, cy - d, cx - d, cy + d, pro::stroke(gridSoft, 1.0f));
        pro::text(c, "M", {cx - 10, cy - h - (compact ? 1.0f : 3.0f) - label, 20, label + 2}, pro::kFaint, label, Align::Centre, true);
        pro::text(c, "L", {cx - d - 18, cy - d - 16, 14, label + 2}, pro::kFaint, label, Align::Centre, true);
        pro::text(c, "R", {cx + d + 4, cy - d - 16, 14, label + 2}, pro::kFaint, label, Align::Centre, true);
        if (! compact)
        {
            pro::text(c, "S", {cx - h - 16, cy - 7, 14, 14}, pro::kFaint, label, Align::Centre, true);
            pro::text(c, "S", {cx + h + 2, cy - 7, 14, 14}, pro::kFaint, label, Align::Centre, true);
        }
    }
    else
    {
        const float radius = std::min(r.w * 0.5f - pad, r.h - 2.0f * pad - (compact ? 4.0f : 12.0f));
        base = r.y + r.h - pad - (compact ? 2.0f : 10.0f);
        unit = radius / kSqrt2 * gain;
        const SkRect outer = SkRect::MakeXYWH(cx - radius, base - radius, 2 * radius, 2 * radius);
        const SkRect inner = SkRect::MakeXYWH(cx - radius * 0.5f, base - radius * 0.5f, radius, radius);
        c.drawArc(outer, 180, 180, false, pro::stroke(grid, 1.0f));
        c.drawArc(inner, 180, 180, false, pro::stroke(gridSoft, 1.0f));
        c.drawLine(cx - radius, base, cx + radius, base, pro::stroke(grid, 1.0f));
        for (const int deg : {-45, 0, 45})
        {
            const float a = (float) deg * 3.14159265f / 180.0f;
            c.drawLine(cx, base, cx + std::sin(a) * radius, base - std::cos(a) * radius, pro::stroke(deg == 0 ? grid : gridSoft, 1.0f));
        }
        const float d = radius * 0.7071f;
        pro::text(c, "M", {cx - 10, base - radius - label - 3, 20, label + 2}, pro::kFaint, label, Align::Centre, true);
        pro::text(c, "L", {cx - d - 18, base - d - 14, 14, label + 2}, pro::kFaint, label, Align::Centre, true);
        pro::text(c, "R", {cx + d + 4, base - d - 14, 14, label + 2}, pro::kFaint, label, Align::Centre, true);
        if (! compact)
        {
            pro::text(c, "out of phase", {r.x + 8, base + 1, 120, 12}, paint::withAlpha(kBad, 170), 9.0f, Align::Left, true);
            pro::text(c, "out of phase", {r.x + r.w - 128, base + 1, 120, 12}, paint::withAlpha(kBad, 170), 9.0f, Align::Right, true);
        }
    }

    // The samples: four ages, the newest brightest, added light on light so busy areas glow.
    if (points_ > 1 && ! bypassed_)
    {
        c.save();
        c.clipRect(SkRect::MakeXYWH(r.x + 1, r.y + 1, r.w - 2, r.h - 2));
        for (int i = 0; i < points_; ++i)
        {
            float x = x_[(size_t) i], y = y_[(size_t) i];
            if (polar && y < 0) { x = -x; y = -y; }
            pts_[(size_t) i] = SkPoint::Make(cx + x * unit, (polar ? base : cy) - y * unit);
        }
        const SkColor trace = reading_.frozen ? mix(kAccent, 0xFF5A6070, 0.55f) : kAccent;
        const uint8_t alphas[4] = {34, 64, 120, 230};
        const int ages = 4;
        for (int a = 0; a < ages; ++a)
        {
            const int from = points_ * a / ages, to = std::min(points_, points_ * (a + 1) / ages + 1);
            if (to - from < 2) continue;
            SkPaint p = pro::stroke(paint::withAlpha(trace, alphas[a]), polar ? (compact ? 1.6f : 2.0f) : (compact ? 0.9f : 1.2f));
            p.setBlendMode(SkBlendMode::kPlus);
            c.drawPoints(polar ? SkCanvas::kPoints_PointMode : SkCanvas::kPolygon_PointMode, {pts_.data() + from, (size_t) (to - from)}, p);
        }
        c.restore();
    }
    if (! compact)
    {
        const std::string zoom = (value(C::ScopeAuto) >= 0.5f ? "auto " : "zoom ") + decibels(zoomDb());
        const float w = pro::textWidth(zoom, 10, true) + 14;
        const Rect plate{r.x + r.w - w - 6, r.y + 6, w, 18};
        pro::rounded(c, plate, 4, 0xE0151820);
        pro::text(c, zoom, plate, pro::kDim, 10, Align::Centre, true);
    }
}

void CorrelationFace::paintMeter(SkCanvas& c, const Rect& t, bool labels)
{
    const auto xOf = [&t](float v) { return t.x + (std::clamp(v, -1.0f, 1.0f) + 1.0f) * 0.5f * t.w; };
    const float radius = std::min(6.0f, t.h * 0.35f);
    pro::rounded(c, t, radius, 0xFF0E1115);
    {
        SkPaint zones = pro::fill(0xFFFFFFFF);
        const SkPoint ends[2] = {{t.x, t.y}, {t.x + t.w, t.y}};
        const SkColor colours[4] = {paint::withAlpha(kBad, 46), paint::withAlpha(kWarn, 34), paint::withAlpha(kGood, 34), paint::withAlpha(kGood, 46)};
        const SkScalar stops[4] = {0.0f, 0.5f, 0.75f, 1.0f};
        zones.setShader(SkGradientShader::MakeLinear(ends, colours, stops, 4, SkTileMode::kClamp));
        c.drawRRect(SkRRect::MakeRectXY(SkRect::MakeXYWH(t.x, t.y, t.w, t.h), radius, radius), zones);
    }
    for (int k = -10; k <= 10; ++k)
    {
        const float x = xOf((float) k / 10.0f);
        const bool major = k % 5 == 0;
        c.drawLine(x, major ? t.y + 2 : t.y + t.h - 5, x, t.y + t.h - 2, pro::stroke(major ? 0x50FFFFFF : 0x28FFFFFF, 1.0f));
    }
    pro::roundedStroke(c, t, radius, 0xFF262B34, 1.0f);
    const auto& r = reading_;
    const bool live = r.signal && ! bypassed_;
    if (live)
    {
        // Held extremes: a band between them and a marker at each end.
        const float lo = xOf(r.minHeld), hi = xOf(r.maxHeld);
        if (hi - lo > 1.0f) c.drawRect(SkRect::MakeLTRB(lo, t.y + 1, hi, t.y + t.h - 1), pro::fill(0x16FFFFFF));
        for (const auto& [x, v] : {std::pair<float, float>{lo, r.minHeld}, {hi, r.maxHeld}})
        {
            SkPathBuilder m;
            const float s = std::max(3.5f, t.h * 0.22f);
            m.moveTo(x - s, t.y - s - 1);
            m.lineTo(x + s, t.y - s - 1);
            m.lineTo(x, t.y - 1);
            m.close();
            c.drawPath(m.detach(), pro::fill(colourFor(v)));
        }
        // The value: a bar from 0 and the needle with a glow.
        const SkColor colour = colourFor(r.correlation);
        const float zero = xOf(0), x = xOf(r.correlation), inset = std::max(3.0f, t.h * 0.2f);
        c.drawRect(SkRect::MakeLTRB(std::min(zero, x), t.y + inset, std::max(zero, x), t.y + t.h - inset), pro::fill(paint::withAlpha(colour, 190)));
        c.drawLine(x, t.y - 3, x, t.y + t.h + 3, pro::stroke(paint::withAlpha(colour, 70), std::max(6.0f, t.h * 0.3f)));
        c.drawLine(x, t.y - 3, x, t.y + t.h + 3, pro::stroke(0xFFF4F6FA, std::max(2.0f, t.h * 0.09f)));
    }
    else
        pro::text(c, bypassed_ ? "off" : "no signal", t, pro::kFaint, std::clamp(t.h * 0.5f, 9.0f, 12.0f), Align::Centre, true);
    if (! labels) return;
    const std::pair<float, const char*> marks[5] = {{-1.0f, "-1"}, {-0.5f, "-0.5"}, {0.0f, "0"}, {0.5f, "+0.5"}, {1.0f, "+1"}};
    for (const auto& [v, text] : marks)
        pro::text(c, text, {xOf(v) - 30, t.y + t.h + 4, 60, 14}, pro::kDim, 10.5f, Align::Centre, true);
    pro::text(c, "out of phase", {t.x, t.y + t.h + 18, 160, 13}, paint::withAlpha(kBad, 200), 9.5f, Align::Left, true);
    pro::text(c, "unrelated", {xOf(0) - 80, t.y + t.h + 18, 160, 13}, paint::withAlpha(kWarn, 200), 9.5f, Align::Centre, true);
    pro::text(c, "mono", {t.x + t.w - 160, t.y + t.h + 18, 160, 13}, paint::withAlpha(kGood, 200), 9.5f, Align::Right, true);
}

void CorrelationFace::paintBands(SkCanvas& c, const Rect& r, bool compact)
{
    pro::well(c, r, compact ? 4.0f : 6.0f);
    const float left = compact ? 4.0f : 30.0f, bottom = compact ? 4.0f : 20.0f;
    const Rect p{r.x + left, r.y + 6, r.w - left - 6, r.h - 6 - bottom};
    const float zero = p.y + p.h * 0.5f, scale = p.h * 0.5f;
    const float slot = p.w / (float) C::kBands;
    // The low end, where a mono sum hurts most.
    int lowBands = 0;
    while (lowBands < C::kBands && C::bandCentre(lowBands) <= C::kLowEndHz * 1.01f) ++lowBands;
    c.drawRect(SkRect::MakeXYWH(p.x, p.y, slot * (float) lowBands, p.h), pro::fill(paint::withAlpha(kLow, 12)));
    if (! compact) pro::text(c, "LOW END", {p.x + 4, p.y + 2, slot * (float) lowBands - 8, 12}, paint::withAlpha(kLow, 170), 9, Align::Left, true);
    for (int step = 2; step >= -2; --step) // +1, +0.5, 0, -0.5, -1
    {
        const float v = (float) step * 0.5f, y = zero - v * scale;
        c.drawLine(p.x, y, p.x + p.w, y, pro::stroke(step == 0 ? 0xFF2C323C : 0xFF1A1E25, 1.0f));
        if (! compact) pro::text(c, step == 0 ? "0" : format("%+.1g", v), {r.x + 2, y - 7, left - 6, 14}, pro::kFaint, 9, Align::Right);
    }
    // Levels: a faint spectrum behind the bars (-90 .. 0 dBFS over the plot's height).
    float loudest = -200.0f;
    for (const auto& b : bands_)
        if (b.signal) loudest = std::max(loudest, b.levelDb);
    if (loudest > -150.0f && ! bypassed_)
    {
        SkPathBuilder level;
        level.moveTo(p.x, p.y + p.h);
        for (int i = 0; i < C::kBands; ++i)
        {
            const float db = bands_[(size_t) i].active ? std::clamp(bands_[(size_t) i].levelDb, -90.0f, 0.0f) : -90.0f;
            level.lineTo(p.x + slot * ((float) i + 0.5f), p.y + p.h - (db + 90.0f) / 90.0f * p.h);
        }
        level.lineTo(p.x + p.w, p.y + p.h);
        level.close();
        SkPaint shade = pro::fill(0xFFFFFFFF);
        const SkPoint ends[2] = {{0, p.y}, {0, p.y + p.h}};
        const SkColor colours[2] = {paint::withAlpha(kAccent, 44), paint::withAlpha(kAccent, 6)};
        shade.setShader(SkGradientShader::MakeLinear(ends, colours, nullptr, 2, SkTileMode::kClamp));
        c.drawPath(level.detach(), shade);
    }
    const int hover = bandAt(designMouseX(), designMouseY());
    for (int i = 0; i < C::kBands; ++i)
    {
        const auto& b = bands_[(size_t) i];
        const float x = p.x + slot * (float) i, w = std::max(2.0f, slot * 0.62f), bx = x + (slot - w) * 0.5f;
        if (i == hover) pro::rounded(c, {x + 1, p.y, slot - 2, p.h}, 3, 0x14FFFFFF);
        if (! b.active || ! b.signal || bypassed_)
        {
            c.drawCircle(x + slot * 0.5f, zero, compact ? 1.2f : 1.8f, pro::fill(b.active ? 0xFF3A404B : 0xFF262B33));
            continue;
        }
        const float strength = std::clamp(1.0f + (b.levelDb - loudest) / 48.0f, 0.22f, 1.0f);
        const SkColor colour = colourFor(b.correlation);
        const float y = zero - std::clamp(b.correlation, -1.0f, 1.0f) * scale;
        const float top = std::min(zero, y), bottomY = std::max(zero, y);
        pro::rounded(c, {bx, top, w, std::max(1.5f, bottomY - top)}, std::min(2.5f, w * 0.3f), paint::withAlpha(colour, (uint8_t) (255.0f * strength)));
        if (b.minHeld < b.correlation - 0.02f && strength > 0.45f) // quiet bands wander: their holds would only add noise
        {
            const float m = zero - std::clamp(b.minHeld, -1.0f, 1.0f) * scale;
            c.drawLine(bx - 1, m, bx + w + 1, m, pro::stroke(paint::withAlpha(colourFor(b.minHeld), (uint8_t) (200.0f * strength)), compact ? 1.2f : 1.8f));
        }
    }
    if (compact) return;
    for (int i = 0; i < C::kBands; i += 3)
        pro::text(c, C::bandLabel(i), {p.x + slot * ((float) i + 0.5f) - 24, p.y + p.h + 4, 48, 13}, i == hover ? pro::kText : pro::kFaint, 9.5f,
                  Align::Centre, true);
}

void CorrelationFace::paintHistory(SkCanvas& c, const Rect& r)
{
    pro::well(c, r, 6);
    const Rect p{r.x + 30, r.y + 8, r.w - 38, r.h - 26};
    const float zero = p.y + p.h * 0.5f, scale = p.h * 0.5f;
    for (int step = 2; step >= -2; --step) // +1, +0.5, 0, -0.5, -1
    {
        const float v = (float) step * 0.5f, y = zero - v * scale;
        c.drawLine(p.x, y, p.x + p.w, y, pro::stroke(step == 0 ? 0xFF2C323C : 0xFF1A1E25, 1.0f));
        if (step % 2 == 0) pro::text(c, step == 0 ? "0" : format("%+.0f", v), {r.x + 2, y - 7, 24, 14}, pro::kFaint, 9, Align::Right);
    }
    const int count = (int) meter_.historyCount();
    const float step = p.w / (float) (C::kHistory - 1);
    SkPathBuilder line, area, low;
    bool open = false, lowOpen = false;
    float runStart = 0, lastX = 0;
    const auto closeArea = [&] {
        if (! open) return;
        area.lineTo(lastX, zero);
        area.lineTo(runStart, zero);
        area.close();
        open = false;
    };
    for (int i = count - 1; i >= 0; --i) // oldest first, left to right
    {
        const auto h = meter_.history(i);
        const float x = p.x + p.w - (float) i * step;
        if (h.signal)
        {
            const float y = zero - std::clamp(h.correlation, -1.0f, 1.0f) * scale;
            if (! open)
            {
                line.moveTo(x, y);
                area.moveTo(x, zero);
                area.lineTo(x, y);
                runStart = x;
                open = true;
            }
            else
            {
                line.lineTo(x, y);
                area.lineTo(x, y);
            }
            lastX = x;
        }
        else closeArea();
        if (h.lowSignal)
        {
            const float y = zero - std::clamp(h.lowEnd, -1.0f, 1.0f) * scale;
            if (! lowOpen) low.moveTo(x, y);
            else low.lineTo(x, y);
            lowOpen = true;
        }
        else lowOpen = false;
    }
    closeArea();
    const auto areaPath = area.detach(), linePath = line.detach(), lowPath = low.detach();
    for (const bool above : {true, false})
    {
        c.save();
        c.clipRect(above ? SkRect::MakeLTRB(p.x, p.y - 2, p.x + p.w + 2, zero) : SkRect::MakeLTRB(p.x, zero, p.x + p.w + 2, p.y + p.h + 2));
        const SkColor colour = above ? kGood : kBad;
        c.drawPath(areaPath, pro::fill(paint::withAlpha(colour, above ? 40 : 70)));
        c.drawPath(linePath, pro::stroke(colour, 1.7f));
        c.restore();
    }
    c.drawPath(lowPath, pro::stroke(paint::withAlpha(kLow, 215), 1.4f));
    pro::text(c, "8 s ago", {p.x, p.y + p.h + 4, 80, 13}, pro::kFaint, 9.5f, Align::Left, true);
    pro::text(c, "4 s", {p.x + p.w * 0.5f - 40, p.y + p.h + 4, 80, 13}, pro::kFaint, 9.5f, Align::Centre, true);
    pro::text(c, "now", {p.x + p.w - 80, p.y + p.h + 4, 80, 13}, pro::kFaint, 9.5f, Align::Right, true);
}

void CorrelationFace::paintStereo(SkCanvas& c, const Rect& r)
{
    const auto& v = reading_;
    const bool live = v.signal && ! bypassed_;
    const float x = r.x + 14, w = r.w - 28;
    const auto row = [&](float y, const char* title, const std::string& value, SkColor valueColour) {
        pro::text(c, title, {x, y, w, 14}, pro::kFaint, 9.5f, Align::Left, true);
        pro::text(c, value, {x, y, w, 14}, valueColour, 11, Align::Right, true);
    };
    // Balance: +-12 dB around the centre.
    float y = r.y + 30;
    std::string balance = "centre";
    if (live && std::abs(v.balanceDb) >= 0.1f) balance = (v.balanceDb > 0 ? "R " : "L ") + format("+%.1f dB", std::abs(v.balanceDb));
    row(y, "BALANCE", live ? balance : "-", pro::kText);
    Rect bar{x + 12, y + 17, w - 24, 8};
    pro::rounded(c, bar, 4, 0xFF0E1115);
    c.drawLine(bar.x + bar.w * 0.5f, bar.y - 2, bar.x + bar.w * 0.5f, bar.y + bar.h + 2, pro::stroke(0xFF3A404B, 1.0f));
    if (live)
    {
        const float to = bar.x + bar.w * 0.5f * (1.0f + std::clamp(v.balanceDb / 12.0f, -1.0f, 1.0f));
        const float from = bar.x + bar.w * 0.5f;
        pro::rounded(c, {std::min(from, to), bar.y + 1, std::max(2.0f, std::abs(to - from)), bar.h - 2}, 3, kAccent);
    }
    pro::text(c, "L", {x, bar.y - 3, 10, 14}, pro::kFaint, 9.5f, Align::Left, true);
    pro::text(c, "R", {x + w - 10, bar.y - 3, 10, 14}, pro::kFaint, 9.5f, Align::Right, true);
    // Width: side against mid, 0 .. 200 %.
    y += 40;
    const float width = std::min(v.width, 2.0f);
    row(y, "WIDTH", live ? format("%.0f%%", (double) std::min(v.width, 9.99f) * 100.0) : "-", live && v.width > 1.0f ? kBad : pro::kText);
    bar = {x, y + 17, w, 8};
    pro::rounded(c, bar, 4, 0xFF0E1115);
    if (live && width > 0.002f)
        pro::rounded(c, {bar.x, bar.y + 1, std::max(2.0f, bar.w * width * 0.5f), bar.h - 2}, 3, width > 1.0f ? kBad : kAccent);
    c.drawLine(bar.x + bar.w * 0.5f, bar.y - 2, bar.x + bar.w * 0.5f, bar.y + bar.h + 2, pro::stroke(0xFF5A6070, 1.0f));
    // The mono sum and the levels.
    y += 40;
    const SkColor monoColour = ! live ? pro::kDim : v.monoDb >= -2.0f ? kGood : v.monoDb >= -4.5f ? kWarn : kBad;
    row(y, "MONO SUM", live ? decibels(v.monoDb) : "-", monoColour);
    const auto level = [](float db) { return db <= -119.0f ? std::string("-inf") : format("%.1f", db); };
    pro::text(c, "L " + level(v.levelL) + "   R " + level(v.levelR) + " dB RMS", {x, y + 18, w, 13}, pro::kFaint, 9.5f, Align::Left, true);
}

void CorrelationFace::paintListenWarning(SkCanvas& c, const Rect& r, float size)
{
    const int listen = (int) std::lround(value(C::Listen));
    if (listen == C::ListenStereo) return;
    static const char* const names[] = {"", "MONO", "SIDE", "LEFT", "RIGHT"};
    const std::string text = std::string("LISTENING: ") + names[std::clamp(listen, 0, 4)] + "  \xC2\xB7  the output is not the input";
    pro::rounded(c, r, 5, paint::withAlpha(kWarn, 34));
    pro::roundedStroke(c, r, 5, paint::withAlpha(kWarn, 150), 1.0f);
    pro::text(c, text, r, kWarn, size, Align::Centre, true);
}

void CorrelationFace::paintBack(SkCanvas& c, Layout layout)
{
    const auto v = verdict();
    const auto& r = reading_;
    const bool live = r.signal && ! bypassed_;
    if (layout == Layout::Compact)
    {
        paintScope(c, {8, 8, 154, 154}, true);
        pro::well(c, {170, 8, 330, 154}, 6);
        pro::text(c, live ? signedValue(r.correlation) : "-", {178, 10, 92, 30}, live ? colourFor(r.correlation) : pro::kFaint, 23, Align::Left, true);
        pro::text(c, v.title, {268, 13, 226, 15}, v.colour, 10.5f, Align::Left, true);
        const std::string sub = live ? "mono sum " + decibels(r.monoDb) + "  \xC2\xB7  held " + signedValue(r.minHeld) + " \xE2\x80\xA6 " + signedValue(r.maxHeld)
                                     : std::string("correlation");
        pro::text(c, sub, {268, 28, 226, 13}, pro::kDim, 9, Align::Left);
        paintMeter(c, {178, 46, 314, 16}, false);
        paintBands(c, bandsPlot(), true);
        return;
    }
    pro::wordmark(c, "correlation", 24, 10, 28, kAccent, version);
    pro::text(c, "LISTEN", {600, 2, 100, 13}, pro::kFaint, 9, Align::Left, true);
    // Verdict.
    pro::panel(c, {16, 64, 1088, 60});
    pro::rounded(c, {16, 64, 5, 60}, 2, v.colour);
    pro::text(c, v.title, {34, 70, 1060, 26}, v.colour, 19, Align::Left, true);
    pro::text(c, v.detail, {34, 97, 1060, 20}, pro::kDim, 12.5f, Align::Left);
    // The meter.
    pro::panel(c, {16, 132, 1088, 96});
    pro::sectionTitle(c, "CORRELATION", 28, 138, kAccent);
    pro::text(c, live ? signedValue(r.correlation) : "-", {28, 158, 180, 46}, live ? colourFor(r.correlation) : pro::kFaint, 40, Align::Left, true);
    pro::text(c, live ? "held " + signedValue(r.minHeld) + "  \xE2\x80\xA6  " + signedValue(r.maxHeld) : std::string("held -"), {30, 204, 190, 16},
              pro::kDim, 10.5f, Align::Left, true);
    paintMeter(c, {226, 160, 862, 26}, true);
    // Scope.
    pro::panel(c, {16, 236, 340, 290});
    pro::sectionTitle(c, "SCOPE", 28, 242, kAccent);
    paintScope(c, {28, 260, 316, 226}, false);
    // Bands.
    pro::panel(c, {368, 236, 736, 290});
    pro::sectionTitle(c, "BANDS", 380, 242, kAccent);
    pro::text(c, "correlation per third octave over its level", {470, 242, 400, 18}, pro::kFaint, 10, Align::Left, true);
    {
        float lx = 900;
        for (const auto& [name, colour] : {std::pair<const char*, SkColor>{"mono-safe", kGood}, {"wide", kWarn}, {"cancels", kBad}})
        {
            c.drawCircle(lx + 3, 251, 3, pro::fill(colour));
            pro::text(c, name, {lx + 9, 243, 80, 16}, pro::kDim, 9.5f, Align::Left, true);
            lx += pro::textWidth(name, 9.5f, true) + 22;
        }
    }
    paintBands(c, bandsPlot(), false);
    // History.
    pro::panel(c, {16, 534, 560, 156});
    pro::sectionTitle(c, "HISTORY", 28, 540, kAccent);
    {
        float lx = 350;
        for (const auto& [name, colour] : {std::pair<const char*, SkColor>{"all", kGood}, {"low end", kLow}})
        {
            c.drawCircle(lx + 3, 549, 3, pro::fill(colour));
            pro::text(c, name, {lx + 9, 541, 90, 16}, pro::kDim, 9.5f, Align::Left, true);
            lx += pro::textWidth(name, 9.5f, true) + 24;
        }
        pro::text(c, format("(below %.0f Hz)", C::kLowEndHz), {lx - 11, 541, 110, 16}, pro::kFaint, 9.5f, Align::Left, true);
    }
    paintHistory(c, {28, 562, 536, 120});
    // Stereo.
    pro::panel(c, {588, 534, 262, 156});
    pro::sectionTitle(c, "STEREO", 600, 540, kAccent);
    paintStereo(c, {588, 534, 262, 156});
    // Meter controls.
    pro::panel(c, {862, 534, 242, 156});
    pro::sectionTitle(c, "METER", 874, 540, kAccent);
    c.drawLine(16, 696, 1104, 696, pro::stroke(pro::kLineSoft, 1.0f));
}

void CorrelationFace::paintFront(SkCanvas& c, Layout layout)
{
    if (layout == Layout::Compact)
    {
        if (reading_.frozen) pro::text(c, "FROZEN", {178, 140, 80, 14}, kWarn, 9, Align::Left, true);
        return;
    }
    paintListenWarning(c, {250, 18, 336, 26}, 10.5f);
    if (reading_.frozen)
    {
        const Rect chip{226, 136, 80, 18};
        pro::rounded(c, chip, 4, paint::withAlpha(kWarn, 40));
        pro::text(c, "FROZEN", chip, kWarn, 10, Align::Centre, true);
    }
    paintHelp(c, {20, 700, 1080, 18});
}
} // namespace sonora::ui
