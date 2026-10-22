#include "ui/views/AutomationEditor.h"

#include "ui/GridModel.h"

#include "engine/Automation.h"
#include "engine/EngineGraph.h"
#include "ui/ClipEditing.h"
#include "ui/Painter.h"
#include "ui/UiContext.h"
#include "ui/Controls.h"
#include "ui/views/ArrangementView.h"
#include "ui/MenuLookAndFeel.h"
#include "core/DeviceCatalog.h"
#include "ui/views/ClipEnvelopeEditor.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace sonora::ui
{

std::vector<AutomationEditor::Target> AutomationEditor::targets() const
{
    std::vector<Target> out;
    const auto* track = ctx().trackAt(track_);
    if (track == nullptr) return out;
    auto add = [&](int id, std::string name, double min, double max, double base,
                   std::string type = "FloatEvent") {
        if (id < 0 || std::any_of(out.begin(), out.end(), [id](const Target& t) { return t.id == id; })) return;
        const auto split = name.find(" / ");
        const std::string group = split == std::string::npos ? "Unmapped Targets" : name.substr(0, split);
        if (split != std::string::npos) name.erase(0, split + 3);
        out.push_back({id, std::move(name), type, min, max, base, type != "FloatEvent", group});
    };
    const auto& mx = track->mixer;
    const bool audio = ! model::isMidiOnly(*track);
    if (audio)
    {
        add(mx.volumeTargetId, "Mixer / Track Volume", 0, 1.995262, mx.volume);
        add(mx.panTargetId, "Mixer / Track Panning", -1, 1, mx.pan);
    }
    add(mx.speakerTargetId, "Mixer / Track Activator", 0, 1, mx.speakerOn ? 1 : 0, "BoolEvent");
    add(mx.tempoTargetId, "Song / Tempo", 20, 999, ctx().song->tempo);
    for (size_t i = 0; audio && i < mx.sendTargetIds.size(); ++i)
        add(mx.sendTargetIds[i], "Mixer / Send " + std::string(1, (char) ('A' + (int) i)), 0, 1,
            i < mx.sends.size() ? mx.sends[i] : 0);
    for (size_t i = 0; i < track->devices.size(); ++i)
    {
        const auto& dev = track->devices[i];
        const std::string prefix = std::to_string(i + 1) + " " + (dev.displayName.empty() ? dev.type : dev.displayName) + " / ";
        add(dev.enabledTargetId, prefix + "On", 0, 1, dev.enabled ? 1 : 0, "BoolEvent");
        const devices::DeviceProcessor* proc = nullptr;
        if (ctx().graph && (size_t) track_ < ctx().graph->nodes.size())
        {
            const auto& processors = ctx().graph->nodes[(size_t) track_].processorForModelDevice;
            if (i < processors.size()) proc = processors[i];
        }
        if (proc)
            for (const auto& p : dev.parameters)
                for (const auto& info : proc->parameters())
                    if (p.path == info.id)
                        add(p.automationTargetId, prefix + info.name, info.minValue, info.maxValue, p.value,
                            info.isToggle ? "BoolEvent" : info.choices.empty() ? "FloatEvent" : "EnumEvent");
        for (const auto& p : dev.pluginParams)
            add(p.automationTargetId, prefix + p.name, 0, 1, p.value);
        for (const auto& p : dev.parameters)
            add(p.automationTargetId, prefix + p.path, std::min(0.0, p.value), std::max(1.0, p.value), p.value);
    }
    // Keep every parsed lane accessible, including targets without a processor. Their range is inferred.
    for (const auto& env : track->automation)
    {
        double min = 0, max = 1;
        for (const auto& p : env.points) { min = std::min(min, p.value); max = std::max(max, p.value); }
        const auto name = model::isMidiController(env.midiController)
            ? "MIDI Ctrl / " + ClipEnvelopeEditor::controllerName(env.midiController) : "Target " + std::to_string(env.targetId);
        add(env.targetId, name, min, max,
            env.points.empty() ? 0 : env.points.front().value, env.eventType);
    }
    return out;
}

model::AutomationEnvelope AutomationEditor::envelope(const Target& target) const
{
    const auto* track = ctx().trackAt(track_);
    if (track)
        for (const auto& env : track->automation)
            if (env.targetId == target.id) return env;
    model::AutomationEnvelope env;
    env.targetId = target.id;
    env.eventType = target.eventType;
    env.stepped = target.stepped;
    env.points.push_back({-63072000.0, target.base});
    return env;
}

Rect AutomationEditor::plot() const
{
    return {0, 5, std::max(1.0f, width() - ctx().arrangementHeaderWidth()), height() - 10};
}

float AutomationEditor::beatX(double beat) const
{
    return (float) ((beat - ctx().scrollBeats) * ctx().pixelsPerBeat);
}

float AutomationEditor::valueY(double value, const Target& target) const
{
    const auto r = plot();
    return r.bottom() - (float) std::clamp((value - target.min) / std::max(1e-9, target.max - target.min), 0.0, 1.0) * r.h;
}

void AutomationEditor::paint(SkCanvas& c, const Theme& t)
{
    paint::fill(c, localBounds(), t.color("SurfaceArea"));
    const auto ts = targets();
    if (ts.empty())
    {
        paint::text(c, "No automation targets", localBounds(), t.color("TextDisabled"), t.metrics.smallFontSize);
        return;
    }
    auto it = std::find_if(ts.begin(), ts.end(), [&](const Target& a) { return a.id == targetId_; });
    if (it == ts.end())
    {
        // Prefer a lane that already has points when this track first opens.
        const auto* track = ctx().trackAt(track_);
        if (track && ! track->automation.empty())
            it = std::find_if(ts.begin(), ts.end(), [&](const Target& a) { return a.id == track->automation.front().targetId; });
        if (it == ts.end()) it = ts.begin();
        targetId_ = it->id;
    }
    const auto& target = *it;
    const auto env = dragging_ || drawing_ ? preview_ : envelope(target);
    const engine::AutomationLane lane(env);
    const auto r = plot();
    c.save();
    c.clipRect(Rect{0, 0, r.w, height()}.sk());
    const double first = ctx().scrollBeats, last = first + r.w / ctx().pixelsPerBeat;
    const double bar = ctx().song ? ctx().song->timeSigNumerator * 4.0 / ctx().song->timeSigDenominator : 4.0;
    const double unit = ctx().song ? 4.0 / std::max(1, ctx().song->timeSigDenominator) : 1.0;
    const double grid = ui::gridBeats(ctx().arrangementGrid, ctx().pixelsPerBeat, bar);
    const double fade = std::clamp((grid * ctx().pixelsPerBeat - 3.0) / 9.0, 0.0, 1.0);
    for (const auto& line : ui::gridLines(first, last, ctx().pixelsPerBeat, bar, unit, grid))
        paint::line(c, beatX(line.beat), 0, beatX(line.beat), height(),
                    paint::withAlpha(t.color("GridLineBase"), line.strength == 2 ? (uint8_t) 255 : line.strength == 1 ? (uint8_t) 120 : (uint8_t) (60 * fade)));
    const auto ink = t.color("AutomationColor");
    // Curved segments (ENG-11b) are drawn as short chords of the lane's own curve.
    const bool curved = ! env.stepped && std::any_of(env.points.begin(), env.points.end(), [](const auto& p) { return p.curved(); });
    auto segment = [&](double t0, double v0, double t1, double v1) {
        const int steps = curved ? std::clamp((int) ((t1 - t0) * ctx().pixelsPerBeat / 3.0), 1, 64) : 1;
        for (int k = 1; k <= steps; ++k)
        {
            const double time = k == steps ? t1 : t0 + (t1 - t0) * k / steps, v = k == steps ? v1 : lane.valueAt(time);
            paint::line(c, beatX(t0), valueY(v0, target), beatX(time), valueY(v, target), ink);
            t0 = time; v0 = v;
        }
    };
    double prevBeat = first, prevValue = lane.valueAt(first);
    for (const auto& p : env.points)
    {
        if (p.time < first || p.time > last) continue;
        if (env.stepped)
        {
            paint::line(c, beatX(prevBeat), valueY(prevValue, target), beatX(p.time), valueY(prevValue, target), ink);
            paint::line(c, beatX(p.time), valueY(prevValue, target), beatX(p.time), valueY(p.value, target), ink);
        }
        else
            segment(prevBeat, prevValue, p.time, p.value);
        prevBeat = p.time;
        prevValue = p.value;
    }
    if (env.stepped)
        paint::line(c, beatX(prevBeat), valueY(prevValue, target), r.w, valueY(lane.valueAt(last), target), ink);
    else
        segment(prevBeat, prevValue, last, lane.valueAt(last));
    for (size_t i = 0; i < env.points.size(); ++i)
    {
        const auto& p = env.points[i];
        if (p.time < first || p.time > last) continue;
        paint::fill(c, {beatX(p.time) - 3, valueY(p.value, target) - 3, 6, 6},
                    (int) i == selectedPoint_ && ctx().selectedAutomationTrack == track_ ? t.color("SelectionBackground") : ink);
    }
    c.restore();

    // Lane header (UI-46, Live's automation lane): the track's name column, tinted with its colour, holds the device and
    // parameter choosers stacked like the track header's rows; the I/O and mixer columns stay empty panel.
    const Rect header{r.w, 0, width() - r.w, height()};
    paint::fill(c, header, t.color("SurfaceBackground"));
    const auto* track = ctx().trackAt(track_);
    const SkColor trackColour = track && track->kind != model::TrackKind::Main ? Theme::paletteColor(track->colorIndex) : t.color("DisplayBackground");
    const Rect title{header.x, 0, arrangementTitleWidth(ctx()), height() - 1};
    paint::fill(c, title, paint::mix(trackColour, t.color("SurfaceBackground"), 0.55f));
    paint::fill(c, {title.right(), 0, 2, height()}, t.color("SurfaceArea"));
    ctl::dropdown(c, t, chooserBounds(true), target.group);
    ctl::dropdown(c, t, chooserBounds(false), target.name);
    char range[64];
    std::snprintf(range, sizeof(range), "%.3g .. %.3g", target.min, target.max);
    if (target.name == "Track Volume" && target.group == "Mixer") std::snprintf(range, sizeof(range), "-\xE2\x88\x9E .. +6 dB");
    else if (target.name == "Track Panning" && target.group == "Mixer") std::snprintf(range, sizeof(range), "50L .. C .. 50R");
    else if (target.eventType == "BoolEvent") std::snprintf(range, sizeof(range), "Off / On");
    const auto device = chooserBounds(false);
    paint::text(c, range, {device.x + 1, device.bottom() + 3, device.w - 2, 14}, paint::contrastText(paint::mix(trackColour, t.color("SurfaceBackground"), 0.55f)),
                t.metrics.smallFontSize);
    paint::line(c, 0, height() - 0.5f, width(), height() - 0.5f, t.color("ControlContrastFrame"));
}

Rect AutomationEditor::chooserBounds(bool device) const
{
    // Inside the name column, under each other, with the track header's 3 px inset.
    const float x = plot().w + 4, w = arrangementTitleWidth(ctx()) - 8;
    return {x, device ? 5.0f : 23.0f, w, 15.0f};
}

void AutomationEditor::showChooser(bool device)
{
    const auto choices = targets();
    if (choices.empty()) return;
    auto selected = std::find_if(choices.begin(), choices.end(), [this](const auto& target) { return target.id == targetId_; });
    if (selected == choices.end()) selected = choices.begin();
    std::vector<std::string> labels, groups;
    std::vector<bool> ticked;
    std::vector<int> ids;
    const auto* track = ctx().trackAt(track_);
    for (const auto& target : choices)
    {
        if (device)
        {
            if (std::find(groups.begin(), groups.end(), target.group) != groups.end()) continue;
            groups.push_back(target.group); labels.push_back(target.group); ids.push_back(target.id);
            ticked.push_back(selected->group == target.group);
        }
        else if (target.group == selected->group)
        {
            const bool automated = track && std::any_of(track->automation.begin(), track->automation.end(), [&](const auto& lane) {
                return lane.targetId == target.id && std::any_of(lane.points.begin(), lane.points.end(), [](const auto& point) { return point.time >= 0; });
            });
            labels.push_back((automated ? "● " : "") + target.name);
            ids.push_back(target.id); ticked.push_back(selected->id == target.id);
        }
    }
    auto pick = [this, ids, song = ctx().song](int index) {
        if (! Widget::isAlive(this) || ctx().song != song || index < 0 || index >= (int) ids.size()) return;
        const auto current = targets();
        if (std::none_of(current.begin(), current.end(), [&](const auto& target) { return target.id == ids[(size_t) index]; })) return;
        targetId_ = ids[(size_t) index]; selectedPoint_ = -1; repaint();
    };
    if (ctx().showChoices) { ctx().showChoices(labels, ticked, std::move(pick)); return; }
    juce::PopupMenu menu;
    for (size_t i = 0; i < labels.size(); ++i) menu.addItem((int) i + 1, labels[i], true, ticked[i]);
    const auto box = chooserBounds(device), origin = boundsInRoot();
    menu.showMenuAsync(dropDownOptions(ctx(), {origin.x + box.x, origin.y + box.y, box.w, box.h}),
                       [pick](int result) { if (result > 0) pick(result - 1); });
}

void AutomationEditor::mouseDown(const MouseEvent& e)
{
    const auto ts = targets();
    if (ts.empty()) return;
    auto it = std::find_if(ts.begin(), ts.end(), [&](const Target& a) { return a.id == targetId_; });
    if (it == ts.end()) it = ts.begin();
    ctx().selectedTrack = ctx().selectedAutomationTrack = track_;
    if (e.x >= plot().w)
    {
        if (chooserBounds(true).contains(e.x, e.y)) showChooser(true);
        else if (chooserBounds(false).contains(e.x, e.y)) showChooser(false);
        return;
    }
    if (! ctx().applyEdit) return;
    dragTarget_ = *it;
    preview_ = envelope(*it);
    selectedPoint_ = -1;
    float best = 9;
    for (size_t i = 0; i < preview_.points.size(); ++i)
    {
        const auto& p = preview_.points[i];
        if (p.time < 0) continue; // the pre-roll value is retained
        const float distance = std::hypot(beatX(p.time) - e.x, valueY(p.value, *it) - e.y);
        if (distance < best) { best = distance; selectedPoint_ = (int) i; }
    }
    changed_ = false;
    if (selectedPoint_ >= 0 && (e.right || e.clicks >= 2))
    {
        preview_.points.erase(preview_.points.begin() + selectedPoint_);
        selectedPoint_ = -1;
        changed_ = true;
        commit();
    }
    else if (selectedPoint_ >= 0)
    {
        dragging_ = true;
    }
    else if (ctx().drawMode && ! e.right)
    {
        drawing_ = true;
        lastCell_ = -1;
        paintCell(e);
    }
    else if (e.clicks >= 2 && ! e.right)
    {
        const double grid = ui::snapSize(ctx().arrangementGrid, ctx().pixelsPerBeat, ctx().song ? ctx().song->timeSigNumerator * 4.0 / ctx().song->timeSigDenominator : 4.0, e.alt || e.cmd);
        const double beat = std::max(0.0, snapToGrid(ctx().scrollBeats + e.x / ctx().pixelsPerBeat, grid));
        const auto r = plot();
        double value = it->min + std::clamp((r.bottom() - e.y) / r.h, 0.0f, 1.0f) * (it->max - it->min);
        if (it->stepped) value = std::round(value);
        preview_.points.push_back({beat, value});
        std::stable_sort(preview_.points.begin(), preview_.points.end(), [](const auto& a, const auto& b) { return a.time < b.time; });
        selectedPoint_ = -1;
        changed_ = true;
        commit();
    }
    repaint();
}

// Draw Mode: the value under the pointer holds for one grid cell; cells crossed by a fast drag are filled in.
void AutomationEditor::paintCell(const MouseEvent& e)
{
    const double bar = ctx().song ? ctx().song->timeSigNumerator * 4.0 / ctx().song->timeSigDenominator : 4.0;
    const double g = ui::gridBeats(ctx().arrangementGrid, ctx().pixelsPerBeat, bar);
    const double cell = std::max(0.0, std::floor((ctx().scrollBeats + e.x / ctx().pixelsPerBeat) / g + 1e-9) * g);
    const auto r = plot();
    double value = dragTarget_.min + std::clamp((r.bottom() - e.y) / r.h, 0.0f, 1.0f) * (dragTarget_.max - dragTarget_.min);
    if (dragTarget_.stepped) value = std::round(value);
    const double from = lastCell_ < 0 ? cell : lastCell_;
    const int steps = std::max(1, (int) std::lround(std::fabs(cell - from) / g));
    const double dir = cell >= from ? 1.0 : -1.0;
    for (int i = 0; i < steps; ++i)
    {
        const double start = std::max(0.0, lastCell_ < 0 ? cell : from + dir * g * (i + 1));
        std::erase_if(preview_.points, [&](const model::AutomationPoint& p) { return p.time >= start - 1e-9 && p.time < start + g - 1e-9 && p.time >= 0; });
        preview_.points.push_back({start, value});
        preview_.points.push_back({start + g, value});
    }
    std::stable_sort(preview_.points.begin(), preview_.points.end(), [](const auto& a, const auto& b) { return a.time < b.time; });
    lastCell_ = cell;
    changed_ = true;
    repaint();
}

void AutomationEditor::mouseDrag(const MouseEvent& e)
{
    if (drawing_) { paintCell(e); return; }
    if (! dragging_ || selectedPoint_ < 0) return;
    auto& p = preview_.points[(size_t) selectedPoint_];
    const auto r = plot();
    const double grid = ui::snapSize(ctx().arrangementGrid, ctx().pixelsPerBeat, ctx().song ? ctx().song->timeSigNumerator * 4.0 / ctx().song->timeSigDenominator : 4.0, e.alt || e.cmd);
    double beat = std::max(0.0, snapToGrid(ctx().scrollBeats + e.x / ctx().pixelsPerBeat, grid));
    // Keep identities stable during a drag; points can meet to create jumps but cannot cross.
    if (selectedPoint_ > 0) beat = std::max(beat, preview_.points[(size_t) selectedPoint_ - 1].time);
    if ((size_t) selectedPoint_ + 1 < preview_.points.size()) beat = std::min(beat, preview_.points[(size_t) selectedPoint_ + 1].time);
    double value = dragTarget_.min + std::clamp((r.bottom() - e.y) / r.h, 0.0f, 1.0f) * (dragTarget_.max - dragTarget_.min);
    if (dragTarget_.stepped) value = std::round(value);
    changed_ |= std::abs(beat - p.time) > 1e-12 || std::abs(value - p.value) > 1e-12;
    p = {beat, value};
    repaint();
}

void AutomationEditor::commit()
{
    if (changed_ && ctx().song && ctx().applyEdit)
        ctx().applyEdit(model::setAutomation(*ctx().song, track_, preview_));
    changed_ = dragging_ = drawing_ = false;
    lastCell_ = -1;
    repaint();
}

void AutomationEditor::mouseUp(const MouseEvent&)
{
    commit();
}

bool AutomationEditor::keyPressed(const KeyEvent& k)
{
    if (ctx().selectedAutomationTrack != track_ || ! ctx().song || ! ctx().applyEdit) return false;
    if (k.keyCode != juce::KeyPress::deleteKey && k.keyCode != juce::KeyPress::backspaceKey) return false;
    const auto ts = targets();
    const auto it = std::find_if(ts.begin(), ts.end(), [&](const Target& a) { return a.id == targetId_; });
    if (it != ts.end() && selectedPoint_ >= 0)
    {
        preview_ = envelope(*it);
        if ((size_t) selectedPoint_ < preview_.points.size())
        {
            preview_.points.erase(preview_.points.begin() + selectedPoint_);
            selectedPoint_ = -1;
            changed_ = true;
            commit();
        }
    }
    return true; // automation focus must not delete the previously selected clip
}

} // namespace sonora::ui
