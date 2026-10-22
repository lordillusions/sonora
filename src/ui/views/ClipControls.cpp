#include "ui/views/ClipControls.h"
#include "ui/views/PianoRoll.h"

#include "ui/WheelScale.h"

#include "ui/AudioClipEdits.h"
#include "ui/Controls.h"
#include "ui/MidiClipEdits.h"
#include "ui/Painter.h"
#include "ui/UiContext.h"
#include "core/TimeUtils.h"
#include "engine/Transport.h"

#include "include/core/SkPaint.h"
#include "include/core/SkRRect.h"

#include <juce_gui_basics/juce_gui_basics.h>
#include <cstdlib>

#include <algorithm>
#include <cmath>
#include <cstdio>

// The control ids are handled selectively in several switches.
#pragma clang diagnostic ignored "-Wswitch-enum"

namespace sonora::ui
{

namespace
{
constexpr float kPad = 9.0f, kBox = 13.0f, kTitle = 18.0f, kHeader = 18.0f, kCaption = 12.0f;
constexpr double kMinGainDb = -70.0, kMaxGainDb = 24.0;

const char* warpName(model::WarpMode m)
{
    switch (m)
    {
        case model::WarpMode::Beats: return "Beats";
        case model::WarpMode::Tones: return "Tones";
        case model::WarpMode::Texture: return "Texture";
        case model::WarpMode::RePitch: return "Re-Pitch";
        case model::WarpMode::Complex: return "Complex";
        case model::WarpMode::ComplexPro: return "Complex Pro";
    }
    return "Beats";
}

// Beats mode "Preserve" in Live's menu order (stored value in brackets in Model.h: 6 = Transients, 3 = 1/8 ...).
constexpr int kPreserveOrder[] = {6, 3, 4, 5, 2, 1, 0};
const char* preserveName(int v)
{
    static const char* names[] = {"1 Bar", "1/2", "1/4", "1/8", "1/16", "1/32", "Transients"};
    return names[std::clamp(v, 0, 6)];
}
const char* launchModeName(int v)
{
    static const char* names[] = {"Trigger", "Gate", "Toggle", "Repeat"};
    return names[std::clamp(v, 0, 3)];
}
const char* launchQuantName(int v)
{
    static const char* names[] = {"Global", "None", "8 Bars", "4 Bars", "2 Bars", "1 Bar", "1/2", "1/2T", "1/4", "1/4T", "1/8", "1/8T",
                                  "1/16", "1/16T", "1/32"};
    return names[std::clamp(v, 0, 14)];
}
const char* loopModeName(int v)
{
    static const char* names[] = {"Loop Off", "Loop Forward", "Loop Back-and-Forth"};
    return names[std::clamp(v, 0, 2)];
}

std::string number(const char* format, double v)
{
    char text[32];
    std::snprintf(text, sizeof(text), format, v);
    return text;
}

double gainDb(double gain) { return gain <= 1e-4 ? kMinGainDb : std::clamp(20.0 * std::log10(gain), kMinGainDb, kMaxGainDb); }

// Live's clip gain fader is a cube-root amplitude taper: 24 dB at the top, -70 dB at the bottom, 0 dB about 60 % down.
// Positions run 0 (bottom) .. 1 (top).
double faderPosition(double db)
{
    const auto taper = [](double d) { return std::pow(10.0, (d - kMaxGainDb) / 60.0); };
    const double floor = taper(kMinGainDb);
    return std::clamp((taper(db) - floor) / (1.0 - floor), 0.0, 1.0);
}
double faderDb(double position)
{
    const double floor = std::pow(10.0, (kMinGainDb - kMaxGainDb) / 60.0);
    const double p = floor + std::clamp(position, 0.0, 1.0) * (1.0 - floor);
    return std::clamp(kMaxGainDb + 60.0 * std::log10(p), kMinGainDb, kMaxGainDb);
}

// Momentary rounded buttons (Set, /2, x2, Reverse, Stems).
void pillButton(SkCanvas& c, const Theme& t, const Rect& r, std::string_view label, bool enabled, bool on = false)
{
    if (r.isEmpty()) return;
    SkPaint p;
    p.setAntiAlias(true);
    const auto shape = SkRRect::MakeRectXY(Rect{r.x + 0.5f, r.y + 0.5f, r.w - 1, r.h - 1}.sk(), r.h * 0.5f, r.h * 0.5f);
    p.setColor(on ? t.color("ChosenDefault") : t.color("SurfaceHighlight"));
    c.drawRRect(shape, p);
    p.setStyle(SkPaint::kStroke_Style);
    p.setColor(enabled ? t.color("ControlContrastFrame") : t.color("TextDisabled"));
    c.drawRRect(shape, p);
    paint::text(c, label, r, enabled ? t.color("ControlForeground") : t.color("TextDisabled"), t.metrics.fontSize, Align::Centre);
}

// Bars.beats.sixteenths in three right-aligned slots ("  1.  1.  1"), other values right-aligned.
void positionField(SkCanvas& c, const Theme& t, const Rect& r, const std::string& text)
{
    if (r.isEmpty()) return;
    ctl::framed(c, t, r, t.color("ControlTextBack"));
    const SkColor fg = t.color("ControlForeground");
    std::vector<std::string> parts;
    size_t from = 0;
    for (size_t dot = text.find(". "); dot != std::string::npos; dot = text.find(". ", from))
    {
        parts.push_back(text.substr(from, dot - from + 1));
        from = dot + 2;
    }
    parts.push_back(text.substr(from));
    if (parts.size() != 3)
    {
        paint::text(c, text, r.reduced(3, 0), fg, t.metrics.fontSize, Align::Right);
        return;
    }
    const float slot = (r.w - 6) / 3.0f;
    for (size_t i = 0; i < 3; ++i)
        paint::text(c, parts[i], {r.x + 3 + slot * (float) i, r.y, slot, r.h}, fg, t.metrics.fontSize, Align::Right);
}

// Numeric box drawn as a horizontal slider: the filled part shows the value (Transient Envelope).
void sliderBox(SkCanvas& c, const Theme& t, const Rect& r, const std::string& text, double fraction)
{
    if (r.isEmpty()) return;
    ctl::framed(c, t, r, t.color("ControlTextBack"));
    const float fill = (r.w - 2) * (float) std::clamp(fraction, 0.0, 1.0);
    paint::fill(c, {r.x + 1, r.y + 1, fill, r.h - 2}, t.color("RangeDefault"));
    paint::text(c, text, r, t.color("ControlForeground"), t.metrics.fontSize, Align::Centre);
}

// The Loop button's icon: a rounded loop with an arrow head.
void loopIcon(SkCanvas& c, float x, float cy, SkColor color)
{
    SkPaint p;
    p.setAntiAlias(true);
    p.setStyle(SkPaint::kStroke_Style);
    p.setStrokeWidth(1.2f);
    p.setColor(color);
    c.drawRRect(SkRRect::MakeRectXY(SkRect::MakeXYWH(x, cy - 3.5f, 9, 7), 2, 2), p);
    ctl::triangle(c, x + 4.5f, cy + 3.5f, 4, 4, ctl::Dir::Left, color);
}

// Beats-mode transient loop mode as Live's icons: a plain arrow (off), an arrow with a return loop (forward),
// two opposed arrows (back-and-forth).
void transientLoopIcon(SkCanvas& c, const Rect& r, int mode, SkColor color)
{
    const float cx = r.x + std::min(r.w, 18.0f) * 0.5f + 2, cy = r.centreY();
    auto arrow = [&](float y, bool right) {
        paint::line(c, cx - 6, y, cx + 6, y, color, 1.2f);
        ctl::triangle(c, right ? cx + 6 : cx - 6, y, 4, 4, right ? ctl::Dir::Right : ctl::Dir::Left, color);
    };
    if (mode == 2) { arrow(cy - 2.5f, true); arrow(cy + 2.5f, false); }
    else if (mode == 1) { arrow(cy - 2, true); paint::line(c, cx - 6, cy + 3, cx + 6, cy + 3, color, 1.2f); paint::line(c, cx - 6, cy - 2, cx - 6, cy + 3, color, 1.2f); }
    else arrow(cy, true);
}
} // namespace

Rect ClipControls::controlBounds(Id id) const
{
    for (const auto& c : controls_)
        if (c.id == id)
            return c.r;
    return {};
}

const ClipControls::Control* ClipControls::controlAt(float x, float y) const
{
    for (const auto& c : controls_)
        if (c.id != Id::Separator && c.r.contains(x, y) && y >= kTitle)
            return &c;
    return nullptr;
}

double ClipControls::gainFaderPosition(double db) { return faderPosition(db); }
double ClipControls::gainFaderDb(double position) { return faderDb(position); }

float ClipControls::columnWidth() const
{
    return std::max(150.0f, ctx().theme.metrics.clipPanelWidth);
}

int ClipControls::columnsFor(float panelHeight)
{
    const auto* selected = ctx().selectedClip();
    if (! selected) return 1;
    layoutControls(*selected, panelHeight);
    return columns_;
}

std::string ClipControls::valueText(Id id, const model::Clip& clip) const
{
    const auto& song = ctx().song;
    const int perBar = song ? std::max(1, song->timeSigNumerator * 4 / std::max(1, song->timeSigDenominator)) : 4;
    const bool beats = clip.kind == model::Clip::Kind::Midi || clip.warped;
    auto position = [&](double v, bool oneBased) { return beats ? audioedit::formatBeats(v, perBar, oneBased) : audioedit::formatSeconds(v); };
    switch (id)
    {
        case Id::Start: return position(clip.loop.loopStart + clip.loop.startRelative, true);
        case Id::End: return position(clip.loop.loopEnd, true);
        case Id::Position: return position(clip.loop.loopStart, true);
        case Id::Length: return position(clip.loop.loopEnd - clip.loop.loopStart, false);
        case Id::Bpm: return editingBpm_ && clip.uid == location_.uid && song == bpmSong_ ? bpmText_ + "|"
                            : number("%.1f", audioedit::clipBpm(clip, song ? song->tempo : 120.0));
        case Id::Envelope: return number("%.0f", clip.transientEnvelope);
        case Id::Grain:
            return number("%.0f", clip.warpMode == model::WarpMode::Texture ? clip.granularityTexture : clip.granularityTones);
        case Id::Flux: return number("%.0f", clip.fluctuationTexture);
        case Id::PitchStBox: return std::to_string(clip.pitchCoarse);
        case Id::PitchCt: return number("%.0f", clip.pitchFine);
        case Id::Formants: return number("%.0f", clip.complexProFormants);
        case Id::ProEnvelope: return number("%.0f", clip.complexProEnvelope);
        case Id::SigNum: return std::to_string(clip.timeSigNumerator);
        case Id::SigDen: return std::to_string(clip.timeSigDenominator);
        case Id::LaunchVelocity: return number("%.0f %%", clip.velocityAmount * 100.0);
        default: return {};
    }
}

void ClipControls::layoutControls(const model::Clip& clip, float panelHeight)
{
    controls_.clear();
    sections_.clear();
    const float cw = columnWidth();
    const float available = std::max(60.0f, (panelHeight >= 0 ? panelHeight : height()) - kTitle);
    const float colB = cw * 0.5f + 2.0f, half = cw * 0.5f - kPad - 2.0f, full = cw - 2 * kPad;
    const bool audio = clip.kind == model::Clip::Kind::Audio;

    // Each box is laid out in its own coordinates, then placed into a column.
    struct Pending { Box box; Id header; bool open; std::vector<Control> controls; float h = 0; };
    std::vector<Pending> boxes;
    float y = 0;
    auto begin = [&](Box box, Id header, bool open) {
        boxes.push_back({box, header, open, {}, 0});
        y = 0;
        if (header != Id::Separator) { boxes.back().controls.push_back({header, {0, 0, cw, kHeader}}); y = kHeader; }
    };
    auto add = [&](Id id, Rect r) { boxes.back().controls.push_back({id, r}); };
    auto separator = [&](float x, float at, float w) { add(Id::Separator, {x, at, w, 1}); };
    auto end = [&](float bottomPad) { boxes.back().h = y + bottomPad; };

    // ---- Clip box: Start/End, Loop, Position/Length, Signature/Groove, Scale (no header, like Live 12)
    begin(Box::Clip, Id::Separator, true);
    y = 4;
    add(Id::StartSet, {kPad + half - 24, y, 24, 11});
    add(Id::EndSet, {colB + half - 24, y, 24, 11});
    y += kCaption + 2;
    add(Id::Start, {kPad, y, half, kBox});
    add(Id::End, {colB, y, half, kBox});
    y += kBox + 8;
    separator(kPad, y, full);
    y += 5;
    if (! audio) { add(Id::NoteDuplicate, {kPad, y, full, kBox}); y += kBox + 4; }
    add(Id::Loop, {kPad, y, full, kBox});
    y += kBox + 5;
    add(Id::PositionSet, {kPad + half - 24, y, 24, 11});
    add(Id::LengthSet, {colB + half - 24, y, 24, 11});
    y += kCaption + 2;
    add(Id::Position, {kPad, y, half, kBox});
    add(Id::Length, {colB, y, half, kBox});
    y += kBox + 6;
    separator(kPad, y, full);
    y += 5 + kCaption + 2; // Signature / Groove captions
    add(Id::SigNum, {kPad, y, (half - 8) * 0.5f, kBox});
    add(Id::SigDen, {kPad + (half - 8) * 0.5f + 8, y, (half - 8) * 0.5f, kBox});
    add(Id::Groove, {colB, y, half, kBox});
    y += kBox + 6;
    separator(kPad, y, full);
    y += 5 + kCaption + 2; // Scale caption
    add(Id::ScaleIcon, {kPad, y, kBox, kBox});
    add(Id::ScaleRoot, {kPad + kBox + 4, y, 40, kBox});
    add(Id::ScaleMode, {kPad + kBox + 48, y, full - kBox - 48, kBox});
    y += kBox;
    end(8);

    // ---- Audio box: left column of warp settings, right column with gain and pitch
    if (audio)
    {
        begin(Box::Audio, Id::AudioHeader, audioOpen_);
        if (audioOpen_)
        {
            const float lw = std::round(full * 0.555f), pair = (lw - 4) * 0.5f;
            const float rx = kPad + lw + 9, rw = cw - kPad - rx;
            const float top = y + 4;
            y = top;
            add(Id::Warp, {kPad, y, pair, kBox});
            add(Id::Follow, {kPad + pair + 4, y, pair, kBox});
            y += kBox + 5;
            add(Id::WarpMode, {kPad, y, lw, kBox});
            y += kBox + 3;
            // One caption + field row and one field row, whose contents depend on the warp mode.
            const float row1 = y + kCaption + 1, row2 = row1 + kBox + 4;
            switch (clip.warpMode)
            {
                case model::WarpMode::Beats:
                    add(Id::Preserve, {kPad, row1, lw, kBox});
                    add(Id::LoopMode, {kPad, row2, pair, kBox});
                    add(Id::Envelope, {kPad + pair + 4, row2, pair, kBox});
                    break;
                case model::WarpMode::Tones: add(Id::Grain, {kPad, row1, pair, kBox}); break;
                case model::WarpMode::Texture:
                    add(Id::Grain, {kPad, row1, pair, kBox});
                    add(Id::Flux, {kPad + pair + 4, row1, pair, kBox});
                    break;
                case model::WarpMode::ComplexPro:
                    add(Id::Formants, {kPad, row1, pair, kBox});
                    add(Id::ProEnvelope, {kPad + pair + 4, row1, pair, kBox});
                    break;
                case model::WarpMode::RePitch:
                case model::WarpMode::Complex: break;
            }
            y = row2 + kBox + 7 + kCaption + 1; // BPM caption
            const float bpmW = std::round(lw * 0.47f), small = (lw - bpmW - 8) * 0.5f;
            add(Id::Bpm, {kPad, y, bpmW, kBox});
            add(Id::Half, {kPad + bpmW + 4, y, small, kBox});
            add(Id::Double, {kPad + bpmW + 8 + small, y, small, kBox});
            y += kBox + 7;
            separator(kPad, y, lw);
            y += 5;
            const float reverseW = std::round(lw * 0.58f);
            add(Id::Reverse, {kPad, y, reverseW, kBox});
            add(Id::Stems, {kPad + reverseW + 4, y, lw - reverseW - 4, kBox});
            y += kBox + 5;
            separator(kPad, y, lw);
            y += 5;
            const float third = (lw - 6) / 3.0f;
            add(Id::Fade, {kPad, y, third, kBox});
            add(Id::Ram, {kPad + third + 3, y, third, kBox});
            add(Id::HiQ, {kPad + 2 * (third + 3), y, third, kBox});
            const float leftBottom = y + kBox;

            // Right column: Gain caption, fader, dB readout; Pitch caption, knob, semitone / cent boxes.
            float ry = top + kCaption + 3;
            const float boxesY = leftBottom - kBox;
            const float knob = 26;
            const float knobY = boxesY - knob - 3;
            const float faderBottom = knobY - kCaption - 2 - 13 - 4; // dB readout and the Pitch caption sit between

            add(Id::Gain, {rx, ry, rw, std::max(30.0f, faderBottom - ry)});
            add(Id::PitchSt, {rx + (rw - knob) * 0.5f, knobY, knob, knob});
            const float boxW = (rw - 3) * 0.5f;
            add(Id::PitchStBox, {rx, boxesY, boxW, kBox});
            add(Id::PitchCt, {rx + boxW + 3, boxesY, boxW, kBox});
            add(Id::Separator, {kPad + lw + 4, top - 2, 1, leftBottom - top + 4});
            y = leftBottom;
            end(8);
        }
        else end(0);
    }

    // ---- MIDI boxes
    auto launch = [&] {
        begin(Box::Launch, Id::LaunchHeader, launchOpen_);
        if (launchOpen_)
        {
            y += 4 + kCaption + 1;
            add(Id::LaunchMode, {kPad, y, half, kBox});
            add(Id::LaunchQuant, {colB, y, half, kBox});
            y += kBox + 5 + kCaption + 1;
            add(Id::LaunchLegato, {kPad, y, half, kBox});
            add(Id::LaunchVelocity, {colB, y, half, kBox});
            y += kBox;
            end(8);
        }
        else end(0);
    };
    if (! audio)
    {
        launch();
        begin(Box::Notes, Id::NotesHeader, notesOpen_);
        if (notesOpen_)
        {
            y += 5;
            const std::pair<Id, Id> rows[] = {{Id::NoteRev, Id::NoteInv}, {Id::FitScale, Id::NoteLegato}, {Id::NoteDouble, Id::NoteHalf}, {Id::Humanize, Id::NoteDuplicate}};
            for (size_t row = 0; row < 4; ++row)
            {
                add(rows[row].first, {kPad, y, half, kBox});
                // Duplicate lives in Clip, above Loop; leave the final row's right side clear.
                if (row < 3) add(rows[row].second, {colB, y, half, kBox});
                y += kBox + 4;
            }
            end(4);
        }
        else end(0);
        auto tools = [&](Box box, Id header, bool open, Id type) {
            begin(box, header, open);
            if (! open) { end(0); return; }
            y += 5;
            add(type, {kPad, y, full, kBox}); y += kBox + 4 + kCaption + 1;
            using Tool = midiedit::Tool;
            const auto tool = type == Id::GenerateType ? generator_.tool : transform_.tool;
            const bool generator = midiedit::isGenerator(tool);
            std::vector<Id> fields;
            if (generator || tool == Tool::Arpeggiate || tool == Tool::Quantize || tool == Tool::Ornament || tool == Tool::Span || tool == Tool::Strum || tool == Tool::Humanize) fields.push_back(Id::ToolRate);
            if (tool == Tool::Quantize || tool == Tool::Strum || tool == Tool::TimeWarp || tool == Tool::Humanize) fields.push_back(Id::ToolAmount);
            if (generator || tool == Tool::Arpeggiate || tool == Tool::Chop || tool == Tool::Span) fields.push_back(Id::ToolGate);
            if (tool == Tool::Chop || tool == Tool::Connect || tool == Tool::Recombine || tool == Tool::Rhythm || tool == Tool::Stacks) fields.push_back(Id::ToolParts);
            if (generator && tool != Tool::Stacks) fields.push_back(Id::ToolDensity);
            if (generator) fields.push_back(Id::ToolLow);
            if (generator && tool != Tool::Rhythm) fields.push_back(Id::ToolHigh);
            for (size_t i = 0; i < fields.size(); ++i)
            {
                add(fields[i], {i % 2 == 0 ? kPad : colB, y, half, kBox});
                if (i % 2 == 1 || i + 1 == fields.size()) y += kBox + 4 + kCaption + 1;
            }
            y -= kCaption + 1;
            add(Id::ToolApply, {kPad, y, half, kBox}); add(Id::ToolReset, {colB, y, half, kBox}); y += kBox;
            end(8);
        };
        tools(Box::Transform, Id::TransformHeader, transformOpen_, Id::TransformType);
        tools(Box::Generate, Id::GenerateHeader, generateOpen_, Id::GenerateType);
    }
    else
        launch();

    // ---- Flow the boxes into columns (audio and MIDI, like Live 12): a box that does not fit below the previous one
    // starts the next column.
    controls_.push_back({Id::Activator, {0, 0, std::max(width(), cw), kTitle}});
    // A collapsed box (just its header) joins the shortest column when it fits there; otherwise it starts the next one.
    std::vector<float> used{0.0f};
    for (auto& box : boxes)
    {
        size_t column = used.size() - 1;
        if (used[column] > 0 && used[column] + box.h > available)
        {
            const auto shortest = (size_t) (std::min_element(used.begin(), used.end()) - used.begin());
            if (box.h <= kHeader && used[shortest] + box.h <= available) column = shortest;
            else { used.push_back(0.0f); column = used.size() - 1; }
        }
        const float ox = (float) column * cw, oy = kTitle + used[column] - scrollY_;
        sections_.push_back({box.box, {ox, oy, cw, box.h}, box.header, box.open});
        for (auto control : box.controls)
        {
            control.r.x += ox;
            control.r.y += oy;
            controls_.push_back(control);
        }
        used[column] += box.h;
    }
    columns_ = (int) used.size();
    contentHeight_ = *std::max_element(used.begin(), used.end());
}

void ClipControls::paint(SkCanvas& c, const Theme& t)
{
    paint::fill(c, localBounds(), t.color("SurfaceBackground"));
    const auto* selected = ctx().selectedClip();
    if (! selected)
        return;
    const auto& clip = dragging_ && selected->uid == before_.uid ? working_ : *selected;
    const float maxScroll = std::max(0.0f, contentHeight_ - (height() - kTitle));
    scrollY_ = std::clamp(scrollY_, 0.0f, maxScroll);
    layoutControls(clip);

    const bool audioClip = clip.kind == model::Clip::Kind::Audio;
    const SkColor fg = t.color("ControlForeground");
    const SkColor line = t.color("SurfaceArea");
    const SkColor accent = t.color("ChosenDefault");
    const float w = width();
    const float fontSize = t.metrics.fontSize;

    c.save();
    c.clipRect(Rect{0, kTitle, w, height() - kTitle}.sk());
    auto rect = [&](Id id) { return controlBounds(id); };
    auto caption = [&](const char* text, float x, float y, float cw, Align align = Align::Left) {
        paint::text(c, text, {x, y, cw, kCaption}, fg, fontSize, align);
    };
    auto captionAbove = [&](Id id, const char* text, Align align = Align::Left, float minWidth = 0) {
        const Rect r = rect(id);
        if (! r.isEmpty()) caption(text, r.x, r.y - kCaption - 1, std::max(r.w, minWidth), align);
    };
    auto box = [&](Id id, Align align = Align::Right, bool enabled = true) {
        const Rect r = rect(id);
        if (! r.isEmpty()) ctl::valueBox(c, t, r, valueText(id, clip), align, enabled);
    };

    for (const auto& section : sections_)
    {
        if (section.header == Id::Separator) continue;
        const Rect r{section.r.x, section.r.y, section.r.w, kHeader};
        paint::fill(c, r, t.color("SurfaceHighlight"));
        paint::line(c, r.x, r.bottom() - 0.5f, r.right(), r.bottom() - 0.5f, line);
        if (section.r.y > kTitle - scrollY_ + 0.5f) paint::line(c, r.x, r.y + 0.5f, r.right(), r.y + 0.5f, line);
        const char* label = section.box == Box::Audio ? "Audio" : section.box == Box::Launch ? "Launch" : section.box == Box::Notes ? "Pitch & Time"
                            : section.box == Box::Transform ? "Transform" : "Generate";
        float textX = r.x + kPad;
        if (section.box == Box::Audio)
        {
            // Waveform glyph: five bars of different heights.
            const float heights[] = {4, 8, 11, 7, 5};
            for (int i = 0; i < 5; ++i)
                paint::fill(c, {r.x + kPad + (float) i * 2.5f, r.centreY() - heights[i] * 0.5f, 1.2f, heights[i]}, fg);
            textX += 17;
        }
        paint::text(c, label, {textX, r.y, r.w - 40, r.h}, fg, fontSize + 0.5f, Align::Left);
        ctl::triangle(c, r.right() - kPad - 4, r.centreY(), 8, 6, section.open ? ctl::Dir::Down : ctl::Dir::Left, fg);
    }
    for (const auto& control : controls_)
        if (control.id == Id::Separator) paint::fill(c, control.r, line);
    for (int column = 1; column < columns_; ++column)
        paint::fill(c, {(float) column * columnWidth() - 1, kTitle, 1, height() - kTitle}, line);

    // ---- Clip box
    {
        const bool canSet = ctx().transport != nullptr && (! audioClip || clip.warped);
        captionAbove(Id::Start, "Start");
        captionAbove(Id::End, "End");
        captionAbove(Id::Position, "Position");
        captionAbove(Id::Length, "Length");
        for (auto id : {Id::StartSet, Id::EndSet, Id::PositionSet, Id::LengthSet})
            pillButton(c, t, rect(id), "Set", canSet);
        for (auto id : {Id::Start, Id::End, Id::Position, Id::Length})
            positionField(c, t, rect(id), valueText(id, clip));
        if (! audioClip) ctl::toggle(c, t, rect(Id::NoteDuplicate), "Duplicate", false, 0);
        const Rect loop = rect(Id::Loop);
        ctl::toggle(c, t, loop, "", clip.loop.enabled, accent);
        loopIcon(c, loop.centreX() - 20, loop.centreY(), clip.loop.enabled ? t.color("ControlOnForeground") : t.color("ControlOffForeground"));
        paint::text(c, "Loop", {loop.centreX() - 7, loop.y, 40, loop.h}, clip.loop.enabled ? t.color("ControlOnForeground") : t.color("ControlOffForeground"),
                    fontSize, Align::Left);
        captionAbove(Id::SigNum, "Signature", Align::Left, 80);
        captionAbove(Id::Groove, "Groove");
        box(Id::SigNum, Align::Centre);
        const Rect sigNum = rect(Id::SigNum), sigDen = rect(Id::SigDen);
        paint::text(c, "/", {sigNum.right(), sigNum.y, sigDen.x - sigNum.right(), kBox}, fg, fontSize, Align::Centre);
        box(Id::SigDen, Align::Centre);
        ctl::dropdown(c, t, rect(Id::Groove), "None", false);
        const Rect icon = rect(Id::ScaleIcon);
        captionAbove(Id::ScaleIcon, "Scale", Align::Left, 80);
        paint::fill(c, icon, ctx().scaleHighlight ? SkColorSetRGB(0xb8, 0x9a, 0xf8) : t.color("ControlTextBack"));
        paint::stroke(c, icon, t.color("ControlContrastFrame"));
        paint::text(c, "\xE2\x99\xAD#", icon, fg, t.metrics.smallFontSize, Align::Centre, true);
        static const char* roots[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
        ctl::dropdown(c, t, rect(Id::ScaleRoot), roots[((clip.scaleRoot % 12) + 12) % 12]); // the clip's own scale
        ctl::dropdown(c, t, rect(Id::ScaleMode), std::string(model::scaleAt(clip.scaleName).name));
    }

    if (! audioClip && notesOpen_)
    {
        ctl::toggle(c, t, rect(Id::NoteRev), "Reverse", false, 0);
        ctl::toggle(c, t, rect(Id::NoteInv), "Invert", false, 0);
        ctl::toggle(c, t, rect(Id::NoteLegato), "Legato", false, 0);
        ctl::toggle(c, t, rect(Id::NoteDouble), "\xC3\x97" "2", false, 0);
        ctl::toggle(c, t, rect(Id::NoteHalf), "/2", false, 0);
        ctl::toggle(c, t, rect(Id::FitScale), "Fit to Scale", false, 0);
        ctl::toggle(c, t, rect(Id::Humanize), "Humanize", false, 0);
    }

    // ---- Audio box
    if (audioClip && audioOpen_)
    {
        ctl::toggle(c, t, rect(Id::Warp), "Warp", clip.warped, accent);
        ctl::toggle(c, t, rect(Id::Follow), "Follow", ctx().sampleFollow, accent);
        ctl::dropdown(c, t, rect(Id::WarpMode), warpName(clip.warpMode));
        switch (clip.warpMode)
        {
            case model::WarpMode::Beats:
            {
                captionAbove(Id::Preserve, "Preserve");
                ctl::dropdown(c, t, rect(Id::Preserve), preserveName(clip.transientResolution));
                const Rect lm = rect(Id::LoopMode);
                ctl::dropdown(c, t, lm, "");
                transientLoopIcon(c, {lm.x + 3, lm.y, lm.w - 14, lm.h}, clip.transientLoopMode, fg);
                sliderBox(c, t, rect(Id::Envelope), valueText(Id::Envelope, clip), clip.transientEnvelope / 100.0);
                break;
            }
            case model::WarpMode::Tones:
                captionAbove(Id::Grain, "Grain Size");
                box(Id::Grain);
                break;
            case model::WarpMode::Texture:
                captionAbove(Id::Grain, "Grain Size");
                captionAbove(Id::Flux, "Flux");
                box(Id::Grain);
                box(Id::Flux);
                break;
            case model::WarpMode::ComplexPro:
                captionAbove(Id::Formants, "Formants");
                captionAbove(Id::ProEnvelope, "Envelope");
                box(Id::Formants);
                box(Id::ProEnvelope);
                break;
            case model::WarpMode::RePitch:
            case model::WarpMode::Complex: break;
        }
        captionAbove(Id::Bpm, "BPM");
        box(Id::Bpm, Align::Right, clip.warped);
        pillButton(c, t, rect(Id::Half), "/2", clip.warped);
        pillButton(c, t, rect(Id::Double), "\xC3\x97" "2", clip.warped);
        pillButton(c, t, rect(Id::Reverse), "Reverse", ctx().reverseClip != nullptr);
        pillButton(c, t, rect(Id::Stems), ctx().stemsRunning ? "Cancel" : "Stems", ctx().separateStems != nullptr, ctx().stemsRunning);
        // Fade applies to Session clips; Arrangement clips use their fade handles instead (greyed like Live).
        const bool sessionClip = ctx().selectedClipScene >= 0;
        ctl::toggle(c, t, rect(Id::Fade), "Fade", clip.clipFade && sessionClip, accent, sessionClip);
        ctl::toggle(c, t, rect(Id::Ram), "RAM", clip.ram, accent);
        ctl::toggle(c, t, rect(Id::HiQ), "HiQ", clip.hiQ, accent);

        // Gain fader: Live's taper (0 dB about 60 % down), minor ticks left of the track, 24 / 0 / 70 to its right.
        const Rect g = rect(Id::Gain);
        caption("Gain", g.x, g.y - kCaption - 2, g.w, Align::Centre);
        const float trackX = std::round(g.centreX()) + 0.5f;
        const float top = g.y + 4, bottom = g.bottom() - 4;
        auto yForDb = [&](double db) { return (float) (bottom - faderPosition(db) * (bottom - top)); };
        paint::line(c, trackX, top, trackX, bottom, fg, 1.5f);
        for (int db : {24, 18, 12, 6, 0, -6, -12, -24, -48, -70})
        {
            const float ty = std::round(yForDb((double) db)) + 0.5f;
            const bool labelled = db == 24 || db == 0 || db == -70;
            paint::line(c, trackX - (labelled ? 5.0f : 4.0f), ty, trackX, ty, fg);
            if (labelled)
                paint::text(c, db == -70 ? "70" : std::to_string(db), {trackX + 4, ty - 6, 24, 12}, fg, fontSize, Align::Left);
        }
        ctl::triangle(c, trackX - 9, yForDb(gainDb(clip.gain)), 11, 11, ctl::Dir::Right, fg);
        paint::text(c, number("%.1f dB", gainDb(clip.gain)), {g.x - 6, g.bottom() + 1, g.w + 12, 12}, fg, fontSize, Align::Centre);

        // Pitch: ring with its gap at the bottom ("st" sits in the gap), needle at the semitone value.
        const Rect k = rect(Id::PitchSt);
        caption("Pitch", k.x - 20, k.y - kCaption - 1, k.w + 40, Align::Centre);
        SkPaint ring;
        ring.setAntiAlias(true);
        ring.setStyle(SkPaint::kStroke_Style);
        ring.setStrokeWidth(2.6f);
        ring.setColor(t.color("Poti"));
        const SkRect oval = SkRect::MakeXYWH(k.x + 2, k.y + 2, k.w - 4, k.h - 4);
        c.drawArc(oval, 135, 270, false, ring);
        const float angle = (float) ((135.0 + 270.0 * (clip.pitchCoarse + 48) / 96.0) * M_PI / 180.0);
        const float cx = k.centreX(), cy = k.centreY(), rad = (k.w - 4) * 0.5f;
        ring.setStrokeCap(SkPaint::kRound_Cap);
        c.drawLine(cx, cy, cx + std::cos(angle) * rad, cy + std::sin(angle) * rad, ring);
        paint::text(c, "st", {k.x, k.bottom() - 8, k.w, 10}, fg, fontSize, Align::Centre);
        box(Id::PitchStBox, Align::Centre);
        box(Id::PitchCt, Align::Centre);
    }

    if (launchOpen_)
    {
        captionAbove(Id::LaunchMode, "Mode");
        captionAbove(Id::LaunchQuant, "Quantization");
        captionAbove(Id::LaunchVelocity, "Velocity");
        ctl::dropdown(c, t, rect(Id::LaunchMode), launchModeName(clip.launchMode));
        ctl::dropdown(c, t, rect(Id::LaunchQuant), launchQuantName(clip.launchQuantisation));
        ctl::toggle(c, t, rect(Id::LaunchLegato), "Legato", clip.legato, accent);
        box(Id::LaunchVelocity);
    }
    if (! audioClip && (transformOpen_ || generateOpen_))
    {
        const auto& tool = generateOpen_ ? generator_ : transform_;
        ctl::dropdown(c, t, rect(generateOpen_ ? Id::GenerateType : Id::TransformType), midiedit::toolName(tool.tool));
        auto field = [&](Id id, const char* label, const std::string& text) {
            const auto r = rect(id); if (r.isEmpty()) return;
            caption(label, r.x, r.y - kCaption - 1, r.w); ctl::valueBox(c, t, r, text, Align::Centre);
        };
        field(Id::ToolRate, "Rate (beats)", number("%.3g", tool.rate));
        field(Id::ToolAmount, "Amount", number("%.0f %%", tool.amount * 100));
        field(Id::ToolGate, "Gate", number("%.0f %%", tool.gate * 100));
        field(Id::ToolParts, tool.tool == midiedit::Tool::Recombine ? "Rotate Steps" : "Parts / Steps", std::to_string(tool.parts));
        field(Id::ToolDensity, "Density", number("%.0f %%", tool.density * 100));
        field(Id::ToolLow, "Min Pitch", std::to_string(tool.lowPitch));
        field(Id::ToolHigh, "Max Pitch", std::to_string(tool.highPitch));
        ctl::toggle(c, t, rect(Id::ToolApply), "Apply", false, 0);
        ctl::toggle(c, t, rect(Id::ToolReset), "Reset", false, 0);
    }
    c.restore();

    // Title bar: clip colour, activator square (lit while the clip is active) and name, above the scrolling content.
    const SkColor clipColour = Theme::paletteColor(clip.colorIndex);
    paint::fill(c, {0, 0, w, kTitle}, clipColour);
    const SkColor ink = paint::contrastText(clipColour);
    const Rect activator{6, 4, 10, 10};
    paint::fill(c, activator, clip.disabled ? t.color("SurfaceBackground") : accent);
    paint::stroke(c, activator, t.color("ControlContrastFrame"));
    paint::text(c, clip.name.empty() ? "Clip" : clip.name, Rect{22, 0, w - 26, kTitle}, ink, fontSize + 0.5f, Align::Left);
    paint::fill(c, {w - 1, 0, 1, height()}, line);
}

void ClipControls::commit(model::Clip changed, const char* label)
{
    auto& x = ctx();
    if (x.song && x.applyEdit && model::findClip(*x.song, location_))
        x.applyEdit(model::modifyClip(*x.song, location_, std::move(changed), label));
}

model::Clip ClipControls::dragged(Id id, const model::Clip& base, float dy, bool fine) const
{
    auto c = base;
    const double tempo = ctx().song ? ctx().song->tempo : 120.0;
    const bool beats = base.kind == model::Clip::Kind::Midi || base.warped;
    const double unit = beats ? (fine ? 0.0625 : 0.25) : (fine ? 0.001 : 0.01);
    const double steps = std::round(dy / 4.0);
    const double delta = steps * unit;
    const auto& L = base.loop;
    switch (id)
    {
        case Id::Start:
            c.loop.startRelative = std::clamp(L.loopStart + L.startRelative + delta, 0.0, std::max(0.0, L.loopEnd - unit)) - L.loopStart;
            c.resetSourceMarkers = true;
            break;
        case Id::End:
            c.loop.loopEnd = std::max(std::max(L.loopStart, L.loopStart + L.startRelative) + unit, L.loopEnd + delta);
            c.resetSourceMarkers = true;
            break;
        case Id::Position:
        {
            const double start = L.loopStart + L.startRelative;
            c.loop.loopStart = std::clamp(L.loopStart + delta, 0.0, std::max(0.0, L.loopEnd - unit));
            c.loop.startRelative = start - c.loop.loopStart;
            c.resetSourceMarkers = true;
            break;
        }
        case Id::Length:
            c.loop.loopEnd = std::max(L.loopStart + unit, L.loopEnd + delta);
            c.resetSourceMarkers = true;
            break;
        case Id::Bpm:
        {
            const double bpm = audioedit::clipBpm(base, tempo);
            const double target = std::clamp(std::round((bpm + std::round(dy / (fine ? 6 : 2)) * 0.1) * 10) / 10, 20.0, 999.0);
            return audioedit::scaleTempo(base, target / bpm);
        }
        case Id::Gain:
        {
            // Along the fader's own taper, so the triangle follows the mouse (Shift: a tenth of the speed).
            const float travel = std::max(20.0f, controlBounds(Id::Gain).h - 8);
            const double db = faderDb(faderPosition(gainDb(base.gain)) + dy / travel * (fine ? 0.1 : 1.0));
            c.gain = db <= kMinGainDb ? 0.0 : std::pow(10.0, db / 20.0);
            break;
        }
        case Id::PitchSt:
        case Id::PitchStBox: c.pitchCoarse = std::clamp(base.pitchCoarse + (int) std::lround(dy / 6.0), -48, 48); break;
        case Id::PitchCt: c.pitchFine = std::clamp(base.pitchFine + std::round(dy / 2.0), -50.0, 50.0); break;
        case Id::Envelope: c.transientEnvelope = std::clamp(base.transientEnvelope + std::round(dy / 2.0), 0.0, 100.0); break;
        case Id::Grain:
            if (base.warpMode == model::WarpMode::Texture)
                c.granularityTexture = std::clamp(base.granularityTexture + std::round(dy / 2.0), 2.0, 263.0);
            else
                c.granularityTones = std::clamp(base.granularityTones + std::round(dy / 2.0), 10.0, 100.0);
            break;
        case Id::Flux: c.fluctuationTexture = std::clamp(base.fluctuationTexture + std::round(dy / 2.0), 0.0, 100.0); break;
        case Id::Formants: c.complexProFormants = std::clamp(base.complexProFormants + std::round(dy / 2.0), 0.0, 100.0); break;
        case Id::ProEnvelope: c.complexProEnvelope = std::clamp(base.complexProEnvelope + std::round(dy / 2.0), 8.0, 256.0); break;
        case Id::SigNum: c.timeSigNumerator = std::clamp(base.timeSigNumerator + (int) std::lround(dy / 6.0), 1, 99); break;
        case Id::LaunchVelocity: c.velocityAmount = std::clamp(base.velocityAmount + std::round(dy / 2.0) * (fine ? 0.001 : 0.01), 0.0, 1.0); break;
        default: break;
    }
    return c;
}

void ClipControls::showMenu(Id id, const model::Clip& clip)
{
    juce::PopupMenu menu;
    const auto location = ctx().selectedClipLocation();
    std::vector<std::function<void()>> actions;
    auto item = [&](const char* label, bool ticked, std::function<void()> action) {
        actions.push_back(std::move(action));
        menu.addItem((int) actions.size(), label, true, ticked);
    };
    const auto base = clip;
    auto change = [this, base, location](std::function<void(model::Clip&)> edit, const char* label) {
        return [this, base, location, edit, label] {
            if (! Widget::isAlive(this)) return;
            location_ = location;
            auto c = base;
            edit(c);
            commit(std::move(c), label);
        };
    };
    switch (id)
    {
        case Id::TransformType:
        case Id::GenerateType:
        {
            const bool generator = id == Id::GenerateType;
            const int begin = generator ? (int) midiedit::Tool::Rhythm : 0;
            const int end = generator ? (int) midiedit::Tool::Stacks : (int) midiedit::Tool::Legato;
            for (int index = begin; index <= end; ++index)
            {
                const auto tool = (midiedit::Tool) index;
                item(midiedit::toolName(tool), (generator ? generator_ : transform_).tool == tool, [this, generator, tool] {
                    if (! Widget::isAlive(this)) return;
                    (generator ? generator_ : transform_).tool = tool; repaint();
                });
            }
            break;
        }
        case Id::WarpMode:
            for (auto mode : {model::WarpMode::Beats, model::WarpMode::Tones, model::WarpMode::Texture, model::WarpMode::RePitch,
                              model::WarpMode::Complex, model::WarpMode::ComplexPro})
                item(warpName(mode), base.warpMode == mode, change([mode](model::Clip& c) { c.warpMode = mode; }, "Change Warp Mode"));
            break;
        case Id::Preserve:
            for (int v : kPreserveOrder)
                item(preserveName(v), base.transientResolution == v, change([v](model::Clip& c) { c.transientResolution = v; }, "Change Transient Preservation"));
            break;
        case Id::LoopMode:
            for (int v = 0; v < 3; ++v)
                item(loopModeName(v), base.transientLoopMode == v, change([v](model::Clip& c) { c.transientLoopMode = v; }, "Change Transient Loop Mode"));
            break;
        case Id::SigDen:
            for (int v : {1, 2, 4, 8, 16})
                item(std::to_string(v).c_str(), base.timeSigDenominator == v, change([v](model::Clip& c) { c.timeSigDenominator = v; }, "Change Clip Signature"));
            break;
        case Id::LaunchMode:
            for (int v = 0; v < 4; ++v)
                item(launchModeName(v), base.launchMode == v, change([v](model::Clip& c) { c.launchMode = v; }, "Change Launch Mode"));
            break;
        case Id::LaunchQuant:
            for (int v = 0; v < 15; ++v)
                item(launchQuantName(v), base.launchQuantisation == v, change([v](model::Clip& c) { c.launchQuantisation = v; }, "Change Launch Quantization"));
            break;
        case Id::ScaleRoot:
        {
            static const char* roots[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
            for (int v = 0; v < 12; ++v)
                item(roots[v], ((base.scaleRoot % 12) + 12) % 12 == v, change([v](model::Clip& c) { c.scaleRoot = v; }, "Change Clip Scale"));
            break;
        }
        case Id::ScaleMode:
            for (int v = 0; v < (int) model::kScales.size(); ++v)
                item(std::string(model::kScales[(size_t) v].name).c_str(), base.scaleName == v,
                     change([v](model::Clip& c) { c.scaleName = v; }, "Change Clip Scale"));
            break;
        default: return;
    }
    menu.showMenuAsync(juce::PopupMenu::Options(), [callbacks = std::move(actions)](int result) {
        if (result >= 1 && (size_t) result <= callbacks.size())
            callbacks[(size_t) result - 1]();
    });
}

bool ClipControls::keyPressed(const KeyEvent& key)
{
    if (editingBpm_)
    {
        if (ctx().song != bpmSong_ || ctx().selectedClipLocation().uid != location_.uid) { editingBpm_ = false; return false; }
        if (key.keyCode == juce::KeyPress::escapeKey) editingBpm_ = false;
        else if (key.keyCode == juce::KeyPress::returnKey) { finishBpmEdit(); return true; }
        else if (key.cmd && (key.textChar == 'a' || key.textChar == 'A')) bpmSelected_ = true;
        else if (key.keyCode == juce::KeyPress::backspaceKey || key.keyCode == juce::KeyPress::deleteKey)
        { if (bpmSelected_) bpmText_.clear(); else paint::popLastCodePoint(bpmText_); bpmSelected_ = false; }
        else if (! key.cmd && ! key.alt && ((key.textChar >= '0' && key.textChar <= '9') || key.textChar == '.' || key.textChar == ','))
        { if (bpmSelected_) bpmText_.clear(); bpmSelected_ = false; if (bpmText_.size() < 8) bpmText_ += key.textChar == ',' ? '.' : (char) key.textChar; }
        repaint(); return true;
    }
    if (key.cmd && ! key.shift && ! key.alt && key.keyCode == juce::KeyPress::returnKey && (transformOpen_ || generateOpen_))
    { applyTool(generateOpen_); return true; }
    return false;
}

void ClipControls::finishBpmEdit()
{
    editingBpm_ = false;
    char* end = nullptr;
    const double entered = std::strtod(bpmText_.c_str(), &end);
    const auto* clip = ctx().song == bpmSong_ ? model::findClip(*bpmSong_, location_) : nullptr;
    if (clip && clip->warped && end != bpmText_.c_str() && *end == '\0' && std::isfinite(entered) && entered >= 20 && entered <= 999)
    {
        const double bpm = std::round(entered * 10) / 10;
        if (! clip->tempoAnalyzed || std::abs(bpm - audioedit::clipBpm(*clip, ctx().song->tempo)) > 1e-8)
            commit(audioedit::scaleTempo(*clip, bpm / audioedit::clipBpm(*clip, ctx().song->tempo)), "Change Clip BPM");
    }
    repaint();
}

void ClipControls::applyTool(bool generate)
{
    if (! parent()) return;
    auto settings = generate ? generator_ : transform_; settings.seed = toolSeed_++;
    for (const auto& child : parent()->children()) if (auto* roll = dynamic_cast<PianoRoll*>(child.get()))
    { roll->runMidiTool(settings); break; }
}

void ClipControls::mouseDown(const MouseEvent& e)
{
    if (e.right) return;
    dragging_ = toolDragging_ = false;
    dragChanged_ = false;
    const auto* selected = ctx().selectedClip();
    if (! selected || ! ctx().applyEdit)
        return;
    layoutControls(*selected);
    const auto* hit = controlAt(e.x, e.y);
    if (e.y < kTitle)
        hit = &controls_.front(); // activator / title
    if (! hit)
        return;
    editingBpm_ = false;
    location_ = ctx().selectedClipLocation();
    const auto& clip = *selected;
    auto& song = *ctx().song;
    switch (hit->id)
    {
        case Id::Activator:
            if (e.x < 20)
            {
                auto c = clip;
                c.disabled = ! c.disabled;
                commit(std::move(c), clip.disabled ? "Activate Clip" : "Deactivate Clip");
            }
            break;
        case Id::AudioHeader: audioOpen_ = ! audioOpen_; repaint(); break;
        case Id::NotesHeader: notesOpen_ = ! notesOpen_; if (notesOpen_) transformOpen_ = generateOpen_ = false; repaint(); break;
        case Id::TransformHeader: transformOpen_ = ! transformOpen_; if (transformOpen_) { generateOpen_ = notesOpen_ = false; } repaint(); break;
        case Id::GenerateHeader: generateOpen_ = ! generateOpen_; if (generateOpen_) { transformOpen_ = notesOpen_ = false; } repaint(); break;
        case Id::ToolApply: applyTool(generateOpen_); break;
        case Id::ToolReset: if (generateOpen_) generator_ = {.tool = generator_.tool}; else transform_ = {.tool = transform_.tool}; repaint(); break;
        case Id::TransformType: case Id::GenerateType: showMenu(hit->id, clip); break;
        case Id::ToolRate: case Id::ToolAmount: case Id::ToolGate: case Id::ToolParts: case Id::ToolLow: case Id::ToolHigh: case Id::ToolDensity:
            toolDragging_ = true; toolBefore_ = generateOpen_ ? generator_ : transform_;
            dragId_ = hit->id; grabY_ = e.y; break;
        case Id::FitScale: { auto saved = transform_; transform_.tool = midiedit::Tool::FitScale; applyTool(false); transform_ = saved; break; }
        case Id::Humanize: { auto saved = transform_; transform_.tool = midiedit::Tool::Humanize; transform_.amount = 0.1; applyTool(false); transform_ = saved; break; }
        case Id::LaunchHeader: launchOpen_ = ! launchOpen_; repaint(); break;
        case Id::LaunchLegato:
        {
            auto c = clip;
            c.legato = ! c.legato;
            commit(std::move(c), "Toggle Launch Legato");
            break;
        }
        case Id::StartSet: case Id::EndSet: case Id::PositionSet: case Id::LengthSet:
        {
            if (! ctx().transport || (clip.kind == model::Clip::Kind::Audio && ! clip.warped)) break; // loop units must be beats
            const double position = std::max(0.0, model::clipContentPosition(ctx().transport->positionBeats(), clip.currentStart,
                clip.loop.loopStart, clip.loop.loopEnd, clip.loop.startRelative, clip.loop.enabled));
            auto changed = clip;
            if (hit->id == Id::StartSet) changed.loop.startRelative = std::min(position, clip.loop.loopEnd - 0.0625) - clip.loop.loopStart;
            else if (hit->id == Id::EndSet || hit->id == Id::LengthSet) changed.loop.loopEnd = std::max(clip.loop.loopStart + 0.0625, position);
            else { const double length = clip.loop.loopEnd - clip.loop.loopStart; changed.loop.loopStart = position; changed.loop.loopEnd = position + length; }
            changed.resetSourceMarkers = true; commit(std::move(changed), "Set Clip Marker"); break;
        }
        case Id::NoteRev: { auto saved = transform_; transform_.tool = midiedit::Tool::Reverse; applyTool(false); transform_ = saved; break; }
        case Id::NoteInv: { auto saved = transform_; transform_.tool = midiedit::Tool::Invert; applyTool(false); transform_ = saved; break; }
        case Id::NoteLegato: { auto saved = transform_; transform_.tool = midiedit::Tool::Legato; applyTool(false); transform_ = saved; break; }
        case Id::NoteDuplicate: commit(midiedit::duplicateLoop(clip), "Duplicate Loop"); break;
        case Id::NoteDouble: commit(midiedit::stretch(clip, 2.0), "Stretch Notes x2"); break;
        case Id::NoteHalf: commit(midiedit::stretch(clip, 0.5), "Stretch Notes /2"); break;
        case Id::Loop:
        {
            auto c = clip;
            c.loop.enabled = ! c.loop.enabled;
            commit(std::move(c), "Toggle Loop");
            break;
        }
        case Id::Follow: ctx().sampleFollow = ! ctx().sampleFollow; break;
        case Id::Fade:
            if (ctx().selectedClipScene >= 0) { auto c = clip; c.clipFade = ! c.clipFade; commit(std::move(c), "Toggle Clip Fade"); }
            break;
        case Id::Ram: { auto c = clip; c.ram = ! c.ram; commit(std::move(c), "Toggle Clip RAM Mode"); break; }
        case Id::HiQ: { auto c = clip; c.hiQ = ! c.hiQ; commit(std::move(c), "Toggle Clip High Quality"); break; }
        case Id::Groove: break; // grooves are not implemented: the chooser stays disabled
        case Id::Warp:
            if (clip.kind == model::Clip::Kind::Audio)
            {
                if (ctx().toggleClipWarp) ctx().toggleClipWarp(location_);
                else commit(audioedit::toggleWarp(clip, song.tempo), clip.warped ? "Warp Off" : "Warp On");
            }
            break;
        case Id::Bpm:
            if (! clip.warped) break;
            if (e.clicks >= 2)
            {
                editingBpm_ = bpmSelected_ = true; bpmSong_ = ctx().song;
                bpmText_ = number("%.1f", audioedit::clipBpm(clip, song.tempo));
            }
            else { dragging_ = true; dragId_ = Id::Bpm; grabY_ = e.y; before_ = working_ = clip; }
            break;
        case Id::Half: if (clip.warped) commit(audioedit::scaleTempo(clip, 0.5), "Clip BPM /2"); break;
        case Id::Double: if (clip.warped) commit(audioedit::scaleTempo(clip, 2.0), "Clip BPM x2"); break;
        case Id::Reverse:
            if (ctx().reverseClip) ctx().reverseClip(location_);
            break;
        case Id::Stems: // splits the sample into vocals / drums / bass / other on new tracks (cancels a run in progress)
            if (ctx().separateStems) ctx().separateStems(location_);
            repaint();
            break;
        case Id::ScaleIcon: ctx().scaleHighlight = ! ctx().scaleHighlight; repaint(); break;
        case Id::WarpMode:
        case Id::Preserve:
        case Id::LoopMode:
        case Id::SigDen:
        case Id::LaunchMode:
        case Id::LaunchQuant:
        case Id::ScaleRoot:
        case Id::ScaleMode: showMenu(hit->id, clip); break;
        default:
            if ((hit->id == Id::Gain || hit->id == Id::PitchSt || hit->id == Id::PitchStBox) && e.clicks >= 2)
            {
                auto c = clip;
                if (hit->id == Id::Gain) c.gain = 1.0; else c.pitchCoarse = 0;
                commit(std::move(c), hit->id == Id::Gain ? "Reset Clip Gain" : "Reset Clip Pitch");
                break;
            }
            dragging_ = true;
            dragId_ = hit->id;
            grabY_ = e.y;
            before_ = working_ = clip;
            break;
    }
    repaint();
}

void ClipControls::mouseDrag(const MouseEvent& e)
{
    if (toolDragging_)
    {
        auto& tool = generateOpen_ ? generator_ : transform_;
        const double delta = (grabY_ - e.y) / (e.shift ? 600.0 : 120.0);
        const int steps = (int) std::lround((grabY_ - e.y) / 6.0f);
        switch (dragId_)
        {
            case Id::ToolRate: tool.rate = std::clamp(toolBefore_.rate * std::pow(2.0, steps), 0.03125, 16.0); break;
            case Id::ToolAmount: tool.amount = std::clamp(toolBefore_.amount + delta, 0.0, 1.0); break;
            case Id::ToolGate: tool.gate = std::clamp(toolBefore_.gate + delta, 0.05, 2.0); break;
            case Id::ToolParts: tool.parts = std::clamp(toolBefore_.parts + steps, 1, 64); break;
            case Id::ToolLow: tool.lowPitch = std::clamp(toolBefore_.lowPitch + steps, 0, tool.highPitch); break;
            case Id::ToolHigh: tool.highPitch = std::clamp(toolBefore_.highPitch + steps, tool.lowPitch, 127); break;
            case Id::ToolDensity: tool.density = std::clamp(toolBefore_.density + delta, 0.0, 1.0); break;
            default: break;
        }
        repaint(); return;
    }
    if (! dragging_)
        return;
    working_ = dragged(dragId_, before_, grabY_ - e.y, e.shift);
    dragChanged_ = true;
    repaint();
}

void ClipControls::mouseUp(const MouseEvent&)
{
    if (toolDragging_) { toolDragging_ = false; repaint(); return; }
    if (dragging_ && dragChanged_)
    {
        const char* label = dragId_ == Id::SigNum ? "Change Clip Signature" : dragId_ == Id::LaunchVelocity ? "Change Launch Velocity"
                            : dragId_ == Id::Gain ? "Change Clip Gain" : dragId_ == Id::PitchSt || dragId_ == Id::PitchStBox || dragId_ == Id::PitchCt ? "Change Clip Pitch"
                            : dragId_ == Id::Bpm ? "Change Clip BPM" : "Edit Clip Settings";
        commit(working_, label);
    }
    dragging_ = false;
    dragChanged_ = false;
    repaint();
}

bool ClipControls::mouseWheel(const MouseEvent& e)
{
    if (const auto* clip = ctx().selectedClip(); clip && clip->warped && ctx().song && ctx().applyEdit)
    {
        layoutControls(*clip);
        if (const auto* control = controlAt(e.x, e.y); control && control->id == Id::Bpm && std::abs(e.wheelDeltaY) > 1e-8f)
        {
            editingBpm_ = false; location_ = ctx().selectedClipLocation();
            const double current = audioedit::clipBpm(*clip, ctx().song->tempo);
            const double next = std::clamp((std::round(current * 10) + (e.wheelDeltaY > 0 ? 1 : -1)) / 10, 20.0, 999.0);
            if (std::abs(current - next) > 1e-8) commit(audioedit::scaleTempo(*clip, next / current), "Change Clip BPM");
            return true;
        }
    }
    const float maxScroll = std::max(0.0f, contentHeight_ - (height() - kTitle));
    if (maxScroll <= 0)
        return false;
    scrollY_ = std::clamp(scrollY_ - e.wheelDeltaY * kWheelPixels, 0.0f, maxScroll);
    repaint();
    return true;
}

} // namespace sonora::ui
