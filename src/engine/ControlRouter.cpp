#include "engine/ControlRouter.h"

#include "core/FaderCurve.h"
#include "engine/ClipLauncher.h"
#include "engine/RecordingManager.h"
#include "midi/MappingStore.h"

#include <algorithm>
#include <cmath>

namespace sonora::engine
{

namespace
{
using Kind = midi::ControlTarget::Kind;
constexpr float kTicksPerFullRange = 64.0f; // 64 encoder clicks sweep a whole control
constexpr double kTempoMin = 60.0, kTempoRange = 140.0;

bool validSend(const EngineGraph* g, int index)
{
    return g != nullptr && index >= 0 && (size_t) index < g->returnNodes.size() && (size_t) index < TrackControls::kMaxSends;
}

TrackControls* controlsFor(EngineGraph* g, int track)
{
    if (g == nullptr || g->controls == nullptr || track < 0 || (size_t) track >= g->controls->size())
        return nullptr;
    return &(*g->controls)[(size_t) track];
}

devices::DeviceProcessor* deviceFor(const EngineGraph* g, const midi::ControlTarget& t)
{
    if (g == nullptr)
        return nullptr;
    for (const auto& node : g->nodes)
        if (node.controlIndex == t.track)
            return t.device >= 0 && (size_t) t.device < node.processorForModelDevice.size()
                       ? node.processorForModelDevice[(size_t) t.device]
                       : nullptr;
    return nullptr;
}

int paramIndex(const devices::DeviceProcessor& d, const std::string& id)
{
    const auto& infos = d.parameters();
    for (size_t i = 0; i < infos.size(); ++i)
        if (infos[i].id == id)
            return (int) i;
    return -1;
}

float resolve(const midi::ControlChange& c, float current)
{
    return c.relative ? std::clamp(current + (float) c.ticks / kTicksPerFullRange, 0.0f, 1.0f) : c.value;
}
} // namespace

std::optional<float> readControl(const midi::ControlTarget& t, const EngineGraph* graph, const Transport& transport)
{
    auto* c = controlsFor(const_cast<EngineGraph*>(graph), t.track);
    switch (t.kind)
    {
        case Kind::TrackVolume: return c ? std::optional(model::faderGainToPosition(c->volume.load())) : std::nullopt;
        case Kind::TrackPan: return c ? std::optional((c->pan.load() + 1.0f) * 0.5f) : std::nullopt;
        case Kind::TrackSend:
            return c && validSend(graph, t.index)
                       ? std::optional(model::faderGainToPosition(c->sends[(size_t) t.index].load()))
                       : std::nullopt;
        case Kind::TrackActivator: return c ? std::optional(c->active.load() ? 1.0f : 0.0f) : std::nullopt;
        case Kind::TrackSolo: return c ? std::optional(c->solo.load() ? 1.0f : 0.0f) : std::nullopt;
        case Kind::TrackArm: return c ? std::optional(c->armed.load() ? 1.0f : 0.0f) : std::nullopt;
        case Kind::DeviceOn:
            if (auto* d = deviceFor(graph, t))
                return d->isBypassed() ? 0.0f : 1.0f;
            return std::nullopt;
        case Kind::DeviceParameter:
            if (auto* d = deviceFor(graph, t))
            {
                const int i = paramIndex(*d, t.param);
                if (i < 0)
                    return std::nullopt;
                const auto& info = d->parameters()[(size_t) i];
                const float span = info.maxValue - info.minValue;
                return span > 0.0f ? std::optional((d->getParameter(i) - info.minValue) / span) : std::nullopt;
            }
            return std::nullopt;
        case Kind::TransportPlay:
        case Kind::TransportToggle: return transport.isPlaying() ? 1.0f : 0.0f;
        case Kind::TransportStop: return transport.isPlaying() ? 0.0f : 1.0f;
        case Kind::Tempo: return (float) ((transport.tempo() - kTempoMin) / kTempoRange);
        case Kind::SessionClip: return std::nullopt; // model resolution is message-thread only
    }
    return std::nullopt;
}

bool applyControl(const midi::ControlTarget& t, const midi::ControlChange& change, EngineGraph* graph,
                  Transport& transport)
{
    auto* c = controlsFor(graph, t.track);
    // Buttons: a note toggles, a CC follows its value (>= 0.5 == pressed/on).
    auto buttonState = [&](bool current) { return change.trigger ? ! current : change.value >= 0.5f; };

    switch (t.kind)
    {
        case Kind::TrackVolume:
            if (c == nullptr)
                return false;
            c->volume.store(model::faderPositionToGain(resolve(change, model::faderGainToPosition(c->volume.load()))));
            return true;
        case Kind::TrackPan:
            if (c == nullptr)
                return false;
            c->pan.store(resolve(change, (c->pan.load() + 1.0f) * 0.5f) * 2.0f - 1.0f);
            return true;
        case Kind::TrackSend:
            if (c == nullptr || ! validSend(graph, t.index))
                return false;
            c->sends[(size_t) t.index].store(
                model::faderPositionToGain(resolve(change, model::faderGainToPosition(c->sends[(size_t) t.index].load()))));
            return true;
        case Kind::TrackActivator:
            if (c == nullptr)
                return false;
            c->active.store(buttonState(c->active.load()));
            return true;
        case Kind::TrackSolo:
            if (c == nullptr)
                return false;
            c->solo.store(buttonState(c->solo.load()));
            return true;
        case Kind::TrackArm:
            if (c == nullptr)
                return false;
            c->armed.store(buttonState(c->armed.load()));
            return true;
        case Kind::DeviceOn:
            if (auto* d = deviceFor(graph, t))
            {
                d->setBypassed(! buttonState(! d->isBypassed()));
                return true;
            }
            return false;
        case Kind::DeviceParameter:
            if (auto* d = deviceFor(graph, t))
            {
                const int i = paramIndex(*d, t.param);
                if (i < 0)
                    return false;
                const auto& info = d->parameters()[(size_t) i];
                const float span = info.maxValue - info.minValue;
                if (span <= 0.0f)
                    return false;
                float v;
                if (change.relative && (info.isToggle || ! info.choices.empty()))
                    v = d->getParameter(i) + (float) change.ticks; // step through discrete values
                else
                {
                    const float norm = resolve(change, (d->getParameter(i) - info.minValue) / span);
                    v = info.minValue + norm * span;
                }
                if (info.isToggle || ! info.choices.empty())
                    v = std::round(v);
                d->setParameter(i, v);
                return true;
            }
            return false;
        case Kind::TransportPlay:
            if (change.value < 0.5f && ! change.trigger)
                return false;
            transport.play();
            return true;
        case Kind::TransportStop:
            if (change.value < 0.5f && ! change.trigger)
                return false;
            transport.stop();
            return true;
        case Kind::TransportToggle:
            if (change.value < 0.5f && ! change.trigger)
                return false;
            transport.togglePlay();
            return true;
        case Kind::Tempo:
            transport.setTempo(kTempoMin + resolve(change, (float) ((transport.tempo() - kTempoMin) / kTempoRange)) * kTempoRange);
            return true;
        case Kind::SessionClip: return false;
    }
    return false;
}

bool applySessionClipControl(const midi::ControlTarget& target, const midi::ControlChange& change, const model::Song& song,
                             EngineGraph* graph, Transport& transport, const RecordingManager* recording)
{
    if (! graph || change.relative) return false;
    const auto location = midi::sessionClipLocation(song, target);
    if (! location || (recording && recording->capturesTrack(location->track))) return false;
    const auto* clip = model::findClip(song, *location);
    if (! clip || location->track < 0 || (size_t) location->track >= graph->nodes.size()) return false;
    const double quant = clipQuantisationBeats(*clip, song.globalQuantisation, song.timeSigNumerator, song.timeSigDenominator);
    if (change.released)
        releaseClip(*graph, transport, *clip, location->track, location->scene, quant);
    else if (change.trigger || change.value >= 0.5f)
        launchClipWithMode(*graph, transport, *clip, location->track, location->scene, quant, change.trigger ? change.value : 1.0f);
    else
        releaseClip(*graph, transport, *clip, location->track, location->scene, quant);
    return true;
}

std::optional<TouchedParameterWatch::Touch> TouchedParameterWatch::poll(const EngineGraph* graph)
{
    std::map<const devices::DeviceProcessor*, Seen> next;
    std::optional<Touch> touch;
    bool touchHeld = false;
    if (graph != nullptr)
        for (const auto& node : graph->nodes)
            for (size_t d = 0; d < node.processorForModelDevice.size(); ++d)
            {
                const auto* proc = node.processorForModelDevice[d];
                if (proc == nullptr || ! proc->isHostedPlugin()) continue;
                const auto count = proc->parameters().size();
                const uint32_t total = proc->handEditTotal();
                auto old = seen_.find(proc);
                auto& state = next[proc];
                if (old == seen_.end() || old->second.serials.size() != count)
                {
                    state.total = total;
                    state.serials.resize(count);
                    for (size_t i = 0; i < count; ++i) state.serials[i] = proc->handEdit((int) i).serial;
                    continue;
                }
                state = std::move(old->second);
                if (state.total == total) continue;
                state.total = total;
                int held = -1, loose = -1, looseCount = 0;
                for (size_t i = 0; i < count; ++i)
                {
                    const auto edit = proc->handEdit((int) i);
                    if (edit.gesture && held < 0) held = (int) i; // a held control wins even before it moves
                    if (edit.serial == state.serials[i]) continue;
                    state.serials[i] = edit.serial;
                    if (! edit.gesture) { if (loose < 0) loose = (int) i; ++looseCount; }
                }
                const int picked = held >= 0 ? held : (looseCount > 0 && looseCount <= 8 ? loose : -1);
                if (picked < 0 || (touch && touchHeld && held < 0)) continue;
                const auto& info = proc->parameters()[(size_t) picked];
                touch = Touch{{midi::ControlTarget::Kind::DeviceParameter, node.controlIndex, (int) d, 0, info.id}, info.name};
                touchHeld = held >= 0;
            }
    seen_ = std::move(next);
    return touch;
}

} // namespace sonora::engine
