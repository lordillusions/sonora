#include "ui/views/KickstractFace.h"

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
constexpr float kMinHz = 20, kMaxHz = 20000, kTopDb = 0, kFloorDb = -84;
constexpr SkColor kGrid = 0xff20242b, kScale = 0xff606775, kInk = 0xffeef1f4;
constexpr const char* kBandTitle[K::kBands] = {"LOW", "MID", "HIGH"};
constexpr float kAttackMs = (float) (K::kAttackSeconds * 1000.0); // the gate ramp ends at the hit

// Full layout (design grid).
constexpr Rect kSpectrumFull{246, 92, 384, 238}, kKickFull{660, 92, 494, 238}, kMeterFull{200, 98, 14, 226};
// Compact layout.
constexpr Rect kSpectrumCompact{8, 8, 318, 154}, kKickCompact{334, 8, 300, 154};

std::string format(const char* pattern, double value)
{
    char buffer[64];
    std::snprintf(buffer, sizeof buffer, pattern, value);
    return buffer;
}

std::string hzText(float hz)
{
    if (hz >= 1000) return format(hz >= 10000 ? "%.0fk" : "%.1fk", hz / 1000);
    return format("%.0f", hz);
}

std::string hzLong(float hz) { return hz >= 1000 ? format("%.2f kHz", hz / 1000) : format("%.0f Hz", hz); }

void strokePath(SkCanvas& c, SkPathBuilder& path, SkColor colour, float width)
{
    auto pen = pro::stroke(colour, width);
    pen.setStrokeJoin(SkPaint::kRound_Join);
    c.drawPath(path.detach(), pen);
}

void fillPath(SkCanvas& c, SkPathBuilder& path, SkColor colour) { c.drawPath(path.detach(), pro::fill(colour)); }

void dot(SkCanvas& c, float x, float y, float radius, SkColor fill, SkColor ring)
{
    c.drawCircle(x, y, radius, pro::fill(fill));
    c.drawCircle(x, y, radius, pro::stroke(ring, 1.4f));
}
} // namespace

KickstractFace::KickstractFace(devices::Kickstract& device) : ProFace(device, kFull, kCompact, kAccent), kickstract_(device) {}

// ---------------------------------------------------------------------------------------------------------------------
// Geometry

Rect KickstractFace::spectrumRect() const { return layoutMode() == Layout::Full ? kSpectrumFull : kSpectrumCompact; }
Rect KickstractFace::kickRect() const { return layoutMode() == Layout::Full ? kKickFull : kKickCompact; }

float KickstractFace::designXForHz(float hz) const
{
    const auto r = spectrumRect();
    return r.x + r.w * std::log(std::clamp(hz, kMinHz, kMaxHz) / kMinHz) / std::log(kMaxHz / kMinHz);
}

float KickstractFace::designHzForX(float x) const
{
    const auto r = spectrumRect();
    return kMinHz * std::pow(kMaxHz / kMinHz, std::clamp((x - r.x) / std::max(1.0f, r.w), 0.0f, 1.0f));
}

float KickstractFace::windowMs() const
{
    if (kick_.bins > 0 && kick_.windowMs > 0) return kick_.windowMs;
    float longest = 0;
    for (int b = 0; b < K::kBands; ++b)
        longest = std::max(longest, value(K::bandParam(b, K::BandLength)) + value(K::bandParam(b, K::BandFade)));
    return std::clamp(longest * 1.15f + 15.0f, 60.0f, 1500.0f);
}

float KickstractFace::designXForMs(float ms) const
{
    const auto r = kickRect().reduced(6, 0);
    return r.x + r.w * std::clamp(ms / windowMs(), 0.0f, 1.0f);
}

float KickstractFace::msForDesignX(float x) const
{
    const auto r = kickRect().reduced(6, 0);
    return std::clamp((x - r.x) / std::max(1.0f, r.w), 0.0f, 1.0f) * windowMs();
}

float KickstractFace::handleY(int band) const { return kickRect().y + 12 + (float) band * 9; }

// The gate's level `ms` after it opened (capture time 0): the ramp up to the onset, Length from the onset, Fade.
float KickstractFace::gateLevelAt(int band, float ms) const
{
    if (value(K::Chop) < 0.5f) return 1.0f;
    const float holdEnd = kAttackMs + value(K::bandParam(band, K::BandLength));
    const float fade = std::max(0.01f, value(K::bandParam(band, K::BandFade)));
    if (ms < kAttackMs) return std::max(0.0f, ms / kAttackMs);
    if (ms <= holdEnd) return 1.0f;
    const float r = std::clamp(1.0f - (ms - holdEnd) / fade, 0.0f, 1.0f);
    return r * r;
}

int KickstractFace::designHandleAt(float x, float y) const
{
    const float grab = layoutMode() == Layout::Full ? 1.0f : 1.4f; // the compact grid is drawn smaller
    const auto s = spectrumRect();
    if (s.contains(x, y))
    {
        const float low = designXForHz(kickstract_.lowSplitHz()), high = designXForHz(kickstract_.highSplitHz());
        const float dl = std::abs(x - low), dh = std::abs(x - high);
        if (std::min(dl, dh) <= 6 * grab) return dl <= dh ? 0 : 1;
        return -1;
    }
    const auto k = kickRect();
    if (! k.contains(x, y) || value(K::Chop) < 0.5f) return -1;
    int best = -1;
    float bestDistance = 8 * grab;
    for (int b = 0; b < K::kBands; ++b)
    {
        const float holdEnd = kAttackMs + value(K::bandParam(b, K::BandLength));
        const float fadeEnd = holdEnd + value(K::bandParam(b, K::BandFade));
        const float hx = designXForMs(holdEnd), fx = designXForMs(fadeEnd), cy = k.centreY();
        if (const float d = std::hypot(x - hx, y - handleY(b)); d < bestDistance) { bestDistance = d; best = 10 + b; }
        if (const float d = std::hypot(x - fx, y - cy); d < bestDistance) { bestDistance = d; best = 20 + b; }
    }
    return best;
}

Rect KickstractFace::spectrumArea() const
{
    const auto r = spectrumRect();
    const float s = scale();
    return {r.x * s, r.y * s, r.w * s, r.h * s};
}

Rect KickstractFace::kickArea() const
{
    const auto r = kickRect();
    const float s = scale();
    return {r.x * s, r.y * s, r.w * s, r.h * s};
}

float KickstractFace::xForHz(float hz) const { return designXForHz(hz) * scale(); }
float KickstractFace::hzForX(float x) const { return designHzForX(x / scale()); }
float KickstractFace::xForMs(float ms) const { return designXForMs(ms) * scale(); }
int KickstractFace::handleAt(float x, float y) const { return designHandleAt(x / scale(), y / scale()); }

// ---------------------------------------------------------------------------------------------------------------------
// Controls

void KickstractFace::dragHandle(float x)
{
    if (drag_ == 0) setValue(K::LowSplit, designHzForX(x));
    else if (drag_ == 1) setValue(K::HighSplit, designHzForX(x));
    else if (drag_ >= 10 && drag_ < 20) setValue(K::bandParam(drag_ - 10, K::BandLength), std::max(1.0f, msForDesignX(x) - kAttackMs));
    else if (drag_ >= 20)
    {
        const int b = drag_ - 20;
        const float holdEnd = kAttackMs + value(K::bandParam(b, K::BandLength));
        setValue(K::bandParam(b, K::BandFade), std::max(1.0f, msForDesignX(x) - holdEnd));
    }
}

void KickstractFace::resetHandle(int handle)
{
    int parameter = -1;
    if (handle == 0) parameter = K::LowSplit;
    else if (handle == 1) parameter = K::HighSplit;
    else if (handle >= 10 && handle < 20) parameter = K::bandParam(handle - 10, K::BandLength);
    else if (handle >= 20) parameter = K::bandParam(handle - 20, K::BandFade);
    if (parameter >= 0) setValue(parameter, info(parameter).defaultValue);
}

std::string KickstractFace::headline() const
{
    return std::string("Low  <  ") + hzLong(kickstract_.lowSplitHz()) + "  <  Mid  <  " + hzLong(kickstract_.highSplitHz()) + "  <  High"
           + (value(K::Chop) > 0.5f ? "   \xC2\xB7   Chop on" : "   \xC2\xB7   filters only");
}

std::string KickstractFace::idleHelp() const
{
    return "Kickstract pulls the kick out of a full mix. Drag the split lines to move the bands, the dots on the last kick to "
           "set each band's Length and Fade.";
}

std::string KickstractFace::valueText(int parameter, float v) const
{
    if (parameter == K::LowSplit || parameter == K::HighSplit) return hzLong(v);
    return ProFace::valueText(parameter, v);
}

void KickstractFace::build(Layout layout)
{
    const auto addHandles = [this](const Rect& r, const char* tip) {
        auto& area = addArea(r, tip);
        area.down = [this](const MouseEvent& e) {
            drag_ = designHandleAt(e.x, e.y);
            if (e.clicks >= 2) { resetHandle(drag_); drag_ = -1; return; }
            if (drag_ >= 0) dragHandle(e.x);
        };
        area.drag = [this](const MouseEvent& e) { if (drag_ >= 0) dragHandle(e.x); };
        area.up = [this](const MouseEvent&) { drag_ = -1; };
    };
    const bool full = layout == Layout::Full;
    addHandles(spectrumRect(), "Spectrum: the input (grey) and the kick that comes out, in each band's colour. Drag a split line "
                               "to move the bands; double-click it for the default.");
    addHandles(kickRect(), "The last kick: the input, each band after its gate and the result. Drag the dots: top = Length, "
                           "middle = Fade of each band; double-click for the default.");

    static constexpr const char* kListenTip[3] = {"Listen: the extracted kick.", "Listen: what the gates removed (the mix without the kick).",
                                                  "Listen: the (delayed) input."};
    const auto bandControls = [&](int b, float x, float y, float knobW, float knobH, float toggleH, float soloW, bool small) {
        const auto colour = kBandColour[b];
        auto& on = addToggle({x, y, small ? 76.0f : 130.0f, toggleH}, K::bandParam(b, K::BandOn), kBandTitle[b],
                             std::string(kBandTitle[b]) + " band on / off.");
        on.colour = colour;
        auto& solo = addToggle({x + (small ? 80.0f : 190.0f), y, soloW, toggleH}, K::bandParam(b, K::BandSolo), small ? "S" : "Solo",
                               std::string("Solo the ") + kBandTitle[b] + " band.");
        solo.colour = colour;
        if (small) solo.textSize = 9.5f, on.textSize = 10.5f;
        const float ky = y + toggleH + (small ? 6.0f : 20.0f);
        const char* labels[3] = {"LENGTH", "FADE", "GAIN"};
        const int fields[3] = {K::BandLength, K::BandFade, K::BandGain};
        const char* tips[3] = {"Length: how long the band stays open after the hit.", "Fade: how long it takes to close after Length.",
                               "Gain: the band's level in the kick."};
        for (int k = 0; k < 3; ++k)
        {
            auto& knob = addKnob({x + (float) k * (knobW + (small ? 2.0f : 4.0f)), ky, knobW, knobH}, K::bandParam(b, fields[k]), labels[k],
                                 std::string(kBandTitle[b]) + " " + tips[k]);
            knob.colour = colour;
            knob.small = small;
            if (! small)
                knob.caption = [k] { return std::string(k == 0 ? "after the hit" : k == 1 ? "closing" : "level"); };
        }
    };

    if (full)
    {
        addSegments({700, 14, 300, 34}, K::Listen, {"Kick", "Removed", "Input"}, false, 4, "Listen: the kick, what was removed, or the input.");
        if (onOpenTimeline)
            addButton({1012, 14, 152, 34}, "Timeline", [this] { if (onOpenTimeline) onOpenTimeline(); },
                      "Timeline: input, kick and what was removed over time, live; Freeze to study it.")
                .textSize = 13.0f;
        addKnob({24, 92, 84, 104}, K::Threshold, "THRESHOLD", "Threshold: how loud the low end must get to count as a kick (the line on the meter).")
            .caption = [] { return std::string("detector"); };
        addKnob({108, 92, 84, 104}, K::Retrigger, "RETRIGGER", "Retrigger: the shortest time between two kicks.")
            .caption = [] { return std::string("min gap"); };
        addKnob({24, 212, 84, 104}, K::Input, "INPUT", "Input: the level going in.").caption = [] { return std::string("level in"); };
        addToggle({112, 236, 82, 28}, K::Chop, "Chop", "Chop: cut each band short after every kick (off: the bands play through, filters only).");
        for (int b = 0; b < K::kBands; ++b) bandControls(b, 28 + (float) b * 302, 360, 88, 104, 26, 74, false);
        const char* finishLabels[3] = {"PUNCH", "DRIVE", "OUTPUT"};
        const int finishParams[3] = {K::Punch, K::Drive, K::Output};
        const char* finishTips[3] = {"Punch: more attack on the kick's first milliseconds.", "Drive: saturation on the kick.",
                                     "Output: the level going out."};
        const char* finishCaptions[3] = {"attack", "saturation", "level out"};
        for (int k = 0; k < 3; ++k)
            addKnob({932 + (float) k * 78, 406, 74, 104}, finishParams[k], finishLabels[k], finishTips[k]).caption = [k, finishCaptions] {
                return std::string(finishCaptions[k]);
            };
        (void) kListenTip;
        return;
    }

    // Compact: the device chain.
    for (int b = 0; b < K::kBands; ++b) bandControls(b, 644 + (float) b * 124, 8, 38, 80, 22, 38, true);
    for (int i = 0; i < 3; ++i)
        addSegment({1016, 8 + (float) i * 28, 76, 24}, K::Listen, (float) i, i == 0 ? "Kick" : i == 1 ? "Rem" : "In", kListenTip[i]).textSize = 10.5f;
    addKnob({1014, 92, 40, 74}, K::Punch, "PUNCH", "Punch: more attack on the kick's first milliseconds.").small = true;
    addKnob({1056, 92, 40, 74}, K::Output, "OUT", "Output: the level going out.").small = true;
}

// ---------------------------------------------------------------------------------------------------------------------
// Animation

bool KickstractFace::animate()
{
    const double now = juce::Time::getMillisecondCounterHiRes() / 1000.0;
    const double seconds = lastTick_ > 0 ? now - lastTick_ : 1.0 / 30.0;
    lastTick_ = now;
    bool changed = inSpectrum_.update(kickstract_.inputTap, seconds);
    changed |= outSpectrum_.update(kickstract_.outputTap, seconds);
    changed |= kickstract_.takeKick(kick_, kick_.serial);
    const float peak = kickstract_.takeDetectorPeak();
    const float meter = std::max(peak, meter_ * (float) std::pow(0.05, seconds));
    if (std::abs(meter - meter_) > 1e-5f) { meter_ = meter < 1e-5f ? 0 : meter; changed = true; }
    if (const auto triggers = kickstract_.triggerCount(); triggers != triggers_)
    {
        triggers_ = triggers;
        flashUntil_ = now + 0.12;
        changed = true;
    }
    for (int b = 0; b < K::kBands; ++b)
    {
        const float level = std::max(kickstract_.bandLevel(b), bandLevels_[b] * (float) std::pow(0.02, seconds));
        if (std::abs(level - bandLevels_[b]) > 1e-3f) { bandLevels_[b] = level; changed = true; }
    }
    if (const int handle = designHandleAt(designMouseX(), designMouseY()); handle != lastHandle_) { lastHandle_ = handle; changed = true; }
    return changed || now < flashUntil_ + 0.05;
}

Cursor KickstractFace::cursorAt(float x, float y) const
{
    return handleAt(x, y) >= 0 || drag_ >= 0 ? Cursor::ResizeHorizontal : ProFace::cursorAt(x, y);
}

// ---------------------------------------------------------------------------------------------------------------------
// Painting

void KickstractFace::paintMeter(SkCanvas& c, const Rect& meter)
{
    // The detector (low band) with the threshold line; the lamp lights on every detected hit.
    pro::well(c, meter, 3);
    const auto yForDb = [&](float db) { return meter.y + meter.h * std::clamp(db / -60.0f, 0.0f, 1.0f); };
    const float level = 20.0f * std::log10(std::max(meter_, 1e-6f));
    const float top = yForDb(level);
    c.drawRect(SkRect::MakeLTRB(meter.x + 2, top, meter.right() - 2, meter.bottom() - 2), pro::fill(kBandColour[K::Low]));
    const float thresholdY = yForDb(value(K::Threshold));
    c.drawLine(meter.x - 4, thresholdY, meter.right() + 2, thresholdY, pro::stroke(kInk, 1.6f));
    const bool lit = juce::Time::getMillisecondCounterHiRes() / 1000.0 < flashUntil_;
    dot(c, meter.centreX(), meter.y - 12, 5, lit ? 0xff5cff7a : pro::kWell, pro::kLine);
}

void KickstractFace::paintSpectrum(SkCanvas& c, bool compact)
{
    const auto r = spectrumRect();
    pro::well(c, r, 6);
    c.save();
    c.clipRect(r.sk());
    const float size = compact ? 9.5f : 10.5f;
    const float lowX = designXForHz(kickstract_.lowSplitHz()), highX = designXForHz(kickstract_.highSplitHz());
    const float edges[K::kBands + 1] = {r.x, lowX, highX, r.right()};
    bool anySolo = false;
    for (int b = 0; b < K::kBands; ++b) anySolo |= value(K::bandParam(b, K::BandSolo)) > 0.5f;
    for (int b = 0; b < K::kBands; ++b) // band regions, brighter while their gate is open
    {
        const bool on = value(K::bandParam(b, K::BandOn)) > 0.5f && (! anySolo || value(K::bandParam(b, K::BandSolo)) > 0.5f);
        const auto alpha = (uint8_t) (on ? 14 + 50 * std::clamp(bandLevels_[b], 0.0f, 1.0f) : 6);
        c.drawRect(SkRect::MakeLTRB(edges[b], r.y, edges[b + 1], r.bottom()), pro::fill(paint::withAlpha(kBandColour[b], alpha)));
        pro::text(c, kBandTitle[b], {edges[b] + 5, r.y + 4, std::max(0.0f, edges[b + 1] - edges[b] - 8), 14},
                  paint::withAlpha(kBandColour[b], on ? 235 : 90), size, Align::Left, true);
    }
    for (const float hz : {50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f})
    {
        const float x = designXForHz(hz);
        c.drawLine(x, r.y, x, r.bottom() - 14, pro::stroke(kGrid, 1.0f));
        if (! compact || hz == 100.0f || hz == 1000.0f || hz == 10000.0f)
            pro::text(c, hzText(hz), {x - 16, r.bottom() - 14, 32, 12}, kScale, size - 0.5f, Align::Centre);
    }
    for (const float db : {-24.0f, -48.0f, -72.0f})
    {
        const float y = r.y + r.h * (kTopDb - db) / (kTopDb - kFloorDb);
        c.drawLine(r.x, y, r.right(), y, pro::stroke(paint::withAlpha(kGrid, 160), 1.0f));
    }
    const auto yForDb = [&](float db) { return r.y + r.h * std::clamp((kTopDb - db) / (kTopDb - kFloorDb), 0.0f, 1.0f); };
    const auto curvePath = [&](const auto& curve, bool closed) {
        SkPathBuilder path;
        bool started = false;
        for (int i = 0; i < dsp::SpectrumAnalyzer::kCurvePoints; ++i)
        {
            const float x = designXForHz(dsp::SpectrumAnalyzer::curveFrequency(i)), y = yForDb(curve[(size_t) i]);
            if (! started) { path.moveTo(x, closed ? r.bottom() : y); if (closed) path.lineTo(x, y); started = true; }
            else path.lineTo(x, y);
        }
        if (closed) { path.lineTo(r.right(), r.bottom()); path.close(); }
        return path;
    };
    auto input = curvePath(inSpectrum_.curve(), true);
    fillPath(c, input, 0x26c8d0da);
    auto inputLine = curvePath(inSpectrum_.curve(), false);
    strokePath(c, inputLine, 0x58c8d0da, 1.0f);
    for (int b = 0; b < K::kBands; ++b) // the result, in each band's colour
    {
        c.save();
        c.clipRect(SkRect::MakeLTRB(edges[b], r.y, edges[b + 1], r.bottom()));
        auto out = curvePath(outSpectrum_.curve(), true);
        fillPath(c, out, paint::withAlpha(kBandColour[b], 64));
        auto outLine = curvePath(outSpectrum_.curve(), false);
        strokePath(c, outLine, kBandColour[b], compact ? 1.5f : 1.8f);
        c.restore();
    }
    const int hover = designHandleAt(designMouseX(), designMouseY());
    for (int s = 0; s < 2; ++s) // split lines and their frequency tags
    {
        const float x = s == 0 ? lowX : highX;
        const bool hot = hover == s || drag_ == s;
        c.drawLine(x, r.y, x, r.bottom(), pro::stroke(paint::withAlpha(kInk, hot ? 255 : 170), hot ? 2.5f : 1.5f));
        const std::string label = hzLong(s == 0 ? kickstract_.lowSplitHz() : kickstract_.highSplitHz());
        const float w = compact ? 50.0f : 60.0f, h = compact ? 14.0f : 16.0f;
        const Rect tag{std::clamp(x - w * 0.5f, r.x + 2, r.right() - w - 2), r.y + (compact ? 18.0f : 22.0f), w, h};
        pro::rounded(c, tag, 4, hot ? kInk : 0xE021252D);
        pro::text(c, label, tag, hot ? pro::kWell : kInk, compact ? 9.5f : 10.5f, Align::Centre, true);
    }
    c.restore();
}

void KickstractFace::paintKick(SkCanvas& c, bool compact)
{
    const auto area = kickRect();
    pro::well(c, area, 6);
    c.save();
    c.clipRect(area.sk());
    const auto r = area.reduced(6, 0);
    const float size = compact ? 9.5f : 10.5f;
    const float mid = r.centreY(), half = r.h * 0.5f - 16;
    const float window = windowMs();
    const float step = window > 600 ? 100.0f : window > 250 ? 50.0f : 20.0f;
    for (float ms = step; ms < window; ms += step) // time grid
    {
        const float x = designXForMs(ms);
        c.drawLine(x, r.y, x, r.bottom() - 14, pro::stroke(kGrid, 1.0f));
        if (! compact || (int) ms % (int) (step * 2) == 0)
            pro::text(c, format("%.0f", ms), {x - 18, r.bottom() - 14, 36, 12}, kScale, size - 0.5f, Align::Centre);
    }
    c.drawLine(r.x, mid, r.right(), mid, pro::stroke(kGrid, 1.0f));
    const float hitX = designXForMs(kAttackMs);
    c.drawLine(hitX, r.y, hitX, r.bottom(), pro::stroke(paint::withAlpha(kInk, 50), 1.0f));

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
        mirrored(kick_.inPeak, 0x22c8d0da); // the input in that window (what the kick came out of)
        for (int b = 0; b < K::kBands; ++b) mirrored(kick_.band[(size_t) b], paint::withAlpha(kBandColour[b], 80));
        auto wave = pro::stroke(kInk, std::max(1.0f, binWidth * 0.8f)); // the result: one min/max line per bin
        for (int i = 0; i < kick_.bins; ++i)
        {
            const float x = r.x + ((float) i + 0.5f) * binWidth;
            c.drawLine(x, mid - kick_.outMax[(size_t) i] * scale, x,
                       mid - std::min(kick_.outMin[(size_t) i] * scale, kick_.outMax[(size_t) i] * scale - 0.6f), wave);
        }
    }
    else
        pro::text(c, "play something with a kick", {r.x, mid - 9, r.w, 18}, pro::kFaint, size + 1.5f, Align::Centre);

    // Each band's gate envelope (where the chop happens), with its Length and Fade handles.
    const bool chop = value(K::Chop) > 0.5f;
    const int hover = designHandleAt(designMouseX(), designMouseY());
    for (int b = 0; b < K::kBands; ++b)
    {
        const bool on = value(K::bandParam(b, K::BandOn)) > 0.5f;
        SkPathBuilder envelope;
        constexpr int kSteps = 160;
        for (int i = 0; i <= kSteps; ++i)
        {
            const float ms = window * (float) i / (float) kSteps;
            const float y = mid - gateLevelAt(b, ms) * (half + 4) + (float) b * 2;
            if (i == 0) envelope.moveTo(designXForMs(ms), y);
            else envelope.lineTo(designXForMs(ms), y);
        }
        strokePath(c, envelope, paint::withAlpha(kBandColour[b], on ? 230 : 70), compact ? 1.4f : 1.8f);
        if (! chop) continue;
        const float holdEnd = kAttackMs + value(K::bandParam(b, K::BandLength));
        const float fadeEnd = holdEnd + value(K::bandParam(b, K::BandFade));
        const float hx = designXForMs(holdEnd), fx = designXForMs(fadeEnd), hy = handleY(b);
        c.drawLine(hx, hy, hx, mid - (half + 4) + (float) b * 2, pro::stroke(paint::withAlpha(kBandColour[b], 90), 1.0f));
        const bool hotLength = hover == 10 + b || drag_ == 10 + b, hotFade = hover == 20 + b || drag_ == 20 + b;
        const float radius = compact ? 3.6f : 4.2f;
        dot(c, hx, hy, hotLength ? radius + 1.6f : radius, kBandColour[b], pro::kWell);
        dot(c, fx, mid, hotFade ? radius + 1.6f : radius, pro::kWell, kBandColour[b]);
    }
    c.restore();
}

void KickstractFace::paintBack(SkCanvas& c, Layout layout)
{
    if (layout == Layout::Compact)
    {
        paintSpectrum(c, true);
        paintKick(c, true);
        for (int b = 0; b < K::kBands; ++b) // a lit strip per band under its On switch: the gate level
        {
            const float x = 644 + (float) b * 124;
            pro::panel(c, {x - 4, 4, 120, 162}, 6);
            c.drawRect(SkRect::MakeXYWH(x, 33, 116, 2), pro::fill(paint::withAlpha(kBandColour[b], 50)));
            c.drawRect(SkRect::MakeXYWH(x, 33, 116 * std::clamp(bandLevels_[b], 0.0f, 1.0f), 2), pro::fill(kBandColour[b]));
        }
        return;
    }
    pro::wordmark(c, "kickstract", 24, 10, 28, kAccent, version);
    pro::text(c, headline(), {250, 12, 440, 22}, pro::kText, 14.5f, Align::Left, true);
    pro::text(c, "the kick, out of the full mix", {250, 34, 440, 18}, pro::kDim, 11.5f, Align::Left);

    pro::panel(c, {16, 64, 210, 276});
    pro::sectionTitle(c, "DETECT", 26, 70, kAccent);
    paintMeter(c, kMeterFull);
    pro::panel(c, {236, 64, 404, 276});
    pro::sectionTitle(c, "SPECTRUM", 248, 70, kAccent);
    pro::text(c, "drag the lines to move the bands", {400, 70, 228, 14}, pro::kFaint, 10, Align::Right);
    paintSpectrum(c, false);
    pro::panel(c, {650, 64, 514, 276});
    pro::sectionTitle(c, "LAST KICK", 662, 70, kAccent);
    pro::text(c, "drag the dots: Length (top) / Fade (middle)", {860, 70, 292, 14}, pro::kFaint, 10, Align::Right);
    paintKick(c, false);
    for (int b = 0; b < K::kBands; ++b)
    {
        const float x = 16 + (float) b * 302;
        pro::panel(c, {x, 350, 292, 190});
        c.drawRect(SkRect::MakeXYWH(x + 12, 392, 268, 3), pro::fill(paint::withAlpha(kBandColour[b], 45)));
        c.drawRect(SkRect::MakeXYWH(x + 12, 392, 268 * std::clamp(bandLevels_[b], 0.0f, 1.0f), 3), pro::fill(kBandColour[b]));
    }
    pro::panel(c, {922, 350, 242, 190});
    pro::sectionTitle(c, "FINISH", 934, 356, kAccent);
    c.drawLine(16, 548, 1164, 548, pro::stroke(pro::kLineSoft, 1.0f));
}

void KickstractFace::paintFront(SkCanvas& c, Layout layout)
{
    if (layout == Layout::Full) paintHelp(c, {20, 550, 1140, 18});
}
} // namespace sonora::ui
