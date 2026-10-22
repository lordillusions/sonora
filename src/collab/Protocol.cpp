#include "collab/Protocol.h"

namespace sonora::collab
{
namespace
{
using model::ByteReader;
using model::ByteWriter;
using model::CodecError;

constexpr size_t maxText = 4096;
constexpr size_t maxBlob = 256u << 20;

class Enc
{
public:
    static constexpr bool decoding = false;
    void v(bool& x) { w.boolean(x); }
    void v(uint8_t& x) { w.u8(x); }
    void v(int& x) { w.i32(x); }
    void v(uint32_t& x) { w.varint(x); }
    void v(uint64_t& x) { w.varint(x); }
    void v(int64_t& x) { w.svarint(x); }
    void v(double& x) { w.f64(x); }
    void v(float& x) { w.f32(x); }
    void text(std::string& s, size_t = maxText) { w.str(s); }
    void blob(std::string& s, size_t = maxBlob) { w.str(s); }
    void blob(std::vector<uint8_t>& b, size_t = maxBlob) { w.bytes(b); }
    template <class E> void e(E& x, int) { w.u8(static_cast<uint8_t>(x)); }
    template <class T, class F> void vec(std::vector<T>& xs, F&& each, size_t = 1)
    {
        w.varint(xs.size());
        for (auto& x : xs) each(x);
    }
    void optionalText(std::optional<std::string>& s)
    {
        w.boolean(s.has_value());
        if (s) w.str(*s);
    }
    ByteWriter w;
};

class Dec
{
public:
    static constexpr bool decoding = true;
    explicit Dec(std::string_view bytes) : r(bytes) {}
    void v(bool& x) { x = r.boolean(); }
    void v(uint8_t& x) { x = r.u8(); }
    void v(int& x) { x = r.i32(); }
    void v(uint32_t& x)
    {
        const auto n = r.varint();
        if (n > UINT32_MAX) throw CodecError("value out of range");
        x = static_cast<uint32_t>(n);
    }
    void v(uint64_t& x) { x = r.varint(); }
    void v(int64_t& x) { x = r.svarint(); }
    void v(double& x) { x = r.f64(); }
    void v(float& x) { x = r.f32(); }
    void text(std::string& s, size_t max = maxText) { s = r.str(max); }
    void blob(std::string& s, size_t max = maxBlob) { s = r.str(max); }
    void blob(std::vector<uint8_t>& b, size_t max = maxBlob) { b = r.bytes(max); }
    template <class E> void e(E& x, int max)
    {
        const auto n = r.u8();
        if (n > max) throw CodecError("enum out of range");
        x = static_cast<E>(n);
    }
    template <class T, class F> void vec(std::vector<T>& xs, F&& each, size_t minBytes = 1)
    {
        const size_t n = r.count(minBytes);
        xs.clear();
        xs.resize(n);
        for (auto& x : xs) each(x);
    }
    void optionalText(std::optional<std::string>& s)
    {
        if (r.boolean()) s = r.str(maxBlob);
        else s.reset();
    }
    ByteReader r;
};

template <class IO> void io(IO& c, ObjectKey& k)
{
    c.e(k.kind, 5);
    c.v(k.id);
}

template <class IO> void io(IO& c, ObjectState& o)
{
    io(c, o.key);
    c.optionalText(o.full);
}

template <class IO> void objects(IO& c, std::vector<ObjectState>& xs) { c.vec(xs, [&](ObjectState& o) { io(c, o); }, 3); }

template <class IO> void io(IO& c, Participant& p)
{
    c.v(p.id);
    c.text(p.name, maxNameLength * 4);
    c.e(p.role, 2);
    c.v(p.color);
    c.v(p.connected);
}

template <class IO> void io(IO& c, AssetEntry& a)
{
    c.text(a.absolutePath);
    c.text(a.relativePath);
    c.text(a.sourceDirectory);
    c.text(a.hash, 64);
    c.v(a.size);
    c.text(a.extension, 32);
}

template <class IO> void io(IO& c, HelloMsg& m)
{
    c.v(m.protocol);
    c.text(m.appVersion, 64);
    c.text(m.name, maxNameLength * 4);
    c.v(m.rejoinAs);
    c.text(m.rejoinToken, 128);
    c.v(m.lastSeq);
}

template <class IO> void io(IO& c, WelcomeMsg& m)
{
    c.v(m.you);
    c.e(m.role, 2);
    c.text(m.sessionName);
    c.vec(m.participants, [&](Participant& p) { io(c, p); }, 5);
    c.v(m.clipUidBase);
    c.text(m.rejoinToken, 128);
    c.v(m.snapshotFollows);
}

template <class IO> void io(IO& c, SnapshotMsg& m)
{
    c.v(m.seq);
    objects(c, m.objects);
    if constexpr (IO::decoding)
    {
        const size_t n = c.r.count(3);
        m.revisions.clear();
        for (size_t i = 0; i < n; ++i)
        {
            ObjectKey k;
            io(c, k);
            m.revisions[k] = c.r.varint();
        }
    }
    else
    {
        c.w.varint(m.revisions.size());
        for (auto& [k, rev] : m.revisions)
        {
            auto key = k;
            io(c, key);
            c.w.varint(rev);
        }
    }
    c.blob(m.document);
    c.text(m.sourceName);
    c.blob(m.sourceFile);
    c.vec(m.assets, [&](AssetEntry& a) { io(c, a); }, 6);
    c.v(m.transport.playing);
    c.v(m.transport.beat);
}

template <class IO> void io(IO& c, CommitMsg& m)
{
    c.v(m.seq);
    c.v(m.author);
    c.v(m.intentId);
    c.e(m.kind, 2);
    c.text(m.label);
    objects(c, m.objects);
}

template <class IO> void io(IO& c, IntentMsg& m)
{
    c.v(m.intentId);
    c.text(m.label);
    c.vec(m.bases, [&](BaseRevision& b) { io(c, b.key); c.v(b.revision); c.v(b.afterIntent); }, 4);
    objects(c, m.objects);
}

template <class IO> void io(IO& c, RejectMsg& m)
{
    c.v(m.intentId);
    c.text(m.code, 64);
    c.text(m.message);
    c.v(m.conflictAuthor);
    c.v(m.hasConflictAuthor);
}

template <class IO> void io(IO& c, UndoMsg& m) { c.v(m.redo); }

template <class IO> void io(IO& c, ParticipantsMsg& m) { c.vec(m.participants, [&](Participant& p) { io(c, p); }, 5); }

template <class IO> void io(IO& c, PresenceMsg& m)
{
    c.v(m.author);
    c.e(m.view, 2);
    c.v(m.trackId);
    c.v(m.beat);
    c.v(m.pitch);
    c.v(m.clipUid);
    c.v(m.selectedTrackFrom);
    c.v(m.selectedTrackTo);
    c.v(m.selectionStart);
    c.v(m.selectionEnd);
    c.v(m.selectedClipUid);
}

template <class IO> void io(IO& c, LiveMsg& m)
{
    c.v(m.author);
    c.vec(m.values, [&](LiveValue& v) { c.v(v.trackId); c.v(v.deviceId); c.v(v.target); c.v(v.value); }, 7);
}

template <class IO> void io(IO& c, PluginStateMsg& m)
{
    c.v(m.author);
    c.v(m.trackId);
    c.v(m.deviceId);
    c.blob(m.state, 64u << 20);
    c.blob(m.controllerState, 64u << 20);
}

template <class IO> void io(IO& c, TransportMsg& m)
{
    c.e(m.action, 2);
    c.v(m.beat);
    c.v(m.hostTimeMs);
    c.v(m.author);
}

template <class IO> void io(IO& c, ClockMsg& m)
{
    c.v(m.pong);
    c.v(m.sentMs);
    c.v(m.hostMs);
}

template <class IO> void io(IO& c, AssetRequestMsg& m)
{
    c.text(m.hash, 64);
    c.v(m.offset);
}

template <class IO> void io(IO& c, AssetChunkMsg& m)
{
    c.text(m.hash, 64);
    c.v(m.offset);
    c.v(m.total);
    c.blob(m.bytes, 1u << 20);
    c.v(m.missing);
}

template <class IO> void io(IO& c, AssetAddMsg& m) { c.vec(m.assets, [&](AssetEntry& a) { io(c, a); }, 6); }

template <class IO> void io(IO& c, UploadMsg& m)
{
    c.v(m.uploadId);
    c.text(m.name, 1024);
    c.v(m.offset);
    c.v(m.total);
    c.blob(m.bytes, 1u << 20);
}

template <class IO> void io(IO& c, DropRequestMsg& m)
{
    c.v(m.uploadId);
    c.v(m.trackId);
    c.v(m.beat);
    c.v(m.scene);
}

template <class IO> void io(IO& c, RoleMsg& m)
{
    c.v(m.who);
    c.e(m.role, 2);
}

template <class IO> void io(IO& c, NoticeMsg& m) { c.text(m.text); }
template <class IO> void io(IO& c, ByeMsg& m) { c.text(m.reason); }

template <size_t I = 0> Message decodeIndex(size_t index, Dec& d)
{
    if constexpr (I < std::variant_size_v<Message>)
    {
        if (index == I)
        {
            std::variant_alternative_t<I, Message> m;
            io(d, m);
            return m;
        }
        return decodeIndex<I + 1>(index, d);
    }
    else throw CodecError("unknown message type");
}

constexpr const char* names[] = {"hello", "welcome", "snapshot", "commit", "intent", "reject", "undo", "participants", "presence",
                                 "live", "plugin-state", "transport", "clock", "asset-request", "asset-chunk", "asset-add",
                                 "upload", "drop-request", "role", "notice", "bye"};
static_assert(std::size(names) == std::variant_size_v<Message>);
} // namespace

const char* toString(Role r)
{
    switch (r)
    {
        case Role::Host: return "Host";
        case Role::Editor: return "Editor";
        case Role::Listener: return "Listener";
    }
    return "?";
}

std::string encodeMessage(const Message& message)
{
    Enc e;
    e.w.u8(static_cast<uint8_t>(message.index()));
    std::visit([&](const auto& m) { io(e, const_cast<std::decay_t<decltype(m)>&>(m)); }, message);
    return e.w.take();
}

Message decodeMessage(std::string_view bytes)
{
    Dec d(bytes);
    const size_t index = d.r.u8();
    auto m = decodeIndex(index, d);
    d.r.expectEnd();
    return m;
}

const char* messageName(const Message& message) { return names[message.index()]; }

} // namespace sonora::collab
