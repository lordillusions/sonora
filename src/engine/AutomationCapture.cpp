#include "engine/AutomationCapture.h"

#include "core/Edit.h"
#include "engine/EngineGraph.h"

#include <algorithm>
#include <cmath>

namespace sonora::engine
{

namespace
{
double toleranceFor(AutomationBinding::Kind kind, double value)
{
    using K = AutomationBinding::Kind;
    if (kind == K::Active || kind == K::DeviceOn)
        return 0.25; // two-state
    return 1e-6 + 1e-4 * std::fabs(value);
}

bool hasLane(const model::Track& track, int targetId)
{
    for (const auto& env : track.automation)
        if (env.targetId == targetId && env.points.size() > 1) // a lone point is the stored static value
            return true;
    return false;
}
} // namespace

void AutomationCapture::begin()
{
    seen_.clear();
    prevDriven_.clear();
    wasOverridden_.clear();
    series_.clear();
}

void AutomationCapture::poll(const model::Song& song, const EngineGraph& graph, double beat)
{
    using Kind = AutomationBinding::Kind;
    if (! graph.controls)
        return;
    const int count = (int) std::min(graph.nodes.size(), graph.controls->size());
    for (int ci = 0; ci < count; ++ci)
    {
        const model::Track* track = model::trackAt(song, ci);
        if (track == nullptr)
            continue;
        const auto& controls = (*graph.controls)[(size_t) ci];
        auto visit = [&](int targetId, Kind kind, int index, int slot, double current) {
            if (targetId < 0)
                return;
            const int overrideKey = automationOverrideKey(kind, index, slot);
            const Key key{ci, kind, index, slot};
            const bool lane = hasLane(*track, targetId);
            const bool overridden = isAutomationOverridden(controls, overrideKey);
            if (lane && ! overridden)
            {
                prevDriven_[key] = current; // the lane drives it: not a hand move (yet)
                seen_[key] = current;
                wasOverridden_[key] = false;
                return;
            }
            const auto seen = seen_.find(key);
            const bool valid = seen != seen_.end();
            double before = valid ? seen->second : current;
            bool moved = valid && std::fabs(current - seen->second) > toleranceFor(kind, current);
            if (lane && overridden && ! wasOverridden_[key])
            {
                // the audio thread just noticed a hand move over a lane: the touch starts here
                if (const auto driven = prevDriven_.find(key); driven != prevDriven_.end())
                    before = driven->second;
                moved = true;
            }
            wasOverridden_[key] = overridden;
            seen_[key] = current;
            if (! moved)
                return;
            auto& series = series_[key];
            if (series.points.empty())
            {
                series.targetId = targetId;
                series.before = before;
                series.lastTime = beat;
            }
            if (series.points.empty() || std::fabs(series.last - current) > 1e-12)
            {
                series.points.push_back({beat, current});
                series.last = current;
                series.lastTime = beat;
            }
        };

        const auto& mixer = track->mixer;
        visit(mixer.volumeTargetId, Kind::Volume, 0, 0, controls.volume.load(std::memory_order_relaxed));
        visit(mixer.panTargetId, Kind::Pan, 0, 0, controls.pan.load(std::memory_order_relaxed));
        visit(mixer.speakerTargetId, Kind::Active, 0, 0, controls.active.load(std::memory_order_relaxed) ? 1.0 : 0.0);
        for (size_t s = 0; s < mixer.sendTargetIds.size() && s < controls.sends.size(); ++s)
            visit(mixer.sendTargetIds[s], Kind::Send, (int) s, 0, controls.sends[s].load(std::memory_order_relaxed));

        const auto& node = graph.nodes[(size_t) ci];
        for (size_t d = 0; d < track->devices.size() && d < node.processorForModelDevice.size(); ++d)
        {
            auto* proc = node.processorForModelDevice[d];
            if (proc == nullptr)
                continue;
            const auto& device = track->devices[d];
            visit(device.enabledTargetId, Kind::DeviceOn, 0, (int) d, proc->isBypassed() ? 0.0 : 1.0);
            if (proc->isHostedPlugin())
                continue; // plugin parameters change by themselves: not read back
            const auto& infos = proc->parameters();
            for (const auto& p : device.parameters)
            {
                if (p.automationTargetId < 0 || p.path == "On")
                    continue;
                const auto info = std::find_if(infos.begin(), infos.end(), [&](const devices::ParameterInfo& pi) { return pi.id == p.path; });
                if (info != infos.end())
                    visit(p.automationTargetId, Kind::DeviceParam, (int) (info - infos.begin()), (int) d,
                          proc->getParameter((int) (info - infos.begin())));
            }
        }
    }
}

std::vector<AutomationCapture::TouchedEnvelope> AutomationCapture::finish(const model::Song& song, const EngineGraph& graph, double endBeat)
{
    using Kind = AutomationBinding::Kind;
    std::vector<TouchedEnvelope> out;
    for (const auto& [key, series] : series_)
    {
        const model::Track* track = model::trackAt(song, key.track);
        if (track == nullptr || series.points.empty())
            continue;
        model::AutomationEnvelope env;
        for (const auto& existing : track->automation)
            if (existing.targetId == series.targetId)
                env = existing;
        if (env.targetId < 0)
        {
            env.targetId = series.targetId;
            const bool stepped = key.kind == Kind::Active || key.kind == Kind::DeviceOn;
            env.stepped = stepped;
            env.eventType = stepped ? "BoolEvent" : "FloatEvent";
            if (key.kind == Kind::DeviceParam && (size_t) key.track < graph.nodes.size()
                && (size_t) key.slot < graph.nodes[(size_t) key.track].processorForModelDevice.size())
                if (const auto* proc = graph.nodes[(size_t) key.track].processorForModelDevice[(size_t) key.slot])
                    if ((size_t) key.index < proc->parameters().size())
                    {
                        const auto& info = proc->parameters()[(size_t) key.index];
                        if (info.isToggle) { env.eventType = "BoolEvent"; env.stepped = true; }
                        else if (! info.choices.empty()) { env.eventType = "EnumEvent"; env.stepped = true; }
                    }
            env.points.push_back({-63072000.0, series.before}); // the value before the first breakpoint
        }
        const double first = series.points.front().time;
        const double last = std::max(endBeat, series.lastTime + 1e-3);
        std::erase_if(env.points, [&](const model::AutomationPoint& p) { return p.time >= 0.0 && p.time >= first - 1e-9 && p.time <= last + 1e-9; });
        env.points.push_back({first, series.before});
        for (const auto& p : series.points)
            env.points.push_back(p);
        env.points.push_back({last, series.last}); // a touched control holds its value until the take ends
        std::stable_sort(env.points.begin(), env.points.end(), [](const auto& a, const auto& b) { return a.time < b.time; });
        out.push_back({key.track, std::move(env), key.kind, key.index, key.slot});
    }
    series_.clear();
    seen_.clear();
    prevDriven_.clear();
    wasOverridden_.clear();
    return out;
}

} // namespace sonora::engine
