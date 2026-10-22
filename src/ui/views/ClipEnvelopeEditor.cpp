#include "ui/views/ClipEnvelopeEditor.h"

#include "ui/WheelScale.h"

#include "ui/GridModel.h"
#include "engine/Automation.h"
#include "ui/ClipEditing.h"
#include "ui/Painter.h"
#include "ui/UiContext.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <algorithm>
#include <cmath>
#include <limits>

namespace sonora::ui
{
namespace
{
bool sameLocation(model::ClipLocation a, model::ClipLocation b)
{
    return a.kind == b.kind && a.track == b.track && a.uid == b.uid && a.scene == b.scene && a.laneId == b.laneId;
}
bool sameEnvelope(const model::AutomationEnvelope& a, const model::AutomationEnvelope& b)
{
    if (a.targetId != b.targetId || a.midiController != b.midiController || a.points.size() != b.points.size()) return false;
    for (size_t i = 0; i < a.points.size(); ++i)
        if (std::abs(a.points[i].time - b.points[i].time) > 1e-12 || std::abs(a.points[i].value - b.points[i].value) > 1e-12) return false;
    return true;
}
std::string lower(std::string value)
{
    for (auto& c : value) c = (char) std::tolower((unsigned char) c);
    return value;
}
}

std::string ClipEnvelopeEditor::controllerName(int index)
{
    if (index == 0) return "Pitch Bend";
    if (index == 1) return "Channel Pressure";
    const int cc = index - 2;
    std::string label = std::to_string(cc) + ": ";
    switch (cc)
    {
        case 1: return label + "Modulation Wheel";
        case 7: return label + "Volume";
        case 10: return label + "Pan";
        case 11: return label + "Expression";
        case 64: return label + "Sustain Pedal";
        case 66: return label + "Sostenuto";
        case 69: return label + "Hold 2";
        default: return label + "CC " + std::to_string(cc);
    }
}

Rect ClipEnvelopeEditor::plot() const { return {40, 20, std::max(1.0f, width() - 48), std::max(1.0f, height() - 48)}; }
Rect ClipEnvelopeEditor::chooser() const { return {40, height() - 23, std::min(230.0f, width() - 40), 20}; }
Rect ClipEnvelopeEditor::menu() const { return {40, 20, std::min(270.0f, width() - 40), std::max(1.0f, height() - 45)}; }
int ClipEnvelopeEditor::menuRows() const { return std::max(1, (int) ((menu().h - 20) / 18)); }
float ClipEnvelopeEditor::beatX(double beat) const { return plot().x + (float) ((beat - scrollBeats) * pixelsPerBeat); }
double ClipEnvelopeEditor::xBeat(float x) const { return scrollBeats + (x - plot().x) / pixelsPerBeat; }
float ClipEnvelopeEditor::valueY(double value) const
{
    const double min = ctx().clipEnvelopeController == 0 ? -8192 : 0, max = ctx().clipEnvelopeController == 0 ? 8191 : 127;
    return plot().bottom() - 2 - (float) std::clamp((value - min) / (max - min), 0.0, 1.0) * (plot().h - 4);
}
double ClipEnvelopeEditor::yValue(float y) const
{
    const double min = ctx().clipEnvelopeController == 0 ? -8192 : 0, max = ctx().clipEnvelopeController == 0 ? 8191 : 127;
    return std::round(min + std::clamp((plot().bottom() - 2 - y) / (plot().h - 4), 0.0f, 1.0f) * (max - min));
}
double ClipEnvelopeEditor::grid() const
{
    const double bar = ctx().song ? ctx().song->timeSigNumerator * 4.0 / ctx().song->timeSigDenominator : 4.0;
    return ui::gridBeats(ctx().clipGrid, pixelsPerBeat, bar);
}

model::AutomationEnvelope ClipEnvelopeEditor::envelope() const
{
    if (const auto* clip = ctx().selectedClip())
        for (const auto& env : clip->envelopes) if (env.midiController == ctx().clipEnvelopeController) return env;
    model::AutomationEnvelope env;
    env.midiController = ctx().clipEnvelopeController;
    env.points.push_back({-63072000, (double) model::midiControllerDefault(env.midiController)});
    return env;
}

void ClipEnvelopeEditor::syncClip()
{
    const auto* clip = ctx().selectedClip();
    if (! clip) { cancel(); return; }
    if (! sameLocation(lastLocation_, ctx().selectedClipLocation()))
    {
        cancel(); lastLocation_ = ctx().selectedClipLocation();
        scrollBeats = std::max(0.0, clip->loop.loopStart);
        pixelsPerBeat = std::clamp(plot().w / std::max(1.0, clip->loop.loopEnd - scrollBeats), 16.0, 400.0);
        if (! model::isMidiController(ctx().clipEnvelopeController))
        {
            ctx().clipEnvelopeController = 0;
            for (const auto& env : clip->envelopes) if (model::isMidiController(env.midiController))
            { ctx().clipEnvelopeController = env.midiController; break; }
        }
        observed_ = envelope();
    }
    const auto current = envelope();
    if (editing_ && (! sameLocation(location_, ctx().selectedClipLocation()) || ! sameEnvelope(current, original_))) cancel();
    if (! editing_ && ! sameEnvelope(current, observed_)) { selected_.clear(); observed_ = current; }
}

std::vector<int> ClipEnvelopeEditor::choices() const
{
    std::vector<int> out;
    const auto search = lower(search_);
    for (int i = 0; i < 130; ++i) if (lower(controllerName(i)).find(search) != std::string::npos) out.push_back(i);
    return out;
}

void ClipEnvelopeEditor::choose(int index)
{
    cancel(); ctx().clipEnvelopeController = index; selected_.clear(); observed_ = envelope(); menuOpen_ = false; repaint();
}

void ClipEnvelopeEditor::paint(SkCanvas& canvas, const Theme& theme)
{
    syncClip();
    paint::fill(canvas, localBounds(), theme.color("SurfaceArea"));
    const auto* clip = ctx().selectedClip();
    if (! clip || clip->kind != model::Clip::Kind::Midi)
    {
        paint::text(canvas, "Select a MIDI clip to edit its envelopes", localBounds(), theme.color("TextDisabled"), theme.metrics.fontSize, Align::Centre);
        return;
    }
    const auto env = editing_ ? working_ : envelope();
    const engine::AutomationLane lane(env);
    const auto area = plot();
    const auto ink = theme.color("AutomationColor");
    const double last = xBeat(area.right());
    const double beatsPerBar = ctx().song->timeSigNumerator * 4.0 / ctx().song->timeSigDenominator;
    canvas.save(); canvas.clipRect(area.sk());
    for (double beat = std::floor(scrollBeats / grid()) * grid(); beat <= last; beat += grid())
        paint::line(canvas, beatX(beat), area.y, beatX(beat), area.bottom(),
                    paint::withAlpha(theme.color("GridLineBase"), std::abs(std::remainder(beat, beatsPerBar)) < 1e-8 ? 255 : 75));
    for (double value : ctx().clipEnvelopeController == 0 ? std::vector<double>{-8192, -4096, 0, 4096, 8191} : std::vector<double>{0, 32, 64, 96, 127})
        paint::line(canvas, area.x, valueY(value), area.right(), valueY(value), paint::withAlpha(theme.color("GridLineBase"), 100));
    // Notes stay visible as context; editing this view never changes their data.
    int lo = 127, hi = 0;
    for (const auto& note : clip->notes) { lo = std::min(lo, note.pitch); hi = std::max(hi, note.pitch); }
    for (const auto& note : clip->notes)
    {
        const float y = area.y + 8 + (float) (hi - note.pitch) / (float) std::max(1, hi - lo + 1) * (area.h - 16);
        paint::fill(canvas, {beatX(note.start), y, std::max(2.0f, (float) (note.duration * pixelsPerBeat)), 5},
                    paint::withAlpha(theme.color("TextDisabled"), 65));
    }
    for (double boundary : {clip->loop.loopStart, clip->loop.loopEnd})
        paint::line(canvas, beatX(boundary), area.y, beatX(boundary), area.bottom(), theme.color("LoopColor"));
    // Curved segments (ENG-11b) are drawn as short chords of the lane's own curve.
    const bool curved = std::any_of(env.points.begin(), env.points.end(), [](const auto& p) { return p.curved(); });
    auto segment = [&](double t0, double v0, double t1, double v1) {
        const int steps = curved ? std::clamp((int) ((t1 - t0) * pixelsPerBeat / 3.0), 1, 64) : 1;
        for (int k = 1; k <= steps; ++k)
        {
            const double t = k == steps ? t1 : t0 + (t1 - t0) * k / steps, v = k == steps ? v1 : lane.valueAt(t);
            paint::line(canvas, beatX(t0), valueY(v0), beatX(t), valueY(v), ink);
            t0 = t; v0 = v;
        }
    };
    double previousTime = scrollBeats, previousValue = lane.valueAt(scrollBeats);
    for (const auto& point : env.points)
    {
        if (point.time < scrollBeats || point.time > last) continue;
        segment(previousTime, previousValue, point.time, point.value);
        previousTime = point.time; previousValue = point.value;
    }
    segment(previousTime, previousValue, last, lane.valueAt(last));
    for (size_t i = 0; i < env.points.size(); ++i)
        if (env.points[i].time >= scrollBeats && env.points[i].time <= last)
            paint::fill(canvas, {beatX(env.points[i].time) - 3, valueY(env.points[i].value) - 3, 6, 6},
                        selected_.contains((int) i) ? theme.color("SelectionBackground") : ink);
    canvas.restore();
    for (double beat = std::ceil(scrollBeats); beat <= last; beat += 1)
        if (pixelsPerBeat >= 40 || std::abs(std::remainder(beat, beatsPerBar)) < 1e-8)
            paint::text(canvas, std::to_string((int) std::floor(beat / beatsPerBar) + 1) + "." + std::to_string((int) std::floor(std::fmod(beat, beatsPerBar)) + 1),
                        {beatX(beat) + 3, 0, 70, 18}, theme.color("ArrangementRulerMarkings"), theme.metrics.smallFontSize);
    const int min = ctx().clipEnvelopeController == 0 ? -8192 : 0, max = ctx().clipEnvelopeController == 0 ? 8191 : 127;
    paint::text(canvas, std::to_string(max), {0, area.y - 2, 36, 16}, theme.color("ControlForeground"), theme.metrics.smallFontSize, Align::Right);
    paint::text(canvas, std::to_string(min), {0, area.bottom() - 14, 36, 16}, theme.color("ControlForeground"), theme.metrics.smallFontSize, Align::Right);
    if (min < 0) paint::text(canvas, "0", {0, valueY(0) - 8, 36, 16}, theme.color("ControlForeground"), theme.metrics.smallFontSize, Align::Right);
    paint::fill(canvas, chooser(), theme.color("ControlBackground")); paint::stroke(canvas, chooser(), theme.color("ControlContrastFrame"));
    paint::text(canvas, "MIDI Ctrl / " + controllerName(ctx().clipEnvelopeController) + "  v", chooser().reduced(4, 0), theme.color("ControlForeground"), theme.metrics.smallFontSize);
    const Rect draw{chooser().right() + 6, chooser().y, 52, 20}, clear{draw.right() + 6, draw.y, 52, 20};
    paint::fill(canvas, draw, theme.color(ctx().drawMode ? "ChosenDefault" : "ControlBackground"));
    paint::text(canvas, "Draw [B]", draw, ctx().drawMode ? paint::contrastText(theme.color("ChosenDefault")) : theme.color("ControlForeground"), theme.metrics.smallFontSize, Align::Centre);
    paint::fill(canvas, clear, theme.color("ControlBackground")); paint::text(canvas, "Clear", clear, theme.color("ControlForeground"), theme.metrics.smallFontSize, Align::Centre);
    paint::text(canvas, "Linked", {clear.right() + 8, clear.y, 55, 20}, theme.color("ControlForeground"), theme.metrics.smallFontSize);
    if (menuOpen_)
    {
        const auto rect = menu(); const auto list = choices();
        menuOffset_ = std::clamp(menuOffset_, 0, std::max(0, (int) list.size() - menuRows()));
        paint::fill(canvas, rect, theme.color("ControlBackground")); paint::stroke(canvas, rect, theme.color("ControlContrastFrame"));
        paint::text(canvas, search_.empty() ? "Type to find a controller" : search_, {rect.x + 5, rect.y, rect.w - 10, 20}, theme.color("ControlForeground"), theme.metrics.smallFontSize);
        for (int row = 0; row < menuRows() && menuOffset_ + row < (int) list.size(); ++row)
        {
            const int index = menuOffset_ + row;
            const Rect item{rect.x + 1, rect.y + 20 + (float) row * 18, rect.w - 2, 18};
            if (index == menuSelection_) paint::fill(canvas, item, theme.color("SelectionBackground"));
            paint::text(canvas, controllerName(list[(size_t) index]), item.reduced(4, 0),
                        theme.color(index == menuSelection_ ? "SelectionForeground" : "ControlForeground"), theme.metrics.smallFontSize);
        }
        if (list.empty()) paint::text(canvas, "No matching controller", {rect.x + 5, rect.y + 20, rect.w - 10, 18}, theme.color("TextDisabled"), theme.metrics.smallFontSize);
    }
}

int ClipEnvelopeEditor::pointAt(float x, float y) const
{
    const auto env = editing_ ? working_ : envelope();
    int index = -1; float best = 8;
    for (size_t i = 0; i < env.points.size(); ++i)
    {
        if (env.points[i].time < 0) continue;
        const float distance = std::hypot(beatX(env.points[i].time) - x, valueY(env.points[i].value) - y);
        if (distance <= best) { best = distance; index = (int) i; }
    }
    return index;
}

void ClipEnvelopeEditor::beginGesture()
{
    location_ = ctx().selectedClipLocation(); original_ = working_ = envelope();
    editing_ = true; changed_ = false; cells_.clear();
}

void ClipEnvelopeEditor::drawCell(const MouseEvent& event)
{
    const double cell = std::max(0.0, std::floor(xBeat(std::clamp(event.x, plot().x, plot().right())) / grid()) * grid());
    const double value = yValue(event.y);
    // Fill skipped cells when native mouse motion coalesces. Dragging back replaces earlier cells.
    const int count = (int) std::lround(std::abs(cell - lastCell_) / grid());
    for (int i = 0; i <= count; ++i)
        cells_[std::min(cell, lastCell_) + i * grid()] = value;
    lastCell_ = cell;
    working_ = original_;
    const engine::AutomationLane old(original_);
    // Merge adjacent painted cells into intervals; restore exact original jumps outside each interval.
    for (auto first = cells_.begin(); first != cells_.end();)
    {
        auto next = std::next(first), tail = first;
        while (next != cells_.end() && std::abs(next->first - tail->first - grid()) < 1e-8) { tail = next; ++next; }
        const double start = first->first, end = tail->first + grid();
        double left = old.valueAt(std::nextafter(start, -std::numeric_limits<double>::infinity()));
        double right = old.valueAt(end);
        for (const auto& p : original_.points)
            if (std::abs(p.time - start) < 1e-12) { left = p.value; break; }
        // If a jump exists at the right edge, keep its last value after drawing's left limit.
        std::erase_if(working_.points, [start, end](const auto& p) { return p.time >= start && p.time <= end; });
        working_.points.push_back({start, left});
        double previous = first->second;
        working_.points.push_back({start, previous});
        for (auto point = std::next(first); point != next; ++point)
        {
            if (std::abs(point->second - previous) > 1e-12)
            {
                working_.points.push_back({point->first, previous});
                working_.points.push_back({point->first, point->second}); previous = point->second;
            }
        }
        working_.points.push_back({end, previous}); working_.points.push_back({end, right});
        first = next;
    }
    std::stable_sort(working_.points.begin(), working_.points.end(), [](const auto& a, const auto& b) { return a.time < b.time; });
    changed_ = ! sameEnvelope(original_, working_); selected_.clear(); repaint();
}

void ClipEnvelopeEditor::mouseDown(const MouseEvent& event)
{
    focused_ = true;
    syncClip();
    const auto* clip = ctx().selectedClip();
    if (! clip || clip->kind != model::Clip::Kind::Midi) return;
    if (menuOpen_)
    {
        if (menu().contains(event.x, event.y))
        {
            const auto list = choices();
            const int row = (int) ((event.y - menu().y - 20) / 18);
            const int index = menuOffset_ + row;
            if (event.y >= menu().y + 20 && row >= 0 && row < menuRows() && index < (int) list.size()) choose(list[(size_t) index]);
        }
        else { menuOpen_ = false; repaint(); }
        return;
    }
    if (chooser().contains(event.x, event.y))
    {
        menuOpen_ = true; search_.clear(); menuSelection_ = ctx().clipEnvelopeController;
        menuOffset_ = std::clamp(menuSelection_ - menuRows() / 2, 0, 130 - menuRows()); repaint(); return;
    }
    const Rect draw{chooser().right() + 6, chooser().y, 52, 20}, clear{draw.right() + 6, draw.y, 52, 20};
    if (draw.contains(event.x, event.y)) { ctx().drawMode = ! ctx().drawMode; repaint(); return; }
    if (! ctx().applyEdit || ! ctx().song) return;
    if (clear.contains(event.x, event.y))
    {
        auto env = envelope(); env.points.clear();
        auto command = model::setClipMidiEnvelope(*ctx().song, ctx().selectedClipLocation(), env);
        if (command.apply) ctx().applyEdit(std::move(command));
        selected_.clear(); repaint(); return;
    }
    // Include a small hit margin around maximum/minimum endpoints at the plot border.
    if (event.x < plot().x || event.x > plot().right() || event.y < plot().y - 4 || event.y > plot().bottom() + 4) return;
    const int point = pointAt(event.x, event.y);
    if (ctx().drawMode && ! event.right)
    {
        beginGesture(); drawing_ = true;
        lastCell_ = std::max(0.0, std::floor(xBeat(event.x) / grid()) * grid()); drawCell(event); return;
    }
    if (point >= 0)
    {
        if (event.shift)
        {
            if (selected_.contains(point)) selected_.erase(point); else selected_.insert(point);
            repaint(); return;
        }
        if (! selected_.contains(point)) { selected_.clear(); selected_.insert(point); }
        beginGesture();
        if (event.right || event.clicks >= 2)
        {
            for (auto i = selected_.rbegin(); i != selected_.rend(); ++i) working_.points.erase(working_.points.begin() + *i);
            selected_.clear(); changed_ = true; commit();
        }
        else { grabBeat_ = xBeat(event.x); grabValue_ = yValue(event.y); }
    }
    else
    {
        selected_.clear();
        if (event.clicks >= 2 && ! event.right)
        {
            beginGesture();
            const double time = std::max(0.0, snapToGrid(xBeat(event.x), ctx().clipGrid.snap && ! event.alt && ! event.cmd ? grid() : 0.0));
            working_.points.push_back({time, yValue(event.y)});
            std::stable_sort(working_.points.begin(), working_.points.end(), [](const auto& a, const auto& b) { return a.time < b.time; });
            changed_ = true; commit();
        }
    }
    repaint();
}

void ClipEnvelopeEditor::mouseDrag(const MouseEvent& event)
{
    if (! editing_) return;
    if (drawing_) { drawCell(event); return; }
    if (selected_.empty()) return;
    double delta = xBeat(event.x) - grabBeat_;
    const int anchor = *selected_.begin();
    if (ctx().clipGrid.snap && ! event.alt && ! event.cmd) delta = snapToGrid(original_.points[(size_t) anchor].time + delta, grid()) - original_.points[(size_t) anchor].time;
    double low = -original_.points[(size_t) anchor].time, high = std::numeric_limits<double>::infinity();
    // Points cannot cross unselected neighbours; meeting them retains duplicate order for steps.
    for (int index : selected_)
    {
        const auto& point = original_.points[(size_t) index];
        if (index > 0 && ! selected_.contains(index - 1)) low = std::max(low, original_.points[(size_t) index - 1].time - point.time);
        if ((size_t) index + 1 < original_.points.size() && ! selected_.contains(index + 1)) high = std::min(high, original_.points[(size_t) index + 1].time - point.time);
    }
    delta = std::clamp(delta, low, high);
    double valueDelta = yValue(event.y) - grabValue_;
    if (event.shift) valueDelta = std::round(valueDelta * 0.1);
    working_ = original_;
    for (int index : selected_)
    {
        const auto& point = original_.points[(size_t) index];
        working_.points[(size_t) index] = {point.time + delta, (double) model::quantiseMidiController(original_.midiController, point.value + valueDelta)};
    }
    changed_ = ! sameEnvelope(original_, working_); repaint();
}

void ClipEnvelopeEditor::commit()
{
    if (editing_ && changed_ && ctx().song && ctx().applyEdit && sameLocation(location_, ctx().selectedClipLocation())
        && model::findClip(*ctx().song, location_) && sameEnvelope(envelope(), original_))
    {
        auto command = model::setClipMidiEnvelope(*ctx().song, location_, working_);
        if (command.apply) ctx().applyEdit(std::move(command));
        observed_ = envelope();
    }
    editing_ = drawing_ = changed_ = false; cells_.clear(); repaint();
}
void ClipEnvelopeEditor::cancel() { editing_ = drawing_ = changed_ = menuOpen_ = false; cells_.clear(); selected_.clear(); }
void ClipEnvelopeEditor::mouseUp(const MouseEvent&) { commit(); }
void ClipEnvelopeEditor::focusLost() { focused_ = false; cancel(); repaint(); }

bool ClipEnvelopeEditor::mouseWheel(const MouseEvent& event)
{
    if (menuOpen_)
    {
        menuOffset_ = std::clamp(menuOffset_ - (int) std::lround(event.wheelDeltaY * 8), 0, std::max(0, (int) choices().size() - menuRows()));
        repaint(); return true;
    }
    if (editing_) return true;
    if (event.cmd)
    {
        const double anchor = xBeat(event.x);
        pixelsPerBeat = std::clamp(pixelsPerBeat * wheelZoomFactor(event.wheelDeltaY), 8.0, 800.0);
        scrollBeats = std::max(0.0, anchor - (event.x - plot().x) / pixelsPerBeat);
    }
    else scrollBeats = std::max(0.0, scrollBeats - (event.wheelDeltaX + event.wheelDeltaY) * kWheelPixels / pixelsPerBeat);
    repaint(); return true;
}

bool ClipEnvelopeEditor::keyPressed(const KeyEvent& key)
{
    if (! focused_) return false;
    syncClip();
    if (! ctx().selectedClip() || ctx().selectedClip()->kind != model::Clip::Kind::Midi) return false;
    if (menuOpen_)
    {
        if (key.keyCode == juce::KeyPress::escapeKey) { menuOpen_ = false; repaint(); return true; }
        auto list = choices();
        if (key.keyCode == juce::KeyPress::returnKey)
        { if (! list.empty()) choose(list[(size_t) std::clamp(menuSelection_, 0, (int) list.size() - 1)]); return true; }
        if (key.keyCode == juce::KeyPress::upKey || key.keyCode == juce::KeyPress::downKey)
        {
            menuSelection_ = std::clamp(menuSelection_ + (key.keyCode == juce::KeyPress::upKey ? -1 : 1), 0, std::max(0, (int) list.size() - 1));
            if (menuSelection_ < menuOffset_) menuOffset_ = menuSelection_;
            if (menuSelection_ >= menuOffset_ + menuRows()) menuOffset_ = menuSelection_ - menuRows() + 1;
        }
        else if (key.keyCode == juce::KeyPress::backspaceKey)
        { if (! search_.empty()) search_.pop_back(); menuSelection_ = menuOffset_ = 0; }
        else if (! key.cmd && key.textChar >= 32 && key.textChar < 127)
        { search_ += (char) key.textChar; menuSelection_ = menuOffset_ = 0; }
        repaint(); return true;
    }
    if (key.keyCode == juce::KeyPress::escapeKey) { cancel(); repaint(); return true; }
    if (! key.cmd && (key.textChar == 'b' || key.textChar == 'B')) { ctx().drawMode = ! ctx().drawMode; repaint(); return true; }
    if (key.cmd && (key.textChar == 'a' || key.textChar == 'A'))
    {
        selected_.clear(); const auto env = envelope();
        for (size_t i = 0; i < env.points.size(); ++i) if (env.points[i].time >= 0) selected_.insert((int) i);
        repaint(); return true;
    }
    if (key.keyCode == juce::KeyPress::deleteKey || key.keyCode == juce::KeyPress::backspaceKey)
    {
        if (! selected_.empty() && ctx().applyEdit && ctx().song)
        {
            beginGesture();
            for (auto i = selected_.rbegin(); i != selected_.rend(); ++i) working_.points.erase(working_.points.begin() + *i);
            selected_.clear(); changed_ = true; commit();
        }
        return true; // local focus must not remove a Session/take/Arrangement clip
    }
    return false;
}
} // namespace sonora::ui
