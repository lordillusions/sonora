#include "ui/views/SweepFace.h"

#include "devices/native/SweepSwell.h"
#include "include/core/SkPathBuilder.h"
#include "include/core/SkRRect.h"

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

// A reverb swell type (its first value is the Source, Sample): listed first, under REVERB SWELLS.
bool isSampleType(const W::Type& t) { return std::lround(t.values.front().second) == W::Sample; }

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
    // In the app the drop is a device edit, which rebuilds the device chain before it returns: this face (and the
    // callback it owns) may be gone afterwards. Keep the callback alive in a local and never touch `this` after it; the
    // root repaints after every drop (the owner's crash in 0.1.51).
    const auto dropped = onSampleDropped;
    const std::string path = payload.value;
    dropped(path);
    return true;
}

std::string SweepFace::headline() const
{
    const bool up = std::lround(value(W::Direction)) == W::Up;
    const int length = std::clamp((int) std::lround(value(W::LengthParam)), 0, W::kLengths - 1);
    const char* what = sweep_.reverbSwell() ? (up ? "Reverb swell" : "Reverb throw") : (up ? "Riser" : "Downlifter");
    return std::string(what) + "  \xC2\xB7  " + W::kLengthNames[length] + "  \xC2\xB7  " +
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
        case W::Tail: // a reverb swell's decay (Fit overrides it); the reversed sample's reverb amount
            if (sweep_.reverbSwell() || std::lround(value(W::Source)) != W::Sample)
                return value(W::Fit) > 0.5f ? std::string("Fit") : format("%.1f s", W::tailSeconds(v));
            return ProFace::valueText(param, v);
        case W::Hit: return v < 0.005f ? std::string("Off") : ProFace::valueText(param, v);
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
        else if (++pendingTicks_ >= 4 || renderLo_.empty())
        {
            sweep_.refreshSample(); // PLG-19d: a swell setting, the Region, Length or the tempo changed: prepare it again
            renderNow();
            dirty = true;
        }
    }
    if (const void* audio = sweep_.sampleAudio().get(); audio != overviewOf_)
    {
        overviewOf_ = audio;
        overview_ = sweep_.sourceOverview(120);
        dirty = true;
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

void SweepFace::setCharacterFrom(float x, float y)
{
    setValue(W::CharacterX, std::clamp((x - kPad.x) / kPad.w, 0.0f, 1.0f));
    setValue(W::CharacterY, std::clamp(1.0f - (y - kPad.y) / kPad.h, 0.0f, 1.0f));
}

void SweepFace::setRegionFrom(float x, bool start)
{
    const float t = std::clamp((x - kRegion.x) / kRegion.w, 0.0f, 1.0f);
    if (start) setValue(W::RegionStart, std::min(t, value(W::RegionEnd) - 0.01f));
    else setValue(W::RegionEnd, std::max(t, value(W::RegionStart) + 0.01f));
}

void SweepFace::build(Layout layout)
{
    const auto addDirection = [&](const Rect& r, float text) {
        const float w = (r.w - 4.0f) * 0.5f;
        addSegment({r.x, r.y, w, r.h}, W::Direction, W::Up, "\xE2\x96\xB2  Up", "Up: a riser that ends on the drop.").textSize = text;
        addSegment({r.x + w + 4.0f, r.y, w, r.h}, W::Direction, W::Down, "\xE2\x96\xBC  Down", "Down: a downlifter that starts on the hit.").textSize = text;
    };
    auto& play = [&]() -> Control& {
        const Rect r = layout == Layout::Full ? Rect{1004, 12, 160, 38} : Rect{316, 118, 212, 44};
        return addButton(r, "", [this] { sweep_.preview(); }, "Preview: plays one sweep now (click again to stop).");
    }();
    play.lit = [this] { return sweep_.progress() >= 0.0f; };
    play.textSize = layout == Layout::Full ? 14.0f : 13.0f;

    if (layout == Layout::Full)
    {
        if (onSaveWav)
            addButton({858, 12, 136, 38}, "Save WAV\xE2\x80\xA6", [this] { if (onSaveWav) onSaveWav(); },
                      "Save WAV: renders this sweep (and its Hit) at the song's tempo into a file.")
                .textSize = 13.0f;
        display_ = {234, 64, 930, 226};
        auto& shown = addArea(display_, onDragOut ? "The sweep over its bars: the sound, the filter (blue), the pitch (violet) and the level (white). "
                                                     "Drag it out to put it on a track."
                                                  : "The sweep over its bars: the sound, the filter (blue), the pitch (violet) and the level (white).");
        shown.down = [this](const MouseEvent& e) { downX_ = e.x; downY_ = e.y; draggingOut_ = false; };
        shown.drag = [this](const MouseEvent& e) {
            if (draggingOut_ || ! onDragOut || std::hypot(e.x - downX_, e.y - downY_) < 6.0f) return;
            draggingOut_ = true;
            const auto out = onDragOut; // it may start a drag that outlives this gesture
            out();
        };

        // TYPE: the reverb swells first (they need a sample), then the synth sweeps.
        const auto& list = W::types();
        float y = 110;
        const auto addType = [&](size_t t) {
            auto& b = addButton({24, y, 190, 25}, list[t].name, [this, t] { sweep_.applyType((int) t); repaint(); },
                                std::string(list[t].name) + ": " + list[t].description);
            b.lit = [this, t] { return sweep_.matchingType() == (int) t; };
            b.textSize = 12.0f;
            y += 28;
        };
        for (size_t t = 0; t < list.size(); ++t)
            if (isSampleType(list[t])) addType(t);
        y += 24;
        for (size_t t = 0; t < list.size(); ++t)
            if (! isSampleType(list[t])) addType(t);

        // SAMPLE: the Region strip and the chooser.
        auto& region = addArea(kRegion, "Region: the part of the sample used (drag the handles). A reverb swell uses up to 12 s.");
        region.down = [this](const MouseEvent& e) {
            const float a = kRegion.x + kRegion.w * value(W::RegionStart), b = kRegion.x + kRegion.w * value(W::RegionEnd);
            regionStart_ = std::abs(e.x - a) <= std::abs(e.x - b);
            setRegionFrom(e.x, regionStart_);
        };
        region.drag = [this](const MouseEvent& e) { setRegionFrom(e.x, regionStart_); };
        if (onChooseSample)
            addButton({24, 700, 92, 24}, "Choose\xE2\x80\xA6", [this] { if (onChooseSample) onChooseSample(); },
                      "Choose a sample file (or drop one from the Browser anywhere on Sweep).")
                .textSize = 11.5f;

        addSourceSegments({304, 300, 556, 32}, {}, false);
        addDirection({872, 300, 292, 32}, 13);
        std::vector<std::string> lengths(W::kLengthNames, W::kLengthNames + W::kLengths);
        addSegments({304, 340, 860, 28}, W::LengthParam, lengths, false, 4,
                    "Length: how long the sweep lasts, in the song's tempo. A riser ends exactly this long after its note.");

        const auto knob = [&](float x, float y0, int param, const char* label, const char* tip) -> Control& {
            return addKnob({x, y0, 92, 104}, param, label, tip);
        };
        // SWELL: the character pad, Decay, Damp, Hit; the preparation; Fit and Reverse.
        auto& pad = addArea(kPad, "Character: Hall (bottom left), Shimmer (top left, an octave up), Bloom (top right, a frozen "
                                  "spectral smear) and Grain (bottom right, a cloud of tiny pieces). Drag to blend them.");
        pad.down = [this](const MouseEvent& e) { setCharacterFrom(e.x, e.y); };
        pad.drag = [this](const MouseEvent& e) { setCharacterFrom(e.x, e.y); };
        knob(428, 398, W::Tail, "DECAY", "Decay: how long the reverb rings (0.4 to 12 s). With Fit on, the swell fills the Length instead.")
            .caption = [this] { return std::string(value(W::Fit) > 0.5f ? "fills the length" : "reverb time"); };
        knob(522, 398, W::Damp, "DAMP", "Damp: darkens the reverb (high frequencies die sooner).").caption = [] { return std::string("darker"); };
        knob(616, 398, W::Hit, "HIT", "Hit: plays the sample itself at the drop (Up) or with the note (Down), after its swell.")
            .caption = [this] { return std::string(std::lround(value(W::Direction)) == W::Up ? "at the drop" : "with the note"); };
        addSegment({428, 508, 138, 26}, W::SwellMode, W::ReverbSwell, "Reverb Swell",
                   "Reverb Swell: only the sample's reverb, reversed, rising into it (the sample itself is the Hit).").textSize = 11.5f;
        addSegment({570, 508, 138, 26}, W::SwellMode, W::ReversedSample, "Reversed Sample",
                   "Reversed Sample: the whole sample played backwards with a reverb under it, landing on its attack.").textSize = 11.5f;
        addToggle({428, 542, 138, 26}, W::Fit, "Fit to length", "Fit: the reverb's decay follows the sweep's Length, so the swell starts with it.");
        addToggle({570, 542, 138, 26}, W::Reverse, "Reverse", "Reverse: an Up sweep plays the tail (or the sample) backwards into the drop.");

        // FILTER
        auto& low = knob(746, 398, W::LowHz, "LOW", "Low: the filter's lowest point (where a riser starts, where a downlifter ends).");
        low.caption = [this] { return std::string(std::lround(value(W::Direction)) == W::Up ? "start" : "end"); };
        auto& high = knob(850, 398, W::HighHz, "HIGH", "High: the filter's highest point (where a riser ends, where a downlifter starts).");
        high.caption = [this] { return std::string(std::lround(value(W::Direction)) == W::Up ? "end" : "start"); };
        knob(954, 398, W::Resonance, "RESO", "Resonance: a whistling peak riding on the filter.").caption = [] { return std::string("peak"); };
        knob(1058, 398, W::Air, "AIR", "Air: a bright noise layer under any source; it follows the filter.").caption = [] { return std::string("noise layer"); };

        // PITCH
        knob(246, 610, W::Pitch, "PITCH", "Pitch: how far the pitch travels (up for a riser, down for a downlifter).").caption = [this] {
            return std::string(std::lround(value(W::Direction)) == W::Up ? "rises" : "falls");
        };
        knob(342, 610, W::Detune, "DETUNE", "Detune: spreads the supersaw voices apart.").caption = [] { return std::string("supersaw"); };
        addSegments({440, 610, 82, 112}, W::Chord, {"1", "Oct", "5th", "Maj", "Min"}, true, 3,
                    "Chord: stacks notes on Supersaw and Sine (one note, octave, fifth, major, minor).");

        // SHAPE
        knob(556, 610, W::Curve, "CURVE", "Curve: early (moves at once, then settles) to late (holds back, then rushes in at the end).").bipolar = true;
        knob(656, 610, W::Swell, "SWELL", "Swell: how much the level rises (or falls) over the sweep.").caption = [] { return std::string("volume"); };
        knob(756, 610, W::Width, "WIDTH", "Width: how wide the stereo gets at its widest.").caption = [] { return std::string("stereo"); };
        knob(856, 610, W::Rhythm, "RHYTHM", "Rhythm: a gate that speeds up over the sweep (Off, or from 1/4 to 1/16 and faster).")
            .caption = [] { return std::string("gate"); };

        // OUT
        knob(1022, 610, W::Output, "LEVEL", "Level: the sweep's volume.").caption = [] { return std::string("output"); };
        return;
    }

    // Compact: the device chain.
    display_ = {8, 8, 300, 154};
    addArea(display_, "The sweep over its bars. Open the window for the types, the swell character and every control.");
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
    {
        const bool hit = std::lround(value(W::Source)) == W::Sample && value(W::Hit) > 0.005f;
        const std::string label = up ? (hit ? "DROP + HIT \xE2\x96\xBC" : "DROP \xE2\x96\xBC") : (hit ? "\xE2\x96\xBC HIT + TAIL" : "\xE2\x96\xBC HIT");
        pro::text(c, label, {up ? dropX - 124 : dropX + 4, outer.y + 8, 120, 16}, kAccent, 11, up ? Align::Right : Align::Left, true);
    }
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

void SweepFace::paintCharacterPad(SkCanvas& c)
{
    pro::well(c, kPad, 8);
    // A grid and the four corners with how much of each is in the blend now.
    for (int i = 1; i < 4; ++i)
    {
        const float fx = kPad.x + kPad.w * (float) i / 4.0f, fy = kPad.y + kPad.h * (float) i / 4.0f;
        c.drawLine(fx, kPad.y + 4, fx, kPad.bottom() - 4, pro::stroke(pro::kLineSoft, 1.0f));
        c.drawLine(kPad.x + 4, fy, kPad.right() - 4, fy, pro::stroke(pro::kLineSoft, 1.0f));
    }
    const auto w = devices::swell::weights(value(W::CharacterX), value(W::CharacterY));
    struct Corner { int which; float x, y; Align align; };
    const Corner corners[4] = {{devices::swell::Shimmer, kPad.x + 8, kPad.y + 6, Align::Left},
                               {devices::swell::Bloom, kPad.right() - 78, kPad.y + 6, Align::Right},
                               {devices::swell::Hall, kPad.x + 8, kPad.bottom() - 34, Align::Left},
                               {devices::swell::Grain, kPad.right() - 78, kPad.bottom() - 34, Align::Right}};
    for (const auto& corner : corners)
    {
        const float share = w[(size_t) corner.which];
        pro::text(c, devices::swell::kCharacterNames[corner.which], {corner.x, corner.y, 70, 14},
                  share > 0.25f ? pro::kText : pro::kDim, 11, corner.align, true);
        pro::text(c, format("%.0f%%", share * 100.0), {corner.x, corner.y + 14, 70, 13}, pro::kFaint, 10, corner.align);
    }
    const float px = kPad.x + kPad.w * value(W::CharacterX), py = kPad.y + kPad.h * (1.0f - value(W::CharacterY));
    c.drawLine(px, kPad.y + 2, px, kPad.bottom() - 2, pro::stroke(paint::withAlpha(kAccent, 70), 1.0f));
    c.drawLine(kPad.x + 2, py, kPad.right() - 2, py, pro::stroke(paint::withAlpha(kAccent, 70), 1.0f));
    c.drawCircle(px, py, 13, pro::fill(paint::withAlpha(kAccent, 50)));
    c.drawCircle(px, py, 7, pro::fill(kAccent));
    c.drawCircle(px, py, 7, pro::stroke(0xFF000000, 1.5f));
}

void SweepFace::paintRegion(SkCanvas& c)
{
    pro::well(c, kRegion, 5);
    const float a = kRegion.x + kRegion.w * value(W::RegionStart), b = kRegion.x + kRegion.w * value(W::RegionEnd);
    if (! overview_.empty())
    {
        std::vector<float> lo, hi;
        for (const auto& [l, h] : overview_) { lo.push_back(l); hi.push_back(h); }
        float peak = 1.0e-6f;
        for (size_t i = 0; i < lo.size(); ++i) peak = std::max(peak, std::max(hi[i], -lo[i]));
        pro::waveform(c, lo.data(), hi.data(), (int) lo.size(), kRegion.reduced(2, 4), paint::withAlpha(kAccent, 200), 1.0f / peak);
        // Outside the Region: dimmed.
        c.drawRect(SkRect::MakeLTRB(kRegion.x, kRegion.y, a, kRegion.bottom()), pro::fill(paint::withAlpha(pro::kWell, 200)));
        c.drawRect(SkRect::MakeLTRB(b, kRegion.y, kRegion.right(), kRegion.bottom()), pro::fill(paint::withAlpha(pro::kWell, 200)));
    }
    else
        pro::text(c, "drop a sample on Sweep", kRegion, pro::kFaint, 10.5f, Align::Centre);
    for (const float x : {a, b})
    {
        c.drawLine(x, kRegion.y - 2, x, kRegion.bottom() + 2, pro::stroke(kAccent, 2.0f));
        c.drawRect(SkRect::MakeXYWH(x - 4, kRegion.y - 4, 8, 6), pro::fill(kAccent));
    }
    if (sweep_.hasSample())
    {
        const double total = (double) sweep_.sampleAudio()->getNumSamples() / std::max(1.0, sweep_.sampleRateOfSample());
        char buffer[96];
        std::snprintf(buffer, sizeof buffer, "%.2f \xE2\x80\x93 %.2f s of %.2f s", value(W::RegionStart) * total, value(W::RegionEnd) * total, total);
        pro::text(c, buffer, {kRegion.x, kRegion.bottom() + 6, kRegion.w, 14}, pro::kDim, 10.5f, Align::Left);
    }
}

void SweepFace::paintBack(SkCanvas& c, Layout layout)
{
    if (layout == Layout::Compact)
    {
        paintDisplay(c, display_, true);
        return;
    }
    pro::wordmark(c, "sweep", 24, 10, 28, kAccent, version);
    pro::text(c, headline(), {200, 12, 640, 22}, pro::kText, 16, Align::Left, true);
    pro::text(c, idleHelp(), {200, 34, 650, 18}, pro::kDim, 11.5f, Align::Left);

    pro::panel(c, {16, 64, 206, 538});
    pro::sectionTitle(c, "TYPE", 26, 70, kAccent);
    pro::text(c, "REVERB SWELLS  \xC2\xB7  of a sample", {26, 92, 186, 14}, pro::kFaint, 9.5f, Align::Left, true);
    int swells = 0;
    for (const auto& t : W::types()) swells += isSampleType(t) ? 1 : 0;
    pro::text(c, "SYNTH SWEEPS", {26, 110 + 28.0f * (float) swells + 5, 186, 14}, pro::kFaint, 9.5f, Align::Left, true);

    pro::panel(c, {16, 610, 206, 120});
    pro::sectionTitle(c, "SAMPLE", 26, 616, kAccent);
    const std::string name = sweep_.sampleName().empty() ? std::string("none loaded") : sweep_.sampleName();
    pro::text(c, name, {90, 614, 124, 18}, sweep_.hasSample() ? pro::kText : pro::kFaint, 12, Align::Right, true);
    paintRegion(c);
    pro::text(c, onChooseSample ? "or drop one" : "drop one on Sweep", {onChooseSample ? 122.0f : 26.0f, 700, 100, 24}, pro::kFaint, 10.5f,
              Align::Left);

    paintDisplay(c, display_, false);
    pro::text(c, "SOURCE", {234, 300, 64, 32}, pro::kFaint, 10.5f, Align::Left, true);
    pro::text(c, "LENGTH", {234, 340, 64, 28}, pro::kFaint, 10.5f, Align::Left, true);

    struct Panel { const char* title; Rect r; };
    const Panel panels[5] = {{"SWELL", {234, 378, 486, 198}}, {"FILTER", {732, 378, 432, 198}}, {"PITCH", {234, 586, 300, 144}},
                             {"SHAPE", {544, 586, 410, 144}}, {"OUT", {964, 586, 200, 144}}};
    for (const auto& [title, r] : panels)
    {
        pro::panel(c, r);
        pro::sectionTitle(c, title, r.x + 12, r.y + 6, kAccent);
    }
    pro::text(c, "CHARACTER", {kPad.x, 384, kPad.w, 14}, pro::kFaint, 9.5f, Align::Right, true);
    paintCharacterPad(c);
    pro::text(c, "CHORD", {440, 592, 82, 16}, pro::kFaint, 9.5f, Align::Left, true);
    c.drawLine(16, 738, 1164, 738, pro::stroke(pro::kLineSoft, 1.0f));
}

void SweepFace::paintFront(SkCanvas& c, Layout layout)
{
    // The Preview button's label and icon.
    const bool playing = sweep_.progress() >= 0.0f;
    const Rect r = layout == Layout::Full ? Rect{1004, 12, 160, 38} : Rect{316, 118, 212, 44};
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
    if (layout != Layout::Full) return;
    // The SWELL panel works on samples: over a generator it is veiled with what to do.
    if (std::lround(value(W::Source)) != W::Sample)
    {
        const Rect veil{236, 400, 482, 174};
        c.drawRRect(SkRRect::MakeRectXY(veil.sk(), 6, 6), pro::fill(paint::withAlpha(pro::kPanel, 205)));
        pro::text(c, "The swell shapes a sample's reverb.", {veil.x, veil.y + 60, veil.w, 20}, pro::kText, 13, Align::Centre, true);
        pro::text(c, "Drop a sample on Sweep or pick a REVERB SWELL type.", {veil.x, veil.y + 82, veil.w, 18}, pro::kDim, 11.5f, Align::Centre);
    }
    else if (std::lround(value(W::SwellMode)) == W::ReversedSample)
    {
        c.drawRRect(SkRRect::MakeRectXY(kPad.sk(), 8, 8), pro::fill(paint::withAlpha(pro::kPanel, 190)));
        pro::text(c, "Reversed Sample:", {kPad.x, kPad.y + 62, kPad.w, 16}, pro::kText, 11.5f, Align::Centre, true);
        pro::text(c, "a plain reverb (Decay)", {kPad.x, kPad.y + 80, kPad.w, 16}, pro::kDim, 11, Align::Centre);
    }
    paintHelp(c, {20, 740, 1140, 18});
}
} // namespace sonora::ui
