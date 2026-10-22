#include "ui/views/PianoRoll.h"
#include "ui/CollabOverlay.h"
#include "ui/views/ClipControls.h"

#include "ui/WheelScale.h"

#include "ui/ClipEditing.h"
#include "core/TimeUtils.h"
#include "engine/EngineGraph.h"
#include "engine/Transport.h"
#include "ui/GridMenu.h"
#include "ui/Painter.h"
#include "gfx/Fonts.h"
#include "ui/Controls.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <cmath>

namespace sonora::ui
{

namespace
{
constexpr float kKeysW = 40.0f, kToolbarH = 20.0f, kRulerH = 36.0f;
bool isBlackKey(int pitch)
{
    const int pc = ((pitch % 12) + 12) % 12;
    return pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10;
}
std::string noteName(int pitch)
{
    static const char* names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    return std::string(names[((pitch % 12) + 12) % 12]) + std::to_string(pitch / 12 - 2); // Live: C3 == 60
}
} // namespace

PianoRoll::~PianoRoll() { stopAudition(); }
void PianoRoll::stopAudition()
{
    if (auditionBank_ && auditionTrack_ >= 0 && (size_t) auditionTrack_ < auditionBank_->size())
        (*auditionBank_)[(size_t) auditionTrack_].previewPitch.store(-1);
    auditionBank_.reset(); auditionTrack_ = auditionUid_ = -1;
}
void PianoRoll::focusLost() { stopAudition(); repaint(); }
void PianoRoll::tick()
{
    const auto* selected = clip();
    if (auditionBank_ && (! ctx().pianoPreview || ! selected || selected->uid != auditionUid_ || ctx().controls != auditionBank_))
    { stopAudition(); repaint(); }
    const double playhead = playheadBeat();
    if ((playhead < 0) != (lastPlayhead_ < 0) || std::abs(playhead - lastPlayhead_) * view.pixelsPerBeat >= 0.5)
    {
        lastPlayhead_ = playhead;
        repaint();
    }
}

// Same clocks as SampleView::playheadSeconds: a Session clip's own elapsed beats when its slot is the one playing, else
// the Arrangement position inside the clip; both folded into the loop like the engine plays them.
double PianoRoll::playheadBeat() const
{
    const auto* cl = clip();
    const auto* transport = ctx().transport;
    if (cl == nullptr || cl->kind != model::Clip::Kind::Midi || transport == nullptr || ! transport->isPlaying()) return -1;
    const auto& loop = cl->loop;
    if (const int scene = ctx().selectedClipScene; scene >= 0)
    {
        const auto& controls = ctx().controls;
        const int track = ctx().selectedClipTrack;
        if (! controls || track < 0 || (size_t) track >= controls->size()) return -1;
        const auto& c = (*controls)[(size_t) track];
        if (c.playingSlot.load(std::memory_order_relaxed) != scene) return -1;
        const double elapsed = std::max(0.0, c.sessionElapsed.load(std::memory_order_relaxed));
        if (! loop.enabled && elapsed >= std::max(loop.loopEnd, cl->currentEnd) - (loop.loopStart + loop.startRelative)) return -1;
        return model::clipContentPosition(elapsed, 0, loop.loopStart, loop.loopEnd, loop.startRelative, loop.enabled);
    }
    const double beat = transport->positionBeats();
    if (beat < cl->currentStart || beat >= cl->currentEnd) return -1;
    return model::clipContentPosition(beat, cl->currentStart, loop.loopStart, loop.loopEnd, loop.startRelative, loop.enabled);
}
void PianoRoll::audition(int pitch)
{
    if (! ctx().pianoPreview) return;
    const int track = ctx().selectedClipTrack;
    if (! ctx().controls || track < 0 || (size_t) track >= ctx().controls->size()) return;
    if (auditionBank_ != ctx().controls || auditionTrack_ != track) stopAudition();
    auditionBank_ = ctx().controls; auditionTrack_ = track; auditionUid_ = clip()->uid;
    (*auditionBank_)[(size_t) track].previewPitch.store(std::clamp(pitch, 0, 127));
    repaint();
}

const model::Clip* PianoRoll::clip() const
{
    return ctx().selectedClip();
}

const std::vector<model::MidiNote>& PianoRoll::notes() const
{
    static const std::vector<model::MidiNote> none;
    if (editing_)
        return working_;
    const auto* c = clip();
    return c != nullptr ? c->notes : none;
}

float PianoRoll::velocityHeight() const
{
    return std::clamp(ctx().theme.metrics.velocityLaneHeight, 24.0f, std::max(24.0f, height() * 0.7f));
}
Rect PianoRoll::keysArea() const { return {0, kRulerH, kKeysW, height() - kRulerH - velocityHeight()}; }
Rect PianoRoll::rulerArea() const { return {kKeysW, kToolbarH, width() - kKeysW, kRulerH - kToolbarH}; }
Rect PianoRoll::headerControlBounds(HeaderControl control) const
{
    if (control == HeaderControl::Fold) return {3, 1, 40, 17};
    if (control == HeaderControl::Scale) return {48, 1, 44, 17};
    return {98, 1, 115, 17};
}
Rect PianoRoll::notesArea() const { return {kKeysW, kRulerH, width() - kKeysW, height() - kRulerH - velocityHeight()}; }
Rect PianoRoll::velocityArea() const { return {kKeysW, height() - velocityHeight(), width() - kKeysW, velocityHeight()}; }

// The top edge of the velocity lane drags to resize it.
Cursor PianoRoll::cursorAt(float x, float y) const
{
    return x >= kKeysW && std::fabs(y - velocityArea().y) <= 2.5f ? Cursor::ResizeVertical : Cursor::Default;
}

double PianoRoll::beatsPerBar() const
{
    return ctx().song ? ctx().song->timeSigNumerator * 4.0 / ctx().song->timeSigDenominator : 4.0;
}

double PianoRoll::grid() const
{
    return ui::gridBeats(ctx().clipGrid, view.pixelsPerBeat, beatsPerBar());
}

double PianoRoll::snapUnit(const MouseEvent& e) const
{
    return ui::snapSize(ctx().clipGrid, view.pixelsPerBeat, beatsPerBar(), e.alt || e.cmd);
}

void PianoRoll::clampVertical()
{
    const double rows = std::max(1.0, (double) notesArea().h / view.rowHeight);
    const double highest = pitches_.empty() ? 127.0 : (double) pitches_.size() - 1;
    const double lowest = std::min(highest, rows - 1.0);
    view.topPitch = std::clamp(view.topPitch, lowest, highest);
}

int PianoRoll::pitchRow(int pitch) const
{
    return pitches_.empty() ? pitch : (int) std::distance(pitches_.begin(), std::lower_bound(pitches_.begin(), pitches_.end(), pitch));
}
int PianoRoll::yToPitch(float y) const
{
    const int row = (int) std::ceil(view.topPitch - (y - notesArea().y) / view.rowHeight);
    return pitches_.empty() ? std::clamp(row, 0, 127) : pitches_[(size_t) std::clamp(row, 0, (int) pitches_.size() - 1)];
}
float PianoRoll::pitchToY(int pitch) const { return notesArea().y + (float) (view.topPitch - pitchRow(pitch)) * view.rowHeight; }
void PianoRoll::syncPitchRows()
{
    std::vector<int> next;
    for (int pitch = 0; pitch < 128; ++pitch)
    {
        const bool used = clip() && std::any_of(clip()->notes.begin(), clip()->notes.end(), [pitch](const auto& note) { return note.pitch == pitch; });
        if (ctx().noteFold ? used : (! ctx().scaleFold || ctx().pitchInScale(pitch) || used)) next.push_back(pitch);
    }
    if (next.empty()) for (int pitch = 0; pitch < 128; ++pitch) next.push_back(pitch);
    if (next == pitches_) return;
    const int top = pitches_.empty() ? (int) view.topPitch : pitches_[(size_t) std::clamp((int) view.topPitch, 0, (int) pitches_.size() - 1)];
    pitches_ = std::move(next);
    view.topPitch = pitchRow(top);
}

void PianoRoll::zoomTime(float anchorX, double factor)
{
    const double anchor = xToBeat(anchorX);
    view.pixelsPerBeat = std::clamp(view.pixelsPerBeat * factor, 2.0, 2400.0);
    view.scrollBeats = std::max(0.0, anchor - (anchorX - notesArea().x) / view.pixelsPerBeat);
}

bool PianoRoll::mouseMagnify(const MouseEvent& e, float factor)
{
    zoomTime(e.x, std::clamp((double) factor, 0.5, 2.0));
    repaint();
    return true;
}

void PianoRoll::revealSelection()
{
    const auto* cl = clip();
    if (cl == nullptr || selected_.empty())
        return;
    int lo = 127, hi = 0;
    double first = 1e18, last = -1e18;
    for (int i : selected_)
        if (i >= 0 && (size_t) i < cl->notes.size())
        {
            lo = std::min(lo, cl->notes[(size_t) i].pitch);
            hi = std::max(hi, cl->notes[(size_t) i].pitch);
            first = std::min(first, cl->notes[(size_t) i].start);
            last = std::max(last, cl->notes[(size_t) i].start + cl->notes[(size_t) i].duration);
        }
    if (lo > hi)
        return;
    const double rows = notesArea().h / view.rowHeight;
    if (pitchRow(hi) + 1 > view.topPitch)
        view.topPitch = pitchRow(hi) + 1.0;
    else if (pitchRow(lo) < view.topPitch - rows + 1.0)
        view.topPitch = pitchRow(lo) + rows - 1.0;
    const double visibleBeats = notesArea().w / view.pixelsPerBeat;
    if (first < view.scrollBeats)
        view.scrollBeats = std::max(0.0, first - 0.25);
    else if (last > view.scrollBeats + visibleBeats)
        view.scrollBeats = std::max(0.0, last - visibleBeats + 0.25);
    clampVertical();
}

void PianoRoll::autoScroll(const MouseEvent& e)
{
    const auto area = notesArea();
    constexpr float edge = 14.0f;
    float dy = 0.0f, dx = 0.0f;
    if (e.y < area.y + edge) dy = (area.y + edge - e.y) / edge;
    else if (e.y > area.bottom() - edge) dy = -(e.y - (area.bottom() - edge)) / edge;
    if (e.x < area.x + edge) dx = -(area.x + edge - e.x) / edge;
    else if (e.x > area.right() - edge) dx = (e.x - (area.right() - edge)) / edge;
    dy = std::clamp(dy, -4.0f, 4.0f);
    dx = std::clamp(dx, -4.0f, 4.0f);
    if (dy != 0.0f)
        view.topPitch += dy * 0.35;
    if (dx != 0.0f)
        view.scrollBeats = std::max(0.0, view.scrollBeats + dx * 12.0 / view.pixelsPerBeat);
    clampVertical();
}

int PianoRoll::noteAt(float x, float y) const
{
    const auto area = notesArea();
    if (! area.contains(x, y))
        return -1;
    const auto& ns = notes();
    for (int i = (int) ns.size() - 1; i >= 0; --i)
    {
        const float x0 = beatToX(ns[(size_t) i].start);
        const float x1 = std::max(x0 + 3.0f, beatToX(ns[(size_t) i].start + ns[(size_t) i].duration));
        const float y0 = pitchToY(ns[(size_t) i].pitch);
        if (x >= x0 && x < x1 && y >= y0 && y < y0 + view.rowHeight)
            return i;
    }
    return -1;
}

void PianoRoll::selectOnly(int index)
{
    selected_.clear();
    if (index >= 0)
        selected_.insert(index);
}

bool PianoRoll::runMidiTool(midiedit::ToolSettings settings)
{
    const auto* current = clip(); auto& x = ctx();
    if (! current || ! x.song || ! x.applyEdit || x.recording || x.recordingRequested) return false;
    double start = timeRange_ ? rangeStart_ : current->loop.loopStart;
    double end = timeRange_ ? rangeEnd_ : current->loop.loopEnd;
    std::vector<int> scope;
    if (! midiedit::isGenerator(settings.tool) && ! selected_.empty() && ! timeRange_)
    {
        start = 1e18; end = 0;
        for (int i : selected_) if (i >= 0 && (size_t) i < current->notes.size())
        { scope.push_back(i); start = std::min(start, current->notes[(size_t) i].start); end = std::max(end, current->notes[(size_t) i].start + current->notes[(size_t) i].duration); }
    }
    else for (size_t i = 0; i < current->notes.size(); ++i)
        if (current->notes[i].start >= start && current->notes[i].start < end) scope.push_back((int) i);
    settings.root = x.editingScaleRoot(); settings.scaleName = x.editingScaleName();
    settings.scale = x.scaleHighlight || settings.tool == midiedit::Tool::FitScale;
    auto changed = midiedit::applyTool(*current, settings, scope, start, end);
    const auto location = x.selectedClipLocation();
    selected_.clear(); editing_ = false;
    x.applyEdit(model::modifyClip(*x.song, location, std::move(changed), midiedit::toolName(settings.tool)));
    // Keep the output in the same editing scope for successive tools.
    if (const auto* updated = x.selectedClip()) for (size_t i = 0; i < updated->notes.size(); ++i)
        if (updated->notes[i].start >= start && updated->notes[i].start < end) selected_.insert((int) i);
    syncPitchRows(); repaint(); return true;
}

// COL-5: someone else editing the same clip: their pitch row and position, with a name tag.
void PianoRoll::paintOverChildren(SkCanvas& c, const Theme& t)
{
    auto& x = ctx();
    const auto* cl = clip();
    if (x.collab.cursors.empty() || cl == nullptr) return;
    const auto area = notesArea();
    c.save();
    c.clipRect(area.sk());
    for (const auto& cursor : x.collab.cursors)
    {
        if (cursor.view != 2 || cursor.clipUid != cl->uid || cursor.beat < 0) continue;
        if (cursor.pitch >= 0)
        {
            const float y = pitchToY(cursor.pitch);
            paint::fill(c, {area.x, y, area.w, view.rowHeight}, paint::withAlpha(collab::colour(cursor), 40));
        }
        collab::pointer(c, beatToX(cursor.beat), area.y, area.bottom(), cursor, t.metrics.fontSize);
    }
    c.restore();
}

void PianoRoll::mouseMove(const MouseEvent& e)
{
    auto& x = ctx();
    if (! x.collabPointer || ! x.collab.active()) return;
    const bool inside = notesArea().contains(e.x, e.y);
    x.collabPointer(2, x.selectedClipTrack, inside ? std::max(0.0, xToBeat(e.x)) : -1.0, inside ? yToPitch(e.y) : -1);
}

void PianoRoll::mouseExit()
{
    auto& x = ctx();
    if (x.collabPointer && x.collab.active()) x.collabPointer(2, x.selectedClipTrack, -1.0, -1);
}

void PianoRoll::paint(SkCanvas& c, const Theme& t)
{
    const auto* cl = clip();
    paint::fill(c, localBounds(), t.color("DetailViewBackground"));
    if (cl == nullptr || cl->kind != model::Clip::Kind::Midi)
    {
        paint::text(c, cl == nullptr ? "Select a MIDI clip to edit its notes"
                                     : "Audio clip: waveform and warp markers (ROADMAP UI-4)",
                    localBounds(), t.color("TextDisabled"), t.metrics.fontSize, Align::Centre);
        return;
    }
    syncPitchRows();
    if (cl->uid != lastUid_ || lastZoomRequest_ != ctx().clipZoomRequest) // fit a newly opened clip to its content length
    {
        stopAudition();
        lastUid_ = cl->uid;
        lastZoomRequest_ = ctx().clipZoomRequest;
        editing_ = false;
        selected_.clear();
        timeRange_ = selectingRuler_ = false;
        view.scrollBeats = std::max(0.0, cl->loop.loopStart + cl->loop.startRelative);
        const double length = ctx().selectedClipScene >= 0 && cl->loop.enabled ? cl->loop.loopEnd - cl->loop.loopStart
                                                                            : cl->currentEnd - cl->currentStart;
        view.pixelsPerBeat = std::clamp((double) std::max(1.0f, notesArea().w - 1) / std::max(0.0625, length), 2.0, 2400.0);
        int hi = 0;
        for (const auto& n : cl->notes)
            hi = std::max(hi, n.pitch);
        view.topPitch = pitchRow(cl->notes.empty() ? 84 : std::clamp(hi + 4, 24, 127));
    }
    clampVertical();

    const auto area = notesArea();
    const SkColor clipColor = Theme::paletteColor(cl->colorIndex);
    // Rows from the one partly visible at the top edge down to the bottom edge.
    const int firstPitch = pitches_[(size_t) std::clamp((int) std::ceil(view.topPitch) + 1, 0, (int) pitches_.size() - 1)];
    const int lastPitch = pitches_[(size_t) std::clamp((int) std::floor(view.topPitch - area.h / view.rowHeight) - 1, 0, (int) pitches_.size() - 1)];

    // ---- rows (white / black keys) and keyboard
    c.save();
    c.clipRect(area.sk());
    for (int pitch = firstPitch; pitch >= lastPitch; --pitch)
    {
        if (! std::binary_search(pitches_.begin(), pitches_.end(), pitch)) continue;
        const Rect row{area.x, pitchToY(pitch), area.w, view.rowHeight};
        paint::fill(c, row, isBlackKey(pitch) ? paint::mix(t.color("DetailViewBackground"), t.color("SurfaceArea"), 0.1f) : t.color("DetailViewBackground"));
        if (ctx().scaleHighlight)
        {
            if (ctx().pitchInScale(pitch))
                paint::fill(c, row, paint::withAlpha(clipColor, 42));
        }
        if (pitch % 12 == 0)
            paint::line(c, row.x, row.bottom() - 0.5f, row.right(), row.bottom() - 0.5f, t.color("GridLineBase"));
    }
    // alternating tiles (per beat when there is room, else per bar) and the adaptive / fixed grid lines
    const double bar = beatsPerBar();
    const double beatUnit = ctx().song ? 4.0 / std::max(1, ctx().song->timeSigDenominator) : 1.0;
    const double firstBeat = xToBeat(area.x), lastBeat = xToBeat(area.right());
    double tileUnit = beatUnit;
    while (tileUnit * view.pixelsPerBeat < 28.0)
        tileUnit = tileUnit < bar - 1e-9 ? bar : tileUnit * 2.0;
    for (double b = std::floor(std::max(0.0, firstBeat) / tileUnit) * tileUnit; b < lastBeat; b += tileUnit)
        if (((long long) std::llround(b / tileUnit)) % 2 != 0)
            paint::fill(c, {beatToX(b), area.y, (float) (tileUnit * view.pixelsPerBeat), area.h}, t.color("ArrangerGridTiles"));
    const double gridSize = grid();
    const double subFade = std::clamp((gridSize * view.pixelsPerBeat - 3.0) / 9.0, 0.0, 1.0);
    for (const auto& line : ui::gridLines(firstBeat, lastBeat, view.pixelsPerBeat, bar, beatUnit, gridSize))
        paint::line(c, beatToX(line.beat), area.y, beatToX(line.beat), area.bottom(),
                    paint::withAlpha(t.color("GridLineBase"), line.strength == 2 ? (uint8_t) 255 : line.strength == 1 ? (uint8_t) 150 : (uint8_t) (70 * subFade)));
    // loop region of the clip
    paint::fill(c, {beatToX(cl->loop.loopEnd), area.y, 1.5f, area.h}, paint::withAlpha(t.color("LoopColor"), 200));
    paint::fill(c, {beatToX(cl->loop.loopStart), area.y, 1.5f, area.h}, paint::withAlpha(t.color("LoopColor"), 200));

    if (timeRange_)
    {
        const float left = std::max(area.x, beatToX(rangeStart_)), right = std::min(area.right(), beatToX(rangeEnd_));
        if (right > left) paint::fill(c, {left, area.y, right - left, area.h}, paint::withAlpha(t.color("SelectionBackground"), 60));
    }
    // ---- notes
    const auto& ns = notes();
    for (int i = 0; i < (int) ns.size(); ++i)
    {
        const auto& n = ns[(size_t) i];
        const float x0 = beatToX(n.start), x1 = std::max(x0 + 2.0f, beatToX(n.start + n.duration));
        const float y0 = pitchToY(n.pitch);
        if (x1 < area.x || x0 > area.right() || y0 + view.rowHeight < area.y || y0 > area.bottom())
            continue;
        const SkColor fill = clipColor;
        const Rect r{x0, y0 + 0.5f, x1 - x0 - 0.5f, view.rowHeight - 1.0f};
        paint::fill(c, r, n.enabled ? fill : paint::withAlpha(fill, 90));
        paint::stroke(c, r, isSelected(i) ? SkColorSetRGB(255, 255, 255) : paint::mix(clipColor, SkColorSetRGB(0, 0, 0), 0.6f));
        // Zoomed in far enough (Live): the note's name centred inside it, once the row and the note fit the text.
        if (view.rowHeight >= 14.0f)
        {
            const float size = std::min(t.metrics.fontSize + 1.0f, view.rowHeight - 4.0f);
            const auto name = noteName(n.pitch);
            if (gfx::measureText(name, size) + 6.0f <= r.w)
                paint::text(c, name, r, paint::withAlpha(paint::contrastText(fill), n.enabled ? (uint8_t) 230 : (uint8_t) 120), size, Align::Centre);
        }
    }
    if (marqueeActive_)
    {
        const float mx0 = beatToX(std::min(marqueeBeat0_, marqueeBeat1_)), mx1 = beatToX(std::max(marqueeBeat0_, marqueeBeat1_));
        const float my0 = area.y + (float) (view.topPitch - std::max(marqueeRow0_, marqueeRow1_)) * view.rowHeight;
        const float my1 = area.y + (float) (view.topPitch - std::min(marqueeRow0_, marqueeRow1_)) * view.rowHeight;
        const Rect box{mx0, my0, std::max(1.0f, mx1 - mx0), std::max(1.0f, my1 - my0)};
        paint::fill(c, box, paint::withAlpha(t.color("SelectionBackground"), 90));
        paint::stroke(c, box, t.color("SelectionFrame"));
    }
    // grid indicator in the corner, like Live
    paint::text(c, ui::gridLabel(gridSize, bar) + (ctx().clipGrid.snap ? "" : " (off)"), {area.right() - 90, area.bottom() - 22, 84, 18},
                paint::withAlpha(t.color("ControlForeground"), 90), t.metrics.fontSize + 3, Align::Right, true);
    c.restore();

    // ---- keys
    const auto keys = keysArea();
    c.save();
    c.clipRect(keys.sk());
    for (int pitch = firstPitch; pitch >= lastPitch; --pitch)
    {
        if (! std::binary_search(pitches_.begin(), pitches_.end(), pitch)) continue;
        const Rect key{keys.x, pitchToY(pitch), keys.w, view.rowHeight};
        paint::fill(c, key, isBlackKey(pitch) ? t.color("PianoBlackKey") : t.color("PianoWhiteKey"));
        if (ctx().scaleHighlight)
        {
            if (ctx().pitchInScale(pitch))
                paint::fill(c, key, paint::mix(isBlackKey(pitch) ? t.color("PianoBlackKey") : t.color("PianoWhiteKey"), clipColor, 0.35f));
        }
        if (auditionBank_ && (*auditionBank_)[(size_t) auditionTrack_].previewPitch.load() == pitch)
            paint::fill(c, key, t.color("SelectionBackground"));
        paint::line(c, key.x, key.bottom() - 0.5f, key.right(), key.bottom() - 0.5f, t.color("PianoKeySeparator"));
        if (pitch % 12 == 0)
            paint::text(c, noteName(pitch), key.reduced(3, 0), t.color("TextDisabled"), t.metrics.smallFontSize, Align::Right);
    }
    c.restore();

    // ---- ruler: bars, beats and finer positions graduate with the zoom (1, 1.2, 1.1.3 ...)
    paint::fill(c, {0, 0, width(), kToolbarH}, t.color("SurfaceBackground"));
    ctl::toggle(c, t, headerControlBounds(HeaderControl::Fold), "Fold", ctx().noteFold, t.color("SelectionBackground"));
    ctl::toggle(c, t, headerControlBounds(HeaderControl::Scale), "Scale", ctx().scaleFold, t.color("SelectionBackground"));
    const auto highlight = headerControlBounds(HeaderControl::Highlight);
    ctl::toggle(c, t, {highlight.x, highlight.y + 2, 13, 13}, ctx().scaleHighlight ? "✓" : "", ctx().scaleHighlight, Theme::paletteColor(cl->colorIndex));
    paint::text(c, "Highlight Scale", {highlight.x + 17, highlight.y, highlight.w - 17, highlight.h}, t.color("ControlForeground"), t.metrics.smallFontSize);
    const auto ruler = rulerArea();
    paint::fill(c, ruler, t.color("SurfaceBackground"));
    c.save();
    c.clipRect(ruler.sk());
    for (const auto& mark : ui::rulerMarks(firstBeat, lastBeat, view.pixelsPerBeat, bar, beatUnit, 5.5f))
    {
        const float mx = beatToX(mark.beat);
        if (mark.labelled)
        {
            paint::line(c, mx + 0.5f, kToolbarH + 2, mx + 0.5f, kRulerH, t.color("ArrangementRulerMarkings"));
            paint::text(c, mark.text, {mx + 3, kToolbarH + 1, 56, 13}, t.color("ControlForeground"), t.metrics.smallFontSize);
        }
        else
            paint::line(c, mx + 0.5f, kToolbarH + (mark.level == 0 ? 8.0f : mark.level == 1 ? 10.0f : 12.0f), mx + 0.5f, kRulerH,
                        paint::withAlpha(t.color("ArrangementRulerMarkings"), 150));
    }
    c.restore();

    // loop brace of the clip along the bottom edge of the ruler
    {
        const float x0 = std::max(ruler.x, beatToX(cl->loop.loopStart)), x1 = std::min(ruler.right(), beatToX(cl->loop.loopEnd));
        if (x1 > x0)
            paint::fill(c, {x0, ruler.bottom() - 5, x1 - x0, 5}, cl->loop.enabled ? t.color("ChosenDefault") : t.color("SurfaceArea"));
    }

    // ---- velocity lane
    const auto vel = velocityArea();
    paint::fill(c, vel, t.color("SurfaceBackground"));
    paint::line(c, vel.x, vel.y + 0.5f, vel.right(), vel.y + 0.5f, t.color("ControlContrastFrame"));
    c.save();
    c.clipRect(vel.sk());
    for (int i = 0; i < (int) ns.size(); ++i)
    {
        const auto& n = ns[(size_t) i];
        const float x0 = beatToX(n.start);
        if (x0 < vel.x - 2 || x0 > vel.right())
            continue;
        const float h = (vel.h - 6) * (probabilityLane_ ? n.probability : n.velocity / 127.0f);
        paint::fill(c, {x0, vel.bottom() - 2 - h, 3.0f, h}, isSelected(i) ? SkColorSetRGB(255, 255, 255) : clipColor);
    }
    c.restore();
    paint::text(c, probabilityLane_ ? "Prob" : "Vel", {0, vel.y, kKeysW - 2, 18}, t.color("ControlForeground"),
                t.metrics.smallFontSize, Align::Centre);
    paint::line(c, kKeysW - 0.5f, 0, kKeysW - 0.5f, height(), t.color("ControlContrastFrame"));

    // ---- playhead: the Arrangement's line (ControlForeground while playing) from the ruler through the velocity lane
    if (const double playhead = playheadBeat(); playhead >= 0)
    {
        const float px = std::round(beatToX(playhead)) + 0.5f;
        if (px >= area.x && px <= area.right())
            paint::line(c, px, ruler.y, px, vel.bottom(), t.color("ControlForeground"));
    }
}

void PianoRoll::mouseDown(const MouseEvent& e)
{
    const auto* cl = clip();
    auto& x = ctx();
    if (cl == nullptr || cl->kind != model::Clip::Kind::Midi)
        return;
    resizeLane_ = false;
    x.clipEditorFocused = true;
    if (! e.right && e.y < kToolbarH)
    {
        if (headerControlBounds(HeaderControl::Fold).contains(e.x, e.y))
        { x.noteFold = ! x.noteFold; if (x.noteFold) x.scaleFold = false; }
        else if (headerControlBounds(HeaderControl::Scale).contains(e.x, e.y))
        { x.scaleFold = ! x.scaleFold; if (x.scaleFold) { x.noteFold = false; x.scaleHighlight = true; } }
        else if (headerControlBounds(HeaderControl::Highlight).contains(e.x, e.y)) x.scaleHighlight = ! x.scaleHighlight;
        syncPitchRows(); repaint(); return;
    }
    if (! e.right && keysArea().contains(e.x, e.y)) { audition(yToPitch(e.y)); return; }
    if (! x.applyEdit) return;
    if (! e.right && e.x >= kKeysW && rulerArea().contains(e.x, e.y))
    {
        timeRange_ = selectingRuler_ = true;
        rangeGrab_ = rangeStart_ = rangeEnd_ = std::max(0.0, ui::snapToGrid(xToBeat(e.x), snapUnit(e)));
        selected_.clear(); repaint(); return;
    }
    if (e.right)
    {
        const int note = noteAt(e.x, e.y);
        if (note >= 0 && ! isSelected(note)) selectOnly(note);
        const auto location = x.selectedClipLocation();
        auto command = [this, location](int code, uint32_t ch, bool cmd = false, bool shift = false) {
            return [this, location, code, ch, cmd, shift] {
                if (Widget::isAlive(this) && ctx().selectedClipLocation().uid == location.uid
                    && ctx().selectedClipLocation().track == location.track)
                    keyPressed({.keyCode = code, .textChar = ch, .shift = shift, .cmd = cmd});
            };
        };
        ui::showGridMenu(x.clipGrid, beatsPerBar(), [this] {
            if (! Widget::isAlive(this)) return;
            if (ctx().layoutChanged) ctx().layoutChanged();
            repaint();
        }, {{"Cut", {}, false, "⌘X"}, {"Copy", {}, false, "⌘C"}, {"Paste", {}, false, "⌘V"},
            {"Duplicate", command('D', 'D', true), timeRange_ || ! selected_.empty(), "⌘D"},
            {"Delete", command(juce::KeyPress::deleteKey, 0), ! selected_.empty(), "⌫"},
            {"Quantize", command('U', 'U', true), ! selected_.empty(), "⌘U"},
            {"Quantize Settings...", command('U', 'U', true, true), true, "⇧⌘U"},
            {"Select All", command('A', 'A', true), true, "⌘A"}}, x.uiZoom);
        repaint();
        return;
    }
    if (e.x >= kKeysW && std::fabs(e.y - velocityArea().y) <= 2.5f)
    {
        resizeLane_ = true;
        grabY_ = e.y;
        startVelocityHeight_ = velocityHeight();
        return;
    }
    if (e.x < kKeysW && e.y >= velocityArea().y)
    {
        probabilityLane_ = ! probabilityLane_;
        repaint();
        return;
    }
    std::erase_if(selected_, [&](int index) { return index < 0 || (size_t) index >= cl->notes.size(); });
    changed_ = false;
    working_ = cl->notes;
    original_ = cl->notes;
    editingLocation_ = ctx().selectedClipLocation();
    editing_ = true;
    grabbedNote_ = -1;
    mode_ = Mode::None;

    if (velocityArea().contains(e.x, e.y))
    {
        timeRange_ = false;
        velocityGrabX_ = velocityLastX_ = e.x; velocityLastY_ = e.y;
        velocityLine_ = x.drawMode;
        // pick the note whose stem is nearest in x
        int best = -1;
        float bestD = 6.0f;
        for (int i = 0; i < (int) working_.size(); ++i)
        {
            const float d = std::fabs(beatToX(working_[(size_t) i].start) + 1.5f - e.x);
            if (d < bestD)
            {
                bestD = d;
                best = i;
            }
        }
        if (best >= 0)
        {
            grabbedNote_ = best;
            mode_ = Mode::Velocity;
            if (! isSelected(best))
                selectOnly(best);
            mouseDrag(e);
        }
        else { mode_ = Mode::Velocity; velocityLine_ = true; }
        return;
    }

    timeRange_ = false;

    const int hit = noteAt(e.x, e.y);
    if (x.drawMode && notesArea().contains(e.x, e.y) && ! e.shift)
    {
        // Draw Mode: clicking a note removes it, clicking empty space paints a note per grid cell while dragging.
        if (hit >= 0)
        {
            working_.erase(working_.begin() + hit);
            selected_.clear();
            changed_ = true;
            commit("Delete Notes");
            return;
        }
        mode_ = Mode::Draw;
        selected_.clear();
        drawCell_ = -1;
        paintNoteAt(std::floor(xToBeat(e.x) / grid() + 1e-9) * grid(), yToPitch(e.y));
        return;
    }
    if (hit >= 0)
    {
        if (e.shift)
        {
            if (isSelected(hit))
                selected_.erase(hit);
            else
                selected_.insert(hit);
        }
        else if (! isSelected(hit))
            selectOnly(hit);
        copying_ = false;
        int grabbed = hit;
        if (e.alt && ! e.shift)
        {
            // Option+drag duplicates the selected notes; the copies are what the drag moves
            std::set<int> copies;
            for (int i : selected_)
            {
                auto copy = working_[(size_t) i];
                copy.noteId = -1;
                working_.push_back(copy);
                original_.push_back(copy);
                copies.insert((int) working_.size() - 1);
                if (i == hit) grabbed = (int) working_.size() - 1;
            }
            selected_ = copies;
            copying_ = true; // nothing is stored unless the drag actually moves the copies
        }
        grabbedNote_ = grabbed;
        grabBeat_ = xToBeat(e.x);
        grabPitch_ = yToPitch(e.y);
        const float x1 = beatToX(working_[(size_t) hit].start + working_[(size_t) hit].duration);
        mode_ = e.x > x1 - 6.0f ? Mode::Resize : Mode::Move;
        return;
    }

    if (e.clicks >= 2 && notesArea().contains(e.x, e.y))
    {
        // new note at the clicked cell, one grid step long
        model::MidiNote n;
        const double g = grid();
        n.start = std::max(0.0, snapUnit(e) > 0.0 ? std::floor(xToBeat(e.x) / g + 1e-9) * g : xToBeat(e.x));
        n.duration = g;
        n.pitch = std::clamp(yToPitch(e.y), 0, 127);
        n.velocity = 100.0f;
        working_.push_back(n);
        selectOnly((int) working_.size() - 1);
        changed_ = true;
        commit("Add Note");
        return;
    }
    // Empty space: drag a selection rectangle (a plain click deselects; Shift keeps the current selection).
    editing_ = false;
    working_.clear();
    if (notesArea().contains(e.x, e.y))
    {
        marqueeBase_ = e.shift ? selected_ : std::set<int>{};
        if (! e.shift) selected_.clear();
        marqueeActive_ = true;
        mode_ = Mode::Marquee;
        marqueeBeat0_ = marqueeBeat1_ = xToBeat(e.x);
        marqueeRow0_ = marqueeRow1_ = yToRow(e.y);
        return;
    }
    selected_.clear();
    repaint();
}

// Draw Mode: one grid-long note per cell, none where a note of that pitch already starts.
void PianoRoll::paintNoteAt(double cellStart, int pitch)
{
    cellStart = std::max(0.0, cellStart);
    pitch = std::clamp(pitch, 0, 127);
    if (std::fabs(cellStart - drawCell_) < 1e-9 && pitch == drawPitch_)
        return;
    drawCell_ = cellStart;
    drawPitch_ = pitch;
    for (const auto& n : working_)
        if (n.pitch == pitch && std::fabs(n.start - cellStart) < 1e-9)
            return;
    model::MidiNote n;
    n.start = cellStart;
    n.duration = grid();
    n.pitch = pitch;
    n.velocity = 100.0f;
    working_.push_back(n);
    changed_ = true;
}

void PianoRoll::updateMarquee(const MouseEvent& e)
{
    const auto* cl = clip();
    if (cl == nullptr)
        return;
    const auto area = notesArea();
    marqueeBeat1_ = xToBeat(std::clamp(e.x, area.x, area.right()));
    marqueeRow1_ = std::clamp(yToRow(e.y), -1.0, 128.0);
    const double b0 = std::min(marqueeBeat0_, marqueeBeat1_), b1 = std::max(marqueeBeat0_, marqueeBeat1_);
    const double r0 = std::min(marqueeRow0_, marqueeRow1_), r1 = std::max(marqueeRow0_, marqueeRow1_);
    selected_ = marqueeBase_;
    for (int i = 0; i < (int) cl->notes.size(); ++i)
    {
        const auto& n = cl->notes[(size_t) i];
        // a note occupies the pitch rows [pitch - 1, pitch] and the beats [start, start + duration]
        if (n.start <= b1 && n.start + n.duration >= b0 && (double) pitchRow(n.pitch) >= r0 && (double) (pitchRow(n.pitch) - 1) <= r1)
            selected_.insert(i);
    }
}

void PianoRoll::mouseDrag(const MouseEvent& e)
{
    if (auditionBank_) { if (keysArea().contains(e.x, e.y)) audition(yToPitch(e.y)); else stopAudition(); return; }
    if (selectingRuler_)
    {
        const double beat = std::max(0.0, ui::snapToGrid(xToBeat(e.x), snapUnit(e)));
        rangeStart_ = std::min(rangeGrab_, beat); rangeEnd_ = std::max(rangeGrab_, beat);
        selected_.clear();
        if (const auto* current = clip())
            for (int i = 0; i < (int) current->notes.size(); ++i)
                if (current->notes[(size_t) i].start >= rangeStart_ && current->notes[(size_t) i].start < rangeEnd_) selected_.insert(i);
        repaint(); return;
    }
    if (mode_ == Mode::Draw && editing_)
    {
        autoScroll(e);
        const double g = grid();
        const double cell = std::floor(xToBeat(e.x) / g + 1e-9) * g;
        // fill every cell crossed since the last event, so fast drags leave no gaps
        const double from = drawCell_ < 0 ? cell : drawCell_;
        const int steps = (int) std::lround(std::fabs(cell - from) / g);
        const double dir = cell >= from ? 1.0 : -1.0;
        for (int i = 1; i <= steps; ++i)
            paintNoteAt(from + dir * g * i, yToPitch(e.y));
        if (steps == 0)
            paintNoteAt(cell, yToPitch(e.y));
        repaint();
        return;
    }
    if (mode_ == Mode::Marquee && marqueeActive_)
    {
        autoScroll(e);
        updateMarquee(e);
        repaint();
        return;
    }
    if (resizeLane_)
    {
        ctx().theme.metrics.velocityLaneHeight = std::clamp(startVelocityHeight_ + (grabY_ - e.y), 24.0f, std::max(24.0f, height() * 0.7f));
        repaint();
        return;
    }
    if (! editing_ || (grabbedNote_ < 0 && mode_ != Mode::Velocity))
        return;
    // Option means "copy" while copying notes, so only Cmd skips the grid then
    const double g = copying_ ? ui::snapSize(ctx().clipGrid, view.pixelsPerBeat, beatsPerBar(), e.cmd) : snapUnit(e);
    if (mode_ == Mode::Move || mode_ == Mode::Resize)
        autoScroll(e);
    if (mode_ == Mode::Velocity)
    {
        const auto vel = velocityArea();
        if (! velocityLine_ && std::fabs(e.x - velocityGrabX_) > 4)
        {
            // A horizontal stroke edits only crossed stems, even after starting on selected notes.
            working_ = original_;
            velocityLine_ = true;
        }
        auto setValue = [&](model::MidiNote& note, float y) {
            const float v = std::clamp(1.0f - (y - vel.y - 3.0f) / (vel.h - 6.0f), 0.0f, 1.0f);
            if (probabilityLane_) note.probability = v;
            else note.velocity = std::clamp(v * 127, 1.0f, 127.0f);
            changed_ = true;
        };
        if (velocityLine_)
        {
            for (auto& note : working_)
            {
                const float nx = beatToX(note.start) + 1.5f;
                if (nx < std::min(velocityLastX_, e.x) - 2 || nx > std::max(velocityLastX_, e.x) + 2) continue;
                const float fraction = std::fabs(e.x - velocityLastX_) < 0.01f ? 1.0f
                    : std::clamp((nx - velocityLastX_) / (e.x - velocityLastX_), 0.0f, 1.0f);
                setValue(note, velocityLastY_ + fraction * (e.y - velocityLastY_));
            }
        }
        else for (int i : selected_) setValue(working_[(size_t) i], e.y);
        velocityLastX_ = e.x; velocityLastY_ = e.y;
    }
    else if (mode_ == Mode::Move)
    {
        const double dBeats = xToBeat(e.x) - grabBeat_;
        const int dPitch = yToPitch(e.y) - grabPitch_;
        // the grabbed note defines the snapped shift for all selected notes
        const auto& ref = original_[(size_t) grabbedNote_];
        const double target = g > 0.0 ? snapToGrid(ref.start + dBeats, g) : ref.start + dBeats;
        const double shift = std::max(-ref.start, target - ref.start);
        for (int i : selected_)
        {
            working_[(size_t) i].start = std::max(0.0, original_[(size_t) i].start + shift);
            working_[(size_t) i].pitch = std::clamp(original_[(size_t) i].pitch + dPitch, 0, 127);
        }
        changed_ = changed_ || std::fabs(shift) > 1e-9 || dPitch != 0;
    }
    else if (mode_ == Mode::Resize)
    {
        const double end = xToBeat(e.x);
        for (int i : selected_)
        {
            const auto& o = original_[(size_t) i];
            double newEnd = g > 0.0 ? snapToGrid(end, g) : end;
            newEnd = std::max(newEnd, o.start + (g > 0.0 ? g : 0.03125));
            working_[(size_t) i].duration = newEnd - o.start;
        }
        changed_ = true;
    }
    repaint();
}

void PianoRoll::mouseUp(const MouseEvent&)
{
    if (selectingRuler_) { selectingRuler_ = false; repaint(); return; }
    if (auditionBank_) { stopAudition(); repaint(); return; }
    if (resizeLane_)
    {
        resizeLane_ = false;
        if (ctx().layoutChanged) ctx().layoutChanged();
        return;
    }
    if (marqueeActive_)
    {
        const double unit = grid();
        rangeStart_ = std::max(0.0, std::floor(std::min(marqueeBeat0_, marqueeBeat1_) / unit + 1e-9) * unit);
        rangeEnd_ = std::ceil(std::max(marqueeBeat0_, marqueeBeat1_) / unit - 1e-9) * unit;
        timeRange_ = rangeEnd_ > rangeStart_ + 1e-9;
        marqueeActive_ = false;
        mode_ = Mode::None;
        repaint();
        return;
    }
    if (! editing_)
        return;
    if (changed_)
        commit(copying_ ? "Copy Notes" : mode_ == Mode::Draw ? "Draw Notes" : mode_ == Mode::Resize ? "Resize Notes" : mode_ == Mode::Velocity
                   ? (probabilityLane_ ? "Change Probability" : "Change Velocity") : "Move Notes");
    else
    {
        editing_ = false;
        working_.clear();
    }
    mode_ = Mode::None;
    copying_ = false;
    repaint();
}

void PianoRoll::commit(const char* label)
{
    auto& x = ctx();
    const auto* cl = x.song ? model::findClip(*x.song, editingLocation_) : nullptr;
    if (cl != nullptr && x.applyEdit)
    {
        // Selected notes keep their identity through the re-sort in setNotes: remember them by content.
        std::vector<model::MidiNote> keep;
        for (int i : selected_)
            if (i >= 0 && i < (int) working_.size())
                keep.push_back(working_[(size_t) i]);
        x.applyEdit(model::setNotes(*x.song, editingLocation_, working_, label));
        selected_.clear();
        const auto* after = clip();
        if (after != nullptr)
            for (int i = 0; i < (int) after->notes.size(); ++i)
                for (const auto& k : keep)
                    if (after->notes[(size_t) i].pitch == k.pitch && std::fabs(after->notes[(size_t) i].start - k.start) < 1e-9
                        && std::fabs(after->notes[(size_t) i].duration - k.duration) < 1e-9)
                        selected_.insert(i);
    }
    editing_ = false;
    working_.clear();
    changed_ = false;
}

bool PianoRoll::mouseWheel(const MouseEvent& e)
{
    if (e.cmd && ! e.alt)
    {
        // Cmd + wheel: horizontal zoom around the pointer, scroll up zooms in (like Live's arrangement and note editor)
        const float delta = std::fabs(e.wheelDeltaY) >= std::fabs(e.wheelDeltaX) ? e.wheelDeltaY : e.wheelDeltaX;
        zoomTime(std::max(e.x, notesArea().x), wheelZoomFactor(delta));
    }
    else if (e.alt)
    {
        // Alt (or Cmd+Alt) + wheel: row height, keeping the pitch under the pointer fixed
        const double anchorPitch = view.topPitch - (e.y - notesArea().y) / view.rowHeight;
        view.rowHeight = std::clamp(view.rowHeight * (float) wheelZoomFactor(e.wheelDeltaY * 0.6), 6.0f, 40.0f);
        view.topPitch = anchorPitch + (e.y - notesArea().y) / view.rowHeight;
    }
    else if (e.shift)
    {
        const float delta = std::fabs(e.wheelDeltaX) > std::fabs(e.wheelDeltaY) ? e.wheelDeltaX : e.wheelDeltaY;
        view.scrollBeats = std::max(0.0, view.scrollBeats - delta * kWheelPixels / view.pixelsPerBeat);
    }
    else
    {
        // plain wheel / two-finger scroll: smooth in pixels, so small trackpad deltas never round away
        view.scrollBeats = std::max(0.0, view.scrollBeats - e.wheelDeltaX * kWheelPixels / view.pixelsPerBeat);
        view.topPitch += e.wheelDeltaY * kWheelPixels / view.rowHeight;
    }
    clampVertical();
    repaint();
    return true;
}

bool PianoRoll::keyPressed(const KeyEvent& k)
{
    auto& x = ctx();
    const auto* cl = clip();
    if (! visible || cl == nullptr || cl->kind != model::Clip::Kind::Midi || ! x.applyEdit)
        return false;
    if (! k.cmd && ! k.alt && ! k.ctrl && (k.textChar == 'k' || k.textChar == 'K') && (! x.computerKeyboard || k.shift))
    { x.scaleHighlight = ! x.scaleHighlight; repaint(); return true; }
    if (! k.cmd && ! k.alt && ! k.ctrl && ! x.computerKeyboard && (k.textChar == 'f' || k.textChar == 'F'))
    { x.noteFold = ! x.noteFold; if (x.noteFold) x.scaleFold = false; syncPitchRows(); repaint(); return true; }
    if (! k.cmd && ! k.alt && ! k.ctrl && ! x.computerKeyboard && (k.textChar == 'g' || k.textChar == 'G'))
    { x.scaleFold = ! x.scaleFold; if (x.scaleFold) { x.noteFold = false; x.scaleHighlight = true; } syncPitchRows(); repaint(); return true; }
    auto edit = [&](std::vector<model::MidiNote> ns, const char* label) {
        x.applyEdit(model::setNotes(*x.song, x.selectedClipLocation(), std::move(ns), label));
    };

    if (k.cmd && ! k.alt && ! k.shift && k.textChar >= '1' && k.textChar <= '5' && ui::handleGridShortcut(x.clipGrid, (int) k.textChar))
    {
        if (x.layoutChanged) x.layoutChanged();
        repaint();
        return true;
    }
    if (! k.cmd && ! k.alt && ! k.ctrl && (k.textChar == '+' || k.textChar == '=' || k.textChar == '-' || k.textChar == '_'))
    {
        zoomTime(notesArea().x + notesArea().w * 0.5f, k.textChar == '+' || k.textChar == '=' ? 1.25 : 0.8);
        repaint();
        return true;
    }

    if (k.cmd && ! k.alt && ! k.ctrl && (k.textChar == 'a' || k.textChar == 'A'))
    {
        std::set<int> selection;
        for (int i = 0; i < (int) cl->notes.size(); ++i)
            if (! k.shift || ! selected_.contains(i)) selection.insert(i);
        selected_ = std::move(selection);
        timeRange_ = false;
        repaint();
        return true;
    }
    std::erase_if(selected_, [&](int index) { return index < 0 || (size_t) index >= cl->notes.size(); });
    if (k.cmd && ! k.alt && ! k.shift && (k.textChar == 'l' || k.textChar == 'L') && ! selected_.empty())
    {
        // Loop Selection: the clip's loop brace becomes the span of the selected notes, and looping is switched on.
        double first = 1e18, last = -1e18;
        for (int i : selected_)
        {
            first = std::min(first, cl->notes[(size_t) i].start);
            last = std::max(last, cl->notes[(size_t) i].start + cl->notes[(size_t) i].duration);
        }
        auto changed = *cl;
        changed.loop.loopStart = first;
        changed.loop.loopEnd = last;
        changed.loop.startRelative = 0.0;
        changed.loop.enabled = true;
        x.applyEdit(model::modifyClip(*x.song, x.selectedClipLocation(), std::move(changed), "Loop Selection"));
        repaint();
        return true;
    }
    if (k.cmd && ! k.alt && (k.textChar == 'g' || k.textChar == 'G'))
    { x.statusText = "Note grouping is not implemented"; return true; }
    if (k.cmd && k.shift && ! k.alt && (k.textChar == 'u' || k.textChar == 'U'))
    {
        if (parent()) for (const auto& child : parent()->children())
            if (auto* panel = dynamic_cast<ClipControls*>(child.get())) panel->showTool(midiedit::Tool::Quantize);
        repaint(); return true;
    }
    if (k.cmd && ! k.alt && ! k.shift && (k.textChar == 'd' || k.textChar == 'D') && (timeRange_ || ! selected_.empty()))
    {
        double first = 1e18, end = 0;
        for (int i : selected_)
        { first = std::min(first, cl->notes[(size_t) i].start); end = std::max(end, cl->notes[(size_t) i].start + cl->notes[(size_t) i].duration); }
        auto duplicated = cl->notes;
        const double span = timeRange_ ? rangeEnd_ - rangeStart_ : std::max(grid(), end - first);
        if (span <= 1e-9) return true;
        std::vector<model::MidiNote> copies;
        const auto indices = selected_; selected_.clear();
        for (int i : indices)
        {
            auto copy = cl->notes[(size_t) i];
            if (timeRange_)
            {
                const double start = std::max(rangeStart_, copy.start), stop = std::min(rangeEnd_, copy.start + copy.duration);
                if (stop <= start) continue;
                copy.start = start; copy.duration = stop - start;
            }
            copy.start += span; copy.noteId = -1;
            copies.push_back(copy); duplicated.push_back(copy);
        }
        edit(std::move(duplicated), "Duplicate Notes");
        if (timeRange_) { rangeStart_ = rangeEnd_; rangeEnd_ += span; }
        if (const auto* after = clip())
            for (int i = 0; i < (int) after->notes.size(); ++i)
                for (const auto& copy : copies)
                    if (after->notes[(size_t) i].pitch == copy.pitch && std::abs(after->notes[(size_t) i].start - copy.start) < 1e-9)
                        selected_.insert(i);
        revealSelection(); repaint(); return true;
    }
    if (! k.cmd && ! k.alt && ! k.ctrl && k.textChar == '0')
    {
        auto changed = cl->notes;
        const bool enable = std::any_of(selected_.begin(), selected_.end(), [&](int i) { return ! changed[(size_t) i].enabled; });
        for (int i : selected_) changed[(size_t) i].enabled = enable;
        if (! selected_.empty()) edit(std::move(changed), "Activate/Deactivate Notes");
        repaint(); return true;
    }
    if (selected_.empty())
        // Delete with nothing selected in the note editor must not fall through and delete the whole clip.
        return k.keyCode == juce::KeyPress::deleteKey || k.keyCode == juce::KeyPress::backspaceKey;

    auto ns = cl->notes;
    std::vector<int> sel(selected_.begin(), selected_.end());
    if (! k.cmd && ! k.alt && ! k.ctrl && ! k.shift && (k.keyCode == juce::KeyPress::deleteKey || k.keyCode == juce::KeyPress::backspaceKey))
    {
        std::vector<model::MidiNote> kept;
        for (int i = 0; i < (int) ns.size(); ++i)
            if (! isSelected(i))
                kept.push_back(ns[(size_t) i]);
        selected_.clear();
        edit(std::move(kept), "Delete Notes");
        return true;
    }
    if (k.keyCode == juce::KeyPress::upKey || k.keyCode == juce::KeyPress::downKey)
    {
        if (k.alt) return true;
        if (k.cmd)
        {
            if (k.shift) { x.statusText = "Velocity deviation editing is not implemented"; return true; }
            for (int i : sel) ns[(size_t) i].velocity = std::clamp(ns[(size_t) i].velocity + (k.keyCode == juce::KeyPress::upKey ? 1.0f : -1.0f), 1.0f, 127.0f);
            edit(std::move(ns), "Change Note Velocity"); repaint(); return true;
        }
        const int d = (k.keyCode == juce::KeyPress::upKey ? 1 : -1) * (k.shift ? 12 : 1);
        for (int i : sel)
            ns[(size_t) i].pitch = std::clamp(ns[(size_t) i].pitch + d, 0, 127);
        std::vector<model::MidiNote> keep;
        for (int i : sel)
            keep.push_back(ns[(size_t) i]);
        edit(std::move(ns), "Transpose Notes");
        selected_.clear();
        const auto* after = clip();
        for (int i = 0; after != nullptr && i < (int) after->notes.size(); ++i)
            for (const auto& kk : keep)
                if (after->notes[(size_t) i].pitch == kk.pitch && std::fabs(after->notes[(size_t) i].start - kk.start) < 1e-9)
                    selected_.insert(i);
        revealSelection(); // the transposed notes stay in view
        repaint();
        return true;
    }
    if (k.keyCode == juce::KeyPress::leftKey || k.keyCode == juce::KeyPress::rightKey)
    {
        if (k.alt || k.cmd) return true;
        const double d = (k.keyCode == juce::KeyPress::rightKey ? 1.0 : -1.0) * grid();
        std::vector<model::MidiNote> keep;
        for (int i : sel)
        {
            ns[(size_t) i].start = std::max(0.0, ns[(size_t) i].start + d);
            keep.push_back(ns[(size_t) i]);
        }
        edit(std::move(ns), "Nudge Notes");
        selected_.clear();
        const auto* after = clip();
        for (int i = 0; after != nullptr && i < (int) after->notes.size(); ++i)
            for (const auto& kk : keep)
                if (after->notes[(size_t) i].pitch == kk.pitch && std::fabs(after->notes[(size_t) i].start - kk.start) < 1e-9)
                    selected_.insert(i);
        revealSelection();
        repaint();
        return true;
    }
    if (k.cmd && ! k.alt && (k.textChar == 'u' || k.textChar == 'U'))
    {
        std::vector<model::MidiNote> keep;
        for (int i : sel)
        {
            ns[(size_t) i].start = snapToGrid(ns[(size_t) i].start, grid());
            keep.push_back(ns[(size_t) i]);
        }
        edit(std::move(ns), "Quantize Notes");
        selected_.clear();
        const auto* after = clip();
        for (int i = 0; after != nullptr && i < (int) after->notes.size(); ++i)
            for (const auto& kk : keep)
                if (after->notes[(size_t) i].pitch == kk.pitch && std::fabs(after->notes[(size_t) i].start - kk.start) < 1e-9)
                    selected_.insert(i);
        return true;
    }
    return false;
}

} // namespace sonora::ui
