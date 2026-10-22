#include "collab/Objects.h"

#include "core/Op.h"

#include <algorithm>
#include <functional>
#include <unordered_map>
#include <unordered_set>

namespace sonora::collab
{
namespace codec = model::codec;
using model::Clip;
using model::Song;
using model::Track;

namespace
{
constexpr size_t maxTracks = 4096;
constexpr size_t maxClipsPerTrack = 65536;
constexpr int maxScenes = 100000;

std::vector<int> readIds(model::ByteReader& r)
{
    const size_t n = r.count(1);
    std::vector<int> ids(n);
    for (auto& id : ids) id = r.i32();
    return ids;
}

void writeIds(model::ByteWriter& w, const std::vector<int>& ids)
{
    w.varint(ids.size());
    for (int id : ids) w.i32(id);
}

// Calls `fn(track)` for tracks, returns, then main.
template <class S, class F> void forEachTrack(S& song, F&& fn)
{
    for (auto& t : song.tracks) fn(t);
    for (auto& t : song.returns) fn(t);
    fn(song.main);
}

Order orderOf(const Track& t)
{
    Order o;
    o.arrangement.reserve(t.arrangementClips.size());
    for (const auto& c : t.arrangementClips) o.arrangement.push_back(c.uid);
    o.slots.reserve(t.slots.size());
    for (const auto& s : t.slots) o.slots.push_back(s.clip ? s.clip->uid : -1);
    return o;
}

Structure structureOf(const Song& song)
{
    Structure s;
    for (const auto& t : song.tracks) s.tracks.push_back(t.id);
    for (const auto& t : song.returns) s.returns.push_back(t.id);
    s.main = song.main.id;
    return s;
}

// Hashes of encodeStructure(structureOf(song)) and encodeOrder(orderOf(t)), without building either.
uint64_t hashStructure(const Song& song)
{
    model::HashWriter w;
    w.varint(codec::formatVersion);
    w.varint(song.tracks.size());
    for (const auto& t : song.tracks) w.i32(t.id);
    w.varint(song.returns.size());
    for (const auto& t : song.returns) w.i32(t.id);
    w.i32(song.main.id);
    return w.value();
}

uint64_t hashOrder(const Track& t)
{
    model::HashWriter w;
    w.varint(codec::formatVersion);
    w.varint(t.arrangementClips.size());
    for (const auto& c : t.arrangementClips) w.i32(c.uid);
    w.varint(t.slots.size());
    for (const auto& s : t.slots) w.i32(s.clip ? s.clip->uid : -1);
    return w.value();
}

// Every object of the song as fn(key, full, hash): `full()` builds the full form, `hash()` hashes the identity form
// (COL-2b: change detection compares hashes, so a scan builds full forms only for the objects that changed).
template <class F> void forEachObject(const Song& song, F&& fn)
{
    fn(ObjectKey::song(), [&] { return codec::encodeSongHeader(song, codec::Form::Full); },
       [&] { return codec::hashSongHeader(song); });
    fn(ObjectKey::structure(), [&] { return encodeStructure(structureOf(song)); }, [&] { return hashStructure(song); });
    forEachTrack(song, [&](const Track& t) {
        fn(ObjectKey::track(t.id), [&] { return codec::encodeTrackHeader(t, codec::Form::Full); },
           [&] { return codec::hashTrackHeader(t); });
        fn(ObjectKey::devices(t.id), [&] { return codec::encodeDevices(t.devices, codec::Form::Full); },
           [&] { return codec::hashDevices(t.devices); });
        fn(ObjectKey::order(t.id), [&] { return encodeOrder(orderOf(t)); }, [&] { return hashOrder(t); });
        const auto clip = [&](const Clip& c) {
            fn(ObjectKey::clip(c.uid), [&] { return codec::encodeClip(c, codec::Form::Full); },
               [&] { return codec::hashClip(c); });
        };
        for (const auto& c : t.arrangementClips) clip(c);
        for (const auto& s : t.slots)
            if (s.clip) clip(*s.clip);
    });
}

struct Decoded
{
    std::optional<std::optional<std::string>> songHeader; // set: patched; inner nullopt never valid
    std::optional<Structure> structure;
    std::map<int, std::optional<Track>> headers;                 // nullopt: track removed
    std::map<int, std::optional<std::vector<model::Device>>> devices;
    std::map<int, std::optional<Order>> orders;
    std::map<int, std::optional<Clip>> clips;                    // nullopt: clip removed
};

Decoded decodeAll(const std::vector<ObjectState>& objects)
{
    Decoded d;
    for (const auto& o : objects)
    {
        const int id = o.key.id;
        switch (o.key.kind)
        {
            case ObjectKey::Kind::Song:
            {
                if (! o.full) throw model::CodecError("the song header cannot be removed");
                Song probe;
                codec::decodeSongHeader(*o.full, probe);
                d.songHeader = o.full;
                break;
            }
            case ObjectKey::Kind::Structure:
                if (! o.full) throw model::CodecError("the track structure cannot be removed");
                d.structure = decodeStructure(*o.full);
                break;
            case ObjectKey::Kind::Track:
                if (o.full)
                {
                    auto t = codec::decodeTrackHeader(*o.full);
                    if (t.id != id) throw model::CodecError("track header id does not match its key");
                    d.headers[id] = std::move(t);
                }
                else d.headers[id] = std::nullopt;
                break;
            case ObjectKey::Kind::Devices:
                if (o.full) d.devices[id] = codec::decodeDevices(*o.full);
                else d.devices[id] = std::nullopt;
                break;
            case ObjectKey::Kind::Order:
                if (o.full) d.orders[id] = decodeOrder(*o.full);
                else d.orders[id] = std::nullopt;
                break;
            case ObjectKey::Kind::Clip:
                if (o.full)
                {
                    auto c = codec::decodeClip(*o.full);
                    if (c.uid != id) throw model::CodecError("clip uid does not match its key");
                    d.clips[id] = std::move(c);
                }
                else d.clips[id] = std::nullopt;
                break;
            default:
                throw model::CodecError("unknown object kind");
        }
    }
    return d;
}

void collectPaths(const model::FileRef& f, std::set<std::string>& out)
{
    for (const auto* s : {&f.absolutePath, &f.relativePath, &f.sourceDirectory})
        if (! s->empty()) out.insert(*s);
}

void collectPaths(const Clip& c, std::set<std::string>& out) { collectPaths(c.sample, out); }

void collectPaths(const model::Device& d, std::set<std::string>& out)
{
    for (const auto* s : {&d.sourceDirectory, &d.maxPatchPath})
        if (! s->empty()) out.insert(*s);
    for (const auto& part : d.sampleParts) collectPaths(part.sample, out);
    if (d.nativeContent && ! d.nativeContent->sourceDirectory.empty()) out.insert(d.nativeContent->sourceDirectory);
    for (const auto& chain : d.chains)
        for (const auto& child : chain.devices) collectPaths(child, out);
}

void collectHeaderPaths(const Track& t, std::set<std::string>& out)
{
    for (const auto& lane : t.takeLanes)
        for (const auto& c : lane.clips) collectPaths(c, out);
    for (const auto& c : t.frozenArrangement) collectPaths(c, out);
    for (const auto& s : t.frozenSlots)
        if (s.clip) collectPaths(*s.clip, out);
}

bool clipFitsTrack(const Clip& c, model::TrackKind kind)
{
    if (kind == model::TrackKind::Midi) return c.kind == Clip::Kind::Midi;
    if (kind == model::TrackKind::Audio) return c.kind == Clip::Kind::Audio;
    return false;
}
} // namespace

std::string toString(const ObjectKey& k)
{
    switch (k.kind)
    {
        case ObjectKey::Kind::Song: return "song";
        case ObjectKey::Kind::Structure: return "tracks";
        case ObjectKey::Kind::Track: return "t/" + std::to_string(k.id);
        case ObjectKey::Kind::Devices: return "d/" + std::to_string(k.id);
        case ObjectKey::Kind::Order: return "o/" + std::to_string(k.id);
        case ObjectKey::Kind::Clip: return "c/" + std::to_string(k.id);
    }
    return "?";
}

std::string encodeStructure(const Structure& s)
{
    model::ByteWriter w;
    w.varint(codec::formatVersion);
    writeIds(w, s.tracks);
    writeIds(w, s.returns);
    w.i32(s.main);
    return w.take();
}

Structure decodeStructure(std::string_view bytes)
{
    model::ByteReader r(bytes);
    if (r.varint() != codec::formatVersion) throw model::CodecError("unsupported structure version");
    Structure s;
    s.tracks = readIds(r);
    s.returns = readIds(r);
    s.main = r.i32();
    r.expectEnd();
    return s;
}

std::string encodeOrder(const Order& o)
{
    model::ByteWriter w;
    w.varint(codec::formatVersion);
    writeIds(w, o.arrangement);
    writeIds(w, o.slots);
    return w.take();
}

Order decodeOrder(std::string_view bytes)
{
    model::ByteReader r(bytes);
    if (r.varint() != codec::formatVersion) throw model::CodecError("unsupported order version");
    Order o;
    o.arrangement = readIds(r);
    o.slots = readIds(r);
    r.expectEnd();
    return o;
}

std::vector<ObjectKey> objectKeys(const Song& song)
{
    std::vector<ObjectKey> keys;
    forEachObject(song, [&](const ObjectKey& k, auto&&, auto&&) { keys.push_back(k); });
    return keys;
}

std::optional<std::string> encodeObject(const Song& song, const ObjectKey& key)
{
    std::optional<std::string> found;
    // Track objects are found directly; clips need a scan.
    forEachObject(song, [&](const ObjectKey& k, auto&& full, auto&&) {
        if (! found && k == key) found = full();
    });
    return found;
}

ApplyEffects applyObjects(Song& song, const std::vector<ObjectState>& objects)
{
    auto d = decodeAll(objects); // throws before anything changes
    ApplyEffects fx;
    if (d.songHeader)
    {
        codec::decodeSongHeader(**d.songHeader, song);
        fx.songHeader = true;
    }

    // Take every track and clip out of the song, keyed by identity.
    const Structure before = structureOf(song);
    std::unordered_map<int, Track> tracks;
    std::unordered_map<int, Order> orders;
    std::unordered_map<int, Clip> pool;
    auto takeOut = [&](Track& t) {
        orders[t.id] = orderOf(t);
        for (auto& c : t.arrangementClips) pool[c.uid] = std::move(c);
        for (auto& s : t.slots)
            if (s.clip) { const int uid = s.clip->uid; pool[uid] = std::move(*s.clip); s.clip.reset(); }
        t.arrangementClips.clear();
        const int id = t.id;
        tracks[id] = std::move(t);
    };
    for (auto& t : song.tracks) takeOut(t);
    for (auto& t : song.returns) takeOut(t);
    takeOut(song.main);
    song.tracks.clear();
    song.returns.clear();

    for (auto& [id, header] : d.headers)
    {
        fx.headers.insert(id);
        if (! header) { tracks.erase(id); continue; }
        auto it = tracks.find(id);
        if (it != tracks.end())
        {
            header->devices = std::move(it->second.devices);
            it->second = std::move(*header);
        }
        else tracks[id] = std::move(*header);
    }
    for (auto& [id, devices] : d.devices)
    {
        fx.devices.insert(id);
        if (auto it = tracks.find(id); it != tracks.end())
            it->second.devices = devices ? std::move(*devices) : std::vector<model::Device>{};
    }
    std::unordered_set<int> clipTracks;
    for (auto& [uid, clip] : d.clips)
    {
        if (clip) pool[uid] = std::move(*clip);
        else pool.erase(uid);
    }
    for (auto& [id, order] : d.orders)
    {
        fx.clips.insert(id);
        if (order) orders[id] = std::move(*order);
        else orders.erase(id);
    }
    // Clip content changes refresh the track that holds the clip.
    if (! d.clips.empty())
        for (const auto& [id, order] : orders)
        {
            bool touched = false;
            for (int uid : order.arrangement) touched = touched || d.clips.count(uid) > 0;
            for (int uid : order.slots) touched = touched || (uid >= 0 && d.clips.count(uid) > 0);
            if (touched) fx.clips.insert(id);
        }

    // Put clips back in their order. A UID referenced twice or missing is skipped.
    for (auto& [id, track] : tracks)
    {
        const auto o = orders.find(id);
        if (o == orders.end()) continue;
        track.arrangementClips.reserve(o->second.arrangement.size());
        for (int uid : o->second.arrangement)
            if (auto c = pool.find(uid); c != pool.end())
            {
                track.arrangementClips.push_back(std::move(c->second));
                pool.erase(c);
            }
        for (size_t i = 0; i < track.slots.size() && i < o->second.slots.size(); ++i)
            if (const int uid = o->second.slots[i]; uid >= 0)
                if (auto c = pool.find(uid); c != pool.end())
                {
                    track.slots[i].clip = std::move(c->second);
                    pool.erase(c);
                }
    }

    const Structure& order = d.structure ? *d.structure : before;
    if (d.structure) fx.structure = true;
    std::unordered_set<int> placed;
    auto place = [&](int id, std::vector<Track>& into) {
        auto it = tracks.find(id);
        if (it == tracks.end() || ! placed.insert(id).second) return;
        into.push_back(std::move(it->second));
        tracks.erase(it);
    };
    for (int id : order.tracks) place(id, song.tracks);
    for (int id : order.returns) place(id, song.returns);
    if (auto it = tracks.find(order.main); it != tracks.end())
    {
        song.main = std::move(it->second);
        tracks.erase(it);
    }
    // Tolerance for inconsistent input (validateObjects rejects it on the sequencer): unplaced tracks keep playing.
    for (auto& [id, track] : tracks)
        if (id == before.main && order.main != before.main) continue; // the main track was replaced
        else if (track.kind == model::TrackKind::Return) song.returns.push_back(std::move(track));
        else if (track.kind != model::TrackKind::Main) song.tracks.push_back(std::move(track));
    return fx;
}

std::string validateObjects(const Song& song, const std::vector<ObjectState>& objects)
{
    Decoded d;
    try { d = decodeAll(objects); }
    catch (const model::CodecError& e) { return std::string("malformed object: ") + e.what(); }

    int scenes = static_cast<int>(song.scenes.size());
    if (d.songHeader)
    {
        Song header;
        codec::decodeSongHeader(**d.songHeader, header);
        if (! codec::finiteSongHeader(header)) return "song values must be finite";
        if (header.tempo < 10.0 || header.tempo > 999.0) return "tempo out of range";
        if (header.timeSigNumerator < 1 || header.timeSigNumerator > 99 || header.timeSigDenominator < 1 || header.timeSigDenominator > 16)
            return "time signature out of range";
        if (header.loopLength < 0.0 || header.loopStart < -1.0e7 || header.loopStart > 1.0e7 || header.loopLength > 1.0e7)
            return "loop out of range";
        scenes = static_cast<int>(header.scenes.size());
        if (scenes > maxScenes) return "too many scenes";
    }

    std::unordered_map<int, const Track*> current;
    forEachTrack(song, [&](const Track& t) { current[t.id] = &t; });
    const Structure structure = d.structure ? *d.structure : structureOf(song);
    if (structure.tracks.size() + structure.returns.size() > maxTracks) return "too many tracks";

    auto headerOf = [&](int id) -> const Track* {
        if (auto it = d.headers.find(id); it != d.headers.end()) return it->second ? &*it->second : nullptr;
        auto it = current.find(id);
        return it == current.end() ? nullptr : it->second;
    };
    std::unordered_set<int> listed, groups;
    auto check = [&](int id, std::initializer_list<model::TrackKind> kinds) -> std::string {
        if (! listed.insert(id).second) return "track " + std::to_string(id) + " is listed twice";
        const Track* header = headerOf(id);
        if (! header) return "track " + std::to_string(id) + " has no header";
        if (std::find(kinds.begin(), kinds.end(), header->kind) == kinds.end()) return "track " + std::to_string(id) + " has the wrong kind";
        // Sets may hold fewer slots than scenes (the reader keeps what the file has), never absurdly many.
        if (header->slots.size() > static_cast<size_t>(std::max(scenes, 1)) + 1024)
            return "track " + std::to_string(id) + " has too many Session slots";
        if (header->kind == model::TrackKind::Group) groups.insert(id);
        const bool hasDevices = d.devices.count(id) ? d.devices.at(id).has_value() : current.count(id) > 0;
        if (! hasDevices) return "track " + std::to_string(id) + " has no device list";
        return {};
    };
    for (int id : structure.tracks)
        if (auto e = check(id, {model::TrackKind::Audio, model::TrackKind::Midi, model::TrackKind::Group}); ! e.empty()) return e;
    for (int id : structure.returns)
        if (auto e = check(id, {model::TrackKind::Return}); ! e.empty()) return e;
    if (auto e = check(structure.main, {model::TrackKind::Main}); ! e.empty()) return e;
    for (int id : structure.tracks)
        if (const int parent = headerOf(id)->groupId; parent >= 0 && ! groups.count(parent)) return "track " + std::to_string(id) + " is in a missing group";
    for (const auto& [id, header] : d.headers)
    {
        if (header && ! listed.count(id)) return "track " + std::to_string(id) + " is not in the track list";
        if (header && ! codec::finiteTrackHeader(*header)) return "track values must be finite";
    }
    for (const auto& [id, devices] : d.devices)
    {
        if (devices && ! listed.count(id)) return "devices of a track that is not in the track list";
        if (devices && ! codec::finiteDevices(*devices)) return "device values must be finite";
    }

    // Clips after the change: kind by UID, then every order must reference each clip once, on a fitting track.
    std::unordered_map<int, const Clip*> clips;
    forEachTrack(song, [&](const Track& t) {
        for (const auto& c : t.arrangementClips) clips[c.uid] = &c;
        for (const auto& s : t.slots)
            if (s.clip) clips[s.clip->uid] = &*s.clip;
    });
    for (const auto& [uid, clip] : d.clips)
    {
        if (! clip) { clips.erase(uid); continue; }
        if (! codec::finiteClip(*clip)) return "clip values must be finite";
        if (clip->notes.size() > model::maxNotesPerClip) return "too many notes in a clip";
        clips[uid] = &*clip;
    }
    std::unordered_set<int> used;
    for (int id : listed)
    {
        Order order;
        if (auto it = d.orders.find(id); it != d.orders.end())
        {
            if (! it->second) return "track " + std::to_string(id) + " has no clip order";
            order = *it->second;
        }
        else if (auto c = current.find(id); c != current.end()) order = orderOf(*c->second);
        const Track* header = headerOf(id);
        if (order.arrangement.size() > maxClipsPerTrack) return "too many clips on a track";
        if (order.slots.size() > header->slots.size()) return "more Session clips than slots";
        auto use = [&](int uid) -> std::string {
            auto c = clips.find(uid);
            if (c == clips.end()) return "clip " + std::to_string(uid) + " does not exist";
            if (! used.insert(uid).second) return "clip " + std::to_string(uid) + " is placed twice";
            if (! clipFitsTrack(*c->second, header->kind)) return "clip " + std::to_string(uid) + " does not fit its track";
            return {};
        };
        for (int uid : order.arrangement)
            if (auto e = use(uid); ! e.empty()) return e;
        for (int uid : order.slots)
            if (uid >= 0)
                if (auto e = use(uid); ! e.empty()) return e;
    }
    for (const auto& [uid, clip] : d.clips)
        if (clip && ! used.count(uid)) return "clip " + std::to_string(uid) + " is not placed on any track";
    for (const auto& [id, order] : d.orders)
        if (order && ! listed.count(id)) return "clip order of a track that is not in the track list";
    return {};
}

namespace
{
const model::Device* findDevice(const std::vector<model::Device>& devices, int id)
{
    for (const auto& d : devices)
    {
        if (d.id == id && id >= 0) return &d;
        for (const auto& chain : d.chains)
            if (const auto* found = findDevice(chain.devices, id)) return found;
    }
    return nullptr;
}

void mergeDevice(model::Device& incoming, const std::vector<model::Device>& current)
{
    if (const auto* now = findDevice(current, incoming.id); now && now->type == incoming.type)
    {
        incoming.enabled = now->enabled;
        for (auto& p : incoming.parameters)
            for (const auto& q : now->parameters)
                if (q.path == p.path) { p.value = q.value; break; }
        for (auto& [key, value] : incoming.plain)
            if (auto it = now->plain.find(key); it != now->plain.end()) value = it->second;
        // Value-only entries the sender's model doesn't have yet (captured here on save) keep this replica's values.
        for (const auto& q : now->parameters)
            if (std::none_of(incoming.parameters.begin(), incoming.parameters.end(), [&](const model::Parameter& p) { return p.path == q.path; })
                && q.automationTargetId < 0 && q.midi.channel < 0 && q.macroIndex < 0)
                incoming.parameters.push_back(q);
        if (incoming.plugin && now->plugin && incoming.plugin->name == now->plugin->name && incoming.plugin->vst3Uid == now->plugin->vst3Uid)
        {
            incoming.plugin->state = now->plugin->state;
            incoming.plugin->controllerState = now->plugin->controllerState;
        }
    }
    for (auto& chain : incoming.chains)
        for (auto& child : chain.devices) mergeDevice(child, current);
}
} // namespace

void mergeLiveValues(const Song& song, std::vector<ObjectState>& states)
{
    std::unordered_map<int, const Track*> tracks;
    forEachTrack(song, [&](const Track& t) { tracks[t.id] = &t; });
    for (auto& s : states)
    {
        if (! s.full) continue;
        const auto it = tracks.find(s.key.id);
        if (it == tracks.end()) continue;
        try
        {
            if (s.key.kind == ObjectKey::Kind::Track)
            {
                auto header = codec::decodeTrackHeader(*s.full);
                const auto& now = it->second->mixer;
                header.mixer.volume = now.volume;
                header.mixer.pan = now.pan;
                header.mixer.speakerOn = now.speakerOn;
                header.mixer.solo = now.solo;
                for (size_t i = 0; i < header.mixer.sends.size() && i < now.sends.size(); ++i) header.mixer.sends[i] = now.sends[i];
                s.full = codec::encodeTrackHeader(header, codec::Form::Full);
            }
            else if (s.key.kind == ObjectKey::Kind::Devices)
            {
                auto devices = codec::decodeDevices(*s.full);
                for (auto& d : devices) mergeDevice(d, it->second->devices);
                s.full = codec::encodeDevices(devices, codec::Form::Full);
            }
        }
        catch (const model::CodecError&) {} // left as is: applyObjects rejects it
    }
}

std::set<std::string> referencedPaths(const std::vector<ObjectState>& objects)
{
    std::set<std::string> out;
    Decoded d;
    try { d = decodeAll(objects); }
    catch (const model::CodecError&) { return out; }
    for (const auto& [id, header] : d.headers)
        if (header) collectHeaderPaths(*header, out);
    for (const auto& [id, devices] : d.devices)
        if (devices)
            for (const auto& device : *devices) collectPaths(device, out);
    for (const auto& [uid, clip] : d.clips)
        if (clip) collectPaths(*clip, out);
    return out;
}

std::set<std::string> referencedPaths(const Song& song)
{
    std::set<std::string> out;
    forEachTrack(song, [&](const Track& t) {
        collectHeaderPaths(t, out);
        for (const auto& device : t.devices) collectPaths(device, out);
        for (const auto& c : t.arrangementClips) collectPaths(c, out);
        for (const auto& s : t.slots)
            if (s.clip) collectPaths(*s.clip, out);
    });
    return out;
}

Song songFromObjects(const std::vector<ObjectState>& objects)
{
    Song song;
    applyObjects(song, objects);
    return song;
}

void ObjectIndex::reset(const Song& song)
{
    entries_.clear();
    forEachObject(song, [&](const ObjectKey& k, auto&& full, auto&& hash) { entries_[k] = {hash(), full()}; });
}

std::vector<Change> ObjectIndex::diff(const Song& song) const
{
    std::vector<Change> changes;
    std::set<ObjectKey> seen;
    forEachObject(song, [&](const ObjectKey& k, auto&& full, auto&& hash) {
        seen.insert(k);
        const auto it = entries_.find(k);
        if (it != entries_.end() && it->second.identity == hash()) return;
        Change c;
        c.key = k;
        if (it != entries_.end()) c.before = it->second.full;
        c.after = full();
        changes.push_back(std::move(c));
    });
    for (const auto& [k, entry] : entries_)
        if (! seen.count(k)) changes.push_back({k, entry.full, std::nullopt});
    return changes;
}

void ObjectIndex::update(const Song& song, const std::vector<ObjectKey>& keys)
{
    std::set<ObjectKey> wanted(keys.begin(), keys.end());
    std::set<ObjectKey> found;
    forEachObject(song, [&](const ObjectKey& k, auto&& full, auto&& hash) {
        if (! wanted.count(k)) return;
        found.insert(k);
        entries_[k] = {hash(), full()};
    });
    for (const auto& k : wanted)
        if (! found.count(k)) entries_.erase(k);
}

std::optional<std::string> ObjectIndex::full(const ObjectKey& key) const
{
    if (auto it = entries_.find(key); it != entries_.end()) return it->second.full;
    return std::nullopt;
}

} // namespace sonora::collab
