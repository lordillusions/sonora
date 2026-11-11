#include "ui/views/AutomationEditor.h"

#include "ui/GridModel.h"

#include "engine/Automation.h"
#include "engine/EngineGraph.h"
#include "engine/Transport.h"
#include "ui/ClipEditing.h"
#include "ui/Painter.h"
#include "ui/UiContext.h"
#include "ui/Controls.h"
#include "ui/views/ArrangementView.h"
#include "ui/MenuLookAndFeel.h"
#include "core/DeviceCatalog.h"
#include "ui/views/ClipEnvelopeEditor.h"

#include "include/core/SkPaint.h"
#include "include/effects/SkDashPathEffect.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
        if (auto* volume = add(mx.volumeTargetId, "Mixer / Track Volume", 0, 1.995262, mx.volume))
        {
            volume->law = model::AutomationLaw::Fader;
            volume->overrideKey = engine::automationOverrideKey(engine::AutomationBinding::Kind::Volume, 0, 0);
        }
        if (auto* pan = add(mx.panTargetId, "Mixer / Track Panning", -1, 1, mx.pan))
            pan->overrideKey = engine::automationOverrideKey(engine::AutomationBinding::Kind::Pan, 0, 0);
    }
    add(mx.speakerTargetId, "Mixer / Track Activator", 0, 1, mx.speakerOn ? 1 : 0, "BoolEvent");
    add(mx.tempoTargetId, "Song / Tempo", 20, 999, ctx.song->tempo);
    for (size_t i = 0; audio && i < mx.sendTargetIds.size(); ++i)
        if (auto* send = add(mx.sendTargetIds[i], "Mixer / Send " + std::string(1, (char) ('A' + (int) i)), 0, 1,
                             i < mx.sends.size() ? mx.sends[i] : 0))
        {
            send->law = model::AutomationLaw::Fader;
            send->overrideKey = engine::automationOverrideKey(engine::AutomationBinding::Kind::Send, (int) i, 0);
        }
    std::set<int> laneTargets;
    for (const auto& env : track->automation) laneTargets.insert(env.targetId);
    for (size_t i = 0; i < track->devices.size(); ++i)
    {
        const auto& dev = track->devices[i];
        const std::string prefix = std::to_string(i + 1) + " " + (dev.displayName.empty() ? dev.type : dev.displayName) + " / ";
        if (auto* on = add(dev.enabledTargetId, prefix + "On", 0, 1, dev.enabled ? 1 : 0, "BoolEvent"))
            on->overrideKey = engine::automationOverrideKey(engine::AutomationBinding::Kind::DeviceOn, 0, (int) i);
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
                    target->overrideKey = engine::automationOverrideKey(engine::AutomationBinding::Kind::DeviceParam, index, (int) i);
                    if (id < -1) target->base = proc->getParameter(index); // what the plugin holds now
                }
            }
        }
        else if (proc)
            for (const auto& p : dev.parameters)
                for (const auto& info : proc->parameters())
                    if (p.path == info.id)
                        if (auto* target = add(p.automationTargetId, prefix + info.name, info.minValue, info.maxValue, p.value,
                                               info.isToggle ? "BoolEvent" : info.choices.empty() ? "FloatEvent" : "EnumEvent"))
                        {
                            const int index = (int) (&info - proc->parameters().data());
                            target->law = model::automationLawFor(info.unit, info.minValue, info.maxValue, target->stepped);
                            target->nativeDevice = (int) i;
                            target->parameterIndex = index;
                            target->overrideKey = engine::automationOverrideKey(engine::AutomationBinding::Kind::DeviceParam, index, (int) i);
                        }
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
    if (selectionHere()) clearSelection();
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
    if (ctx().selectedAutomationTrack == track_ && ctx().automationSelection.track == track_) ctx().automationSelection = {};
    if (ctx().layoutChanged) ctx().layoutChanged();
}

model::AutomationEnvelope AutomationEditor::envelope(const Target& target) const { return envelopeOf(ctx(), track_, target); }

model::AutomationEnvelope AutomationEditor::envelopeOf(const UiContext& ctx, int trackIndex, const Target& target)
{
    const auto* track = ctx.trackAt(trackIndex);
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
    return r.bottom() - (float) target.position(value) * r.h;
}

double AutomationEditor::currentValue(const Target& target) const { return controlValue(ctx(), track_, target); }

double AutomationEditor::controlValue(const UiContext& x, int trackIndex, const Target& target)
{
    if (x.controls && (size_t) trackIndex < x.controls->size())
    {
        const auto& controls = (*x.controls)[(size_t) trackIndex];
        using K = engine::AutomationBinding::Kind;
        if (target.overrideKey == engine::automationOverrideKey(K::Volume, 0, 0)) return controls.volume.load();
        if (target.overrideKey == engine::automationOverrideKey(K::Pan, 0, 0)) return controls.pan.load();
        for (size_t i = 0; i < controls.sends.size(); ++i)
            if (target.overrideKey == engine::automationOverrideKey(K::Send, (int) i, 0)) return controls.sends[i].load();
    }
    const int device = target.nativeDevice >= 0 ? target.nativeDevice : target.pluginDevice;
    if (device >= 0 && target.parameterIndex >= 0 && x.graph && (size_t) trackIndex < x.graph->nodes.size())
    {
        const auto& processors = x.graph->nodes[(size_t) trackIndex].processorForModelDevice;
        if ((size_t) device < processors.size() && processors[(size_t) device] != nullptr
            && (size_t) target.parameterIndex < processors[(size_t) device]->parameters().size())
            return processors[(size_t) device]->getParameter(target.parameterIndex);
    }
    return target.base;
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
    // UI-77c: a lane being written in Automation Write mode shows the take so far.
    const auto live = ctx().automationPreview.find({track_, target.id});
    const bool writing = live != ctx().automationPreview.end();
    const auto env = dragging_ || drawing_ || (gesture_ != Gesture::None && gesture_ != Gesture::Range) ? preview_
                     : writing ? live->second : envelope(target);
    const engine::AutomationLane lane(env, target.law, target.min, target.max);
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
    // The lane's time selection (UI-76): drawn under the envelope like the Arrangement's.
    {
        double start = 0, end = 0;
        if (selectionHere() && ctx().automationSelection.range && selectionRange(start, end) && end > start)
        {
            const Rect band{beatX(start), 0, (float) ((end - start) * ctx().pixelsPerBeat), height()};
            paint::fill(c, band, paint::withAlpha(t.color("SelectionBackground"), 90));
            paint::line(c, band.x, 0, band.x, height(), t.color("SelectionFrame"));
            paint::line(c, band.right(), 0, band.right(), height(), t.color("SelectionFrame"));
        }
    }
    // UI-75: as in the reference, a lane whose control is held by hand (Re-Enable Automation lit) is drawn grey, and a
    // control with no automation yet shows its current value as a dashed line (the first click on it adds a breakpoint).
    const bool overridden = target.overrideKey >= 0 && ctx().controls && (size_t) track_ < ctx().controls->size()
                            && engine::isAutomationOverridden((*ctx().controls)[(size_t) track_], target.overrideKey);
    const auto ink = writing ? t.color("ChosenRecord") : overridden ? t.color("AutomationDisabled") : t.color("AutomationColor");
    if (! writing && ! dragging_ && ! drawing_ && ! (moved_ && gesture_ != Gesture::None && gesture_ != Gesture::Range)
        && ! isAutomated(ctx(), track_, target.id))
    {
        const float y = valueY(currentValue(target), target);
        SkPaint pen;
        pen.setAntiAlias(true);
        pen.setStyle(SkPaint::kStroke_Style);
        pen.setStrokeWidth(1.0f);
        pen.setColor(ink);
        const SkScalar dashes[]{4, 3};
        pen.setPathEffect(SkDashPathEffect::Make(dashes, 0));
        c.drawLine(0, y, r.w, y, pen);
    }
    else
    {
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
        // UI-76: selected breakpoints are larger and framed; the hovered one gets a ring.
        const bool movingPoints = gesture_ == Gesture::Points && moved_;
        auto selected = [&](double time) {
            if (movingPoints) return std::any_of(movedTimes_.begin(), movedTimes_.end(), [&](double m) { return std::abs(m - time) < 1e-9; });
            return pointSelected(time);
        };
        const int hovered = gesture_ == Gesture::None && hoverX_ >= 0 ? pointAt(env, target, hoverX_, hoverY_) : -1;
        for (size_t i = 0; i < env.points.size(); ++i)
        {
            const auto& p = env.points[i];
            if (p.time < first || p.time > last) continue;
            const float px = beatX(p.time), py = valueY(p.value, target);
            if (selected(p.time))
            {
                paint::fill(c, {px - 4, py - 4, 8, 8}, t.color("SelectionFrame"));
                paint::fill(c, {px - 2.5f, py - 2.5f, 5, 5}, t.color("SelectionBackground"));
            }
            else paint::fill(c, {px - 3, py - 3, 6, 6}, ink);
            if ((int) i == hovered) paint::stroke(c, {px - 5, py - 5, 10, 10}, ink);
        }
        // Live's preview breakpoint: hovering the line shows where a click adds one.
        if (hovered < 0 && gesture_ == Gesture::None && hoverX_ >= 0 && ! ctx().drawMode
            && lineDistance(env, target, hoverX_, hoverY_) <= 4.0f)
        {
            const double beat = snapped(beatAt(hoverX_), false);
            SkPaint ring;
            ring.setAntiAlias(true);
            ring.setStyle(SkPaint::kStroke_Style);
            ring.setStrokeWidth(1.2f);
            ring.setColor(ink);
            c.drawCircle(beatX(beat), valueY(model::laneValueAt(env, beat, scaleOf(target)), target), 3.5f, ring);
        }
        // The value under a dragged or hovered breakpoint, as the reference shows it next to the point.
        double readTime = -1; // -2: the value under the pointer during a segment drag
        bool segmentRead = false;
        if (movingPoints && ! movedTimes_.empty()) readTime = movedTimes_.front();
        else if (gesture_ == Gesture::Points && ! moved_) readTime = pressedTime_;
        else if (hovered >= 0) readTime = env.points[(size_t) hovered].time;
        if (gesture_ == Gesture::Points && movingPoints)
            for (size_t k = 0; k < movingTimes_.size() && k < movedTimes_.size(); ++k)
                if (std::abs(movingTimes_[k] - pressedTime_) < 1e-9) readTime = movedTimes_[k];
        if ((gesture_ == Gesture::Segment || gesture_ == Gesture::Curve) && moved_) segmentRead = true;
        if (readTime >= 0 || segmentRead)
        {
            const double beat = readTime >= 0 ? readTime : beatAt(downX_);
            const double value = readTime >= 0 ? model::laneValueAt(env, beat + 1e-9, scaleOf(target)) : model::laneValueAt(env, beat, scaleOf(target));
            const auto text = formatValue(target, readTime >= 0 ? [&] {
                for (const auto& p : env.points) if (std::abs(p.time - readTime) < 1e-9) return p.value;
                return value; }() : value);
            const float px = beatX(beat), py = valueY(value, target);
            const float w = std::max(36.0f, 6.5f * (float) text.size() + 8), h = 14;
            Rect box{std::clamp(px + 7, 0.0f, std::max(0.0f, r.w - w)), py - h - 4 < 0 ? py + 6 : py - h - 4, w, h};
            paint::fill(c, box, t.color("DisplayBackground"));
            paint::stroke(c, box, t.color("ControlContrastFrame"));
            paint::text(c, text, box, t.color("ControlForeground"), t.metrics.smallFontSize, Align::Centre);
        }
    }
    if (editingValue_)
    {
        const double value = [&] { for (const auto& p : env.points) if (std::abs(p.time - editTime_) < 1e-9) return p.value; return target.base; }();
        const Rect field{std::clamp(beatX(editTime_) - 35, 0.0f, std::max(0.0f, r.w - 70)), std::clamp(valueY(value, target) - 8, 0.0f, height() - 16), 70, 16};
        valueEdit_.paint(c, field, t.color("ControlTextBack"), t.color("ControlForeground"), t.color("SelectionBackground"),
                         t.color("SelectionForeground"), t.metrics.smallFontSize);
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

// ---- UI-76: editing like the reference's lanes (Live 12 manual, "Automation and Editing Envelopes"). Draw Mode off: a click
// on the line adds a breakpoint, a click on a breakpoint deletes it, a drag moves it (with every selected one); Shift
// constrains to one axis and fine-tunes the value; Cmd (Alt) bypasses the grid. A click near the line selects its segment
// and a drag moves it up or down (inside a time selection: the whole selection, pinned at its edges). Option + drag bends
// a segment, Option + double-click straightens it. A drag on the background selects a time range: Delete, Cmd+C/X/V/D,
// Insert Shape and Simplify Envelope work on it; Cmd+A selects the lane; Cmd+Delete deletes the lane's automation.

std::string AutomationEditor::formatValue(const Target& t, double value)
{
    const bool pan = t.group == "Mixer" && t.name == "Track Panning";
    return model::formatLaneValue(value, scaleOf(t), pan, t.eventType == "BoolEvent", t.law == model::AutomationLaw::Log
        ? (t.max > 1000 ? "Hz" : "ms") : "");
}

const AutomationEditor::Target* AutomationEditor::current(const std::vector<Target>& ts)
{
    if (ts.empty()) return nullptr;
    syncTarget(ts);
    const auto it = std::find_if(ts.begin(), ts.end(), [&](const Target& a) { return a.id == targetId_; });
    return it == ts.end() ? &ts.front() : &*it;
}

double AutomationEditor::beatAt(float x) const { return ctx().scrollBeats + x / ctx().pixelsPerBeat; }

double AutomationEditor::positionAt(float y) const
{
    const auto r = plot();
    return std::clamp((double) (r.bottom() - y) / r.h, 0.0, 1.0);
}

double AutomationEditor::snapped(double beat, bool bypass) const
{
    const double bar = ctx().song ? ctx().song->timeSigNumerator * 4.0 / ctx().song->timeSigDenominator : 4.0;
    return std::max(0.0, snapToGrid(beat, ui::snapSize(ctx().arrangementGrid, ctx().pixelsPerBeat, bar, bypass)));
}

int AutomationEditor::pointAt(const model::AutomationEnvelope& env, const Target& t, float x, float y) const
{
    int found = -1;
    float best = 9.0f; // the old editor's grab radius
    for (size_t i = 0; i < env.points.size(); ++i)
    {
        const auto& p = env.points[i];
        if (p.time < 0) continue; // the pre-roll value is retained
        const float distance = std::hypot(beatX(p.time) - x, valueY(p.value, t) - y);
        if (distance < best) { best = distance; found = (int) i; }
    }
    return found;
}

float AutomationEditor::lineDistance(const model::AutomationEnvelope& env, const Target& t, float x, float y) const
{
    const double beat = beatAt(x);
    float distance = std::abs(valueY(model::laneValueAt(env, beat, scaleOf(t)), t) - y);
    // A jump (two points at one time, or a stepped lane's step) is a vertical piece of the line.
    for (size_t i = 0; i + 1 < env.points.size(); ++i)
    {
        const auto& a = env.points[i];
        const auto& b = env.points[i + 1];
        const bool jump = std::abs(b.time - a.time) < 1e-9 || env.stepped;
        if (! jump || b.time < 0 || std::abs(beatX(b.time) - x) > 3) continue;
        const float y0 = valueY(env.stepped ? a.value : a.value, t), y1 = valueY(b.value, t);
        if (y >= std::min(y0, y1) && y <= std::max(y0, y1)) distance = std::min(distance, std::abs(beatX(b.time) - x));
    }
    return distance;
}

bool AutomationEditor::selectionHere() const
{
    const auto& s = ctx().automationSelection;
    return s.track == track_ && s.target == targetId_ && targetId_ != -1;
}

bool AutomationEditor::pointSelected(double time) const
{
    if (! selectionHere()) return false;
    const auto& points = ctx().automationSelection.points;
    return std::any_of(points.begin(), points.end(), [&](double p) { return std::abs(p - time) < 1e-9; });
}

void AutomationEditor::selectPoints(std::vector<double> times, bool range, double start, double end)
{
    auto& s = ctx().automationSelection;
    s = {};
    s.track = track_;
    s.target = targetId_;
    s.points = std::move(times);
    s.range = range;
    s.start = std::min(start, end);
    s.end = std::max(start, end);
    ctx().selectedAutomationTrack = track_;
}

void AutomationEditor::clearSelection()
{
    if (ctx().automationSelection.track == track_) ctx().automationSelection = {};
}

bool AutomationEditor::selectionRange(double& start, double& end) const
{
    if (! selectionHere()) return false;
    const auto& s = ctx().automationSelection;
    if (s.range && s.end > s.start) { start = s.start; end = s.end; return true; }
    if (s.points.empty()) return false;
    start = *std::min_element(s.points.begin(), s.points.end());
    end = *std::max_element(s.points.begin(), s.points.end());
    return true;
}

bool AutomationEditor::editLane(const std::function<void(model::AutomationEnvelope&, const Target&)>& edit, const char* label)
{
    if (! ctx().song || ! ctx().applyEdit || ctx().recording) return false;
    const auto ts = targets();
    const auto* t = current(ts);
    if (t == nullptr) return false;
    dragTarget_ = *t;
    preview_ = envelope(*t);
    const auto before = preview_.points;
    edit(preview_, *t);
    const bool same = before.size() == preview_.points.size()
                      && std::equal(before.begin(), before.end(), preview_.points.begin(), [](const auto& a, const auto& b) {
                             return std::memcmp(&a, &b, sizeof(a)) == 0;
                         });
    if (same) { repaint(); return false; }
    changed_ = true;
    (void) label;
    commit();
    return true;
}

void AutomationEditor::mouseDown(const MouseEvent& e)
{
    const auto ts = targets();
    const auto* it = current(ts);
    if (it == nullptr) return;
    ctx().selectedTrack = ctx().selectedAutomationTrack = track_;
    if (editingValue_) commitValueEdit();
    if (e.x >= plot().w)
    {
        if (chooserBounds(true).contains(e.x, e.y)) showChooser(true);
        else if (chooserBounds(false).contains(e.x, e.y)) showChooser(false);
        else if (laneButton(true).contains(e.x, e.y)) addLane(e.cmd);
        else if (laneButton(false).contains(e.x, e.y)) removeLane(e.cmd);
        return;
    }
    if (e.right) { showContextMenu(e.x, e.y); return; }
    if (! ctx().applyEdit || ctx().recording) return;
    dragTarget_ = *it;
    original_ = preview_ = envelope(*it);
    gesture_ = Gesture::None;
    moved_ = changed_ = false;
    downX_ = e.x; downY_ = e.y;
    shift_ = e.shift;
    bypassSnap_ = e.cmd;
    if (ctx().drawMode)
    {
        drawing_ = true;
        lastCell_ = -1;
        bypassSnap_ = e.cmd || e.alt;
        paintCell(e);
        return;
    }
    const int point = pointAt(preview_, *it, e.x, e.y);
    const float distance = lineDistance(preview_, *it, e.x, e.y);
    if (e.clicks >= 2 && point < 0)
    {
        if (e.alt)
        {
            // Option + double-click: the segment goes back to a straight line.
            const auto seg = model::segmentAt(preview_, beatAt(e.x));
            if (seg.first >= 0) { model::curveSegment(preview_, seg.first, 0); changed_ = true; commit(); }
            return;
        }
        // Double-click on an empty part of the lane: a breakpoint at the pointer.
        const double beat = snapped(beatAt(e.x), e.cmd || e.alt);
        double value = it->valueAt(positionAt(e.y));
        if (it->stepped) value = std::round(value);
        preview_.points.push_back({beat, value});
        std::stable_sort(preview_.points.begin(), preview_.points.end(), [](const auto& a, const auto& b) { return a.time < b.time; });
        changed_ = true;
        commit();
        return;
    }
    if (point >= 0)
    {
        gesture_ = Gesture::Points;
        const auto& p = preview_.points[(size_t) point];
        pressedTime_ = p.time;
        pressedPosition_ = it->position(p.value);
        const auto& sel = ctx().automationSelection;
        if (pointSelected(p.time) && sel.points.size() > 1) movingTimes_ = sel.points;
        else movingTimes_ = {p.time};
        movedTimes_ = movingTimes_;
        dragging_ = true;
        repaint();
        return;
    }
    if (distance <= 4.0f)
    {
        const auto seg = model::segmentAt(preview_, beatAt(e.x));
        if (e.alt && seg.first >= 0 && ! preview_.stepped)
        {
            gesture_ = Gesture::Curve;
            curveIndex_ = seg.first;
            curveStart_ = model::segmentCurvature(preview_.points[(size_t) seg.first]);
            curveRising_ = it->position(preview_.points[(size_t) seg.second].value) >= it->position(preview_.points[(size_t) seg.first].value);
            dragging_ = true;
            return;
        }
        if (e.shift && seg.first >= 0)
        {
            std::vector<double> times;
            for (int i : {seg.first, seg.second}) if (preview_.points[(size_t) i].time >= 0) times.push_back(preview_.points[(size_t) i].time);
            selectPoints(std::move(times));
            repaint();
            return;
        }
        gesture_ = Gesture::LineClick;
        return;
    }
    if (distance <= 12.0f) { gesture_ = Gesture::SegmentNear; return; }
    gesture_ = Gesture::Range;
    rangeAnchor_ = snapped(beatAt(e.x), e.cmd || e.alt);
}

// A segment drag moves the two breakpoints around the pointer up or down. Inside a time selection it moves everything
// selected, with the lane's values pinned at the selection edges first (the reference inserts those breakpoints).
void AutomationEditor::beginSegmentDrag(float x)
{
    gesture_ = Gesture::Segment;
    dragging_ = true;
    movesPreRoll_ = false;
    movingTimes_.clear();
    const auto scale = scaleOf(dragTarget_);
    const double beat = beatAt(x);
    double start = 0, end = 0;
    if (selectionHere() && ctx().automationSelection.range && selectionRange(start, end) && beat >= start && beat <= end)
    {
        const double before = model::laneValueAt(original_, std::nextafter(start, -INFINITY), scale);
        const double inStart = model::laneValueAt(original_, start, scale);
        const double inEnd = model::laneValueAt(original_, std::nextafter(end, -INFINITY), scale);
        const double after = model::laneValueAt(original_, end, scale);
        auto& p = original_.points;
        std::erase_if(p, [&](const auto& q) { return q.time >= 0 && (std::abs(q.time - start) < 1e-9 || std::abs(q.time - end) < 1e-9); });
        // Outer pins keep the lane outside; the inner ones (a hair inside) move with the selection.
        const double inner0 = start + 1e-6, inner1 = end - 1e-6;
        p.push_back({start, before});
        p.push_back({inner0, inStart});
        p.push_back({inner1, inEnd});
        p.push_back({end, after});
        std::stable_sort(p.begin(), p.end(), [](const auto& a, const auto& b) { return a.time < b.time; });
        movingTimes_ = model::envelopePointTimes(original_, inner0, inner1);
    }
    else
    {
        const auto seg = model::segmentAt(original_, beat);
        if (seg.first >= 0)
        {
            for (int i : {seg.first, seg.second}) movingTimes_.push_back(original_.points[(size_t) i].time);
        }
        else
        {
            // Before the first breakpoint (or a lane without any): the flat start moves with the first breakpoint.
            std::vector<double> times;
            for (const auto& p : original_.points) if (p.time >= 0) times.push_back(p.time);
            if (times.empty() || beat < times.front())
            {
                movesPreRoll_ = true;
                if (! times.empty()) movingTimes_.push_back(times.front());
            }
            else movingTimes_.push_back(times.back());
        }
    }
    preview_ = original_;
}

void AutomationEditor::mouseDrag(const MouseEvent& e)
{
    if (drawing_) { paintCell(e); return; }
    if (gesture_ == Gesture::None) return;
    if (! moved_ && std::hypot(e.x - downX_, e.y - downY_) < 3.0f) return;
    moved_ = true;
    const auto scale = scaleOf(dragTarget_);
    switch (gesture_)
    {
        case Gesture::Points:
        {
            double dx = e.x - downX_, dy = e.y - downY_;
            bool horizontal = true;
            if (shift_) { if (std::abs(dx) > std::abs(dy)) dy = 0; else { dx = 0; horizontal = false; } } // one axis, the reference's Shift
            const double target = ! horizontal ? pressedTime_ : snapped(pressedTime_ + dx / ctx().pixelsPerBeat, bypassSnap_ || e.cmd);
            double deltaPosition = -dy / plot().h * (shift_ ? 0.1 : 1.0); // Shift: fine values
            deltaPosition = std::clamp(pressedPosition_ + deltaPosition, 0.0, 1.0) - pressedPosition_;
            preview_ = original_;
            movedTimes_ = model::moveEnvelopePoints(preview_, movingTimes_, target - pressedTime_, deltaPosition, scale);
            changed_ = true;
            break;
        }
        case Gesture::LineClick:
        case Gesture::SegmentNear:
            beginSegmentDrag(downX_);
            [[fallthrough]];
        case Gesture::Segment:
        {
            const double deltaPosition = -(e.y - downY_) / plot().h * (e.shift ? 0.1 : 1.0);
            preview_ = original_;
            model::moveEnvelopePoints(preview_, movingTimes_, 0.0, deltaPosition, scale);
            if (movesPreRoll_)
                for (auto& p : preview_.points)
                    if (p.time < 0) p.value = scale.value(std::clamp(scale.position(p.value) + deltaPosition, 0.0, 1.0));
            if (preview_.stepped) for (auto& p : preview_.points) p.value = std::round(p.value);
            changed_ = true;
            break;
        }
        case Gesture::Curve:
        {
            const double amount = curveStart_ + (downY_ - e.y) / 60.0 * (curveRising_ ? 1.0 : -1.0);
            preview_ = original_;
            model::curveSegment(preview_, curveIndex_, amount);
            changed_ = true;
            break;
        }
        case Gesture::Range:
        {
            const double end = snapped(beatAt(e.x), e.cmd || e.alt);
            const double a = std::min(rangeAnchor_, end), b = std::max(rangeAnchor_, end);
            selectPoints(model::envelopePointTimes(original_, a, b), true, a, b);
            break;
        }
        case Gesture::None: break;
    }
    repaint();
}

int AutomationEditor::store(UiContext& ctx, int track, const Target& target, model::AutomationEnvelope env)
{
    if (! ctx.song || ! ctx.applyEdit) return -1;
    int id = target.id;
    if (target.id < -1 && target.pluginDevice >= 0)
    {
        // UI-65: the first breakpoint of a plugin parameter the set does not list: give it a target first.
        int next = ctx.song->nextPointeeId;
        model::EditCommand assign;
        const auto ids = model::pluginParameterTargets(*ctx.song, next, track, target.pluginDevice,
                                                       {{target.hostId, target.name, target.base}}, assign);
        if (ids.empty() || ids.front() < 0) return -1;
        id = env.targetId = ids.front();
        auto edit = model::setAutomation(*ctx.song, track, env);
        std::vector<model::EditCommand> parts;
        if (assign.apply) parts.push_back(std::move(assign));
        parts.push_back(std::move(edit));
        auto& lanes = ctx.automationLaneTargets[track];
        for (auto& lane : lanes) if (lane == target.id) lane = id;
        ctx.applyEdit(model::compose("Edit Automation", std::move(parts)));
    }
    else
        ctx.applyEdit(model::setAutomation(*ctx.song, track, std::move(env)));
    // The lane drives the control again (the reference re-enables a parameter whose envelope is edited).
    if (target.overrideKey >= 0 && ctx.reEnableControl) ctx.reEnableControl(track, target.overrideKey);
    return id;
}

void AutomationEditor::commit()
{
    if (changed_ && ctx().song && ctx().applyEdit)
    {
        const int id = store(ctx(), track_, dragTarget_, preview_);
        if (id != -1 && id != dragTarget_.id) setTarget(id);
    }
    changed_ = dragging_ = drawing_ = false;
    lastCell_ = -1;
    repaint();
}

void AutomationEditor::mouseUp(const MouseEvent& e)
{
    if (drawing_) { commit(); return; }
    const auto gesture = std::exchange(gesture_, Gesture::None);
    const bool moved = moved_;
    moved_ = false;
    switch (gesture)
    {
        case Gesture::Points:
            if (! moved)
            {
                if (shift_)
                {
                    // Shift-click: adds the breakpoint to the selection or takes it out.
                    auto times = selectionHere() ? ctx().automationSelection.points : std::vector<double>{};
                    const auto found = std::find_if(times.begin(), times.end(), [&](double t) { return std::abs(t - pressedTime_) < 1e-9; });
                    if (found != times.end()) times.erase(found); else times.push_back(pressedTime_);
                    selectPoints(std::move(times));
                    dragging_ = false;
                    repaint();
                    return;
                }
                // The reference: a click on a breakpoint deletes it.
                std::erase_if(preview_.points, [&](const auto& p) { return p.time >= 0 && std::abs(p.time - pressedTime_) < 1e-9; });
                if (selectionHere())
                {
                    auto& points = ctx().automationSelection.points;
                    std::erase_if(points, [&](double t) { return std::abs(t - pressedTime_) < 1e-9; });
                }
                changed_ = true;
                commit();
                return;
            }
            commit();
            if (movingTimes_.size() > 1) selectPoints(movedTimes_);
            else clearSelection();
            return;
        case Gesture::LineClick:
            if (! moved)
            {
                // A click on the line adds a breakpoint on it.
                const double beat = snapped(beatAt(e.x), e.cmd || e.alt);
                double value = model::laneValueAt(original_, beat, scaleOf(dragTarget_));
                if (dragTarget_.stepped) value = std::round(value);
                preview_ = original_;
                preview_.points.push_back({beat, value});
                std::stable_sort(preview_.points.begin(), preview_.points.end(), [](const auto& a, const auto& b) { return a.time < b.time; });
                changed_ = true;
                commit();
                return;
            }
            commit();
            return;
        case Gesture::SegmentNear:
            if (! moved)
            {
                const auto seg = model::segmentAt(original_, beatAt(e.x));
                std::vector<double> times;
                if (seg.first >= 0)
                    for (int i : {seg.first, seg.second}) if (original_.points[(size_t) i].time >= 0) times.push_back(original_.points[(size_t) i].time);
                if (times.empty()) clearSelection(); else selectPoints(std::move(times));
                repaint();
                return;
            }
            commit();
            return;
        case Gesture::Segment:
        case Gesture::Curve:
            commit();
            return;
        case Gesture::Range:
            if (! moved) clearSelection();
            repaint();
            return;
        case Gesture::None:
            commit();
            return;
    }
}

void AutomationEditor::mouseMove(const MouseEvent& e)
{
    const bool inPlot = e.x < plot().w;
    const float x = inPlot ? e.x : -1, y = inPlot ? e.y : -1;
    if (std::abs(x - hoverX_) > 0.01f || std::abs(y - hoverY_) > 0.01f) { hoverX_ = x; hoverY_ = y; repaint(); }
}

void AutomationEditor::mouseExit()
{
    if (hoverX_ >= 0) { hoverX_ = hoverY_ = -1; repaint(); }
}

Cursor AutomationEditor::cursorAt(float x, float y) const
{
    if (x >= plot().w || ctx().drawMode || ! ctx().song) return Cursor::Default;
    const auto ts = targets();
    if (ts.empty()) return Cursor::Default;
    auto it = std::find_if(ts.begin(), ts.end(), [&](const Target& a) { return a.id == targetId_; });
    if (it == ts.end()) it = ts.begin();
    const auto env = envelope(*it);
    if (pointAt(env, *it, x, y) >= 0) return Cursor::Move;
    const float d = lineDistance(env, *it, x, y);
    return d > 4.0f && d <= 12.0f ? Cursor::ResizeVertical : Cursor::Default;
}

void AutomationEditor::focusLost()
{
    if (editingValue_) commitValueEdit();
}

// Draw Mode: the value under the pointer holds for one grid cell; cells crossed by a fast drag are filled in. Cmd/Option
// (or no grid) draws freehand: one breakpoint per pointer move.
void AutomationEditor::paintCell(const MouseEvent& e)
{
    const double bar = ctx().song ? ctx().song->timeSigNumerator * 4.0 / ctx().song->timeSigDenominator : 4.0;
    const auto r = plot();
    double value = dragTarget_.valueAt(std::clamp((r.bottom() - e.y) / r.h, 0.0f, 1.0f));
    if (dragTarget_.stepped) value = std::round(value);
    if (bypassSnap_ || ctx().arrangementGrid.snap == false)
    {
        const double beat = std::max(0.0, beatAt(e.x));
        const double from = lastCell_ < 0 ? beat : lastCell_;
        const double a = std::min(from, beat), b = std::max(from, beat);
        std::erase_if(preview_.points, [&](const model::AutomationPoint& p) { return p.time >= 0 && p.time > a - 1e-9 && p.time < b + 1e-9; });
        preview_.points.push_back({beat, value});
        std::stable_sort(preview_.points.begin(), preview_.points.end(), [](const auto& x0, const auto& x1) { return x0.time < x1.time; });
        lastCell_ = beat;
        changed_ = true;
        repaint();
        return;
    }
    const double g = ui::gridBeats(ctx().arrangementGrid, ctx().pixelsPerBeat, bar);
    const double cell = std::max(0.0, std::floor((ctx().scrollBeats + e.x / ctx().pixelsPerBeat) / g + 1e-9) * g);
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

void AutomationEditor::beginValueEdit(double time)
{
    const auto ts = targets();
    const auto* t = current(ts);
    if (t == nullptr) return;
    const auto env = envelope(*t);
    const auto found = std::find_if(env.points.begin(), env.points.end(), [&](const auto& p) { return p.time >= 0 && std::abs(p.time - time) < 1e-9; });
    if (found == env.points.end()) return;
    editingValue_ = true;
    editTime_ = time;
    valueEdit_.start(formatValue(*t, found->value));
    repaint();
}

void AutomationEditor::commitValueEdit()
{
    if (! editingValue_) return;
    editingValue_ = false;
    const auto ts = targets();
    const auto* t = current(ts);
    double value = 0;
    if (t == nullptr || ! model::parseLaneValue(valueEdit_.text, scaleOf(*t), t->group == "Mixer" && t->name == "Track Panning",
                                                t->eventType == "BoolEvent", value))
    {
        ctx().statusText = "Edit Value: type a number";
        repaint();
        return;
    }
    if (t->stepped) value = std::round(value);
    const double time = editTime_;
    // With several breakpoints selected, they move by the same amount on the lane (the reference moves them relatively).
    std::vector<double> times = pointSelected(time) ? ctx().automationSelection.points : std::vector<double>{time};
    editLane([&](model::AutomationEnvelope& env, const Target& target) {
        const auto scale = scaleOf(target);
        double delta = 0;
        for (const auto& p : env.points) if (p.time >= 0 && std::abs(p.time - time) < 1e-9) delta = scale.position(value) - scale.position(p.value);
        if (times.size() == 1)
        {
            for (auto& p : env.points) if (p.time >= 0 && std::abs(p.time - time) < 1e-9) { p.value = value; break; }
            return;
        }
        model::moveEnvelopePoints(env, times, 0.0, delta, scale);
    });
}

bool AutomationEditor::copySelection(bool cut)
{
    double start = 0, end = 0;
    if (! selectionRange(start, end) || end <= start) return false;
    const auto ts = targets();
    const auto* t = current(ts);
    if (t == nullptr) return false;
    ctx().automationClipboard = model::copyEnvelopeRange(envelope(*t), start, end, scaleOf(*t));
    ctx().statusText = cut ? "Cut automation" : "Copied automation";
    if (cut) editLane([&](model::AutomationEnvelope& env, const Target& target) { model::eraseEnvelopeRange(env, start, end, scaleOf(target)); });
    return true;
}

bool AutomationEditor::pasteClipboard()
{
    const auto clip = ctx().automationClipboard;
    if (clip.empty()) return false;
    double start = 0, end = 0;
    double at = selectionRange(start, end) ? start : ctx().transport ? ctx().transport->positionBeats() : 0.0;
    at = std::max(0.0, at);
    editLane([&](model::AutomationEnvelope& env, const Target& target) { model::pasteEnvelopeClip(env, at, clip, scaleOf(target)); });
    selectPoints({}, true, at, at + clip.length);
    ctx().statusText = "Pasted automation";
    return true;
}

bool AutomationEditor::duplicateSelection()
{
    double start = 0, end = 0;
    if (! selectionRange(start, end) || end <= start) return false;
    const auto ts = targets();
    const auto* t = current(ts);
    if (t == nullptr) return false;
    const auto clip = model::copyEnvelopeRange(envelope(*t), start, end, scaleOf(*t));
    editLane([&](model::AutomationEnvelope& env, const Target& target) { model::pasteEnvelopeClip(env, end, clip, scaleOf(target)); });
    selectPoints(model::envelopePointTimes(envelope(*t), end, end + (end - start)), true, end, end + (end - start));
    ctx().statusText = "Duplicated automation";
    return true;
}

bool AutomationEditor::deleteSelection()
{
    if (! selectionHere()) return false;
    const auto sel = ctx().automationSelection;
    clearSelection();
    if (sel.range && sel.end > sel.start)
        return editLane([&](model::AutomationEnvelope& env, const Target& target) { model::eraseEnvelopeRange(env, sel.start, sel.end, scaleOf(target)); });
    if (sel.points.empty()) return false;
    return editLane([&](model::AutomationEnvelope& env, const Target&) {
        std::erase_if(env.points, [&](const auto& p) {
            return p.time >= 0 && std::any_of(sel.points.begin(), sel.points.end(), [&](double t) { return std::abs(t - p.time) < 1e-9; });
        });
    });
}

std::vector<MenuAction> AutomationEditor::contextActions(float x, float y)
{
    std::vector<MenuAction> out;
    const auto ts = targets();
    const auto* t = current(ts);
    if (t == nullptr || ! ctx().song) return out;
    const bool editable = static_cast<bool>(ctx().applyEdit) && ! ctx().recording;
    const auto env = envelope(*t);
    const int point = pointAt(env, *t, x, y);
    auto* self = this;
    auto alive = [self] { return Widget::isAlive(self); };
    if (point >= 0)
    {
        const double time = env.points[(size_t) point].time;
        out.push_back({"Edit Value", [=] { if (alive()) self->beginValueEdit(time); }, editable});
        out.push_back({"Delete Breakpoint", [=] {
            if (! alive()) return;
            self->editLane([time](model::AutomationEnvelope& e, const Target&) {
                std::erase_if(e.points, [&](const auto& p) { return p.time >= 0 && std::abs(p.time - time) < 1e-9; });
            });
        }, editable});
        out.push_back({"", {}, false});
    }
    double start = 0, end = 0;
    const bool range = selectionRange(start, end) && end > start;
    out.push_back({"Cut", [=] { if (alive()) self->copySelection(true); }, editable && range, "\xE2\x8C\x98X"});
    out.push_back({"Copy", [=] { if (alive()) self->copySelection(false); }, range, "\xE2\x8C\x98" "C"});
    out.push_back({"Paste", [=] { if (alive()) self->pasteClipboard(); }, editable && ! ctx().automationClipboard.empty(), "\xE2\x8C\x98V"});
    out.push_back({"Duplicate", [=] { if (alive()) self->duplicateSelection(); }, editable && range, "\xE2\x8C\x98" "D"});
    out.push_back({"Delete", [=] { if (alive()) self->deleteSelection(); }, editable && selectionHere(), "\xE2\x8C\xAB"});
    out.push_back({"", {}, false});
    const bool shapes = editable && range && ! t->stepped;
    for (auto shape : {model::EnvelopeShape::Sine, model::EnvelopeShape::Triangle, model::EnvelopeShape::Sawtooth,
                       model::EnvelopeShape::InverseSawtooth, model::EnvelopeShape::Square, model::EnvelopeShape::RampUp,
                       model::EnvelopeShape::RampDown, model::EnvelopeShape::Adsr})
        out.push_back({std::string("Insert Shape/") + model::envelopeShapeName(shape), [=] {
            if (! alive()) return;
            double a = 0, b = 0;
            if (! self->selectionRange(a, b) || b <= a) return;
            const double bar = self->ctx().song ? self->ctx().song->timeSigNumerator * 4.0 / self->ctx().song->timeSigDenominator : 4.0;
            // One cycle per bar of the selection (at least one), across the whole lane height like the reference.
            const int cycles = std::max(1, (int) std::lround((b - a) / bar));
            self->editLane([=](model::AutomationEnvelope& e, const Target& target) {
                model::insertEnvelopeShape(e, a, b, shape, cycles, 0.0, 1.0, scaleOf(target));
            }, "Insert Shape");
            self->selectPoints(model::envelopePointTimes(self->envelope(*self->current(self->targets())), a, b), true, a, b);
        }, shapes});
    out.push_back({"Simplify Envelope", [=] {
        if (! alive()) return;
        double a = 0, b = 0;
        if (! self->selectionRange(a, b)) { a = 0; b = 1e12; }
        self->editLane([=](model::AutomationEnvelope& e, const Target& target) { model::simplifyEnvelope(e, a, b, scaleOf(target)); },
                       "Simplify Envelope");
    }, editable && isAutomated(ctx(), track_, t->id)});
    out.push_back({"Select All", [=] {
        if (! alive()) return;
        const auto all = self->envelope(*self->current(self->targets()));
        auto times = model::envelopePointTimes(all, 0, 1e12);
        const double last = times.empty() ? 0 : times.back();
        self->selectPoints(std::move(times), ! all.points.empty(), 0, last);
        self->repaint();
    }, true, "\xE2\x8C\x98" "A"});
    out.push_back({"", {}, false});
    out.push_back({"Delete Automation", [=] {
        if (! alive() || ! self->ctx().applyEdit) return;
        if (auto edit = model::clearAutomation(*self->ctx().song, self->track_, self->targetId_); edit.apply)
        {
            self->clearSelection();
            self->ctx().applyEdit(std::move(edit));
        }
    }, editable && isAutomated(ctx(), track_, t->id), "\xE2\x8C\x98\xE2\x8C\xAB"});
    const bool overridden = t->overrideKey >= 0 && ctx().controls && (size_t) track_ < ctx().controls->size()
                            && engine::isAutomationOverridden((*ctx().controls)[(size_t) track_], t->overrideKey);
    const int key = t->overrideKey;
    out.push_back({"Re-Enable Automation", [=] { if (alive() && self->ctx().reEnableControl) self->ctx().reEnableControl(self->track_, key); },
                   overridden && static_cast<bool>(ctx().reEnableControl)});
    out.push_back({"Lock Envelopes", [=] {
        if (! alive()) return;
        auto& c = self->ctx();
        c.clipAutomationPolicy = c.clipAutomationPolicy == 2 ? 0 : 2;
        if (c.setClipAutomationPolicy) c.setClipAutomationPolicy(c.clipAutomationPolicy);
    }, true, "", ctx().clipAutomationPolicy == 2});
    return out;
}

void AutomationEditor::showContextMenu(float x, float y)
{
    auto actions = contextActions(x, y);
    if (actions.empty()) return;
    if (ctx().showChoices)
    {
        // Tests (and hosts without native menus) pick by index from the flat list.
        std::vector<std::string> labels;
        std::vector<bool> ticks;
        for (const auto& a : actions) { labels.push_back(a.label); ticks.push_back(a.ticked); }
        ctx().showChoices(labels, ticks, [actions](int index) {
            if (index >= 0 && index < (int) actions.size() && actions[(size_t) index].enabled && actions[(size_t) index].run)
                actions[(size_t) index].run();
        });
        return;
    }
    juce::PopupMenu menu, shapes;
    bool separator = false;
    for (const auto& a : actions)
    {
        if (a.label.empty()) { separator = true; continue; }
        juce::PopupMenu::Item item(juce::String::fromUTF8(a.label.rfind("Insert Shape/", 0) == 0 ? a.label.substr(13).c_str() : a.label.c_str()));
        item.isEnabled = a.enabled;
        item.isTicked = a.ticked;
        if (! a.shortcut.empty()) item.shortcutKeyDescription = juce::String::fromUTF8(a.shortcut.c_str());
        auto run = a.run;
        item.action = [run] { if (run) run(); };
        if (a.label.rfind("Insert Shape/", 0) == 0) { shapes.addItem(std::move(item)); continue; }
        if (separator && menu.getNumItems() > 0) menu.addSeparator();
        separator = false;
        if (a.label == "Simplify Envelope") menu.addSubMenu("Insert Shape", shapes, shapes.containsAnyActiveItems());
        menu.addItem(std::move(item));
    }
    menu.showMenuAsync(juce::PopupMenu::Options());
}

bool AutomationEditor::keyPressed(const KeyEvent& k)
{
    if (editingValue_)
    {
        const auto result = valueEdit_.key(k);
        if (result == InlineTextEdit::Result::Commit) commitValueEdit();
        else if (result == InlineTextEdit::Result::Cancel) editingValue_ = false;
        repaint();
        return true;
    }
    if (ctx().selectedAutomationTrack != track_ || ! ctx().song) return false;
    const auto& sel = ctx().automationSelection;
    // Several lanes on one track: only the lane holding the selection answers (the first one when none does).
    const bool mine = selectionHere() || (sel.track != track_ && lane_ == 0);
    const bool del = k.keyCode == juce::KeyPress::deleteKey || k.keyCode == juce::KeyPress::backspaceKey;
    if (! mine) return del && ! k.cmd; // a lane with the focus never lets Delete reach the selected clip
    const char ch = (char) std::tolower((int) k.textChar);
    if (del && k.cmd)
    {
        // Cmd+Delete: the lane's whole automation (the reference's shortcut for the selected control).
        if (ctx().applyEdit && ! ctx().recording)
            if (auto edit = model::clearAutomation(*ctx().song, track_, targetId_); edit.apply) { clearSelection(); ctx().applyEdit(std::move(edit)); }
        return true;
    }
    if (del) { deleteSelection(); return true; } // automation focus must not delete the previously selected clip
    if (k.keyCode == juce::KeyPress::escapeKey && selectionHere()) { clearSelection(); repaint(); return true; }
    if (! k.cmd || k.alt) return false;
    if (ch == 'a')
    {
        for (const auto& a : contextActions(-100, -100)) if (a.label == "Select All" && a.run) a.run();
        return true;
    }
    if (ch == 'c' && ! k.shift) return copySelection(false);
    if (ch == 'x' && ! k.shift) return copySelection(true);
    if (ch == 'v' && ! k.shift) return pasteClipboard();
    if (ch == 'd' && ! k.shift) return duplicateSelection();
    return false;
}

// ---- UI-77 Automation Write mode
int AutomationEditor::touchControl(UiContext& ctx, const midi::ControlTarget& control)
{
    int track = -1;
    const int target = targetFor(ctx, control, track);
    if (target == -1 || track < 0) return -1;
    auto& lanes = ctx.automationLaneTargets[track];
    // An existing lane of this control stays where it is; otherwise the first lane switches to it.
    if (! ctx.showsAutomation(track) || std::find(lanes.begin(), lanes.end(), target) == lanes.end()) showTarget(ctx, track, target, false);
    else { ctx.mainView = MainView::Arrangement; ctx.selectedTrack = track; }
    return track;
}

std::optional<model::AutomationEnvelope> AutomationEditor::pointEnvelope(const UiContext& ctx, const midi::ControlTarget& control,
                                                                         double beat, int& track, int& targetId)
{
    track = -1;
    targetId = targetFor(ctx, control, track);
    if (targetId == -1 || track < 0) return std::nullopt;
    const auto all = targets(ctx, track);
    const auto found = std::find_if(all.begin(), all.end(), [&](const Target& t) { return t.id == targetId; });
    if (found == all.end()) return std::nullopt;
    double value = controlValue(ctx, track, *found);
    if (found->stepped) value = std::round(value);
    auto env = envelopeOf(ctx, track, *found);
    beat = std::max(0.0, beat);
    if (! isAutomated(ctx, track, targetId))
    {
        // A lane that never moved becomes flat at the new value (the point marks where it was set).
        for (auto& p : env.points) if (p.time < 0) p.value = value;
    }
    std::erase_if(env.points, [&](const auto& p) { return p.time >= 0 && std::abs(p.time - beat) < 1e-9; });
    env.points.push_back({beat, value});
    std::stable_sort(env.points.begin(), env.points.end(), [](const auto& a, const auto& b) { return a.time < b.time; });
    return env;
}

bool AutomationEditor::writePoint(UiContext& ctx, const midi::ControlTarget& control, double beat)
{
    if (! ctx.song || ! ctx.applyEdit || ctx.recording) return false;
    int track = -1, targetId = -1;
    auto env = pointEnvelope(ctx, control, beat, track, targetId);
    if (! env) return false;
    const auto all = targets(ctx, track);
    const auto found = std::find_if(all.begin(), all.end(), [&](const Target& t) { return t.id == targetId; });
    return found != all.end() && store(ctx, track, *found, std::move(*env)) != -1;
}

} // namespace sonora::ui
