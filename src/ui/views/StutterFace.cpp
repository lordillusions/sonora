#include "ui/views/StutterFace.h"

#include "include/core/SkPathBuilder.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

namespace sonora::ui
{
namespace
{
using S = devices::Stutter;

std::string format(const char* pattern, double value)
{
    char buffer[96];
    std::snprintf(buffer, sizeof buffer, pattern, value);
    return buffer;
}

std::string upper(std::string s)
{
    for (auto& ch : s) ch = (char) std::toupper((unsigned char) ch);
    return s;
}

const char* const kSlow[S::kModes] = {"almost still, every grain swells in", "long reversed swells drifting back",
                                      "scattered fragments with gaps",       "stops dead, blooms into a soft pad",
                                      "a huge slow cloud, left and right",   "sinks into a dark, echoing tube",
                                      "repeats the last beat"};
const char* const kFast[S::kModes] = {"short, tight grains racing ahead", "rushes backwards through the phrase",
                                      "a dense spray of tiny glitches",   "the frozen moment, chopped hard",
                                      "a hard, wide doubler",             "a metallic, fluttering pipe",
                                      "a machine-gun roll"};
const char* const kModeTips[S::kModes] = {
    "Swell: stretches the voice slower and slower; every grain fades in. White key C plays it while held.",
    "Rewind: plays the phrase backwards in reversed grains. White key D.",
    "Shatter: scattered, sometimes reversed or octave-jumping fragments. White key E.",
    "Freeze: holds the moment you pressed: a pad when slow, hard chops when fast. White key F.",
    "Spread: a stereo cloud of detuned grains, left and right in turn. White key G.",
    "Tunnel: grains through a resonant tube, dark and echoing. White key A.",
    "Retrigger: the classic stutter, repeating the slice before you pressed (in time with Sync). White key B."};
} // namespace

StutterFace::StutterFace(devices::Stutter& device) : ProFace(device, kFull, kCompact, kAccent), stutter_(device)
{
    stutter_.readView(view_);
}

std::string StutterFace::speedWords(float speed)
{
    const float a = std::abs(speed);
    if (a < 0.005f) return "frozen";
    std::string words;
    if (std::abs(a - 1.0f) < 0.01f) words = "real time";
    else if (a < 1.0f) words = format("%.1f\xC3\x97 slower", 1.0 / a);
    else words = format("%.1f\xC3\x97 faster", a);
    return speed < 0 ? "backwards, " + words : words;
}

std::string StutterFace::slowLine(int mode) { return kSlow[std::clamp(mode, 0, S::kModes - 1)]; }
std::string StutterFace::fastLine(int mode) { return kFast[std::clamp(mode, 0, S::kModes - 1)]; }

std::string StutterFace::statusLine() const
{
    const auto& s = view_.playing;
    std::string line = speedWords(s.fixedSlice ? 0.0f : s.speed);
    if (s.fixedSlice) line = "repeating";
    std::string every = s.everyBeats > 0 ? std::string(S::noteName(s.everyBeats)) : format("%.0f ms", s.everyMs);
    line += "  \xC2\xB7  " + format("%.0f ms grains", s.grainMs) + " every " + every;
    if (std::abs(s.pitch) > 0.05f) line += "  \xC2\xB7  " + format("%+.0f st", s.pitch);
    return line;
}

bool StutterFace::driven(int param) const
{
    if (value(S::Macro) < 0.5f) return false;
    switch (param)
    {
        case S::Grain: case S::Shape: case S::Every: case S::Speed: case S::Width: case S::Jitter: case S::Space: return true;
        default: return false;
    }
}

float StutterFace::shownValue(int param) const
{
    const auto& s = view_.playing;
    if (! driven(param))
    {
        if (param == S::Every && value(S::Sync) > 0.5f) return s.everyMs;
        return value(param);
    }
    switch (param)
    {
        case S::Grain: return s.grainMs;
        case S::Shape: return s.shape;
        case S::Every: return s.everyMs;
        case S::Speed: return s.fixedSlice ? 0.0f : s.speed;
        case S::Width: return s.width;
        case S::Jitter: return s.jitter;
        case S::Space: return s.space;
        default: return value(param);
    }
}

void StutterFace::willEdit(int param)
{
    if (driven(param)) stutter_.detachMacro();
}

std::string StutterFace::valueText(int param, float v) const
{
    switch (param)
    {
        case S::Motion: return (v < 0.5f ? "SLOW " : "FAST ") + std::to_string((int) std::lround(v * 100.0f));
        case S::Speed: return std::abs(v) < 0.005f ? "0\xC3\x97" : format(std::abs(v) < 1.0f ? "%.2f\xC3\x97" : "%.1f\xC3\x97", v);
        case S::Every:
            if (value(S::Sync) > 0.5f || view_.playing.everyBeats > 0)
                if (const auto* name = S::noteName(S::snapToNote(v, stutter_.tempo())); *name) return name;
            return ProFace::valueText(param, v);
        case S::Tone:
            if (std::abs(v) < 0.02f) return "Open";
            return format(v < 0 ? "Dark %.0f" : "Thin %.0f", std::abs(v) * 100.0);
        default: return ProFace::valueText(param, v);
    }
}

std::string StutterFace::idleHelp() const
{
    const int mode = std::clamp((int) std::lround(value(S::ModeParam)), 0, S::kModes - 1);
    return kModeTips[mode];
}

bool StutterFace::animate()
{
    const bool wasEngaged = view_.engaged;
    const auto newest = view_.inputNewest;
    stutter_.readView(view_);
    return view_.engaged || wasEngaged || newest != view_.inputNewest;
}

void StutterFace::addGrainKnob(const Rect& r, int param, std::string label, std::string tip)
{
    auto& k = addKnob(r, param, std::move(label), std::move(tip));
    k.driven = [this, param] { return driven(param); };
    k.shown = [this, param] { return shownValue(param); };
    if (param == S::Speed)
    {
        k.bipolar = true;
        k.toNorm = [](float s) { return 0.5f + 0.5f * (s < 0 ? -1.0f : 1.0f) * std::sqrt(std::abs(s) / 4.0f); };
        k.fromNorm = [](float t) {
            const float d = 2.0f * (t - 0.5f);
            return (d < 0 ? -4.0f : 4.0f) * d * d;
        };
    }
    switch (param)
    {
        case S::Grain:
            k.caption = [this] {
                const auto& s = view_.playing;
                const float overlap = shownValue(S::Grain) / std::max(1.0f, shownValue(S::Every));
                (void) s;
                return overlap >= 1.05f ? format("overlap \xC3\x97%.1f", overlap) : format("gaps %.0f%%", (1.0 - overlap) * 100.0);
            };
            break;
        case S::Every:
            k.caption = [this] {
                const float ms = shownValue(S::Every);
                return view_.playing.everyBeats > 0 ? format("%.0f ms", ms) : format("%.1f grains/s", 1000.0 / std::max(1.0f, ms));
            };
            break;
        case S::Shape:
            k.caption = [this] {
                const float v = shownValue(S::Shape);
                return std::string(v < 0.15f ? "hard gate" : v > 0.85f ? "swells in & out" : "soft edges");
            };
            break;
        case S::Speed: k.caption = [this] { return speedWords(shownValue(S::Speed)); }; break;
        case S::Jitter:
            k.caption = [this] {
                const float v = shownValue(S::Jitter);
                return std::string(v < 0.05f ? "steady" : v < 0.4f ? "loose" : "scattered");
            };
            break;
        case S::Width:
            k.caption = [this] { return shownValue(S::Width) < 0.02f ? std::string("centre") : std::string("odd L \xC2\xB7 even R"); };
            break;
        case S::Space:
            k.caption = [this] {
                const float v = shownValue(S::Space);
                return std::string(v < 0.02f ? "dry" : v < 0.35f ? "room" : "big room");
            };
            break;
        default: break;
    }
}

void StutterFace::build(Layout layout)
{
    const auto modeLabel = [](int m) { return std::string(S::kModeNames[m]); };
    if (layout == Layout::Full)
    {
        addSegment({920, 18, 80, 28}, S::Sync, 0, "Free", "Free: grain times in milliseconds.");
        addSegment({1004, 18, 140, 28}, S::Sync, 1, "Sync to tempo", "Sync: grains on the song's grid (note values).");
        input_ = {28, 96, 1104, 72};
        output_ = {28, 186, 1104, 160};
        display_ = {16, 64, 1128, 290};
        std::vector<std::string> captures(S::kCaptureNames, S::kCaptureNames + S::kCaptures);
        addSegments({820, 70, 312, 20}, S::Capture, captures, false, 4,
                    "Capture: how far back the grains may read (the last beat, bar, 2 or 4 bars, or all 20 s recorded).");
        addArea(input_, "Input: the last 12 s recorded. Shaded: where the grains may read. The line: the read head.");
        addArea(output_, "Output: what comes out. Each grain is drawn on top as its envelope: pink left, violet right.");

        addPad({16, 366, 170, 146}, "HOLD", [this](bool down) { stutter_.press(down ? S::kPressCurrent : -1); },
               "Hold to stutter (or play a MIDI note: white keys pick the mode, black keys play the selected one).")
            .lit = [this] { return view_.engaged; };
        addToggle({16, 520, 170, 30}, S::Hold, "Latch", "Latch: keeps it stuttering until you click again (automatable).");

        auto& motion = addKnob({206, 392, 156, 168}, S::Motion, "MOTION",
                               "Motion: from Slow (long grains that barely move) to Fast (short grains racing). Each mode reads it its own way.");
        motion.caption = [] { return std::string("\xE2\x97\x80 slow \xC2\xB7 fast \xE2\x96\xB6"); };
        addToggle({214, 566, 140, 26}, S::Macro, "Macro", "Macro: Motion drives the grain knobs. Turn a grain knob to take over by hand.");
        for (int m = 0; m < S::kModes; ++m)
        {
            const float x = 374 + (float) (m % 2) * 132, y = 392 + (float) (m / 2) * 34;
            addSegment({x, y, 124, 28}, S::ModeParam, (float) m, modeLabel(m), kModeTips[m]);
        }

        const float gx = 660, row1 = 390, row2 = 496, kw = 82, kh = 104;
        addGrainKnob({gx, row1, kw, kh}, S::Grain, "GRAIN", "Grain: how long each grain plays.");
        addGrainKnob({gx + kw, row1, kw, kh}, S::Every, "EVERY", "Every: how often a new grain starts (a note value with Sync).");
        addGrainKnob({gx + 2 * kw, row1, kw, kh}, S::Shape, "SHAPE", "Shape: 0 = hard-gated grains, 100 = every grain swells in and out.");
        addGrainKnob({gx + 3 * kw, row1, kw, kh}, S::Speed, "SPEED",
                     "Speed: how fast the read head moves through the recording (1x real time, 0 frozen, below 0 backwards).");
        addGrainKnob({gx, row2, kw, kh}, S::Jitter, "JITTER", "Jitter: scatters where and when the grains start.");
        addGrainKnob({gx + kw, row2, kw, kh}, S::Width, "WIDTH", "Width: odd grains go left, even grains right.");
        auto& pitch = addKnob({gx + 2 * kw, row2, kw, kh}, S::Pitch, "PITCH", "Pitch: transposes the grains (the speed stays).");
        pitch.bipolar = true;
        pitch.caption = [] { return std::string("transpose"); };
        addGrainKnob({gx + 3 * kw, row2, kw, kh}, S::Space, "SPACE", "Space: a diffuse room under the grains; it rings on after you let go.");

        const float ox = 1012;
        auto& tone = addKnob({ox, row1, 64, kh}, S::Tone, "TONE", "Tone: left darker, right thinner.");
        tone.bipolar = true;
        tone.caption = [] { return std::string("filter"); };
        auto& mix = addKnob({ox + 66, row1, 64, kh}, S::Mix, "MIX", "Mix: 100% replaces the voice while it stutters; less keeps the dry voice under it.");
        mix.caption = [this] { return value(S::Mix) > 0.99f ? std::string("stutter only") : std::string("dry under it"); };
        auto& level = addKnob({ox + 18, row2, 74, kh}, S::Output, "LEVEL", "Level: the stutter's volume (the dry voice is untouched while idle).");
        level.caption = [] { return std::string("stutter level"); };
        return;
    }

    // Compact: the device chain.
    input_ = {114, 24, 252, 40};
    output_ = {114, 70, 252, 90};
    display_ = {108, 6, 264, 158};
    addArea(display_, "Input above, output below: the grains are drawn on top. Open the window for every control.");
    addPad({8, 8, 92, 118}, "HOLD", [this](bool down) { stutter_.press(down ? S::kPressCurrent : -1); },
           "Hold to stutter (or play a MIDI note).")
        .lit = [this] { return view_.engaged; };
    addToggle({8, 134, 92, 28}, S::Hold, "Latch", "Latch: keeps it stuttering until you click again.");
    auto& motion = addKnob({378, 6, 90, 128}, S::Motion, "MOTION", "Motion: Slow (long grains, barely moving) to Fast (short, racing).");
    motion.caption = [] { return std::string("slow \xC2\xB7 fast"); };
    addToggle({382, 138, 82, 24}, S::Macro, "Macro", "Macro: Motion drives the grain controls.");
    for (int m = 0; m < S::kModes; ++m)
    {
        const float x = 476 + (float) (m % 2) * 62, y = 8 + (float) (m / 2) * 39;
        auto& seg = addSegment({x, y, 58, 33}, S::ModeParam, (float) m, modeLabel(m), kModeTips[m]);
        seg.textSize = 10.0f;
    }
    addGrainKnob({606, 4, 88, 80}, S::Speed, "SPEED", "Speed through the recording (1x real time, 0 frozen, below 0 backwards).");
    auto& mix = addKnob({606, 88, 88, 80}, S::Mix, "MIX", "Mix: 100% replaces the voice while it stutters.");
    mix.caption = [] { return std::string(); };
}

void StutterFace::paintDisplay(SkCanvas& c, const Rect& in, const Rect& out, bool compact)
{
    const double sr = std::max(1.0, view_.sampleRate);
    const auto& s = view_.playing;
    const float label = compact ? 8.5f : 10.5f;
    pro::rounded(c, in, 4, 0xFF0E1115);
    pro::rounded(c, out, 4, 0xFF0E1115);

    // ---- input: the recording, the capture window, the read head, where the grains read
    const auto xIn = [&](int64_t sample) {
        return in.x + in.w - (float) ((double) (view_.inputNewest - sample) / (sr * S::kInputSeconds)) * in.w;
    };
    int64_t windowFrom = 0, windowTo = view_.now;
    if (view_.engaged) windowFrom = view_.windowStart;
    else
    {
        const int capture = std::clamp((int) std::lround(value(S::Capture)), 0, S::kCaptures - 1);
        const double beats = S::kCaptureBeats[capture];
        windowFrom = beats > 0 ? view_.now - (int64_t) (beats * 60.0 / stutter_.tempo() * sr) : view_.now - (int64_t) (S::kHistorySeconds * sr);
    }
    const float wx0 = std::clamp(xIn(windowFrom), in.x, in.x + in.w), wx1 = std::clamp(xIn(windowTo), in.x, in.x + in.w);
    if (wx1 > wx0)
    {
        pro::rounded(c, {wx0, in.y, wx1 - wx0, in.h}, 3, paint::withAlpha(kAccent, view_.engaged ? 44 : 22));
        c.drawLine(wx0, in.y, wx0, in.y + in.h, pro::stroke(paint::withAlpha(kAccent, 140), 1.0f));
    }
    if (view_.inputBins > 1)
    {
        const float x0 = in.x + in.w * (1.0f - (float) view_.inputBins / (float) S::kBins);
        pro::waveform(c, view_.inMin.data(), view_.inMax.data(), view_.inputBins, {x0, in.y + 2, in.x + in.w - x0, in.h - 4},
                      0xFF5E6573, 1.6f);
    }
    if (view_.engaged)
    {
        const float hx = std::clamp(xIn(view_.head), in.x, in.x + in.w);
        c.drawLine(hx, in.y, hx, in.y + in.h, pro::stroke(0xFFFFFFFF, 1.5f));
        SkPathBuilder tri;
        tri.moveTo(hx - 4, in.y);
        tri.lineTo(hx + 4, in.y);
        tri.lineTo(hx, in.y + 5);
        tri.close();
        c.drawPath(tri.detach(), pro::fill(0xFFFFFFFF));
    }
    for (const auto& g : view_.grains)
    {
        const double age = (double) (view_.now - g.at) / sr;
        if (age > 0.8) continue;
        const float x = xIn(g.read);
        if (x < in.x || x > in.x + in.w) continue;
        const auto alpha = (uint8_t) std::clamp(255.0 * (1.0 - age / 0.8), 30.0, 255.0);
        c.drawLine(x, in.y + in.h - 7, x, in.y + in.h - 1, pro::stroke(paint::withAlpha(g.pan < 0 ? kAccent : kAccent2, alpha), 1.5f));
    }
    pro::text(c, "INPUT", {in.x + 6, in.y + 2, 60, label + 4}, pro::kFaint, label, Align::Left, true);

    // ---- output: the waveform and every grain as its envelope
    const auto xOut = [&](int64_t sample) {
        return out.x + out.w - (float) ((double) (view_.outputNewest - sample) / (sr * S::kOutputSeconds)) * out.w;
    };
    for (int t = 1; t < 8; ++t) // half-second ticks
    {
        const float x = out.x + out.w * (float) t / 8.0f;
        c.drawLine(x, out.y + 4, x, out.y + out.h - 4, pro::stroke(0xFF181C22, 1.0f));
    }
    if (view_.outputBins > 1)
    {
        const float x0 = out.x + out.w * (1.0f - (float) view_.outputBins / (float) S::kBins);
        const Rect wave{x0, out.y + out.h * 0.22f, out.x + out.w - x0, out.h * 0.74f};
        pro::waveform(c, view_.outMin.data(), view_.outMax.data(), view_.outputBins, wave,
                      view_.engaged ? paint::withAlpha(kAccent, 215) : 0xFF6A7181, 1.3f);
    }
    const float band = out.h * 0.17f, top = out.y + 4;
    c.save();
    c.clipRect(SkRect::MakeXYWH(out.x, out.y, out.w, out.h));
    for (const auto& g : view_.grains)
    {
        const float x0 = xOut(g.at), x1 = xOut(g.at + g.length);
        if (x1 < out.x || x0 > out.x + out.w) continue;
        const float f = std::clamp(s.shape, 0.02f, 1.0f) * 0.5f * (x1 - x0);
        const float y0 = top + (g.pan < 0 ? 0.0f : band * 0.35f), y1 = y0 + band * 0.65f;
        SkPathBuilder p;
        p.moveTo(x0, y1);
        if (g.reversed) { p.lineTo(x0, y0); p.lineTo(x1 - f, y0); }
        else { p.lineTo(x0 + f, y0); p.lineTo(x1 - f, y0); }
        p.lineTo(x1, y1);
        p.close();
        const SkColor colour = g.pan < 0 ? kAccent : kAccent2;
        const auto path = p.detach();
        c.drawPath(path, pro::fill(paint::withAlpha(colour, 70)));
        c.drawPath(path, pro::stroke(paint::withAlpha(colour, 210), 1.0f));
    }
    c.restore();
    pro::text(c, "OUTPUT", {out.x + 6, out.y + out.h - label - 6, 80, label + 4}, pro::kFaint, label, Align::Left, true);
    if (! view_.engaged && ! compact)
        pro::text(c, "hold the pad, play a MIDI note or automate Hold",
                  {out.x, out.y + out.h * 0.5f - 10, out.w, 20}, paint::withAlpha(pro::kDim, 170), 13.0f);
}

void StutterFace::paintBack(SkCanvas& c, Layout layout)
{
    if (layout == Layout::Compact)
    {
        pro::well(c, display_, 6);
        paintDisplay(c, input_, output_, true);
        return;
    }
    pro::wordmark(c, "stutter", 24, 10, 28, kAccent, version);
    const int mode = std::clamp(view_.mode, 0, S::kModes - 1);
    const std::string name = upper(S::kModeNames[mode]);
    const float nameW = pro::textWidth(name, 16, true);
    const float sx = 230;
    pro::rounded(c, {sx, 22, 10, 10}, 5, view_.engaged ? kAccent : 0xFF3A404B);
    pro::text(c, name, {sx + 18, 14, nameW + 4, 26}, view_.engaged ? kAccent : pro::kText, 16, Align::Left, true);
    pro::text(c, view_.engaged ? statusLine() : "ready  \xC2\xB7  recording  \xC2\xB7  " + statusLine(),
              {sx + 26 + nameW, 14, 900 - sx - nameW - 30, 26}, pro::kDim, 13, Align::Left);

    pro::well(c, display_, 8);
    paintDisplay(c, input_, output_, false);
    pro::text(c, "CAPTURE", {752, 70, 64, 20}, pro::kFaint, 10, Align::Right, true);

    pro::panel(c, {198, 362, 444, 238});
    pro::sectionTitle(c, "MOTION & MODE", 210, 368, kAccent);
    pro::panel(c, {652, 362, 346, 238});
    pro::sectionTitle(c, "GRAIN", 664, 368, kAccent);
    if (value(S::Macro) > 0.5f)
        pro::text(c, "driven by Motion", {760, 368, 226, 18}, paint::withAlpha(kAccent, 200), 10.5f, Align::Right, true);
    pro::panel(c, {1006, 362, 138, 238});
    pro::sectionTitle(c, "OUT", 1018, 368, kAccent);

    c.drawLine(16, 608, 1144, 608, pro::stroke(pro::kLineSoft, 1.0f));
}

void StutterFace::paintFront(SkCanvas& c, Layout layout)
{
    // The white key that plays each mode.
    for (const auto& control : controls())
        if (control.kind == Kind::Segment && control.param == S::ModeParam)
        {
            const int m = (int) control.value;
            const bool on = std::lround(value(S::ModeParam)) == m;
            const float size = layout == Layout::Full ? 10.0f : 8.0f;
            pro::text(c, S::kModeKeys[m], {control.r.x + control.r.w - size - 6, control.r.y + 2, size + 4, size + 2},
                      on ? paint::withAlpha(0xFF000000, 120) : pro::kFaint, size, Align::Right, true);
        }
    if (layout == Layout::Compact) return;

    // Hold pad hint.
    pro::text(c, "or play a MIDI note", {16, 466, 170, 18}, view_.engaged ? paint::withAlpha(0xFF000000, 150) : pro::kFaint, 11);

    // What Slow and Fast do in this mode.
    const int mode = std::clamp((int) std::lround(value(S::ModeParam)), 0, S::kModes - 1);
    pro::text(c, "SLOW", {374, 534, 44, 18}, kAccent, 10, Align::Left, true);
    pro::text(c, slowLine(mode), {416, 534, 220, 18}, pro::kDim, 11.5f, Align::Left);
    pro::text(c, "FAST", {374, 558, 44, 18}, kAccent, 10, Align::Left, true);
    pro::text(c, fastLine(mode), {416, 558, 220, 18}, pro::kDim, 11.5f, Align::Left);

    // Output level.
    const float db = view_.level > 1.0e-5f ? 20.0f * std::log10(view_.level) : -100.0f;
    const float t = std::clamp((db + 48.0f) / 54.0f, 0.0f, 1.0f);
    const Rect meter{1132, 500, 5, 92};
    pro::rounded(c, meter, 2, 0xFF0E1115);
    if (t > 0) pro::rounded(c, {meter.x, meter.y + meter.h * (1 - t), meter.w, meter.h * t}, 2, db > -1.0f ? 0xFFFF5A5A : kAccent);

    paintHelp(c, {20, 612, 1120, 22});
}
} // namespace sonora::ui
