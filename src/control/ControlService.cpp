#include "control/ControlService.h"
#include "control/ControlInternal.h"
#include "control/DeviceControl.h"
#include "core/DeviceCatalog.h"
#include "core/OpJson.h"
#include "engine/AudioFileFormats.h"
#include "io/als/AlsReader.h"
#include "io/als/AlsNewSet.h"
#include "io/als/AlsTemplates.h"
#include <regex>
#include <set>
#include <stdexcept>

namespace sonora::control
{
juce::var object(std::initializer_list<Field> fields)
{
    auto* out = new juce::DynamicObject();
    for (const auto& [key, value] : fields) out->setProperty(key, value);
    return juce::var(out);
}
namespace detail
{
void fields(const juce::var& value, std::initializer_list<const char*> allowed)
{
    auto* data = value.getDynamicObject();
    if (! data) throw std::runtime_error("Expected an object");
    for (const auto& property : data->getProperties())
    {
        bool found = false;
        for (const auto* key : allowed) found |= property.name == juce::Identifier(key);
        if (! found) throw std::runtime_error("Unknown field: " + property.name.toString().toStdString());
    }
}
double number(const juce::var& value, const char* name, double lo, double hi)
{
    const auto v = value[name];
    if (! (v.isInt() || v.isInt64() || v.isDouble())) throw std::runtime_error(std::string("Missing numeric ") + name);
    const double d = static_cast<double>(v);
    if (! std::isfinite(d) || d < lo || d > hi) throw std::runtime_error(std::string("Out of range: ") + name);
    return d;
}
int integer(const juce::var& value, const char* name, int lo, int hi)
{
    const double d = number(value, name, lo, hi);
    if (std::floor(d) != d) throw std::runtime_error(std::string("Expected integer ") + name);
    return static_cast<int>(d);
}
juce::String string(const juce::var& value, const char* name, int maximum)
{
    const auto field = value[name];
    if (! field.isString() || field.toString().isEmpty() || field.toString().length() > maximum)
        throw std::runtime_error(std::string("Expected a bounded non-empty string: ") + name);
    return field.toString();
}
bool boolean(const juce::var& value, const char* name)
{
    const auto field = value[name];
    if (! field.isBool()) throw std::runtime_error(std::string("Expected a boolean: ") + name);
    return static_cast<bool>(field);
}
bool has(const juce::var& value, const char* name) { return value.hasProperty(name); }
const char* kindName(model::TrackKind kind)
{
    switch (kind)
    {
        case model::TrackKind::Audio: return "audio";
        case model::TrackKind::Midi: return "midi";
        case model::TrackKind::Group: return "group";
        case model::TrackKind::Return: return "return";
        case model::TrackKind::Main: return "main";
    }
    return "audio";
}
int regularTrackIndex(const model::Song& song, int id)
{
    for (size_t i = 0; i < song.tracks.size(); ++i)
        if (song.tracks[i].id == id) return static_cast<int>(i);
    throw model::OpError("target_replaced", "Track ID no longer exists");
}
int controlIndexOf(const model::Song& song, int id, bool allowMain)
{
    for (size_t i = 0; i < song.tracks.size(); ++i)
        if (song.tracks[i].id == id) return static_cast<int>(i);
    for (size_t i = 0; i < song.returns.size(); ++i)
        if (song.returns[i].id == id) return static_cast<int>(song.tracks.size() + i);
    if (allowMain && model::mainTrackId(song) == id) return static_cast<int>(song.tracks.size() + song.returns.size());
    throw model::OpError("target_replaced", "Track ID no longer exists");
}
const model::Track* findAnyTrack(const model::Song& song, int id)
{
    for (const auto* list : {&song.tracks, &song.returns})
        for (const auto& candidate : *list) if (candidate.id == id) return &candidate;
    return model::mainTrackId(song) == id ? &song.main : nullptr;
}
bool hasAutomation(const model::Track& track, int target)
{
    if (target < 0) return false;
    const auto contains = [target](const auto& lanes) {
        return std::any_of(lanes.begin(), lanes.end(), [target](const auto& lane) { return lane.targetId == target && ! lane.points.empty(); });
    };
    if (contains(track.automation)) return true;
    for (const auto& clip : track.arrangementClips) if (contains(clip.envelopes)) return true;
    for (const auto& slot : track.slots) if (slot.clip && contains(slot.clip->envelopes)) return true;
    for (const auto& lane : track.takeLanes) for (const auto& clip : lane.clips) if (contains(clip.envelopes)) return true;
    return false;
}
} // namespace detail
using namespace detail;
namespace
{
int trackIndex(const model::Song& song, int id)
{
    for (size_t i = 0; i < song.tracks.size(); ++i)
        if (song.tracks[i].id == id) return static_cast<int>(i);
    throw std::runtime_error("Track ID no longer exists");
}
model::ClipLocation clipLocation(const model::Song& song, const juce::var& args)
{
    const int track = trackIndex(song, integer(args, "trackId", 0, 1000000000));
    const int uid = integer(args, "clipId", 0, 1000000000);
    if (args.hasProperty("scene") && args.hasProperty("laneId")) throw std::runtime_error("Choose scene OR laneId");
    if (args.hasProperty("scene")) return model::ClipLocation::session(track, uid, integer(args, "scene", 0, 1000000));
    if (args.hasProperty("laneId")) return model::ClipLocation::take(track, uid, integer(args, "laneId", 0, 1000000000));
    return model::ClipLocation::arrangement(track, uid);
}
// The public API addresses notes by revision-scoped array index; ops use per-clip note keys. Translate one
// command to the op form against the staged song (the keys opToCommand will see), keeping index diagnostics.
juce::var noteKeysForIndices(const model::Song& song, const juce::var& item)
{
    const auto type = item["type"].toString();
    if (type != "notes.update" && type != "notes.delete" && type != "notes.add" && type != "notes.replace") return item;
    for (const auto* value : {"updates", "noteIndices", "notes"})
        if (item.hasProperty(value) && item[value].getArray() && item[value].getArray()->size() > 4096)
            throw std::runtime_error("Expected 1–4096 note changes (replace also accepts empty)");
    if (type == "notes.add" || type == "notes.replace")
    {
        if (const auto* notes = item["notes"].getArray())
            for (const auto& note : *notes)
                if (note.hasProperty("noteIndex")) throw std::runtime_error("New notes cannot specify noteIndex");
        return item;
    }
    fields(item, {"type", "trackId", "clipId", "scene", "laneId", type == "notes.update" ? "updates" : "noteIndices"});
    const int trackId = integer(item, "trackId", 0, 1000000000);
    if (std::none_of(song.tracks.begin(), song.tracks.end(), [trackId](const auto& track) { return track.id == trackId; }))
        throw model::OpError("target_replaced", "Track ID no longer exists"); // same code as every other edit
    const auto location = clipLocation(song, item);
    const auto* clip = model::findClip(song, location);
    if (! clip) throw model::OpError("target_replaced", "Clip no longer exists at the specified location");
    const auto notes = model::keyedNotes(*clip);
    std::set<int> seen;
    const auto keyFor = [&](const juce::var& holder) {
        const int noteIndex = integer(holder, "noteIndex", 0, 1000000000);
        if (static_cast<size_t>(noteIndex) >= notes.size()) throw std::runtime_error("Note index no longer exists");
        if (! seen.insert(noteIndex).second) throw std::runtime_error("Duplicate note index");
        return static_cast<juce::int64>(notes[static_cast<size_t>(noteIndex)].key);
    };
    auto* translated = item.getDynamicObject()->clone().release();
    juce::var result(translated);
    juce::Array<juce::var> out;
    const auto* changes = item[type == "notes.update" ? "updates" : "noteIndices"].getArray();
    if (! changes) throw std::runtime_error("Expected 1–4096 note changes (replace also accepts empty)");
    for (const auto& change : *changes)
    {
        if (type == "notes.delete") { out.add(keyFor(object({{"noteIndex", change}}))); continue; }
        if (! change.getDynamicObject()) throw std::runtime_error("Expected an object");
        auto* update = change.getDynamicObject()->clone().release();
        juce::var updateVar(update);
        update->setProperty("noteKey", keyFor(change));
        update->removeProperty("noteIndex");
        out.add(updateVar);
    }
    if (type == "notes.delete") { translated->removeProperty("noteIndices"); translated->setProperty("noteKeys", out); }
    else translated->setProperty("updates", out);
    return result;
}
// A device nested in a rack, at any depth: the top-level device holding it and the chain/child indices down to it.
const model::Device* findNested(const model::Device& device, int deviceId, std::vector<int>& path, int depth = 0)
{
    if (depth > 16) return nullptr;
    for (size_t c = 0; c < device.chains.size(); ++c)
        for (size_t d = 0; d < device.chains[c].devices.size(); ++d)
        {
            const auto& child = device.chains[c].devices[d];
            path.push_back(static_cast<int>(c)); path.push_back(static_cast<int>(d));
            if (child.id == deviceId) return &child;
            if (const auto* inner = findNested(child, deviceId, path, depth + 1)) return inner;
            path.pop_back(); path.pop_back();
        }
    return nullptr;
}
bool descendantAutomation(const model::Track& track, const model::Device& device)
{
    for (const auto& chain : device.chains) for (const auto& child : chain.devices)
    {
        if (hasAutomation(track, child.enabledTargetId)) return true;
        for (const auto& p : child.parameters) if (hasAutomation(track, p.automationTargetId)) return true;
        for (const auto& p : child.pluginParams) if (hasAutomation(track, p.automationTargetId)) return true;
        if (descendantAutomation(track, child)) return true;
    }
    return false;
}
} // namespace

ControlService::ControlService(Backend backend) : backend_(std::move(backend)) {}

juce::var ControlService::result(const char* status, juce::var data)
{
    const auto state = backend_.state();
    return object({{"apiVersion", "1.0"}, {"ok", true}, {"status", status}, {"instanceId", instanceId_},
                   {"documentId", state.documentId}, {"revision", state.revision}, {"data", data}});
}
juce::var ControlService::failure(const std::string& code, const std::string& message)
{
    auto out = result("failed");
    out.getDynamicObject()->setProperty("ok", false);
    out.getDynamicObject()->setProperty("error", object({{"code", code}, {"message", message}}));
    return out;
}
juce::var ControlService::automationTarget(const model::Song& song, const juce::var& item)
{
    const auto type = item["type"].toString();
    const bool noPoints = type == "automation.clear" || type == "automation.get";
    if (noPoints) fields(item, {"type", "trackId", "target", "sendIndex", "deviceId", "parameterId", "targetId"});
    else fields(item, {"type", "trackId", "target", "sendIndex", "deviceId", "parameterId", "targetId", "points", "rangeStartBeat", "rangeEndBeat"});
    const bool clear = noPoints;
    const int trackId = integer(item, "trackId", 0, 1000000000);
    const model::Track* track = findAnyTrack(song, trackId);
    if (! track) throw model::OpError("target_replaced", "Track ID no longer exists");
    int targetId = -1;
    juce::String target;
    if (item.hasProperty("targetId"))
    {
        if (item.hasProperty("target")) throw std::runtime_error("Choose target OR targetId");
        targetId = integer(item, "targetId", 0, 1000000000);
    }
    else
    {
        target = string(item, "target", 32);
        const auto& mixer = track->mixer;
        if (target == "volume") targetId = mixer.volumeTargetId;
        else if (target == "pan") targetId = mixer.panTargetId;
        else if (target == "activator") targetId = mixer.speakerTargetId;
        else if (target == "tempo")
        {
            if (track->kind != model::TrackKind::Main) throw model::OpError("invalid_request", "Only the main track has a tempo lane");
            targetId = mixer.tempoTargetId;
        }
        else if (target == "send")
        {
            const int send = integer(item, "sendIndex", 0, 1000);
            if (static_cast<size_t>(send) >= mixer.sendTargetIds.size()) throw model::OpError("invalid_request", "No such send; sendIndex follows returnTracks[]");
            targetId = mixer.sendTargetIds[static_cast<size_t>(send)];
        }
        else if (target == "device")
        {
            const int deviceId = integer(item, "deviceId", 0, 1000000000);
            const auto parameter = string(item, "parameterId", 1024).toStdString();
            std::function<const model::Device*(const std::vector<model::Device>&)> find = [&](const std::vector<model::Device>& devices) -> const model::Device* {
                for (const auto& device : devices)
                {
                    if (device.id == deviceId) return &device;
                    for (const auto& chain : device.chains) if (const auto* inner = find(chain.devices)) return inner;
                }
                return nullptr;
            };
            const auto* device = find(track->devices);
            if (! device) throw model::OpError("target_replaced", "Device no longer exists on this track");
            if (parameter == "on") targetId = device->enabledTargetId;
            for (const auto& p : device->parameters) if (p.path == parameter) targetId = p.automationTargetId;
            for (const auto& p : device->pluginParams) if (std::to_string(p.parameterId) == parameter) targetId = p.automationTargetId;
            if (targetId < 0)
                throw model::OpError("parameter_mismatch", "That parameter has no automation target in this set (use parameterId from sonora_parameters_get, or \"on\")");
        }
        else throw std::runtime_error("target must be volume, pan, activator, tempo, send or device");
        if (targetId < 0) throw model::OpError("invalid_request", "This track has no " + target.toStdString() + " to automate");
    }
    auto* translated = new juce::DynamicObject();
    juce::var out(translated);
    translated->setProperty("type", item["type"]);
    translated->setProperty("trackId", trackId);
    translated->setProperty("targetId", targetId);
    if (clear) return out;
    const auto info = model::describeAutomationTarget(*track, targetId);
    // Native device parameters: the processor's range and kind (switch/choice) come from its descriptors.
    double lo = -1e12, hi = 1e12;
    if (info.kind == "device" && ! info.normalized && info.parameter != "on" && backend_.parameters)
    {
        const auto descriptors = backend_.parameters(trackId, info.deviceId);
        if (const auto* list = descriptors.getArray())
            for (const auto& d : *list)
                if (d["parameterId"].toString().toStdString() == info.parameter && d.hasProperty("minimum"))
                {
                    lo = static_cast<double>(d["minimum"]); hi = static_cast<double>(d["maximum"]);
                    const int steps = static_cast<int>(d["steps"]);
                    if (steps > 0) translated->setProperty("eventType", d["choices"].size() > 0 ? "EnumEvent" : "BoolEvent");
                }
    }
    const auto* points = item["points"].getArray();
    if (! points || points->size() > 4096) throw std::runtime_error("Expected an array of 0–4096 points");
    juce::Array<juce::var> converted;
    for (const auto& point : *points)
    {
        fields(point, {"beat", "value", "gainDb"});
        if (point.hasProperty("value") == point.hasProperty("gainDb")) throw std::runtime_error("Each point needs value OR gainDb");
        double value = 0;
        if (point.hasProperty("gainDb"))
        {
            if (info.kind != "volume" && info.kind != "send") throw std::runtime_error("gainDb applies to volume and send targets only");
            const double db = number(point, "gainDb", -1000, info.kind == "volume" ? 6.0 : 0.0);
            value = db <= -70 ? 0.0 : std::pow(10.0, db / 20.0);
        }
        else value = number(point, "value", -1e12, 1e12);
        if (value < lo || value > hi) throw std::runtime_error("Out of range: automation value (see the parameter's minimum/maximum)");
        converted.add(object({{"beat", point["beat"]}, {"value", value}}));
    }
    translated->setProperty("points", converted);
    for (const auto* key : {"rangeStartBeat", "rangeEndBeat"}) if (item.hasProperty(key)) translated->setProperty(key, item[key]);
    return out;
}

juce::var ControlService::inspect() { return result("observed", inspectSnapshot(backend_.state())); }
juce::var ControlService::context()
{
    const auto state = backend_.state();
    return object({{"documentId", state.documentId}, {"busy", state.busy},
        {"undoAvailable", state.busy.empty() && ! lastUndoRevision_.empty()
            && state.documentId == lastUndoDocument_ && state.revision == lastUndoRevision_}});
}

namespace
{
// One table per envelope class. Dispatch, validation and capabilities.get all read these, so they cannot drift.
const std::vector<std::string> queryOperations = {"project.inspect", "capabilities.get", "clips.get", "devices.parameters", "devices.catalog",
                                                  "automation.get", "status.get"};
const std::vector<std::string> mutationOperations = {"edits.apply", "devices.parameters.set", "devices.insert", "devices.remove", "devices.move",
                                                     "workflow.serum_supersaw", "history.undo", "history.step", "clips.import_audio"};
const std::vector<std::string> legacyEditTypes = {"tempo.set", "track.set_gain", "clip.duplicate", "notes.add", "notes.update", "notes.delete", "notes.replace",
                                                  "locator.add", "locator.update", "locator.delete", "track.set_folded", "clip.create", "clip.copy",
                                                  "clip.move", "clip.delete", "automation.set", "automation.clear", "track.rename", "track.set_color",
                                                  "loop.set"};
bool listed(const std::vector<std::string>& list, const juce::String& name)
{
    return std::find(list.begin(), list.end(), name.toStdString()) != list.end();
}
juce::var toArray(const std::vector<std::string>& list)
{
    juce::Array<juce::var> out;
    for (const auto& name : list) out.add(juce::String(name));
    return juce::var(out);
}
} // namespace

juce::var ControlService::capabilities()
{
    std::vector<std::string> operations = queryOperations, live;
    for (const auto& name : mutationOperations)
    {
        if (name == "clips.import_audio" && ! backend_.importAudio) continue;
        if (name == "history.step" && ! backend_.historyStep) continue;
        if (name == "devices.move" && ! backend_.syncModel && ! backend_.captureDevice) continue;
        operations.push_back(name);
    }
    for (const auto& name : liveOperationNames())
        if (backend_.live.count(name)) { operations.push_back(name); live.push_back(name); }
    std::vector<std::string> edits = legacyEditTypes;
    for (const auto& name : newEditTypeNames()) edits.push_back(name);
    return object({{"operations", toArray(operations)}, {"promptMode", "client-provided reasoning; optional configured native LLM"},
        {"editTypes", toArray(edits)}, {"liveActions", toArray(live)},
        {"limits", "Queries (target only), mutations (revision + idempotency key + undo token) and live actions (target only, no undo). "
                   "Edits: song, tracks, clips, regions, take lanes, notes, locators, automation. Devices: native, VST3, VST2 and Audio Unit plug-ins "
                   "on tracks, returns and main; top-level parameter writes. Live actions exist only when the host application supplies them."}});
}

juce::var ControlService::execute(const juce::var& request)
{
    if (executing_) return failure("busy", "Another control request is executing");
    const juce::ScopedValueSetter<bool> executing(executing_, true);
    try
    {
        fields(request, {"apiVersion", "operation", "arguments", "target", "expectedRevision", "idempotencyKey"});
        if (string(request, "apiVersion", 16) != "1.0") return failure("unsupported_version", "Expected apiVersion 1.0");
        const auto operation = string(request, "operation", 64);
        const auto state = backend_.state();
        if (operation == "project.inspect") return inspect();
        if (operation == "capabilities.get") return result("observed", capabilities());
        if (! listed(queryOperations, operation) && ! listed(mutationOperations, operation) && ! isLiveOperation(operation))
            return failure("unsupported_operation", "Unsupported operation: " + operation.toStdString());
        const auto target = request["target"];
        fields(target, {"instanceId", "documentId"});
        if (string(target, "instanceId").toStdString() != instanceId_) return failure("stale_instance", "Application instance changed");
        if (! state.song || string(target, "documentId").toStdString() != state.documentId)
            return failure("stale_document", "Inspect and explicitly target the current document");
        auto args = request["arguments"];
        if (args.isVoid()) args = object({});
        if (listed(queryOperations, operation)) return dispatch(operation, args, state);
        if (isLiveOperation(operation))
        {
            // Live actions carry no revision or idempotency key: they take absolute values and are safe to repeat.
            if (request.hasProperty("expectedRevision") || request.hasProperty("idempotencyKey"))
                return failure("invalid_request", "Live actions take no expectedRevision or idempotencyKey");
            return liveAction(operation, args, state);
        }
        if (receiptsDocument_ != state.documentId) { receipts_.clear(); receiptsDocument_ = state.documentId; }
        const auto key = string(request, "idempotencyKey", 128).toStdString();
        const auto body = juce::JSON::toString(request, true);
        if (const auto found = receipts_.find(key); found != receipts_.end())
            return found->second.body == body ? found->second.result : failure("idempotency_conflict", "Key was used with different arguments");
        if (string(request, "expectedRevision").toStdString() != state.revision)
            return failure("stale_revision", "The project changed; inspect again before editing");
        if (! state.busy.empty()) return failure("busy", state.busy);
        if (receipts_.size() >= 256) return failure("receipt_capacity", "Control receipt limit reached for this document");
        auto out = dispatch(operation, args, state);
        receipts_.emplace(key, Receipt{body, out});
        return out;
    }
    catch (const model::OpError& error) { return failure(error.code, error.what()); }
    catch (const std::exception& error) { return failure("invalid_request", error.what()); }
}

juce::var ControlService::liveAction(const juce::String& operation, const juce::var& args, const State& state)
{
    const auto handler = backend_.live.find(operation.toStdString());
    if (handler == backend_.live.end() || ! handler->second)
        return failure("unsupported_operation", operation.toStdString() + " is not available in this host");
    const auto normalised = normaliseLive(operation, args, state);
    juce::var receipt = object({});
    const auto applied = handler->second(normalised, receipt);
    if (! applied.ok) return failure(applied.code, applied.message);
    if (! receipt.getDynamicObject()) receipt = object({});
    if (! receipt.hasProperty("undoable")) receipt.getDynamicObject()->setProperty("undoable", false);
    return result("applied", receipt);
}

juce::var ControlService::dispatch(const juce::String& operation, const juce::var& args, const State& before)
{
    if (operation == "automation.get")
    {
        fields(args, {"trackId", "target", "sendIndex", "deviceId", "parameterId", "targetId", "offset", "limit"});
        const int offset = args.hasProperty("offset") ? integer(args, "offset", 0, 1000000000) : 0;
        const int limit = args.hasProperty("limit") ? integer(args, "limit", 1, 1024) : 512;
        auto item = args.getDynamicObject()->clone().release();
        juce::var query(item);
        item->removeProperty("offset"); item->removeProperty("limit");
        item->setProperty("type", "automation.get");
        const auto resolved = automationTarget(*before.song, query);
        const int trackId = static_cast<int>(resolved["trackId"]);
        const auto* track = findAnyTrack(*before.song, trackId);
        auto detail = automationDetail(*before.song, *track, static_cast<int>(resolved["targetId"]), offset, limit);
        detail.getDynamicObject()->setProperty("trackId", trackId);
        return result("observed", detail);
    }
    if (operation == "status.get")
    {
        fields(args, {});
        return result("observed", statusSnapshot(before, backend_));
    }
    if (operation == "devices.catalog")
    {
        fields(args, {"filter", "kind", "offset", "limit"});
        if (! backend_.deviceCatalog) return failure("unsupported_operation", "Device catalogue is unavailable");
        const auto filter = args.hasProperty("filter") ? string(args, "filter").toLowerCase() : juce::String();
        const auto kind = args.hasProperty("kind") ? string(args, "kind") : juce::String();
        if (kind.isNotEmpty() && kind != "native" && kind != "vst3" && kind != "vst2" && kind != "au") throw std::runtime_error("Choose native, vst3, vst2 or au kind");
        const int offset = args.hasProperty("offset") ? integer(args, "offset", 0, 1000000000) : 0;
        const int limit = args.hasProperty("limit") ? integer(args, "limit", 1, 128) : 64;
        const auto catalog = backend_.deviceCatalog(); juce::Array<juce::var> matched, page;
        if (const auto* entries = catalog.getArray()) for (const auto& entry : *entries)
            if ((kind.isEmpty() || entry["kind"].toString() == kind) && (filter.isEmpty()
                || entry["name"].toString().containsIgnoreCase(filter) || entry["catalogId"].toString().containsIgnoreCase(filter))) matched.add(entry);
        for (int i = offset; i < matched.size() && page.size() < limit; ++i) page.add(matched[i]);
        return result("observed", object({{"devices", page}, {"total", matched.size()}, {"offset", offset}, {"hasMore", offset + page.size() < matched.size()}}));
    }
    if (operation == "devices.parameters")
    {
        fields(args, {"trackId", "deviceId"});
        const int trackId = integer(args, "trackId", 0, 1000000000), deviceId = integer(args, "deviceId", 0, 1000000000);
        controlIndexOf(*before.song, trackId, true);
        return backend_.parameters ? result("observed", backend_.parameters(trackId, deviceId))
                                   : failure("unsupported_operation", "Plugin inspection is unavailable");
    }
    if (operation == "clips.get")
    {
        fields(args, {"trackId", "clipId", "scene", "laneId", "offset", "limit"});
        const auto* clip = model::findClip(*before.song, clipLocation(*before.song, args));
        if (! clip) return failure("target_replaced", "Clip no longer exists");
        const int offset = args.hasProperty("offset") ? integer(args, "offset", 0, 1000000000) : 0;
        const int limit = args.hasProperty("limit") ? integer(args, "limit", 1, 4096) : 4096;
        juce::Array<juce::var> notes;
        for (size_t i = static_cast<size_t>(offset); i < clip->notes.size() && notes.size() < limit; ++i)
        {
            const auto& note = clip->notes[i];
            notes.add(object({{"noteIndex", static_cast<int>(i)}, {"noteId", note.noteId}, {"pitch", note.pitch}, {"startBeat", note.start},
                {"durationBeats", note.duration}, {"velocity", note.velocity}, {"releaseVelocity", note.releaseVelocity},
                {"probability", note.probability}, {"enabled", note.enabled}, {"velocityDeviation", note.velocityDeviation}}));
        }
        return result("observed", object({{"notes", notes}, {"total", static_cast<int>(clip->notes.size())},
            {"offset", offset}, {"hasMore", static_cast<size_t>(offset) + static_cast<size_t>(notes.size()) < clip->notes.size()},
            {"loopStart", clip->loop.loopStart}, {"loopEnd", clip->loop.loopEnd}}));
    }
    if (operation == "history.undo")
    {
        fields(args, {"undoToken"});
        if (lastUndoRevision_.empty()) return failure("undo_conflict", "There is no latest agent edit to undo");
        if (string(args, "undoToken").toStdString() != lastUndoRevision_ || before.revision != lastUndoRevision_
            || before.documentId != lastUndoDocument_) return failure("undo_conflict", "Agent edit is no longer the latest unchanged action");
        if (lastUndoCheck_)
        {
            const auto checked = lastUndoCheck_();
            if (! checked.ok) return failure(checked.code, checked.message);
        }
        const auto applied = backend_.undo();
        if (! applied.ok) return failure(applied.code, applied.message);
        lastUndoRevision_.clear(); lastUndoCheck_ = {};
        return result("undone");
    }
    if (operation == "history.step")
    {
        fields(args, {"direction", "expectedLabel"});
        const auto direction = string(args, "direction", 8);
        if (direction != "undo" && direction != "redo") throw std::runtime_error("direction must be undo or redo");
        const bool undo = direction == "undo";
        const auto expected = string(args, "expectedLabel", 256).toStdString();
        if (! backend_.historyStep) return failure("unsupported_operation", "General history stepping is unavailable");
        if (undo ? ! before.history.canUndo : ! before.history.canRedo) return failure("undo_conflict", undo ? "There is nothing to undo" : "There is nothing to redo");
        const auto& next = undo ? before.history.undoLabel : before.history.redoLabel;
        if (expected != next)
            return failure("undo_conflict", "The next " + direction.toStdString() + " is '" + next + "', not the expected label; re-read status.get");
        std::string label;
        const auto stepped = backend_.historyStep(undo, label);
        if (! stepped.ok) return failure(stepped.code, stepped.message);
        lastUndoRevision_.clear(); lastUndoCheck_ = {};
        return result(undo ? "undone" : "redone", object({{"direction", direction}, {"label", label}, {"saved", false}, {"audioVerified", false}}));
    }
    if (operation == "clips.import_audio")
    {
        fields(args, {"paths", "trackId", "startBeat", "scene"});
        if (! backend_.importAudio) return failure("unsupported_operation", "Audio import is unavailable");
        const auto* list = args["paths"].getArray();
        if (! list || list->isEmpty() || list->size() > 16) throw std::runtime_error("Expected 1–16 absolute audio file paths");
        std::vector<std::string> paths;
        for (const auto& value : *list)
        {
            if (! value.isString() || value.toString().isEmpty() || value.toString().length() > 4096) throw std::runtime_error("Invalid path");
            if (! juce::File::isAbsolutePath(value.toString())) throw std::runtime_error("Paths must be absolute");
            const juce::File file(value.toString());
            if (! file.existsAsFile()) throw std::runtime_error("File does not exist: " + value.toString().toStdString());
            if (! engine::isAudioFile(file)) throw std::runtime_error("Not a supported audio file: " + file.getFileName().toStdString());
            paths.push_back(value.toString().toStdString());
        }
        int trackId = -1, scene = -1;
        double startBeat = 0.0;
        if (args.hasProperty("trackId"))
        {
            trackId = integer(args, "trackId", 0, 1000000000);
            const auto& track = before.song->tracks[static_cast<size_t>(regularTrackIndex(*before.song, trackId))];
            if (track.kind != model::TrackKind::Audio) throw model::OpError("incompatible_track", "Audio files go on audio tracks");
        }
        if (args.hasProperty("startBeat") && args.hasProperty("scene")) throw std::runtime_error("Choose startBeat OR scene");
        if (args.hasProperty("scene")) scene = integer(args, "scene", 0, 999);
        else if (args.hasProperty("startBeat")) startBeat = number(args, "startBeat", 0, 1000000);
        if (scene >= 0 && trackId < 0) throw std::runtime_error("scene needs a trackId");
        if (backend_.syncModel) backend_.syncModel();
        juce::var receipt = object({});
        const auto imported = backend_.importAudio(paths, trackId, startBeat, scene, receipt);
        if (! imported.ok) return failure(imported.code, imported.message);
        if (! receipt.getDynamicObject()) receipt = object({});
        const auto after = backend_.state();
        lastUndoRevision_ = after.revision; lastUndoDocument_ = after.documentId; lastUndoCheck_ = {};
        receipt.getDynamicObject()->setProperty("undoToken", juce::String(lastUndoRevision_));
        receipt.getDynamicObject()->setProperty("saved", false);
        receipt.getDynamicObject()->setProperty("audioVerified", false);
        return result("committed", receipt);
    }
    model::EditCommand command;
    juce::var report;
    int pluginTrack = -1, pluginDevice = -1, readbackDevice = -1;
    if (operation == "devices.parameters.set")
    {
        fields(args, {"trackId", "deviceId", "parameters"});
        const int trackId = integer(args, "trackId", 0, 1000000000), deviceId = integer(args, "deviceId", 0, 1000000000);
        const int index = controlIndexOf(*before.song, trackId, true);
        const auto& track = *model::trackAt(*before.song, index);
        auto found = std::find_if(track.devices.begin(), track.devices.end(), [deviceId](const auto& d) { return d.id == deviceId; });
        // AGT-7e: a device nested in a rack is written by editing it on the isolated rack processor and committing the whole top-level rack.
        std::vector<int> nestedPath;
        const model::Device* target = found != track.devices.end() ? &*found : nullptr;
        if (! target)
            for (auto top = track.devices.begin(); top != track.devices.end() && ! target; ++top)
            {
                nestedPath.clear();
                if (const auto* child = findNested(*top, deviceId, nestedPath)) { target = child; found = top; }
            }
        if (! target) return failure("target_replaced", "Device no longer exists");
        const int deviceIndex = static_cast<int>(found - track.devices.begin());
        if (found->kind == model::DeviceKind::Rack && descendantAutomation(track, *found))
            return failure("automation_conflict", "Rack descendants have authored automation; whole-rack parameter replacement is unavailable");
        if (! nestedPath.empty())
        {
            if (target->kind != model::DeviceKind::Native && target->kind != model::DeviceKind::Rack)
                return failure("unsupported_device", "Nested hosted plug-ins (VST3, VST2, Audio Unit) cannot be written through their rack; use a native device or a top-level plug-in");
        }
        else if (found->kind != model::DeviceKind::PluginVst3 && found->kind != model::DeviceKind::Native && found->kind != model::DeviceKind::Rack)
            return failure("unsupported_device", "Select a top-level native, rack or hosted VST3 device");
        const auto* values = args["parameters"].getArray();
        if (! values || values->isEmpty() || values->size() > 128) throw std::runtime_error("Expected 1–128 parameter writes");
        std::set<std::string> ids;
        std::vector<Backend::ParameterWrite> writes;
        for (const auto& value : *values)
        {
            fields(value, {"parameterId", "name", "normalized", "value"});
            if (value.hasProperty("normalized") == value.hasProperty("value")) throw std::runtime_error("Choose normalized OR native value");
            Backend::ParameterWrite write{string(value, "parameterId").toStdString(), string(value, "name").toStdString(),
                value.hasProperty("normalized") ? static_cast<float>(number(value, "normalized", 0, 1)) : 0};
            if (value.hasProperty("value")) write.value = number(value, "value", -1e12, 1e12);
            if (! ids.insert(write.parameterId).second) throw std::runtime_error("Duplicate parameter ID");
            for (const auto& p : target->pluginParams)
                if ((std::to_string(p.parameterId) == write.parameterId || p.name == write.name) && hasAutomation(track, p.automationTargetId))
                    return failure("automation_conflict", "Parameter has authored automation; edit its envelope instead");
            for (const auto& p : target->parameters)
                if (p.path == write.parameterId && hasAutomation(track, p.automationTargetId))
                    return failure("automation_conflict", "Native parameter has authored automation; edit its envelope instead");
            write.path = nestedPath;
            writes.push_back(std::move(write));
        }
        if (! backend_.prepareParameters) return failure("unsupported_operation", "Device parameter preparation is unavailable");
        model::Device captured, changed;
        const int topId = found->id; // nested writes edit the child on the rack processor and capture/commit the whole top-level rack
        const auto prepared = backend_.prepareParameters(trackId, topId, writes, captured, changed, report);
        if (! prepared.ok) return failure(prepared.code, prepared.message);
        const auto current = backend_.state();
        if (current.documentId != before.documentId || current.revision != before.revision || ! current.busy.empty())
            return failure("stale_revision", "Project changed during plugin preparation");
        if (backend_.captureDevice)
        {
            model::Device stillCurrent;
            const auto checked = backend_.captureDevice(trackId, topId, stillCurrent);
            if (! checked.ok || ! sameDeviceState(captured, stillCurrent))
                return failure("stale_revision", "Plugin state changed during preparation; inspect again");
        }
        model::Song staged = *current.song;
        model::trackAt(staged, index)->devices[static_cast<size_t>(deviceIndex)] = captured;
        changed.id = -1; model::allocateDeviceIds(staged, changed);
        const int newId = changed.id;
        command = model::replaceDevice(staged, index, deviceIndex, std::move(changed), "Set device parameters");
        const auto apply = command.apply;
        command.apply = [apply, nextId = staged.nextDeviceId, nextPointee = staged.nextPointeeId](model::Song& song) {
            song.nextDeviceId = std::max(song.nextDeviceId, nextId);
            song.nextPointeeId = std::max(song.nextPointeeId, nextPointee);
            song.pointeeIdsAllocated = true; apply(song);
        };
        if (! report.isObject()) report = object({});
        report.getDynamicObject()->setProperty("trackId", trackId);
        report.getDynamicObject()->setProperty("previousDeviceId", topId);
        report.getDynamicObject()->setProperty("deviceId", newId);
        if (! nestedPath.empty()) { report.getDynamicObject()->setProperty("nestedDeviceId", deviceId); readbackDevice = deviceId; }
        pluginTrack = trackId; pluginDevice = newId;
    }
    else if (operation == "devices.insert")
    {
        fields(args, {"trackId", "catalogId", "position", "replaceInstrument"});
        if (! backend_.prepareDevice) return failure("unsupported_operation", "Device insertion is unavailable");
        const int trackId = integer(args, "trackId", 0, 1000000000), index = controlIndexOf(*before.song, trackId, true);
        const bool replaceInstrument = args.hasProperty("replaceInstrument") && boolean(args, "replaceInstrument");
        const auto track = *model::trackAt(*before.song, index);
        if (track.devices.size() >= 64) return failure("capacity", "Agent insertion is limited to 64 top-level devices per track");
        model::Device device;
        const auto id = string(args, "catalogId", 1024);
        const auto prepared = backend_.prepareDevice(id.toStdString(), device, report);
        if (! prepared.ok) return failure(prepared.code, prepared.message);
        if (replaceInstrument && backend_.syncModel) backend_.syncModel(); // the replaced instrument is kept with its live state for undo
        const auto current = backend_.state();
        if (! current.song || current.documentId != before.documentId || current.revision != before.revision || ! current.busy.empty())
            return failure("stale_revision", "Project changed during device preparation");
        const auto role = model::deviceRole(device);
        if ((role == model::DeviceRole::Instrument || role == model::DeviceRole::MidiEffect) && track.kind != model::TrackKind::Midi)
            return failure("incompatible_track", "Instruments and MIDI effects require a MIDI track");
        const bool replacing = role == model::DeviceRole::Instrument && model::instrumentIndex(track) >= 0;
        if (replacing && ! replaceInstrument)
            return failure("instrument_conflict", "Track already has an instrument; pass replaceInstrument: true, remove it first or choose an empty MIDI track");
        int position = static_cast<int>(track.devices.size());
        if (role == model::DeviceRole::MidiEffect) position = 0;
        else if (role == model::DeviceRole::Instrument)
        { position = 0; while (position < static_cast<int>(track.devices.size()) && model::deviceRole(track.devices[static_cast<size_t>(position)]) == model::DeviceRole::MidiEffect) ++position; }
        if (args.hasProperty("position")) position = integer(args, "position", 0, static_cast<int>(track.devices.size()));
        auto ordered = track.devices; ordered.insert(ordered.begin() + position, device); int previous = 0;
        for (const auto& d : ordered)
        {
            if (replacing) break; // the browser policy places the new instrument and drops the old one
            const auto r = model::deviceRole(d); const int rank = r == model::DeviceRole::MidiEffect ? 0 : r == model::DeviceRole::Instrument ? 1 : 2;
            if (rank < previous) return failure("invalid_device_order", "Use MIDI effects, then instrument, then audio effects");
            previous = rank;
        }
        model::Song staged = *current.song; model::allocateDeviceIds(staged, device);
        device.sourceXml = std::make_shared<const std::string>(io::deviceTemplate(device));
        pluginTrack = trackId; pluginDevice = device.id;
        command = replacing ? model::insertTrackDevice(*current.song, index, std::move(device), args.hasProperty("position") ? position : -1)
                            : model::insertDevice(index, std::move(device), position);
        const auto apply = command.apply;
        command.apply = [apply, nextId = staged.nextDeviceId, nextPointee = staged.nextPointeeId](model::Song& song) {
            song.nextDeviceId = std::max(song.nextDeviceId, nextId); song.nextPointeeId = std::max(song.nextPointeeId, nextPointee);
            song.pointeeIdsAllocated = true; apply(song);
        };
        if (! report.getDynamicObject()) report = object({});
        report.getDynamicObject()->setProperty("trackId", trackId); report.getDynamicObject()->setProperty("deviceId", pluginDevice);
        report.getDynamicObject()->setProperty("catalogId", id); report.getDynamicObject()->setProperty("position", position);
        if (replacing) report.getDynamicObject()->setProperty("replacedInstrument", true);
    }
    else if (operation == "devices.remove")
    {
        fields(args, {"trackId", "deviceId"});
        const int trackId = integer(args, "trackId", 0, 1000000000), id = integer(args, "deviceId", 0, 1000000000);
        const int index = controlIndexOf(*before.song, trackId, true);
        if (! backend_.captureDevice) return failure("unsupported_operation", "Device capture is unavailable");
        model::Device captured; const auto capture = backend_.captureDevice(trackId, id, captured);
        if (! capture.ok) return failure(capture.code, capture.message);
        const auto current = backend_.state();
        if (current.documentId != before.documentId || current.revision != before.revision || ! current.busy.empty())
            return failure("stale_revision", "Project changed during device capture");
        command = model::removeDevice(*current.song, index, id);
        if (! command.apply) return failure("target_replaced", "Device no longer exists");
        const auto apply = command.apply;
        command.apply = [apply, index, id, captured](model::Song& song) {
            for (auto& device : model::trackAt(song, index)->devices) if (device.id == id) device = captured;
            apply(song);
        };
        report = object({{"trackId", trackId}, {"removedDeviceId", id}});
    }
    else if (operation == "devices.move")
    {
        fields(args, {"trackId", "deviceId", "position", "toTrackId"});
        const int trackId = integer(args, "trackId", 0, 1000000000), deviceId = integer(args, "deviceId", 0, 1000000000);
        if (args.hasProperty("position") == args.hasProperty("toTrackId")) throw std::runtime_error("Give position (reorder on this track) OR toTrackId (move to another track)");
        if (backend_.syncModel) backend_.syncModel(); // the device travels with the state it has right now
        const auto current = backend_.state();
        if (! current.song || current.documentId != before.documentId || current.revision != before.revision || ! current.busy.empty())
            return failure("stale_revision", "Project changed while preparing the move");
        const auto& song = *current.song;
        const int from = controlIndexOf(song, trackId, true);
        const auto& source = *model::trackAt(song, from);
        const auto found = std::find_if(source.devices.begin(), source.devices.end(), [deviceId](const auto& d) { return d.id == deviceId; });
        if (found == source.devices.end()) return failure("target_replaced", "Device no longer exists on that track (top-level devices only)");
        if (source.frozen) return failure("track_frozen", "Unfreeze the track before moving its devices");
        report = object({{"trackId", trackId}, {"previousDeviceId", deviceId}});
        if (args.hasProperty("toTrackId"))
        {
            const int toTrackId = integer(args, "toTrackId", 0, 1000000000), to = controlIndexOf(song, toTrackId, true);
            if (to == from) return failure("invalid_request", "That is the same track; use position to reorder");
            if (model::trackAt(song, to)->frozen) return failure("track_frozen", "Unfreeze the destination track first");
            if (! model::canHoldDevice(song, to, *found)) return failure("incompatible_track", "Instruments and MIDI effects need a MIDI track");
            const int newId = song.nextDeviceId;
            command = model::moveDeviceToTrack(song, from, deviceId, to, newId);
            if (! command.apply) return failure("invalid_request", "The device cannot move to that track");
            const auto apply = command.apply;
            command.apply = [apply, newId](model::Song& target) { target.nextDeviceId = std::max(target.nextDeviceId, newId + 1); apply(target); };
            report.getDynamicObject()->setProperty("trackId", toTrackId);
            report.getDynamicObject()->setProperty("previousTrackId", trackId);
            report.getDynamicObject()->setProperty("deviceId", newId);
        }
        else
        {
            const int position = integer(args, "position", 0, static_cast<int>(source.devices.size()) - 1);
            if (! model::canMoveAudioEffect(song, from, deviceId, position)) return failure("invalid_request", "Only audio effects move, and they stay after the instrument and MIDI effects");
            command = model::moveAudioEffect(song, from, deviceId, position);
            if (! command.apply) return failure("invalid_request", "The device is already at that position");
            report.getDynamicObject()->setProperty("deviceId", deviceId);
            report.getDynamicObject()->setProperty("position", position);
        }
    }
    else if (operation == "workflow.serum_supersaw")
    {
        fields(args, {"root", "mode", "bars", "startBeat"});
        if (string(args, "root") != "D#" || string(args, "mode") != "minor"
            || integer(args, "bars", 4, 4) != 4) return failure("unsupported_workflow", "First recipe supports D# minor, four bars");
        if (before.song->timeSigNumerator != 4 || before.song->timeSigDenominator != 4)
            return failure("unsupported_meter", "The four-bar recipe currently requires 4/4");
        const double start = number(args, "startBeat", 0, 1000000);
        if (! backend_.prepareSerum) return failure("missing_plugin", "Serum 2 preparation is unavailable");
        model::Device device;
        const auto prepared = backend_.prepareSerum(device, report);
        if (! prepared.ok) return failure(prepared.code, prepared.message);
        const auto current = backend_.state();
        if (current.documentId != before.documentId || current.revision != before.revision || ! current.busy.empty())
            return failure("stale_revision", "Project changed during plugin preparation");
        model::Song staged = *current.song;
        int endId = staged.nextPointeeId;
        auto track = io::createMidiTrack(staged, "Serum Supersaw · D# minor", 16, endId);
        staged.nextPointeeId = endId;
        model::allocateDeviceIds(staged, device);
        endId = std::max(endId, staged.nextPointeeId);
        device.sourceXml = std::make_shared<const std::string>(io::deviceTemplate(device));
        track.devices.push_back(std::move(device));
        model::Clip clip;
        clip.sourceXml = std::make_shared<const std::string>(io::midiClipTemplate());
        clip.uid = model::allocateClipUid(); clip.kind = model::Clip::Kind::Midi;
        clip.name = "D#m | B | F# | C#"; clip.time = clip.currentStart = start; clip.currentEnd = start + 16;
        clip.loop = {0, 16, 0, true}; clip.resetSourceMarkers = true;
        constexpr int chords[4][3] = {{63, 66, 70}, {59, 63, 66}, {61, 66, 70}, {61, 65, 68}};
        for (int bar = 0; bar < 4; ++bar)
            for (const int pitch : chords[bar])
            { model::MidiNote note; note.pitch = pitch; note.start = bar * 4.0; note.duration = 3.75; note.velocity = 88; clip.notes.push_back(note); }
        track.arrangementClips.push_back(std::move(clip));
        command = model::insertTracks({track}, endId, "Create Serum supersaw progression");
        const auto insert = command.apply;
        command.apply = [insert, nextDeviceId = staged.nextDeviceId](model::Song& song) {
            song.nextDeviceId = std::max(song.nextDeviceId, nextDeviceId); insert(song);
        };
        auto* data = report.getDynamicObject();
        if (! data) { report = object({}); data = report.getDynamicObject(); }
        data->setProperty("trackId", track.id); data->setProperty("clipId", track.arrangementClips.front().uid);
        data->setProperty("progression", "D#m | B | F# | C#"); data->setProperty("lengthBeats", 16);
    }
    else if (operation == "edits.apply")
    {
        fields(args, {"commands"});
        const auto* commands = args["commands"].getArray();
        if (! commands || commands->isEmpty() || commands->size() > 32) throw std::runtime_error("Expected 1–32 commands");
        // Structural track commands read and rewrite device chains: the live plug-in/mixer state goes into the model first, as in the UI.
        bool needsSync = false;
        for (const auto& item : *commands) needsSync |= editNeedsLiveSync(item["type"].toString());
        const State synced = needsSync && backend_.syncModel ? (backend_.syncModel(), backend_.state()) : before;
        if (! synced.song || synced.documentId != before.documentId || synced.revision != before.revision || ! synced.busy.empty())
            return failure("stale_revision", "Project changed while syncing live state");
        model::Song staged = *synced.song;
        for (size_t i = 0; i < staged.tracks.size() && i < before.liveGains.size(); ++i)
            staged.tracks[i].mixer.volume = before.liveGains[i];
        if (before.liveTempo > 0) staged.tempo = before.liveTempo;
        // clip.create copies the clip format from the set, or from this template when the set has no MIDI clip yet.
        if (! staged.midiClipTemplate) staged.midiClipTemplate = std::make_shared<const std::string>(io::midiClipTemplate());
        std::vector<model::EditCommand> parts;
        juce::Array<juce::var> createdTracks;
        juce::var extra = object({});
        try
        {
            for (const auto& item : *commands)
            {
                // COL-1: every edit is a model::Op, the same intent collab peers exchange.
                const auto type = item["type"].toString();
                if (! item.getDynamicObject()) throw std::runtime_error("Expected an object");
                if (! listed(legacyEditTypes, type) && ! isNewEditType(type))
                    return failure("unsupported_operation", "Unsupported edit type: " + type.toStdString());
                if (isNewEditType(type))
                {
                    auto built = buildEdit(staged, item);
                    if (! built.command.apply) throw std::runtime_error("The command changes nothing");
                    built.command.apply(staged);
                    parts.push_back(std::move(built.command));
                    for (const auto& track : built.tracks) createdTracks.add(track);
                    if (built.extra.getDynamicObject())
                        for (const auto& property : built.extra.getDynamicObject()->getProperties())
                        {
                            // Lists (deletedTrackIds) accumulate across the batch; other members keep the latest value.
                            auto merged = extra.hasProperty(property.name) && property.value.getArray() ? extra[property.name] : juce::var(juce::Array<juce::var>());
                            if (property.value.getArray()) { for (const auto& value : *property.value.getArray()) merged.getArray()->add(value); extra.getDynamicObject()->setProperty(property.name, merged); }
                            else extra.getDynamicObject()->setProperty(property.name, property.value);
                        }
                    continue;
                }
                const auto translated = type.startsWith("automation.") ? automationTarget(staged, item) : noteKeysForIndices(staged, item);
                auto part = model::opToCommand(staged, model::opFromJson(translated));
                part.apply(staged); parts.push_back(std::move(part));
            }
        }
        catch (const model::OpError& error) { return failure(error.code, error.what()); }
        command = model::compose("Agent edit", std::move(parts));
        // UI-3b: handles of locators this batch created, in creation order (ids only grow).
        juce::Array<juce::var> created;
        for (const auto& locator : staged.locators)
            if (! model::findLocator(*before.song, locator.id)) created.add(locator.id);
        report = extra;
        if (! createdTracks.isEmpty()) report.getDynamicObject()->setProperty("tracks", createdTracks);
        if (! created.isEmpty())
        {
            std::sort(created.begin(), created.end(), [](const juce::var& a, const juce::var& b) { return static_cast<int>(a) < static_cast<int>(b); });
            report.getDynamicObject()->setProperty("locatorIds", created);
        }
        // AGT-7d: handles of clips this batch created (clip.create/copy/duplicate/move to another track). The commit
        // replays the staged command, which keeps the UIDs its first apply allocated.
        juce::Array<juce::var> clipsCreated;
        for (const auto& track : staged.tracks)
        {
            const auto old = std::find_if(before.song->tracks.begin(), before.song->tracks.end(), [&track](const auto& t) { return t.id == track.id; });
            const auto existed = [&](int uid, int scene) {
                if (old == before.song->tracks.end()) return false;
                if (scene < 0) return std::any_of(old->arrangementClips.begin(), old->arrangementClips.end(), [uid](const auto& c) { return c.uid == uid; });
                return static_cast<size_t>(scene) < old->slots.size() && old->slots[static_cast<size_t>(scene)].clip && old->slots[static_cast<size_t>(scene)].clip->uid == uid;
            };
            for (const auto& clip : track.arrangementClips)
                if (! existed(clip.uid, -1)) clipsCreated.add(object({{"trackId", track.id}, {"clipId", clip.uid}, {"startBeat", clip.currentStart}, {"endBeat", clip.currentEnd}}));
            for (size_t scene = 0; scene < track.slots.size(); ++scene)
                if (const auto& slot = track.slots[scene]; slot.clip && ! existed(slot.clip->uid, static_cast<int>(scene)))
                    clipsCreated.add(object({{"trackId", track.id}, {"clipId", slot.clip->uid}, {"scene", static_cast<int>(scene)}}));
        }
        if (! clipsCreated.isEmpty()) report.getDynamicObject()->setProperty("clips", clipsCreated);
    }
    else return failure("unsupported_operation", "Unsupported operation: " + operation.toStdString());
    ApplyResult applied;
    try { applied = backend_.apply(std::move(command)); }
    catch (const std::exception& error) { return failure("outcome_unknown", error.what()); }
    if (! applied.ok) return failure(applied.code, applied.message);
    std::function<ApplyResult()> undoCheck;
    if (pluginTrack >= 0 && backend_.captureDevice)
    {
        // Only capture at commit/undo, never on routine inspection. Non-parameter editor changes may
        // leave the cheap revision unchanged, but must not be overwritten by a whole-state inverse.
        model::Device baseline;
        const auto captured = backend_.captureDevice(pluginTrack, pluginDevice, baseline);
        bool verified = captured.ok;
        if (const auto* expected = report["parameters"].getArray())
        {
            const auto actual = backend_.parameters ? backend_.parameters(pluginTrack, readbackDevice >= 0 ? readbackDevice : pluginDevice) : juce::var{};
            for (const auto& value : *expected)
            {
                bool matched = false;
                if (const auto* values = actual.getArray())
                    for (const auto& observed : *values)
                        if (observed["parameterId"] == value["parameterId"] && observed["name"] == value["name"])
                        {
                            const double normalized = static_cast<double>(observed["normalized"]);
                            matched = std::isfinite(normalized) && std::abs(normalized - static_cast<double>(value["normalized"])) <= 1e-4;
                            if (value.hasProperty("value"))
                            {
                                const double actualValue = static_cast<double>(observed["value"]), expectedValue = static_cast<double>(value["value"]);
                                matched &= observed.hasProperty("value") && std::isfinite(actualValue)
                                    && std::abs(actualValue - expectedValue) <= 1e-4 * std::max(1.0, std::abs(expectedValue));
                            }
                        }
                verified &= matched;
            }
        }
        if (! verified)
        {
            const auto rolledBack = backend_.undo();
            return failure(rolledBack.ok ? "plugin_commit_failed" : "outcome_unknown", rolledBack.ok
                ? "Live device readback failed; the parameter transaction was undone"
                : "Live device verification and rollback failed; inspect before continuing");
        }
        report.getDynamicObject()->setProperty("liveReadbackVerified", true);
        undoCheck = [this, pluginTrack, pluginDevice, baseline] {
            model::Device current;
            if (! backend_.captureDevice(pluginTrack, pluginDevice, current).ok || ! sameDeviceState(baseline, current))
                return ApplyResult::failure("undo_conflict", "Device state changed or could not be verified; undo refused");
            return ApplyResult::success();
        };
    }
    lastUndoCheck_ = std::move(undoCheck);
    const auto after = backend_.state();
    lastUndoRevision_ = after.revision; lastUndoDocument_ = after.documentId;
    if (! report.isObject()) report = object({});
    report.getDynamicObject()->setProperty("undoToken", juce::String(lastUndoRevision_));
    report.getDynamicObject()->setProperty("saved", false);
    report.getDynamicObject()->setProperty("audioVerified", false);
    return result("committed", report);
}

juce::var ControlService::submit(const juce::String& prompt)
{
    if (prompt.length() > 8192) return failure("invalid_request", "Prompt exceeds 8192 characters");
    const auto text = prompt.toLowerCase().trim();
    const auto normalized = std::regex_replace(text.replaceCharacter(',', ' ').toStdString(), std::regex("\\s+"), " ");
    const std::regex serumFirst("(?:please )?(?:make|create)(?: me)?(?: a)? serum(?: 2)? supersaw(?: lead)?(?: chord progression)? in (?:d#|d sharp)(?: (?:minor|scale))? (?:with|for) (?:4|four) bars[.!]?");
    const std::regex soundFirst("(?:please )?(?:make|create)(?: me)?(?: a)? supersaw(?: lead)?(?: chord progression)? (?:with|using) serum(?: 2)? (?:in|and in) (?:d#|d sharp)(?: (?:minor|scale))? (?:with|for) (?:4|four) bars[.!]?");
    if (text == "inspect" || text == "inspect project") return inspect();
    const auto state = backend_.state();
    juce::String operation;
    juce::var args;
    if (text == "undo") { operation = "history.undo"; args = object({{"undoToken", lastUndoRevision_}}); }
    else if (std::regex_match(normalized, serumFirst) || std::regex_match(normalized, soundFirst))
    { operation = "workflow.serum_supersaw"; args = object({{"root", "D#"}, {"mode", "minor"}, {"bars", 4}, {"startBeat", 0}}); }
    else
    {
        std::smatch match;
        const auto string = text.toStdString();
        if (std::regex_match(string, match, std::regex("(?:set )?tempo(?: to)? ([0-9]+(?:\\.[0-9]+)?)")))
        { operation = "edits.apply"; args = object({{"commands", juce::Array<juce::var>{object({{"type", "tempo.set"}, {"bpm", std::stod(match[1])}})}}}); }
        else return failure("unsupported_prompt", "Built-in workflows: inspect project; set tempo to 128; undo; or make a Serum 2 supersaw in D# minor with 4 bars. Connect an MCP agent for broader reasoning.");
    }
    return execute(object({{"apiVersion", "1.0"}, {"operation", operation}, {"arguments", args},
        {"target", object({{"instanceId", instanceId_}, {"documentId", state.documentId}})},
        {"expectedRevision", state.revision}, {"idempotencyKey", juce::Uuid().toString()}}));
}
} // namespace sonora::control
