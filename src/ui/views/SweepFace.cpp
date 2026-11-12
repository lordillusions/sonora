#include "ui/views/SweepFace.h"

#include "include/core/SkPathBuilder.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>

namespace sonora::ui
{
namespace
{
using W = devices::Sweep;
constexpr int kRenderBins = 420;

std::string format(const char* pattern, double value)
{
    char buffer[96];
    std::snprintf(buffer, sizeof buffer, pattern, value);
    return buffer;
}

// Source cells in the order the face shows them: the generators first, the sample last.
struct SourceCell { int value; const char* label; const char* shortLabel; const char* tip; };
constexpr SourceCell kSources[] = {
    {W::Noise, "White", "White", "White noise: the classic bright riser."},
    {W::Pink, "Pink", "Pink", "Pink noise: softer and fuller, less hiss."},
    {W::Tone, "Supersaw", "Saw", "Supersaw: five detuned saws per note (Chord stacks notes). The note you play sets the pitch."},
    {W::Sine, "Sine", "Sine", "Sine: pure tone for sub drops and booms (play a low note)."},
    {W::Whoosh, "Whoosh", "Whoosh", "Whoosh: air rushing past, a resonant band moving through noise."},
    {W::Metal, "Metal", "Metal", "Metal: a cymbal-like wash (inharmonic partials and bright noise): reverse crashes without a sample."},
    {W::Sample, "Sample", "Sample", "Sample: the loaded file, reversed with its reverb tail when going up. Drop one from the Browser."},
};

float hzToY(float hz, const Rect& r)
{
    const float t = std::log(std::clamp(hz, 20.0f, 20000.0f) / 20.0f) / std::log(1000.0f);
    return r.y + r.h - t * r.h;
}
} // namespace

SweepFace::SweepFace(devices::Sweep& device) : ProFace(device, kFull, kCompact, kAccent), sweep_(device) {}

bool SweepFace::browserDrop(const BrowserDrag& payload, const MouseEvent&)
{
    if (payload.kind != BrowserDrag::Kind::Sample || payload.value.empty() || ! onSampleDropped) return false;
    onSampleDropped(payload.value);
    repaint();
    return true;
}

std::string SweepFace::headline() const
{
    const bool up = std::lround(value(W::Direction)) == W::Up;
    const int length = std::clamp((int) std::lround(value(W::LengthParam)), 0, W::kLengths - 1);
    return std::string(up ? "Riser" : "Downlifter") + "  \xC2\xB7  " + W::kLengthNames[length] + "  \xC2\xB7  " +
           format("%.1f s", sweep_.lengthSeconds()) + format(" at %.0f BPM", sweep_.tempo());
}

std::string SweepFace::valueText(int param, float v) const
{
    switch (param)
    {
        case W::Curve:
            if (std::abs(v) < 0.05f) return "Linear";
            return format(v > 0 ? "Late %.0f" : "Early %.0f", std::abs(v) * 100.0);
        case W::Pitch: return std::abs(v) < 0.5f ? "0 st" : format(std::lround(value(W::Direction)) == W::Up ? "+%.0f st" : "-%.0f st", v);
        case W::Rhythm:
        {
            static const char* const kNames[4] = {"Off", "1/4\xE2\x86\x92" "1/16", "1/8\xE2\x86\x92" "1/32", "1/16\xE2\x86\x92" "1/64"};
            return kNames[std::clamp((int) std::lround(v), 0, 3)];
        }
        default: return ProFace::valueText(param, v);
    }
}

std::string SweepFace::idleHelp() const
{
    const int length = std::clamp((int) std::lround(value(W::LengthParam)), 0, W::kLengths - 1);
    const bool up = std::lround(value(W::Direction)) == W::Up;
    return up ? std::string("Play a note ") + W::kLengthNames[length] + " before the drop: the riser ends exactly on it. Or click Preview."
              : std::string("Play a note on the hit: the downlifter falls for ") + W::kLengthNames[length] + ". Or click Preview.";
}

uint64_t SweepFace::signature() const
{
    uint64_t h = 1469598103934665603ull;
    for (int i = 0; i < W::NumParams; ++i)
    {
        if (i == W::Output) continue;
        float v = value(i);
        uint32_t bits = 0;
        std::memcpy(&bits, &v, sizeof bits);
        h = (h ^ bits) * 1099511628211ull;
    }
    h = (h ^ (uint64_t) (sweep_.tempo() * 100.0)) * 1099511628211ull;
    h = (h ^ (uint64_t) (uintptr_t) sweep_.sampleAudio().get()) * 1099511628211ull;
    return h;
}

void SweepFace::renderNow()
{
    renderedSignature_ = pendingSignature_ = signature();
    std::vector<float> values;
    for (int i = 0; i < W::NumParams; ++i) values.push_back(value(i));
    values[(size_t) W::Output] = 0.0f;
    // Long sweeps render with the time compressed to 6 s: the shape over the bars is what matters here.
    const double seconds = sweep_.lengthSeconds();
    const double compress = std::max(1.0, seconds / 6.0);
    const int source = (int) std::lround(value(W::Source));
    const auto sample = sweep_.sampleAudio();
    const auto* audio = source == W::Sample && sample != nullptr ? sample.get() : nullptr;
    constexpr double kRate = 22050.0;
    const auto rendered = W::renderOffline(values, audio, kRate, sweep_.tempo() * compress, W::previewNote(source), sweep_.sampleRateOfSample());
    renderLo_.assign(kRenderBins, 0.0f);
    renderHi_.assign(kRenderBins, 0.0f);
    const int total = rendered.getNumSamples();
    if (total <= 0) return;
    float peak = 1.0e-6f;
    for (int b = 0; b < kRenderBins; ++b)
    {
        const int from = (int) ((int64_t) total * b / kRenderBins), to = std::max(from + 1, (int) ((int64_t) total * (b + 1) / kRenderBins));
        float lo = 0, hi = 0;
        for (int i = from; i < to && i < total; ++i)
        {
            const float v = 0.5f * (rendered.getSample(0, i) + rendered.getSample(1, i));
            lo = std::min(lo, v);
            hi = std::max(hi, v);
        }
        renderLo_[(size_t) b] = lo;
        renderHi_[(size_t) b] = hi;
        peak = std::max(peak, std::max(hi, -lo));
    }
    for (int b = 0; b < kRenderBins; ++b) // normalised: the display shows the shape, not the level
    {
        renderLo_[(size_t) b] /= peak;
        renderHi_[(size_t) b] /= peak;
    }
}

const std::vector<float>& SweepFace::renderedMax()
{
    if (signature() != renderedSignature_) renderNow();
    return renderHi_;
}

bool SweepFace::animate()
{
    bool dirty = false;
    const float p = sweep_.progress();
    if (std::abs(p - progress_) > 0.0f) { progress_ = p; dirty = true; }
    // Render again once the settings stay still for a moment (dragging a knob does not render every frame).
    const auto sig = signature();
    if (sig != renderedSignature_)
    {
        if (sig != pendingSignature_) { pendingSignature_ = sig; pendingTicks_ = 0; }
        else if (++pendingTicks_ >= 4 || renderLo_.empty()) { renderNow(); dirty = true; }
    }
    return dirty;
}

void SweepFace::addSourceSegments(const Rect& row1, const Rect& row2, bool compact)
{
    // Full: one row of seven. Compact: four on the first row, three on the second.
    const int count = (int) (sizeof kSources / sizeof kSources[0]);
    for (int i = 0; i < count; ++i)
    {
        Rect r;
        if (! compact)
        {
            const float w = (row1.w - 4.0f * (float) (count - 1)) / (float) count;
            r = {row1.x + (float) i * (w + 4.0f), row1.y, w, row1.h};
        }
        else if (i < 4)
        {
            const float w = (row1.w - 12.0f) / 4.0f;
            r = {row1.x + (float) i * (w + 4.0f), row1.y, w, row1.h};
        }
        else
        {
            const float w = (row2.w - 8.0f) / 3.0f;
            r = {row2.x + (float) (i - 4) * (w + 4.0f), row2.y, w, row2.h};
        }
        auto& s = addSegment(r, W::Source, (float) kSources[i].value, compact ? kSources[i].shortLabel : kSources[i].label, kSources[i].tip);
        if (compact) s.textSize = 10.0f;
    }
}

void SweepFace::build(Layout layout)
{
    const auto addDirection = [&](const Rect& r, float text) {
        const float w = (r.w - 4.0f) * 0.5f;
        addSegment({r.x, r.y, w, r.h}, W::Direction, W::Up, "\xE2\x96\xB2  Up", "Up: a riser that ends on the drop.").textSize = text;
        addSegment({r.x + w + 4.0f, r.y, w, r.h}, W::Direction, W::Down, "\xE2\x96\xBC  Down", "Down: a downlifter that starts on the hit.").textSize = text;
    };
    auto& play = [&]() -> Control& {
        const Rect r = layout == Layout::Full ? Rect{944, 12, 140, 36} : Rect{316, 118, 212, 44};
        return addButton(r, "", [this] { sweep_.preview(); }, "Preview: plays one sweep now (click again to stop).");
    }();
    play.lit = [this] { return sweep_.progress() >= 0.0f; };
    play.textSize = layout == Layout::Full ? 14.0f : 13.0f;

    if (layout == Layout::Full)
    {
        display_ = {234, 64, 850, 266};
        addArea(display_, "The sweep over its bars: the sound itself, the filter (blue), the pitch (violet) and the level (white).");
        const auto& list = W::types();
        for (size_t t = 0; t < list.size(); ++t)
            addButton({24, 92 + (float) t * 40, 190, 34}, list[t].name, [this, t] { sweep_.applyType((int) t); repaint(); },
                      std::string(list[t].name) + ": " + list[t].description)
                .lit = [this, t] { return sweep_.matchingType() == (int) t; };
        if (onChooseSample)
            addButton({24, 632, 92, 26}, "Choose\xE2\x80\xA6", [this] { if (onChooseSample) onChooseSample(); },
                      "Choose a sample file (or drop one from the Browser anywhere on Sweep).");

        addSourceSegments({304, 340, 556, 34}, {}, false);
        addDirection({872, 340, 212, 34}, 13);
        std::vector<std::string> lengths(W::kLengthNames, W::kLengthNames + W::kLengths);
        addSegments({304, 384, 780, 28}, W::LengthParam, lengths, false, 4,
                    "Length: how long the sweep lasts, in the song's tempo. A riser ends exactly this long after its note.");

        const auto knob = [&](float panelX, int column, int row, int param, const char* label, const char* tip) -> Control& {
            return addKnob({panelX + 12 + (float) column * 96, row == 0 ? 448.0f : 556.0f, 92, 104}, param, label, tip);
        };
        const float filterX = 234, pitchX = 449, shapeX = 664, outX = 879;
        auto& low = knob(filterX, 0, 0, W::LowHz, "LOW", "Low: the filter's lowest point (where a riser starts, where a downlifter ends).");
        low.caption = [this] { return std::string(std::lround(value(W::Direction)) == W::Up ? "start" : "end"); };
        auto& high = knob(filterX, 1, 0, W::HighHz, "HIGH", "High: the filter's highest point (where a riser ends, where a downlifter starts).");
        high.caption = [this] { return std::string(std::lround(value(W::Direction)) == W::Up ? "end" : "start"); };
        knob(filterX, 0, 1, W::Resonance, "RESO", "Resonance: a whistling peak riding on the filter.").caption = [] { return std::string("peak"); };
        knob(filterX, 1, 1, W::Air, "AIR", "Air: a bright noise layer under any source; it follows the filter.").caption = [] { return std::string("noise layer"); };

        knob(pitchX, 0, 0, W::Pitch, "PITCH", "Pitch: how far the pitch travels (up for a riser, down for a downlifter).").caption = [this] {
            return std::string(std::lround(value(W::Direction)) == W::Up ? "rises" : "falls");
        };
        knob(pitchX, 1, 0, W::Detune, "DETUNE", "Detune: spreads the supersaw voices apart.").caption = [] { return std::string("supersaw"); };
        addSegments({pitchX + 12, 590, 181, 26}, W::Chord, {"1", "Oct", "5th", "Maj", "Min"}, false, 3,
                    "Chord: stacks notes on Supersaw and Sine (one note, octave, fifth, major, minor).");

        knob(shapeX, 0, 0, W::Curve, "CURVE", "Curve: early (moves at once, then settles) to late (holds back, then rushes in at the end).").bipolar = true;
        knob(shapeX, 1, 0, W::Swell, "SWELL", "Swell: how much the level rises (or falls) over the sweep.").caption = [] { return std::string("volume"); };
        knob(shapeX, 0, 1, W::Width, "WIDTH", "Width: how wide the stereo gets at its widest.").caption = [] { return std::string("stereo"); };
        knob(shapeX, 1, 1, W::Rhythm, "RHYTHM", "Rhythm: a gate that speeds up over the sweep (Off, or from 1/4 to 1/16 and faster).")
            .caption = [] { return std::string("gate"); };

        knob(outX, 0, 0, W::Output, "LEVEL", "Level: the sweep's volume.").caption = [] { return std::string("output"); };
        knob(outX, 1, 0, W::Tail, "TAIL", "Tail: the reverb added to a sample before it is reversed.").caption = [] { return std::string("sample reverb"); };
        addToggle({outX + 16, 572, 170, 28}, W::Reverse, "Reverse sample", "Reverse: an Up sweep plays the sample backwards into its attack.");
        return;
    }

    // Compact: the device chain.
    display_ = {8, 8, 300, 154};
    addArea(display_, "The sweep over its bars. Open the window for the types and every control.");
    addSourceSegments({316, 8, 212, 30}, {316, 44, 212, 30}, true);
    addDirection({316, 80, 212, 30}, 11);
    addKnob({536, 4, 86, 80}, W::LengthParam, "LENGTH", "Length of the sweep in the song's tempo.");
    addKnob({536, 88, 86, 80}, W::Swell, "SWELL", "Swell: how much the level rises over the sweep.");
    addKnob({626, 4, 88, 80}, W::HighHz, "HIGH", "High: the filter's highest point.");
    addKnob({626, 88, 88, 80}, W::Output, "LEVEL", "Level: the sweep's volume.");
}

void SweepFace::paintDisplay(SkCanvas& c, const Rect& outer, bool compact)
{
    pro::well(c, outer, 8);
    const Rect r{outer.x + 10, outer.y + (compact ? 18.0f : 30.0f), outer.w - 20, outer.h - (compact ? 34.0f : 52.0f)};
    const bool up = std::lround(value(W::Direction)) == W::Up;
    const int length = std::clamp((int) std::lround(value(W::LengthParam)), 0, W::kLengths - 1);
    const double beats = W::kLengthBeats[length];
    // Bars and beats.
    const int beatLines = (int) beats;
    for (int b = 0; b <= beatLines; ++b)
    {
        const bool bar = b % 4 == 0;
        if (! bar && beatLines > 32) continue;
        const float x = r.x + r.w * (float) b / (float) beats;
        c.drawLine(x, r.y, x, r.y + r.h, pro::stroke(bar ? 0xFF262B34 : 0xFF181C22, 1.0f));
        if (bar && ! compact && (beatLines <= 32 || (b / 4) % 2 == 0) && b < beatLines)
            pro::text(c, std::to_string(b / 4 + 1), {x + 3, r.y + r.h + 4, 30, 14}, pro::kFaint, 10, Align::Left, true);
    }
    // The sound itself.
    if (renderLo_.empty()) renderNow();
    if (! renderLo_.empty())
        pro::waveform(c, renderLo_.data(), renderHi_.data(), (int) renderLo_.size(), {r.x, r.y + r.h * 0.15f, r.w, r.h * 0.7f},
                      paint::withAlpha(kAccent, 120));
    // The curves.
    const auto curve = [&](auto fn, SkColor colour, float widthPx) {
        SkPathBuilder p;
        for (int i = 0; i <= 120; ++i)
        {
            const float t = (float) i / 120.0f;
            const float x = r.x + r.w * t, y = fn(t);
            if (i == 0) p.moveTo(x, y);
            else p.lineTo(x, y);
        }
        c.drawPath(p.detach(), pro::stroke(colour, widthPx));
    };
    const float pitchRange = std::max(12.0f, value(W::Pitch));
    curve([&](float t) { return hzToY(sweep_.cutoffAt(t), r); }, kCutoff, compact ? 1.6f : 2.2f);
    curve([&](float t) { return r.y + r.h * 0.5f - sweep_.pitchAt(t) / pitchRange * r.h * 0.45f; }, kPitch, compact ? 1.4f : 2.0f);
    curve([&](float t) { return r.y + r.h - sweep_.levelAt(t) * r.h; }, paint::withAlpha(kLevel, 200), compact ? 1.2f : 1.6f);
    // The drop.
    const float dropX = up ? r.x + r.w : r.x;
    c.drawLine(dropX, outer.y + 6, dropX, r.y + r.h, pro::stroke(kAccent, 2.0f));
    if (! compact)
        pro::text(c, up ? "DROP \xE2\x96\xBC" : "\xE2\x96\xBC HIT", {up ? dropX - 70 : dropX + 4, outer.y + 8, 66, 16}, kAccent, 11,
                  up ? Align::Right : Align::Left, true);
    // The play head.
    if (progress_ >= 0)
    {
        const float x = r.x + r.w * progress_;
        c.drawLine(x, r.y, x, r.y + r.h, pro::stroke(0xFFFFFFFF, 1.5f));
    }
    // Legend.
    const float ly = outer.y + (compact ? 4.0f : 8.0f), size = compact ? 8.5f : 10.5f;
    float lx = outer.x + 12;
    for (const auto& [name, colour] : {std::pair<const char*, SkColor>{"filter", kCutoff}, {"pitch", kPitch}, {"level", kLevel}})
    {
        c.drawCircle(lx + 3, ly + size * 0.65f, 3, pro::fill(colour));
        pro::text(c, name, {lx + 10, ly, 60, size + 4}, pro::kDim, size, Align::Left, true);
        lx += compact ? 52 : 66;
    }
    if (std::lround(value(W::Source)) == W::Sample && ! sweep_.hasSample())
        pro::text(c, "drop a sample here (from the Browser or the Finder)", {outer.x, outer.y + outer.h * 0.5f - 10, outer.w, 20},
                  pro::kText, compact ? 10.5f : 14.0f, Align::Centre, true);
}

void SweepFace::paintBack(SkCanvas& c, Layout layout)
{
    if (layout == Layout::Compact)
    {
        paintDisplay(c, display_, true);
        return;
    }
    pro::wordmark(c, "sweep", 24, 10, 28, kAccent, version);
    pro::text(c, headline(), {200, 12, 720, 22}, pro::kText, 16, Align::Left, true);
    pro::text(c, idleHelp(), {200, 34, 720, 18}, pro::kDim, 11.5f, Align::Left);

    pro::panel(c, {16, 64, 206, 496});
    pro::sectionTitle(c, "TYPE", 26, 70, kAccent);
    pro::panel(c, {16, 570, 206, 98});
    pro::sectionTitle(c, "SAMPLE", 26, 576, kAccent);
    const std::string name = sweep_.sampleName().empty() ? std::string("none loaded") : sweep_.sampleName();
    pro::text(c, name, {26, 598, 188, 18}, sweep_.hasSample() ? pro::kText : pro::kFaint, 12, Align::Left, true);
    pro::text(c, onChooseSample ? "or drop one" : "drop one on Sweep", {onChooseSample ? 122.0f : 26.0f, 634, 100, 22}, pro::kFaint, 10.5f,
              Align::Left);

    paintDisplay(c, display_, false);
    pro::text(c, "SOURCE", {234, 340, 64, 34}, pro::kFaint, 10.5f, Align::Left, true);
    pro::text(c, "LENGTH", {234, 384, 64, 28}, pro::kFaint, 10.5f, Align::Left, true);
    const std::pair<const char*, float> panels[4] = {{"FILTER", 234}, {"PITCH", 449}, {"SHAPE", 664}, {"OUT", 879}};
    for (const auto& [title, x] : panels)
    {
        pro::panel(c, {x, 424, 205, 244});
        pro::sectionTitle(c, title, x + 12, 430, kAccent);
    }
    pro::text(c, "CHORD", {461, 572, 100, 16}, pro::kFaint, 9.5f, Align::Left, true);
    c.drawLine(16, 676, 1084, 676, pro::stroke(pro::kLineSoft, 1.0f));
}

void SweepFace::paintFront(SkCanvas& c, Layout layout)
{
    // The Preview button's label and icon.
    const bool playing = sweep_.progress() >= 0.0f;
    const Rect r = layout == Layout::Full ? Rect{944, 12, 140, 36} : Rect{316, 118, 212, 44};
    const SkColor ink = playing ? paint::contrastText(kAccent) : kAccent;
    const float cx = r.x + 22, cy = r.y + r.h * 0.5f, s = 6;
    if (playing) c.drawRect(SkRect::MakeXYWH(cx - s, cy - s, 2 * s, 2 * s), pro::fill(ink));
    else
    {
        SkPathBuilder tri;
        tri.moveTo(cx - s * 0.8f, cy - s);
        tri.lineTo(cx + s, cy);
        tri.lineTo(cx - s * 0.8f, cy + s);
        tri.close();
        c.drawPath(tri.detach(), pro::fill(ink));
    }
    pro::text(c, playing ? "Stop" : "Preview", {r.x + 36, r.y, r.w - 44, r.h}, ink, layout == Layout::Full ? 14.0f : 13.0f, Align::Left, true);
    if (layout == Layout::Full) paintHelp(c, {20, 680, 1060, 18});
}
} // namespace sonora::ui
