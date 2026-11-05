#pragma once
// COL-3: messages between collab peers (docs/collab/PEER_PROTOCOL.md).
//
// Peers form a star around the host over WebRTC data channels (or, in tests, any ordered byte pipe). Every message is
// one binary frame: a type byte, then the fields with core/ByteCodec. Decoding is strict and bounded: unknown types,
// truncation, trailing bytes and oversize fields throw model::CodecError, and the receiver drops the peer.

#include "collab/Session.h"

#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <variant>

namespace sonora::collab
{

inline constexpr uint32_t protocolVersion = 3; // 2 (COL-3c): deflated snapshot, commit and intent bodies;
                                                // 3 (PLG-15): model codec 4 (VST2 preset fields)
inline constexpr size_t maxNameLength = 64;

// COL-9: optional features a peer announces in its hello (HelloMsg::features). The wire protocol stays 3, so a peer
// without them (0.1.9 and older) still joins; messages for a feature only go to peers that announced it, because
// older decoders drop the link on an unknown message type.
enum Feature : uint32_t
{
    FeatureChat = 1u << 0,  // policy + chat messages
    FeatureVoice = 1u << 1, // COL-7: media-state messages and the SRTP audio track
    FeatureVideo = 1u << 2, // COL-8: the H.264 video track (macOS: AVFoundation + VideoToolbox)
};
#if defined(SONORA_HAVE_OPUS) && SONORA_HAVE_OPUS
inline constexpr uint32_t voiceFeature = FeatureVoice;
#else
inline constexpr uint32_t voiceFeature = 0;
#endif
#if defined(__APPLE__)
inline constexpr uint32_t videoFeature = FeatureVideo;
#else
inline constexpr uint32_t videoFeature = 0; // TODO(COM-7): camera capture and H.264 on Windows/Linux
#endif
inline constexpr uint32_t supportedFeatures = FeatureChat | voiceFeature | videoFeature;

// Chat limits (PEER_PROTOCOL.md "Chat").
inline constexpr size_t maxChatChars = 1000;  // Unicode code points per message
inline constexpr size_t maxChatBytes = 4000;  // UTF-8 bytes per message
// Valid UTF-8 (bad bytes dropped), no control characters except line breaks (tabs become spaces, at most two blank
// lines in a row), trimmed, at most maxChatChars code points. "" when nothing is left.
std::string cleanChatText(std::string_view);

enum class Role : uint8_t
{
    Host = 0,
    Editor = 1,
    Listener = 2,
};
const char* toString(Role);

struct Participant
{
    AuthorId id = hostAuthor;
    std::string name;
    Role role = Role::Editor;
    uint8_t color = 0; // index into the participant palette
    bool connected = true;
};

// A file the song references, keyed as the host's model stores it, with its content hash.
struct AssetEntry
{
    std::string absolutePath, relativePath, sourceDirectory; // the FileRef fields that identify it
    std::string hash;                                       // SHA-256, lowercase hex
    uint64_t size = 0;
    std::string extension;                                  // ".wav": decoders pick formats by extension
};

struct TransportState
{
    bool playing = false;
    double beat = 0.0;
};

struct HelloMsg
{
    uint32_t protocol = protocolVersion;
    std::string appVersion;
    std::string name;
    AuthorId rejoinAs = 0;    // non-zero: a reconnect as this participant
    std::string rejoinToken;  // proves the reconnecting guest is the same one
    uint64_t lastSeq = 0;     // reconnect: commits after this are replayed when the host still has them
    uint32_t features = 0;    // Feature bits; on the wire as a suffix of appVersion (PEER_PROTOCOL.md "Features")
};

struct WelcomeMsg
{
    AuthorId you = 0;
    Role role = Role::Editor;
    std::string sessionName;
    std::vector<Participant> participants;
    int clipUidBase = 0;      // this guest allocates clip UIDs from here, so peers never collide
    std::string rejoinToken;
    bool snapshotFollows = true; // false: commits after the guest's lastSeq follow instead
};

struct SnapshotMsg
{
    uint64_t seq = 0;
    std::vector<ObjectState> objects;
    std::map<ObjectKey, uint64_t> revisions;
    std::string document;     // codec::encodeDocument
    std::string sourceName;   // the set's file name
    std::string sourceFile;   // the host's saved .als, so a guest can save a copy with untouched elements intact
    std::vector<AssetEntry> assets;
    TransportState transport;
};

struct ParticipantsMsg
{
    std::vector<Participant> participants;
};

// Where someone is looking and pointing, in musical coordinates (COL-5).
struct PresenceMsg
{
    enum class View : uint8_t
    {
        Arrangement = 0,
        Session = 1,
        Clip = 2,
    };
    AuthorId author = 0;
    View view = View::Arrangement;
    int trackId = -1;         // the track under the pointer
    double beat = -1.0;       // pointer position, < 0 outside the timeline
    int pitch = -1;           // piano roll only
    int clipUid = -1;         // the clip open in the clip view
    int selectedTrackFrom = -1, selectedTrackTo = -1; // selected track ids (range in display order)
    double selectionStart = -1.0, selectionEnd = -1.0;
    int selectedClipUid = -1;
};

// Live control values (mixer and device parameters, COL-6): latest wins.
struct LiveValue
{
    enum Target : int
    {
        Volume = -1,
        Pan = -2,
        Active = -3,
        Solo = -4,
        SendBase = -100, // send n is SendBase - n
        Bypass = -5,     // with a device id
    };
    int trackId = -1;
    int deviceId = -1;   // -1: the track's mixer
    int target = 0;      // a parameter index of the device, or one of Target
    float value = 0.0f;
};

struct LiveMsg
{
    AuthorId author = 0;
    std::vector<LiveValue> values;
};

// A hosted plugin's opaque state after someone changed it in the plugin's own editor.
struct PluginStateMsg
{
    AuthorId author = 0;
    int trackId = -1, deviceId = -1;
    std::vector<uint8_t> state, controllerState;
};

struct TransportMsg
{
    enum class Action : uint8_t
    {
        Play = 0,
        Stop = 1,
        Seek = 2,
    };
    Action action = Action::Play;
    double beat = 0.0;
    int64_t hostTimeMs = 0; // when the host's clock reaches this, start (Play) or jump (Seek)
    AuthorId author = 0;
};

struct ClockMsg
{
    bool pong = false;
    int64_t sentMs = 0;  // the guest's clock when it sent the ping
    int64_t hostMs = 0;  // the host's clock when it answered
};

struct AssetRequestMsg
{
    std::string hash;
    uint64_t offset = 0;
};

struct AssetChunkMsg
{
    std::string hash;
    uint64_t offset = 0, total = 0;
    std::string bytes;
    bool missing = false; // the host can't read this file
};

struct AssetAddMsg
{
    std::vector<AssetEntry> assets;
};

// A guest drops a file from its disk onto a track: it uploads the file, the host stores it in the project and
// performs the drop (COL-4).
struct UploadMsg
{
    uint64_t uploadId = 0;
    std::string name;    // file name only
    uint64_t offset = 0, total = 0;
    std::string bytes;
};

struct DropRequestMsg
{
    uint64_t uploadId = 0;
    int trackId = -1;    // -1: a new track
    double beat = 0.0;
    int scene = -1;      // >= 0: a Session slot
};

struct UndoMsg
{
    bool redo = false;
};

struct RoleMsg
{
    AuthorId who = 0;
    Role role = Role::Editor;
};

struct NoticeMsg
{
    std::string text;
};

struct ByeMsg
{
    std::string reason;
};

// What the host allows in this session (COL-9). Defaults are the host's settings.
struct SessionPolicy
{
    bool chat = true, voice = true, video = true;
    bool operator==(const SessionPolicy&) const = default;
};

// Extension messages (COL-9) start with a version byte. A later version only appends fields: a reader keeps the
// fields it knows and skips the rest. They go only to peers whose hello announced the feature.
inline constexpr uint8_t policyMessageVersion = 1;
inline constexpr uint8_t chatMessageVersion = 1;
inline constexpr uint8_t mediaStateMessageVersion = 1;

// Host -> guests with FeatureChat: the session's policy, the host's features and everyone's.
struct PolicyMsg
{
    uint8_t version = policyMessageVersion;
    uint32_t hostFeatures = 0;
    SessionPolicy policy;
    std::vector<std::pair<AuthorId, uint32_t>> features; // participant, Feature bits
};

// A chat line. Guest -> host: author and id are ignored, clientId is the sender's own counter. Host -> guests: the host
// stamps author, id (its order), time and the sender's clientId (so the sender knows it arrived).
struct ChatMsg
{
    uint8_t version = chatMessageVersion;
    AuthorId author = 0;
    uint64_t id = 0;
    uint64_t clientId = 0;
    int64_t timeMs = 0;     // host clock
    bool history = false;   // sent to a joining guest: earlier conversation, not new
    std::string text;
};

// Someone's own mic and camera switches (COL-7/8). Any -> host -> guests with voice; the host stamps the author and
// sends everyone's to a joining guest.
struct MediaStateMsg
{
    uint8_t version = mediaStateMessageVersion;
    AuthorId author = 0;
    bool micOn = false;
    bool cameraOn = false;
};

using Message = std::variant<HelloMsg, WelcomeMsg, SnapshotMsg, CommitMsg, IntentMsg, RejectMsg, UndoMsg, ParticipantsMsg,
                             PresenceMsg, LiveMsg, PluginStateMsg, TransportMsg, ClockMsg, AssetRequestMsg, AssetChunkMsg,
                             AssetAddMsg, UploadMsg, DropRequestMsg, RoleMsg, NoticeMsg, ByeMsg, PolicyMsg, ChatMsg, MediaStateMsg>;

// COL-3c: chunks of big bodies (snapshot, commits, intents) that already crossed a link travel as 8-byte references,
// so renaming a track whose header carries megabytes of source XML sends only the chunk that changed. One dictionary
// per link and side. Both ends decide the same way because each direction sees the same chunks in the same order (Ops
// is reliable and ordered); each direction keeps at most `maxBytes` of chunks. Dictionaries own their chunks; the pool
// lets the links of a session share the bytes, which go when the last link holding them closes.
using WireChunk = std::shared_ptr<const std::string>;

struct WirePool
{
    std::unordered_map<uint64_t, std::weak_ptr<const std::string>> chunks;
};

struct WireDictionary
{
    explicit WireDictionary(std::shared_ptr<WirePool> p = std::make_shared<WirePool>()) : pool(std::move(p)) {}
    static constexpr uint64_t maxBytes = 256u << 20;
    std::shared_ptr<WirePool> pool;
    std::unordered_map<uint64_t, WireChunk> sent, received; // chunks this side sent / received on this link
    uint64_t sentBytes = 0, receivedBytes = 0;
    uint64_t referencedBytes = 0;                           // body bytes that travelled as references (stats)
};

// Without a dictionary big bodies are only deflated. Encoding with one updates it; so does decoding.
std::string encodeMessage(const Message&, WireDictionary* = nullptr);
Message decodeMessage(std::string_view, WireDictionary* = nullptr); // throws model::CodecError
// Messages whose encoding depends on the link's dictionary (they can't be encoded once for everyone).
bool usesDictionary(const Message&);
const char* messageName(const Message&);

} // namespace sonora::collab
