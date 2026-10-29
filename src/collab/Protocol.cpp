#include "collab/Protocol.h"

#include <juce_core/juce_core.h>

#include <array>
#include <bit>
#include <cstring>

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
    WireDictionary* dictionary = nullptr;
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
    WireDictionary* dictionary = nullptr;
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

// COL-3c: deflate (zlib) for big bodies. Level 1: the model codec's objects compress ~5x even at the fastest level
// (a 47-track set's 119 MB snapshot body: 25 MB in 1.4 s), and the host compresses on the message thread.
constexpr size_t packThreshold = 4096; // smaller bodies (most edits) go as they are
constexpr size_t maxPackedBody = 512u << 20; // inflated, the transport's message limit

std::string deflate(const std::string& bytes)
{
    juce::MemoryOutputStream out;
    {
        juce::GZIPCompressorOutputStream zip(out, 1);
        zip.write(bytes.data(), bytes.size());
        zip.flush();
    }
    return {static_cast<const char*>(out.getData()), out.getDataSize()};
}

// Exactly `size` bytes or CodecError: a short, long or corrupt stream never yields a partial body.
std::string inflate(const std::string& packed, uint64_t size)
{
    if (size > maxPackedBody) throw CodecError("packed body too large");
    juce::GZIPDecompressorInputStream zip(new juce::MemoryInputStream(packed.data(), packed.size(), false), true,
                                          juce::GZIPDecompressorInputStream::zlibFormat);
    // Grows as it inflates: a forged size alone never allocates the maximum.
    std::string body;
    body.reserve(static_cast<size_t>(std::min<uint64_t>(size, 64u << 20)));
    std::vector<char> buffer(1u << 20);
    while (body.size() < size)
    {
        const auto n = zip.read(buffer.data(), static_cast<int>(std::min<uint64_t>(buffer.size(), size - body.size())));
        if (n <= 0) throw CodecError("packed body truncated");
        body.append(buffer.data(), static_cast<size_t>(n));
    }
    char extra = 0;
    if (zip.read(&extra, 1) > 0) throw CodecError("packed body too long");
    return body;
}

// Content-defined chunks (gear rolling hash, 4-64 KiB, ~20 KiB on average): an edit in the middle of a body changes
// only the chunks around it, the rest cut at the same places as before.
constexpr size_t minChunk = 4u << 10, maxChunk = 64u << 10;
constexpr uint64_t chunkMask = (1u << 14) - 1;
constexpr size_t dedupThreshold = 64u << 10; // smaller bodies are only deflated

const std::array<uint64_t, 256>& gear()
{
    static const auto table = [] {
        std::array<uint64_t, 256> t{};
        uint64_t x = 0;
        for (auto& v : t) // splitmix64
        {
            x += 0x9e3779b97f4a7c15ull;
            uint64_t z = x;
            z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
            z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
            v = z ^ (z >> 31);
        }
        return t;
    }();
    return table;
}

template <class F> void forEachChunk(std::string_view bytes, F&& each)
{
    const auto& g = gear();
    size_t start = 0;
    while (start < bytes.size())
    {
        const size_t end = std::min(bytes.size(), start + maxChunk);
        size_t cut = end;
        uint64_t h = 0;
        for (size_t i = start + minChunk; i < end; ++i)
        {
            h = (h << 1) + g[static_cast<uint8_t>(bytes[i])];
            if ((h & chunkMask) == 0) { cut = i + 1; break; }
        }
        each(bytes.substr(start, cut - start));
        start = cut;
    }
}

// 64-bit chunk key (length mixed in). Not cryptographic: a peer that forges a collision only corrupts its own messages.
uint64_t chunkKey(std::string_view s)
{
    uint64_t h = 0x9e3779b97f4a7c15ull ^ (static_cast<uint64_t>(s.size()) * 0xff51afd7ed558ccdull);
    size_t i = 0;
    for (; i + 8 <= s.size(); i += 8)
    {
        uint64_t v = 0;
        std::memcpy(&v, s.data() + i, 8);
        h ^= v * 0x87c37b91114253d5ull;
        h = std::rotl(h, 31) * 0x4cf5ad432745937full;
    }
    uint64_t tail = 0;
    std::memcpy(&tail, s.data() + i, s.size() - i);
    h ^= tail * 0x87c37b91114253d5ull;
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdull;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53ull;
    return h ^ (h >> 33);
}

// The pool's copy of a chunk when another link still holds it, else a new one.
WireChunk intern(WirePool& pool, uint64_t key, std::string_view bytes)
{
    auto& slot = pool.chunks[key];
    if (auto existing = slot.lock(); existing && *existing == bytes) return existing;
    auto chunk = std::make_shared<const std::string>(bytes);
    slot = chunk;
    if (pool.chunks.size() > 4096 && pool.chunks.size() % 4096 == 0) // drop entries of closed links now and then
        for (auto it = pool.chunks.begin(); it != pool.chunks.end();)
            it = it->second.expired() ? pool.chunks.erase(it) : std::next(it);
    return chunk;
}

const WireChunk* known(const WireDictionary& d, uint64_t key)
{
    if (auto it = d.sent.find(key); it != d.sent.end()) return &it->second;
    if (auto it = d.received.find(key); it != d.received.end()) return &it->second;
    return nullptr;
}

// Body -> entries (count; then 0 + literal bytes, or 1 + key). New literals are remembered as sent.
std::string dedupe(std::string_view body, WireDictionary& d)
{
    ByteWriter entries;
    std::vector<std::string_view> chunks;
    forEachChunk(body, [&](std::string_view c) { chunks.push_back(c); });
    entries.varint(chunks.size());
    for (const auto c : chunks)
    {
        const auto key = chunkKey(c);
        if (known(d, key))
        {
            entries.u8(1);
            entries.varint(key);
            d.referencedBytes += c.size();
            continue;
        }
        entries.u8(0);
        entries.str(c);
        if (d.sentBytes + c.size() <= WireDictionary::maxBytes)
        {
            d.sent.emplace(key, intern(*d.pool, key, c));
            d.sentBytes += c.size();
        }
    }
    return entries.take();
}

std::string redupe(std::string_view entriesBytes, uint64_t size, WireDictionary& d)
{
    if (size > maxPackedBody) throw CodecError("packed body too large");
    ByteReader r(entriesBytes);
    const size_t n = r.count(2);
    std::string body;
    body.reserve(static_cast<size_t>(std::min<uint64_t>(size, 64u << 20)));
    for (size_t i = 0; i < n; ++i)
    {
        const auto kind = r.u8();
        if (kind == 0)
        {
            const auto c = r.str(maxChunk);
            if (body.size() + c.size() > size) throw CodecError("packed body too long");
            const auto key = chunkKey(c);
            if (! d.received.count(key) && d.receivedBytes + c.size() <= WireDictionary::maxBytes)
            {
                d.received.emplace(key, intern(*d.pool, key, c));
                d.receivedBytes += c.size();
            }
            body += c;
        }
        else if (kind == 1)
        {
            const auto* chunk = known(d, r.varint());
            if (! chunk) throw CodecError("reference to an unknown chunk");
            if (body.size() + (*chunk)->size() > size) throw CodecError("packed body too long");
            body += **chunk;
            d.referencedBytes += (*chunk)->size();
        }
        else throw CodecError("invalid chunk entry");
    }
    r.expectEnd();
    if (body.size() != size) throw CodecError("packed body truncated");
    return body;
}

// Fields written by `fields` as one body: a mode byte, the body and for compressed bodies their size. Modes: 0 plain
// (under 4 KiB), 1 deflated, 2 deduplicated against the link's dictionary, then deflated.
template <class IO, class F> void packed(IO& c, F&& fields)
{
    if constexpr (IO::decoding)
    {
        const auto mode = c.r.u8();
        if (mode > 2) throw CodecError("invalid packing");
        std::string bytes;
        c.blob(bytes, maxPackedBody);
        if (mode >= 1) bytes = inflate(bytes, c.r.varint());
        if (mode == 2)
        {
            if (! c.dictionary) throw CodecError("deduplicated body without a link dictionary");
            bytes = redupe(bytes, c.r.varint(), *c.dictionary);
        }
        Dec inner(bytes);
        fields(inner);
        inner.r.expectEnd();
    }
    else
    {
        Enc inner;
        fields(inner);
        auto bytes = inner.w.take();
        if (bytes.size() < packThreshold)
        {
            c.w.u8(0);
            c.blob(bytes);
            return;
        }
        if (c.dictionary && bytes.size() >= dedupThreshold)
        {
            auto entries = dedupe(bytes, *c.dictionary);
            auto zipped = deflate(entries);
            c.w.u8(2);
            c.blob(zipped);
            c.w.varint(entries.size());
            c.w.varint(bytes.size());
            return;
        }
        auto zipped = deflate(bytes);
        c.w.u8(1);
        c.blob(zipped);
        c.w.varint(bytes.size());
    }
}

template <class IO> void revisions(IO& c, std::map<ObjectKey, uint64_t>& xs)
{
    if constexpr (IO::decoding)
    {
        const size_t n = c.r.count(3);
        xs.clear();
        for (size_t i = 0; i < n; ++i)
        {
            ObjectKey k;
            io(c, k);
            xs[k] = c.r.varint();
        }
    }
    else
    {
        c.w.varint(xs.size());
        for (auto& [k, rev] : xs)
        {
            auto key = k;
            io(c, key);
            c.w.varint(rev);
        }
    }
}

// Protocol 2: everything but the source file is one packed body; the source file is already gzip (.als).
template <class IO> void io(IO& c, SnapshotMsg& m)
{
    c.v(m.seq);
    packed(c, [&](auto& p) {
        objects(p, m.objects);
        revisions(p, m.revisions);
        p.blob(m.document, maxPackedBody);
        p.vec(m.assets, [&](AssetEntry& a) { io(p, a); }, 6);
        p.v(m.transport.playing);
        p.v(m.transport.beat);
    });
    c.text(m.sourceName);
    c.blob(m.sourceFile);
}

template <class IO> void io(IO& c, CommitMsg& m)
{
    c.v(m.seq);
    c.v(m.author);
    c.v(m.intentId);
    c.e(m.kind, 2);
    c.text(m.label);
    packed(c, [&](auto& p) { objects(p, m.objects); });
}

template <class IO> void io(IO& c, IntentMsg& m)
{
    c.v(m.intentId);
    c.text(m.label);
    c.vec(m.bases, [&](BaseRevision& b) { io(c, b.key); c.v(b.revision); c.v(b.afterIntent); }, 4);
    packed(c, [&](auto& p) { objects(p, m.objects); });
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

bool usesDictionary(const Message& m)
{
    return std::holds_alternative<SnapshotMsg>(m) || std::holds_alternative<CommitMsg>(m) || std::holds_alternative<IntentMsg>(m);
}

std::string encodeMessage(const Message& message, WireDictionary* dictionary)
{
    Enc e;
    e.dictionary = dictionary;
    e.w.u8(static_cast<uint8_t>(message.index()));
    std::visit([&](const auto& m) { io(e, const_cast<std::decay_t<decltype(m)>&>(m)); }, message);
    return e.w.take();
}

Message decodeMessage(std::string_view bytes, WireDictionary* dictionary)
{
    Dec d(bytes);
    d.dictionary = dictionary;
    const size_t index = d.r.u8();
    auto m = decodeIndex(index, d);
    d.r.expectEnd();
    return m;
}

const char* messageName(const Message& message) { return names[message.index()]; }

} // namespace sonora::collab
