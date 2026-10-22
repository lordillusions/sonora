#include "core/ModelCodec.h"

#include <cmath>
#include <map>
#include <type_traits>

namespace sonora::model::codec
{
namespace
{
using Shared = std::shared_ptr<const std::string>;
using SharedContent = std::shared_ptr<const NativeContent>;

constexpr uint8_t sharedNull = 0, sharedInline = 1, sharedBackref = 2;

uint64_t contentHash(const SharedContent& p)
{
    // Holder-keyed cache: the weak pointer proves the address was not reused by another holder.
    static thread_local std::map<const NativeContent*, std::pair<std::weak_ptr<const NativeContent>, uint64_t>> cache;
    if (auto it = cache.find(p.get()); it != cache.end() && ! it->second.first.expired() && it->second.first.lock() == p)
        return it->second.second;
    if (cache.size() > 4096)
        for (auto it = cache.begin(); it != cache.end();)
            it = it->second.first.expired() ? cache.erase(it) : std::next(it);
    uint64_t h = fnv1a64({reinterpret_cast<const char*>(p->data.data()), p->data.size()});
    h = fnv1a64({reinterpret_cast<const char*>(p->controllerData.data()), p->controllerData.size()}, h);
    cache[p.get()] = {p, h};
    return h;
}

// Encoder and Decoder expose the same visiting interface, so one visit() per struct keeps both directions in step.
// The encoder writes to a ByteWriter, or in the identity form to a HashWriter (the same values, hashed in place).
template <class Sink> class BasicEncoder
{
public:
    explicit BasicEncoder(Form form) : form_(form) {}
    static constexpr bool decoding = false;
    bool identity() const { return form_ == Form::Identity; }

    void v(int& x) { w_.i32(x); }
    void v(bool& x) { w_.boolean(x); }
    void v(double& x) { w_.f64(x); }
    void v(float& x) { w_.f32(x); }
    void v(int64_t& x) { w_.svarint(x); }
    void v(uint32_t& x) { w_.varint(x); }
    void v(std::string& s) { w_.str(s); }
    void v(std::vector<uint8_t>& b) { w_.bytes(b); }
    template <class E> void e(E& x, int /*max*/) { w_.i32(static_cast<int>(x)); }
    // Live values: synced by the live-parameter channel, so change detection ignores them.
    template <class T> void live(T& x)
    {
        if (! identity()) v(x);
    }
    template <class T, class F> void vec(std::vector<T>& xs, F&& each, size_t /*minBytes*/ = 1)
    {
        w_.varint(xs.size());
        for (auto& x : xs) each(x);
    }
    template <class T, class F> void opt(std::optional<T>& o, F&& each)
    {
        w_.boolean(o.has_value());
        if (o) each(*o);
    }
    void shared(Shared& p)
    {
        if (! p) { w_.u8(sharedNull); return; }
        if (identity())
        {
            // Immutable fragments: a changed fragment is a new pointer. Equal content behind a new pointer only costs a
            // redundant patch.
            w_.u8(sharedInline);
            w_.varint(reinterpret_cast<uintptr_t>(p.get()));
            return;
        }
        const auto [it, inserted] = sharedIndex_.emplace(p.get(), static_cast<uint32_t>(sharedIndex_.size()));
        if (! inserted) { w_.u8(sharedBackref); w_.varint(it->second); return; }
        w_.u8(sharedInline);
        w_.str(*p);
    }
    void content(SharedContent& p)
    {
        w_.boolean(p != nullptr);
        if (! p) return;
        auto& c = const_cast<NativeContent&>(*p);
        v(c.format);
        v(c.sourceDirectory);
        if (identity())
        {
            // Saving re-stores Wavetable content into a fresh holder, so compare by content (hashed once per holder).
            w_.varint(c.data.size());
            w_.varint(contentHash(p));
            w_.varint(c.controllerData.size());
            return;
        }
        v(c.data);
        v(c.controllerData);
    }
    void enter() {}
    void leave() {}
    void count(size_t n) { w_.varint(n); }

    Sink& sink() { return w_; }

private:
    Form form_;
    Sink w_;
    std::map<const void*, uint32_t> sharedIndex_;
};
using Encoder = BasicEncoder<ByteWriter>;
using Hasher = BasicEncoder<HashWriter>;

class Decoder
{
public:
    explicit Decoder(std::string_view bytes) : r_(bytes) {}
    static constexpr bool decoding = true;
    bool identity() const { return false; }

    void v(int& x) { x = r_.i32(); }
    void v(bool& x) { x = r_.boolean(); }
    void v(double& x) { x = r_.f64(); }
    void v(float& x) { x = r_.f32(); }
    void v(int64_t& x) { x = r_.svarint(); }
    void v(uint32_t& x)
    {
        const auto n = r_.varint();
        if (n > UINT32_MAX) throw CodecError("value out of range");
        x = static_cast<uint32_t>(n);
    }
    void v(std::string& s) { s = r_.str(); }
    void v(std::vector<uint8_t>& b) { b = r_.bytes(); }
    template <class E> void e(E& x, int max)
    {
        const int n = r_.i32();
        if (n < 0 || n > max) throw CodecError("enum out of range");
        x = static_cast<E>(n);
    }
    template <class T> void live(T& x) { v(x); }
    template <class T, class F> void vec(std::vector<T>& xs, F&& each, size_t minBytes = 1)
    {
        const size_t n = r_.count(minBytes);
        xs.clear();
        xs.resize(n);
        for (auto& x : xs) each(x);
    }
    template <class T, class F> void opt(std::optional<T>& o, F&& each)
    {
        o.reset();
        if (r_.boolean()) { o.emplace(); each(*o); }
    }
    void shared(Shared& p)
    {
        const auto tag = r_.u8();
        if (tag == sharedNull) { p.reset(); return; }
        if (tag == sharedInline)
        {
            p = std::make_shared<const std::string>(r_.str());
            table_.push_back(p);
            return;
        }
        if (tag != sharedBackref) throw CodecError("invalid shared tag");
        const auto index = r_.varint();
        if (index >= table_.size()) throw CodecError("invalid shared reference");
        p = table_[static_cast<size_t>(index)];
    }
    void content(SharedContent& p)
    {
        p.reset();
        if (! r_.boolean()) return;
        auto c = std::make_shared<NativeContent>();
        v(c->format);
        v(c->sourceDirectory);
        c->data = r_.bytes(NativeContent::maxBytes);
        c->controllerData = r_.bytes(NativeContent::maxBytes);
        p = std::move(c);
    }
    void enter()
    {
        if (++depth_ > maxDeviceDepth) throw CodecError("devices nested too deeply");
    }
    void count(size_t) {}
    void leave() { --depth_; }

    ByteReader& reader() { return r_; }

private:
    ByteReader r_;
    std::vector<Shared> table_;
    int depth_ = 0;
};


// Visits every floating-point value; finite_ becomes false at the first NaN or infinity.
class FiniteChecker
{
public:
    static constexpr bool decoding = false;
    bool identity() const { return false; }
    bool finite() const { return finite_; }

    template <class T> void v(T& x)
    {
        if constexpr (std::is_floating_point_v<T>)
            if (! std::isfinite(x)) finite_ = false;
    }
    template <class E> void e(E&, int) {}
    template <class T> void live(T& x) { v(x); }
    template <class T, class F> void vec(std::vector<T>& xs, F&& each, size_t = 1)
    {
        for (auto& x : xs) each(x);
    }
    template <class T, class F> void opt(std::optional<T>& o, F&& each)
    {
        if (o) each(*o);
    }
    void shared(Shared&) {}
    void content(SharedContent&) {}
    void enter() {}
    void leave() {}
    void count(size_t) {}

private:
    bool finite_ = true;
};

// ---- visitors: every field of Model.h, in declaration order.

template <class IO> void visit(IO& io, MidiBinding& b)
{
    io.v(b.channel);
    io.v(b.isNote);
    io.v(b.number);
    io.v(b.mapMode);
}

template <class IO> void visit(IO& io, Parameter& p)
{
    io.v(p.path);
    io.live(p.value);
    io.v(p.automationTargetId);
    visit(io, p.midi);
    io.v(p.macroIndex);
    io.v(p.rangeMin);
    io.v(p.rangeMax);
    io.v(p.booleanValue);
    io.v(p.macroMappingEdited);
}

template <class IO> void visit(IO& io, AutomationPoint& p)
{
    io.v(p.time);
    io.v(p.value);
    io.v(p.x1);
    io.v(p.y1);
    io.v(p.x2);
    io.v(p.y2);
}

template <class IO> void visit(IO& io, AutomationEnvelope& e)
{
    io.v(e.targetId);
    io.v(e.stepped);
    io.vec(e.points, [&](AutomationPoint& p) { visit(io, p); }, 48);
    io.v(e.eventType);
    io.v(e.midiController);
}

template <class IO> void visit(IO& io, NoteExpression& n)
{
    io.v(n.noteId);
    io.v(n.cc);
    io.vec(n.points, [&](NoteExpression::Point& p) {
        io.v(p.time); io.v(p.value); io.v(p.x1); io.v(p.y1); io.v(p.x2); io.v(p.y2);
    }, 48);
    io.v(n.edited);
}

template <class IO> void visit(IO& io, MidiNote& n)
{
    io.v(n.start);
    io.v(n.duration);
    io.v(n.pitch);
    io.v(n.velocity);
    io.v(n.releaseVelocity);
    io.v(n.probability);
    io.v(n.enabled);
    io.v(n.noteId);
    io.v(n.velocityDeviation);
    io.v(n.key);
}

template <class IO> void visit(IO& io, FileRef& f)
{
    io.v(f.absolutePath);
    io.v(f.relativePath);
    io.v(f.relativePathType);
    io.v(f.sourceDirectory);
}

template <class IO> void visit(IO& io, LoopSettings& l)
{
    io.v(l.loopStart);
    io.v(l.loopEnd);
    io.v(l.startRelative);
    io.v(l.enabled);
}

template <class IO> void visit(IO& io, Clip& c)
{
    io.v(c.uid);
    io.v(c.takeId);
    io.v(c.xmlId);
    io.v(c.cloneOfXmlId);
    io.v(c.cloneOfSessionSlot);
    io.v(c.cloneOfTrackId);
    io.shared(c.sourceXml);
    io.v(c.stripSourceEnvelopes);
    io.v(c.resetSourceMarkers);
    io.e(c.kind, 1);
    io.v(c.name);
    io.v(c.colorIndex);
    io.v(c.linkGroup);
    io.v(c.time);
    io.v(c.currentStart);
    io.v(c.currentEnd);
    visit(io, c.loop);
    io.v(c.disabled);
    io.v(c.launchMode);
    io.v(c.launchQuantisation);
    io.v(c.legato);
    io.v(c.scaleRoot);
    io.v(c.scaleName);
    io.v(c.inKey);
    io.v(c.velocityAmount);
    io.v(c.timeSigNumerator);
    io.v(c.timeSigDenominator);
    io.vec(c.notes, [&](MidiNote& n) { visit(io, n); }, 20);
    io.vec(c.noteExpressions, [&](NoteExpression& n) { visit(io, n); }, 4);
    io.v(c.noteExpressionsEdited);
    visit(io, c.sample);
    io.v(c.sampleRate);
    io.v(c.lengthSamples);
    io.v(c.warped);
    io.v(c.tempoAnalyzed);
    io.e(c.warpMode, 6);
    if constexpr (IO::decoding)
        if (c.warpMode == static_cast<WarpMode>(5)) throw CodecError("invalid warp mode");
    io.vec(c.warpMarkers, [&](WarpMarker& m) { io.v(m.seconds); io.v(m.beats); }, 16);
    io.v(c.gain);
    io.v(c.pitchCoarse);
    io.v(c.pitchFine);
    io.v(c.transientResolution);
    io.v(c.transientLoopMode);
    io.v(c.transientEnvelope);
    io.v(c.granularityTones);
    io.v(c.granularityTexture);
    io.v(c.fluctuationTexture);
    io.v(c.complexProFormants);
    io.v(c.complexProEnvelope);
    io.v(c.clipFade);
    io.v(c.ram);
    io.v(c.hiQ);
    io.v(c.fadeIn);
    io.v(c.fadeOut);
    io.v(c.fadeInCurve);
    io.v(c.fadeOutCurve);
    io.v(c.fadeInDefault);
    io.v(c.fadeOutDefault);
    io.vec(c.envelopes, [&](AutomationEnvelope& e) { visit(io, e); }, 4);
}

template <class IO> void visit(IO& io, PluginInfo& p)
{
    io.v(p.name);
    io.v(p.vendor);
    io.v(p.path);
    io.v(p.vst2UniqueId);
    for (auto& field : p.vst3Uid) io.v(field);
    io.v(p.auType);
    io.v(p.auSubType);
    io.v(p.auManufacturer);
    io.v(p.isInstrument);
    io.live(p.state);
    io.live(p.controllerState);
}

template <class IO> void visit(IO& io, SamplePart& s)
{
    io.shared(s.sourceXml);
    io.v(s.name);
    io.v(s.keyMin);
    io.v(s.keyMax);
    io.v(s.velMin);
    io.v(s.velMax);
    io.v(s.rootKey);
    io.v(s.detuneCents);
    io.v(s.pan);
    io.v(s.volume);
    io.v(s.sampleStart);
    io.v(s.sampleEnd);
    io.v(s.loopStart);
    io.v(s.loopEnd);
    io.v(s.loopMode);
    io.v(s.loopCrossfade);
    visit(io, s.sample);
    io.v(s.sampleRate);
    io.v(s.lengthSamples);
}

template <class IO> void visit(IO& io, Sidechain& s)
{
    io.v(s.on);
    io.v(s.sourceTrackId);
    io.e(s.tap, 2);
    io.v(s.gain);
    io.v(s.edited);
}

template <class IO> void visit(IO& io, PluginParam& p)
{
    io.v(p.parameterId);
    io.v(p.name);
    io.v(p.value);
    io.v(p.automationTargetId);
    io.v(p.valueEdited);
}

template <class IO> void visit(IO& io, Device& d);

template <class IO> void visit(IO& io, Device::Chain& c)
{
    io.shared(c.sourceXml);
    io.v(c.name);
    io.vec(c.devices, [&](Device& d) { visit(io, d); }, 8);
    io.v(c.volume);
    io.v(c.pan);
    io.v(c.speakerOn);
    io.v(c.receivingNote);
    io.v(c.sendingNote);
    io.v(c.chokeGroup);
    io.v(c.keyMin);
    io.v(c.keyMax);
    io.v(c.velMin);
    io.v(c.velMax);
    io.v(c.selMin);
    io.v(c.selMax);
}

template <class IO> void visit(IO& io, Device& d)
{
    io.enter();
    io.e(d.kind, 5);
    io.v(d.type);
    io.v(d.displayName);
    io.v(d.id);
    io.shared(d.sourceXml);
    io.v(d.sourceDirectory);
    io.v(d.maxPatch);
    io.v(d.maxPatchPath);
    io.live(d.enabled);
    io.v(d.enabledTargetId);
    if (io.identity())
    {
        // Saving captures moved native controls as new value-only entries (no automation target, no mapping): those are
        // live values too, so they don't make the device chain look structurally changed.
        std::vector<Parameter*> structural;
        for (auto& p : d.parameters)
            if (p.automationTargetId >= 0 || p.midi.channel >= 0 || p.macroIndex >= 0 || p.macroMappingEdited) structural.push_back(&p);
        io.count(structural.size());
        for (auto* p : structural) visit(io, *p);
    }
    else io.vec(d.parameters, [&](Parameter& p) { visit(io, p); }, 8);
    // Native "plain" leaves: the keys belong to the device structure, the values are live.
    if constexpr (IO::decoding)
    {
        auto& r = io.reader();
        const size_t n = r.count(9);
        d.plain.clear();
        for (size_t i = 0; i < n; ++i)
        {
            auto key = r.str(4096);
            d.plain[std::move(key)] = r.f64();
        }
    }
    else
    {
        io.count(d.plain.size());
        for (auto& [key, value] : d.plain)
        {
            auto k = key;
            io.v(k);
            io.live(value);
        }
    }
    io.vec(d.sampleParts, [&](SamplePart& s) { visit(io, s); }, 16);
    io.v(d.samplePartsEdited);
    io.v(d.chainsEdited);
    visit(io, d.sidechain);
    io.vec(d.pluginParams, [&](PluginParam& p) { visit(io, p); }, 8);
    io.opt(d.plugin, [&](PluginInfo& p) { visit(io, p); });
    io.content(d.nativeContent);
    io.v(d.nativeContentEdited);
    io.vec(d.chains, [&](Device::Chain& c) { visit(io, c); }, 8);
    io.vec(d.macroNames, [&](std::string& s) { io.v(s); }, 1);
    io.v(d.numVisibleMacros);
    io.leave();
}

template <class IO> void visit(IO& io, Mixer& m)
{
    io.v(m.sendsEdited);
    io.vec(m.sendSourceXml, [&](Shared& p) { io.shared(p); }, 1);
    io.live(m.volume);
    io.live(m.pan);
    io.live(m.speakerOn);
    io.live(m.solo);
    io.vec(m.sends, [&](double& s) { io.live(s); }, 8);
    io.v(m.volumeTargetId);
    io.v(m.panTargetId);
    io.v(m.speakerTargetId);
    io.v(m.tempoTargetId);
    io.v(m.timeSignatureTargetId);
    io.vec(m.sendTargetIds, [&](int& id) { io.v(id); }, 1);
    visit(io, m.volumeMidi);
    visit(io, m.panMidi);
    visit(io, m.speakerMidi);
    io.vec(m.sendMidi, [&](MidiBinding& b) { visit(io, b); }, 4);
}

template <class IO> void visit(IO& io, TakeLane& l)
{
    io.v(l.id);
    io.v(l.height);
    io.v(l.name);
    io.v(l.annotation);
    io.v(l.audition);
    io.vec(l.clips, [&](Clip& c) { visit(io, c); }, 64);
    io.shared(l.sourceXml);
}

template <class IO> void visitSlots(IO& io, std::vector<ClipSlot>& slots)
{
    io.vec(slots, [&](ClipSlot& s) { io.opt(s.clip, [&](Clip& c) { visit(io, c); }); });
}

// Track without devices, arrangement clips and Session clips (slot count only).
template <class IO> void visitHeader(IO& io, Track& t)
{
    io.e(t.kind, 4);
    io.v(t.id);
    io.v(t.name);
    io.v(t.nameIsAutomatic);
    io.v(t.colorIndex);
    io.v(t.groupId);
    io.v(t.outputRouting);
    io.v(t.outputRoutingEdited);
    io.shared(t.sourceXml);
    io.v(t.trackDelayMs);
    io.v(t.folded);
    io.v(t.inputRouting);
    io.v(t.inputUpperDisplay);
    io.v(t.inputLowerDisplay);
    io.v(t.monitoring);
    io.v(t.inputRoutingPresent);
    io.v(t.monitoringPresent);
    io.v(t.inputRoutingEdited);
    io.v(t.monitoringEdited);
    for (auto& id : t.midiControllerTargets) io.v(id);
    io.v(t.midiControllersPresent);
    io.v(t.midiControllersEdited);
    visit(io, t.mixer);
    int slotCount = static_cast<int>(t.slots.size());
    io.v(slotCount);
    if constexpr (IO::decoding)
    {
        if (slotCount < 0 || slotCount > 100000) throw CodecError("slot count out of range");
        t.slots.assign(static_cast<size_t>(slotCount), {});
    }
    io.vec(t.takeLanes, [&](TakeLane& l) { visit(io, l); }, 8);
    io.shared(t.takeLanesSourceXml);
    io.v(t.takeLanesNeedPatch);
    io.v(t.nextTakeLaneId);
    io.v(t.nextTakeId);
    io.v(t.recorderTakeCounter);
    io.v(t.takeCounterAdvanced);
    io.vec(t.automation, [&](AutomationEnvelope& e) { visit(io, e); }, 4);
    io.v(t.hidden);
    io.v(t.deactivated);
    io.v(t.frozen);
    io.vec(t.frozenArrangement, [&](Clip& c) { visit(io, c); }, 64);
    visitSlots(io, t.frozenSlots);
}

template <class IO> void visitSongHeader(IO& io, Song& s)
{
    io.v(s.nextPointeeId);
    io.v(s.pointeeIdsAllocated);
    io.v(s.nextDeviceId);
    io.v(s.tempo);
    io.v(s.timeSigNumerator);
    io.v(s.timeSigDenominator);
    io.vec(s.sendPreXml, [&](Shared& p) { io.shared(p); }, 1);
    io.v(s.sendsPreEdited);
    io.vec(s.scenes, [&](Scene& scene) {
        io.v(scene.name);
        io.v(scene.colorIndex);
        io.opt(scene.tempo, [&](double& tempo) { io.v(tempo); });
    }, 3);
    io.vec(s.locators, [&](Locator& l) { io.v(l.time); io.v(l.name); }, 9);
    io.v(s.loopStart);
    io.v(s.loopLength);
    io.v(s.loopOn);
    io.v(s.punchIn);
    io.v(s.punchOut);
    io.v(s.punchInPresent);
    io.v(s.punchOutPresent);
    io.v(s.punchEdited);
    io.v(s.currentTime);
    io.v(s.globalQuantisation);
    io.v(s.scaleRoot);
    io.v(s.scaleName);
    io.v(s.inKey);
}

template <class IO> void visitDocument(IO& io, Song& s)
{
    if constexpr (IO::decoding)
    {
        auto& r = io.reader();
        const size_t n = r.count(4);
        s.savedClips.clear();
        for (size_t i = 0; i < n; ++i)
        {
            const int uid = r.i32();
            Song::SavedClipLocation l;
            l.trackId = r.i32();
            l.ordinal = r.i32();
            l.sessionSlot = r.i32();
            s.savedClips[uid] = l;
        }
    }
    else
    {
        io.count(s.savedClips.size());
        for (auto& [uid, l] : s.savedClips)
        {
            int u = uid;
            io.v(u);
            io.v(l.trackId);
            io.v(l.ordinal);
            io.v(l.sessionSlot);
        }
    }
    io.v(s.sourcePath);
    io.v(s.creator);
    io.v(s.schemaVersion);
    io.vec(s.warnings, [&](std::string& w) { io.v(w); }, 1);
    io.shared(s.midiClipTemplate);
}

template <class IO> void visitTrack(IO& io, Track& t)
{
    visitHeader(io, t);
    io.vec(t.devices, [&](Device& d) { visit(io, d); }, 8);
    io.vec(t.arrangementClips, [&](Clip& c) { visit(io, c); }, 64);
    visitSlots(io, t.slots);
}

template <class F> std::string encodeWith(Form form, F&& body)
{
    Encoder io(form);
    uint32_t version = formatVersion;
    io.v(version);
    body(io);
    return io.sink().take();
}

// The identity form's values, hashed in place: the same visit() calls as encodeWith(Form::Identity, ...).
template <class F> uint64_t hashWith(F&& body)
{
    Hasher io(Form::Identity);
    uint32_t version = formatVersion;
    io.v(version);
    body(io);
    return io.sink().value();
}

template <class F> void decodeWith(std::string_view bytes, F&& body)
{
    Decoder io(bytes);
    uint32_t version = 0;
    io.v(version);
    if (version != formatVersion) throw CodecError("unsupported model codec version");
    body(io);
    io.reader().expectEnd();
}

#if defined(__APPLE__) && defined(__aarch64__) && defined(_LIBCPP_VERSION)
// A new model field changes these sizes: add it to the visitors above (and to the live/identity split), then update.
static_assert(sizeof(Clip) == 520, "model::Clip changed: update ModelCodec");
static_assert(sizeof(Device) == 544, "model::Device changed: update ModelCodec");
static_assert(sizeof(Device::Chain) == 120, "model::Device::Chain changed: update ModelCodec");
static_assert(sizeof(Track) == 1136, "model::Track changed: update ModelCodec");
static_assert(sizeof(Mixer) == 200, "model::Mixer changed: update ModelCodec");
static_assert(sizeof(Song) == 1480, "model::Song changed: update ModelCodec");
static_assert(sizeof(MidiNote) == 48, "model::MidiNote changed: update ModelCodec");
static_assert(sizeof(SamplePart) == 232, "model::SamplePart changed: update ModelCodec");
static_assert(sizeof(PluginInfo) == 160, "model::PluginInfo changed: update ModelCodec");
static_assert(sizeof(Parameter) == 80, "model::Parameter changed: update ModelCodec");
static_assert(sizeof(TakeLane) == 104, "model::TakeLane changed: update ModelCodec");
static_assert(sizeof(AutomationEnvelope) == 64, "model::AutomationEnvelope changed: update ModelCodec");
#endif

} // namespace

bool finiteSongHeader(const Song& song)
{
    FiniteChecker io;
    visitSongHeader(io, const_cast<Song&>(song));
    return io.finite();
}

bool finiteTrackHeader(const Track& track)
{
    FiniteChecker io;
    visitHeader(io, const_cast<Track&>(track));
    return io.finite();
}

bool finiteDevices(const std::vector<Device>& devices)
{
    FiniteChecker io;
    for (auto& d : const_cast<std::vector<Device>&>(devices)) visit(io, d);
    return io.finite();
}

bool finiteClip(const Clip& clip)
{
    FiniteChecker io;
    visit(io, const_cast<Clip&>(clip));
    return io.finite();
}

std::string encodeSongHeader(const Song& song, Form form)
{
    return encodeWith(form, [&](Encoder& io) { visitSongHeader(io, const_cast<Song&>(song)); });
}

uint64_t hashSongHeader(const Song& song)
{
    return hashWith([&](Hasher& io) { visitSongHeader(io, const_cast<Song&>(song)); });
}

void decodeSongHeader(std::string_view bytes, Song& song)
{
    decodeWith(bytes, [&](Decoder& io) { visitSongHeader(io, song); });
}

std::string encodeDocument(const Song& song)
{
    return encodeWith(Form::Full, [&](Encoder& io) { visitDocument(io, const_cast<Song&>(song)); });
}

void decodeDocument(std::string_view bytes, Song& song)
{
    decodeWith(bytes, [&](Decoder& io) { visitDocument(io, song); });
}

std::string encodeTrackHeader(const Track& track, Form form)
{
    return encodeWith(form, [&](Encoder& io) { visitHeader(io, const_cast<Track&>(track)); });
}

uint64_t hashTrackHeader(const Track& track)
{
    return hashWith([&](Hasher& io) { visitHeader(io, const_cast<Track&>(track)); });
}

Track decodeTrackHeader(std::string_view bytes)
{
    Track track;
    decodeWith(bytes, [&](Decoder& io) { visitHeader(io, track); });
    return track;
}

std::string encodeDevices(const std::vector<Device>& devices, Form form)
{
    return encodeWith(form, [&](Encoder& io) {
        io.vec(const_cast<std::vector<Device>&>(devices), [&](Device& d) { visit(io, d); });
    });
}

uint64_t hashDevices(const std::vector<Device>& devices)
{
    return hashWith([&](Hasher& io) {
        io.vec(const_cast<std::vector<Device>&>(devices), [&](Device& d) { visit(io, d); });
    });
}

std::vector<Device> decodeDevices(std::string_view bytes)
{
    std::vector<Device> devices;
    decodeWith(bytes, [&](Decoder& io) { io.vec(devices, [&](Device& d) { visit(io, d); }, 8); });
    return devices;
}

std::string encodeClip(const Clip& clip, Form form)
{
    return encodeWith(form, [&](Encoder& io) { visit(io, const_cast<Clip&>(clip)); });
}

uint64_t hashClip(const Clip& clip)
{
    return hashWith([&](Hasher& io) { visit(io, const_cast<Clip&>(clip)); });
}

Clip decodeClip(std::string_view bytes)
{
    Clip clip;
    decodeWith(bytes, [&](Decoder& io) { visit(io, clip); });
    return clip;
}

std::string encodeSong(const Song& song)
{
    return encodeWith(Form::Full, [&](Encoder& io) {
        auto& s = const_cast<Song&>(song);
        visitSongHeader(io, s);
        visitDocument(io, s);
        io.vec(s.tracks, [&](Track& t) { visitTrack(io, t); });
        io.vec(s.returns, [&](Track& t) { visitTrack(io, t); });
        visitTrack(io, s.main);
    });
}

Song decodeSong(std::string_view bytes)
{
    Song song;
    decodeWith(bytes, [&](Decoder& io) {
        visitSongHeader(io, song);
        visitDocument(io, song);
        io.vec(song.tracks, [&](Track& t) { visitTrack(io, t); }, 64);
        io.vec(song.returns, [&](Track& t) { visitTrack(io, t); }, 64);
        visitTrack(io, song.main);
    });
    return song;
}

} // namespace sonora::model::codec
