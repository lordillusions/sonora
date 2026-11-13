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

std::string beatsText(double beats)
{
    if (beats < 0.99) return format("%.2g beat", beats);
    return format(std::abs(beats - std::round(beats)) < 0.01 ? "%.0f beats" : "%.1f beats", beats);
}
} // namespace

SkColor StutterFace::modeColour(int mode)
{
    static constexpr SkColor kColours[S::kModes] = {0xFFFF4F8B, 0xFF9B87FF, 0xFFFF9F43, 0xFF6FD3FF, 0xFF4FE3B0, 0xFF5B7CFF, 0xFFFFD34F};
    return kColours[std::clamp(mode, 0, S::kModes - 1)];
}

std::string StutterFace::autoLine() const
{
    const auto& p = S::preset(view_.autoPreset);
    std::string line = std::string("AUTO  \xC2\xB7  ") + p.name;
    if (view_.autoEngaged && view_.autoMove >= 0 && view_.autoMove < p.count)
        return line + "  \xC2\xB7  " + p.moves[(size_t) view_.autoMove].label + "  \xC2\xB7  " + statusLine();
    if (view_.autoPosition < 0) return line + "  \xC2\xB7  waiting for the voice";
    const int bars = std::max(1, (int) std::lround(p.cycleBeats / 4.0));
    line += "  \xC2\xB7  bar " + std::to_string(std::min(bars, (int) (view_.autoPosition / 4.0) + 1)) + " of " + std::to_string(bars);
    if (view_.autoResting) return line + "  \xC2\xB7  no voice, resting";
    double away = 0;
    const int next = S::nextMove(p, view_.autoPosition, S::tierFor(value(S::AutoAmount)), away);
    if (next >= 0) line += std::string("  \xC2\xB7  next: ") + p.moves[(size_t) next].label + " in " + beatsText(away);
    return line;
}

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
    if (value(S::Macro) < 0.5f && view_.autoMove < 0) return false;
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
        case S::AutoAmount: return v < 0.34f ? "LIGHT" : v < 0.67f ? "MEDIUM" : "WILD";
        default: return ProFace::valueText(param, v);
    }
}

std::string StutterFace::idleHelp() const
{
    if (value(S::Auto) > 0.5f) return S::preset(view_.autoPreset).blurb;
    const int mode = std::clamp((int) std::lround(value(S::ModeParam)), 0, S::kModes - 1);
    return kModeTips[mode];
}

bool StutterFace::animate()
{
    const bool wasEngaged = view_.engaged;
    const auto newest = view_.inputNewest;
    const double position = view_.autoPosition;
    stutter_.readView(view_);
    return view_.engaged || wasEngaged || newest != view_.inputNewest || std::abs(position - view_.autoPosition) > 1e-9;
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
        input_ = {28, 96, 1104, 64};
        output_ = {28, 170, 1104, 136};
        display_ = {16, 64, 1128, 250};
        std::vector<std::string> captures(S::kCaptureNames, S::kCaptureNames + S::kCaptures);
        addSegments({820, 70, 312, 20}, S::Capture, captures, false, 4,
                    "Capture: how far back the grains may read (the last beat, bar, 2 or 4 bars, or all 20 s recorded).");
        addArea(input_, "Input: the last 12 s recorded. Shaded: where the grains may read. The line: the read head. Green: where the voice was heard.");
        addArea(output_, "Output: what comes out. Each grain is drawn on top as its envelope: pink left, violet right.");

        // AUTO: one click, a preset plays a choreography of stutters on the bars while the voice is there.
        auto& autoButton = addButton({28, 334, 150, 58}, "AUTO", [this] { setValue(S::Auto, value(S::Auto) > 0.5f ? 0.0f : 1.0f); },
                                     "Auto: one click and the preset plays by itself on the song's bars, only while the voice is there.");
        autoButton.lit = [this] { return value(S::Auto) > 0.5f; };
        autoButton.textSize = 20;
        addArea({28, 400, 150, 58}, "Voice: lights while the voice (or any sound) comes in; the bar is its level, the tick the threshold (Sensitivity).");
        for (int k = 0; k < S::kPresets; ++k)
        {
            auto& chip = addSegment({192 + (float) (k % 4) * 146, 342 + (float) (k / 4) * 38, 140, 32}, S::AutoPreset, (float) k,
                                    S::preset(k).name, S::preset(k).blurb);
            chip.textSize = 12.5f;
        }
        addSegments({786, 342, 190, 32}, S::AutoFollow, {"Bars", "Voice"}, false, 4,
                    "Follow: Bars = the preset sits on the song's bars. Voice = it starts when the voice comes in (always when the transport is stopped).");
        auto& amount = addKnob({988, 334, 74, 78}, S::AutoAmount, "AMOUNT",
                               "Amount: how many of the preset's moves play. Light: the main ones. Wild: all of them (the dim blocks light up).");
        amount.small = true;
        auto& sensitivity = addKnob({1064, 334, 76, 78}, S::Sensitivity, "SENSITIVITY",
                                    "Sensitivity: how quiet a voice still counts (raise it for soft vocals, lower it for noisy recordings).");
        sensitivity.small = true;
        timeline_ = {192, 420, 940, 40};
        addArea(timeline_, "The preset's cycle, bar by bar: each block is a move (colour = mode). Dim blocks play at a higher Amount. "
                           "The white line is where the song is.");

        addPad({16, 484, 170, 108}, "HOLD", [this](bool down) { stutter_.press(down ? S::kPressCurrent : -1); },
               "Hold to stutter (or play a MIDI note: white keys pick the mode, black keys play the selected one).")
            .lit = [this] { return view_.engaged && ! view_.autoEngaged; };
        addToggle({16, 602, 170, 30}, S::Hold, "Latch", "Latch: keeps it stuttering until you click again (automatable).");

        auto& motion = addKnob({206, 510, 156, 168}, S::Motion, "MOTION",
                               "Motion: from Slow (long grains that barely move) to Fast (short grains racing). Each mode reads it its own way.");
        motion.caption = [] { return std::string("\xE2\x97\x80 slow \xC2\xB7 fast \xE2\x96\xB6"); };
        addToggle({214, 684, 140, 26}, S::Macro, "Link",
                  "Link: Motion turns the grain knobs for you. Turn a grain knob to take over by hand. (Automatic effects in a sequence: AUTO and a preset.)");
        for (int m = 0; m < S::kModes; ++m)
        {
            const float x = 374 + (float) (m % 2) * 132, y = 510 + (float) (m / 2) * 34;
            addSegment({x, y, 124, 28}, S::ModeParam, (float) m, modeLabel(m), kModeTips[m]);
        }

        const float gx = 660, row1 = 508, row2 = 614, kw = 82, kh = 104;
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
    input_ = {114, 22, 252, 38};
    output_ = {114, 64, 252, 80};
    display_ = {108, 6, 264, 158};
    timeline_ = {114, 149, 252, 11};
    addArea(display_, "Input above, output below: the grains are drawn on top; with AUTO on, the preset's cycle under them.");
    addPad({8, 8, 92, 62}, "HOLD", [this](bool down) { stutter_.press(down ? S::kPressCurrent : -1); },
           "Hold to stutter (or play a MIDI note).")
        .lit = [this] { return view_.engaged && ! view_.autoEngaged; };
    auto& autoButton = addButton({8, 76, 92, 26}, "AUTO", [this] { setValue(S::Auto, value(S::Auto) > 0.5f ? 0.0f : 1.0f); },
                                 "Auto: one click and the preset plays by itself on the song's bars while the voice is there.");
    autoButton.lit = [this] { return value(S::Auto) > 0.5f; };
    autoButton.textSize = 12;
    addButton({8, 106, 92, 26}, "", [this] { setValue(S::AutoPreset, (float) ((view_.autoPreset + 1) % S::kPresets)); },
              "Preset: click for the next one. Open the window to see them all, with Amount and Follow.");
    addToggle({8, 138, 92, 24}, S::Hold, "Latch", "Latch: keeps it stuttering until you click again.");
    auto& motion = addKnob({378, 6, 90, 128}, S::Motion, "MOTION", "Motion: Slow (long grains, barely moving) to Fast (short, racing).");
    motion.caption = [] { return std::string("slow \xC2\xB7 fast"); };
    addToggle({382, 138, 82, 24}, S::Macro, "Link", "Link: Motion turns the grain controls for you.");
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
    if (view_.autoOn && view_.inputBins > 1) // where the voice detector heard the voice
    {
        const float binW = in.w / (float) S::kBins, x0 = in.x + in.w * (1.0f - (float) view_.inputBins / (float) S::kBins);
        for (int b = 0; b < view_.inputBins; ++b)
            if (view_.voice[(size_t) b] != 0)
                pro::rounded(c, {x0 + (float) b * binW, in.y + in.h - 4, binW + 0.5f, 3}, 1, paint::withAlpha(0xFF5BE38C, 190));
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
        pro::text(c, view_.autoOn ? "AUTO is on: the preset plays when the voice is there" : "hold the pad, play a MIDI note, or click AUTO and pick a preset",
                  {out.x, out.y + out.h * 0.5f - 10, out.w, 20}, paint::withAlpha(pro::kDim, 170), 13.0f);
}

void StutterFace::paintTimeline(SkCanvas& c, const Rect& r, bool compact)
{
    const auto& p = S::preset(view_.autoPreset);
    const int tier = S::tierFor(value(S::AutoAmount));
    const bool on = view_.autoOn;
    const float cycle = (float) p.cycleBeats;
    const auto xAt = [&](double beat) { return r.x + r.w * (float) (beat / p.cycleBeats); };
    pro::rounded(c, r, compact ? 2.0f : 4.0f, 0xFF0E1115);
    const float lane = compact ? r.y : r.y + 14, laneH = compact ? r.h : r.h - 16;
    // bars and beats
    for (int beat = 0; beat < (int) cycle; ++beat)
    {
        const float x = xAt(beat);
        const bool bar = beat % 4 == 0;
        if (beat > 0) c.drawLine(x, bar ? r.y : lane + laneH - 5, x, r.y + r.h, pro::stroke(bar ? pro::kLine : pro::kLineSoft, 1.0f));
        if (bar && ! compact)
            pro::text(c, std::to_string(beat / 4 + 1), {x + 4, r.y + 1, 30, 12}, pro::kFaint, 9.5f, Align::Left, true);
    }
    // the moves
    for (int i = 0; i < p.count; ++i)
    {
        const auto& m = p.moves[(size_t) i];
        const bool plays = m.tier <= tier;
        const bool now = on && view_.autoMove == i;
        const SkColor colour = modeColour(m.mode);
        const Rect block{xAt(m.at) + 1, lane + 1, std::max(2.0f, xAt(m.at + m.length) - xAt(m.at) - 2), laneH - 2};
        const float radius = compact ? 1.5f : 3.0f;
        if (now)
            for (int g = 2; g >= 1; --g)
                pro::rounded(c, {block.x - (float) g * 2, block.y - (float) g * 2, block.w + (float) g * 4, block.h + (float) g * 4},
                             radius + (float) g * 2, paint::withAlpha(colour, (uint8_t) (40 * (3 - g))));
        if (plays) pro::rounded(c, block, radius, paint::withAlpha(colour, now ? 255 : on ? 185 : 110));
        else pro::roundedStroke(c, block, radius, paint::withAlpha(colour, 90), 1.0f);
        if (now) // progress
            pro::rounded(c, {block.x, block.y + block.h - 3, block.w * std::clamp(view_.autoProgress, 0.0f, 1.0f), 3}, 1,
                         paint::withAlpha(0xFFFFFFFF, 200));
        if (! compact)
        {
            float size = 10.5f;
            while (size > 8.0f && pro::textWidth(m.label, size, true) > block.w - 4) size -= 0.5f;
            if (pro::textWidth(m.label, size, true) <= block.w - 4) // too narrow: the colour says the mode
                pro::text(c, m.label, block, plays ? paint::withAlpha(0xFF000000, now ? 230 : 190) : paint::withAlpha(colour, 150), size,
                          Align::Centre, true);
        }
    }
    // where the song is
    if (on && view_.autoPosition >= 0)
    {
        const float x = std::clamp(xAt(view_.autoPosition), r.x, r.x + r.w);
        c.drawLine(x, r.y - (compact ? 1.0f : 2.0f), x, r.y + r.h + (compact ? 1.0f : 2.0f), pro::stroke(0xFFFFFFFF, compact ? 1.5f : 2.0f));
    }
    if (! compact)
        pro::text(c, view_.autoResting ? "no voice: resting" : (on ? "" : "AUTO off"), {r.x + r.w - 160, r.y + 1, 156, 12},
                  pro::kFaint, 9.5f, Align::Right, true);
}

void StutterFace::paintBack(SkCanvas& c, Layout layout)
{
    if (layout == Layout::Compact)
    {
        pro::well(c, display_, 6);
        paintDisplay(c, input_, output_, true);
        if (view_.autoOn) paintTimeline(c, timeline_, true);
        return;
    }
    pro::wordmark(c, "stutter", 24, 10, 28, kAccent, version);
    const int mode = std::clamp(view_.mode, 0, S::kModes - 1);
    const std::string name = upper(S::kModeNames[mode]);
    const float nameW = pro::textWidth(name, 16, true);
    const float sx = 230;
    pro::rounded(c, {sx, 22, 10, 10}, 5, view_.engaged ? kAccent : 0xFF3A404B);
    pro::text(c, name, {sx + 18, 14, nameW + 4, 26}, view_.engaged ? kAccent : pro::kText, 16, Align::Left, true);
    std::string status = view_.engaged ? statusLine() : "ready  \xC2\xB7  recording  \xC2\xB7  " + statusLine();
    if (view_.autoOn && (view_.autoEngaged || ! view_.engaged)) status = autoLine();
    pro::text(c, status,
              {sx + 26 + nameW, 14, 900 - sx - nameW - 30, 26}, pro::kDim, 13, Align::Left);

    pro::well(c, display_, 8);
    paintDisplay(c, input_, output_, false);
    pro::text(c, "CAPTURE", {752, 70, 64, 20}, pro::kFaint, 10, Align::Right, true);

    pro::panel(c, {16, 322, 1128, 148});
    pro::text(c, "PRESET", {192, 325, 120, 15}, pro::kFaint, 9.5f, Align::Left, true);
    pro::text(c, "FOLLOW", {786, 325, 120, 15}, pro::kFaint, 9.5f, Align::Left, true);
    const bool followVoice = std::lround(value(S::AutoFollow)) == S::FollowVoice;
    pro::text(c, followVoice ? "starts when the voice comes in" : "sits on the song's bars", {786, 380, 190, 18}, pro::kDim, 11, Align::Left);
    pro::text(c, followVoice ? "(after a bar of silence)" : "(stopped: from the voice)", {786, 396, 190, 16}, pro::kFaint, 10, Align::Left);
    paintTimeline(c, timeline_, false);

    pro::panel(c, {198, 480, 444, 238});
    pro::sectionTitle(c, "MOTION & MODE", 210, 486, kAccent);
    pro::panel(c, {652, 480, 346, 238});
    pro::sectionTitle(c, "GRAIN", 664, 486, kAccent);
    if (view_.autoMove >= 0)
        pro::text(c, "driven by AUTO", {760, 486, 226, 18}, paint::withAlpha(kAccent, 200), 10.5f, Align::Right, true);
    else if (value(S::Macro) > 0.5f)
        pro::text(c, "linked to Motion", {760, 486, 226, 18}, paint::withAlpha(kAccent, 200), 10.5f, Align::Right, true);
    pro::panel(c, {1006, 480, 138, 238});
    pro::sectionTitle(c, "OUT", 1018, 486, kAccent);

    c.drawLine(16, 726, 1144, 726, pro::stroke(pro::kLineSoft, 1.0f));
}

void StutterFace::paintFront(SkCanvas& c, Layout layout)
{
    // The white key that plays each mode; a ring around the mode playing when it is not the selected one (AUTO, MIDI).
    for (const auto& control : controls())
        if (control.kind == Kind::Segment && control.param == S::ModeParam)
        {
            const int m = (int) control.value;
            if (view_.engaged && view_.mode == m && std::lround(value(S::ModeParam)) != m)
                pro::roundedStroke(c, control.r, std::min(6.0f, control.r.h * 0.3f), modeColour(m), 2.0f);
            const bool on = std::lround(value(S::ModeParam)) == m;
            const float size = layout == Layout::Full ? 10.0f : 8.0f;
            pro::text(c, S::kModeKeys[m], {control.r.x + control.r.w - size - 6, control.r.y + 2, size + 4, size + 2},
                      on ? paint::withAlpha(0xFF000000, 120) : pro::kFaint, size, Align::Right, true);
        }
    if (layout == Layout::Compact)
    {
        // The preset button shows the preset's name.
        const auto& name = std::string(S::preset(view_.autoPreset).name);
        const Rect r{8, 106, 92, 26};
        float size = 10.5f;
        while (size > 7.5f && pro::textWidth(name, size, true) > r.w - 6) size -= 0.5f;
        pro::text(c, name, r, value(S::Auto) > 0.5f ? pro::kText : pro::kDim, size, Align::Centre, true);
        return;
    }

    // Voice: the detector's light, level and threshold.
    {
        const Rect r{28, 404, 150, 50};
        const SkColor voice = 0xFF5BE38C;
        c.drawCircle(r.x + 10, r.y + 12, 6, pro::fill(view_.voiced ? voice : 0xFF2A2F38));
        if (view_.voiced) c.drawCircle(r.x + 10, r.y + 12, 10, pro::fill(paint::withAlpha(voice, 50)));
        pro::text(c, view_.voiced ? "VOICE" : "no voice", {r.x + 24, r.y + 3, 110, 18}, view_.voiced ? voice : pro::kFaint, 11, Align::Left, true);
        const Rect bar{r.x + 4, r.y + 32, r.w - 8, 8};
        pro::rounded(c, bar, 3, 0xFF0E1115);
        const auto toX = [&](float db) { return bar.x + bar.w * std::clamp((db + 72.0f) / 72.0f, 0.0f, 1.0f); };
        const float x = toX(view_.envDb);
        if (x > bar.x + 1) pro::rounded(c, {bar.x, bar.y, x - bar.x, bar.h}, 3, view_.voiced ? voice : 0xFF59606E);
        const float tx = toX(view_.thresholdDb);
        c.drawLine(tx, bar.y - 3, tx, bar.y + bar.h + 3, pro::stroke(pro::kText, 1.5f));
    }

    // Hold pad hint.
    pro::text(c, "or play a MIDI note", {16, 564, 170, 18}, view_.engaged && ! view_.autoEngaged ? paint::withAlpha(0xFF000000, 150) : pro::kFaint, 11);

    // What Slow and Fast do in this mode.
    const int mode = std::clamp((int) std::lround(value(S::ModeParam)), 0, S::kModes - 1);
    pro::text(c, "SLOW", {374, 652, 44, 18}, kAccent, 10, Align::Left, true);
    pro::text(c, slowLine(mode), {416, 652, 220, 18}, pro::kDim, 11.5f, Align::Left);
    pro::text(c, "FAST", {374, 676, 44, 18}, kAccent, 10, Align::Left, true);
    pro::text(c, fastLine(mode), {416, 676, 220, 18}, pro::kDim, 11.5f, Align::Left);

    // Output level.
    const float db = view_.level > 1.0e-5f ? 20.0f * std::log10(view_.level) : -100.0f;
    const float t = std::clamp((db + 48.0f) / 54.0f, 0.0f, 1.0f);
    const Rect meter{1132, 618, 5, 92};
    pro::rounded(c, meter, 2, 0xFF0E1115);
    if (t > 0) pro::rounded(c, {meter.x, meter.y + meter.h * (1 - t), meter.w, meter.h * t}, 2, db > -1.0f ? 0xFFFF5A5A : kAccent);

    paintHelp(c, {20, 730, 1120, 22});
}
} // namespace sonora::ui
