#pragma once
// COL-2 (ADR 0010): a Song as a set of replicated objects.
//
// Every edit, whatever code made it, becomes the list of objects it changed. Objects are addressed by stable identity
// only (track id, clip UID), so peers never exchange display indices:
//
//   song        song header (tempo, signature, scenes, locators, loop, allocators...)
//   tracks      the track structure: ordered ids of tracks and returns, and the main track id
//   t/<id>      a track header (name, colour, routing, mixer, automation, take lanes, frozen clips...)
//   d/<id>      a track's device chain
//   o/<id>      a track's clip order: Arrangement clip UIDs in list order, and the clip UID of each Session slot
//   c/<uid>     one Arrangement or Session clip
//
// Objects travel in the codec's full form; ObjectIndex keeps a hash of each object's identity form to detect what
// changed.
// This layer is pure model code: no engine, no UI, no network.

#include "core/ModelCodec.h"

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace sonora::collab
{

struct ObjectKey
{
    enum class Kind : uint8_t
    {
        Song = 0,
        Structure = 1,
        Track = 2,
        Devices = 3,
        Order = 4,
        Clip = 5,
    };
    Kind kind = Kind::Song;
    int id = 0; // track id, or clip UID; 0 for song and structure

    static ObjectKey song() { return {Kind::Song, 0}; }
    static ObjectKey structure() { return {Kind::Structure, 0}; }
    static ObjectKey track(int id) { return {Kind::Track, id}; }
    static ObjectKey devices(int id) { return {Kind::Devices, id}; }
    static ObjectKey order(int id) { return {Kind::Order, id}; }
    static ObjectKey clip(int uid) { return {Kind::Clip, uid}; }

    bool operator<(const ObjectKey& o) const { return kind != o.kind ? kind < o.kind : id < o.id; }
    bool operator==(const ObjectKey& o) const { return kind == o.kind && id == o.id; }
    bool operator!=(const ObjectKey& o) const { return ! (*this == o); }
};
std::string toString(const ObjectKey&); // "c/1000042"

// One object's new state: the full encoding, or nullopt when the object no longer exists.
struct ObjectState
{
    ObjectKey key;
    std::optional<std::string> full;
};

// A changed object with the full forms before and after (nullopt: absent).
struct Change
{
    ObjectKey key;
    std::optional<std::string> before, after;
};

struct Structure
{
    std::vector<int> tracks, returns;
    int main = -1;
};
std::string encodeStructure(const Structure&);
Structure decodeStructure(std::string_view);

struct Order
{
    std::vector<int> arrangement; // clip UIDs in list order
    std::vector<int> slots;       // clip UID per Session slot, -1 when empty
};
std::string encodeOrder(const Order&);
Order decodeOrder(std::string_view);

// Every object of `song`, in a deterministic order (song, structure, then per track t, d, o, its clips).
std::vector<ObjectKey> objectKeys(const model::Song&);
// Full form of one object of `song`; nullopt when it does not exist.
std::optional<std::string> encodeObject(const model::Song&, const ObjectKey&);

// What applying a list of objects changed, for engine and UI refresh.
struct ApplyEffects
{
    bool songHeader = false;   // tempo, signature, loop, scenes, locators...
    bool structure = false;    // tracks inserted, removed or reordered: the engine graph must be rebuilt
    std::set<int> headers;     // track ids whose header changed
    std::set<int> devices;     // track ids whose device chain changed
    std::set<int> clips;       // track ids whose clips or clip order changed
    bool empty() const { return ! songHeader && ! structure && headers.empty() && devices.empty() && clips.empty(); }
};

// Applies decoded object states to `song`. Throws model::CodecError (before mutating anything) when an object
// does not decode. Missing references are tolerated: a clip UID no clip object provides is skipped.
ApplyEffects applyObjects(model::Song&, const std::vector<ObjectState>&);

// Checks that applying `objects` to `song` leaves a consistent song: unique track ids, every track with a header,
// devices and order, every ordered clip present exactly once, bounded slot counts, clip kinds
// matching track kinds, finite values and limits. Returns an empty string when valid, else the reason.
std::string validateObjects(const model::Song&, const std::vector<ObjectState>&);

// Live values (mixer levels, device parameter values, bypass, opaque plugin state) of objects that already exist here
// stay as this replica has them: they are synced by the live channel, and a sender's model may hold older values.
// Rewrites track headers and device chains in `states` with the live values of `song` (devices matched by id).
void mergeLiveValues(const model::Song& song, std::vector<ObjectState>& states);

// Every file path a set of objects references (clip samples, sampler zones, device and content directories, Max patches).
std::set<std::string> referencedPaths(const std::vector<ObjectState>&);
std::set<std::string> referencedPaths(const model::Song&);

// Builds a song from the objects of a snapshot (song, structure and every track's objects).
model::Song songFromObjects(const std::vector<ObjectState>&);

// Change-detection cache of one replica: per object, the hash of its identity form and its full form. diff()
// reports the objects whose identity changed since the last update, with full forms before and after.
class ObjectIndex
{
public:
    void reset(const model::Song&);
    std::vector<Change> diff(const model::Song&) const;
    // Records the current state of these keys (after a commit or a remote apply).
    void update(const model::Song&, const std::vector<ObjectKey>&);
    // The full form recorded for a key (nullopt when absent).
    std::optional<std::string> full(const ObjectKey&) const;
    size_t size() const { return entries_.size(); }

private:
    struct Entry
    {
        uint64_t identity = 0; // codec hash of the identity form
        std::string full;
    };
    std::map<ObjectKey, Entry> entries_;
};

} // namespace sonora::collab
