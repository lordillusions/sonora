#include "engine/Automation.h"

#include "devices/DeviceProcessor.h"
#include "engine/EngineGraph.h"
#include "engine/Transport.h"

#include <algorithm>
#include <cmath>

namespace sonora::engine
{

AutomationLane::AutomationLane(const model::AutomationEnvelope& env) : points_(env.points), stepped_(env.stepped)
{
    std::stable_sort(points_.begin(), points_.end(),
                     [](const model::AutomationPoint& a, const model::AutomationPoint& b) { return a.time < b.time; });
}

double AutomationLane::valueAt(model::Beats beat) const
{
    if (points_.empty())
        return 0.0;

    // First point strictly after `beat`; with equal times this lands after the last of them,
    // so a jump takes effect exactly at its time.
    const auto next = std::upper_bound(points_.begin(), points_.end(), beat,
                                       [](model::Beats t, const model::AutomationPoint& p) { return t < p.time; });
    if (next == points_.begin())
        return next->value;
    const auto& prev = *(next - 1);
    if (next == points_.end() || stepped_)
        return prev.value;

    const double span = next->time - prev.time; // > 0: prev.time <= beat < next->time
    return prev.value + (next->value - prev.value) * model::segmentShape(prev, (beat - prev.time) / span); // ENG-11b curves
}

void applyBindingValue(const AutomationBinding& b, TrackControls& controls, double v)
{
    switch (b.kind)
    {
        case AutomationBinding::Kind::Volume:
            controls.volume.store((float) v, std::memory_order_relaxed);
            break;
        case AutomationBinding::Kind::Pan:
            controls.pan.store((float) std::clamp(v, -1.0, 1.0), std::memory_order_relaxed);
            break;
        case AutomationBinding::Kind::Active:
            controls.active.store(v >= 0.5, std::memory_order_relaxed);
            break;
        case AutomationBinding::Kind::Send:
            if ((size_t) b.index < controls.sends.size())
                controls.sends[(size_t) b.index].store((float) v, std::memory_order_relaxed);
            break;
        case AutomationBinding::Kind::DeviceParam:
            if (b.device != nullptr)
                b.device->setParameter(b.index, (float) v);
            break;
        case AutomationBinding::Kind::DeviceOn:
            if (b.device != nullptr)
                b.device->setBypassed(v < 0.5);
            break;
        case AutomationBinding::Kind::Tempo: // handled by AudioEngine (needs the transport)
            break;
    }
}

int automationOverrideKey(AutomationBinding::Kind kind, int index, int deviceSlot)
{
    using K = AutomationBinding::Kind;
    switch (kind)
    {
        case K::Volume: return 0;
        case K::Pan: return 1;
        case K::Active: return 2;
        case K::Send: return 3 + std::clamp(index, 0, 11);
        case K::DeviceOn: return 16 + std::clamp(deviceSlot, 0, 15);
        case K::DeviceParam: return 64 + std::clamp(deviceSlot, 0, 15) * 64 + (index & 63);
        case K::Tempo: return 15; // after the sends (3..14)
    }
    return -1;
}

bool readBindingValue(const AutomationBinding& b, const TrackControls& controls, double& value)
{
    using K = AutomationBinding::Kind;
    switch (b.kind)
    {
        case K::Volume: value = controls.volume.load(std::memory_order_relaxed); return true;
        case K::Pan: value = controls.pan.load(std::memory_order_relaxed); return true;
        case K::Active: value = controls.active.load(std::memory_order_relaxed) ? 1.0 : 0.0; return true;
        case K::Send:
            if ((size_t) b.index >= controls.sends.size()) return false;
            value = controls.sends[(size_t) b.index].load(std::memory_order_relaxed);
            return true;
        case K::DeviceParam:
            if (b.device == nullptr || b.hostedPlugin) return false;
            value = b.device->getParameter(b.index);
            return true;
        case K::DeviceOn:
            if (b.device == nullptr) return false;
            value = b.device->isBypassed() ? 0.0 : 1.0;
            return true;
        case K::Tempo: return false;
    }
    return false;
}

bool isStaticTempoEnvelope(const model::AutomationEnvelope& env)
{
    return env.points.size() == 1 && env.points.front().time < 0.0;
}

void applyTempoAutomation(const AutomationBinding& b, TrackControls& controls, Transport& transport, model::Beats beat)
{
    const int key = automationOverrideKey(b.kind, b.index, b.deviceSlot);
    if (isAutomationOverridden(controls, key))
        return; // set by hand: keep the user's tempo
    // A pending re-enable is consumed by applyAutomation later in this block; what the lane wrote before it is stale.
    const double current = transport.tempo(), last = controls.automationLast[(size_t) key];
    if (! controls.automationReEnable.load(std::memory_order_acquire) && controls.automationWrote.test((size_t) key)
        && std::fabs(current - last) > 1e-6 + 1e-4 * std::fabs(current))
    {
        setAutomationOverridden(controls, key, true);
        return;
    }
    transport.setTempo(b.lane.valueAt(beat));
    controls.automationLast[(size_t) key] = (float) transport.tempo();
    controls.automationWrote.set((size_t) key);
}

bool isAutomationOverridden(const TrackControls& c, int key)
{
    return key >= 0 && (c.automationOverrides[(size_t) key / 64].load(std::memory_order_relaxed) >> (key % 64)) & 1u;
}

void setAutomationOverridden(TrackControls& c, int key, bool on)
{
    if (key < 0) return;
    const uint64_t bit = uint64_t{1} << (key % 64);
    if (on) c.automationOverrides[(size_t) key / 64].fetch_or(bit, std::memory_order_relaxed);
    else c.automationOverrides[(size_t) key / 64].fetch_and(~bit, std::memory_order_relaxed);
}

bool automationOverridden(const EngineGraph& graph)
{
    if (! graph.controls)
        return false;
    for (size_t i = 0; i < graph.controls->size(); ++i)
        for (const auto& word : (*graph.controls)[i].automationOverrides)
            if (word.load(std::memory_order_relaxed) != 0)
                return true;
    return false;
}

void reEnableAutomation(EngineGraph& graph, int controlIndex, int key)
{
    if (! graph.controls || controlIndex < 0 || (size_t) controlIndex >= graph.controls->size())
        return;
    auto& c = (*graph.controls)[(size_t) controlIndex];
    if (key >= 0)
        setAutomationOverridden(c, key, false);
    else
        for (auto& word : c.automationOverrides) word.store(0, std::memory_order_relaxed);
    c.automationReEnable.store(true, std::memory_order_release);
}

void reEnableAutomation(EngineGraph& graph)
{
    if (! graph.controls)
        return;
    for (size_t i = 0; i < graph.controls->size(); ++i)
        reEnableAutomation(graph, (int) i);
}

void applyAutomation(const std::vector<AutomationBinding>& bindings, TrackControls& controls, model::Beats beat)
{
    if (controls.automationReEnable.exchange(false, std::memory_order_acq_rel))
        controls.automationWrote.reset(); // the lanes take their controls back: forget what they wrote
    for (const auto& b : bindings)
    {
        if (b.kind == AutomationBinding::Kind::Tempo)
            continue; // needs the transport: applyTempoAutomation
        const int key = automationOverrideKey(b.kind, b.index, b.deviceSlot);
        if (key >= 0)
        {
            if (isAutomationOverridden(controls, key))
                continue; // moved by hand: leave it where the user put it
            double current = 0.0;
            if (controls.automationWrote.test((size_t) key) && readBindingValue(b, controls, current)
                && std::fabs(current - (double) controls.automationLast[(size_t) key]) > 1e-6 + 1e-4 * std::fabs(current))
            {
                setAutomationOverridden(controls, key, true);
                continue;
            }
        }
        applyBindingValue(b, controls, b.lane.valueAt(beat));
        double written = 0.0;
        if (key >= 0 && readBindingValue(b, controls, written))
        {
            controls.automationLast[(size_t) key] = (float) written;
            controls.automationWrote.set((size_t) key);
        }
    }
}

} // namespace sonora::engine
