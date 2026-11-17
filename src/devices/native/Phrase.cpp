#include "devices/native/Phrase.h"

#include "core/PatternRandom.h"
#include "core/Scales.h"

#include <algorithm>
#include <cmath>

namespace sonora::devices
{
namespace
{
std::vector<std::string> keyNames() { return {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"}; }
std::vector<std::string> scaleNames()
{
    std::vector<std::string> names;
    for (const auto& s : model::kScales) names.emplace_back(s.name);
    return names;
}

std::vector<ParameterInfo> phraseParameters()
{
    using P = Phrase;
    std::vector<ParameterInfo> p(P::NumParams);
    p[P::Key] = makeParam("Key", "Key", 0, 11, 4, "", false, keyNames());
    p[P::Scale] = makeParam("Scale", "Scale", 0, (float) model::kScales.size() - 1, 1, "", false, scaleNames());
    p[P::Chords] = makeParam("Chords", "Chords", 0, 2, 0, "", false, {"Progression", "MIDI In", "Link"});
    p[P::LinkChannel] = makeParam("LinkChannel", "Link", 0, 4, 0, "", false, {"Off", "A", "B", "C", "D"});
    p[P::Part] = makeParam("Part", "Part", 0, 3, 0, "", false, {"Arp", "Stabs", "Pad", "Bass"});
    p[P::Pattern] = makeParam("Pattern", "Pattern", 0, 8, 0, "", false, {"Form", "A", "B", "C", "D", "E", "F", "G", "H"});
    p[P::Density] = makeParam("Density", "Density", 0, 1, 1, "%");
    p[P::Swing] = makeParam("Swing", "Swing", 0, 1, 0, "%");
    p[P::Gate] = makeParam("Gate", "Gate", 0.25f, 4.0f, 1, "%");
    p[P::Velocity] = makeParam("Velocity", "Velocity", 0.25f, 2.0f, 1, "%");
    p[P::Accent] = makeParam("Accent", "Accent", 0, 60, 20, "int");
    p[P::Octave] = makeParam("Octave", "Octave", -3, 3, 0, "int");
    p[P::Shift] = makeParam("Shift", "Shift", -7, 7, 0, "int");
    p[P::Humanize] = makeParam("Humanize", "Humanize", 0, 1, 0, "%");
    p[P::KickGuard] = makeParam("KickGuard", "Kick Guard", 0, 1, 0, "%");
    p[P::Seed] = makeParam("Seed", "Seed", 1, 999, 1, "int");
    p[P::Latch] = makeParam("Latch", "Latch", 0, 1, 0, "", true);
    p[P::Audition] = makeParam("Audition", "Audition", 0, 1, 0, "", true);
    return p;
}

uint64_t packChord(const model::ChordTones& t)
{
    uint64_t v = (uint64_t) (t.root & 15) | ((uint64_t) (t.count & 7) << 4) | ((uint64_t) (t.bass < 0 ? 31 : t.bass & 15) << 7)
                 | ((uint64_t) (t.third ? 1 : 0) << 12);
    for (int i = 0; i < 6; ++i) v |= (uint64_t) (std::clamp((int) t.intervals[(size_t) i], 0, 31)) << (13 + 5 * i);
    return v;
}

model::ChordTones unpackChord(uint64_t v)
{
    model::ChordTones t;
    t.root = (int) (v & 15);
    t.count = (int) ((v >> 4) & 7);
    const int bass = (int) ((v >> 7) & 31);
    t.bass = bass == 31 ? -1 : bass;
    t.third = ((v >> 12) & 1) != 0;
    for (int i = 0; i < 6; ++i) t.intervals[(size_t) i] = (int8_t) ((v >> (13 + 5 * i)) & 31);
    return t;
}

struct SinkAdapter
{
    MidiGenerator::Sink& sink;
    void note(double beat, double length, int pitch, int velocity) { sink.note(beat, length, pitch, velocity); }
};
} // namespace

Phrase::Phrase(model::HarmonyLink& link)
    : MidiGenerator(phraseParameters()), link_(link), id_(model::HarmonyLink::newId()), slot_(model::defaultPhraseState())
{
}

Phrase::~Phrase()
{
    if (ledChannel_ >= 0) link_.release(ledChannel_, id_);
}

std::vector<DeviceProcessor::UiGroup> Phrase::uiGroups() const
{
    return {
        {"Harmony", {Key, Scale, Chords, LinkChannel}},
        {"Part", {Part, Pattern, Seed, Latch}},
        {"Perform", {Density, Swing, Gate, Velocity, Accent, Octave, Shift, Humanize, KickGuard}, 5},
    };
}

void Phrase::publishState()
{
    slot_.publish();
    stateVersion_.fetch_add(1, std::memory_order_relaxed);
}

void Phrase::setState(const model::PhraseState& s)
{
    slot_.edit() = s;
    publishState();
}

void Phrase::dice(int slotIndex, model::DiceKeep keep)
{
    auto& s = slot_.edit();
    const auto seed = model::PatternRandom::hash(s.part(part()).diceSeed, slotIndex, (int64_t) keep, 0x0d1ce);
    model::phraseGenerate(s, part(), slotIndex, seed == 0 ? 1 : seed, keep);
    publishState();
}

void Phrase::answer(int from, int to, model::AnswerKind kind)
{
    model::phraseAnswer(slot_.edit(), part(), from, to, kind);
    publishState();
}

void Phrase::mutate(int slotIndex)
{
    auto& s = slot_.edit();
    auto& p = s.part(part());
    p.diceSeed = model::PatternRandom::hash(p.diceSeed, slotIndex, 0x7a7e);
    model::phraseMutate(s, part(), slotIndex, p.diceSeed);
    publishState();
}

model::PhraseLive Phrase::liveValues() const
{
    model::PhraseLive l;
    l.part = part();
    l.pattern = std::clamp((int) std::lround(getParameter(Pattern)), 0, 8);
    l.key = keyInUse();
    l.scale = scaleInUse();
    l.density = getParameter(Density);
    l.swing = getParameter(Swing);
    l.gate = getParameter(Gate);
    l.velocity = getParameter(Velocity);
    l.accent = (int) std::lround(getParameter(Accent));
    l.octave = (int) std::lround(getParameter(Octave));
    l.shift = (int) std::lround(getParameter(Shift));
    l.humanize = getParameter(Humanize);
    l.kickGuard = getParameter(KickGuard);
    l.seed = (uint32_t) std::lround(getParameter(Seed));
    return l;
}

int Phrase::keyInUse() const
{
    if (linkStatus() == LinkStatus::Following)
    {
        linkSeen_.update();
        return linkSeen_.live().key;
    }
    return (int) std::lround(getParameter(Key));
}

int Phrase::scaleInUse() const
{
    if (linkStatus() == LinkStatus::Following)
    {
        linkSeen_.update();
        return linkSeen_.live().scale;
    }
    return (int) std::lround(getParameter(Scale));
}

model::Progression Phrase::progressionInUse() const
{
    if (linkStatus() == LinkStatus::Following)
    {
        linkSeen_.update();
        return linkSeen_.live().progression;
    }
    return state().progression;
}

model::ChordTones Phrase::heldChord() const { return unpackChord(heldPacked_.load(std::memory_order_relaxed)); }

double Phrase::clipLengthBeats() const
{
    const auto& s = state();
    double length = s.part(part()).formBeats();
    const int pattern = (int) std::lround(getParameter(Pattern));
    if (pattern >= 1) length = s.part(part()).slots[(size_t) (pattern - 1)].lengthBeats();
    if (std::lround(getParameter(Chords)) != FromMidi) length = std::max(length, progressionInUse().lengthBeats());
    return std::clamp(length, 4.0, 128.0);
}

// ------------------------------------------------------------------------------------------------------ audio
void Phrase::audition(const int* pitches, int count)
{
    uint64_t packed = 0;
    int n = 0;
    for (int i = 0; i < count && n < 6; ++i)
        if (pitches[i] >= 0 && pitches[i] <= 127) packed |= (uint64_t) pitches[i] << (7 * n++);
    if (n > 0) auditionRequest_.store(packed | ((uint64_t) n << 60), std::memory_order_release);
}

void Phrase::blockStarted()
{
    if (const auto request = auditionRequest_.exchange(0, std::memory_order_acq_rel); request != 0)
    {
        auditionCount_ = (int) (request >> 60);
        for (int i = 0; i < auditionCount_; ++i) auditionPitches_[(size_t) i] = (int) ((request >> (7 * i)) & 127);
        auditionFresh_ = true;
    }
    if (slot_.update()) seenVersion_ = stateVersion_.load(std::memory_order_relaxed);
    updateLink();
}

void Phrase::updateLink()
{
    const int mode = (int) std::lround(value(Chords));
    const int channel = (int) std::lround(value(LinkChannel)) - 1;
    const bool wantLead = mode == FromProgression && channel >= 0;
    if (ledChannel_ >= 0 && (! wantLead || channel != ledChannel_))
    {
        link_.release(ledChannel_, id_);
        ledChannel_ = -1;
        superseded_ = false;
    }
    following_ = false;
    const int key = (int) std::lround(value(Key)), scale = (int) std::lround(value(Scale));
    const auto& progression = slot_.live().progression;
    if (wantLead)
    {
        if (ledChannel_ != channel)
        {
            if (link_.publish(channel, id_, key, scale, progression, true))
            {
                ledChannel_ = channel;
                superseded_ = false;
                publishedVersion_ = seenVersion_;
                lastKey_ = key;
                lastScale_ = scale;
            }
        }
        else if (! superseded_)
        {
            if (link_.leader(channel) != id_) superseded_ = true; // another instance took the channel over
            else if (seenVersion_ != publishedVersion_ || key != lastKey_ || scale != lastScale_)
                if (link_.publish(channel, id_, key, scale, progression, false))
                {
                    publishedVersion_ = seenVersion_;
                    lastKey_ = key;
                    lastScale_ = scale;
                }
        }
        following_ = superseded_;
    }
    if (mode == FromLink) following_ = true;
    if (following_)
    {
        if (channel < 0) linkData_ = {};
        else link_.read(channel, linkData_);
        linkSeen_.edit() = linkData_;
        linkSeen_.publish();
    }
    LinkStatus status = LinkStatus::Off;
    if (following_) status = linkData_.leader != 0 ? LinkStatus::Following : LinkStatus::NoLeader;
    else if (wantLead && ledChannel_ == channel) status = LinkStatus::Leading;
    linkStatus_.store((int) status, std::memory_order_relaxed);
}

void Phrase::heldChanged()
{
    std::array<int, kMaxHeld> pitches{};
    for (int i = 0; i < heldCount(); ++i) pitches[(size_t) i] = held(i).pitch;
    const auto tones = model::chordFromHeldNotes(pitches.data(), heldCount());
    if (tones == held_) return;
    held_ = tones;
    heldSince_ = currentBeat();
    heldPacked_.store(packChord(tones), std::memory_order_relaxed);
    if (std::lround(value(Chords)) == FromMidi) releaseSounding(); // the old chord does not ring on
}

void Phrase::located(double beat) { heldSince_ = beat; }

void Phrase::generate(double fromBeat, double toBeat, Sink& sink)
{
    if (auditionFresh_)
    {
        auditionFresh_ = false;
        for (int i = 0; i < auditionCount_; ++i) sink.note(fromBeat, 1.0, auditionPitches_[(size_t) i], 96);
        if (! host().playing) return; // stopped: only the audition sounds
    }
    const auto& s = slot_.live();
    model::PhraseLive l;
    l.part = (model::PhrasePart) std::clamp((int) std::lround(value(Part)), 0, 3);
    l.pattern = std::clamp((int) std::lround(value(Pattern)), 0, 8);
    l.key = (int) std::lround(value(Key));
    l.scale = (int) std::lround(value(Scale));
    l.tempo = host().tempo;
    l.density = value(Density);
    l.swing = value(Swing);
    l.gate = value(Gate);
    l.velocity = value(Velocity);
    l.accent = (int) std::lround(value(Accent));
    l.octave = (int) std::lround(value(Octave));
    l.shift = (int) std::lround(value(Shift));
    l.humanize = value(Humanize);
    l.kickGuard = value(KickGuard);
    l.seed = (uint32_t) std::lround(value(Seed));
    model::PhraseChords chords;
    if (std::lround(value(Chords)) == FromMidi)
    {
        chords.fromHeld = true;
        chords.held = held_;
        chords.heldSince = heldSince_;
    }
    else if (following_)
    {
        if (linkData_.leader == 0) return; // nobody leads the channel: rest (the face says so)
        chords.progression = &linkData_.progression;
        l.key = linkData_.key;
        l.scale = linkData_.scale;
    }
    else
        chords.progression = &s.progression;
    SinkAdapter adapter{sink};
    model::phraseRun(s, l, chords, fromBeat, toBeat, adapter);
}

// ------------------------------------------------------------------------------------------------------ state
std::vector<uint8_t> Phrase::saveState() const { return encodeState(state()); }

bool Phrase::loadState(const uint8_t* data, size_t size, std::string& error)
{
    model::PhraseState s;
    if (! decodeState(data, size, s, error)) return false;
    setState(s);
    return true;
}

namespace
{
constexpr uint32_t kMagic = 0x68506E53u; // "SnPh"
constexpr uint16_t kVersion = 1;

void writeChord(StateWriter& w, const model::Chord& c)
{
    w.u8((uint8_t) c.root);
    w.u8(c.chromatic ? 1 : 0);
    w.u8((uint8_t) c.quality);
    w.u8((uint8_t) c.extension);
    w.u8((uint8_t) c.inversion);
    w.u8((uint8_t) c.bass);
}
model::Chord readChord(StateReader& r)
{
    model::Chord c;
    c.root = (int8_t) r.u8();
    c.chromatic = r.u8() != 0;
    c.quality = (model::ChordQuality) std::min<int>(r.u8(), (int) model::ChordQuality::Count - 1);
    c.extension = (model::ChordExtension) std::min<int>(r.u8(), (int) model::ChordExtension::Count - 1);
    c.inversion = (int8_t) r.u8();
    c.bass = (int8_t) r.u8();
    return c;
}
template <class E> E readEnum(StateReader& r, E count) { return (E) std::min<int>(r.u8(), (int) count - 1); }
} // namespace

std::vector<uint8_t> Phrase::encodeState(const model::PhraseState& s)
{
    StateWriter w;
    w.u32(kMagic);
    w.u16(kVersion);
    const int count = std::clamp(s.progression.count, 1, model::Progression::kMaxSlots);
    w.u8((uint8_t) count);
    for (int i = 0; i < count; ++i)
    {
        writeChord(w, s.progression.slots[(size_t) i].chord);
        w.f64(s.progression.slots[(size_t) i].beats);
    }
    w.u8(s.keepChords ? 1 : 0);
    for (const auto& part : s.parts)
    {
        w.u8(part.formLength);
        for (auto f : part.form) w.u8(f);
        const auto& a = part.arp;
        for (uint8_t v : {(uint8_t) a.shape, (uint8_t) a.rhythm, (uint8_t) a.accents, (uint8_t) (a.accentGlide ? 1 : 0), a.tension, a.range,
                          a.passing, a.variation, a.bars, a.euclidHits})
            w.u8(v);
        for (uint8_t v : {(uint8_t) part.stabs.rhythm, part.stabs.hits, (uint8_t) part.stabs.contour, part.stabs.bars}) w.u8(v);
        for (uint8_t v : {(uint8_t) part.bass.rhythm, part.bass.octaveJumps, part.bass.fifths, part.bass.bars}) w.u8(v);
        w.u32(part.diceSeed);
        for (const auto& p : part.slots)
        {
            for (uint8_t v : {p.rate, p.length, (uint8_t) p.octave, p.voices, (uint8_t) p.voicing, p.strumMs, (uint8_t) (p.strumDown ? 1 : 0),
                              p.padVoices, (uint8_t) p.padVoicing, p.padCentre, p.colour, (uint8_t) p.padBass, (uint8_t) p.motion,
                              p.motionBars, p.overlap, p.padStrumMs, p.shimmer, p.padVelocity})
                w.u8(v);
            const int steps = std::clamp((int) p.length, 1, model::kPhraseSteps);
            for (int i = 0; i < steps; ++i)
            {
                const auto& st = p.steps[(size_t) i];
                w.u8((uint8_t) st.state);
                w.u8((uint8_t) st.tone);
                w.u8((uint8_t) st.scaleStep);
                w.u8((uint8_t) st.octave);
                w.u8(st.velocity);
                w.u16(st.gate);
                w.u8(st.chance);
                w.u8((uint8_t) ((st.accent ? 1 : 0) | (st.slide ? 2 : 0)));
                w.u8(st.rank);
                w.u8((uint8_t) st.top);
            }
        }
    }
    return w.take();
}

bool Phrase::decodeState(const uint8_t* data, size_t size, model::PhraseState& out, std::string& error)
{
    StateReader r(data, size);
    if (r.u32() != kMagic) { error = "not a Sonora Phrase state"; return false; }
    const auto version = r.u16();
    if (version > kVersion) { error = "saved by a newer Sonora Phrase (state version " + std::to_string(version) + ")"; return false; }
    model::PhraseState s;
    s.progression.count = std::clamp((int) r.u8(), 1, model::Progression::kMaxSlots);
    for (int i = 0; i < s.progression.count; ++i)
    {
        s.progression.slots[(size_t) i].chord = readChord(r);
        const double beats = r.f64();
        s.progression.slots[(size_t) i].beats = std::isfinite(beats) ? std::clamp(beats, 0.25, 512.0) : 8.0;
    }
    s.keepChords = r.u8() != 0;
    for (auto& part : s.parts)
    {
        part.formLength = (uint8_t) std::clamp((int) r.u8(), 1, model::kPhraseFormMax);
        for (auto& f : part.form) f = (uint8_t) std::min<int>(r.u8(), model::kPhraseSlots - 1);
        auto& a = part.arp;
        a.shape = readEnum(r, model::ArpShape::Count);
        a.rhythm = readEnum(r, model::ArpRhythm::Count);
        a.accents = readEnum(r, model::ArpAccents::Count);
        a.accentGlide = r.u8() != 0;
        a.tension = r.u8();
        a.range = r.u8();
        a.passing = r.u8();
        a.variation = r.u8();
        a.bars = r.u8();
        a.euclidHits = r.u8();
        part.stabs.rhythm = readEnum(r, model::StabRhythm::Count);
        part.stabs.hits = r.u8();
        part.stabs.contour = readEnum(r, model::StabContour::Count);
        part.stabs.bars = r.u8();
        part.bass.rhythm = readEnum(r, model::BassRhythm::Count);
        part.bass.octaveJumps = r.u8();
        part.bass.fifths = r.u8();
        part.bass.bars = r.u8();
        part.diceSeed = r.u32();
        for (auto& p : part.slots)
        {
            p.rate = (uint8_t) std::min<int>(r.u8(), (int) model::kGridRates.size() - 1);
            p.length = (uint8_t) std::clamp((int) r.u8(), 1, model::kPhraseSteps);
            p.octave = (int8_t) std::clamp((int) (int8_t) r.u8(), 0, 8);
            p.voices = (uint8_t) std::clamp((int) r.u8(), 3, 6);
            p.voicing = readEnum(r, model::VoicingStyle::Count);
            p.strumMs = (uint8_t) std::min<int>(r.u8(), 60);
            p.strumDown = r.u8() != 0;
            p.padVoices = (uint8_t) std::clamp((int) r.u8(), 3, 7);
            p.padVoicing = readEnum(r, model::VoicingStyle::Count);
            p.padCentre = (uint8_t) std::clamp((int) r.u8(), 36, 96);
            p.colour = (uint8_t) std::min<int>(r.u8(), 100);
            p.padBass = readEnum(r, model::PadBass::Count);
            p.motion = readEnum(r, model::PadMotion::Count);
            p.motionBars = (uint8_t) std::clamp((int) r.u8(), 1, 8);
            p.overlap = (uint8_t) std::min<int>(r.u8(), 100);
            p.padStrumMs = (uint8_t) std::min<int>(r.u8(), 250);
            p.shimmer = (uint8_t) std::min<int>(r.u8(), 100);
            p.padVelocity = (uint8_t) std::clamp((int) r.u8(), 1, 127);
            for (int i = 0; i < p.length; ++i)
            {
                auto& st = p.steps[(size_t) i];
                st.state = readEnum(r, (model::StepState) 3);
                st.tone = (int8_t) std::clamp((int) (int8_t) r.u8(), 0, 12);
                st.scaleStep = (int8_t) std::clamp((int) (int8_t) r.u8(), -7, 7);
                st.octave = (int8_t) std::clamp((int) (int8_t) r.u8(), -2, 2);
                st.velocity = (uint8_t) std::clamp((int) r.u8(), 1, 127);
                st.gate = (uint16_t) std::clamp((int) r.u16(), 5, 400);
                st.chance = (uint8_t) std::min<int>(r.u8(), 100);
                const auto flags = r.u8();
                st.accent = (flags & 1) != 0;
                st.slide = (flags & 2) != 0;
                st.rank = r.u8();
                st.top = (int8_t) std::clamp((int) (int8_t) r.u8(), 0, 12);
            }
        }
    }
    if (! r.ok()) { error = "the Sonora Phrase state is cut short"; return false; }
    out = s;
    return true;
}

// ------------------------------------------------------------------------------------------------------ presets
void Phrase::applyPreset(int index)
{
    const auto& list = presets();
    if (index < 0 || index >= (int) list.size()) return;
    const auto& preset = list[(size_t) index];
    setParameter(Part, (float) preset.part);
    for (const auto& [param, v] : preset.values) setParameter(param, v);
    auto s = state();
    const auto progression = s.progression;
    const bool keep = s.keepChords || std::lround(getParameter(Chords)) != FromProgression;
    if (preset.build) preset.build(s);
    if (keep) s.progression = progression;
    s.keepChords = keep && state().keepChords;
    setState(s);
    presetIndex_ = index;
    presetState_ = std::make_shared<model::PhraseState>(s);
    presetValues_.assign((size_t) NumParams, 0.0f);
    for (int i = 0; i < NumParams; ++i) presetValues_[(size_t) i] = getParameter(i);
}

int Phrase::matchingPreset() const
{
    if (presetIndex_ < 0 || ! presetState_) return -1;
    if (! (state().part(part()) == presetState_->part(part()))) return -1;
    const auto& preset = presets()[(size_t) presetIndex_];
    if (std::lround(getParameter(Part)) != (long) preset.part) return -1;
    for (const auto& [param, v] : preset.values)
        if (std::abs(getParameter(param) - presetValues_[(size_t) param]) > 1e-4f) return -1;
    return presetIndex_;
}
} // namespace sonora::devices
