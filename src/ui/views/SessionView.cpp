#include "ui/views/SessionView.h"
#include "ui/CollabOverlay.h"

#include <map>
#include "core/DeviceCatalog.h"

#include "ui/WheelScale.h"

#include "ui/TrackMenu.h"
#include "midi/MappingStore.h"
#include "midi/MidiManager.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include "ui/views/InputControls.h"

#include "engine/ClipLauncher.h"
#include "engine/Transport.h"

#include "engine/EngineGraph.h"
#include "ui/Controls.h"
#include "ui/Painter.h"
#include "ui/Waveform.h"
#include "ui/UiContext.h"
#include "ui/widgets/Buttons.h"
#include "ui/widgets/Fader.h"
#include "ui/widgets/Knob.h"

#include <cmath>
#include <cstdio>

namespace sonora::ui
{

namespace
{

std::string formatPan(float v)
{
    const int p = (int) std::lround(v * 50.0f);
    if (p == 0)
        return "C";
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%d%c", std::abs(p), p < 0 ? 'L' : 'R');
    return buf;
}

std::string formatSendDb(float g)
{
    if (g <= 0.000317f)
        return "-inf";
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%.1f", 20.0 * std::log10(g));
    return buf;
}

// Vertical layout of the mixer part of a column (measured from a running Live at 100 % zoom).
// Local y == 0 is the top of the I/O section (or of the first visible section).
struct MixerLayout
{
    static constexpr float kSep = 2.0f;
    static constexpr float kIoHeight = InputControls::sessionHeight;
    static constexpr float kVolumeBase = 120.0f; // the Live default the control offsets below were measured at
    static constexpr float kColourStrip = 7.0f;
    static constexpr float kStopRow = 19.0f;

    bool io = false;
    int sends = 0;
    float ioTop = 0, sendsTop = 0, sendsHeight = 0, volumeTop = 0, volumeHeight = kVolumeBase, stripTop = 0, total = 0;

    explicit MixerLayout(const UiContext& ctx)
    {
        io = ctx.ioVisible;
        sends = ctx.mixerVisible && ctx.sendsVisible ? std::min(ctx.numReturns(), engine::TrackControls::kMaxSends) : 0;
        float y = 0;
        if (io)
        {
            ioTop = 0;
            y += kIoHeight + kSep;
        }
        if (sends > 0)
        {
            sendsTop = y;
            sendsHeight = sends == 1 ? 40.0f : 55.0f + 15.5f * (float) (sends - 2);
            y += sendsHeight + kSep;
        }
        volumeTop = y;
        volumeHeight = ctx.mixerVisible ? std::clamp(ctx.theme.metrics.sessionVolumeHeight, 100.0f, 520.0f) : 0.0f;
        y += volumeHeight + kSep;
        stripTop = y;
        total = y + kColourStrip;
    }
};

// Column contents of one mixer strip (one per track / return / main).
class MixerStrip : public Widget
{
public:
    MixerStrip(int controlIndex, int numSends, bool isMain, bool isReturn, bool isGroup)
        : index_(controlIndex), isMain_(isMain), isReturn_(isReturn), isGroup_(isGroup)
    {
        for (int s = 0; s < numSends && ! isMain && ! isReturn; ++s)
        {
            Knob::Source src;
            src.get = [this, s] { auto* c = controls(); return c ? c->sends[(size_t) s].load() : 0.0f; };
            src.set = [this, s](float v) { if (auto* c = controls()) c->sends[(size_t) s].store(v); };
            src.minValue = 0.0f;
            src.maxValue = 1.0f;
            src.format = formatSendDb;
            const char name[2] = {(char) ('A' + s), 0};
            auto* k = add<Knob>(name, src);
            k->style = Knob::Style::Poti;
            k->showValue = false;
            k->midiTarget = midi::ControlTarget{midi::ControlTarget::Kind::TrackSend, controlIndex, 0, s, {}};
            sends_.push_back(k);
        }

        Knob::Source pan;
        pan.get = [this] { auto* c = controls(); return c ? c->pan.load() : 0.0f; };
        pan.set = [this](float v) { if (auto* c = controls()) c->pan.store(v); };
        pan.minValue = -1;
        pan.maxValue = 1;
        pan.bipolar = true;
        pan.format = formatPan;
        pan_ = add<Knob>("", pan);
        pan_->style = Knob::Style::Poti;
        pan_->showLabel = false;
        pan_->showValue = false;
        pan_->midiTarget = midi::ControlTarget{midi::ControlTarget::Kind::TrackPan, controlIndex, 0, 0, {}};

        fader_ = add<Fader>();
        fader_->midiTarget = midi::ControlTarget{midi::ControlTarget::Kind::TrackVolume, controlIndex, 0, 0, {}};
        fader_->getGain = [this] { auto* c = controls(); return c ? c->volume.load() : 1.0f; };
        fader_->setGain = [this](float g) { if (auto* c = controls()) c->volume.store(g); };
        fader_->getPeakL = [this] { auto* c = controls(); return c ? c->peakL.load() : 0.0f; };
        fader_->getPeakR = [this] { auto* c = controls(); return c ? c->peakR.load() : 0.0f; };

        if (! isMain)
        {
            activator_ = add<TextButton>(isReturn ? std::string(1, (char) ('A' + 0)) : std::to_string(controlIndex + 1),
                                         TextButton::Style::Activator);
            activator_->midiTarget = midi::ControlTarget{midi::ControlTarget::Kind::TrackActivator, controlIndex, 0, 0, {}};
            activator_->isOn = [this] { auto* c = controls(); return c ? c->active.load() : true; };
            activator_->onClick = [this] { if (auto* c = controls()) c->active.store(! c->active.load()); };

            solo_ = add<TextButton>("S", TextButton::Style::Solo);
            solo_->midiTarget = midi::ControlTarget{midi::ControlTarget::Kind::TrackSolo, controlIndex, 0, 0, {}};
            solo_->isOn = [this] { auto* c = controls(); return c && c->solo.load(); };
            solo_->onClickWithModifiers = [this](const MouseEvent& event) { toggleTrackSolo(ctx(), index_, event.cmd); };
        }
        if (! isMain && ! isReturn && ! isGroup)
        {
            input_ = add<InputControls>(controlIndex, true);
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

    void layout() override
    {
        const MixerLayout L(ctx());
        const bool audio = ! midiOnly();
        if (input_) input_->visible = L.io;
        if (input_ && L.io)
            input_->setBounds({0, L.ioTop, width(), MixerLayout::kIoHeight});
        for (size_t i = 0; i < sends_.size(); ++i)
        {
            const float cx = (i % 2 == 0 ? 29.0f : 61.5f) + columnShift();
            const float cy = L.sendsTop + 19.0f + 15.5f * (float) i;
            sends_[i]->visible = audio && ctx().mixerVisible && ctx().sendsVisible;
            sends_[i]->setBounds({cx - 13, cy - 13, 26, 26});
        }
        pan_->visible = fader_->visible = audio && ctx().mixerVisible;
        if (activator_) activator_->visible = solo_->visible = ctx().mixerVisible;
        if (arm_) arm_->visible = ctx().mixerVisible;
        const float v = L.volumeTop, extra = L.volumeHeight - MixerLayout::kVolumeBase, dx = columnShift();
        pan_->setBounds({14 + dx, v + 30.5f, 22, 22});
        fader_->setBounds({46 + dx, v + 6.5f, 46, 107.5f + extra});
        if (activator_)
        {
            activator_->setBounds({13.5f + dx, v + 57.5f + extra, 22.5f, 22.5f});
            solo_->setBounds({13.5f + dx, v + 84 + extra, 22.5f, 13});
            if (arm_)
                arm_->setBounds({13.5f + dx, v + 101.5f + extra, 22.5f, 13});
        }
    }

    // Wider columns keep their controls centred (Live's strips behave the same way).
    float columnShift() const { return std::max(0.0f, (width() - 95.0f) * 0.5f); }
    bool midiOnly() const { const auto* track = ctx().trackAt(index_); return track && model::isMidiOnly(*track); }

    void tick() override
    {
        if (auto* c = controls())
        {
            const auto activity = c->midiActivity.load(std::memory_order_relaxed);
            if (activity != lastMidiActivity_) { lastMidiActivity_ = activity; midiTicks_ = 5; repaint(); }
            else if (midiTicks_ > 0 && --midiTicks_ == 0) repaint();
            const float p = std::max(c->peakL.load(), c->peakR.load());
            if (p > peakHold_)
            {
                peakHold_ = p;
                repaint();
            }
        }
    }

    void paint(SkCanvas& c, const Theme& t) override
    {
        const MixerLayout L(ctx());
        const bool selected = ctx().isTrackSelected(index_);
        const SkColor bg = selected ? t.color("SurfaceHighlight") : t.color("SurfaceBackground");
        const SkColor line = t.color("SurfaceArea");
        const float w = width();
        paint::fill(c, localBounds(), bg);

        // I/O section for returns / groups / main (tracks draw theirs through InputControls).
        if (L.io && (isReturn_ || isGroup_ || isMain_))
        {
            if (isMain_)
            {
                ctl::caption(c, t, {5, L.ioTop + 56, w - 10, 10}, "Main Out");
                ctl::dropdown(c, t, {5, L.ioTop + 67, w - 10, 13}, "1/2");
            }
            else
            {
                ctl::caption(c, t, {5, L.ioTop + 69, w - 10, 10}, "Audio To");
                ctl::dropdown(c, t, {5, L.ioTop + 82, w - 10, 13}, "Main");
                ctl::emptyBox(c, t, {5, L.ioTop + 98, w - 10, 13});
            }
        }
        if (L.sends > 0 && ! isReturn_ && ! isMain_ && ! midiOnly())
            ctl::caption(c, t, {w * 0.5f + 2, L.sendsTop + 5, 44, 11}, "Sends", Align::Left);

        // Volume section: peak readout, pan marker.
        const float v = L.volumeTop;
        if (ctx().mixerVisible && midiOnly())
            ctl::midiMeter(c, t, {58 + columnShift(), v + 7, 10, L.volumeHeight - 14}, midiTicks_ > 0);
        if (ctx().mixerVisible && ! midiOnly())
        {
            const Rect pill{8 + columnShift(), v + 6.5f, 33.5f, 12};
            const bool clip = peakHold_ >= 1.0f;
            paint::fillRounded(c, pill, 6, clip ? t.color("ChosenRecord") : t.color("ControlTextBack"));
            char buf[16];
            if (peakHold_ <= 0.000317f)
                std::snprintf(buf, sizeof(buf), "-\xE2\x88\x9E");
            else
                std::snprintf(buf, sizeof(buf), "%.1f", 20.0 * std::log10(peakHold_));
            paint::text(c, buf, pill, t.color("ControlForeground"), t.metrics.smallFontSize, Align::Centre);
            ctl::triangle(c, 25 + columnShift(), v + 26, 6, 4, ctl::Dir::Down, t.color("ControlForeground"));
        }

        // Section separators and the colour strip.
        auto sep = [&](float y) { paint::fill(c, {0, y, w, MixerLayout::kSep}, line); };
        if (L.io)
            sep(MixerLayout::kIoHeight);
        if (L.sends > 0)
            sep(L.sendsTop + L.sendsHeight);
        sep(L.stripTop - MixerLayout::kSep);
        if (const auto* track = ctx().trackAt(index_))
            paint::fill(c, {0, L.stripTop, w, MixerLayout::kColourStrip}, Theme::paletteColor(track->colorIndex));
        paint::fill(c, {w - 2, 0, 2, height()}, line);
    }

    // Width resizing: tracks grow from their right edge, returns / Main from their left edge.
    bool inResizeZone(float x) const { return isReturn_ || isMain_ ? x < 4.0f : x >= width() - 5.0f; }
    Cursor cursorAt(float x, float) const override { return inResizeZone(x) ? Cursor::ResizeHorizontal : Cursor::Default; }

    void mouseDrag(const MouseEvent& e) override
    {
        if (! resizing_)
            return;
        const float delta = (e.x - grabX_) * (isReturn_ || isMain_ ? -1.0f : 1.0f);
        auto& widths = ctx().sessionColumnWidths;
        if (widths.size() < (size_t) ctx().numControlTracks()) widths.resize((size_t) ctx().numControlTracks(), 0.0f);
        widths[(size_t) index_] = std::clamp(startWidth_ + delta, 92.0f, 400.0f);
        if (auto* view = parent())
        {
            view->layout();
            view->repaint();
        }
    }

    void mouseUp(const MouseEvent&) override
    {
        if (resizing_ && ctx().layoutChanged) ctx().layoutChanged();
        resizing_ = false;
    }

    void mouseDown(const MouseEvent& e) override
    {
        if (inResizeZone(e.x))
        {
            resizing_ = true;
            grabX_ = e.x;
            startWidth_ = width();
            return;
        }
        const MixerLayout L(ctx());
        if (Rect{8 + columnShift(), L.volumeTop + 6.5f, 33.5f, 12}.contains(e.x, e.y))
            peakHold_ = 0.0f;
        ctx().selectTrack(index_, false, false);
        repaint();
    }

private:
    engine::TrackControls* controls() const
    {
        auto& bank = ctx().controls;
        return bank && (size_t) index_ < bank->size() ? &(*bank)[(size_t) index_] : nullptr;
    }

    int index_;
    bool isMain_, isReturn_, isGroup_;
    float peakHold_ = 0.0f;
    uint32_t lastMidiActivity_ = 0;
    int midiTicks_ = 0;
    bool resizing_ = false;
    float grabX_ = 0, startWidth_ = 0;
    std::vector<Knob*> sends_;
    Knob* pan_ = nullptr;
    Fader* fader_ = nullptr;
    TextButton* activator_ = nullptr;
    TextButton* solo_ = nullptr;
    TextButton* arm_ = nullptr;
    InputControls* input_ = nullptr;
};
} // namespace

// ---- geometry: tracks scroll in a viewport on the left; returns and Main stay anchored at the right edge.

bool SessionView::columnVisible(int column) const
{
    return column >= 0 && ((size_t) column >= visibleColumns_.size() || visibleColumns_[(size_t) column]);
}

bool SessionView::descendantOf(int track, int group) const
{
    const auto& x = ctx();
    if (! x.song || track < 0 || group < 0 || track >= x.numTracks() || group >= x.numTracks()) return false;
    const auto id = x.song->tracks[(size_t) group].id;
    auto parent = x.song->tracks[(size_t) track].groupId;
    // The bound also tolerates malformed imported parent cycles.
    for (int depth = 0; parent >= 0 && depth < x.numTracks(); ++depth)
    {
        if (parent == id) return true;
        const auto found = std::find_if(x.song->tracks.begin(), x.song->tracks.end(), [parent](const auto& t) { return t.id == parent; });
        if (found == x.song->tracks.end()) break;
        parent = found->groupId;
    }
    return false;
}

uint64_t SessionView::foldSignature() const
{
    uint64_t signature = 1469598103934665603ull;
    if (ctx().song)
        for (const auto& t : ctx().song->tracks)
            signature = (signature ^ (t.folded ? 1ull : 0ull) ^ (t.hidden ? 2ull : 0ull)) * 1099511628211ull;
    if (ctx().song)
        for (const auto& t : ctx().song->returns)
            signature = (signature ^ (t.hidden ? 2ull : 0ull)) * 1099511628211ull;
    return (signature ^ (ctx().showHiddenTracks ? 4ull : 0ull)) * 1099511628211ull;
}

void SessionView::updateVisibleColumns()
{
    visibleColumns_.assign((size_t) numColumns(), true);
    if (ctx().song)
        for (int group = 0; group < ctx().numTracks(); ++group)
            if (ctx().song->tracks[(size_t) group].kind == model::TrackKind::Group && ctx().song->tracks[(size_t) group].folded)
                for (int member = 0; member < ctx().numTracks(); ++member)
                    if (member != group && descendantOf(member, group)) visibleColumns_[(size_t) member] = false;
    for (int column = 0; column < numColumns(); ++column)
        if (trackHiddenInViews(ctx(), column)) visibleColumns_[(size_t) column] = false;
    builtFold_ = foldSignature();
}

Rect SessionView::groupToggleBounds(int column) const
{
    const auto* track = ctx().trackAt(column);
    if (! track || track->kind != model::TrackKind::Group || ! columnVisible(column)) return {};
    return {columnX(column) + columnWidth(column) - 17, ctx().theme.metrics.sessionHeaderHeight - 15, 12, 12};
}

void SessionView::stopGroup(int column)
{
    auto& x = ctx();
    if (! x.stopTrack || ! x.song) return;
    // Snapshot the targets before callbacks: a stop may finish a recording and rebuild the view.
    std::vector<int> members;
    for (int t = 0; t < x.numTracks(); ++t)
        if (x.song->tracks[(size_t) t].kind != model::TrackKind::Group && descendantOf(t, column)) members.push_back(t);
    const auto stop = x.stopTrack;
    for (int member : members) stop(member);
}

float SessionView::columnWidth(int column) const
{
    const auto& widths = ctx().sessionColumnWidths;
    if (column >= 0 && (size_t) column < widths.size() && widths[(size_t) column] > 0.0f)
        return widths[(size_t) column];
    const auto& m = ctx().theme.metrics;
    return column == ctx().numTracks() + ctx().numReturns() ? m.sceneColumnWidth : m.sessionTrackWidth;
}

float SessionView::widthOf(int from, int to) const
{
    float total = 0;
    for (int c = from; c < to; ++c)
        if (columnVisible(c)) total += columnWidth(c);
    return total;
}

float SessionView::mainWidth() const
{
    return columnWidth(ctx().numTracks() + ctx().numReturns());
}

float SessionView::tracksViewport() const
{
    const int nTracks = ctx().numTracks(), nReturns = ctx().numReturns();
    return std::max(ctx().theme.metrics.sessionTrackWidth, width() - mainWidth() - widthOf(nTracks, nTracks + nReturns));
}

int SessionView::numColumns() const
{
    return ctx().numControlTracks();
}

float SessionView::columnX(int column) const
{
    const int nTracks = ctx().numTracks(), nReturns = ctx().numReturns();
    if (column < nTracks)
        return widthOf(0, column) - scrollX_;
    if (column == nTracks + nReturns)
        return width() - mainWidth();
    return width() - mainWidth() - widthOf(column, nTracks + nReturns);
}

int SessionView::columnAt(float x) const
{
    const int nTracks = ctx().numTracks(), nReturns = ctx().numReturns();
    if (x >= width() - mainWidth())
        return nTracks + nReturns;
    for (int c = nTracks; c < nTracks + nReturns; ++c)
        if (x >= columnX(c) && x < columnX(c) + columnWidth(c))
            return c;
    if (x < tracksViewport())
        for (int c = 0; c < nTracks; ++c)
            if (columnVisible(c) && x >= columnX(c) && x < columnX(c) + columnWidth(c))
                return c;
    return -1;
}

void SessionView::rebuild()
{
    removeAllChildren();
    mixerStrips_.clear();
    visibleColumns_.clear();
    builtFor_ = ctx().song;
    builtStructure_ = ctx().trackStructure();
    if (builtFor_ == nullptr)
        return;

    const int nTracks = ctx().numTracks();
    const int nReturns = ctx().numReturns();
    for (int i = 0; i < numColumns(); ++i)
    {
        const bool isReturn = i >= nTracks && i < nTracks + nReturns;
        const bool isMain = i == nTracks + nReturns;
        auto* strip = add<MixerStrip>(i, std::min(nReturns, engine::TrackControls::kMaxSends), isMain, isReturn, ctx().trackAt(i)->kind == model::TrackKind::Group);
        if (isReturn)
            strip->setActivatorLabel(std::string(1, (char) ('A' + (i - nTracks))));
        mixerStrips_.push_back(strip);
    }
}

float SessionView::mixerHeight() const
{
    return std::min(MixerLayout(ctx()).total, height() * 0.7f);
}

// Slot grid ends above the stop-clip row and the mixer.
float SessionView::gridBottom() const
{
    return height() - mixerHeight() - MixerLayout::kStopRow - MixerLayout::kSep;
}

Rect SessionView::stopRowBounds(int column) const
{
    if (! columnVisible(column)) return {};
    return {columnX(column), gridBottom() + MixerLayout::kSep, columnWidth(column) - 2, MixerLayout::kStopRow - 1};
}

void SessionView::layout()
{
    if (builtFor_ != ctx().song || builtStructure_ != ctx().trackStructure())
        rebuild();
    updateVisibleColumns();
    scrollX_ = std::clamp(scrollX_, 0.0f, std::max(0.0f, widthOf(0, ctx().numTracks()) - tracksViewport()));
    const float mh = mixerHeight();
    for (size_t i = 0; i < mixerStrips_.size(); ++i)
    {
        mixerStrips_[i]->visible = columnVisible((int) i);
        if (mixerStrips_[i]->visible)
            mixerStrips_[i]->setBounds({columnX((int) i), height() - mh, columnWidth((int) i), mh});
    }
}

void SessionView::tick()
{
    auto& x = ctx();
    if (builtFor_ != x.song || builtStructure_ != x.trackStructure() || builtFold_ != foldSignature())
    {
        layout();
        repaint();
        return;
    }
    if (x.recording && x.sessionRecordingPhase && x.sessionRecordingPhase(-1) != 0) repaint();
    // Animate while a clip is playing (progress bar) or queued (blinking button).
    if (x.graph != nullptr && x.controls != nullptr)
        for (size_t i = 0; i < x.controls->size() && i < x.graph->nodes.size(); ++i)
        {
            const auto& c = (*x.controls)[i];
            if (c.playingSlot.load(std::memory_order_relaxed) != engine::TrackControls::kNoSlot
                || c.queuedSlot.load(std::memory_order_relaxed) != engine::TrackControls::kNothingQueued)
            {
                repaint();
                break;
            }
        }
}

// COL-5: a collaborator looking at the Session view marks the column under their pointer.
void SessionView::paintOverChildren(SkCanvas& c, const Theme& t)
{
    auto& x = ctx();
    std::map<int, int> stacked; // tags per column, stacked upwards from the bottom of the slot grid
    for (const auto& cursor : x.collab.cursors)
    {
        if (cursor.view != 1 || cursor.trackId < 0) continue;
        for (int column = 0; column < x.numControlTracks(); ++column)
            if (const auto* track = x.trackAt(column); track && track->id == cursor.trackId && columnVisible(column))
            {
                const float x0 = columnX(column), w = columnWidth(column);
                paint::fill(c, {x0, 0, w, 3}, collab::colour(cursor));
                paint::stroke(c, Rect{x0, 0, w, gridBottom()}.reduced(0.5f), paint::withAlpha(collab::colour(cursor), 110));
                const int below = stacked[column]++;
                collab::tag(c, x0 + 3, gridBottom() - (t.metrics.fontSize + 8.0f) * (float) (below + 1), cursor, t.metrics.fontSize);
            }
    }
}

void SessionView::mouseMove(const MouseEvent& e)
{
    auto& x = ctx();
    if (x.collabPointer && x.collab.active()) x.collabPointer(1, columnAt(e.x), -1.0, -1);
}

void SessionView::mouseExit()
{
    auto& x = ctx();
    if (x.collabPointer && x.collab.active()) x.collabPointer(1, -1, -1.0, -1);
}

void SessionView::paint(SkCanvas& c, const Theme& t)
{
    const auto& m = t.metrics;
    const SkColor bg = t.color("SurfaceBackground");
    const SkColor line = t.color("SurfaceArea");
    const SkColor rowLine = line; // full-contrast Session slot dividers, matching the column rules
    paint::fill(c, localBounds(), bg);
    auto& x = ctx();
    if (x.song == nullptr)
    {
        paint::text(c, "Open a Live Set (Cmd+O) or double-click one in the browser", localBounds(),
                    t.color("TextDisabled"), m.fontSize + 2, Align::Centre);
        return;
    }

    const auto clipMappings = (x.midiMapMode || x.keyMapMode) && x.midi ? x.midi->mappings() : std::vector<midi::Mapping>{};
    const int nTracks = x.numTracks();
    const int nReturns = x.numReturns();
    const int nScenes = (int) x.song->scenes.size();
    const float gridBottom = this->gridBottom();
    const float stopTop = gridBottom + MixerLayout::kSep;
    const SkColor green = t.color("ChosenPlay");

    // The empty area between the last track and the returns holds the drop hint on the selected scene row.
    {
        const float gapX = std::min(widthOf(0, nTracks) - scrollX_, tracksViewport());
        const float gapW = width() - mainWidth() - widthOf(nTracks, nTracks + nReturns) - gapX;
        if (gapW > 160 && nScenes > 0 && x.selectedSessionScene >= 0 && x.selectedSessionScene < nScenes)
        {
            const float ry = m.sessionHeaderHeight + 1 + (float) x.selectedSessionScene * m.sessionSlotHeight - scrollY_;
            const Rect row{gapX, ry, gapW, m.sessionSlotHeight - 1};
            paint::fill(c, row, t.color("SurfaceHighlight"));
            paint::text(c, "Drop Files and Devices Here", row, t.color("ControlForeground"), m.fontSize, Align::Centre);
        }
    }

    // The first return column needs its own left divider (the last track's divider lies in the gap before it).
    if (nReturns > 0)
        paint::fill(c, {columnX(nTracks) - 2, 0, 2, height()}, line);
    // Main stays pinned even with no returns; its divider must span the mixer too.
    paint::fill(c, {columnX(nTracks + nReturns) - 2, 0, 2, height()}, line);

    for (int col = 0; col < numColumns(); ++col)
    {
        if (! columnVisible(col)) continue;
        const float cx = columnX(col);
        const float cw = columnWidth(col);
        const bool isMain = col == nTracks + nReturns;
        const bool isReturn = col >= nTracks && ! isMain;
        if (cx > width() || cx + cw < 0 || (col < nTracks && cx + cw > tracksViewport() + 0.5f && false))
            continue;
        c.save();
        if (col < nTracks)
            c.clipRect(SkRect::MakeXYWH(0, 0, tracksViewport(), height()));
        const auto* track = x.trackAt(col);
        const SkColor trackColor = track->deactivated ? paint::mix(Theme::paletteColor(track->colorIndex), SkColorSetRGB(0x9a, 0x9a, 0x9a), 0.8f)
                                                      : Theme::paletteColor(track->colorIndex);

        if (track->kind == model::TrackKind::Group)
            paint::fill(c, {cx, m.sessionHeaderHeight, cw - 2, gridBottom - m.sessionHeaderHeight}, t.color("SurfaceHighlight"));

        // Group members retain a narrow parent-colour rail above and to the left of their own title.
        Rect title{cx, 0, cw - 2, m.sessionHeaderHeight};
        if (col < nTracks && track->groupId >= 0)
        {
            const auto parent = std::find_if(x.song->tracks.begin(), x.song->tracks.end(),
                                            [track](const auto& t0) { return t0.id == track->groupId; });
            if (parent != x.song->tracks.end())
            {
                const int parentIndex = (int) (parent - x.song->tracks.begin());
                const auto frame = x.sessionTrackSelection && x.isTrackSelected(parentIndex)
                    ? t.color("SelectionBackground") : Theme::paletteColor(parent->colorIndex);
                paint::fill(c, title, frame);
                title.x += 3; title.w -= 3; title.y += 6; title.h -= 6;
                paint::fill(c, {title.x - 1, title.y - 1, title.w + 1, 1}, line);
                paint::fill(c, {title.x - 1, title.y, 1, title.h}, line);
            }
        }
        const bool selected = x.sessionTrackSelection && x.isTrackSelected(col);
        const auto titleColor = selected ? t.color("SelectionBackground") : trackColor;
        paint::fill(c, title, titleColor);
        Rect titleLabel{title.x + 5, m.sessionHeaderHeight - 18, title.w - 10, 18};
        const auto toggle = groupToggleBounds(col);
        if (! toggle.isEmpty()) titleLabel.w = std::max(0.0f, toggle.x - titleLabel.x - 3);
        const auto titleInk = selected ? t.color("SelectionForeground") : paint::contrastText(trackColor);
        paint::text(c, (track->hidden ? "(hidden) " : "") + x.trackDisplayName(col), titleLabel, titleInk, m.fontSize, Align::Left, true);
        if (! toggle.isEmpty())
        {
            SkPaint pen; pen.setAntiAlias(true); pen.setColor(titleInk);
            pen.setStyle(SkPaint::kStroke_Style); pen.setStrokeWidth(0.8f);
            c.drawCircle(toggle.centreX(), toggle.centreY(), 5.8f, pen);
            for (int row = 0; row < 3; ++row)
            {
                const float offset = track->folded ? 0.0f : (float) row;
                c.drawLine(toggle.x + 3 + offset * 0.5f, toggle.y + 4 + (float) row * 2,
                           toggle.x + 8 + offset * 0.5f, toggle.y + 4 + (float) row * 2, pen);
            }
        }
        paint::fill(c, {cx, m.sessionHeaderHeight, cw - 2, 1}, line);

        // Slots cannot paint over the pinned title when vertically scrolling.
        c.save();
        c.clipRect(SkRect::MakeXYWH(cx, m.sessionHeaderHeight + 1, cw - 2, std::max(0.0f, gridBottom - m.sessionHeaderHeight - 1)));
        float y = m.sessionHeaderHeight + 1 - scrollY_;
        for (int s = 0; s < nScenes; ++s, y += m.sessionSlotHeight)
        {
            if (y + m.sessionSlotHeight < m.sessionHeaderHeight || y > gridBottom)
                continue;
            Rect slot{cx, y, cw - 2, m.sessionSlotHeight - 1};
            const bool sceneSelected = s == x.selectedSessionScene;

            if (isMain)
            {
                // Scene launcher: grey play triangle plus the scene name (or its number).
                if (sceneSelected) paint::fill(c, slot, t.color("SurfaceHighlight"));
                paint::playTriangle(c, {slot.x + 6, slot.centreY() - 4, 7, 8}, t.color("ClipSlotButton"));
                const auto& name = x.song->scenes[(size_t) s].name;
                paint::text(c, name.empty() ? std::to_string(s + 1) : name, Rect{slot.x + 24, slot.y, slot.w - 28, slot.h},
                            t.color("ControlForeground"), m.fontSize);
                paint::fill(c, {cx, slot.bottom(), cw - 2, 1}, rowLine);
                continue;
            }

            if (sceneSelected && ! isReturn)
                paint::fill(c, slot, t.color(track->kind == model::TrackKind::Group && x.isTrackSelected(col)
                                                ? "StandbySelectionBackground" : "SurfaceHighlight"));
            paint::fill(c, {cx, slot.bottom(), cw - 2, 1}, rowLine);
            const model::Clip* clip = nullptr;
            if (! isReturn && (size_t) s < track->slots.size() && track->slots[(size_t) s].clip)
                clip = &*track->slots[(size_t) s].clip;

            if (clip != nullptr)
            {
                const auto state = x.graph != nullptr ? engine::slotState(*x.graph, col, s) : engine::SlotState::Stopped;
                const bool blink = (juce::Time::getMillisecondCounter() / 250) % 2 == 0;
                const SkColor cc = Theme::paletteColor(clip->colorIndex);
                const bool playing = state == engine::SlotState::Playing || state == engine::SlotState::QueuedStop;
                paint::fill(c, slot, cc);
                // Launch button cell: black with a green triangle while playing (blinking while queued).
                const Rect cell{slot.x, slot.y, 17, slot.h};
                if (playing || state == engine::SlotState::Queued)
                    paint::fill(c, cell, SkColorSetRGB(0x0d, 0x0d, 0x0d));
                paint::fill(c, {slot.x + 17, slot.y, 1, slot.h}, paint::withAlpha(SK_ColorBLACK, 40));
                const Rect btn{slot.x + 5, slot.centreY() - 4, 7, 8};
                if (state == engine::SlotState::Playing)
                    paint::playTriangle(c, btn, green);
                else if (state == engine::SlotState::Queued)
                    paint::playTriangle(c, btn, blink ? green : t.color("ControlDisabled"));
                else if (state == engine::SlotState::QueuedStop)
                    paint::stopSquare(c, btn, blink ? green : t.color("ControlDisabled"));
                else
                    paint::playTriangle(c, btn, t.color("ClipSlotButton"));
                if (clip->kind == model::Clip::Kind::Audio && x.peaks)
                    if (auto peaks = x.peaks(*clip))
                    {
                        auto shown = *clip;
                        shown.currentStart = 0;
                        const double length = std::max(0.001, shown.currentEnd);
                        const Rect wave{slot.x + 18, slot.y + 2, slot.w - 20, slot.h - 4};
                        paintWaveform(c, *peaks, shown, wave, wave.x, (float) (wave.w / length),
                                      x.song->tempo, wave.x, wave.right(), paint::withAlpha(paint::contrastText(cc), 80));
                    }
                const float linkW = clip->linkGroup >= 0 ? 15.0f : 0.0f; // TRK-5
                if (linkW > 0) paint::linkGlyph(c, {slot.x + 19, slot.y, 14, slot.h}, paint::contrastText(cc));
                paint::text(c, clip->name, Rect{slot.x + 20 + linkW, slot.y, slot.w - 22 - linkW, slot.h}, paint::contrastText(cc), m.fontSize);

                if (playing && x.transport != nullptr && x.graph != nullptr)
                {
                    // Progress along the clip loop (approximation for unwarped audio clips, whose loop is in seconds).
                    const double lenRaw = clip->loop.enabled ? clip->loop.loopEnd - clip->loop.loopStart : clip->currentEnd - clip->currentStart;
                    const double len = clip->kind == model::Clip::Kind::Audio && ! clip->warped ? lenRaw * x.transport->tempo() / 60.0 : lenRaw;
                    const double elapsed = (*x.graph->controls)[(size_t) col].sessionElapsed.load();
                    if (len > 0.01)
                    {
                        const double ph = std::fmod(std::max(0.0, elapsed), len) / len;
                        paint::fill(c, {slot.x + 18, slot.bottom() - 3, (slot.w - 19) * (float) ph, 2}, green);
                    }
                }
                if ((x.midiMapMode || x.keyMapMode) && x.midi)
                    if (const auto target = midi::sessionClipTarget(*x.song, col, s))
                    {
                        const bool mapSelected = x.mapSelection && *x.mapSelection == *target;
                        const auto colour = t.color("ChosenAlternative");
                        const auto overlay = paint::mix(cc, colour, mapSelected ? 0.85f : 0.65f);
                        paint::fill(c, slot, overlay);
                        paint::stroke(c, slot, colour, mapSelected ? 2.0f : 1.0f);
                        std::string label = mapSelected ? "..." : clip->name;
                        for (const auto& mapping : clipMappings)
                            if ((mapping.keyCode > 0) == x.keyMapMode && mapping.target == *target)
                                label = mapping.keyCode > 0 ? std::string("Key ") + (char) mapping.keyCode
                                    : std::to_string(mapping.channel) + (mapping.isNote ? " Note " : " CC ") + std::to_string(mapping.number);
                        paint::playTriangle(c, btn, paint::contrastText(overlay));
                        paint::text(c, label, Rect{slot.x + 20, slot.y, slot.w - 22, slot.h}, paint::contrastText(overlay), m.smallFontSize);
                    }
            }
            else if (! isReturn)
            {
                Rect btn{slot.x + 5, slot.centreY() - 4, 8, 8};
                const bool armed = track->kind != model::TrackKind::Group && x.controls && (*x.controls)[(size_t) col].armed.load();
                if (armed) { SkPaint ink; ink.setColor(t.color("ClipSlotButton")); ink.setAntiAlias(true); c.drawCircle(btn.centreX(), btn.centreY(), 4, ink); }
                else paint::fill(c, btn, t.color("ClipSlotButton"));
            }
            if (x.recording && x.recordingSessionScene && x.recordingSessionScene(col) == s)
            {
                const int phase = x.sessionRecordingPhase ? x.sessionRecordingPhase(col) : 2;
                const bool blink = (juce::Time::getMillisecondCounter() / 250) % 2 == 0;
                SkPaint ink; ink.setAntiAlias(true); ink.setColor((phase == 1 || phase == 3) && ! blink ? t.color("ClipSlotButton") : t.color("ChosenRecord"));
                c.drawCircle(slot.x + 8, slot.centreY(), 4, ink);
                if (! clip) paint::text(c, phase == 1 ? "Queued" : phase == 3 ? "Stopping" : phase == 4 ? "Saving" : "Recording", Rect{slot.x + 18, slot.y, slot.w - 20, slot.h}, t.color("ControlForeground"), m.smallFontSize);
            }
        }

        c.restore();
        // Stop-clip row under the grid (Main: stop all clips).
        {
            paint::fill(c, {cx, gridBottom, cw - 2, MixerLayout::kSep}, line);
            const Rect stop{cx, stopTop, cw - 2, MixerLayout::kStopRow - 1};
            paint::fill(c, stop, bg);
            if (! isReturn)
                paint::stopSquare(c, {stop.x + 5, stop.centreY() - 4, 8, 8}, t.color("ClipSlotButton"));
            paint::fill(c, {cx, stop.bottom(), cw - 2, 1}, rowLine);
        }
        paint::fill(c, {cx + cw - 2, 0, 2, gridBottom + MixerLayout::kStopRow + MixerLayout::kSep}, line);
        if (track->groupId >= 0) paint::fill(c, {cx, 0, 1, gridBottom}, line);
        c.restore();
    }
}

// Width handles: a track's right edge, a return's / Main's left edge (their right sides are anchored).
std::optional<midi::ControlTarget> SessionView::mappingTargetAt(float x, float y) const
{
    const auto& context = ctx();
    if (! context.song || y < context.theme.metrics.sessionHeaderHeight + 1 || y >= gridBottom()) return std::nullopt;
    const int column = columnAt(x);
    const int scene = (int) std::floor((y - context.theme.metrics.sessionHeaderHeight - 1 + scrollY_) / context.theme.metrics.sessionSlotHeight);
    if (scene < 0 || (size_t) scene >= context.song->scenes.size()) return std::nullopt;
    return midi::sessionClipTarget(*context.song, column, scene);
}

int SessionView::resizeColumnAt(float x) const
{
    const int nTracks = ctx().numTracks();
    for (int c = 0; c < numColumns(); ++c)
    {
        if (! columnVisible(c)) continue;
        const float cx = columnX(c), cw = columnWidth(c);
        if (c < nTracks)
        {
            if (cx + cw <= tracksViewport() + 3 && std::fabs(x - (cx + cw - 1)) <= 3.5f) return c;
        }
        else if (std::fabs(x - (cx + 1)) <= 3.5f)
            return c;
    }
    return -1;
}

// The boundary between the clip slots and the mixer resizes the fader section.
bool SessionView::nearMixerEdge(float y) const
{
    return std::fabs(y - (gridBottom() + 1.0f)) <= 3.0f;
}

Cursor SessionView::cursorAt(float x, float y) const
{
    if (resizeColumnAt(x) >= 0 && y < height() - mixerHeight())
        return Cursor::ResizeHorizontal;
    return nearMixerEdge(y) ? Cursor::ResizeVertical : Cursor::Default;
}

void SessionView::mouseDrag(const MouseEvent& e)
{
    if (resizeColumn_ >= 0)
    {
        const int col = resizeColumn_;
        const bool track = col < ctx().numTracks();
        auto& widths = ctx().sessionColumnWidths;
        if (widths.size() < (size_t) numColumns()) widths.resize((size_t) numColumns(), 0.0f);
        widths[(size_t) col] = std::clamp(startSize_ + (e.x - grabX_) * (track ? 1.0f : -1.0f), 92.0f, 400.0f);
        layout();
        repaint();
    }
    else if (resizingMixer_)
    {
        ctx().theme.metrics.sessionVolumeHeight = std::clamp(startSize_ + (grabY_ - e.y), 100.0f, 520.0f);
        layout();
        repaint();
    }
}

void SessionView::mouseUp(const MouseEvent&)
{
    if (gateTrack_ >= 0 && ctx().releaseClip)
        ctx().releaseClip(gateTrack_, gateScene_);
    gateTrack_ = gateScene_ = -1;
    if ((resizeColumn_ >= 0 || resizingMixer_) && ctx().layoutChanged)
        ctx().layoutChanged();
    resizeColumn_ = -1;
    resizingMixer_ = false;
}

std::optional<TrackDropTarget> SessionView::trackDropTarget(const MouseEvent& e) const
{
    const auto& x = ctx();
    if (! x.song || e.y < 0 || e.y > x.theme.metrics.sessionHeaderHeight) return {};
    const int col = columnAt(e.x);
    if (col < 0)
    {
        if (e.x >= widthOf(0, x.numTracks()) - scrollX_ && e.x < tracksViewport())
            return TrackDropTarget{x.numTracks(), -1, {e.x, 0, 2, x.theme.metrics.sessionHeaderHeight}};
        return {};
    }
    if (col >= x.numTracks()) return {};
    const auto& track = x.song->tracks[(size_t) col];
    const float left = columnX(col), w = columnWidth(col), position = (e.x - left) / w;
    if (track.kind == model::TrackKind::Group && position > 0.25f && position < 0.75f)
        return TrackDropTarget{col + 1, track.id, {left + 2, 2, w - 4, x.theme.metrics.sessionHeaderHeight - 4}};
    int before = col;
    if (position >= 0.5f)
    {
        ++before;
        while (before < x.numTracks() && descendantOf(before, col)) ++before;
    }
    const float edge = position < 0.5f ? left : before < x.numTracks() ? columnX(before) : widthOf(0, x.numTracks()) - scrollX_;
    return TrackDropTarget{before, track.groupId, {edge - 1, 0, 2, gridBottom()}};
}

void SessionView::mouseDown(const MouseEvent& e)
{
    resizeColumn_ = -1;
    resizingMixer_ = false;
    if (const int handle = resizeColumnAt(e.x); handle >= 0 && ! e.right && e.y < height() - mixerHeight())
    {
        resizeColumn_ = handle;
        grabX_ = e.x;
        startSize_ = columnWidth(handle);
        return;
    }
    if (! e.right && nearMixerEdge(e.y))
    {
        resizingMixer_ = true;
        grabY_ = e.y;
        startSize_ = ctx().theme.metrics.sessionVolumeHeight;
        return;
    }
    const int col = columnAt(e.x);
    if (col < 0)
        return;
    auto& x = ctx();
    const auto& m = x.theme.metrics;
    // Shift / Cmd on a track title select several tracks (Cmd+G groups them)
    const bool onTitle = e.y < m.sessionHeaderHeight;
    if (e.right && onTitle)
    {
        x.sessionTrackSelection = true;
        x.selectedClipTrack = x.selectedClipUid = x.selectedClipScene = x.selectedTakeLane = -1;
        x.detailTab = DetailTab::Devices;
        if (! x.isTrackSelected(col)) x.selectTrack(col, false, false);
        showTrackContextMenu(x);
        repaint();
        return;
    }
    const bool selectRange = onTitle || (e.y > m.sessionHeaderHeight && e.y < gridBottom() && e.x - columnX(col) >= 17);
    x.sessionTrackSelection = onTitle || (selectRange && (e.shift || e.cmd));
    if (! (onTitle && ! e.shift && ! e.cmd && x.isTrackSelected(col)))
        x.selectTrack(col, selectRange && e.shift, selectRange && e.cmd && ! e.shift);
    if (onTitle)
    {
        x.selectedClipTrack = x.selectedClipUid = x.selectedClipScene = x.selectedTakeLane = -1;
        x.detailTab = DetailTab::Devices;
        if (x.selectedTrackIndices().size() > 1)
            x.statusText = std::to_string(x.selectedTrackIndices().size()) + " tracks selected";
    }
    if (e.right)
    {
        if (e.y > m.sessionHeaderHeight && e.y < gridBottom() && x.song && col < x.numTracks())
        {
            const int scene = (int) std::floor((e.y - m.sessionHeaderHeight - 1 + scrollY_) / m.sessionSlotHeight);
            if (scene >= 0 && (size_t) scene < x.song->scenes.size())
            {
                x.selectedSessionScene = scene;
                const auto* track = x.trackAt(col);
                const auto* clip = (size_t) scene < track->slots.size() && track->slots[(size_t) scene].clip ? &*track->slots[(size_t) scene].clip : nullptr;
                x.selectedClipTrack = clip ? col : -1; x.selectedClipUid = clip ? clip->uid : -1;
                x.selectedClipScene = clip ? scene : -1; x.selectedTakeLane = -1;
                showSessionSlotContextMenu(x, col, scene);
            }
        }
        repaint(); return; // a context click must never launch, stop or record a clip
    }
    if (onTitle && ! e.shift && ! e.cmd && groupToggleBounds(col).contains(e.x, e.y) && x.song && x.applyEdit)
    {
        const auto* track = x.trackAt(col);
        x.applyEdit(model::setTrackFolded(*x.song, col, ! track->folded));
        layout(); repaint(); return;
    }
    if (onTitle && col < x.numTracks() && ! e.shift && ! e.cmd && e.clicks < 2) x.beginTrackDrag();
    if (e.y >= gridBottom() && e.y < gridBottom() + MixerLayout::kSep + MixerLayout::kStopRow && x.song != nullptr)
    {
        // Stop-clip row: stops that track, or every clip in the Main column.
        if (col == x.numTracks() + x.numReturns())
        {
            if (x.stopAllClips) x.stopAllClips();
        }
        else if (col < x.numTracks() && x.trackAt(col))
        {
            if (x.trackAt(col)->kind == model::TrackKind::Group) stopGroup(col);
            else if (x.stopTrack) x.stopTrack(col);
        }
    }
    else if (e.y > m.sessionHeaderHeight && e.y < gridBottom() && x.song != nullptr)
    {
        const int scene = (int) std::floor((e.y - m.sessionHeaderHeight - 1 + scrollY_) / m.sessionSlotHeight);
        const int nScenes = (int) x.song->scenes.size();
        if (scene >= 0 && scene < nScenes) x.selectedSessionScene = scene;
        const bool isMain = col == x.numTracks() + x.numReturns();
        const bool isReturn = col >= x.numTracks() && ! isMain;
        if (isMain)
        {
            if (scene >= 0 && scene < nScenes && x.launchScene)
                x.launchScene(scene);
        }
        else if (! isReturn && scene >= 0 && scene < nScenes)
        {
            const auto* track = x.trackAt(col);
            if (track && track->kind == model::TrackKind::Group)
            {
                x.selectedClipTrack = x.selectedClipScene = x.selectedClipUid = x.selectedTakeLane = -1;
                if (e.x - columnX(col) < 17) stopGroup(col);
                repaint(); return;
            }
            const bool hasClip = track != nullptr && (size_t) scene < track->slots.size() && track->slots[(size_t) scene].clip.has_value();
            const bool launchButton = e.x - columnX(col) < 17;
            x.selectedTakeLane = -1;
            x.selectedClipTrack = hasClip ? col : -1;
            x.selectedClipScene = hasClip ? scene : -1;
            x.selectedClipUid = hasClip ? track->slots[(size_t) scene].clip->uid : -1;
            if (e.clicks >= 2 && ! launchButton)
            {
                if (! hasClip && track && track->kind == model::TrackKind::Midi && x.applyEdit)
                {
                    model::Clip fresh;
                    if (model::makeEmptyMidiClip(*x.song, 0, 4, fresh))
                    {
                        fresh.colorIndex = track->colorIndex;
                        auto command = model::setSessionClip(*x.song, col, scene, std::move(fresh));
                        command.label = "Insert Session MIDI Clip";
                        x.applyEdit(std::move(command));
                        const auto* after = x.trackAt(col);
                        if (after && (size_t) scene < after->slots.size() && after->slots[(size_t) scene].clip)
                        {
                            x.selectedClipTrack = col;
                            x.selectedClipScene = scene;
                            x.selectedClipUid = after->slots[(size_t) scene].clip->uid;
                        }
                    }
                    else x.statusText = "Cannot create a MIDI clip: no MIDI clip template in this set";
                }
                x.detailTab = DetailTab::Clip;
                x.detailVisible = true;
            }
            else if (launchButton && x.recordSessionClip && ((x.recording && x.recordingSessionScene && x.recordingSessionScene(col) == scene)
                     || (x.controls && (*x.controls)[(size_t) col].armed.load() && (! hasClip || e.shift))))
                x.recordSessionClip(col, scene);
            else if (launchButton && hasClip && x.launchClip)
            {
                x.launchClip(col, scene);
                gateTrack_ = col; // released on mouse up (Gate/Repeat launch modes)
                gateScene_ = scene;
            }
            else if (launchButton && ! hasClip && track != nullptr && track->kind != model::TrackKind::Group && x.stopTrack)
                x.stopTrack(col);
        }
    }
    repaint();
}

bool SessionView::mouseWheel(const MouseEvent& e)
{
    const auto& m = ctx().theme.metrics;
    const float maxX = std::max(0.0f, widthOf(0, ctx().numTracks()) - tracksViewport());
    scrollX_ = std::clamp(scrollX_ - e.wheelDeltaX * kWheelPixels, 0.0f, maxX);
    const float rows = ctx().song ? (float) ctx().song->scenes.size() : 0.0f;
    const float maxY = std::max(0.0f, rows * m.sessionSlotHeight - (gridBottom() - m.sessionHeaderHeight));
    scrollY_ = std::clamp(scrollY_ - e.wheelDeltaY * kWheelPixels, 0.0f, maxY);
    layout();
    repaint();
    return true;
}

bool SessionView::browserDrop(const BrowserDrag& payload, const MouseEvent& event)
{
    const int track = columnAt(event.x); // -1: samples dropped in the empty area right of the last track get a new audio track
    if (track < 0 && (payload.kind != BrowserDrag::Kind::Sample || event.x >= tracksViewport())) return false;
    int scene = -1;
    if (payload.kind == BrowserDrag::Kind::Sample)
    {
        const auto& metrics = ctx().theme.metrics;
        if (event.y < metrics.sessionHeaderHeight || event.y >= gridBottom()) return false;
        scene = std::max(0, (int) std::floor((event.y - metrics.sessionHeaderHeight - 1 + scrollY_) / metrics.sessionSlotHeight));
    }
    if (ctx().dropBrowserItem) ctx().dropBrowserItem(payload, track, 0, scene);
    return true;
}

bool SessionView::keyPressed(const KeyEvent& key)
{
    auto& x = ctx();
    if (! visible || ! x.song) return false;
    auto selectSlot = [&] {
        const auto* track = x.trackAt(x.selectedTrack);
        const auto scene = (size_t) std::max(0, x.selectedSessionScene);
        const auto* clip = track && scene < track->slots.size() && track->slots[scene].clip ? &*track->slots[scene].clip : nullptr;
        x.selectedClipTrack = clip ? x.selectedTrack : -1; x.selectedClipUid = clip ? clip->uid : -1;
        x.selectedClipScene = clip ? x.selectedSessionScene : -1; x.selectedTakeLane = -1;
        repaint();
    };
    if (! key.cmd && ! key.alt && ! key.ctrl
        && (key.keyCode == juce::KeyPress::leftKey || key.keyCode == juce::KeyPress::rightKey
            || key.keyCode == juce::KeyPress::upKey || key.keyCode == juce::KeyPress::downKey
            || key.keyCode == juce::KeyPress::homeKey || key.keyCode == juce::KeyPress::endKey))
    {
        const bool horizontal = key.keyCode == juce::KeyPress::leftKey || key.keyCode == juce::KeyPress::rightKey;
        if (horizontal)
        {
            const int delta = key.keyCode == juce::KeyPress::leftKey ? -1 : 1;
            int next = x.selectedTrack + delta;
            while (next >= 0 && next < numColumns() && ! columnVisible(next)) next += delta;
            if (next >= 0 && next < numColumns()) x.selectTrack(next, key.shift, false);
        }
        else if (key.keyCode == juce::KeyPress::homeKey || key.keyCode == juce::KeyPress::endKey)
            x.selectTrack(key.keyCode == juce::KeyPress::homeKey ? 0 : numColumns() - 1, false, false);
        else
        {
            x.sessionTrackSelection = false;
            x.selectedSessionScene = std::clamp(x.selectedSessionScene + (key.keyCode == juce::KeyPress::upKey ? -1 : 1), 0, std::max(0, (int) x.song->scenes.size() - 1));
        }
        if (! x.sessionTrackSelection) selectSlot();
        const auto selectedX = columnX(x.selectedTrack);
        if (x.selectedTrack < x.numTracks())
        {
            if (selectedX < 0) scrollX_ = std::max(0.0f, scrollX_ + selectedX);
            else if (selectedX + columnWidth(x.selectedTrack) > tracksViewport()) scrollX_ += selectedX + columnWidth(x.selectedTrack) - tracksViewport();
        }
        const float top = (float) x.selectedSessionScene * x.theme.metrics.sessionSlotHeight;
        const float view = gridBottom() - x.theme.metrics.sessionHeaderHeight - 1;
        if (top < scrollY_) scrollY_ = top;
        else if (top + x.theme.metrics.sessionSlotHeight > scrollY_ + view) scrollY_ = std::max(0.0f, top + x.theme.metrics.sessionSlotHeight - view);
        layout(); repaint(); return true;
    }
    if (! key.cmd && ! key.alt && key.keyCode == juce::KeyPress::escapeKey)
    { x.sessionTrackSelection = true; x.selectedClipTrack = x.selectedClipUid = x.selectedClipScene = x.selectedTakeLane = -1; repaint(); return true; }
    if (! key.alt && ! key.shift && key.keyCode == juce::KeyPress::returnKey)
    {
        const int track = x.selectedTrack, scene = x.selectedSessionScene;
        if (key.cmd) { if (x.stopTrack && track < x.numTracks()) x.stopTrack(track); }
        else if (track == numColumns() - 1) { if (x.launchScene) x.launchScene(scene); }
        else if (track >= 0 && track < x.numTracks())
        {
            const auto* target = x.trackAt(track);
            if (target->kind == model::TrackKind::Group) stopGroup(track);
            else if (scene >= 0 && (size_t) scene < target->slots.size() && target->slots[(size_t) scene].clip)
            { if (x.launchClip) x.launchClip(track, scene); }
            else if (x.controls && (*x.controls)[(size_t) track].armed.load() && x.recordSessionClip) x.recordSessionClip(track, scene);
            else if (x.stopTrack) x.stopTrack(track);
        }
        return true;
    }
    if (x.sessionTrackSelection && key.cmd && ! key.alt && ! key.shift && (key.textChar == 'a' || key.textChar == 'A'))
    { if (x.numTracks() > 0) { x.selectTrack(0, false, false); x.selectTrack(x.numTracks() - 1, true, false); } repaint(); return true; }
    if (! key.cmd && ! key.alt && ! key.ctrl && (key.textChar == 'u' || key.textChar == 'U'))
    {
        const auto* track = x.trackAt(x.selectedTrack);
        if (track && track->kind == model::TrackKind::Group && x.applyEdit)
        { x.applyEdit(model::setTrackFolded(*x.song, x.selectedTrack, ! track->folded)); layout(); repaint(); }
        return true;
    }
    if (! x.applyEdit || x.selectedClipScene < 0 || ! x.selectedClip()) return false;
    if (! key.cmd && ! key.alt && ! key.ctrl && ! key.shift && (key.keyCode == juce::KeyPress::deleteKey || key.keyCode == juce::KeyPress::backspaceKey))
    {
        x.applyEdit(model::deleteClip(*x.song, x.selectedClipLocation()));
        return true;
    }
    if (key.cmd && (key.textChar == 'd' || key.textChar == 'D'))
    {
        x.applyEdit(model::duplicateClip(*x.song, x.selectedClipLocation()));
        return true;
    }
    return false;
}

} // namespace sonora::ui
