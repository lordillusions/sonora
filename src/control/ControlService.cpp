#include "control/ControlService.h"
#include "control/DeviceControl.h"
#include "core/DeviceCatalog.h"
#include "core/OpJson.h"
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
namespace
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
juce::String string(const juce::var& value, const char* name, int maximum = 256)
{
    const auto field = value[name];
    if (! field.isString() || field.toString().isEmpty() || field.toString().length() > maximum)
        throw std::runtime_error(std::string("Expected a bounded non-empty string: ") + name);
    return field.toString();
}
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
juce::var snapshot(const State& state)
{
    juce::Array<juce::var> tracks;
    if (state.song)
        for (const auto& track : state.song->tracks)
        {
            juce::Array<juce::var> clips, devices, sessionClips, takeClips;
            for (const auto& clip : track.arrangementClips)
                if (clips.size() < 512)
                    clips.add(object({{"clipId", clip.uid}, {"name", clip.name}, {"startBeat", clip.currentStart},
                                      {"endBeat", clip.currentEnd}, {"noteCount", static_cast<int>(clip.notes.size())}}));
            for (size_t scene = 0; scene < track.slots.size(); ++scene)
                if (const auto& slot = track.slots[scene]; slot.clip && sessionClips.size() < 512)
                    sessionClips.add(object({{"clipId", slot.clip->uid}, {"scene", static_cast<int>(scene)}, {"name", slot.clip->name}}));
            for (const auto& lane : track.takeLanes)
                for (const auto& clip : lane.clips)
                    if (takeClips.size() < 512) takeClips.add(object({{"clipId", clip.uid}, {"laneId", lane.id}, {"name", clip.name}}));
            for (const auto& device : track.devices)
                devices.add(object({{"deviceId", device.id}, {"name", device.displayName}, {"type", device.type},
                                    {"hosted", device.plugin.has_value()}}));
            const auto index = static_cast<size_t>(tracks.size());
            const double gain = index < state.liveGains.size() ? state.liveGains[index] : track.mixer.volume;
            tracks.add(object({{"trackId", track.id}, {"name", track.name}, {"gainLinear", gain}, {"authoredGainLinear", track.mixer.volume},
                               {"clips", clips}, {"sessionClips", sessionClips}, {"takeClips", takeClips}, {"devices", devices}}));
        }
    return object({{"documentId", state.documentId}, {"revision", state.revision}, {"busy", state.busy},
                   {"tempo", state.liveTempo > 0 ? state.liveTempo : (state.song ? state.song->tempo : 0.0)}, {"tracks", tracks}});
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
juce::var ControlService::inspect() { return result("observed", snapshot(backend_.state())); }
juce::var ControlService::context()
{
    const auto state = backend_.state();
    return object({{"documentId", state.documentId}, {"busy", state.busy},
        {"undoAvailable", state.busy.empty() && ! lastUndoRevision_.empty()
            && state.documentId == lastUndoDocument_ && state.revision == lastUndoRevision_}});
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
        if (operation == "capabilities.get")
            return result("observed", object({{"operations", juce::var(juce::StringArray{
                "project.inspect", "capabilities.get", "clips.get", "devices.parameters", "edits.apply",
                "devices.parameters.set", "devices.catalog", "devices.insert", "devices.remove", "workflow.serum_supersaw", "history.undo"})}, {"promptMode", "client-provided reasoning; optional configured native LLM"},
                {"editTypes", juce::var(juce::StringArray{"tempo.set", "track.set_gain", "clip.duplicate", "notes.add", "notes.update", "notes.delete", "notes.replace"})},
                {"limits", "Top-level native/rack/VST3 controls and device insertion/removal; parameter writes rebuild processor; Arrangement/Session/take notes; no save/export commands yet"}}));
        const auto target = request["target"];
        fields(target, {"instanceId", "documentId"});
        if (string(target, "instanceId").toStdString() != instanceId_) return failure("stale_instance", "Application instance changed");
        if (! state.song || string(target, "documentId").toStdString() != state.documentId)
            return failure("stale_document", "Inspect and explicitly target the current document");
        auto args = request["arguments"];
        if (args.isVoid()) args = object({});
        if (operation == "clips.get" || operation == "devices.parameters" || operation == "devices.catalog") return dispatch(operation, args, state);
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
    catch (const std::exception& error) { return failure("invalid_request", error.what()); }
}

juce::var ControlService::dispatch(const juce::String& operation, const juce::var& args, const State& before)
{
    if (operation == "devices.catalog")
    {
        fields(args, {"filter", "kind", "offset", "limit"});
        if (! backend_.deviceCatalog) return failure("unsupported_operation", "Device catalogue is unavailable");
        const auto filter = args.hasProperty("filter") ? string(args, "filter").toLowerCase() : juce::String();
        const auto kind = args.hasProperty("kind") ? string(args, "kind") : juce::String();
        if (kind.isNotEmpty() && kind != "native" && kind != "vst3") throw std::runtime_error("Choose native or vst3 kind");
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
        trackIndex(*before.song, trackId);
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
    model::EditCommand command;
    juce::var report;
    int pluginTrack = -1, pluginDevice = -1;
    if (operation == "devices.parameters.set")
    {
        fields(args, {"trackId", "deviceId", "parameters"});
        const int trackId = integer(args, "trackId", 0, 1000000000), deviceId = integer(args, "deviceId", 0, 1000000000);
        const int index = trackIndex(*before.song, trackId);
        const auto& track = before.song->tracks[static_cast<size_t>(index)];
        const auto found = std::find_if(track.devices.begin(), track.devices.end(), [deviceId](const auto& d) { return d.id == deviceId; });
        if (found == track.devices.end()) return failure("target_replaced", "Device no longer exists");
        const int deviceIndex = static_cast<int>(found - track.devices.begin());
        if (found->kind == model::DeviceKind::Rack && descendantAutomation(track, *found))
            return failure("automation_conflict", "Rack descendants have authored automation; whole-rack parameter replacement is unavailable");
        if (found->kind != model::DeviceKind::PluginVst3 && found->kind != model::DeviceKind::Native && found->kind != model::DeviceKind::Rack)
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
            for (const auto& p : found->pluginParams)
                if ((std::to_string(p.parameterId) == write.parameterId || p.name == write.name) && hasAutomation(track, p.automationTargetId))
                    return failure("automation_conflict", "Parameter has authored automation; edit its envelope instead");
            for (const auto& p : found->parameters)
                if (p.path == write.parameterId && hasAutomation(track, p.automationTargetId))
                    return failure("automation_conflict", "Native parameter has authored automation; edit its envelope instead");
            writes.push_back(std::move(write));
        }
        if (! backend_.prepareParameters) return failure("unsupported_operation", "Device parameter preparation is unavailable");
        model::Device captured, changed;
        const auto prepared = backend_.prepareParameters(trackId, deviceId, writes, captured, changed, report);
        if (! prepared.ok) return failure(prepared.code, prepared.message);
        const auto current = backend_.state();
        if (current.documentId != before.documentId || current.revision != before.revision || ! current.busy.empty())
            return failure("stale_revision", "Project changed during plugin preparation");
        if (backend_.captureDevice)
        {
            model::Device stillCurrent;
            const auto checked = backend_.captureDevice(trackId, deviceId, stillCurrent);
            if (! checked.ok || ! sameDeviceState(captured, stillCurrent))
                return failure("stale_revision", "Plugin state changed during preparation; inspect again");
        }
        model::Song staged = *current.song;
        staged.tracks[static_cast<size_t>(index)].devices[static_cast<size_t>(deviceIndex)] = captured;
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
        report.getDynamicObject()->setProperty("previousDeviceId", deviceId);
        report.getDynamicObject()->setProperty("deviceId", newId);
        pluginTrack = trackId; pluginDevice = newId;
    }
    else if (operation == "devices.insert")
    {
        fields(args, {"trackId", "catalogId", "position"});
        if (! backend_.prepareDevice) return failure("unsupported_operation", "Device insertion is unavailable");
        const int trackId = integer(args, "trackId", 0, 1000000000), index = trackIndex(*before.song, trackId);
        const auto track = before.song->tracks[static_cast<size_t>(index)];
        if (track.devices.size() >= 64) return failure("capacity", "Agent insertion is limited to 64 top-level devices per track");
        model::Device device;
        const auto id = string(args, "catalogId", 1024);
        const auto prepared = backend_.prepareDevice(id.toStdString(), device, report);
        if (! prepared.ok) return failure(prepared.code, prepared.message);
        const auto current = backend_.state();
        if (current.documentId != before.documentId || current.revision != before.revision || ! current.busy.empty())
            return failure("stale_revision", "Project changed during device preparation");
        const auto role = model::deviceRole(device);
        if ((role == model::DeviceRole::Instrument || role == model::DeviceRole::MidiEffect) && track.kind != model::TrackKind::Midi)
            return failure("incompatible_track", "Instruments and MIDI effects require a MIDI track");
        if (role == model::DeviceRole::Instrument && model::instrumentIndex(track) >= 0)
            return failure("instrument_conflict", "Track already has an instrument; explicitly remove it or choose an empty MIDI track");
        int position = static_cast<int>(track.devices.size());
        if (role == model::DeviceRole::MidiEffect) position = 0;
        else if (role == model::DeviceRole::Instrument)
        { position = 0; while (position < static_cast<int>(track.devices.size()) && model::deviceRole(track.devices[static_cast<size_t>(position)]) == model::DeviceRole::MidiEffect) ++position; }
        if (args.hasProperty("position")) position = integer(args, "position", 0, static_cast<int>(track.devices.size()));
        auto ordered = track.devices; ordered.insert(ordered.begin() + position, device); int previous = 0;
        for (const auto& d : ordered)
        {
            const auto r = model::deviceRole(d); const int rank = r == model::DeviceRole::MidiEffect ? 0 : r == model::DeviceRole::Instrument ? 1 : 2;
            if (rank < previous) return failure("invalid_device_order", "Use MIDI effects, then instrument, then audio effects");
            previous = rank;
        }
        model::Song staged = *current.song; model::allocateDeviceIds(staged, device);
        device.sourceXml = std::make_shared<const std::string>(io::deviceTemplate(device));
        pluginTrack = trackId; pluginDevice = device.id;
        command = model::insertDevice(index, std::move(device), position);
        const auto apply = command.apply;
        command.apply = [apply, nextId = staged.nextDeviceId, nextPointee = staged.nextPointeeId](model::Song& song) {
            song.nextDeviceId = std::max(song.nextDeviceId, nextId); song.nextPointeeId = std::max(song.nextPointeeId, nextPointee);
            song.pointeeIdsAllocated = true; apply(song);
        };
        if (! report.getDynamicObject()) report = object({});
        report.getDynamicObject()->setProperty("trackId", trackId); report.getDynamicObject()->setProperty("deviceId", pluginDevice);
        report.getDynamicObject()->setProperty("catalogId", id); report.getDynamicObject()->setProperty("position", position);
    }
    else if (operation == "devices.remove")
    {
        fields(args, {"trackId", "deviceId"});
        const int trackId = integer(args, "trackId", 0, 1000000000), id = integer(args, "deviceId", 0, 1000000000);
        const int index = trackIndex(*before.song, trackId);
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
            for (auto& device : song.tracks[static_cast<size_t>(index)].devices) if (device.id == id) device = captured;
            apply(song);
        };
        report = object({{"trackId", trackId}, {"removedDeviceId", id}});
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
        model::Song staged = *before.song;
        for (size_t i = 0; i < staged.tracks.size() && i < before.liveGains.size(); ++i)
            staged.tracks[i].mixer.volume = before.liveGains[i];
        if (before.liveTempo > 0) staged.tempo = before.liveTempo;
        std::vector<model::EditCommand> parts;
        try
        {
            for (const auto& item : *commands)
            {
                // COL-1: every edit is a model::Op, the same intent collab peers exchange.
                const auto type = item["type"].toString();
                if (! item.getDynamicObject()) throw std::runtime_error("Expected an object");
                if (type != "tempo.set" && type != "track.set_gain" && type != "clip.duplicate" && type != "notes.add"
                    && type != "notes.update" && type != "notes.delete" && type != "notes.replace")
                    return failure("unsupported_operation", "Unsupported edit type: " + type.toStdString());
                auto part = model::opToCommand(staged, model::opFromJson(noteKeysForIndices(staged, item)));
                part.apply(staged); parts.push_back(std::move(part));
            }
        }
        catch (const model::OpError& error) { return failure(error.code, error.what()); }
        command = model::compose("Agent edit", std::move(parts));
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
            const auto actual = backend_.parameters ? backend_.parameters(pluginTrack, pluginDevice) : juce::var{};
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
