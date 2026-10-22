#include "core/OpJson.h"

#include <cmath>
#include <limits>

namespace sonora::model
{
namespace
{
[[noreturn]] void invalid(const std::string& message) { throw OpError("invalid_request", message); }

struct Object
{
    juce::DynamicObject::Ptr data = new juce::DynamicObject();
    Object& set(const char* key, juce::var value)
    {
        data->setProperty(key, std::move(value));
        return *this;
    }
    juce::var var() const { return juce::var(data.get()); }
};

void allow(const juce::var& value, std::initializer_list<const char*> names)
{
    const auto* data = value.getDynamicObject();
    if (! data) invalid("Expected an object");
    for (const auto& property : data->getProperties())
    {
        bool known = false;
        for (const auto* name : names) known |= property.name == juce::Identifier(name);
        if (! known) invalid("Unknown field: " + property.name.toString().toStdString());
    }
}

double number(const juce::var& value, const char* name)
{
    const auto field = value[name];
    if (! (field.isInt() || field.isInt64() || field.isDouble())) invalid(std::string("Missing numeric ") + name);
    const auto result = static_cast<double>(field);
    if (! std::isfinite(result)) invalid(std::string("Expected a finite number: ") + name);
    return result;
}

int64_t integer(const juce::var& value, const char* name, int64_t lo, int64_t hi)
{
    const double d = number(value, name);
    if (std::floor(d) != d || d < static_cast<double>(lo) || d > static_cast<double>(hi))
        invalid(std::string("Expected an integer in range: ") + name);
    return static_cast<int64_t>(d);
}

int id(const juce::var& value, const char* name)
{
    return static_cast<int>(integer(value, name, -1, std::numeric_limits<int>::max()));
}

bool boolean(const juce::var& value, const char* name)
{
    const auto field = value[name];
    if (! field.isBool()) invalid(std::string(name) + " must be boolean");
    return static_cast<bool>(field);
}

template<class T> std::optional<T> maybe(const juce::var& value, const char* name, T (*read)(const juce::var&, const char*))
{
    if (! value.hasProperty(name)) return std::nullopt;
    return read(value, name);
}

float realF(const juce::var& value, const char* name) { return static_cast<float>(number(value, name)); }
double real(const juce::var& value, const char* name) { return number(value, name); }
int pitch(const juce::var& value, const char* name) { return static_cast<int>(integer(value, name, -1000000, 1000000)); }

const juce::Array<juce::var>& array(const juce::var& value, const char* name)
{
    const auto* items = value[name].getArray();
    if (! items) invalid(std::string("Expected an array: ") + name);
    if (static_cast<size_t>(items->size()) > maxNotesPerClip) invalid(std::string("Array too long: ") + name);
    return *items;
}

void write(Object& out, const ClipRef& ref)
{
    out.set("trackId", ref.trackId).set("clipId", ref.clipUid);
    if (ref.scene >= 0) out.set("scene", ref.scene);
    if (ref.laneId >= 0) out.set("laneId", ref.laneId);
}

ClipRef readClip(const juce::var& value)
{
    ClipRef ref;
    ref.trackId = id(value, "trackId");
    ref.clipUid = id(value, "clipId");
    if (value.hasProperty("scene")) ref.scene = id(value, "scene");
    if (value.hasProperty("laneId")) ref.laneId = id(value, "laneId");
    return ref;
}

juce::var writeNote(const MidiNote& note)
{
    return Object()
        .set("pitch", note.pitch).set("startBeat", note.start).set("durationBeats", note.duration)
        .set("velocity", static_cast<double>(note.velocity)).set("releaseVelocity", static_cast<double>(note.releaseVelocity))
        .set("probability", static_cast<double>(note.probability)).set("enabled", note.enabled)
        .set("velocityDeviation", static_cast<double>(note.velocityDeviation))
        .var();
}

MidiNote readNote(const juce::var& value)
{
    allow(value, {"pitch", "startBeat", "durationBeats", "velocity", "releaseVelocity", "probability", "enabled", "velocityDeviation"});
    MidiNote note;
    note.pitch = pitch(value, "pitch");
    note.start = number(value, "startBeat");
    note.duration = number(value, "durationBeats");
    note.velocity = realF(value, "velocity");
    if (value.hasProperty("releaseVelocity")) note.releaseVelocity = realF(value, "releaseVelocity");
    if (value.hasProperty("probability")) note.probability = realF(value, "probability");
    if (value.hasProperty("enabled")) note.enabled = boolean(value, "enabled");
    if (value.hasProperty("velocityDeviation")) note.velocityDeviation = realF(value, "velocityDeviation");
    return note;
}

juce::var writeNotes(const std::vector<MidiNote>& notes)
{
    juce::Array<juce::var> out;
    out.ensureStorageAllocated(static_cast<int>(notes.size()));
    for (const auto& note : notes) out.add(writeNote(note));
    return out;
}

std::vector<MidiNote> readNotes(const juce::var& value)
{
    std::vector<MidiNote> notes;
    for (const auto& item : array(value, "notes")) notes.push_back(readNote(item));
    return notes;
}

uint32_t noteKey(const juce::var& value, const char* name)
{
    return static_cast<uint32_t>(integer(value, name, 1, std::numeric_limits<uint32_t>::max()));
}

struct Encoder
{
    Object& out;
    void operator()(const ops::SetTempo& op) { out.set("bpm", op.bpm); }
    void operator()(const ops::SetTrackGain& op) { out.set("trackId", op.trackId).set("gainDb", op.gainDb); }
    void operator()(const ops::RenameTrack& op) { out.set("trackId", op.trackId).set("name", juce::String(op.name)); }
    void operator()(const ops::SetTrackColor& op)
    {
        juce::Array<juce::var> ids;
        for (const auto trackId : op.trackIds) ids.add(trackId);
        out.set("trackIds", ids).set("color", op.color);
    }
    void operator()(const ops::SetLoop& op) { out.set("on", op.on).set("startBeat", op.start).set("lengthBeats", op.length); }
    void operator()(const ops::MoveClip& op)
    {
        out.set("trackId", op.trackId).set("clipId", op.clipUid).set("startBeat", op.startBeat);
        if (op.toTrackId >= 0) out.set("toTrackId", op.toTrackId);
    }
    void operator()(const ops::DuplicateClip& op)
    {
        out.set("trackId", op.trackId).set("clipId", op.clipUid).set("destinationBeat", op.destinationBeat);
    }
    void operator()(const ops::DeleteClip& op) { write(out, op.clip); }
    void operator()(const ops::AddNotes& op) { write(out, op.clip); out.set("notes", writeNotes(op.notes)); }
    void operator()(const ops::ReplaceNotes& op) { write(out, op.clip); out.set("notes", writeNotes(op.notes)); }
    void operator()(const ops::UpdateNotes& op)
    {
        write(out, op.clip);
        juce::Array<juce::var> updates;
        for (const auto& update : op.updates)
        {
            Object item;
            item.set("noteKey", static_cast<juce::int64>(update.key));
            if (update.pitch) item.set("pitch", *update.pitch);
            if (update.start) item.set("startBeat", *update.start);
            if (update.duration) item.set("durationBeats", *update.duration);
            if (update.velocity) item.set("velocity", static_cast<double>(*update.velocity));
            if (update.releaseVelocity) item.set("releaseVelocity", static_cast<double>(*update.releaseVelocity));
            if (update.probability) item.set("probability", static_cast<double>(*update.probability));
            if (update.enabled) item.set("enabled", *update.enabled);
            if (update.velocityDeviation) item.set("velocityDeviation", static_cast<double>(*update.velocityDeviation));
            updates.add(item.var());
        }
        out.set("updates", updates);
    }
    void operator()(const ops::DeleteNotes& op)
    {
        write(out, op.clip);
        juce::Array<juce::var> keys;
        for (const auto key : op.keys) keys.add(static_cast<juce::int64>(key));
        out.set("noteKeys", keys);
    }
};

Op decode(const juce::var& value)
{
    const auto typeField = value["type"];
    if (! typeField.isString()) invalid("Missing op type");
    const auto type = typeField.toString();
    if (type == "tempo.set")
    {
        allow(value, {"type", "bpm"});
        return {ops::SetTempo{number(value, "bpm")}};
    }
    if (type == "track.set_gain")
    {
        allow(value, {"type", "trackId", "gainDb"});
        return {ops::SetTrackGain{id(value, "trackId"), number(value, "gainDb")}};
    }
    if (type == "track.rename")
    {
        allow(value, {"type", "trackId", "name"});
        if (! value["name"].isString()) invalid("Expected a string: name");
        return {ops::RenameTrack{id(value, "trackId"), value["name"].toString().toStdString()}};
    }
    if (type == "track.set_color")
    {
        allow(value, {"type", "trackIds", "color"});
        ops::SetTrackColor op;
        const auto* ids = value["trackIds"].getArray();
        if (! ids || ids->size() > 1024) invalid("Expected an array of at most 1024 trackIds");
        for (const auto& item : *ids)
        {
            if (! (item.isInt() || item.isInt64() || item.isDouble()) || std::floor(static_cast<double>(item)) != static_cast<double>(item))
                invalid("Expected integer trackIds");
            op.trackIds.push_back(static_cast<int>(item));
        }
        op.color = static_cast<int>(integer(value, "color", -1, 1000));
        return {op};
    }
    if (type == "loop.set")
    {
        allow(value, {"type", "on", "startBeat", "lengthBeats"});
        return {ops::SetLoop{boolean(value, "on"), number(value, "startBeat"), number(value, "lengthBeats")}};
    }
    if (type == "clip.move")
    {
        allow(value, {"type", "trackId", "clipId", "toTrackId", "startBeat"});
        return {ops::MoveClip{id(value, "trackId"), id(value, "clipId"),
                              value.hasProperty("toTrackId") ? id(value, "toTrackId") : -1, number(value, "startBeat")}};
    }
    if (type == "clip.duplicate")
    {
        allow(value, {"type", "trackId", "clipId", "destinationBeat"});
        return {ops::DuplicateClip{id(value, "trackId"), id(value, "clipId"), number(value, "destinationBeat")}};
    }
    if (type == "clip.delete")
    {
        allow(value, {"type", "trackId", "clipId", "scene", "laneId"});
        return {ops::DeleteClip{readClip(value)}};
    }
    if (type == "notes.add" || type == "notes.replace")
    {
        allow(value, {"type", "trackId", "clipId", "scene", "laneId", "notes"});
        if (type == "notes.add") return {ops::AddNotes{readClip(value), readNotes(value)}};
        return {ops::ReplaceNotes{readClip(value), readNotes(value)}};
    }
    if (type == "notes.update")
    {
        allow(value, {"type", "trackId", "clipId", "scene", "laneId", "updates"});
        ops::UpdateNotes op{readClip(value), {}};
        for (const auto& item : array(value, "updates"))
        {
            allow(item, {"noteKey", "pitch", "startBeat", "durationBeats", "velocity", "releaseVelocity", "probability", "enabled", "velocityDeviation"});
            NoteUpdate update;
            update.key = noteKey(item, "noteKey");
            update.pitch = maybe<int>(item, "pitch", pitch);
            update.start = maybe<double>(item, "startBeat", real);
            update.duration = maybe<double>(item, "durationBeats", real);
            update.velocity = maybe<float>(item, "velocity", realF);
            update.releaseVelocity = maybe<float>(item, "releaseVelocity", realF);
            update.probability = maybe<float>(item, "probability", realF);
            update.velocityDeviation = maybe<float>(item, "velocityDeviation", realF);
            update.enabled = maybe<bool>(item, "enabled", boolean);
            op.updates.push_back(update);
        }
        return {op};
    }
    if (type == "notes.delete")
    {
        allow(value, {"type", "trackId", "clipId", "scene", "laneId", "noteKeys"});
        ops::DeleteNotes op{readClip(value), {}};
        for (const auto& item : array(value, "noteKeys")) op.keys.push_back(noteKey(Object().set("k", item).var(), "k"));
        return {op};
    }
    throw OpError("unsupported_operation", "Unsupported edit type: " + type.toStdString());
}
} // namespace

juce::var opToJson(const Op& op)
{
    Object out;
    out.set("type", juce::String(opType(op)));
    std::visit(Encoder{out}, op.payload);
    return out.var();
}

Op opFromJson(const juce::var& value)
{
    if (! value.getDynamicObject()) invalid("Expected an op object");
    return decode(value);
}

juce::var committedOpToJson(const Op& op, const OpIdentities& identities)
{
    juce::Array<juce::var> uids;
    for (const auto uid : identities.clipUids) uids.add(uid);
    return Object().set("v", opFormatVersion).set("op", opToJson(op)).set("clipUids", uids).var();
}

std::pair<Op, OpIdentities> committedOpFromJson(const juce::var& value)
{
    allow(value, {"v", "op", "clipUids"});
    if (integer(value, "v", 0, 1000000) != opFormatVersion) throw OpError("unsupported_version", "Unsupported op format version");
    OpIdentities identities;
    const auto* uids = value["clipUids"].getArray();
    if (! uids || uids->size() > 65536) invalid("Expected an array: clipUids");
    for (const auto& uid : *uids) identities.clipUids.push_back(id(Object().set("u", uid).var(), "u"));
    return {opFromJson(value["op"]), std::move(identities)};
}

juce::String opMessageToString(const juce::var& message)
{
    return juce::JSON::toString(message, juce::JSON::FormatOptions{}.withSpacing(juce::JSON::Spacing::none).withMaxDecimalPlaces(20));
}

} // namespace sonora::model
