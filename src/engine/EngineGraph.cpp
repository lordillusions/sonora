#include "engine/EngineGraph.h"
#include "core/ClipOperations.h"
#include "midi/InputBlock.h"

#include "engine/SampleCache.h"
#include "io/als/AlsReader.h"

#include <algorithm>
#include <map>

namespace sonora::engine
{

void EngineGraph::prepare(double sampleRate, int maxBlockSize)
{
    preparedSampleRate = sampleRate;
    preparedBlockSize = maxBlockSize;
    for (auto& n : nodes)
    {
        n.buffer.setSize(2, maxBlockSize);
        n.buffer.clear(); // never leave allocated-but-unwritten samples uninitialised
        n.midi.ensureSize(midi::InputBlock::bufferCapacity * 2);
        n.inputMidi.ensureSize(midi::InputBlock::bufferCapacity);
        n.inputMonitorMask.resize((size_t) maxBlockSize);
        n.audioState.prepare(sampleRate, maxBlockSize);
        for (auto& d : n.devices)
            d->prepare(sampleRate, maxBlockSize);
        for (size_t t = 0; t < n.tap.size(); ++t)
            if (n.needTap[t])
            {
                n.tap[t].setSize(2, maxBlockSize);
                n.tap[t].clear();
            }
        n.trackDelaySamples = std::max(0, (int) std::lround(n.trackDelayMs * 0.001 * sampleRate));
        n.trackDelay.prepare(n.trackDelaySamples);
    }
    computeDelayCompensation();
}

// Latency of a node = slowest input path + its own devices. Every shorter path into a
// summing point (group, return, main) is delayed so that all contributions line up.
void EngineGraph::computeDelayCompensation()
{
    const size_t count = nodes.size();
    std::vector<int> inLatency(count, 0), total(count, 0);
    auto ownLatency = [&](const TrackNode& n) {
        int l = n.trackDelaySamples;
        for (const auto& d : n.devices)
            l += std::max(0, d->latencySamples());
        return l;
    };
    auto parentOf = [&](const TrackNode& n) { return n.outputNode >= 0 ? n.outputNode : mainNode; };

    for (int idx : trackOrder) // children before their groups
    {
        auto& n = nodes[(size_t) idx];
        total[(size_t) idx] = inLatency[(size_t) idx] + ownLatency(n);
        auto& up = inLatency[(size_t) parentOf(n)];
        up = std::max(up, total[(size_t) idx]);
        for (size_t r = 0; r < returnNodes.size() && r < n.sendEnabled.size(); ++r)
            if (n.sendEnabled[r])
            {
                auto& ret = inLatency[(size_t) returnNodes[r]];
                ret = std::max(ret, total[(size_t) idx]);
            }
    }
    for (int idx : returnNodes)
    {
        total[(size_t) idx] = inLatency[(size_t) idx] + ownLatency(nodes[(size_t) idx]);
        inLatency[(size_t) mainNode] = std::max(inLatency[(size_t) mainNode], total[(size_t) idx]);
    }
    total[(size_t) mainNode] = inLatency[(size_t) mainNode] + ownLatency(nodes[(size_t) mainNode]);

    for (size_t i = 0; i < count; ++i)
        nodes[i].latency = total[i];

    for (int idx : trackOrder)
    {
        auto& n = nodes[(size_t) idx];
        n.outDelay.prepare(inLatency[(size_t) parentOf(n)] - total[(size_t) idx]);
        n.sendDelays.assign(returnNodes.size(), DelayLine{});
        for (size_t r = 0; r < returnNodes.size() && r < n.sendEnabled.size(); ++r)
            if (n.sendEnabled[r])
                n.sendDelays[r].prepare(inLatency[(size_t) returnNodes[r]] - total[(size_t) idx]);
    }
    for (int idx : returnNodes)
        nodes[(size_t) idx].outDelay.prepare(inLatency[(size_t) mainNode] - total[(size_t) idx]);
}

namespace
{
// Everything an automation / clip-envelope target id can mean on one track.
struct TargetRef
{
    AutomationBinding::Kind kind = AutomationBinding::Kind::Volume;
    int index = 0;
    devices::DeviceProcessor* device = nullptr;
    float baseValue = 0.0f;
    int deviceSlot = 0; // position in the model device chain
};

std::map<int, TargetRef> collectTargets(const TrackNode& n, const model::Track& t)
{
    using Kind = AutomationBinding::Kind;
    std::map<int, TargetRef> out;
    int slot = 0; // the device being visited (set per device below)
    auto add = [&](int id, Kind kind, int index, devices::DeviceProcessor* dev, float base) {
        if (id >= 0)
            out[id] = {kind, index, dev, base, slot};
    };
    const auto& mx = t.mixer;
    add(mx.volumeTargetId, Kind::Volume, 0, nullptr, (float) mx.volume);
    add(mx.panTargetId, Kind::Pan, 0, nullptr, (float) mx.pan);
    add(mx.speakerTargetId, Kind::Active, 0, nullptr, mx.speakerOn ? 1.0f : 0.0f);
    add(mx.tempoTargetId, Kind::Tempo, 0, nullptr, 120.0f);
    for (size_t i = 0; i < mx.sendTargetIds.size(); ++i)
        add(mx.sendTargetIds[i], Kind::Send, (int) i, nullptr, i < mx.sends.size() ? (float) mx.sends[i] : 0.0f);

    for (size_t i = 0; i < t.devices.size() && i < n.processorForModelDevice.size(); ++i)
    {
        auto* proc = n.processorForModelDevice[i];
        if (proc == nullptr)
            continue;
        slot = (int) i;
        const auto& infos = proc->parameters();
        add(t.devices[i].enabledTargetId, Kind::DeviceOn, 0, proc, t.devices[i].enabled ? 1.0f : 0.0f); // every device has an on/off switch
        for (const auto& p : t.devices[i].parameters)
        {
            if (p.automationTargetId < 0)
                continue;
            if (p.path == "On")
            {
                add(p.automationTargetId, Kind::DeviceOn, 0, proc, t.devices[i].enabled ? 1.0f : 0.0f);
                continue;
            }
            const auto info = std::find_if(infos.begin(), infos.end(), [&](const devices::ParameterInfo& pi) { return pi.id == p.path; });
            if (info != infos.end())
                add(p.automationTargetId, Kind::DeviceParam, (int) (info - infos.begin()), proc, (float) p.value);
        }
        for (const auto& pp : t.devices[i].pluginParams) // plugin parameters (PLG-7), normalised 0..1
        {
            const int idx = proc->findParameterByHostId(pp.parameterId, pp.name);
            if (idx >= 0)
                add(pp.automationTargetId, Kind::DeviceParam, idx, proc, (float) pp.value);
        }
    }
    return out;
}

std::vector<ClipEnvelopeBinding> clipEnvelopeBindings(const model::Clip& c, bool session, int index,
                                                      const std::map<int, TargetRef>& targets)
{
    std::vector<ClipEnvelopeBinding> out;
    for (const auto& env : c.envelopes)
    {
        const auto it = targets.find(env.targetId);
        if (it == targets.end() || env.points.empty())
            continue;
        ClipEnvelopeBinding b;
        b.session = session;
        b.clipIndex = index;
        b.target = {it->second.kind, it->second.index, it->second.device, AutomationLane(env), it->second.deviceSlot,
                    it->second.device != nullptr && it->second.device->isHostedPlugin()};
        b.baseValue = it->second.baseValue;
        out.push_back(std::move(b));
    }
    return out;
}

// Matches the track's envelopes and its clips' envelopes to their targets. Envelopes whose target we
// cannot drive yet (plugin params PLG-7, devices nested in racks, tempo, ...) are reported in the
// build log, never an error.
void bindAutomation(TrackNode& n, const model::Track& t, std::vector<std::string>& log)
{
    n.midiControllers = prepareMidiControllers(t.automation);
    const auto targets = collectTargets(n, t);

    int unbound = 0;
    for (const auto& env : t.automation)
    {
        if (env.targetId < 0 || env.points.empty() || model::isMidiController(env.midiController))
            continue;
        const auto it = targets.find(env.targetId);
        if (it == targets.end())
        {
            if (env.points.size() > 1) // a lone point is the stored static value: nothing to play
                ++unbound;
            continue;
        }
        if (it->second.kind == AutomationBinding::Kind::Tempo && isStaticTempoEnvelope(env))
            continue; // Song::tempo already holds it; binding it would undo every tempo change
        n.automation.push_back({it->second.kind, it->second.index, it->second.device, AutomationLane(env), it->second.deviceSlot,
                                it->second.device != nullptr && it->second.device->isHostedPlugin()});
    }
    if (unbound > 0)
        log.push_back("Automation not played on '" + t.name + "': " + std::to_string(unbound)
                      + " envelope(s) target parameters we do not drive yet");

    // Session clip envelopes are bound once; arrangement ones live in the ClipSet (see buildClipSet).
    for (size_t s = 0; s < t.slots.size(); ++s)
        if (t.slots[s].clip && ! t.slots[s].clip->disabled)
            for (auto& b : clipEnvelopeBindings(*t.slots[s].clip, true, (int) s, targets))
                n.sessionEnvelopes.push_back(std::move(b));
}

std::vector<PreparedClip> prepareArrangementClips(const model::Track& t, const model::Song& song, SampleCache& samples,
                                                  std::vector<std::string>& log)
{
    std::vector<PreparedClip> out;
    for (const auto& c : t.arrangementClips)
    {
        if (c.disabled)
            continue;
        std::shared_ptr<SampleSlot> data;
        if (c.kind == model::Clip::Kind::Audio)
        {
            const auto path = io::resolveSamplePath(song, c.sample);
            if (path.empty())
                log.push_back("Sample not found: " + (c.sample.relativePath.empty() ? c.sample.absolutePath : c.sample.relativePath));
            else
                data = samples.request(path); // async when the cache has a loader pool
        }
        out.push_back(prepareClip(c, std::move(data)));
        if (out.back().expressionOverflow) log.push_back("MPE member channels exhausted in clip '" + c.name + "': extra overlapping notes play without expression");
    }
    return out;
}

void prepareTakes(ClipSet& set, const model::Track& t, const TrackNode& node, const model::Song& song,
                  SampleCache& samples, std::vector<std::string>& log)
{
    const auto targets = collectTargets(node, t);
    for (const auto& lane : t.takeLanes)
    {
        PreparedTakeLane prepared; prepared.id = lane.id;
        model::Track source; source.arrangementClips = lane.clips;
        prepared.clips = prepareArrangementClips(source, song, samples, log);
        int ci = 0;
        for (const auto& clip : lane.clips) if (! clip.disabled)
        {
            for (auto& binding : clipEnvelopeBindings(clip, false, ci, targets)) prepared.envelopes.push_back(std::move(binding));
            ++ci;
        }
        if (lane.audition && set.auditionLane < 0) set.auditionLane = lane.id;
        set.takes.push_back(std::move(prepared));
    }
}

TrackNode makeNode(const model::Track& t, int controlIndex, devices::BuildContext& ctx, SampleCache& samples,
                   const model::Song& song, std::vector<std::string>& log)
{
    if (t.deactivated)
    {
        // TRK-1: nothing is loaded; mixer controls (and their automation) stay bound so reactivation is seamless.
        auto empty = t;
        empty.deactivated = false; empty.frozen = false;
        empty.devices.clear(); empty.arrangementClips.clear(); empty.takeLanes.clear();
        for (auto& slot : empty.slots) slot.clip.reset();
        const auto& mx = t.mixer;
        std::erase_if(empty.automation, [&mx](const model::AutomationEnvelope& env) {
            return env.targetId != mx.volumeTargetId && env.targetId != mx.panTargetId && env.targetId != mx.speakerTargetId
                && std::find(mx.sendTargetIds.begin(), mx.sendTargetIds.end(), env.targetId) == mx.sendTargetIds.end();
        });
        auto n = makeNode(empty, controlIndex, ctx, samples, song, log);
        n.deactivated = true;
        n.outputEnabled = false;
        return n;
    }
    if (t.frozen)
    {
        auto playback = model::frozenPlaybackTrack(t);
        bool missing = false;
        for (const auto& clip : playback.arrangementClips) missing |= io::resolveSamplePath(song, clip.sample).empty();
        for (const auto& slot : playback.slots) if (slot.clip) missing |= io::resolveSamplePath(song, slot.clip->sample).empty();
        if (missing) { log.push_back("Frozen audio missing on '" + t.name + "': playing the retained original instead"); playback = t; playback.frozen = false; }
        return makeNode(playback, controlIndex, ctx, samples, song, log);
    }
    TrackNode n;
    n.kind = t.kind;
    n.name = t.name;
    n.controlIndex = controlIndex;
    n.trackDelayMs = (float) t.trackDelayMs;
    n.outputEnabled = t.outputRouting != "AudioOut/None";
    if ((t.kind == model::TrackKind::Audio || t.kind == model::TrackKind::Midi) && model::parseInput(t).kind == model::InputKind::Unsupported)
        log.push_back("Input of '" + t.name + "' is not supported yet (" + t.inputRouting + "): clip playback remains available");
    if (t.outputRouting.rfind("AudioOut/Track.", 0) == 0)
        log.push_back("Output of '" + t.name + "' is routed to another track (" + t.outputRouting + "): playing it into its group / Main instead");

    n.clips = prepareArrangementClips(t, song, samples, log);

    // Session slots: one prepared clip per scene (samples load asynchronously like everything else).
    for (const auto& slot : t.slots)
    {
        const bool has = slot.clip.has_value() && ! slot.clip->disabled;
        std::shared_ptr<SampleSlot> data;
        if (has && slot.clip->kind == model::Clip::Kind::Audio)
        {
            const auto path = io::resolveSamplePath(song, slot.clip->sample);
            if (! path.empty())
                data = samples.request(path);
        }
        n.sessionClips.push_back(has ? prepareSessionClip(*slot.clip, std::move(data)) : PreparedClip{});
        if (n.sessionClips.back().expressionOverflow) log.push_back("MPE member channels exhausted in a Session clip: extra overlapping notes play without expression");
        n.hasSessionClip.push_back(has ? 1 : 0);
    }

    n.sessionAvailability = n.hasSessionClip;

    for (const auto& d : t.devices)
    {
        auto p = devices::createDevice(d, ctx);
        n.processorForModelDevice.push_back(p.get());
        if (p)
            n.devices.push_back(std::move(p));
    }
    bindAutomation(n, t, log);
    {
        n.midiControllers = prepareMidiControllers(t.automation);
    const auto targets = collectTargets(n, t);
        int ci = 0;
        for (const auto& c : t.arrangementClips)
            if (! c.disabled)
            {
                for (auto& b : clipEnvelopeBindings(c, false, ci, targets))
                    n.clipEnvelopes.push_back(std::move(b));
                ++ci;
            }
    }
    ClipSet takes; prepareTakes(takes, t, n, song, samples, log);
    n.takes = std::move(takes.takes); n.auditionLane = takes.auditionLane;
    for (double send : t.mixer.sends)
        n.sendEnabled.push_back(send > 0.000317); // Live's -inf (-70 dB)
    return n;
}

void initControls(TrackControls& c, const model::Track& t)
{
    for (const auto& lane : t.takeLanes) if (lane.audition) { c.auditionLane.store(lane.id); break; }
    c.inputRoute.store(model::packInput(model::parseInput(t)));
    c.monitoring.store(t.monitoring);
    c.volume.store((float) t.mixer.volume);
    c.pan.store((float) t.mixer.pan);
    c.active.store(t.mixer.speakerOn);
    c.solo.store(t.mixer.solo);
    for (size_t i = 0; i < c.sends.size(); ++i)
        c.sends[i].store(i < t.mixer.sends.size() ? (float) t.mixer.sends[i] : 0.0f);
}
} // namespace

bool isTrackAudible(const EngineGraph& graph, const ControlBank& controls, const TrackNode& node, bool anySolo)
{
    if (node.deactivated || ! controls[(size_t) node.controlIndex].active.load(std::memory_order_relaxed))
        return false;
    if (! anySolo || node.kind == model::TrackKind::Group)
        return true;

    bool soloed = controls[(size_t) node.controlIndex].solo.load(std::memory_order_relaxed);
    for (int p = node.outputNode; ! soloed && p >= 0; p = graph.nodes[(size_t) p].outputNode)
        soloed = controls[(size_t) graph.nodes[(size_t) p].controlIndex].solo.load(std::memory_order_relaxed);
    return soloed;
}

std::unique_ptr<ClipSet> buildClipSet(const model::Track& t, const TrackNode& node, const model::Song& song,
                                      SampleCache& samples, std::vector<std::string>& log, bool takesOnly)
{
    if (t.deactivated)
    {
        auto empty = t;
        empty.deactivated = false; empty.frozen = false; empty.arrangementClips.clear(); empty.takeLanes.clear();
        for (auto& slot : empty.slots) slot.clip.reset();
        return buildClipSet(empty, node, song, samples, log, takesOnly);
    }
    if (t.frozen) return buildClipSet(model::frozenPlaybackTrack(t), node, song, samples, log, takesOnly);
    auto set = std::make_unique<ClipSet>();
    set->takesOnly = takesOnly;
    prepareTakes(*set, t, node, song, samples, log);
    if (takesOnly) return set;
    set->arrangement = prepareArrangementClips(t, song, samples, log);
    set->midiControllers = prepareMidiControllers(t.automation);
    const auto targets = collectTargets(node, t);
    for (const auto& env : t.automation)
        if (! env.points.empty())
            if (const auto it = targets.find(env.targetId);
                it != targets.end() && ! (it->second.kind == AutomationBinding::Kind::Tempo && isStaticTempoEnvelope(env)))
                set->automation.push_back({it->second.kind, it->second.index, it->second.device, AutomationLane(env), it->second.deviceSlot,
                                           it->second.device != nullptr && it->second.device->isHostedPlugin()});
    for (size_t i = 0; i < t.slots.size(); ++i)
    {
        const auto& slot = t.slots[i];
        const bool has = slot.clip && ! slot.clip->disabled;
        std::shared_ptr<SampleSlot> data;
        if (has && slot.clip->kind == model::Clip::Kind::Audio)
        {
            const auto path = io::resolveSamplePath(song, slot.clip->sample);
            if (! path.empty()) data = samples.request(path);
        }
        set->session.push_back(has ? prepareSessionClip(*slot.clip, std::move(data)) : PreparedClip{});
        if (set->session.back().expressionOverflow) log.push_back("MPE member channels exhausted in a Session clip: extra overlapping notes play without expression");
        set->hasSessionClip.push_back(has ? 1 : 0);
        if (has)
            for (auto& b : clipEnvelopeBindings(*slot.clip, true, (int) i, targets))
                set->sessionEnvelopes.push_back(std::move(b));
    }
    int ci = 0;
    for (const auto& c : t.arrangementClips)
        if (! c.disabled)
        {
            for (auto& b : clipEnvelopeBindings(c, false, ci, targets))
                set->envelopes.push_back(std::move(b));
            ++ci;
        }
    return set;
}

GraphBuildResult buildGraph(const model::Song& song, devices::BuildContext ctx, SampleCache& samples)
{
    GraphBuildResult result;
    ctx.log = &result.log;
    ctx.samples = &samples;
    ctx.song = &song;
    auto g = std::make_unique<EngineGraph>();
    g->controls = std::make_shared<ControlBank>(song.tracks.size() + song.returns.size() + 1);

    int ci = 0;
    std::map<model::TrackId, int> nodeById;
    for (const auto& t : song.tracks)
    {
        nodeById[t.id] = (int) g->nodes.size();
        initControls((*g->controls)[(size_t) ci], t);
        g->nodes.push_back(makeNode(t, ci++, ctx, samples, song, result.log));
    }
    for (const auto& t : song.returns)
    {
        g->returnNodes.push_back((int) g->nodes.size());
        initControls((*g->controls)[(size_t) ci], t);
        g->nodes.push_back(makeNode(t, ci++, ctx, samples, song, result.log));
    }
    g->mainNode = (int) g->nodes.size();
    initControls((*g->controls)[(size_t) ci], song.main);
    g->nodes.push_back(makeNode(song.main, ci, ctx, samples, song, result.log));

    // Group routing + order: deeper tracks first so children are summed into their group
    // before the group itself is processed.
    std::vector<int> depth(song.tracks.size(), 0);
    for (size_t i = 0; i < song.tracks.size(); ++i)
    {
        const auto& t = song.tracks[i];
        if (t.groupId >= 0)
            if (auto it = nodeById.find(t.groupId); it != nodeById.end() && it->second != (int) i)
                g->nodes[i].outputNode = it->second;
        int d = 0;
        for (auto gid = t.groupId; gid != -1 && d < 32; ++d)
        {
            auto it2 = nodeById.find(gid);
            gid = it2 == nodeById.end() ? -1 : song.tracks[(size_t) it2->second].groupId;
        }
        depth[i] = d;
    }
    // Sidechain inputs (ALS-12): the listening device reads another track's tapped audio, so that track
    // must be processed first.
    std::vector<std::pair<int, int>> edges; // (must run before, depends on it)
    const int numTracks = (int) song.tracks.size();
    auto linkSidechains = [&](const model::Track& t, size_t nodeIndex) {
        auto& node = g->nodes[nodeIndex];
        for (size_t di = 0; di < t.devices.size() && di < node.processorForModelDevice.size(); ++di)
        {
            const auto& sc = t.devices[di].sidechain;
            auto* proc = node.processorForModelDevice[di];
            if (! sc.routed() || proc == nullptr)
                continue;
            const auto src = nodeById.find(sc.sourceTrackId);
            if (src == nodeById.end() || src->second == (int) nodeIndex)
            {
                result.log.push_back("Sidechain of '" + t.devices[di].displayName + "' listens to a track we cannot find: ignored");
                continue;
            }
            const int tapIndex = sc.tap == model::Sidechain::Tap::PreFx ? 0 : sc.tap == model::Sidechain::Tap::PostFx ? 1 : 2;
            g->nodes[(size_t) src->second].needTap[(size_t) tapIndex] = true;
            node.sidechains.push_back({proc, src->second, tapIndex, (float) sc.gain});
            if ((int) nodeIndex < numTracks)
                edges.push_back({src->second, (int) nodeIndex});
        }
    };
    for (size_t i = 0; i < song.tracks.size(); ++i)
        linkSidechains(song.tracks[i], i);
    for (size_t i = 0; i < song.returns.size(); ++i)
        linkSidechains(song.returns[i], song.tracks.size() + i);
    linkSidechains(song.main, (size_t) g->mainNode);

    // Order: children before their groups (deeper first), sources before listeners. Kahn's algorithm,
    // picking the deepest ready track first so the result stays close to the old order.
    for (int i = 0; i < numTracks; ++i)
        if (g->nodes[(size_t) i].outputNode >= 0)
            edges.push_back({i, g->nodes[(size_t) i].outputNode});
    std::vector<int> indegree((size_t) numTracks, 0);
    for (const auto& e : edges)
        if (e.first < numTracks && e.second < numTracks)
            ++indegree[(size_t) e.second];
    std::vector<int> ready;
    for (int i = 0; i < numTracks; ++i)
        if (indegree[(size_t) i] == 0)
            ready.push_back(i);
    std::vector<char> placed((size_t) numTracks, 0);
    while (! ready.empty())
    {
        auto best = std::min_element(ready.begin(), ready.end(), [&](int a, int b) {
            return depth[(size_t) a] != depth[(size_t) b] ? depth[(size_t) a] > depth[(size_t) b] : a < b;
        });
        const int cur = *best;
        ready.erase(best);
        g->trackOrder.push_back(cur);
        placed[(size_t) cur] = 1;
        for (const auto& e : edges)
            if (e.first == cur && e.second < numTracks && --indegree[(size_t) e.second] == 0)
                ready.push_back(e.second);
    }
    for (int i = 0; i < numTracks; ++i)
        if (! placed[(size_t) i]) // a cycle: keep the track, process it last
        {
            g->trackOrder.push_back(i);
            result.log.push_back("Sidechain / group routing loop around '" + song.tracks[(size_t) i].name + "'");
        }

    std::sort(result.log.begin(), result.log.end());
    result.log.erase(std::unique(result.log.begin(), result.log.end()), result.log.end());
    result.graph = std::move(g);
    return result;
}

} // namespace sonora::engine
