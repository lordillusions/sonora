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
#include <cstdlib>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace sonora::ui
{

std::vector<AutomationEditor::Target> AutomationEditor::targets(const UiContext& ctx, int trackIndex)
{
    std::vector<Target> out;
    const auto* track = ctx.trackAt(trackIndex);
    if (track == nullptr) return out;
    std::unordered_set<int> seen;
    auto add = [&](int id, std::string name, double min, double max, double base,
                   std::string type = "FloatEvent") -> Target* {
        if (id == -1 || ! seen.insert(id).second) return nullptr;
        const auto split = name.find(" / ");
        const std::string group = split == std::string::npos ? "Unmapped Targets" : name.substr(0, split);
        if (split != std::string::npos) name.erase(0, split + 3);
        out.push_back({id, std::move(name), type, min, max, base, type != "FloatEvent", group});
        return &out.back();
    };
    const auto& mx = track->mixer;
    const bool audio = ! model::isMidiOnly(*track);
    if (audio)
    {
        add(mx.volumeTargetId, "Mixer / Track Volume", 0, 1.995262, mx.volume);
        add(mx.panTargetId, "Mixer / Track Panning", -1, 1, mx.pan);
    }
    add(mx.speakerTargetId, "Mixer / Track Activator", 0, 1, mx.speakerOn ? 1 : 0, "BoolEvent");
    add(mx.tempoTargetId, "Song / Tempo", 20, 999, ctx.song->tempo);
    for (size_t i = 0; audio && i < mx.sendTargetIds.size(); ++i)
        add(mx.sendTargetIds[i], "Mixer / Send " + std::string(1, (char) ('A' + (int) i)), 0, 1,
            i < mx.sends.size() ? mx.sends[i] : 0);
    std::set<int> laneTargets;
    for (const auto& env : track->automation) laneTargets.insert(env.targetId);
    for (size_t i = 0; i < track->devices.size(); ++i)
    {
        const auto& dev = track->devices[i];
        const std::string prefix = std::to_string(i + 1) + " " + (dev.displayName.empty() ? dev.type : dev.displayName) + " / ";
        add(dev.enabledTargetId, prefix + "On", 0, 1, dev.enabled ? 1 : 0, "BoolEvent");
        const devices::DeviceProcessor* proc = nullptr;
        if (ctx.graph && (size_t) trackIndex < ctx.graph->nodes.size())
        {
            const auto& processors = ctx.graph->nodes[(size_t) trackIndex].processorForModelDevice;
            if (i < processors.size()) proc = processors[i];
        }
        if (proc && proc->isHostedPlugin())
        {
            // UI-65: every parameter the loaded plugin lets a host automate, in the plugin's order, whether or not the set
            // lists it yet (Live keeps 128 slots, and plugins Sonora inserted have none).
            std::unordered_map<int, const model::PluginParam*> listed;
            for (const auto& p : dev.pluginParams)
                if (p.parameterId != -1 && p.automationTargetId >= 0) listed.emplace(p.parameterId, &p);
            const auto& infos = proc->parameters();
            out.reserve(out.size() + infos.size());
            for (int index = 0; index < (int) infos.size(); ++index)
            {
                int hostId = -1;
                std::string name;
                if (! proc->hostParameter(index, hostId, name)) continue;
                int id = pendingTargetId((int) i, index);
                double value = 0.0;
                if (const auto found = listed.find(hostId); found != listed.end())
                {
                    id = found->second->automationTargetId;
                    value = found->second->value;
                }
                if (auto* target = add(id, prefix + (name.empty() ? "Parameter " + std::to_string(index + 1) : name), 0, 1, value))
                {
                    target->pluginDevice = (int) i;
                    target->hostId = hostId;
                    target->parameterIndex = index;
                    if (id < -1) target->base = proc->getParameter(index); // what the plugin holds now
                }
            }
        }
        else if (proc)
            for (const auto& p : dev.parameters)
                for (const auto& info : proc->parameters())
                    if (p.path == info.id)
                        add(p.automationTargetId, prefix + info.name, info.minValue, info.maxValue, p.value,
                            info.isToggle ? "BoolEvent" : info.choices.empty() ? "FloatEvent" : "EnumEvent");
        // Listed plugin parameters the instance did not match (not loaded, or a renamed id): only real entries, or the
        // empty slots (ParameterId -1) that still hold a lane.
        for (const auto& p : dev.pluginParams)
            if (p.automationTargetId >= 0 && (p.parameterId != -1 || laneTargets.count(p.automationTargetId)))
                if (auto* target = add(p.automationTargetId, prefix + (p.name.empty() ? "Parameter" : p.name), 0, 1, p.value))
                    target->pluginDevice = (int) i, target->hostId = p.parameterId;
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

bool AutomationEditor::isAutomated(const UiContext& ctx, int track, int targetId)
{
    const auto* t = ctx.trackAt(track);
    return t && targetId >= 0 && std::any_of(t->automation.begin(), t->automation.end(), [&](const auto& lane) {
        return lane.targetId == targetId && std::any_of(lane.points.begin(), lane.points.end(), [](const auto& point) { return point.time >= 0; });
    });
}

std::vector<int> AutomationEditor::automatedTargets(const UiContext& ctx, int track)
{
    std::vector<int> out;
    for (const auto& target : targets(ctx, track))
        if (isAutomated(ctx, track, target.id)) out.push_back(target.id);
    return out;
}

bool AutomationEditor::showAutomatedLanes(UiContext& ctx, const std::vector<int>& tracks)
{
    std::vector<int> shown;
    for (int track : tracks)
    {
        auto automated = automatedTargets(ctx, track);
        if (automated.empty()) continue;
        shown.push_back(track);
        ctx.automationLaneTargets[track] = std::move(automated);
    }
    if (shown.empty()) return false;
    // An empty automationTracks means every track: only the automated ones are listed.
    if (! ctx.automationVisible) { ctx.automationTracks = shown; ctx.automationVisible = true; }
    else if (! ctx.automationTracks.empty())
        for (int track : shown)
            if (std::find(ctx.automationTracks.begin(), ctx.automationTracks.end(), track) == ctx.automationTracks.end())
                ctx.automationTracks.push_back(track);
    return true;
}

void AutomationEditor::toggleAllAutomation(UiContext& ctx)
{
    if (ctx.automationVisible)
    {
        ctx.automationVisible = false;
        ctx.automationTracks.clear();
        ctx.selectedAutomationTrack = -1;
        ctx.statusText = "Automation lanes hidden";
    }
    else
    {
        std::vector<int> all;
        for (int i = 0; i < ctx.numControlTracks(); ++i) all.push_back(i);
        ctx.mainView = MainView::Arrangement;
        if (! showAutomatedLanes(ctx, all)) ctx.statusText = "No automation in this set yet";
        else ctx.statusText = "Showing every automated parameter; Shift+A hides them";
    }
    if (ctx.layoutChanged) ctx.layoutChanged();
}

int AutomationEditor::targetFor(const UiContext& ctx, const midi::ControlTarget& control, int& track)
{
    using K = midi::ControlTarget::Kind;
    track = control.kind == K::Tempo ? ctx.numControlTracks() - 1 : control.track;
    const auto* t = ctx.trackAt(track);
    if (t == nullptr) return -1;
    if (control.kind == K::TrackVolume) return t->mixer.volumeTargetId;
    if (control.kind == K::TrackPan) return t->mixer.panTargetId;
    if (control.kind == K::TrackActivator) return t->mixer.speakerTargetId;
    if (control.kind == K::Tempo) return t->mixer.tempoTargetId;
    if (control.kind == K::TrackSend)
        return (size_t) control.index < t->mixer.sendTargetIds.size() ? t->mixer.sendTargetIds[(size_t) control.index] : -1;
    if (control.kind == K::DeviceOn)
        return (size_t) control.device < t->devices.size() ? t->devices[(size_t) control.device].enabledTargetId : -1;
    if (control.kind != K::DeviceParameter || (size_t) control.device >= t->devices.size()) return -1;
    for (const auto& p : t->devices[(size_t) control.device].parameters)
        if (p.path == control.param && p.automationTargetId >= 0) return p.automationTargetId;
    // A hosted plugin's parameter is named by its index.
    for (const auto& target : targets(ctx, track))
        if (target.pluginDevice == control.device && target.parameterIndex >= 0 && std::to_string(target.parameterIndex) == control.param)
            return target.id;
    return -1;
}

bool isControlAutomated(const UiContext& ctx, const midi::ControlTarget& control)
{
    using K = midi::ControlTarget::Kind;
    if (ctx.song == nullptr) return false;
    const int track = control.kind == K::Tempo ? ctx.numControlTracks() - 1 : control.track;
    const auto* t = ctx.trackAt(track);
    if (t == nullptr || t->automation.empty()) return false;
    int target = -1;
    if (control.kind == K::DeviceParameter)
    {
        // Cheap path for paint: no target list. Native parameters by path, hosted ones by the plugin's id.
        if ((size_t) control.device >= t->devices.size()) return false;
        const auto& device = t->devices[(size_t) control.device];
        for (const auto& p : device.parameters)
            if (p.path == control.param) { target = p.automationTargetId; break; }
        if (target < 0 && ! device.pluginParams.empty() && ctx.graph && (size_t) track < ctx.graph->nodes.size())
        {
            const auto& processors = ctx.graph->nodes[(size_t) track].processorForModelDevice;
            const auto* proc = (size_t) control.device < processors.size() ? processors[(size_t) control.device] : nullptr;
            int hostId = -1;
            std::string name;
            char* end = nullptr;
            const long index = std::strtol(control.param.c_str(), &end, 10);
            if (proc && end && *end == 0 && proc->hostParameter((int) index, hostId, name))
                for (const auto& p : device.pluginParams)
                    if (p.parameterId == hostId && p.parameterId != -1) { target = p.automationTargetId; break; }
        }
    }
    else
    {
        int ignored = -1;
        target = AutomationEditor::targetFor(ctx, control, ignored);
    }
    return AutomationEditor::isAutomated(ctx, track, target);
}

void AutomationEditor::showTarget(UiContext& ctx, int track, int targetId, bool newLane)
{
    if (targetId == -1) return;
    auto& lanes = ctx.automationLaneTargets[track];
    if (! ctx.showsAutomation(track)) ctx.toggleAutomationLane(track);
    if (lanes.empty()) lanes.push_back(-1); // the first lane keeps choosing its own target
    if (! newLane) lanes.front() = targetId;
    else if (std::find(lanes.begin(), lanes.end(), targetId) == lanes.end()) lanes.push_back(targetId);
    ctx.mainView = MainView::Arrangement;
    ctx.selectedTrack = track;
    if (ctx.layoutChanged) ctx.layoutChanged();
}

void AutomationEditor::syncTarget(const std::vector<Target>& ts)
{
    auto has = [&](int id) { return std::any_of(ts.begin(), ts.end(), [id](const Target& a) { return a.id == id; }); };
    const auto lanes = ctx().automationLaneTargets.find(track_);
    if (lanes != ctx().automationLaneTargets.end() && (size_t) lane_ < lanes->second.size() && has(lanes->second[(size_t) lane_]))
    {
        targetId_ = lanes->second[(size_t) lane_];
        return;
    }
    if (has(targetId_)) return;
    // Prefer a lane that already has points when this track first opens.
    const auto automated = automatedTargets(ctx(), track_);
    targetId_ = ! automated.empty() ? automated.front() : ts.empty() ? -1 : ts.front().id;
    if (lanes != ctx().automationLaneTargets.end() && (size_t) lane_ < lanes->second.size()) lanes->second[(size_t) lane_] = targetId_;
}

void AutomationEditor::setTarget(int id)
{
    targetId_ = id;
    selectedPoint_ = -1;
    auto& lanes = ctx().automationLaneTargets[track_];
    if (lanes.size() <= (size_t) lane_) lanes.resize((size_t) lane_ + 1, -1);
    lanes[(size_t) lane_] = id;
}

// Live's lane "+" (fold-out) button: another lane below this one, showing the next automated target this track does not
// show yet (else the next target in chooser order). Cmd: a lane for every automated target.
void AutomationEditor::addLane(bool allAutomated)
{
    if (allAutomated)
    {
        if (! showAutomatedLanes(ctx(), {track_})) ctx().statusText = "This track has no automation yet";
    }
    else
    {
        const auto ts = targets();
        auto& lanes = ctx().automationLaneTargets[track_];
        if (lanes.empty()) lanes.push_back(targetId_);
        auto shown = [&](int id) { return std::find(lanes.begin(), lanes.end(), id) != lanes.end(); };
        int next = -1;
        for (int id : automatedTargets(ctx(), track_)) if (! shown(id)) { next = id; break; }
        if (next == -1)
        {
            const auto here = std::find_if(ts.begin(), ts.end(), [&](const Target& a) { return a.id == targetId_; });
            for (auto it = here == ts.end() ? ts.begin() : here + 1; it != ts.end() && next == -1; ++it) if (! shown(it->id)) next = it->id;
            for (auto it = ts.begin(); it != ts.end() && next == -1; ++it) if (! shown(it->id)) next = it->id;
        }
        if (next == -1) return;
        lanes.insert(lanes.begin() + std::min((ptrdiff_t) lane_ + 1, (ptrdiff_t) lanes.size()), next);
    }
    if (ctx().layoutChanged) ctx().layoutChanged();
}

// Live's lane hide button: hides this lane (its envelope keeps playing). Cmd: this lane and every lane below it. Hiding the
// track's last lane hides its automation.
void AutomationEditor::removeLane(bool andBelow)
{
    auto& lanes = ctx().automationLaneTargets[track_];
    if (lanes.empty()) lanes.push_back(targetId_);
    if (lane_ == 0 && (andBelow || lanes.size() <= 1))
    {
        lanes.resize(1);
        ctx().toggleAutomationLane(track_);
    }
    else if ((size_t) lane_ < lanes.size())
    {
        if (andBelow) lanes.resize((size_t) lane_);
        else lanes.erase(lanes.begin() + lane_);
    }
    if (ctx().selectedAutomationTrack == track_) selectedPoint_ = -1;
    if (ctx().layoutChanged) ctx().layoutChanged();
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
    syncTarget(ts);
    auto it = std::find_if(ts.begin(), ts.end(), [&](const Target& a) { return a.id == targetId_; });
    if (it == ts.end()) it = ts.begin();
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
    // UI-65: the LED Live puts beside automated devices and parameters, here a dot before the name.
    const bool groupAutomated = std::any_of(ts.begin(), ts.end(), [&](const Target& other) {
        return other.group == target.group && isAutomated(ctx(), track_, other.id);
    });
    ctl::dropdown(c, t, chooserBounds(true), (groupAutomated ? "\xE2\x97\x8F " : "") + target.group);
    ctl::dropdown(c, t, chooserBounds(false), (isAutomated(ctx(), track_, target.id) ? "\xE2\x97\x8F " : "") + target.name);
    for (bool add : {true, false})
    {
        const auto box = laneButton(add);
        paint::fill(c, box, t.color("ControlBackground"));
        paint::stroke(c, box, t.color("ControlContrastFrame"));
        const float cx = box.x + box.w / 2, cy = box.y + box.h / 2;
        paint::line(c, cx - 3, cy, cx + 3, cy, t.color("ControlForeground"));
        if (add) paint::line(c, cx, cy - 3, cx, cy + 3, t.color("ControlForeground"));
    }
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

// Lane "+" / "-" buttons, right of the name column (where the track header has its I/O column).
Rect AutomationEditor::laneButton(bool add) const
{
    const float x = plot().w + arrangementTitleWidth(ctx()) + 6;
    return {add ? x : x + 18, 5, 15, 15};
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
    const bool onlyAutomated = ctx().automationAutomatedOnly;
    std::vector<std::string> labels, groups;
    std::vector<bool> ticked;
    std::vector<int> ids; // -1: the "Show Automated Parameters Only" switch
    const std::string dot = "\xE2\x97\x8F ";
    for (const auto& target : choices)
    {
        const bool automated = isAutomated(ctx(), track_, target.id);
        if (device)
        {
            if (std::find(groups.begin(), groups.end(), target.group) != groups.end()) continue;
            const bool groupAutomated = std::any_of(choices.begin(), choices.end(), [&](const Target& other) {
                return other.group == target.group && isAutomated(ctx(), track_, other.id);
            });
            if (onlyAutomated && ! groupAutomated && selected->group != target.group) continue;
            // The device's first automated target, so picking a device shows something that moves.
            int pick = target.id;
            if (groupAutomated)
                for (const auto& other : choices)
                    if (other.group == target.group && isAutomated(ctx(), track_, other.id)) { pick = other.id; break; }
            groups.push_back(target.group); labels.push_back((groupAutomated ? dot : "") + target.group); ids.push_back(pick);
            ticked.push_back(selected->group == target.group);
        }
        else if (target.group == selected->group)
        {
            if (onlyAutomated && ! automated && target.id != selected->id) continue;
            labels.push_back((automated ? dot : "") + target.name);
            ids.push_back(target.id); ticked.push_back(selected->id == target.id);
        }
    }
    const size_t count = labels.size();
    labels.push_back("Show Automated Parameters Only"); ticked.push_back(onlyAutomated); ids.push_back(-1);
    auto pick = [this, ids, song = ctx().song](int index) {
        if (! Widget::isAlive(this) || ctx().song != song || index < 0 || index >= (int) ids.size()) return;
        if (ids[(size_t) index] == -1)
        {
            ctx().automationAutomatedOnly = ! ctx().automationAutomatedOnly;
            repaint();
            return;
        }
        const auto current = targets();
        if (std::none_of(current.begin(), current.end(), [&](const auto& target) { return target.id == ids[(size_t) index]; })) return;
        setTarget(ids[(size_t) index]); repaint();
    };
    if (ctx().showChoices) { ctx().showChoices(labels, ticked, std::move(pick)); return; }
    juce::PopupMenu menu;
    // Plugins can have thousands of parameters: long lists go into submenus of 64.
    constexpr size_t page = 64;
    if (count > page * 2)
        for (size_t first = 0; first < count; first += page)
        {
            juce::PopupMenu sub;
            bool tick = false;
            const size_t last = std::min(count, first + page);
            for (size_t i = first; i < last; ++i) { sub.addItem((int) i + 1, labels[i], true, ticked[i]); tick |= ticked[i]; }
            const auto strip = [&](const std::string& text) { return text.rfind(dot, 0) == 0 ? text.substr(dot.size()) : text; };
            menu.addSubMenu(juce::String::fromUTF8((strip(labels[first]) + "  \xE2\x80\xA6  " + strip(labels[last - 1])).c_str()), sub, true,
                            nullptr, tick);
        }
    else
        for (size_t i = 0; i < count; ++i) menu.addItem((int) i + 1, juce::String::fromUTF8(labels[i].c_str()), true, ticked[i]);
    menu.addSeparator();
    menu.addItem((int) count + 1, labels[count], true, onlyAutomated);
    const auto box = chooserBounds(device), origin = boundsInRoot();
    menu.showMenuAsync(dropDownOptions(ctx(), {origin.x + box.x, origin.y + box.y, box.w, box.h}),
                       [pick](int result) { if (result > 0) pick(result - 1); });
}

void AutomationEditor::mouseDown(const MouseEvent& e)
{
    const auto ts = targets();
    if (ts.empty()) return;
    syncTarget(ts);
    auto it = std::find_if(ts.begin(), ts.end(), [&](const Target& a) { return a.id == targetId_; });
    if (it == ts.end()) it = ts.begin();
    ctx().selectedTrack = ctx().selectedAutomationTrack = track_;
    if (e.x >= plot().w)
    {
        if (chooserBounds(true).contains(e.x, e.y)) showChooser(true);
        else if (chooserBounds(false).contains(e.x, e.y)) showChooser(false);
        else if (laneButton(true).contains(e.x, e.y)) addLane(e.cmd);
        else if (laneButton(false).contains(e.x, e.y)) removeLane(e.cmd);
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
    {
        if (dragTarget_.id < -1 && dragTarget_.pluginDevice >= 0)
        {
            // UI-65: the first breakpoint of a plugin parameter the set does not list: give it a target first.
            int next = ctx().song->nextPointeeId;
            model::EditCommand assign;
            const auto ids = model::pluginParameterTargets(*ctx().song, next, track_, dragTarget_.pluginDevice,
                                                           {{dragTarget_.hostId, dragTarget_.name, dragTarget_.base}}, assign);
            if (ids.empty() || ids.front() < 0) { changed_ = dragging_ = drawing_ = false; repaint(); return; }
            preview_.targetId = ids.front();
            auto edit = model::setAutomation(*ctx().song, track_, preview_);
            std::vector<model::EditCommand> parts;
            if (assign.apply) parts.push_back(std::move(assign));
            parts.push_back(std::move(edit));
            setTarget(ids.front());
            ctx().applyEdit(model::compose("Edit Automation", std::move(parts)));
        }
        else
            ctx().applyEdit(model::setAutomation(*ctx().song, track_, preview_));
    }
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
        dragTarget_ = *it;
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
