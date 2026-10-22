#include "ui/views/ArrangementView.h"
#include "ui/CollabOverlay.h"
#include "core/TimeUtils.h"
#include "core/DeviceCatalog.h"

#include "ui/WheelScale.h"

#include "include/core/SkPaint.h"
#include "include/core/SkPath.h"
#include "ui/GridMenu.h"
#include "ui/TrackMenu.h"

#include "engine/EngineGraph.h"
#include "engine/Transport.h"
#include "ui/Controls.h"
#include "ui/MeterBallistics.h"
#include "ui/Painter.h"
#include "ui/UiContext.h"
#include "ui/Waveform.h"
#include "ui/views/AutomationEditor.h"
#include "ui/views/InputControls.h"
#include "ui/widgets/Buttons.h"
#include "ui/widgets/Fader.h"
#include "ui/widgets/Splitter.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include <juce_gui_basics/juce_gui_basics.h>

namespace sonora::ui
{

namespace
{
// Live's Arrangement track header: a coloured title column, an I/O column, a mixer column
// (activator / solo / arm, volume, pan, peak readouts) and a pair of level meters.
constexpr float kTitleDefault = 103, kHeaderSep = 2, kIoW = 79, kMixW = 106, kMeterW = 14;
constexpr float kShortLane = 40; // lanes shorter than this (returns, main, collapsed) use a single row

class WaveformZoomButton : public TextButton
{
public:
    WaveformZoomButton() : TextButton("") {}
    void paint(SkCanvas& canvas, const Theme& theme) override
    {
        ctl::toggle(canvas, theme, localBounds(), "", ctx().waveformZoomEnabled, theme.color("ChosenDefault"));
        for (int bar = -3; bar <= 3; ++bar)
        {
            const float extent = (float) (bar % 2 == 0 ? 5 : 3);
            paint::line(canvas, width() * 0.5f + (float) bar * 2, height() * 0.5f - extent,
                        width() * 0.5f + (float) bar * 2, height() * 0.5f + extent, theme.color("ControlForeground"));
        }
    }
};

std::string formatPanText(float v)
{
    const int p = (int) std::lround(v * 50.0f);
    if (p == 0)
        return "C";
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%d%c", std::abs(p), p < 0 ? 'L' : 'R');
    return buf;
}

std::string formatDb(float gain)
{
    if (gain <= 0.000317f)
        return "-\xE2\x88\x9E";
    const double db = 20.0 * std::log10(gain);
    if (std::abs(db) < 0.05)
        return "0";
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%.1f", db);
    return buf;
}

class TrackHeader : public Widget
{
public:
    TrackHeader(int controlIndex, bool isMain, bool isReturn, bool isGroup)
        : index_(controlIndex), isMain_(isMain), isReturn_(isReturn), isGroup_(isGroup)
    {
        if (isMain)
            return;
        activator_ = add<TextButton>(isReturn ? "" : std::to_string(controlIndex + 1), TextButton::Style::Activator);
        activator_->midiTarget = midi::ControlTarget{midi::ControlTarget::Kind::TrackActivator, controlIndex, 0, 0, {}};
        activator_->isOn = [this] { auto* c = controls(); return c ? c->active.load() : true; };
        activator_->onClick = [this] { if (auto* c = controls()) c->active.store(! c->active.load()); };
        solo_ = add<TextButton>("S", TextButton::Style::Solo);
        solo_->midiTarget = midi::ControlTarget{midi::ControlTarget::Kind::TrackSolo, controlIndex, 0, 0, {}};
        solo_->isOn = [this] { auto* c = controls(); return c && c->solo.load(); };
        solo_->onClickWithModifiers = [this](const MouseEvent& event) { toggleTrackSolo(ctx(), index_, event.cmd); };
        if (! isReturn && ! isGroup)
        {
            input_ = add<InputControls>(controlIndex);
            arm_ = add<TextButton>("", TextButton::Style::Arm);
            arm_->isMidiArm = [this] { const auto* track = ctx().trackAt(index_); return track && track->kind == model::TrackKind::Midi; };
            arm_->midiTarget = midi::ControlTarget{midi::ControlTarget::Kind::TrackArm, controlIndex, 0, 0, {}};
            arm_->isOn = [this] { auto* c = controls(); return c && c->armed.load(); };
            arm_->onClick = [this] {
                if (auto* c = controls())
                {
                    if (ctx().armTrack) ctx().armTrack(index_, ! c->armed.load());
                    else c->armed.store(! c->armed.load());
                }
            };
        }
    }

    void setActivatorLabel(const std::string& s)
    {
        if (activator_)
            activator_->setLabel(s);
    }

    // The title column takes whatever width the other columns leave (dragging the header edge resizes it).
    static float totalWidth(bool io, float title = kTitleDefault) { return title + kHeaderSep + (io ? kIoW + kHeaderSep : 0.0f) + kMixW + kMeterW; }
    float titleW() const { return std::max(40.0f, width() - (totalWidth(ctx().ioVisible, 0.0f))); }
    bool midiOnly() const { const auto* track = ctx().trackAt(index_); return track && model::isMidiOnly(*track); }

    void layout() override
    {
        const bool io = ctx().ioVisible;
        const bool shortLane = height() < kShortLane;
        ioX_ = titleW() + kHeaderSep;
        mixX_ = ioX_ + (io ? kIoW + kHeaderSep : 0.0f);
        meterX_ = mixX_ + kMixW;
        const float rowH = 13;
        const float y1 = shortLane ? std::max(1.0f, (height() - rowH) * 0.5f) : 4.0f;
        if (input_)
        {
            input_->visible = io && ! shortLane;
            input_->setBounds({ioX_ + 3, 4, kIoW - 6, InputControls::preferredHeight});
        }
        if (activator_)
            activator_->setBounds({mixX_ + 5, y1, 48, rowH});
        if (solo_)
            solo_->setBounds({mixX_ + 57, y1, 22, rowH});
        if (arm_)
        {
            arm_->visible = ! shortLane;
            arm_->setBounds({mixX_ + 83, y1, 22, rowH});
        }
        volRect_ = {mixX_ + 5, shortLane ? y1 : 22.5f, 48, rowH};
        panRect_ = {mixX_ + 57, shortLane ? y1 : 22.5f, 48, rowH};
        if (isMain_ && shortLane)
        {
            volRect_.y = panRect_.y = y1;
        }
        peakL_ = {mixX_ + 5, 41, 32, rowH};
        peakR_ = {mixX_ + 41, 41, 32, rowH};
        if (shortLane && ! isMain_)
        {
            volRect_ = panRect_ = peakL_ = peakR_ = {};
        }
        if (midiOnly()) volRect_ = panRect_ = peakL_ = peakR_ = {};
    }

    void tick() override
    {
        auto* c = controls();
        if (c == nullptr)
            return;
        const auto activity = c->midiActivity.load(std::memory_order_relaxed);
        if (activity != lastMidiActivity_) { lastMidiActivity_ = activity; midiTicks_ = 5; repaint(); }
        else if (midiTicks_ > 0 && --midiTicks_ == 0) repaint();
        const float l = c->peakL.load(), r = c->peakR.load();
        const float nl = meterDecay(l, shownL_), nr = meterDecay(r, shownR_);
        const bool changed = std::abs(nl - shownL_) > 0.0f || std::abs(nr - shownR_) > 0.0f;
        shownL_ = nl;
        shownR_ = nr;
        holdL_ = std::max(holdL_, l);
        holdR_ = std::max(holdR_, r);
        if (changed)
            repaint();
    }

    void paint(SkCanvas& c, const Theme& t) override
    {
        const auto* track = ctx().trackAt(index_);
        const bool selected = ctx().isTrackSelected(index_);
        const bool io = ctx().ioVisible;
        const bool shortLane = height() < kShortLane;
        const SkColor tc = selected && ctx().sessionTrackSelection ? t.color("SelectionBackground")
                          : track && track->deactivated ? paint::mix(Theme::paletteColor(track->colorIndex), SkColorSetRGB(0x9a, 0x9a, 0x9a), 0.8f)
                          : track && track->kind != model::TrackKind::Main ? Theme::paletteColor(track->colorIndex)
                                                                           : t.color("DisplayBackground");
        const SkColor panel = selected ? t.color("SurfaceHighlight") : t.color("SurfaceBackground");
        const SkColor line = t.color("SurfaceArea");
        const float h = height();

        // Columns
        paint::fill(c, localBounds(), panel);
        const float kTitleW = titleW();
        paint::fill(c, {0, 0, kTitleW, h}, isMain_ ? SkColorSetRGB(0x8b, 0x6f, 0x4e) : tc);
        paint::fill(c, {kTitleW, 0, kHeaderSep, h}, line);
        if (io)
            paint::fill(c, {mixX_ - kHeaderSep, 0, kHeaderSep, h}, line);
        paint::fill(c, {meterX_ - 0.0f, 0, 1, h}, line);
        paint::fill(c, {0, 0, 2, h}, line); // left border: Live outlines every header

        float indent = 0;
        if (track && ctx().song)
        {
            auto groupId = track->groupId;
            for (size_t depth = 0; groupId >= 0 && depth < ctx().song->tracks.size() && indent + 6 < kTitleW - 30; ++depth)
            {
                const auto parent = std::find_if(ctx().song->tracks.begin(), ctx().song->tracks.end(),
                                                 [groupId](const auto& candidate) { return candidate.id == groupId; });
                if (parent == ctx().song->tracks.end()) break;
                const auto parentIndex = (int) std::distance(ctx().song->tracks.begin(), parent);
                const auto frame = ctx().sessionTrackSelection && ctx().isTrackSelected(parentIndex)
                                 ? t.color("SelectionBackground") : Theme::paletteColor(parent->colorIndex);
                paint::fill(c, {indent + 2, 0, 4, h}, frame);
                indent += 6;
                paint::fill(c, {indent - 1, 0, 1, h}, line);
                paint::fill(c, {indent, 0, kTitleW - indent, 2}, frame);
                groupId = parent->groupId;
            }
        }

        // Title with fold/collapse ring
        const SkColor ink = paint::contrastText(isMain_ ? SkColorSetRGB(0x8b, 0x6f, 0x4e) : tc);
        const float ringY = shortLane ? (h - 12) * 0.5f : 4.0f;
        {
            SkPaint ring;
            ring.setAntiAlias(true);
            ring.setStyle(SkPaint::kStroke_Style);
            ring.setStrokeWidth(1.3f);
            ring.setColor(ink);
            c.drawCircle(indent + 11, ringY + 6, 5.4f, ring);
            if (isGroup_)
            {
                ring.setStrokeWidth(0.8f);
                for (int row = 0; row < 3; ++row)
                    c.drawLine(indent + 8 + (float) row * 0.4f, ringY + 4 + (float) row * 2,
                               indent + 13 + (float) row * 0.4f, ringY + 4 + (float) row * 2, ring);
            }
            else ctl::triangle(c, indent + 11, ringY + 6, 5, 4, collapsed() ? ctl::Dir::Right : ctl::Dir::Down, ink);
        }
        const std::string name = (track && track->hidden ? "(hidden) " : "") + ctx().trackDisplayName(index_);
        paint::text(c, name, Rect{indent + 21, shortLane ? 0.0f : 2.0f, kTitleW - indent - 24, shortLane ? h : 15.0f}, ink, t.metrics.fontSize, Align::Left, true);

        // I/O column for returns, groups and main
        if (io && (isReturn_ || isGroup_ || isMain_))
        {
            const float y = shortLane ? std::max(1.0f, (h - 13) * 0.5f) : 4.0f;
            if (isMain_)
                ctl::dropdown(c, t, {ioX_ + 3, y, kIoW - 6, 13}, "1/2");
            else
                ctl::emptyBox(c, t, {ioX_ + 3, y, kIoW - 6, 13});
        }

        // Volume / pan / peak readouts
        if (! volRect_.isEmpty())
        {
            const auto* ctrl = controls();
            const float gain = ctrl ? ctrl->volume.load() : 1.0f;
            const float pan = ctrl ? ctrl->pan.load() : 0.0f;
            const SkColor fill = paint::mix(t.color("RangeEditField"), t.color("ControlTextBack"), 0.3f);
            const SkColor rest = paint::mix(t.color("RangeEditField"), t.color("ControlTextBack"), 0.85f);
            ctl::framed(c, t, volRect_, rest);
            paint::fill(c, {volRect_.x + 1, volRect_.y + 1, (volRect_.w - 2) * Fader::gainToPosition(gain), volRect_.h - 2}, fill);
            paint::text(c, formatDb(gain), volRect_, t.color("ControlForeground"), t.metrics.smallFontSize, Align::Centre);
            ctl::valueBox(c, t, panRect_, formatPanText(pan));
            if (! peakL_.isEmpty())
            {
                auto peakText = [](float p) { return p <= 0.000317f ? std::string("-\xE2\x88\x9E") : formatDb(p); };
                ctl::valueBox(c, t, peakL_, peakText(holdL_));
                ctl::valueBox(c, t, peakR_, peakText(holdR_));
            }
        }

        // MIDI-only tracks carry activity dots instead of audio meters/readouts.
        if (midiOnly()) ctl::midiMeter(c, t, {meterX_ + 1, 5, kMeterW - 2, h - 10}, midiTicks_ > 0);
        else
        {
            const float mh = std::max(6.0f, h - 6);
            const float half = (kMeterW - 4) * 0.5f;
            for (int ch = 0; ch < 2; ++ch)
            {
                const Rect bar{meterX_ + 2 + (float) ch * (half + 0.5f), 3, half, mh};
                paint::fill(c, bar, t.color("MeterBackground"));
                const float level = ch == 0 ? shownL_ : shownR_;
                const float pos = Fader::gainToPosition(level);
                if (pos > 0)
                    paint::fill(c, {bar.x, bar.bottom() - bar.h * pos, bar.w, bar.h * pos},
                                level >= 1.0f ? SkColorSetRGB(0xff, 0x0a, 0x0a) : level > 0.5f ? SkColorSetRGB(0xff, 0xd1, 0x00) : SkColorSetRGB(0x00, 0xf7, 0x58));
            }
        }
        paint::fill(c, {0, h - kHeaderSep, width(), kHeaderSep}, line);
    }

    // The bottom edge of a header resizes that track's lane.
    Cursor cursorAt(float, float y) const override { return y >= height() - 5.0f ? Cursor::ResizeVertical : Cursor::Default; }

    void mouseUp(const MouseEvent&) override
    {
        if (resizingLane_ && ctx().layoutChanged) ctx().layoutChanged();
        resizingLane_ = false;
    }

    void mouseDown(const MouseEvent& e) override
    {
        resizingLane_ = false;
        if (e.x < titleW())
        {
            ctx().sessionTrackSelection = true;
            ctx().clipEditorFocused = false;
            ctx().arrangementSelection = {};
            ctx().selectedClipTrack = ctx().selectedClipUid = ctx().selectedClipScene = ctx().selectedTakeLane = -1;
            ctx().detailTab = DetailTab::Devices;
        }
        if (e.right)
        {
            if (! ctx().isTrackSelected(index_)) ctx().selectTrack(index_, false, false);
            showTrackContextMenu(ctx()); repaint(); return;
        }
        if (e.y >= height() - 5.0f)
        {
            resizingLane_ = true;
            grabLaneY_ = e.y;
            startLane_ = height();
            return;
        }
        if (ctx().selectedTakeLane >= 0)
            ctx().selectedClipTrack = ctx().selectedClipUid = ctx().selectedClipScene = ctx().selectedTakeLane = -1;
        if (e.right)
        {
            // right-click a track name: Group / Ungroup (the track joins the selection first unless it is already in it)
            if (! ctx().isTrackSelected(index_)) ctx().selectTrack(index_, false, false);
            showTrackContextMenu(ctx());
            repaint();
            return;
        }
        field_ = Field::None;
        if (e.x < titleW() && (e.shift || e.cmd))
        {
            ctx().selectTrack(index_, e.shift, e.cmd && ! e.shift); // Shift: range, Cmd: toggle (to group tracks, Cmd+G)
            repaint();
            return;
        }
        if (! (e.x < titleW() && ctx().isTrackSelected(index_))) ctx().selectTrack(index_, false, false);
        float indent = 0;
        if (const auto* track = ctx().trackAt(index_); track && ctx().song)
            for (auto id = track->groupId; id >= 0 && indent < titleW() - 36;)
            {
                const auto parent = std::find_if(ctx().song->tracks.begin(), ctx().song->tracks.end(), [id](const auto& candidate) { return candidate.id == id; });
                if (parent == ctx().song->tracks.end()) break;
                indent += 6; id = parent->groupId;
            }
        if (Rect{indent, 0, 22, std::min(height(), 22.0f)}.contains(e.x, e.y) && ! isMain_)
        {
            toggleCollapsed();
            return;
        }
        if (e.x < titleW() && e.clicks < 2 && index_ < ctx().numTracks()) ctx().beginTrackDrag();
        if (auto* ctrl = controls())
        {
            if (volRect_.contains(e.x, e.y))
            {
                field_ = Field::Volume;
                startValue_ = Fader::gainToPosition(ctrl->volume.load());
                startX_ = e.x;
                if (e.clicks >= 2) { ctrl->volume.store(1.0f); field_ = Field::None; }
            }
            else if (panRect_.contains(e.x, e.y))
            {
                field_ = Field::Pan;
                startValue_ = ctrl->pan.load();
                startX_ = e.x;
                if (e.clicks >= 2) { ctrl->pan.store(0.0f); field_ = Field::None; }
            }
            else if (peakL_.contains(e.x, e.y) || peakR_.contains(e.x, e.y))
                holdL_ = holdR_ = 0.0f;
        }
        repaint();
    }

    void mouseDrag(const MouseEvent& e) override
    {
        if (resizingLane_)
        {
            auto& heights = ctx().laneHeights;
            if (heights.size() < (size_t) ctx().numControlTracks()) heights.resize((size_t) ctx().numControlTracks(), 0.0f);
            heights[(size_t) index_] = std::clamp(startLane_ + (e.y - grabLaneY_), 18.0f, 420.0f);
            for (auto* w = parent(); w != nullptr; w = w->parent())
                if (auto* arrangement = dynamic_cast<ArrangementView*>(w)) { arrangement->layout(); arrangement->repaint(); break; }
            return;
        }
        auto* ctrl = controls();
        if (ctrl == nullptr || field_ == Field::None)
            return;
        const float dx = (e.x - startX_) / (e.shift ? 500.0f : 100.0f);
        if (field_ == Field::Volume)
            ctrl->volume.store(Fader::positionToGain(startValue_ + dx));
        else
            ctrl->pan.store(std::clamp(startValue_ + dx * 2.0f, -1.0f, 1.0f));
        repaint();
    }

private:
    enum class Field { None, Volume, Pan };

    bool collapsed() const
    {
        if (isGroup_)
            if (const auto* track = ctx().trackAt(index_)) return track->folded; // a group's ring opens and closes the group
        const auto& v = ctx().collapsedTracks;
        return std::find(v.begin(), v.end(), index_) != v.end();
    }

    void toggleCollapsed()
    {
        if (isGroup_ && ctx().song != nullptr && ctx().applyEdit && index_ < ctx().numTracks())
        {
            ctx().applyEdit(model::setTrackFolded(*ctx().song, index_, ! ctx().song->tracks[(size_t) index_].folded));
            return;
        }
        auto& v = ctx().collapsedTracks;
        const auto it = std::find(v.begin(), v.end(), index_);
        if (it == v.end()) v.push_back(index_); else v.erase(it);
        for (auto* w = parent(); w != nullptr; w = w->parent())
            if (auto* arrangement = dynamic_cast<ArrangementView*>(w)) { arrangement->layout(); break; }
        repaint();
    }

    engine::TrackControls* controls() const
    {
        auto& bank = ctx().controls;
        return bank && (size_t) index_ < bank->size() ? &(*bank)[(size_t) index_] : nullptr;
    }

    int index_;
    bool isMain_, isReturn_, isGroup_;
    float ioX_ = 0, mixX_ = 0, meterX_ = 0;
    Rect volRect_, panRect_, peakL_, peakR_;
    float shownL_ = 0, shownR_ = 0, holdL_ = 0, holdR_ = 0;
    Field field_ = Field::None;
    float startValue_ = 0, startX_ = 0;
    bool resizingLane_ = false;
    float grabLaneY_ = 0, startLane_ = 0;
    InputControls* input_ = nullptr;
    TextButton* activator_ = nullptr;
    TextButton* solo_ = nullptr;
    TextButton* arm_ = nullptr;
    uint32_t lastMidiActivity_ = 0;
    int midiTicks_ = 0;
};

struct WaveformSource
{
    const engine::Peaks* peaks = nullptr;
    double tempo = 120.0;
    float visibleX0 = 0, visibleX1 = 0;
    float displayGain = 1.0f;
};

// Where the fade handles of an audio clip sit (top corners of its body): the end of the fade-in and the start of the fade-out.
constexpr float kFadeHandle = 7.0f;
Rect fadeHandleRect(const Rect& body, float x) { return {x - kFadeHandle * 0.5f, body.y + 1.5f, kFadeHandle, kFadeHandle}; }

void drawFades(SkCanvas& c, const Theme& t, const model::Clip& clip, const Rect& r, const Rect& body, float pixelsPerBeat, SkColor ink, bool handles)
{
    const float fi = (float) (clip.fadeIn * pixelsPerBeat), fo = (float) (clip.fadeOut * pixelsPerBeat);
    SkPaint shade;
    shade.setAntiAlias(true);
    shade.setColor(paint::withAlpha(t.color("SurfaceBackground"), 150));
    SkPaint line;
    line.setAntiAlias(true);
    line.setStyle(SkPaint::kStroke_Style);
    line.setStrokeWidth(1.2f);
    line.setColor(ink);
    auto curve = [&](float length, bool fadeIn) {
        if (length <= 1.0f) return;
        const float start = fadeIn ? r.x : r.right() - length;
        SkPath path;
        for (int step = 0; step <= 48; ++step)
        {
            const double progress = (double) step / 48;
            const float px = start + length * (float) progress;
            const float py = body.bottom() - body.h * (float) model::clipFadeGain(fadeIn ? progress : 1.0 - progress, fadeIn ? clip.fadeInCurve : clip.fadeOutCurve);
            if (step == 0) path.moveTo(px, py); else path.lineTo(px, py);
        }
        c.drawPath(path, line);
        path.lineTo(start + length, body.y); path.lineTo(start, body.y); path.close();
        c.drawPath(path, shade);
    };
    curve(fi, true); curve(fo, false);
    if (handles)
        for (const bool fadeIn : {true, false})
        {
            const float length = fadeIn ? fi : fo;
            if (length < 14) continue;
            const float px = fadeIn ? r.x + length * 0.5f : r.right() - length * 0.5f;
            const float py = body.bottom() - body.h * (float) model::clipFadeGain(0.5, fadeIn ? clip.fadeInCurve : clip.fadeOutCurve);
            SkPath diamond;
            diamond.moveTo(px, py - 4); diamond.lineTo(px + 4, py); diamond.lineTo(px, py + 4); diamond.lineTo(px - 4, py); diamond.close();
            SkPaint fill; fill.setAntiAlias(true); fill.setColor(0xffffdf00); c.drawPath(diamond, fill); c.drawPath(diamond, line);
        }
    if (handles && r.w > 28.0f)
        for (const float hx : {r.x + fi, r.right() - fo})
        {
            const Rect h = fadeHandleRect(body, hx);
            paint::fill(c, h, paint::withAlpha(SK_ColorWHITE, 220));
            paint::stroke(c, h, ink);
        }
    if (handles && r.w > 28.0f)
        for (float px : {r.x + 0.5f, r.right() - kFadeHandle - 0.5f})
        {
            const Rect corner{px, body.bottom() - kFadeHandle, kFadeHandle, kFadeHandle};
            paint::fill(c, corner, paint::withAlpha(SK_ColorWHITE, 220)); paint::stroke(c, corner, ink);
        }
}

void drawClip(SkCanvas& c, const Theme& t, const model::Clip& clip, const Rect& r, float pixelsPerBeat, const WaveformSource& wave,
              bool selected = false, bool ghost = false, bool handles = false)
{
    const SkColor cc = Theme::paletteColor(clip.colorIndex);
    Rect header = r.withHeight(std::min(12.0f, r.h));
    Rect body{r.x, r.y + header.h, r.w, r.h - header.h};
    const auto bgc = t.color("SurfaceBackground");
    const bool lightTheme = 0.299f * SkColorGetR(bgc) + 0.587f * SkColorGetG(bgc) + 0.114f * SkColorGetB(bgc) > 128.0f;
    paint::fill(c, header, cc);
    paint::fill(c, body, cc);
    const bool linked = clip.linkGroup >= 0 && r.w > 20; // TRK-5
    if (linked) paint::linkGlyph(c, {header.x + 2, header.y, 14, header.h}, paint::contrastText(cc));
    if (r.w > 24)
        {
        auto label = header.reduced(3, 0);
        if (linked) { label.x += 14.0f; label.w = std::max(0.0f, label.w - 14.0f); }
        paint::text(c, clip.name, label, paint::contrastText(cc), t.metrics.smallFontSize);
    }

    const SkColor ink = paint::mix(cc, SkColorSetRGB(0, 0, 0), lightTheme ? 0.75f : 0.6f);
    if (clip.kind == model::Clip::Kind::Midi && ! clip.notes.empty() && body.h > 4)
    {
        int lo = 127, hi = 0;
        for (const auto& n : clip.notes)
        {
            lo = std::min(lo, n.pitch);
            hi = std::max(hi, n.pitch);
        }
        const float range = (float) std::max(hi - lo + 1, 8);
        const float noteH = std::max(1.0f, (body.h - 2) / range);
        const double playStart = clip.loop.loopStart + clip.loop.startRelative;
        const double loopLen = clip.loop.loopEnd - clip.loop.loopStart;
        const double clipLen = clip.currentEnd - clip.currentStart;
        // Draw each loop pass.
        for (double pass = 0; pass < clipLen; pass += (clip.loop.enabled && loopLen > 0 ? loopLen : 1e12))
            for (const auto& n : clip.notes)
            {
                double start = n.start - playStart + pass;
                if (clip.loop.enabled && (n.start < clip.loop.loopStart || n.start >= clip.loop.loopEnd))
                    continue;
                if (start < 0 || start >= clipLen)
                    continue;
                const float nx = r.x + (float) (start * pixelsPerBeat);
                const float nw = std::max(1.5f, (float) (std::min(n.duration, clipLen - start) * pixelsPerBeat) - 0.5f);
                const float ny = body.bottom() - 1 - (float) (n.pitch - lo + 1) * noteH;
                paint::fill(c, {nx, ny, nw, std::max(1.0f, noteH - 0.5f)}, ink);
            }
    }
    else if (clip.kind == model::Clip::Kind::Audio && body.h > 4)
    {
        if (wave.peaks != nullptr)
            paintWaveform(c, *wave.peaks, clip, body, r.x, pixelsPerBeat, wave.tempo, wave.visibleX0, wave.visibleX1, ink, wave.displayGain);
        else
            paint::line(c, r.x, body.centreY(), r.right(), body.centreY(), ink); // still loading
        drawFades(c, t, clip, r, body, pixelsPerBeat, ink, handles);
    }
    paint::stroke(c, r, selected ? SkColorSetRGB(255, 255, 255) : t.color("ClipBorder"), selected ? 2.0f : 1.0f);
    if (ghost)
        paint::fill(c, r, paint::withAlpha(SkColorSetRGB(255, 255, 255), 40)); // being dragged
}
} // namespace

float arrangementTitleWidth(const UiContext& ctx)
{
    return std::max(40.0f, ctx.arrangementHeaderWidth() - (kHeaderSep + (ctx.ioVisible ? kIoW + kHeaderSep : 0.0f) + kMixW + kMeterW));
}

Rect ArrangementView::overviewRect() const
{
    return {2, 2, std::max(1.0f, width() - ctx().arrangementHeaderWidth() - 4), 12};
}

double ArrangementView::overviewBeats() const
{
    double end = 64.0;
    if (const auto& song = ctx().song)
    {
        end = std::max(end, song->loopStart + song->loopLength);
        for (const auto& track : song->tracks)
            for (const auto& clip : track.arrangementClips)
                end = std::max(end, clip.currentEnd);
    }
    return end + 8.0;
}

// Compact summary of the whole arrangement with the visible window outlined (click/drag to scroll).
void ArrangementView::paintOverview(SkCanvas& c, const Theme& t, const Rect& r)
{
    auto& x = ctx();
    paint::fill(c, r, t.color("SurfaceHighlight"));
    if (x.song != nullptr && x.numTracks() > 0)
    {
        const double total = overviewBeats();
        const float pxPerBeat = (float) (r.w / total);
        const float rowH = std::max(1.0f, (r.h - 2) / (float) x.numTracks());
        for (int i = 0; i < x.numTracks(); ++i)
            for (const auto& clip : x.song->tracks[(size_t) i].arrangementClips)
                paint::fill(c, {r.x + (float) clip.currentStart * pxPerBeat, r.y + 1 + (float) i * rowH,
                                std::max(1.0f, (float) (clip.currentEnd - clip.currentStart) * pxPerBeat), std::max(1.0f, rowH - 0.5f)},
                            Theme::paletteColor(clip.colorIndex));
        const Rect view{r.x + (float) x.scrollBeats * pxPerBeat, r.y, (float) (timelineArea().w / x.pixelsPerBeat) * pxPerBeat, r.h};
        paint::fill(c, view, paint::withAlpha(SK_ColorWHITE, 70));
        paint::stroke(c, view.withWidth(std::max(2.0f, view.w)), t.color("ControlContrastFrame"));
        if (x.transport != nullptr)
            paint::line(c, r.x + (float) x.transport->positionBeats() * pxPerBeat, r.y, r.x + (float) x.transport->positionBeats() * pxPerBeat,
                        r.bottom(), t.color("ControlForeground"));
    }
    paint::stroke(c, r.reduced(-1), t.color("SurfaceArea"));
}

Rect ArrangementView::timelineArea() const
{
    const auto& m = ctx().theme.metrics;
    return {0, m.arrangementRulerHeight, width() - ctx().arrangementHeaderWidth(),
            height() - m.arrangementRulerHeight - m.arrangementFooterHeight};
}

Rect ArrangementView::waveformZoomBounds() const
{
    const float h = std::min(16.0f, ctx().theme.metrics.arrangementFooterHeight - 2);
    return {width() - 114, height() - ctx().theme.metrics.arrangementFooterHeight + 1, 52, h};
}

void ArrangementView::commitWaveformZoom()
{
    char* end = nullptr;
    const double entered = std::strtod(waveformZoomText_.c_str(), &end);
    if (end != waveformZoomText_.c_str() && *end == '\0' && std::isfinite(entered))
    {
        ctx().waveformZoom = (float) std::clamp(waveformDecibels_ ? std::pow(10.0, entered / 20) : entered, 0.01, 100.0);
        ctx().waveformZoomEnabled = true;
    }
    editingWaveformZoom_ = false;
    repaint();
}

void ArrangementView::showWaveformZoomMenu()
{
    editingWaveformZoom_ = false;
    auto pick = [this](int choice) {
        if (! Widget::isAlive(this)) return;
        if (choice == 0) waveformDecibels_ = false;
        else if (choice == 1) waveformDecibels_ = true;
        else if (choice == 2) ctx().waveformZoom = 1.0f;
        repaint();
    };
    const std::vector<std::string> labels{"Factor (x)", "Decibels (dB)", "Reset to 1.00x"};
    const std::vector<bool> ticked{! waveformDecibels_, waveformDecibels_, false};
    if (ctx().showChoices) { ctx().showChoices(labels, ticked, pick); return; }
    juce::PopupMenu menu;
    for (size_t i = 0; i < labels.size(); ++i) menu.addItem((int) i + 1, labels[i], true, ticked[i]);
    menu.showMenuAsync(juce::PopupMenu::Options(), [pick](int result) { if (result > 0) pick(result - 1); });
}

void ArrangementView::optimizeWidth()
{
    auto& x = ctx();
    if (! x.song) return;
    if (widthOptimized_)
    {
        x.pixelsPerBeat = savedPixelsPerBeat_; x.scrollBeats = savedScrollBeats_;
    }
    else
    {
        savedPixelsPerBeat_ = x.pixelsPerBeat; savedScrollBeats_ = x.scrollBeats;
        double end = beatsPerBar();
        for (const auto& track : x.song->tracks)
            for (const auto& clip : track.arrangementClips) end = std::max(end, clip.currentEnd);
        if (x.song->loopOn) end = std::max(end, x.song->loopStart + x.song->loopLength);
        x.scrollBeats = 0;
        x.pixelsPerBeat = std::clamp((timelineArea().w - 12) / end, 0.01, 2400.0);
    }
    widthOptimized_ = ! widthOptimized_;
    repaint();
}

void ArrangementView::optimizeHeight()
{
    auto& x = ctx();
    if (! x.song || lanes_.empty()) return;
    if (heightOptimized_)
    {
        x.laneHeights = savedLaneHeights_; x.scrollTracksY = savedTrackScroll_;
    }
    else
    {
        savedLaneHeights_ = x.laneHeights; savedTrackScroll_ = x.scrollTracksY;
        float fixed = 4;
        int count = 0;
        for (const auto& lane : lanes_)
        {
            const bool collapsed = std::find(x.collapsedTracks.begin(), x.collapsedTracks.end(), lane.controlIndex) != x.collapsedTracks.end();
            if (lane.takeLaneId >= 0 || collapsed) fixed += lane.h;
            else { ++count; fixed += x.automationHeight(lane.controlIndex); }
        }
        const float fitted = std::max(18.0f, (timelineArea().h - fixed) / (float) std::max(1, count));
        x.laneHeights.resize((size_t) x.numControlTracks(), 0);
        for (const auto& lane : lanes_) if (lane.takeLaneId < 0) x.laneHeights[(size_t) lane.controlIndex] = fitted;
        x.scrollTracksY = 0;
    }
    heightOptimized_ = ! heightOptimized_;
    layout(); repaint();
}

float ArrangementView::beatToX(double beat) const
{
    return (float) ((beat - ctx().scrollBeats) * ctx().pixelsPerBeat);
}

double ArrangementView::xToBeat(float x) const
{
    return ctx().scrollBeats + x / ctx().pixelsPerBeat;
}

void ArrangementView::computeLanes()
{
    lanes_.clear();
    auto& x = ctx();
    if (x.song == nullptr)
        return;
    const auto& m = x.theme.metrics;
    float y = m.arrangementRulerHeight - x.scrollTracksY;

    // Hide children of folded groups.
    std::vector<model::TrackId> folded;
    for (const auto& t : x.song->tracks)
        if (t.kind == model::TrackKind::Group && t.folded)
            folded.push_back(t.id);
    auto hidden = [&](const model::Track& t) {
        for (auto gid = t.groupId; gid != -1;)
        {
            if (std::find(folded.begin(), folded.end(), gid) != folded.end())
                return true;
            auto it = std::find_if(x.song->tracks.begin(), x.song->tracks.end(),
                                   [gid](const model::Track& o) { return o.id == gid; });
            gid = it == x.song->tracks.end() ? -1 : it->groupId;
        }
        return false;
    };

    for (int i = 0; i < x.numTracks(); ++i)
    {
        const auto& t = x.song->tracks[(size_t) i];
        if (hidden(t) || trackHiddenInViews(x, i))
            continue;
        const bool collapsed = std::find(x.collapsedTracks.begin(), x.collapsedTracks.end(), i) != x.collapsedTracks.end();
        const float custom = (size_t) i < x.laneHeights.size() ? x.laneHeights[(size_t) i] : 0.0f;
        // a closed group shows what is inside it, so its lane is taller than an open group's header lane
        const float groupHeight = t.kind == model::TrackKind::Group ? (t.folded ? 44.0f : m.arrangementLaneHeight) : m.arrangementLaneHeight;
        const float h = collapsed ? 18.0f : custom > 0.0f ? custom : groupHeight;
        const float total = h + x.automationHeight(i);
        lanes_.push_back({i, y, total});
        y += total;
        if (! x.showsAutomation(i) && std::find(x.unfoldedTakeTracks.begin(), x.unfoldedTakeTracks.end(), i) != x.unfoldedTakeTracks.end())
            for (const auto& take : t.takeLanes)
            {
                const float takeHeight = (float) std::clamp(take.height, 24, 200);
                lanes_.push_back({i, y, takeHeight, take.id}); y += takeHeight;
            }
    }
    y += 4; // gap before returns, like Live
    for (int i = x.numTracks(); i < x.numControlTracks(); ++i)
    {
        if (trackHiddenInViews(x, i))
            continue;
        const bool collapsed = std::find(x.collapsedTracks.begin(), x.collapsedTracks.end(), i) != x.collapsedTracks.end();
        const float customReturn = (size_t) i < x.laneHeights.size() ? x.laneHeights[(size_t) i] : 0.0f;
        const float total = (collapsed ? 18.0f : customReturn > 0.0f ? customReturn : 20.0f) + x.automationHeight(i);
        lanes_.push_back({i, y, total});
        y += total;
    }
}

void ArrangementView::rebuild()
{
    removeAllChildren();
    auto* waveform = add<WaveformZoomButton>();
    waveform->tooltip = "Waveform Vertical Zoom";
    waveform->onClick = [this] { ctx().waveformZoomEnabled = ! ctx().waveformZoomEnabled; repaint(); };
    waveformButton_ = waveform;
    auto* fitHeight = add<TextButton>("H");
    fitHeight->tooltip = "Optimize Arrangement Height (H)";
    fitHeight->isOn = [this] { return heightOptimized_; };
    fitHeight->onClick = [this] { optimizeHeight(); };
    heightButton_ = fitHeight;
    auto* fitWidth = add<TextButton>("W");
    fitWidth->tooltip = "Optimize Arrangement Width (W)";
    fitWidth->isOn = [this] { return widthOptimized_; };
    fitWidth->onClick = [this] { optimizeWidth(); };
    widthButton_ = fitWidth;
    laneContainer_ = add<Widget>();
    laneContainer_->interceptsMouse = false;
    auto* split = add<Splitter>(Splitter::Orientation::Vertical);
    split->growDirection = -1.0f;
    split->getSize = [this] { return ctx().arrangementHeaderWidth(); };
    split->setSize = [this](float w) {
        const bool io = ctx().ioVisible;
        const float minimum = TrackHeader::totalWidth(io, 40.0f);
        ctx().theme.metrics.arrangementHeaderWidth = std::clamp(w, minimum, 620.0f) + (io ? 0.0f : 81.0f);
        layout();
        repaint();
    };
    split->finished = [this] { if (ctx().layoutChanged) ctx().layoutChanged(); };
    headerSplit_ = split;
    headers_.clear();
    automationEditors_.clear();
    if (builtFor_ != ctx().song)
    {
        takeRange_ = {}; ctx().arrangementSelection = {}; ctx().arrangementCursor = -1; renaming_ = false;
        heightOptimized_ = widthOptimized_ = draggingWaveformZoom_ = editingWaveformZoom_ = false;
    }
    regionDrag_ = {};
    builtFor_ = ctx().song;
    builtStructure_ = ctx().trackStructure();
    if (builtFor_ == nullptr)
        return;
    const int nTracks = ctx().numTracks();
    const int nReturns = ctx().numReturns();
    for (int i = 0; i < ctx().numControlTracks(); ++i)
    {
        const bool isReturn = i >= nTracks && i < nTracks + nReturns;
        const auto* t = ctx().trackAt(i);
        auto* h = laneContainer_->add<TrackHeader>(i, i == nTracks + nReturns, isReturn, t && t->kind == model::TrackKind::Group);
        if (isReturn)
            h->setActivatorLabel(std::string(1, (char) ('A' + (i - nTracks))));
        headers_.push_back(h);
        automationEditors_.push_back(laneContainer_->add<AutomationEditor>(i));
    }
}

void ArrangementView::layout()
{
    if (builtFor_ != ctx().song || laneContainer_ == nullptr || builtStructure_ != ctx().trackStructure())
        rebuild();
    computeLanes();
    const auto zoom = waveformZoomBounds();
    waveformButton_->setBounds({zoom.x - 28, zoom.y, 24, zoom.h});
    heightButton_->setBounds({zoom.right() + 10, zoom.y, 20, zoom.h});
    widthButton_->setBounds({zoom.right() + 34, zoom.y, 20, zoom.h});
    if (! lanes_.empty())
    {
        const float total = lanes_.back().y + lanes_.back().h - ctx().theme.metrics.arrangementRulerHeight + ctx().scrollTracksY;
        const float maximum = std::max(0.0f, total - timelineArea().h + 40);
        if (ctx().scrollTracksY > maximum) { ctx().scrollTracksY = maximum; computeLanes(); }
    }
    const auto& m = ctx().theme.metrics;
    if (headerSplit_ != nullptr)
        headerSplit_->setBounds({width() - ctx().arrangementHeaderWidth() - 3, m.arrangementRulerHeight, 6, height() - m.arrangementRulerHeight - m.arrangementFooterHeight});
    laneContainer_->setBounds({0, m.arrangementRulerHeight, width(), height() - m.arrangementRulerHeight - m.arrangementFooterHeight});
    for (auto* h : headers_)
        h->visible = false;
    for (auto* editor : automationEditors_) editor->visible = false;
    for (const auto& lane : lanes_)
    {
        if (lane.takeLaneId >= 0) continue;
        const float clipHeight = lane.h - ctx().automationHeight(lane.controlIndex);
        auto* editor = automationEditors_[(size_t) lane.controlIndex];
        editor->visible = ctx().showsAutomation(lane.controlIndex) && lane.y + lane.h > m.arrangementRulerHeight && lane.y + clipHeight < height();
        editor->setBounds({0, lane.y + clipHeight - m.arrangementRulerHeight, width(), ui::UiContext::automationLaneHeight});
        auto* h = headers_[(size_t) lane.controlIndex];
        h->visible = lane.y + lane.h > m.arrangementRulerHeight && lane.y < height();
        h->setBounds({width() - ctx().arrangementHeaderWidth(), lane.y - m.arrangementRulerHeight, ctx().arrangementHeaderWidth(), clipHeight});
    }
}

void ArrangementView::tick()
{
    if (builtFor_ != ctx().song || builtStructure_ != ctx().trackStructure())
    {
        layout();
        repaint();
        return;
    }
    const double pos = ctx().transport ? ctx().transport->positionBeats() : 0.0;
    if (std::abs(pos - lastPlayhead_) * ctx().pixelsPerBeat >= 0.5)
    {
        lastPlayhead_ = pos;
        // Follow the playhead (Live's "Follow" behaviour) when it leaves the view.
        const float px = beatToX(pos);
        if (ctx().transport && ctx().transport->isPlaying() && (px > timelineArea().w - 20 || px < 0))
            ctx().scrollBeats = std::max(0.0, pos - 2.0);
        repaint();
    }
}

void ArrangementView::paint(SkCanvas& c, const Theme& t)
{
    const auto& m = t.metrics;
    auto& x = ctx();
    paint::fill(c, localBounds(), t.color("SurfaceBackground"));
    if (x.song == nullptr)
    {
        paint::text(c, "Open a Live Set (Cmd+O) or double-click one in the browser", localBounds(),
                    t.color("TextDisabled"), m.fontSize + 2, Align::Centre);
        return;
    }

    const auto area = timelineArea();
    const double beatsPerBar = x.song->timeSigNumerator * 4.0 / x.song->timeSigDenominator;
    const double firstBeat = xToBeat(0), lastBeat = xToBeat(area.w);

    const double beatUnit = 4.0 / std::max(1, x.song->timeSigDenominator);
    const double grid = gridBeats();
    const auto gridLinesNow = ui::gridLines(firstBeat, lastBeat, x.pixelsPerBeat, beatsPerBar, beatUnit, grid);
    const double subFade = std::clamp((grid * x.pixelsPerBeat - 3.0) / 9.0, 0.0, 1.0);
    double tileUnit = beatUnit; // alternate per beat when there is room, otherwise per bar or several bars
    while (tileUnit * x.pixelsPerBeat < 28.0)
        tileUnit = tileUnit < beatsPerBar - 1e-9 ? beatsPerBar : tileUnit * 2.0;

    // ---- lanes
    c.save();
    c.clipRect(area.sk());
    std::vector<std::shared_ptr<const engine::Peaks>> keepAlive; // overviews used by this frame
    auto waveFor = [&](const model::Clip& clip) {
        WaveformSource w;
        if (clip.kind == model::Clip::Kind::Audio && x.peaks)
        {
            keepAlive.push_back(x.peaks(clip));
            w.peaks = keepAlive.back().get();
        }
        w.tempo = x.song != nullptr ? x.song->tempo : 120.0;
        w.visibleX0 = 0;
        w.visibleX1 = area.w;
        w.displayGain = x.waveformZoomEnabled ? x.waveformZoom : 1.0f;
        return w;
    };
    for (const auto& lane : lanes_)
    {
        if (lane.y + lane.h < area.y || lane.y > height())
            continue;
        const float clipHeight = lane.h - (lane.takeLaneId < 0 ? x.automationHeight(lane.controlIndex) : 0.0f);
        Rect laneRect{0, lane.y, area.w, clipHeight};
        const bool selected = x.isTrackSelected(lane.controlIndex) && (lane.takeLaneId < 0 || lane.takeLaneId == x.selectedTakeLane);
        const auto* track = x.trackAt(lane.controlIndex);
        const bool group = track && track->kind == model::TrackKind::Group;
        const SkColor laneBg = t.color(group ? "SurfaceHighlight" : "SurfaceBackground");
        paint::fill(c, laneRect, selected && ! group ? paint::mix(laneBg, t.color("SurfaceArea"), 0.32f) : laneBg);

        // Alternating tiles (beats when zoomed in, else bars) and the adaptive/fixed grid lines.
        for (double b = std::floor(firstBeat / tileUnit) * tileUnit; b < lastBeat; b += tileUnit)
            if (((long long) std::llround(b / tileUnit)) % 2 != 0)
                paint::fill(c, {beatToX(b), lane.y, (float) (tileUnit * x.pixelsPerBeat), clipHeight},
                            group ? paint::mix(laneBg, t.color("ArrangerGridTiles"), 0.3f) : t.color("ArrangerGridTiles"));
        for (const auto& line : gridLinesNow)
        {
            const SkColor base = t.color("GridLineBase");
            const SkColor colour = line.strength == 2 ? base : paint::withAlpha(base, line.strength == 1 ? (uint8_t) 0x50 : (uint8_t) (0x30 * subFade));
            paint::line(c, beatToX(line.beat), lane.y, beatToX(line.beat), lane.y + clipHeight, colour);
        }

        const auto* clips = laneClips(lane);
        const bool movingRegion = regionDrag_.active && regionDrag_.moved && regionDrag_.valid && lane.takeLaneId < 0;
        c.save();
        if (movingRegion)
        {
            const auto& selection = regionDrag_.original;
            if (! regionDrag_.copy && lane.controlIndex >= selection.firstTrack && lane.controlIndex <= selection.lastTrack)
                c.clipRect(Rect{beatToX(selection.start), lane.y, (float) ((selection.end - selection.start) * x.pixelsPerBeat), clipHeight}.sk(), SkClipOp::kDifference);
            if (lane.controlIndex >= selection.firstTrack + regionDrag_.trackOffset && lane.controlIndex <= selection.lastTrack + regionDrag_.trackOffset)
                c.clipRect(Rect{beatToX(regionDrag_.at), lane.y, (float) ((selection.end - selection.start) * x.pixelsPerBeat), clipHeight}.sk(), SkClipOp::kDifference);
        }
        if (track != nullptr && track->kind == model::TrackKind::Group && track->folded && lane.takeLaneId < 0 && clipHeight > 12
            && lane.controlIndex < x.numTracks())
        {
            // A closed group shows the clips of everything inside it (children and nested groups) in their own colours.
            for (const auto& child : x.song->tracks)
            {
                bool inside = false;
                for (auto gid = child.groupId; gid != -1 && ! inside;)
                {
                    inside = gid == track->id;
                    const auto parent = std::find_if(x.song->tracks.begin(), x.song->tracks.end(), [gid](const model::Track& o) { return o.id == gid; });
                    gid = parent == x.song->tracks.end() ? -1 : parent->groupId;
                }
                if (! inside || child.kind == model::TrackKind::Group)
                    continue;
                const SkColor tint = paint::mix(Theme::paletteColor(child.colorIndex), SK_ColorWHITE, 0.35f);
                for (const auto& clip : child.arrangementClips)
                {
                    if (clip.disabled || clip.currentEnd < firstBeat || clip.currentStart > lastBeat)
                        continue;
                    const float cx0 = beatToX(clip.currentStart), cx1 = beatToX(clip.currentEnd);
                    const Rect block{cx0, lane.y + 3, std::max(2.0f, cx1 - cx0), clipHeight - 6};
                    paint::fill(c, block, paint::withAlpha(tint, 150));
                    paint::stroke(c, block, paint::withAlpha(t.color("ClipBorder"), 160));
                }
            }
        }
        if (track != nullptr && clips && clipHeight > 20)
            for (const auto& clip : *clips)
            {
                if (drag_.active && drag_.moved && ! drag_.copy && lane.controlIndex == drag_.track && lane.takeLaneId == drag_.laneId && clip.uid == drag_.uid)
                    continue; // drawn as the preview below (a copy leaves the original where it is)
                if (clip.currentEnd < firstBeat || clip.currentStart > lastBeat)
                    continue;
                const float x0 = beatToX(clip.currentStart);
                const float x1 = beatToX(clip.currentEnd);
                const bool selectedClip = x.selectedClipScene < 0 && lane.takeLaneId == x.selectedTakeLane && lane.controlIndex == x.selectedClipTrack && clip.uid == x.selectedClipUid;
                const bool hovered = lane.takeLaneId < 0 && lane.controlIndex == hoverTrack_ && clip.uid == hoverUid_;
                drawClip(c, t, clip, {x0, lane.y + 1, std::max(2.0f, x1 - x0), clipHeight - 2}, (float) x.pixelsPerBeat,
                         waveFor(clip), selectedClip, false, lane.takeLaneId < 0 && (selectedClip || hovered));
                if (lane.takeLaneId >= 0)
                    paint::fill(c, {x0, lane.y + 1, std::max(2.0f, x1 - x0), clipHeight - 2}, paint::withAlpha(t.color("SurfaceArea"), 95));
                if (lane.takeLaneId >= 0 && clip.takeId >= 0)
                    for (const auto& comp : track->arrangementClips)
                        if (comp.takeId == clip.takeId && ! comp.disabled)
                        {
                            const double left = std::max(clip.currentStart, comp.currentStart), right = std::min(clip.currentEnd, comp.currentEnd);
                            if (right > left) paint::fill(c, {beatToX(left), lane.y + clipHeight - 4, (float) ((right - left) * x.pixelsPerBeat), 3}, t.color("ChosenDefault"));
                        }
            }
        c.restore();
        if (movingRegion && clipHeight > 20)
        {
            const double delta = regionDrag_.at - regionDrag_.original.start;
            const float span = (float) ((regionDrag_.original.end - regionDrag_.original.start) * x.pixelsPerBeat);
            c.save(); c.clipRect(Rect{beatToX(regionDrag_.at), lane.y, span, clipHeight}.sk());
            for (const auto& [sourceTrack, clip] : regionDrag_.clips)
                if (sourceTrack + regionDrag_.trackOffset == lane.controlIndex)
                {
                    const float x0 = beatToX(clip.currentStart + delta), x1 = beatToX(clip.currentEnd + delta);
                    drawClip(c, t, clip, {x0, lane.y + 1, std::max(2.0f, x1 - x0), clipHeight - 2}, (float) x.pixelsPerBeat,
                             waveFor(clip), true, true, false);
                }
            c.restore();
        }
        if (drag_.active && drag_.moved && lane.controlIndex == drag_.targetTrack && lane.takeLaneId == drag_.laneId)
        {
            const float x0 = beatToX(drag_.preview.currentStart);
            const float x1 = beatToX(drag_.preview.currentEnd);
            drawClip(c, t, drag_.preview, {x0, lane.y + 1, std::max(2.0f, x1 - x0), clipHeight - 2}, (float) x.pixelsPerBeat,
                     waveFor(drag_.preview), true, true, true);
        }
        if (takeRange_.active && lane.controlIndex == takeRange_.track && lane.takeLaneId == takeRange_.laneId)
        {
            Rect range{beatToX(takeRange_.start), lane.y, (float) ((takeRange_.end - takeRange_.start) * x.pixelsPerBeat), clipHeight};
            paint::fill(c, range, paint::withAlpha(t.color("SelectionBackground"), 100));
            paint::stroke(c, range, t.color("SelectionFrame"));
        }
        paint::fill(c, {0, lane.y + lane.h - 2, area.w, 2}, t.color("SurfaceArea"));
    }
    c.restore();

    // ---- ruler: overview strip, bar numbers, locators and the loop brace
    Rect ruler{0, 0, area.w, m.arrangementRulerHeight};
    paint::fill(c, ruler, t.color("SurfaceBackground"));
    paintOverview(c, t, overviewRect());
    if (x.transport != nullptr || x.song != nullptr)
    {
        const double ls = x.transport ? x.transport->loopStart() : x.song->loopStart;
        const double ll = x.transport ? x.transport->loopLength() : x.song->loopLength;
        const bool on = x.transport ? x.transport->loopEnabled() : x.song->loopOn;
        c.save(); c.clipRect(ruler.sk());
        ctl::loopBrace(c, t, {beatToX(ls), ruler.bottom() - 13, (float) (ll * x.pixelsPerBeat), 10}, on);
        c.restore();
    }
    c.save();
    c.clipRect(Rect{0, 16, area.w, m.arrangementRulerHeight - 16}.sk());
    // Live's time ruler: bars (1, 2), beats (1.2, 1.3) and finer positions (1.1.3) as the zoom allows, plus unlabelled ticks.
    for (const auto& mark : ui::rulerMarks(firstBeat, lastBeat, x.pixelsPerBeat, beatsPerBar, beatUnit))
    {
        const float bx = beatToX(mark.beat);
        if (mark.labelled)
        {
            paint::line(c, bx + 0.5f, 18, bx + 0.5f, 31, t.color("ArrangementRulerMarkings"));
            paint::text(c, mark.text, {bx + 4, 16, 64, 14}, t.color("ControlForeground"), m.fontSize);
        }
        else
            paint::line(c, bx + 0.5f, mark.level == 0 ? 23.0f : mark.level == 1 ? 26.0f : 28.0f, bx + 0.5f, 31,
                        paint::withAlpha(t.color("ArrangementRulerMarkings"), 160));
    }
    for (const auto& loc : x.song->locators)
    {
        const float lx = beatToX(loc.time);
        paint::playTriangle(c, {lx - 3, 34, 7, 8}, t.color("ControlForeground"));
        if (! loc.name.empty())
            paint::text(c, loc.name, {lx + 6, 31, 160, 14}, t.color("ControlForeground"), m.smallFontSize);
    }
    c.restore();
    paint::fill(c, {0, ruler.bottom() - 1, area.w, 1}, t.color("SurfaceArea"));

    // ---- footer: time ruler in minutes:seconds
    {
        const Rect footer{0, height() - m.arrangementFooterHeight, area.w, m.arrangementFooterHeight};
        paint::fill(c, footer, t.color("SurfaceBackground"));
        const double secondsPerBeat = 60.0 / std::max(1.0, x.transport ? x.transport->tempo() : x.song->tempo);
        const double pixelsPerSecond = x.pixelsPerBeat / secondsPerBeat;
        double step = 1;
        for (double candidate : {1.0, 2.0, 5.0, 10.0, 15.0, 30.0, 60.0, 120.0, 300.0, 600.0})
        {
            step = candidate;
            if (candidate * pixelsPerSecond >= 70)
                break;
        }
        const double firstSecond = firstBeat * secondsPerBeat, lastSecond = lastBeat * secondsPerBeat;
        for (double sec = std::floor(firstSecond / step) * step; sec < lastSecond; sec += step)
        {
            const float sx = beatToX(sec / secondsPerBeat);
            paint::line(c, sx + 0.5f, footer.y + 1, sx + 0.5f, footer.y + 9, t.color("ArrangementRulerMarkings"));
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%d:%02d", (int) sec / 60, (int) sec % 60);
            paint::text(c, buf, {sx + 3, footer.y, 50, footer.h}, t.color("ControlForeground"), m.smallFontSize);
        }
    }

    // Grid indicator in the corner of the timeline (Live shows the current grid size, e.g. 1/8).
    paint::text(c, ui::gridLabel(grid, beatsPerBar) + (x.arrangementGrid.snap ? "" : " (off)"),
                {area.w - 90, height() - m.arrangementFooterHeight - 22, 82, 18}, paint::withAlpha(t.color("ControlForeground"), 90),
                m.fontSize + 4, Align::Right, true);

    // Header column background (headers are child widgets)
    paint::fill(c, {area.w, 0, ctx().arrangementHeaderWidth(), height()}, t.color("SurfaceBackground"));
    char zoomText[24];
    if (waveformDecibels_) std::snprintf(zoomText, sizeof(zoomText), "%.1f dB", 20.0 * std::log10(x.waveformZoom));
    else std::snprintf(zoomText, sizeof(zoomText), "%.2fx", x.waveformZoom);
    const auto zoomBox = waveformZoomBounds();
    ctl::valueBox(c, t, zoomBox, editingWaveformZoom_ ? waveformZoomText_ : zoomText);
    if (editingWaveformZoom_)
    {
        paint::fill(c, zoomBox.reduced(1), t.color(waveformTextSelected_ ? "SelectionBackground" : "ControlTextBack"));
        paint::text(c, waveformZoomText_ + (waveformTextSelected_ ? "" : "|"), zoomBox, t.color("ControlForeground"), m.smallFontSize, Align::Centre);
    }
    paint::fill(c, {area.w, 0, 2, m.arrangementRulerHeight}, t.color("SurfaceBackground"));
    c.save(); c.clipRect({area.w, area.y, width(), height()});
    for (const auto& lane : lanes_) if (lane.takeLaneId >= 0 && lane.y + lane.h > area.y && lane.y < height())
    {
        const auto* take = model::findTakeLane(*x.song, lane.controlIndex, lane.takeLaneId);
        if (! take) continue;
        Rect header{area.w, lane.y, ctx().arrangementHeaderWidth(), lane.h};
        const bool selected = x.selectedTrack == lane.controlIndex && x.selectedTakeLane == lane.takeLaneId;
        paint::fill(c, header, selected ? t.color("SurfaceHighlight") : paint::mix(Theme::paletteColor(x.trackAt(lane.controlIndex)->colorIndex), t.color("SurfaceBackground"), 0.7f));
        const bool editing = renaming_ && renameTrack_ == lane.controlIndex && renameLane_ == lane.takeLaneId;
        if (editing) paint::fill(c, {header.x + 10, header.y + 3, header.w - 80, 18}, t.color("ControlBackground"));
        paint::text(c, editing ? renameText_ + "|" : take->name, {header.x + 12, header.y + 4, header.w - 78, 16}, t.color("ControlForeground"), m.fontSize);
        Rect button{header.right() - 23, header.y + 4, 18, 15};
        paint::fill(c, button, take->audition ? t.color("ChosenDefault") : t.color("ControlBackground"));
        paint::text(c, "T", button, t.color("ControlForeground"), m.smallFontSize, Align::Centre);
        c.save(); c.translate(header.right() - 56, header.y + 11); c.rotate(-90);
        paint::playTriangle(c, {-4, -4, 8, 8}, t.color("ControlForeground")); c.restore();
        c.save(); c.translate(header.right() - 35, header.y + 11); c.rotate(90);
        paint::playTriangle(c, {-4, -4, 8, 8}, t.color("ControlForeground")); c.restore();
        paint::line(c, header.x + 0.5f, header.y, header.x + 0.5f, header.bottom(), t.color("ControlContrastFrame"));
        paint::line(c, header.x, header.bottom() - 0.5f, header.right(), header.bottom() - 0.5f, t.color("ControlContrastFrame"));
    }
    c.restore();
}

void ArrangementView::paintOverChildren(SkCanvas& c, const Theme& t)
{
    auto& x = ctx();
    if (x.song == nullptr)
        return;
    const double pos = x.transport ? x.transport->positionBeats() : 0.0;
    const float px = beatToX(pos);
    const auto area = timelineArea();
    if (x.arrangementSelection.active)
    {
        auto selection = x.arrangementSelection;
        const bool preview = regionDrag_.active && regionDrag_.moved;
        if (preview)
        {
            selection.end = regionDrag_.at + regionDrag_.original.end - regionDrag_.original.start;
            selection.start = regionDrag_.at;
            selection.firstTrack += regionDrag_.trackOffset; selection.lastTrack += regionDrag_.trackOffset;
        }
        c.save(); c.clipRect(area.sk());
        for (const auto& lane : lanes_)
            if (lane.takeLaneId < 0 && lane.controlIndex >= selection.firstTrack && lane.controlIndex <= selection.lastTrack)
            {
                const Rect highlight{beatToX(selection.start), lane.y, (float) ((selection.end - selection.start) * x.pixelsPerBeat), lane.h};
                paint::fill(c, highlight, paint::withAlpha(t.color("SelectionBackground"), 85));
                if (preview) paint::stroke(c, highlight.reduced(0.5f), regionDrag_.valid ? t.color("SelectionFrame") : t.color("ChosenRecord"));
            }
        c.restore();
    }
    if (px >= 0 && px <= area.w)
        paint::line(c, px, 0, px, height(), x.transport && x.transport->isPlaying() ? t.color("ControlForeground")
                                                                                     : t.color("ChosenDefault"));
    if (! x.arrangementSelection.active && ! x.sessionTrackSelection && x.arrangementCursor >= 0)
    {
        const float cursor = beatToX(x.arrangementCursor);
        if (cursor >= 0 && cursor < area.w)
            for (const auto& lane : lanes_)
                if (lane.controlIndex == x.selectedTrack && lane.takeLaneId < 0)
                    paint::line(c, cursor, std::max(area.y, lane.y), cursor, std::min(area.bottom(), lane.y + lane.h), t.color("ControlForeground"));
    }
    paintCollaborators(c, t);
}

// COL-5: where the others are: their selected clip outlined, their time selection framed, their pointer with a name tag.
void ArrangementView::paintCollaborators(SkCanvas& c, const Theme& t)
{
    auto& x = ctx();
    if (x.collab.cursors.empty() || x.song == nullptr) return;
    const auto area = timelineArea();
    c.save();
    c.clipRect(area.sk());
    for (const auto& cursor : x.collab.cursors)
    {
        const auto colour = collab::colour(cursor);
        int fromIndex = -1, toIndex = -1;
        for (int i = 0; i < x.numControlTracks(); ++i)
            if (const auto* track = x.trackAt(i))
            {
                if (track->id == cursor.selectedTrackFrom) fromIndex = i;
                if (track->id == cursor.selectedTrackTo) toIndex = i;
            }
        for (const auto& lane : lanes_)
        {
            if (lane.takeLaneId >= 0) continue;
            const auto* track = x.trackAt(lane.controlIndex);
            if (track == nullptr) continue;
            if (cursor.selectedClipUid >= 0)
                for (const auto& clip : track->arrangementClips)
                    if (clip.uid == cursor.selectedClipUid)
                    {
                        const Rect r{beatToX(clip.currentStart), lane.y + 1, (float) ((clip.currentEnd - clip.currentStart) * x.pixelsPerBeat), lane.h - 2};
                        paint::stroke(c, r, colour, 2.0f);
                    }
            if (cursor.selectionEnd > cursor.selectionStart && cursor.selectionStart >= 0 && fromIndex >= 0 && toIndex >= 0
                && lane.controlIndex >= std::min(fromIndex, toIndex) && lane.controlIndex <= std::max(fromIndex, toIndex))
            {
                const Rect r{beatToX(cursor.selectionStart), lane.y, (float) ((cursor.selectionEnd - cursor.selectionStart) * x.pixelsPerBeat), lane.h};
                paint::fill(c, r, paint::withAlpha(colour, 28));
                paint::stroke(c, r.reduced(0.5f), paint::withAlpha(colour, 140));
            }
            if (cursor.view == 0 && cursor.beat >= 0 && track->id == cursor.trackId)
                collab::pointer(c, beatToX(cursor.beat), lane.y, lane.y + lane.h, cursor, t.metrics.fontSize);
        }
    }
    c.restore();
}

void ArrangementView::reportPointer(const MouseEvent& e)
{
    auto& x = ctx();
    if (! x.collabPointer || ! x.collab.active()) return;
    const auto* lane = laneAt(e.y);
    const bool inside = timelineArea().contains(e.x, e.y);
    x.collabPointer(0, lane && inside ? lane->controlIndex : -1, inside ? std::max(0.0, xToBeat(e.x)) : -1.0, -1);
}

void ArrangementView::mouseExit()
{
    auto& x = ctx();
    if (x.collabPointer && x.collab.active()) x.collabPointer(0, -1, -1.0, -1);
}

double ArrangementView::beatsPerBar() const
{
    return ctx().song ? ctx().song->timeSigNumerator * 4.0 / ctx().song->timeSigDenominator : 4.0;
}

double ArrangementView::gridBeats() const
{
    return ui::gridBeats(ctx().arrangementGrid, ctx().pixelsPerBeat, beatsPerBar());
}

double ArrangementView::snapUnit(const MouseEvent& e) const
{
    return ui::snapSize(ctx().arrangementGrid, ctx().pixelsPerBeat, beatsPerBar(), e.alt || e.cmd);
}

void ArrangementView::zoomAt(float anchorX, double factor)
{
    widthOptimized_ = false;
    auto& x = ctx();
    const double anchor = xToBeat(anchorX);
    x.pixelsPerBeat = std::clamp(x.pixelsPerBeat * factor, 0.25, 2400.0);
    x.scrollBeats = std::max(0.0, anchor - anchorX / x.pixelsPerBeat);
    layout();
    repaint();
}

void ArrangementView::zoomTrackHeight(double factor, float anchorY)
{
    auto& x = ctx();
    const auto* anchor = laneAt(anchorY);
    if (! anchor || anchor->takeLaneId >= 0) return;
    const int anchorTrack = anchor->controlIndex;
    const float fraction = std::clamp((anchorY - anchor->y) / anchor->h, 0.0f, 1.0f);
    if (x.laneHeights.size() < (size_t) x.numControlTracks()) x.laneHeights.resize((size_t) x.numControlTracks(), 0);
    const auto range = x.arrangementSelection;
    for (const auto& lane : lanes_)
    {
        if (lane.takeLaneId >= 0) continue;
        const bool selected = range.active ? lane.controlIndex >= range.firstTrack && lane.controlIndex <= range.lastTrack
                                           : x.sessionTrackSelection ? x.isTrackSelected(lane.controlIndex) : lane.controlIndex == anchorTrack;
        if (! selected) continue;
        const float automation = x.automationHeight(lane.controlIndex);
        x.laneHeights[(size_t) lane.controlIndex] = std::clamp((float) ((lane.h - automation) * factor), 18.0f, 420.0f);
        if (factor > 1)
            x.collapsedTracks.erase(std::remove(x.collapsedTracks.begin(), x.collapsedTracks.end(), lane.controlIndex), x.collapsedTracks.end());
    }
    heightOptimized_ = false;
    computeLanes();
    for (const auto& lane : lanes_)
        if (lane.takeLaneId < 0 && lane.controlIndex == anchorTrack)
            x.scrollTracksY = std::max(0.0f, x.scrollTracksY + lane.y + fraction * lane.h - anchorY);
    layout(); repaint();
    if (x.layoutChanged) x.layoutChanged();
}

bool ArrangementView::mouseMagnify(const MouseEvent& e, float factor)
{
    if (e.x >= timelineArea().w)
        return false;
    zoomAt(e.x, std::clamp((double) factor, 0.5, 2.0));
    return true;
}

const ArrangementView::Lane* ArrangementView::laneAt(float y) const
{
    for (const auto& lane : lanes_) if (y >= lane.y && y < lane.y + lane.h) return &lane;
    return nullptr;
}

const std::vector<model::Clip>* ArrangementView::laneClips(const Lane& lane) const
{
    if (! ctx().song || lane.controlIndex < 0 || lane.controlIndex >= ctx().numTracks()) return nullptr;
    if (lane.takeLaneId < 0) return &ctx().song->tracks[(size_t) lane.controlIndex].arrangementClips;
    const auto* take = model::findTakeLane(*ctx().song, lane.controlIndex, lane.takeLaneId);
    return take ? &take->clips : nullptr;
}

int ArrangementView::laneTrackAt(float y) const
{
    for (const auto& lane : lanes_)
        if (y >= lane.y && y < lane.y + lane.h)
            return lane.takeLaneId < 0 && lane.controlIndex < ctx().numTracks() ? lane.controlIndex : -1;
    return -1;
}

bool ArrangementView::browserDrop(const BrowserDrag& payload, const MouseEvent& event)
{
    for (const auto& lane : lanes_)
        if (event.y >= lane.y && event.y < lane.y + lane.h)
        {
            if (lane.takeLaneId >= 0) { ctx().statusText = "Drop samples onto the main track lane"; return true; }
            if (ctx().dropBrowserItem)
                ctx().dropBrowserItem(payload, lane.controlIndex,
                    std::max(0.0, ui::snapToGrid(xToBeat(event.x), snapUnit(event))), -1);
            return true;
        }
    // Samples dropped on the empty space below the last lane go to a new audio track.
    const auto area = timelineArea();
    float lanesEnd = area.y;
    for (const auto& lane : lanes_) lanesEnd = std::max(lanesEnd, lane.y + lane.h);
    if (payload.kind == BrowserDrag::Kind::Sample && event.y >= lanesEnd && event.y < area.bottom())
    {
        const double beat = event.x < area.right() ? std::max(0.0, ui::snapToGrid(xToBeat(event.x), snapUnit(event))) : 0.0;
        if (ctx().dropBrowserItem) ctx().dropBrowserItem(payload, -1, beat, -1);
        return true;
    }
    return false;
}

const model::Clip* ArrangementView::clipAt(float x, float y, model::ClipLocation& location, float& clipX0, float& clipX1) const
{
    const auto& song = ctx().song;
    if (song == nullptr)
        return nullptr;
    for (const auto& lane : lanes_)
    {
        if (y < lane.y || y >= lane.y + lane.h - (lane.takeLaneId < 0 ? ctx().automationHeight(lane.controlIndex) : 0.0f) || lane.controlIndex >= ctx().numTracks() || lane.h <= 20)
            continue;
        const auto* clips = laneClips(lane);
        if (! clips) continue;
        for (auto it = clips->rbegin(); it != clips->rend(); ++it) // later clips are drawn on top
        {
            const float x0 = beatToX(it->currentStart), x1 = std::max(beatToX(it->currentEnd), x0 + 2.0f);
            if (x >= x0 && x < x1)
            {
                location = lane.takeLaneId >= 0 ? model::ClipLocation::take(lane.controlIndex, it->uid, lane.takeLaneId)
                                           : model::ClipLocation::arrangement(lane.controlIndex, it->uid);
                clipX0 = x0;
                clipX1 = x1;
                return &*it;
            }
        }
    }
    return nullptr;
}

int ArrangementView::laneEdgeAt(float x, float y) const
{
    if (x >= timelineArea().w || y < ctx().theme.metrics.arrangementRulerHeight)
        return -1;
    for (const auto& lane : lanes_)
        if (lane.takeLaneId < 0 && std::fabs(y - (lane.y + lane.h - 1.0f)) <= 3.0f && ! ctx().showsAutomation(lane.controlIndex))
            return lane.controlIndex;
    return -1;
}

bool ArrangementView::fadeHandleAt(const model::Clip& clip, const model::ClipLocation& location, float clipX0, float clipX1, float x, float y,
                                   ui::DragMode& mode) const
{
    if (clip.kind != model::Clip::Kind::Audio || location.kind == model::ClipLocation::Kind::Take || clipX1 - clipX0 < 28.0f)
        return false;
    const Lane* lane = laneAt(y);
    if (lane == nullptr)
        return false;
    const float bodyTop = lane->y + 1 + 12; // below the clip's title strip
    for (const bool fadeIn : {true, false})
    {
        const float length = (float) ((fadeIn ? clip.fadeIn : clip.fadeOut) * ctx().pixelsPerBeat);
        if (length < 14) continue;
        const float px = fadeIn ? clipX0 + length * 0.5f : clipX1 - length * 0.5f;
        const float bodyHeight = lane->h - 15.0f;
        const float py = bodyTop + bodyHeight - bodyHeight * (float) model::clipFadeGain(0.5, fadeIn ? clip.fadeInCurve : clip.fadeOutCurve);
        if (std::fabs(x - px) <= 8 && std::fabs(y - py) <= 8)
        { mode = fadeIn ? ui::DragMode::FadeInCurve : ui::DragMode::FadeOutCurve; return true; }
    }
    if (y < bodyTop - 2 || y > bodyTop + kFadeHandle + 4) return false;
    const float inX = clipX0 + (float) (clip.fadeIn * ctx().pixelsPerBeat), outX = clipX1 - (float) (clip.fadeOut * ctx().pixelsPerBeat);
    const bool nearIn = std::fabs(x - inX) <= kFadeHandle, nearOut = std::fabs(x - outX) <= kFadeHandle;
    if (! nearIn && ! nearOut)
        return false;
    // overlapping handles (a clip fully faded): the closer one wins
    mode = nearIn && (! nearOut || std::fabs(x - inX) <= std::fabs(x - outX)) ? ui::DragMode::FadeIn : ui::DragMode::FadeOut;
    return true;
}

ArrangementView::LoopDrag ArrangementView::loopZoneAt(float x, float y) const
{
    const auto& m = ctx().theme.metrics;
    if (ctx().transport == nullptr || y >= m.arrangementRulerHeight || y <= m.arrangementRulerHeight - 14 || x >= timelineArea().w)
        return LoopDrag::None;
    const double ls = ctx().transport->loopStart(), ll = ctx().transport->loopLength();
    const float bx0 = beatToX(ls), bx1 = beatToX(ls + ll);
    if (x < bx0 - 4 || x > bx1 + 4) return LoopDrag::None;
    return x < bx0 + 6 ? LoopDrag::Left : x > bx1 - 6 ? LoopDrag::Right : LoopDrag::Move;
}

ui::DragMode ArrangementView::clipEdgeAt(const Lane* lane, float x, float y, float x0, float x1) const
{
    if (lane == nullptr || y >= lane->y + 13) return ui::DragMode::Move; // below the title bar
    return ui::hitZone(x - x0, x1 - x0);
}

Cursor ArrangementView::cursorAt(float x, float y) const
{
    // The loop brace: its ends resize (the resize cursor shows it), its body moves it.
    if (const auto zone = loopDrag_ != LoopDrag::None ? loopDrag_ : loopZoneAt(x, y); zone != LoopDrag::None)
        return zone == LoopDrag::Move ? Cursor::Move : Cursor::ResizeHorizontal;
    model::ClipLocation location;
    float x0 = 0, x1 = 0;
    ui::DragMode mode;
    if (const auto* clip = clipAt(x, y, location, x0, x1); clip != nullptr && fadeHandleAt(*clip, location, x0, x1, x, y, mode))
        return mode == ui::DragMode::FadeInCurve || mode == ui::DragMode::FadeOutCurve ? Cursor::ResizeVertical : Cursor::ResizeHorizontal;
    if (const auto* clip = clipAt(x, y, location, x0, x1); clip && clipEdgeAt(laneAt(y), x, y, x0, x1) != ui::DragMode::Move)
        return Cursor::ResizeHorizontal;
    if (laneEdgeAt(x, y) >= 0) return Cursor::ResizeVertical;
    if (inTimeSelection(x, y)) return Cursor::Move;
    return Cursor::Default;
}

bool ArrangementView::inTimeSelection(float x, float y) const
{
    const auto& range = ctx().arrangementSelection;
    const auto* lane = laneAt(y);
    return timelineArea().contains(x, y) && lane && lane->takeLaneId < 0 && lane->controlIndex < ctx().numTracks()
        && range.active && range.end > range.start && range.firstTrack >= 0 && range.lastTrack < ctx().numTracks()
        && lane->controlIndex >= range.firstTrack && lane->controlIndex <= range.lastTrack && xToBeat(x) >= range.start && xToBeat(x) < range.end;
}

void ArrangementView::mouseMove(const MouseEvent& e)
{
    reportPointer(e);
    tooltip = waveformZoomBounds().contains(e.x, e.y) ? "Waveform Vertical Zoom Level: drag/scroll; double-click to type; right-click for x/dB" : "";
    model::ClipLocation location;
    float x0 = 0, x1 = 0;
    const auto* clip = clipAt(e.x, e.y, location, x0, x1);
    const int uid = clip != nullptr && location.kind != model::ClipLocation::Kind::Take ? clip->uid : -1;
    const int track = uid >= 0 ? location.track : -1;
    if (uid != hoverUid_ || track != hoverTrack_)
    {
        hoverUid_ = uid;
        hoverTrack_ = track;
        repaint();
    }
}

void ArrangementView::mouseDown(const MouseEvent& e)
{
    auto& x = ctx();
    const auto& m = x.theme.metrics;
    if (waveformZoomBounds().contains(e.x, e.y))
    {
        if (e.right) { showWaveformZoomMenu(); return; }
        if (e.cmd) { editingWaveformZoom_ = false; x.waveformZoom = 1.0f; repaint(); return; }
        if (e.clicks >= 2)
        {
            char text[24];
            std::snprintf(text, sizeof(text), "%.2f", waveformDecibels_ ? 20.0 * std::log10(x.waveformZoom) : x.waveformZoom);
            waveformZoomText_ = text; editingWaveformZoom_ = waveformTextSelected_ = true; repaint(); return;
        }
        editingWaveformZoom_ = false;
        x.waveformZoomEnabled = true;
        draggingWaveformZoom_ = true; waveformGrabY_ = e.y; waveformBefore_ = x.waveformZoom;
        return;
    }
    if (editingWaveformZoom_) commitWaveformZoom();
    x.clipEditorFocused = false;
    model::ClipLocation edgeLocation; float edgeX0 = 0, edgeX1 = 0;
    const auto* edgeClip = clipAt(e.x, e.y, edgeLocation, edgeX0, edgeX1);
    const bool clipEdge = edgeClip && ui::hitZone(e.x - edgeX0, edgeX1 - edgeX0) != ui::DragMode::Move;
    laneResize_ = e.right || clipEdge || (e.alt && edgeClip) ? -1 : laneEdgeAt(e.x, e.y);
    if (laneResize_ >= 0)
    {
        heightOptimized_ = false;
        laneGrabY_ = e.y;
        laneStartHeight_ = 0;
        for (const auto& lane : lanes_)
            if (lane.controlIndex == laneResize_ && lane.takeLaneId < 0) laneStartHeight_ = lane.h;
        return;
    }
    draggingRuler_ = false;
    drag_ = {};
    regionDrag_ = {};
    loopDrag_ = LoopDrag::None;
    selectingTime_ = false;
    const auto* hitLane = laneAt(e.y);
    if (e.x >= timelineArea().w)
    {
        if (hitLane && hitLane->takeLaneId >= 0 && x.song && x.applyEdit)
        {
            const int track = hitLane->controlIndex, id = hitLane->takeLaneId;
            x.selectedTrack = x.selectedClipTrack = track; x.selectedTakeLane = id;
            x.selectedClipUid = x.selectedClipScene = -1; takeRange_ = {};
            const auto* take = model::findTakeLane(*x.song, track, id);
            if (e.x >= width() - 25 && e.y < hitLane->y + 23)
                x.applyEdit(model::auditionTakeLane(*x.song, track, take->audition ? -1 : id));
            else if (e.x >= width() - 65 && e.y < hitLane->y + 23)
                x.applyEdit(model::moveTakeLane(*x.song, track, id, e.x < width() - 44 ? -1 : 1));
            else if (e.clicks >= 2) { renaming_ = renameSelection_ = true; renameTrack_ = track; renameLane_ = id; renameText_ = take->name; }
            repaint();
        }
        return;
    }
    renaming_ = false;
    if (e.right)
    {
        showContextMenu(e);
        return;
    }

    if (e.y < m.arrangementRulerHeight && overviewRect().reduced(-1).contains(e.x, e.y))
    {
        draggingOverview_ = true;
        scrollToOverview(e.x);
        return;
    }
    if (e.y < m.arrangementRulerHeight)
    {
        // The loop brace occupies the bottom strip of the ruler: drag its body to move it, its ends to resize.
        const double ls = x.transport ? x.transport->loopStart() : 0.0, ll = x.transport ? x.transport->loopLength() : 4.0;
        if (const auto zone = loopZoneAt(e.x, e.y); zone != LoopDrag::None)
        {
            loopDrag_ = zone;
            loopStartBefore_ = ls;
            loopLengthBefore_ = ll;
            loopGrabBeat_ = xToBeat(e.x);
            x.arrangementSelection = {true, ls, ls + ll, 0, x.numTracks() - 1};
        }
        else
        {
            // The scrub area: click to play from that position, drag to move it (like Live).
            draggingRuler_ = true;
            scrub(e, true);
        }
        repaint();
        return;
    }

    // Shift extends from the first clicked clip, including both endpoints and all time/tracks between them.
    if (e.shift && e.clicks < 2 && x.song && hitLane && hitLane->takeLaneId < 0)
    {
        model::ClipLocation target;
        float left = 0, right = 0;
        // (Shift on an audio clip's title-bar edge stretches it instead, UI-45.)
        if (const auto* last = clipAt(e.x, e.y, target, left, right); last && target.kind == model::ClipLocation::Kind::Arrangement
            && (last->kind != model::Clip::Kind::Audio || clipEdgeAt(hitLane, e.x, e.y, left, right) == ui::DragMode::Move))
        {
            auto anchor = selectionAnchorSong_ == x.song ? selectionAnchor_ : x.selectedClipLocation();
            const auto* first = anchor.kind == model::ClipLocation::Kind::Arrangement ? model::findClip(*x.song, anchor) : nullptr;
            if (first)
            {
                selectionAnchor_ = anchor; selectionAnchorSong_ = x.song;
                x.arrangementSelection = {true, std::min(first->currentStart, last->currentStart), std::max(first->currentEnd, last->currentEnd),
                                          std::min(anchor.track, target.track), std::max(anchor.track, target.track)};
                x.selectedTrack = x.selectedClipTrack = target.track; x.selectedClipUid = last->uid;
                x.selectedTakeLane = x.selectedClipScene = x.selectedAutomationTrack = -1;
                x.sessionTrackSelection = false; takeRange_ = {};
                repaint(); return;
            }
        }
    }

    if (! e.shift && e.clicks < 2 && x.song && x.applyEdit && inTimeSelection(e.x, e.y))
    {
        model::ClipLocation hit;
        float left = 0, right = 0;
        ui::DragMode mode;
        const auto* clip = clipAt(e.x, e.y, hit, left, right);
        if (! clip || ! fadeHandleAt(*clip, hit, left, right, e.x, e.y, mode))
        {
            if (x.recording || x.recordingRequested) { x.statusText = "Finish recording before moving the selection"; return; }
            regionDrag_.active = regionDrag_.valid = true; regionDrag_.copy = e.alt;
            regionDrag_.original = x.arrangementSelection; regionDrag_.song = x.song; regionDrag_.structure = x.trackStructure();
            regionDrag_.at = regionDrag_.original.start; regionDrag_.grabBeat = xToBeat(e.x);
            regionDrag_.startX = e.x; regionDrag_.startY = e.y; regionDrag_.grabTrack = hitLane->controlIndex;
            for (int track = regionDrag_.original.firstTrack; track <= regionDrag_.original.lastTrack; ++track)
                for (const auto& selected : x.song->tracks[(size_t) track].arrangementClips)
                    if (selected.currentStart < regionDrag_.original.end && selected.currentEnd > regionDrag_.original.start)
                        regionDrag_.clips.emplace_back(track, selected);
            x.selectedAutomationTrack = -1; x.sessionTrackSelection = false;
            takeRange_ = {};
            repaint(); return;
        }
    }
    x.selectedAutomationTrack = -1;
    x.sessionTrackSelection = false;
    x.arrangementSelection = {};
    x.arrangementCursor = std::max(0.0, ui::snapToGrid(xToBeat(e.x), snapUnit(e)));
    model::ClipLocation location;
    float x0 = 0, x1 = 0;
    if (const auto* clip = clipAt(e.x, e.y, location, x0, x1))
    {
        const int track = location.track;
        selectionAnchor_ = location; selectionAnchorSong_ = x.song;
        x.selectedTakeLane = location.laneId;
        x.selectedTrack = track;
        x.selectedClipTrack = track;
        x.selectedClipScene = -1;
        x.selectedClipUid = clip->uid;
        if (e.clicks >= 2) // double-click opens the clip in the detail view
        {
            ++x.clipZoomRequest;
            x.detailTab = DetailTab::Clip;
            x.detailVisible = true;
        }
        if (location.kind == model::ClipLocation::Kind::Take && e.clicks < 2 && hitLane && e.y >= hitLane->y + 13)
        {
            const double beat = std::clamp(ui::snapToGrid(xToBeat(e.x), snapUnit(e)), clip->currentStart, clip->currentEnd);
            takeRange_ = {true, true, track, location.laneId, beat, beat, beat};
            x.statusText = "Select a take range, then Enter to comp; T auditions the lane";
            repaint(); return;
        }
        takeRange_ = {};

        ui::DragMode fadeMode;
        const bool fadeHandle = fadeHandleAt(*clip, location, x0, x1, e.x, e.y, fadeMode);
        // UI-45 (Live): edges resize, trim or (Shift/Alt, audio) stretch only from the clip's title bar; below it the
        // whole body, edges included, selects time.
        const auto edge = clipEdgeAt(hitLane, e.x, e.y, x0, x1);
        const bool stretch = edge != ui::DragMode::Move && (e.shift || e.alt) && clip->kind == model::Clip::Kind::Audio
                             && location.kind != model::ClipLocation::Kind::Take;
        if (hitLane && hitLane->takeLaneId < 0 && e.clicks < 2 && ! e.alt && ! fadeHandle && ! stretch
            && (e.shift || e.y >= hitLane->y + 13))
        {
            selectionGrab_ = std::max(0.0, ui::snapToGrid(xToBeat(e.x), snapUnit(e)));
            selectionTrack_ = hitLane->controlIndex;
            selectingTime_ = true;
            repaint(); return;
        }
        drag_.laneId = location.laneId;
        drag_.active = true;
        drag_.track = drag_.targetTrack = track;
        drag_.uid = clip->uid;
        drag_.original = drag_.preview = *clip;
        drag_.mode = edge;
        if (fadeHandle)
            drag_.mode = fadeMode; // the fade handles sit at the top corners of the waveform
        else if (stretch)
            drag_.mode = edge == ui::DragMode::TrimStart ? ui::DragMode::StretchStart : ui::DragMode::StretchEnd;
        drag_.copy = e.alt && drag_.mode == ui::DragMode::Move && location.kind != model::ClipLocation::Kind::Take; // Opt+drag copies
        drag_.startX = e.x;
        drag_.startY = e.y;
    }
    else
    {
        for (const auto& lane : lanes_)
            if (e.y >= lane.y && e.y < lane.y + lane.h)
                x.selectTrack(lane.controlIndex, e.shift, false); // Shift+click in a lane extends the track selection
        x.selectedClipTrack = x.selectedClipUid = x.selectedTakeLane = -1;
        takeRange_ = {};

        if (hitLane && hitLane->takeLaneId < 0 && hitLane->controlIndex < x.numTracks() && e.clicks < 2)
        {
            selectionGrab_ = std::max(0.0, ui::snapToGrid(xToBeat(e.x), snapUnit(e)));
            selectionTrack_ = hitLane->controlIndex;
            selectingTime_ = true;
        }

        // A click on empty lane space moves the insert marker (the play start) when stopped, like Live.
        if (x.transport != nullptr && ! x.transport->isPlaying() && e.clicks < 2)
            x.transport->setPosition(std::max(0.0, ui::snapToGrid(xToBeat(e.x), snapUnit(e))));

        // Double-click on an empty MIDI lane creates a one-bar clip at the grid position left of the click.
        const int lane = laneTrackAt(e.y);
        if (e.clicks >= 2 && lane >= 0 && x.song != nullptr && x.applyEdit
            && x.song->tracks[(size_t) lane].kind == model::TrackKind::Midi)
        {
            const double g = std::min(gridBeats(), beatsPerBar());
            insertMidiClip(lane, std::max(0.0, std::floor(xToBeat(e.x) / g + 1e-9) * g));
        }
    }
    repaint();
}

std::optional<TrackDropTarget> ArrangementView::trackDropTarget(const MouseEvent& e) const
{
    const auto& x = ctx();
    const float headerX = timelineArea().right();
    if (! x.song || e.x < headerX || e.x >= width() || e.y < x.theme.metrics.arrangementRulerHeight) return {};
    const auto* lane = laneAt(e.y);
    if (! lane || lane->takeLaneId >= 0 || lane->controlIndex >= x.numTracks()) return {};
    const int index = lane->controlIndex;
    const auto& track = x.song->tracks[(size_t) index];
    const float fraction = (e.y - lane->y) / lane->h;
    if (track.kind == model::TrackKind::Group && fraction > 0.25f && fraction < 0.75f)
        return TrackDropTarget{index + 1, track.id, {headerX + 2, lane->y + 2, width() - headerX - 4, lane->h - 4}};
    int before = index + (fraction >= 0.5f ? 1 : 0);
    if (fraction >= 0.5f && track.kind == model::TrackKind::Group)
        for (; before < x.numTracks(); ++before)
        {
            auto parent = x.song->tracks[(size_t) before].groupId;
            int depth = x.numTracks();
            while (parent >= 0 && parent != track.id && --depth > 0)
            {
                const auto found = std::find_if(x.song->tracks.begin(), x.song->tracks.end(), [parent](const auto& t) { return t.id == parent; });
                if (found == x.song->tracks.end()) break;
                parent = found->groupId;
            }
            if (parent != track.id) break;
        }
    float edge = before == index ? lane->y : lane->y + lane->h;
    if (before > index) for (const auto& visible : lanes_)
        if (visible.controlIndex >= index && visible.controlIndex < before) edge = std::max(edge, visible.y + visible.h);
    return TrackDropTarget{before, track.groupId, {headerX, edge - 1, width() - headerX, 2}};
}

void ArrangementView::insertMidiClip(int track, double beat)
{
    auto& x = ctx();
    if (x.song == nullptr || ! x.applyEdit || track < 0 || (size_t) track >= x.song->tracks.size())
        return;
    double length = beatsPerBar();
    for (const auto& other : x.song->tracks[(size_t) track].arrangementClips) // do not run into the next clip
        if (other.currentStart > beat + 1e-9)
            length = std::min(length, other.currentStart - beat);
    model::Clip fresh;
    if (length < 0.0625)
        x.statusText = "No room for a MIDI clip here";
    else if (model::makeEmptyMidiClip(*x.song, beat, length, fresh))
    {
        fresh.colorIndex = x.song->tracks[(size_t) track].colorIndex;
        x.applyEdit(model::insertClip(track, fresh, "Insert MIDI Clip"));
    }
    else
        x.statusText = "Cannot create a MIDI clip: this set has no MIDI clip to copy the format from";
}

void ArrangementView::scrub(const MouseEvent& e, bool start)
{
    auto& x = ctx();
    if (x.transport == nullptr)
        return;
    x.arrangementCursor = std::max(0.0, ui::snapToGrid(xToBeat(e.x), snapUnit(e)));
    x.transport->setPosition(x.arrangementCursor);
    if (start && x.song != nullptr && ! x.transport->isPlaying() && ! x.transport->isCountingIn())
        x.transport->play();
    repaint();
}

void ArrangementView::zoomTimeSelection(bool back)
{
    auto& x = ctx();
    if (back)
    {
        if (! selectionZoomed_) return;
        x.pixelsPerBeat = selectionZoomBefore_; x.scrollBeats = selectionScrollBefore_; selectionZoomed_ = false;
    }
    else
    {
        double start = x.arrangementSelection.start, end = x.arrangementSelection.end;
        if (! x.arrangementSelection.active || end <= start) return;
        if (! selectionZoomed_) { selectionZoomBefore_ = x.pixelsPerBeat; selectionScrollBefore_ = x.scrollBeats; }
        x.scrollBeats = start; x.pixelsPerBeat = std::clamp((double) timelineArea().w / (end - start), 0.1, 2000.0);
        selectionZoomed_ = true;
    }
    layout(); repaint();
}

std::vector<MenuAction> ArrangementView::gridContextActions(const MouseEvent& e)
{
    auto& x = ctx();
    const auto* song = x.song;
    const int lane = e.y >= x.theme.metrics.arrangementRulerHeight ? laneTrackAt(e.y) : -1;
    const auto* track = x.trackAt(lane);
    const auto id = track ? track->id : -1;
    const auto range = x.arrangementSelection;
    const bool selected = range.active && range.end > range.start && lane >= range.firstTrack && lane <= range.lastTrack;
    const bool editable = song && x.applyEdit && ! x.recording && ! x.recordingRequested;
    auto valid = [this, song, id, lane] {
        return Widget::isAlive(this) && ctx().song == song && ctx().trackAt(lane) && ctx().trackAt(lane)->id == id
               && ! ctx().recording && ! ctx().recordingRequested;
    };
    const bool insert = editable && track && track->kind == model::TrackKind::Midi;
    const double grid = std::min(gridBeats(), beatsPerBar());
    const double beat = selected ? range.start : std::max(0.0, std::floor(xToBeat(e.x) / grid + 1e-9) * grid);
    std::vector<MenuAction> result{
        {selectionZoomed_ ? "Zoom Back from Time Selection" : "Zoom to Time Selection",
         [this] { if (Widget::isAlive(this)) zoomTimeSelection(selectionZoomed_); }, selectionZoomed_ || selected, selectionZoomed_ ? "X" : "Z"},
        {"", {}, false},
    };
    if (track && track->kind == model::TrackKind::Midi)
        result.push_back({"Insert Empty MIDI Clip", [this, valid, lane, beat, selected, range] {
            if (! valid() || ! ctx().applyEdit) return;
            if (selected)
            {
                model::Clip clip;
                if (model::makeEmptyMidiClip(*ctx().song, range.start, range.end - range.start, clip))
                {
                    clip.colorIndex = ctx().song->tracks[(size_t) lane].colorIndex;
                    ctx().applyEdit(model::insertClip(lane, std::move(clip), "Insert MIDI Clip"));
                }
            }
            else insertMidiClip(lane, beat);
        }, insert, "⇧⌘M"});
    result.push_back({"", {}, false});
    const bool loopOn = song && song->loopOn && (! selected || (std::abs(song->loopStart - range.start) < 1e-9 && std::abs(song->loopLength - (range.end - range.start)) < 1e-9));
    result.push_back({loopOn ? "Deactivate Loop" : "Activate Loop", [this, valid, range, selected, loopOn] {
        if (! valid() || ! ctx().applyEdit) return;
        const auto& current = *ctx().song;
        ctx().applyEdit(model::setLoop(current, ! loopOn, selected ? range.start : current.loopStart,
                                      selected ? range.end - range.start : current.loopLength));
        repaint();
    }, editable && track, "⌘L"});
    result.push_back({"", {}, false});
    if (track && (track->kind == model::TrackKind::Audio || track->kind == model::TrackKind::Midi))
        result.push_back({track->frozen ? "Unfreeze Track" : "Freeze Track", [this, valid, lane] {
            if (valid() && ctx().renderTrack) ctx().renderTrack(lane, true, false);
        }, editable && static_cast<bool>(x.renderTrack), "⌥⇧⌘F"});
    result.push_back({track && track->kind == model::TrackKind::Group ? "Bounce Group in Place" : "Bounce Track in Place", [this, valid, lane] {
        if (valid() && ctx().renderTrack) ctx().renderTrack(lane, false, false);
    }, editable && track && static_cast<bool>(x.renderTrack)});
    result.push_back({"Bounce to New Track", [this, valid, lane] {
        if (valid() && ctx().renderTrack) ctx().renderTrack(lane, false, true);
    }, editable && track && static_cast<bool>(x.renderTrack), "⌘B"});
    result.push_back({"Consolidate", [this, valid] {
        if (valid() && ctx().consolidateSelection) ctx().consolidateSelection();
    }, editable && selected && static_cast<bool>(x.consolidateSelection), "⌘J"});
    result.push_back({"Paste Bounced Audio", {}, false, "⌥⌘V"});
    return result;
}

void ArrangementView::showContextMenu(const MouseEvent& e)
{
    auto& x = ctx();
    const auto& range = x.arrangementSelection;
    const int lane = laneTrackAt(e.y);
    const bool inRange = range.active && range.end > range.start && lane >= range.firstTrack && lane <= range.lastTrack
                         && xToBeat(e.x) >= range.start && xToBeat(e.x) < range.end;
    model::ClipLocation location; float x0 = 0, x1 = 0;
    if (! inRange && x.song)
        if (const auto* clip = clipAt(e.x, e.y, location, x0, x1))
        {
            x.selectedTrack = location.track; x.selectedClipTrack = location.track; x.selectedClipUid = clip->uid;
            x.selectedClipScene = location.scene; x.selectedTakeLane = location.laneId;
            x.arrangementSelection = {}; x.arrangementCursor = std::max(0.0, ui::snapToGrid(xToBeat(e.x), snapUnit(e)));
            showClipContextMenu(x, location); repaint(); return;
        }
    ui::showGridMenu(x.arrangementGrid, beatsPerBar(), [this] {
        if (! Widget::isAlive(this)) return;
        if (ctx().layoutChanged) ctx().layoutChanged(); repaint();
    }, gridContextActions(e), x.uiZoom);
}

void ArrangementView::scrollToOverview(float px)
{
    auto& x = ctx();
    const auto r = overviewRect();
    const double beat = (px - r.x) / r.w * overviewBeats();
    x.scrollBeats = std::max(0.0, beat - timelineArea().w / x.pixelsPerBeat * 0.5);
    layout();
    repaint();
}

void ArrangementView::mouseDrag(const MouseEvent& e)
{
    reportPointer(e);
    if (draggingWaveformZoom_)
    {
        ctx().waveformZoom = std::clamp(waveformBefore_ * (float) std::exp((waveformGrabY_ - e.y) * 0.025f), 0.01f, 100.0f);
        repaint(); return;
    }
    auto& x = ctx();
    if (regionDrag_.active)
    {
        if (regionDrag_.song != x.song || regionDrag_.structure != x.trackStructure()) { regionDrag_ = {}; repaint(); return; }
        if (std::abs(e.x - regionDrag_.startX) > 3 || std::abs(e.y - regionDrag_.startY) > 3) regionDrag_.moved = true;
        if (! regionDrag_.moved) return;
        const double unit = ui::snapSize(x.arrangementGrid, x.pixelsPerBeat, beatsPerBar(), e.cmd || (! regionDrag_.copy && e.alt));
        regionDrag_.at = std::max(0.0, ui::snapToGrid(regionDrag_.original.start + xToBeat(e.x) - regionDrag_.grabBeat, unit));
        const auto* lane = laneAt(e.y);
        regionDrag_.trackOffset = lane ? lane->controlIndex - regionDrag_.grabTrack : 0;
        regionDrag_.valid = lane && lane->takeLaneId < 0 && timelineArea().contains(e.x, e.y)
            && model::canTransferArrangementRegion(*x.song, regionDrag_.original.firstTrack, regionDrag_.original.lastTrack,
                regionDrag_.original.start, regionDrag_.original.end, regionDrag_.at, regionDrag_.trackOffset);
        x.statusText = regionDrag_.valid ? (regionDrag_.copy ? "Copy selection" : "Move selection")
                                        : "Selection needs compatible tracks and automation targets";
        repaint(); return;
    }
    if (laneResize_ >= 0)
    {
        if (x.laneHeights.size() < (size_t) x.numControlTracks()) x.laneHeights.resize((size_t) x.numControlTracks(), 0.0f);
        x.laneHeights[(size_t) laneResize_] = std::clamp(laneStartHeight_ + (e.y - laneGrabY_), 18.0f, 420.0f);
        layout();
        repaint();
        return;
    }
    if (draggingOverview_)
    {
        scrollToOverview(e.x);
        return;
    }
    if (draggingRuler_ && x.transport)
    {
        scrub(e, false);
        return;
    }
    if (selectingTime_)
    {
        const double beat = std::max(0.0, ui::snapToGrid(xToBeat(e.x), snapUnit(e)));
        const auto* lane = laneAt(e.y);
        const int lastTrack = x.numTracks() - 1;
        if (lastTrack < 0) { selectingTime_ = false; return; }
        // Overshooting the last channel must keep all touched tracks selected, including when
        // the pointer ends in blank canvas or a return/Main lane.
        const int track = lane ? std::clamp(lane->controlIndex, 0, lastTrack)
                               : e.y < timelineArea().y ? 0 : lastTrack;
        x.arrangementSelection = {std::abs(beat - selectionGrab_) > 1e-9, std::min(beat, selectionGrab_), std::max(beat, selectionGrab_),
                                  std::min(track, selectionTrack_), std::max(track, selectionTrack_)};
        repaint(); return;
    }
    if (loopDrag_ != LoopDrag::None && x.transport != nullptr)
    {
        if (x.recording) { x.statusText = "Finish recording before changing the punch/loop region"; return; }
        const double grid = snapUnit(e) > 0.0 ? std::max(1.0, snapUnit(e)) : 0.0;
        const double delta = xToBeat(e.x) - loopGrabBeat_;
        double start = loopStartBefore_, length = loopLengthBefore_;
        if (loopDrag_ == LoopDrag::Move)
            start = std::max(0.0, ui::snapToGrid(loopStartBefore_ + delta, grid));
        else if (loopDrag_ == LoopDrag::Left)
        {
            const double end = loopStartBefore_ + loopLengthBefore_;
            start = std::clamp(ui::snapToGrid(loopStartBefore_ + delta, grid), 0.0, end - 1.0);
            length = end - start;
        }
        else
            length = std::max(1.0, ui::snapToGrid(loopStartBefore_ + loopLengthBefore_ + delta, grid) - loopStartBefore_);
        x.transport->setLoop(x.transport->loopEnabled(), start, length);
        repaint();
        return;
    }
    if (takeRange_.selecting && x.song)
    {
        const double beat = std::max(0.0, ui::snapToGrid(xToBeat(e.x), snapUnit(e)));
        takeRange_.start = std::min(beat, takeRange_.grab); takeRange_.end = std::max(beat, takeRange_.grab);
        repaint(); return;
    }
    if (! drag_.active)
        return;

    if (std::abs(e.x - drag_.startX) > 3 || std::abs(e.y - drag_.startY) > 3)
        drag_.moved = true;
    if (! drag_.moved)
        return;
    const double deltaBeats = (e.x - drag_.startX) / x.pixelsPerBeat;
    // Option means "copy" for a clip body drag, so only Cmd skips the grid there
    const double unit = drag_.copy ? ui::snapSize(x.arrangementGrid, x.pixelsPerBeat, beatsPerBar(), e.cmd) : snapUnit(e);
    drag_.preview = ui::dragClip(drag_.original, drag_.mode, deltaBeats, unit, unit > 0.0, x.song ? x.song->tempo : 120.0);

    if (drag_.mode == ui::DragMode::FadeInCurve || drag_.mode == ui::DragMode::FadeOutCurve)
    {
        auto& shape = drag_.mode == ui::DragMode::FadeInCurve ? drag_.preview.fadeInCurve : drag_.preview.fadeOutCurve;
        const double before = drag_.mode == ui::DragMode::FadeInCurve ? drag_.original.fadeInCurve : drag_.original.fadeOutCurve;
        shape = std::clamp(before + (e.y - drag_.startY) * 0.012, -0.95, 0.95);
    }
    // Moving can also change the track, if the lane under the mouse takes the same kind of clip.
    drag_.targetTrack = drag_.track;
    if (drag_.laneId < 0 && drag_.mode == ui::DragMode::Move && x.song != nullptr)
    {
        const int lane = laneTrackAt(e.y);
        if (lane >= 0 && x.song->tracks[(size_t) lane].kind == x.song->tracks[(size_t) drag_.track].kind
            && x.song->tracks[(size_t) lane].kind != model::TrackKind::Group)
            drag_.targetTrack = lane;
        else if (lane >= 0 && lane != drag_.track)
            // The clip stays on its track; say why instead of ignoring the move silently.
            x.statusText = x.song->tracks[(size_t) drag_.track].kind == model::TrackKind::Audio
                               ? "Audio clips can only move to audio tracks"
                               : "MIDI clips can only move to MIDI tracks";
    }
    repaint();
}

void ArrangementView::mouseUp(const MouseEvent& e)
{
    draggingWaveformZoom_ = false;
    selectingTime_ = false;
    draggingOverview_ = false;
    if (laneResize_ >= 0)
    {
        laneResize_ = -1;
        return;
    }
    takeRange_.selecting = false;
    auto& x = ctx();
    if (regionDrag_.active)
    {
        mouseDrag(e); // validate the release position even when no final drag event arrived
        auto region = std::move(regionDrag_); regionDrag_ = {};
        if (region.moved && region.valid && region.song == x.song && region.structure == x.trackStructure()
            && ! x.recording && ! x.recordingRequested && x.applyEdit)
        {
            auto command = model::transferArrangementRegion(*x.song, region.original.firstTrack, region.original.lastTrack,
                region.original.start, region.original.end, region.at, region.trackOffset, region.copy);
            if (command.apply)
            {
                x.applyEdit(std::move(command));
                x.arrangementSelection = {true, region.at, region.at + region.original.end - region.original.start,
                    region.original.firstTrack + region.trackOffset, region.original.lastTrack + region.trackOffset};
                x.selectedTrack = region.grabTrack + region.trackOffset; x.trackSelection.clear();
                x.selectedClipTrack = x.selectedClipUid = x.selectedClipScene = x.selectedTakeLane = -1;
                x.statusText = region.copy ? "Copied selection" : "Moved selection";
            }
        }
        repaint(); return;
    }
    if (loopDrag_ != LoopDrag::None && x.transport != nullptr && x.song != nullptr && x.applyEdit)
    {
        x.applyEdit(model::setLoop(*x.song, x.transport->loopEnabled(), x.transport->loopStart(), x.transport->loopLength()));
        loopDrag_ = LoopDrag::None;
        return;
    }
    if (drag_.active && drag_.moved && x.song != nullptr && x.applyEdit)
    {
        const char* label = drag_.mode == ui::DragMode::Move ? "Move Clip" : drag_.mode == ui::DragMode::ResizeEnd ? "Resize Clip"
                          : drag_.mode == ui::DragMode::TrimStart ? "Trim Clip"
                          : (drag_.mode == ui::DragMode::StretchStart || drag_.mode == ui::DragMode::StretchEnd) ? "Stretch Clip" : "Change Fade";
        if (drag_.copy)
        {
            // Option+drag: copy the clip to the dropped position, on this track or another one of the same kind
            x.applyEdit(model::copyClipToTrack(*x.song, drag_.track, drag_.uid, drag_.targetTrack, drag_.preview.currentStart));
            for (const auto& c : x.song->tracks[(size_t) drag_.targetTrack].arrangementClips)
                if (c.uid != drag_.uid && std::fabs(c.currentStart - drag_.preview.currentStart) < 1e-9)
                {
                    x.selectedTrack = x.selectedClipTrack = drag_.targetTrack;
                    x.selectedClipUid = c.uid;
                    x.selectedClipScene = -1;
                }
            x.statusText = "Copied clip";
        }
        else if (drag_.targetTrack != drag_.track)
        {
            x.applyEdit(model::moveClipToTrack(*x.song, drag_.track, drag_.uid, drag_.targetTrack, drag_.preview.currentStart));
            x.selectedClipTrack = x.selectedClipUid = -1; // the clip on the new track is a new object
        }
        else
        {
            x.applyEdit(model::modifyClip(*x.song, drag_.laneId >= 0 ? model::ClipLocation::take(drag_.track, drag_.uid, drag_.laneId)
                                                                       : model::ClipLocation::arrangement(drag_.track, drag_.uid), drag_.preview, label));
        }
    }
    drag_ = {};
    repaint();
}

bool ArrangementView::keyPressed(const KeyEvent& k)
{
    auto& x = ctx();
    if (regionDrag_.active && k.keyCode == juce::KeyPress::escapeKey)
    { regionDrag_ = {}; x.statusText = "Move cancelled"; repaint(); return true; }
    if (editingWaveformZoom_)
    {
        if (k.keyCode == juce::KeyPress::escapeKey) editingWaveformZoom_ = false;
        else if (k.keyCode == juce::KeyPress::returnKey) commitWaveformZoom();
        else if (k.cmd && (k.textChar == 'a' || k.textChar == 'A')) waveformTextSelected_ = true;
        else if (k.keyCode == juce::KeyPress::backspaceKey || k.keyCode == juce::KeyPress::deleteKey)
        { if (waveformTextSelected_) waveformZoomText_.clear(); else paint::popLastCodePoint(waveformZoomText_); waveformTextSelected_ = false; }
        else if (! k.cmd && ! k.alt && (k.textChar == '.' || k.textChar == '-' || k.textChar == '+' || (k.textChar >= '0' && k.textChar <= '9')))
        {
            if (waveformTextSelected_) waveformZoomText_.clear();
            waveformZoomText_ += (char) k.textChar; waveformTextSelected_ = false;
        }
        repaint(); return true;
    }
    if (renaming_)
    {
        if (k.keyCode == juce::KeyPress::escapeKey) { renaming_ = false; repaint(); return true; }
        if (k.keyCode == juce::KeyPress::returnKey)
        {
            renaming_ = false;
            if (x.song && x.applyEdit && model::findTakeLane(*x.song, renameTrack_, renameLane_))
                x.applyEdit(model::renameTakeLane(*x.song, renameTrack_, renameLane_, renameText_));
            repaint(); return true;
        }
        if (k.cmd && (k.textChar == 'a' || k.textChar == 'A')) renameSelection_ = true;
        else if (k.cmd && (k.textChar == 'v' || k.textChar == 'V'))
        { if (renameSelection_) renameText_.clear(); renameText_ += juce::SystemClipboard::getTextFromClipboard().toStdString(); renameSelection_ = false; }
        else if (k.keyCode == juce::KeyPress::backspaceKey || k.keyCode == juce::KeyPress::deleteKey)
        { if (renameSelection_) renameText_.clear(); else paint::popLastCodePoint(renameText_); renameSelection_ = false; }
        else if (k.textChar >= 32 && ! k.cmd && ! k.alt)
        { if (renameSelection_) renameText_.clear(); renameText_ += juce::String::charToString((juce::juce_wchar) k.textChar).toStdString(); renameSelection_ = false; }
        repaint(); return true;
    }
    if (! k.cmd && k.alt && ! k.ctrl && (k.keyCode == '+' || k.keyCode == '=' || k.keyCode == '-' || k.textChar == '+' || k.textChar == '-' || k.textChar == '='))
    {
        for (const auto& lane : lanes_) if (lane.controlIndex == x.selectedTrack && lane.takeLaneId < 0)
        { zoomTrackHeight(k.keyCode == '-' || k.textChar == '-' ? 0.8 : 1.25, lane.y + lane.h * 0.5f); break; }
        return true;
    }
    if (k.alt && ! k.cmd && ! k.ctrl && ! k.shift && (k.keyCode == juce::KeyPress::leftKey || k.keyCode == juce::KeyPress::rightKey)
        && x.jumpToTransient && visible)
    {
        x.jumpToTransient(k.keyCode == juce::KeyPress::rightKey ? 1 : -1); // TRK-3
        repaint();
        return true;
    }
    if (! k.cmd && ! k.alt && ! k.ctrl && ! ctx().computerKeyboard && visible)
    {
        if (k.textChar == 'h' || k.textChar == 'H') { optimizeHeight(); return true; }
        if (k.textChar == 'w' || k.textChar == 'W') { optimizeWidth(); return true; }
    }
    if (k.cmd && ! k.alt && ! k.shift && (k.textChar == 'l' || k.textChar == 'L') && takeRange_.active && takeRange_.end > takeRange_.start
        && x.song != nullptr && x.applyEdit)
    {
        x.applyEdit(model::setLoop(*x.song, true, takeRange_.start, takeRange_.end - takeRange_.start)); // Loop Selection
        repaint();
        return true;
    }
    if (k.cmd && ! k.alt && ! k.shift && k.textChar >= '1' && k.textChar <= '5' && ui::handleGridShortcut(x.arrangementGrid, (int) k.textChar))
    {
        if (x.layoutChanged) x.layoutChanged();
        repaint();
        return true;
    }
    if (! k.cmd && ! k.alt && ! k.ctrl && ! k.shift && (k.textChar == 'z' || k.textChar == 'Z' || k.textChar == 'x' || k.textChar == 'X'))
    { zoomTimeSelection(k.textChar == 'x' || k.textChar == 'X'); return true; }
    if (! k.cmd && ! k.alt && ! k.ctrl && (k.textChar == '+' || k.textChar == '=' || k.textChar == '-' || k.textChar == '_'))
    {
        // + / - zoom around the play position (or the centre of the view when it is off screen), like Live.
        const double pos = x.transport ? x.transport->positionBeats() : 0.0;
        float anchor = beatToX(pos);
        if (anchor < 0 || anchor > timelineArea().w)
            anchor = timelineArea().w * 0.5f;
        zoomAt(anchor, k.textChar == '+' || k.textChar == '=' ? 1.25 : 0.8);
        return true;
    }
    if (k.cmd && k.alt && (k.textChar == 'u' || k.textChar == 'U') && x.song && x.selectedTrack >= 0 && x.selectedTrack < x.numTracks())
    {
        auto& tracks = x.unfoldedTakeTracks;
        const auto found = std::find(tracks.begin(), tracks.end(), x.selectedTrack);
        if (found == tracks.end()) { tracks.push_back(x.selectedTrack); x.automationVisible = false; }
        else { tracks.erase(found); takeRange_ = {}; }
        layout(); repaint(); return true;
    }
    if (x.song && x.applyEdit && x.selectedTakeLane >= 0 && x.selectedTrack >= 0
        && x.selectedTrack == x.selectedClipTrack && model::findTakeLane(*x.song, x.selectedTrack, x.selectedTakeLane))
    {
        const int track = x.selectedTrack, id = x.selectedTakeLane;
        const auto* take = model::findTakeLane(*x.song, track, id);
        if (! k.cmd && ! k.alt && (k.textChar == 't' || k.textChar == 'T'))
        { x.applyEdit(model::auditionTakeLane(*x.song, track, take->audition ? -1 : id)); repaint(); return true; }
        if (k.keyCode == juce::KeyPress::returnKey && takeRange_.active && takeRange_.end > takeRange_.start)
        { x.applyEdit(model::compTakeRange(*x.song, takeRange_.track, takeRange_.laneId, takeRange_.start, takeRange_.end)); repaint(); return true; }
        if (k.cmd && (k.textChar == 'r' || k.textChar == 'R'))
        { renaming_ = renameSelection_ = true; renameTrack_ = track; renameLane_ = id; renameText_ = take->name; repaint(); return true; }
        if (x.selectedClipUid < 0)
        {
            if (k.cmd && (k.textChar == 'd' || k.textChar == 'D'))
            { x.applyEdit(model::duplicateTakeLane(*x.song, track, id)); layout(); repaint(); return true; }
            if (k.keyCode == juce::KeyPress::deleteKey || k.keyCode == juce::KeyPress::backspaceKey)
            { x.applyEdit(model::deleteTakeLane(*x.song, track, id)); takeRange_ = {}; x.selectedTakeLane = -1; layout(); repaint(); return true; }
            if (k.cmd && (k.keyCode == juce::KeyPress::upKey || k.keyCode == juce::KeyPress::downKey))
            { x.statusText = "Cycling comp takes is not implemented"; return true; }
        }
    }
    if (! k.cmd && ! k.alt && (k.textChar == 'a' || k.textChar == 'A'))
    {
        x.toggleAutomationLane(x.selectedTrack);
        if (x.automationVisible) takeRange_ = {};
        layout();
        repaint();
        return true;
    }
    if (x.song == nullptr || ! x.applyEdit || ! visible)
        return false;
    const bool hasSelection = x.selectedClipScene < 0 && x.selectedClip() != nullptr;

    if (k.cmd && ! k.shift && ! k.alt && (k.textChar == 'd' || k.textChar == 'D')
        && x.arrangementSelection.active && x.arrangementSelection.end > x.arrangementSelection.start)
    {
        auto& range = x.arrangementSelection;
        const double span = range.end - range.start;
        x.applyEdit(model::duplicateArrangementRegion(*x.song, range.firstTrack, range.lastTrack, range.start, range.end));
        range.start = range.end; range.end += span;
        x.selectedClipTrack = x.selectedClipUid = x.selectedTakeLane = -1;
        x.statusText = "Duplicated time selection";
        repaint(); return true;
    }

    if (hasSelection && ! k.cmd && ! k.alt && ! k.ctrl && ! k.shift && (k.keyCode == juce::KeyPress::deleteKey || k.keyCode == juce::KeyPress::backspaceKey))
    {
        x.applyEdit(model::deleteClip(*x.song, x.selectedClipLocation()));
        return true;
    }
    if (hasSelection && k.cmd && (k.textChar == 'd' || k.textChar == 'D'))
    {
        const int track = x.selectedClipTrack;
        const int uid = x.selectedClipUid;
        const double at = x.selectedClip()->currentEnd;
        x.applyEdit(model::duplicateClip(*x.song, x.selectedClipLocation()));
        // Select the copy (the clip that now starts where the original ended and is not the original).
        for (const auto& c : x.selectedTakeLane >= 0 ? model::findTakeLane(*x.song, track, x.selectedTakeLane)->clips
                                                   : x.song->tracks[(size_t) track].arrangementClips)
            if (c.uid != uid && std::abs(c.currentStart - at) < 1e-9)
                x.selectedClipUid = c.uid;
        repaint();
        return true;
    }
    if (k.cmd && k.shift && (k.textChar == 'm' || k.textChar == 'M'))
    {
        const int lane = x.selectedTrack;
        if (lane >= 0 && lane < x.numTracks() && x.song->tracks[(size_t) lane].kind == model::TrackKind::Midi)
        {
            model::Clip fresh;
            const double bar = x.song->timeSigNumerator * 4.0 / x.song->timeSigDenominator;
            const double at = x.transport ? std::floor(x.transport->positionBeats() / bar) * bar : 0.0;
            if (model::makeEmptyMidiClip(*x.song, at, bar, fresh))
            {
                fresh.colorIndex = x.song->tracks[(size_t) lane].colorIndex;
                x.applyEdit(model::insertClip(lane, fresh, "Insert MIDI Clip"));
            }
            else
                x.statusText = "Cannot create a MIDI clip: this set has no MIDI clip to copy the format from";
            return true;
        }
    }
    return false;
}

bool ArrangementView::mouseWheel(const MouseEvent& e)
{
    auto& x = ctx();
    if (waveformZoomBounds().contains(e.x, e.y))
    {
        x.waveformZoomEnabled = true;
        x.waveformZoom = std::clamp(x.waveformZoom * (float) std::exp(e.wheelDeltaY * 1.5f), 0.01f, 100.0f);
        repaint(); return true;
    }
    if (e.cmd && ! e.alt)
    {
        // Cmd + wheel: horizontal zoom around the pointer (scroll up zooms in), as in Live.
        const float delta = std::fabs(e.wheelDeltaY) >= std::fabs(e.wheelDeltaX) ? e.wheelDeltaY : e.wheelDeltaX;
        zoomAt(std::min(e.x, timelineArea().w), wheelZoomFactor(delta));
        return true;
    }
    if (e.alt && ! e.cmd && timelineArea().contains(e.x, e.y) && std::abs(e.wheelDeltaY) > 1e-8f)
    { zoomTrackHeight(wheelZoomFactor(e.wheelDeltaY), e.y); return true; }
    // Shift + wheel scrolls horizontally; the wheel scrolls the tracks.
    const bool horizontal = e.shift;
    const float dx = horizontal ? (std::fabs(e.wheelDeltaX) > std::fabs(e.wheelDeltaY) ? e.wheelDeltaX : e.wheelDeltaY) : e.wheelDeltaX;
    const float dy = horizontal ? 0.0f : e.wheelDeltaY;
    x.scrollBeats = std::max(0.0, x.scrollBeats - dx * kWheelPixels / x.pixelsPerBeat);
    float total = 0;
    for (const auto& l : lanes_)
        total += l.h;
    x.scrollTracksY = std::clamp(x.scrollTracksY - dy * kWheelPixels, 0.0f, std::max(0.0f, total - timelineArea().h + 40));
    layout();
    repaint();
    return true;
}

} // namespace sonora::ui
