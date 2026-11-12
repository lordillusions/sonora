#include "ui/views/PhaseAlignFace.h"

#include "include/core/SkPathBuilder.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

namespace sonora::ui
{
namespace
{
using P = devices::PhaseAlign;
constexpr float kPi = 3.14159265f;

std::string format(const char* pattern, double value)
{
    char buffer[128];
    std::snprintf(buffer, sizeof buffer, pattern, value);
    return buffer;
}

std::string degrees(float d) { return format(std::abs(d) < 0.5f ? "%.0f\xC2\xB0" : "%+.0f\xC2\xB0", d); }
std::string decibels(float d) { return format(std::abs(d) < 0.05f ? "%.1f dB" : "%+.1f dB", d); }

float peakOf(const float* v, int n)
{
    float p = 0;
    for (int i = 0; i < n; ++i) p = std::max(p, std::abs(v[i]));
    return p;
}

void line(SkCanvas& c, const float* v, int n, const Rect& r, float scale, SkColor colour, float width)
{
    if (n < 2) return;
    SkPathBuilder p;
    for (int i = 0; i < n; ++i)
    {
        const float x = r.x + r.w * (float) i / (float) (n - 1);
        const float y = r.y + r.h * 0.5f - std::clamp(v[i] * scale, -1.0f, 1.0f) * r.h * 0.46f;
        if (i == 0) p.moveTo(x, y);
        else p.lineTo(x, y);
    }
    c.drawPath(p.detach(), pro::stroke(colour, width));
}
} // namespace

PhaseAlignFace::PhaseAlignFace(devices::PhaseAlign& device) : ProFace(device, kFull, kCompact, kAccent), phase_(device) {}

std::string PhaseAlignFace::kickLabel() const
{
    if (! sidechainName) return "Kick: host sidechain input";
    const auto name = sidechainName();
    return name.empty() ? std::string("Kick: choose a track \xE2\x96\xBE") : "Kick: " + name + " \xE2\x96\xBE";
}

std::string PhaseAlignFace::verdictTitle() const
{
    switch (phase_.verdict())
    {
        case P::NoKick: return "No kick to listen to";
        case P::Waiting: return "Listening for the kick\xE2\x80\xA6";
        case P::Silent: return "Nothing to align: the bass is silent while the kick plays";
        case P::Unrelated: return "Nothing to align: kick and bass don't share a frequency";
        case P::InPhase: return "Already in phase";
        case P::Aligning: return decibels(phase_.lastGainDb() > 0.05f ? phase_.lastGainDb() : phase_.lastPotentialDb()) + " more low end on every kick";
        case P::ManualAngle: return "Manual: the bass below " + valueText(P::Crossover, value(P::Crossover)) + " turned by " + degrees(value(P::Angle));
    }
    return {};
}

std::string PhaseAlignFace::verdictDetail() const
{
    const std::string crossover = valueText(P::Crossover, value(P::Crossover));
    switch (phase_.verdict())
    {
        case P::NoKick:
            return sidechainName ? "Choose the kick's track (Kick \xE2\x96\xBE above, or the device's sidechain arrow). Until then the bass passes untouched."
                                 : "Route the kick into Phase Align's sidechain input in your host. Until then the bass passes untouched.";
        case P::Waiting: return "Play the song: every kick hit is measured against the bass.";
        case P::Silent:
            return "Bass under the kick: " + format("%.0f dB", std::max(-120.0f, phase_.lastBassUnderKickDb())) +
                   ". Kick and bass take turns (an off-beat or ducked bass), so the bass passes untouched.";
        case P::Unrelated:
            return "Shared low end: " + format("%.0f%%", phase_.lastCoherence() * 100.0) +
                   ". They sound together at different pitches: no angle adds low end, so the bass passes untouched.";
        case P::InPhase:
            return "The best angle would add only " + decibels(phase_.lastPotentialDb()) + ": the bass passes untouched.";
        case P::Aligning:
            return "Turning the bass below " + crossover + " by " + degrees(phase_.currentAngle()) + " while the kick sounds (the best possible: " +
                   decibels(phase_.lastPotentialDb()) + ").";
        case P::ManualAngle:
            return "This angle gives " + decibels(phase_.lastGainDb()) + " on the last kick; the best would give " + decibels(phase_.lastPotentialDb()) + ".";
    }
    return {};
}

SkColor PhaseAlignFace::verdictColour() const
{
    switch (phase_.verdict())
    {
        case P::NoKick: return kBad;
        case P::Waiting: return pro::kDim;
        case P::Silent:
        case P::Unrelated: return kWarn;
        case P::InPhase: return kGood;
        case P::Aligning: return kGood;
        case P::ManualAngle: return kAccent;
    }
    return pro::kText;
}

std::string PhaseAlignFace::valueText(int param, float v) const
{
    if (param == P::Angle) return degrees(v);
    return ProFace::valueText(param, v);
}

std::string PhaseAlignFace::idleHelp() const { return verdictDetail(); }

bool PhaseAlignFace::animate()
{
    bool dirty = phase_.takeShape(shape_, shape_.serial);
    const auto hits = phase_.hitCount();
    const float angle = phase_.currentAngle(), target = phase_.targetAngle(), mix = phase_.processedAmount();
    const int verdict = (int) phase_.verdict();
    dirty = dirty || hits != hits_ || std::abs(angle - angle_) > 0.05f || std::abs(target - target_) > 0.05f || std::abs(mix - mix_) > 0.005f ||
            verdict != verdict_;
    hits_ = hits;
    angle_ = angle;
    target_ = target;
    mix_ = mix;
    verdict_ = verdict;
    return dirty;
}

void PhaseAlignFace::build(Layout layout)
{
    const auto kickButton = [this](const Rect& r, float text) {
        auto& b = addButton(r, "", [this] { if (onChooseSidechain) onChooseSidechain(); },
                            onChooseSidechain ? "Kick: the track Phase Align listens to (the sidechain). Click to choose another."
                                              : "Kick: your host's sidechain input. Route the kick track into it.");
        b.textSize = text;
        b.colour = kKick;
    };
    if (layout == Layout::Full)
    {
        kickButton({780, 14, 304, 32}, 13);
        addSegments({28, 368, 276, 28}, P::ModeParam, {"Auto", "Hold", "Manual"}, false, 4,
                    "Auto follows every kick; Hold keeps the angle it learned; Manual uses the Angle knob.");
        addArea({328, 140, 480, 272}, "The last kick: the kick (orange), the bass before (grey) and after (blue), each scaled to be seen; below, kick + bass as it was and aligned.");
        addArea({820, 140, 264, 272}, "One bar per kick: the outline is what the best angle could add, the fill what it adds now.");
        const float row = 440, kw = 92, kh = 108;
        auto& angle = addKnob({28, row, kw, kh}, P::Angle, "ANGLE", "Angle: the rotation in Manual (180 = polarity flip).");
        angle.bipolar = true;
        angle.caption = [] { return std::string("manual"); };
        addKnob({28 + kw, row, kw, kh}, P::Speed, "SPEED", "Speed: how fast a new angle is reached (shorter = quicker, longer = smoother).")
            .caption = [] { return std::string("glide"); };
        addKnob({28 + 2 * kw, row, kw, kh}, P::Crossover, "CROSSOVER", "Crossover: only the bass below it is rotated. Keep it just above where kick and bass overlap.")
            .caption = [] { return std::string("rotated below"); };
        addKnob({340, row, kw, kh}, P::Threshold, "THRESHOLD", "Threshold: how loud the kick must be to count as a hit.").caption = [] {
            return std::string("kick level");
        };
        addKnob({340 + kw, row, kw, kh}, P::Window, "WINDOW", "Window: how long after each kick hit kick and bass are compared (cover the kick's tail).")
            .caption = [] { return std::string("after the hit"); };
        addKnob({340 + 2 * kw, row, kw, kh}, P::MinCoherence, "MIN. SHARED", "Min. shared: hits where kick and bass share less low end than this are ignored.")
            .caption = [] { return std::string("coherence"); };
        addSegments({652, 474, 300, 30}, P::Listen, {"Bass", "Kick + Bass", "Original + Kick"}, false, 4,
                    "Listen: the bass (normal), the aligned bass with the kick (hear the low end together), or the original bass with the kick (A/B).");
        addKnob({972, row, 100, kh}, P::Output, "OUTPUT", "Output: the bass's level.").caption = [] { return std::string("level"); };
        return;
    }
    // Compact: the device chain.
    kickButton({166, 136, 330, 26}, 11);
    addSegments({504, 8, 96, 72}, P::ModeParam, {"Auto", "Hold", "Manual"}, true, 3,
                "Auto follows every kick; Hold keeps the angle; Manual uses the Angle knob.");
    addSegments({504, 88, 96, 74}, P::Listen, {"Bass", "Kick+Bass", "Orig+Kick"}, true, 3,
                "Listen: the bass, the aligned bass with the kick, or the original with the kick (A/B).");
    addKnob({608, 4, 74, 80}, P::Crossover, "XOVER", "Crossover: only the bass below it is rotated.");
    addKnob({684, 4, 74, 80}, P::Window, "WINDOW", "Window: how long after each kick hit they are compared.");
    addKnob({608, 88, 74, 80}, P::Angle, "ANGLE", "Angle: the rotation in Manual.").bipolar = true;
    addKnob({684, 88, 74, 80}, P::Output, "OUTPUT", "Output: the bass's level.");
}

void PhaseAlignFace::paintDial(SkCanvas& c, float cx, float cy, float radius, bool compact)
{
    c.drawCircle(cx, cy, radius, pro::stroke(0xFF262B34, compact ? 6.0f : 9.0f));
    // Coherence: the ring fills as kick and bass share more.
    const float coherence = std::clamp(phase_.lastCoherence(), 0.0f, 1.0f);
    const SkRect arc = SkRect::MakeXYWH(cx - radius, cy - radius, 2 * radius, 2 * radius);
    if (coherence > 0.01f) c.drawArc(arc, -90, 360 * coherence, false, pro::stroke(paint::withAlpha(kAccent, 150), compact ? 6.0f : 9.0f));
    for (int t = 0; t < 4; ++t)
    {
        const float a = (float) t * kPi * 0.5f;
        c.drawLine(cx + std::cos(a) * radius * 0.82f, cy + std::sin(a) * radius * 0.82f, cx + std::cos(a) * radius * 0.92f,
                   cy + std::sin(a) * radius * 0.92f, pro::stroke(pro::kFaint, 1.5f));
    }
    // 0 degrees at the top, clockwise.
    const auto at = [&](float deg, float r) {
        const float a = (deg - 90.0f) * kPi / 180.0f;
        return SkPoint{cx + std::cos(a) * r, cy + std::sin(a) * r};
    };
    const auto t = at(target_, radius);
    c.drawCircle(t.fX, t.fY, compact ? 3.5f : 5.0f, pro::fill(kKick));
    const auto tip = at(angle_, radius * 0.78f);
    const SkColor needle = mix_ > 0.5f ? 0xFFFFFFFF : pro::kFaint;
    c.drawLine(cx, cy, tip.fX, tip.fY, pro::stroke(needle, compact ? 2.0f : 3.0f));
    c.drawCircle(cx, cy, compact ? 3.0f : 4.5f, pro::fill(needle));
    // The readout on a plate, so the needle never runs through it.
    const float plateW = compact ? 64.0f : 104.0f, plateH = compact ? 30.0f : 46.0f, plateY = cy + radius * 0.34f;
    pro::rounded(c, {cx - plateW * 0.5f, plateY, plateW, plateH}, 6, 0xF0151820);
    pro::text(c, degrees(angle_), {cx - plateW * 0.5f, plateY + 2, plateW, compact ? 15.0f : 24.0f}, pro::kText, compact ? 12.5f : 20.0f,
              Align::Centre, true);
    pro::text(c, mix_ > 0.5f ? "rotating" : "untouched", {cx - plateW * 0.5f, plateY + (compact ? 16.0f : 26.0f), plateW, 14},
              mix_ > 0.5f ? kAccent : pro::kFaint, compact ? 8.5f : 10.5f, Align::Centre, true);
}

void PhaseAlignFace::paintHit(SkCanvas& c, const Rect& waves, const Rect& sum, bool compact)
{
    const int n = shape_.bins;
    pro::rounded(c, waves, 4, 0xFF0E1115);
    if (sum.w > 0) pro::rounded(c, sum, 4, 0xFF0E1115);
    if (n < 2)
    {
        pro::text(c, "the last kick appears here", waves, pro::kFaint, compact ? 10.0f : 12.0f);
        return;
    }
    c.drawLine(waves.x, waves.y + waves.h * 0.5f, waves.x + waves.w, waves.y + waves.h * 0.5f, pro::stroke(0xFF1D2229, 1.0f));
    // Each trace scaled to be seen: a bass far below the kick would be a flat line otherwise.
    const float kickPeak = std::max(1.0e-9f, peakOf(shape_.kick.data(), n));
    const float bassPeak = std::max(peakOf(shape_.before.data(), n), peakOf(shape_.after.data(), n));
    const bool bassSilent = bassPeak < kickPeak * 1.0e-4f;
    const float bassScale = bassSilent ? 1.0f / kickPeak : 1.0f / std::max(bassPeak, 1.0e-9f);
    line(c, shape_.kick.data(), n, waves, 1.0f / kickPeak, kKick, compact ? 1.4f : 2.0f);
    line(c, shape_.before.data(), n, waves, bassScale, kBefore, compact ? 1.2f : 1.6f);
    line(c, shape_.after.data(), n, waves, bassScale, kAccent, compact ? 1.4f : 2.0f);
    const float label = compact ? 8.5f : 10.5f;
    const float relative = 20.0f * std::log10(std::max(bassPeak, 1.0e-12f) / kickPeak);
    const std::string note = bassSilent ? std::string("bass silent under the kick") : "bass drawn " + format("%+.0f dB", -relative);
    const float noteW = pro::textWidth(note, label, true) + 12;
    const Rect plate{waves.x + waves.w - noteW - 4, waves.y + 4, noteW, label + 6};
    pro::rounded(c, plate, 4, 0xE0151820);
    pro::text(c, note, plate, bassSilent ? kWarn : pro::kDim, label, Align::Centre, true);
    if (sum.w <= 0) return;
    // Kick + bass as it was against aligned, on one scale.
    std::array<float, P::kShapeBins> was{}, now{};
    float peak = 1.0e-9f;
    for (int i = 0; i < n; ++i)
    {
        was[(size_t) i] = shape_.kick[(size_t) i] + shape_.before[(size_t) i];
        now[(size_t) i] = shape_.kick[(size_t) i] + shape_.after[(size_t) i];
        peak = std::max(peak, std::max(std::abs(was[(size_t) i]), std::abs(now[(size_t) i])));
    }
    line(c, was.data(), n, sum, 1.0f / peak, kBefore, 1.6f);
    line(c, now.data(), n, sum, 1.0f / peak, kAccent, 2.0f);
    pro::text(c, "KICK + BASS", {sum.x + 6, sum.y + 4, 120, 14}, pro::kFaint, 10, Align::Left, true);
}

void PhaseAlignFace::paintHistory(SkCanvas& c, const Rect& r)
{
    pro::rounded(c, r, 4, 0xFF0E1115);
    const float zero = r.y + r.h - 16, top = r.y + 14;
    const float perDb = (zero - top) / 6.0f;
    for (int db = 0; db <= 6; db += 3)
    {
        const float y = zero - (float) db * perDb;
        c.drawLine(r.x + 26, y, r.x + r.w - 6, y, pro::stroke(0xFF1D2229, 1.0f));
        pro::text(c, "+" + std::to_string(db), {r.x + 2, y - 7, 22, 14}, pro::kFaint, 9, Align::Right);
    }
    const int count = (int) std::min<uint32_t>(hits_, P::kHistory);
    const float slot = (r.w - 34) / (float) P::kHistory;
    for (int i = 0; i < count; ++i)
    {
        const auto h = phase_.history(i); // newest first, drawn from the right
        const float x = r.x + r.w - 6 - slot * (float) (i + 1);
        if (! h.used)
        {
            c.drawCircle(x + slot * 0.5f, zero - 3, 1.8f, pro::fill(kWarn));
            continue;
        }
        const float potential = std::clamp(h.potentialDb, 0.0f, 6.0f) * perDb, gain = std::clamp(h.gainDb, 0.0f, 6.0f) * perDb;
        if (potential > 0.5f)
            c.drawRect(SkRect::MakeXYWH(x + 1, zero - potential, slot - 2, potential), pro::stroke(paint::withAlpha(kAccent, 120), 1.0f));
        if (gain > 0.5f) c.drawRect(SkRect::MakeXYWH(x + 1, zero - gain, slot - 2, gain), pro::fill(paint::withAlpha(kAccent, 210)));
        else c.drawLine(x + 1, zero - 1, x + slot - 1, zero - 1, pro::stroke(kAccent, 1.0f));
    }
    pro::text(c, "dB per kick", {r.x + 30, r.y + 2, r.w - 36, 12}, pro::kFaint, 9.5f, Align::Left, true);
}

void PhaseAlignFace::paintBack(SkCanvas& c, Layout layout)
{
    if (layout == Layout::Compact)
    {
        pro::well(c, {8, 8, 150, 154}, 6);
        paintDial(c, 83, 76, 54, true);
        pro::well(c, {166, 8, 330, 122}, 6);
        pro::text(c, verdictTitle(), {174, 12, 316, 18}, verdictColour(), 11.5f, Align::Left, true);
        pro::text(c, verdictDetail(), {174, 30, 316, 14}, pro::kDim, 9.0f, Align::Left);
        paintHit(c, {174, 48, 314, 76}, {}, true);
        return;
    }
    pro::wordmark(c, "phase align", 24, 10, 28, kAccent, version);
    // Verdict.
    pro::panel(c, {16, 64, 1068, 62});
    pro::rounded(c, {16, 64, 5, 62}, 2, verdictColour());
    pro::text(c, verdictTitle(), {34, 70, 1040, 28}, verdictColour(), 20, Align::Left, true);
    pro::text(c, verdictDetail(), {34, 98, 1040, 20}, pro::kDim, 12.5f, Align::Left);

    pro::panel(c, {16, 136, 300, 276});
    pro::sectionTitle(c, "PHASE", 28, 142, kAccent);
    paintDial(c, 166, 252, 92, false);
    pro::panel(c, {328, 136, 480, 276});
    pro::sectionTitle(c, "LAST KICK", 340, 142, kAccent);
    paintHit(c, {340, 166, 456, 120}, {340, 294, 456, 110}, false);
    pro::panel(c, {820, 136, 264, 276});
    pro::sectionTitle(c, "HITS", 832, 142, kAccent);
    paintHistory(c, {832, 166, 240, 200});
    pro::text(c, "bass under the kick " + format("%.0f dB", std::max(-120.0f, phase_.lastBassUnderKickDb())), {832, 372, 240, 16}, pro::kDim, 10.5f,
              Align::Left);
    pro::text(c, "shared low end " + format("%.0f%%", phase_.lastCoherence() * 100.0), {832, 388, 240, 16}, pro::kDim, 10.5f, Align::Left);

    const std::pair<const char*, Rect> panels[3] = {{"ALIGN", {16, 420, 300, 140}}, {"DETECT", {328, 420, 300, 140}}, {"OUT", {640, 420, 444, 140}}};
    for (const auto& [title, r] : panels)
    {
        pro::panel(c, r);
        pro::sectionTitle(c, title, r.x + 12, r.y + 6, kAccent);
    }
    pro::text(c, "LISTEN", {652, 454, 100, 16}, pro::kFaint, 9.5f, Align::Left, true);
    // Legend for the last hit.
    float lx = 452;
    for (const auto& [name, colour] : {std::pair<const char*, SkColor>{"kick", kKick}, {"bass before", kBefore}, {"bass after", kAccent}})
    {
        c.drawCircle(lx + 3, 151, 3, pro::fill(colour));
        pro::text(c, name, {lx + 10, 143, 90, 16}, pro::kDim, 10, Align::Left, true);
        lx += 96;
    }
    c.drawLine(16, 568, 1084, 568, pro::stroke(pro::kLineSoft, 1.0f));
}

void PhaseAlignFace::paintFront(SkCanvas& c, Layout layout)
{
    // The kick source button's label.
    for (const auto& control : controls())
        if (control.kind == Kind::Button)
            pro::text(c, kickLabel(), control.r, kKick, layout == Layout::Full ? 13.0f : 10.5f, Align::Centre, true);
    if (layout == Layout::Full) paintHelp(c, {20, 572, 1060, 20});
}
} // namespace sonora::ui
