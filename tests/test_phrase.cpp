// PLG-25: Sonora Phrase. Engine (core/PhraseEngine), harmony link (core/HarmonyLink) and, below, the device.

#include "TestFramework.h"

#include "core/HarmonyLink.h"
#include "core/PhraseEngine.h"
#include "core/Scales.h"

#include <algorithm>
#include <map>
#include <random>
#include <set>
#include <thread>
#include <vector>

using namespace sonora;
using namespace sonora::model;

namespace
{
constexpr int kMinor = 1, kPhrygian = 5;

struct Collect
{
    std::vector<PhraseEvent> events;
    void note(double beat, double length, int pitch, int velocity) { events.push_back({beat, length, pitch, velocity}); }
};

std::vector<PhraseEvent> render(const PhraseState& s, const PhraseLive& l, const PhraseChords& c, double from, double to)
{
    Collect sink;
    phraseRun(s, l, c, from, to, sink);
    std::stable_sort(sink.events.begin(), sink.events.end(), [](const PhraseEvent& a, const PhraseEvent& b) {
        return a.beat < b.beat - 1e-12 || (std::abs(a.beat - b.beat) <= 1e-12 && a.pitch < b.pitch);
    });
    return sink.events;
}

bool same(const std::vector<PhraseEvent>& a, const std::vector<PhraseEvent>& b, double shift = 0)
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::abs(a[i].beat + shift - b[i].beat) > 1e-9 || std::abs(a[i].length - b[i].length) > 1e-9 || a[i].pitch != b[i].pitch
            || a[i].velocity != b[i].velocity)
            return false;
    return true;
}

Progression progression(std::initializer_list<std::pair<int, double>> slots)
{
    Progression p;
    p.count = 0;
    for (const auto& [degree, beats] : slots) p.slots[(size_t) p.count++] = {Chord::degree(degree), beats};
    return p;
}

PhraseLive liveFor(PhrasePart part, int key = 9, int scale = kMinor)
{
    PhraseLive l;
    l.part = part;
    l.key = key;
    l.scale = scale;
    return l;
}

PhraseChords chordsOf(const Progression& p)
{
    PhraseChords c;
    c.progression = &p;
    return c;
}

PhrasePattern plain(int length, std::initializer_list<int> notes, int tone = 0)
{
    PhrasePattern p;
    p.length = (uint8_t) length;
    for (int i : notes)
    {
        p.steps[(size_t) i].state = StepState::Note;
        p.steps[(size_t) i].tone = (int8_t) tone;
        p.steps[(size_t) i].gate = 50;
        p.steps[(size_t) i].velocity = 100;
    }
    phraseRank(p, 1);
    return p;
}

std::set<int> pitchClasses(const std::vector<PhraseEvent>& events, double from, double to)
{
    std::set<int> out;
    for (const auto& e : events)
        if (e.beat >= from && e.beat < to) out.insert(e.pitch % 12);
    return out;
}
} // namespace

TEST_CASE("phrase engine: one span, beat-sized spans and random spans give the same notes; the form loops exactly")
{
    auto state = defaultPhraseState();
    for (int p = 0; p < (int) PhrasePart::Count; ++p)
    {
        auto live = liveFor((PhrasePart) p);
        live.swing = 0.4f;
        live.humanize = 0.5f;
        const auto chords = chordsOf(state.progression);
        const auto whole = render(state, live, chords, 0, 64);
        CHECK(! whole.empty());
        std::mt19937 random(7 + (unsigned) p);
        std::vector<PhraseEvent> pieces;
        for (double from = 0; from < 64;)
        {
            const double to = std::min(64.0, from + (random() % 1000) / 997.0 + 0.0001);
            const auto part = render(state, live, chords, from, to);
            pieces.insert(pieces.end(), part.begin(), part.end());
            from = to;
        }
        std::stable_sort(pieces.begin(), pieces.end(), [](const PhraseEvent& a, const PhraseEvent& b) {
            return a.beat < b.beat - 1e-12 || (std::abs(a.beat - b.beat) <= 1e-12 && a.pitch < b.pitch);
        });
        CHECK(same(whole, pieces));
        // Without chance and humanize the music repeats with the least common lap of form and progression.
        live.humanize = 0;
        const auto lap = render(state, live, chords, 0, 32), next = render(state, live, chords, 32, 64);
        CHECK(same(lap, next, 32.0));
    }
}

TEST_CASE("phrase engine: an arp follows the chord; a tie stops at a chord change")
{
    PhraseState state;
    state.progression = progression({{0, 4}, {5, 4}}); // Am, F in A minor
    auto& arp = state.part(PhrasePart::Arp);
    arp.slots[0] = plain(32, {0, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 22, 24, 26, 28, 30});
    for (int i = 0; i < 32; i += 2) arp.slots[0].steps[(size_t) i].tone = (int8_t) ((i / 2) % 3);
    const auto live = liveFor(PhrasePart::Arp);
    const auto events = render(state, live, chordsOf(state.progression), 0, 8);
    CHECK(pitchClasses(events, 0, 4) == std::set<int>({9, 0, 4}));
    CHECK(pitchClasses(events, 4, 8) == std::set<int>({5, 9, 0}));
    for (const auto& e : events) CHECK(e.pitch >= 69 && e.pitch < 69 + 24); // A4 and up
    // A note on step 14 tied over the change at beat 4: it ends there (its own gate is shorter).
    arp.slots[0] = plain(32, {14});
    for (int i = 15; i < 20; ++i) arp.slots[0].steps[(size_t) i].state = StepState::Tie;
    const auto tied = render(state, live, chordsOf(state.progression), 0, 8);
    REQUIRE(tied.size() == 1);
    CHECK_NEAR(tied[0].beat + tied[0].length, 4.0, 1e-9);
    // Without the change it lasts its ties.
    state.progression = progression({{0, 8}});
    const auto held = render(state, live, chordsOf(state.progression), 0, 8);
    REQUIRE(held.size() == 1);
    CHECK_NEAR(held[0].length, 5 * 0.25 + 0.5 * 0.25, 1e-9);
}

TEST_CASE("phrase engine: density brings steps in by rank, monotonic; 0 % is silence")
{
    auto state = defaultPhraseState();
    const auto live0 = liveFor(PhrasePart::Arp);
    const auto chords = chordsOf(state.progression);
    std::vector<std::set<double>> sets;
    for (float d : {0.0f, 0.2f, 0.4f, 0.6f, 0.8f, 1.0f})
    {
        auto live = live0;
        live.density = d;
        std::set<double> beats;
        for (const auto& e : render(state, live, chords, 0, 16)) beats.insert(std::round(e.beat * 1e6) / 1e6);
        sets.push_back(beats);
    }
    CHECK(sets[0].empty());
    for (size_t i = 1; i < sets.size(); ++i)
    {
        CHECK(sets[i].size() >= sets[i - 1].size());
        for (double b : sets[i - 1]) CHECK(sets[i].count(b) == 1);
    }
    // Rank order: at a low density the bar downbeats are in.
    auto low = live0;
    low.density = 0.1f;
    const auto skeleton = render(state, low, chords, 0, 16);
    REQUIRE(! skeleton.empty());
    CHECK(std::abs(std::fmod(skeleton.front().beat, 4.0)) < 1e-9);
}

TEST_CASE("phrase engine: Kick Guard scales the beat's sixteenth and mutes it at 100 %")
{
    PhraseState state;
    state.progression = progression({{0, 16}});
    state.part(PhrasePart::Arp).slots[0] = plain(16, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15});
    auto live = liveFor(PhrasePart::Arp);
    live.kickGuard = 0.5f;
    for (const auto& e : render(state, live, chordsOf(state.progression), 0, 4))
        CHECK(e.velocity == (std::abs(std::fmod(e.beat, 1.0)) < 1e-9 ? 50 : 100));
    live.kickGuard = 1.0f;
    const auto guarded = render(state, live, chordsOf(state.progression), 0, 4);
    CHECK(guarded.size() == 12);
    for (const auto& e : guarded) CHECK(std::abs(std::fmod(e.beat, 1.0)) > 1e-9);
}

TEST_CASE("phrase generators: every part and style over 20 seeds stays in the scale and register, keeps its rhythm, asks")
{
    for (int scale : {kMinor, kPhrygian})
        for (int p = 0; p < (int) PhrasePart::Count; ++p)
        {
            const auto part = (PhrasePart) p;
            const int styles = part == PhrasePart::Arp ? (int) ArpShape::Count * (int) ArpRhythm::Count
                               : part == PhrasePart::Stabs ? (int) StabRhythm::Count * (int) StabContour::Count
                               : part == PhrasePart::Bass ? (int) BassRhythm::Count : 1;
            for (int style = 0; style < styles; ++style)
                for (uint32_t seed = 1; seed <= 20; ++seed)
                {
                    auto state = defaultPhraseState();
                    state.progression = progression({{0, 8}, {5, 8}, {6, 8}, {0, 8}});
                    auto& ps = state.part(part);
                    ps.formLength = 1;
                    if (part == PhrasePart::Arp)
                    {
                        ps.arp.shape = (ArpShape) (style % (int) ArpShape::Count);
                        ps.arp.rhythm = (ArpRhythm) (style / (int) ArpShape::Count);
                        ps.arp.passing = (uint8_t) (seed % 2 == 0 ? 0 : 40);
                        ps.arp.bars = (uint8_t) (1 << (seed % 3));
                        ps.arp.tension = 50;
                    }
                    if (part == PhrasePart::Stabs)
                    {
                        ps.stabs.rhythm = (StabRhythm) (style % (int) StabRhythm::Count);
                        ps.stabs.contour = (StabContour) (style / (int) StabRhythm::Count);
                        ps.stabs.hits = (uint8_t) (2 + seed % 5);
                    }
                    if (part == PhrasePart::Bass) ps.bass.rhythm = (BassRhythm) style;
                    phraseGenerate(state, part, 0, seed);
                    const auto& pat = ps.slots[0];
                    auto live = liveFor(part, 4, scale);
                    const auto events = render(state, live, chordsOf(state.progression), 0, 32);
                    CHECK(! events.empty());
                    for (const auto& e : events)
                    {
                        CHECK(inScale(e.pitch, 4, scale));
                        if (part == PhrasePart::Bass) CHECK(e.pitch >= 28 && e.pitch <= 39 + 12 + 7); // E1..D#2, octave jumps, fifths
                        if (part == PhrasePart::Arp) CHECK(e.pitch >= 64 - 2 && e.pitch < 64 + 12 * 5); // a passing step below the root
                        if (part == PhrasePart::Stabs) CHECK(e.pitch >= 36 && e.pitch <= 96 + 12);
                    }
                    if (part == PhrasePart::Pad) continue;
                    // Rhythm: every onset is a note step of the pattern.
                    for (const auto& e : events)
                    {
                        const double position = std::fmod(e.beat, pat.lengthBeats()) / pat.stepBeats();
                        const int step = (int) std::lround(position);
                        CHECK(std::abs(position - step) < 1e-6);
                        CHECK(pat.steps[(size_t) (step % pat.length)].state == StepState::Note);
                    }
                    // Bass leaves the kick's sixteenth empty.
                    if (part == PhrasePart::Bass)
                        for (const auto& e : events) CHECK(grid::positionIn(e.beat, 1.0) >= 0.25 - 1e-9);
                    // Arp without passing and tension: chord tones only.
                    if (part == PhrasePart::Arp && ps.arp.passing == 0 && ps.arp.shape != ArpShape::PedalLow)
                    {
                        auto noTension = state;
                        noTension.part(part).arp.tension = 0;
                        phraseGenerate(noTension, part, 0, seed);
                        for (const auto& e : render(noTension, live, chordsOf(noTension.progression), 0, 8))
                            CHECK(chordTones(Chord::degree(0), 4, scale).contains(e.pitch % 12));
                    }
                    // The question: its last note is off the root, on a weak step.
                    int last = -1;
                    for (int i = 0; i < pat.length; ++i)
                        if (pat.steps[(size_t) i].state == StepState::Note) last = i;
                    REQUIRE(last >= 0);
                    if (part == PhrasePart::Arp) CHECK(pat.steps[(size_t) last].tone % 3 != 0);
                    if (part == PhrasePart::Stabs) CHECK(pat.steps[(size_t) last].top % 3 != 0);
                    if (part != PhrasePart::Bass) CHECK(grid::positionIn(last * pat.stepBeats(), 1.0) > 1e-9);
                    // The answer closes on the root on a strong step and keeps the first half's rhythm.
                    phraseAnswer(state, part, 0, 1);
                    const auto& answer = ps.slots[1];
                    for (int i = 0; i < pat.length / 2; ++i) CHECK(answer.steps[(size_t) i].state == pat.steps[(size_t) i].state);
                    int close = -1;
                    for (int i = 0; i < answer.length; ++i)
                        if (answer.steps[(size_t) i].state == StepState::Note) close = i;
                    REQUIRE(close >= 0);
                    if (part != PhrasePart::Bass)
                    {
                        CHECK(grid::positionIn(close * answer.stepBeats(), 1.0) < 1e-9);
                        if (part == PhrasePart::Stabs) CHECK(answer.steps[(size_t) close].top % 3 == 0);
                        else CHECK(answer.steps[(size_t) close].tone == 0 && answer.steps[(size_t) close].scaleStep == 0);
                    }
                }
        }
}

TEST_CASE("phrase stabs: the top lane is the highest note, voices count and strum")
{
    PhraseState state;
    state.progression = progression({{0, 8}, {5, 8}});
    auto& stabs = state.part(PhrasePart::Stabs);
    stabs.slots[0] = plain(16, {2, 6, 10, 14});
    for (int i : {2, 6, 10, 14}) stabs.slots[0].steps[(size_t) i].top = (int8_t) ((i / 4) % 3);
    for (int voices : {3, 4, 5, 6})
    {
        stabs.slots[0].voices = (uint8_t) voices;
        stabs.slots[0].strumMs = 0;
        const auto live = liveFor(PhrasePart::Stabs);
        const auto events = render(state, live, chordsOf(state.progression), 0, 16);
        std::map<long, std::vector<int>> groups;
        for (const auto& e : events) groups[std::lround(e.beat * 1000)].push_back(e.pitch);
        CHECK(groups.size() == 16);
        for (const auto& [atMs, pitches] : groups)
        {
            CHECK((int) pitches.size() == voices);
            const double at = (double) atMs / 1000.0;
            const int step = (int) std::lround(std::fmod(at, 4.0) / 0.25);
            const int top = stabs.slots[0].steps[(size_t) step].top;
            const auto tones = chordTones(state.progression.chordAt(at), 9, kMinor);
            CHECK(*std::max_element(pitches.begin(), pitches.end()) % 12 == tones.pitchClass(top));
        }
    }
    // Strum: the onsets of one chord spread over the strum time, low to high (down: high to low).
    stabs.slots[0].voices = 4;
    stabs.slots[0].strumMs = 40;
    auto live = liveFor(PhrasePart::Stabs);
    live.tempo = 120;
    auto events = render(state, live, chordsOf(state.progression), 0, 1);
    REQUIRE(events.size() == 4);
    CHECK_NEAR(events.back().beat - events.front().beat, 0.040 * 2.0, 1e-9);
    for (size_t i = 1; i < events.size(); ++i) CHECK(events[i].pitch > events[i - 1].pitch);
    stabs.slots[0].strumDown = true;
    events = render(state, live, chordsOf(state.progression), 0, 1);
    for (size_t i = 1; i < events.size(); ++i) CHECK(events[i].pitch < events[i - 1].pitch);
}

TEST_CASE("phrase pad: common tones held in Hold, Breathe moves one voice, overlap, colour avoids minor ninths")
{
    PhraseState state;
    state.progression = progression({{0, 8}, {5, 8}}); // Am, F: A and C in common
    auto& pad = state.part(PhrasePart::Pad);
    pad.slots[0].rate = (uint8_t) kRate1_1;
    pad.slots[0].length = 4;
    pad.slots[0].colour = 0;
    pad.slots[0].padBass = PadBass::Off;
    pad.slots[0].padVoicing = VoicingStyle::Close;
    pad.slots[0].padVoices = 3;
    pad.slots[0].overlap = 50;
    pad.slots[0].shimmer = 0;
    const auto live = liveFor(PhrasePart::Pad);
    auto events = render(state, live, chordsOf(state.progression), 0, 16);
    int across = 0, struckAt8 = 0;
    for (const auto& e : events)
    {
        if (e.beat < 1e-9 && e.beat + e.length > 16 - 1e-9) ++across; // held through both chords
        if (std::abs(e.beat - 8) < 1e-9) ++struckAt8;
    }
    CHECK(across == 2);
    CHECK(struckAt8 == 1);
    for (const auto& e : events) CHECK_NEAR(std::fmod(e.beat + e.length, 8.0), 0.5, 1e-9); // ends overlap by half a beat
    // Breathe: every bar inside a chord, exactly one voice changes.
    state.progression = progression({{0, 16}});
    pad.slots[0].motion = PadMotion::Breathe;
    pad.slots[0].motionBars = 1;
    pad.slots[0].padVoices = 5;
    pad.slots[0].overlap = 0;
    events = render(state, live, chordsOf(state.progression), 0, 16);
    auto sounding = [&](double at) {
        std::set<int> s;
        for (const auto& e : events)
            if (e.beat <= at + 1e-9 && e.beat + e.length > at + 1e-6) s.insert(e.pitch);
        return s;
    };
    for (double bar : {4.0, 8.0, 12.0})
    {
        const auto before = sounding(bar - 0.01), after = sounding(bar + 0.01);
        std::vector<int> gone, added;
        std::set_difference(before.begin(), before.end(), after.begin(), after.end(), std::back_inserter(gone));
        std::set_difference(after.begin(), after.end(), before.begin(), before.end(), std::back_inserter(added));
        CHECK(gone.size() == 1);
        CHECK(added.size() == 1);
    }
    // Colour tones: never a minor ninth above a chord tone.
    pad.slots[0].motion = PadMotion::Hold;
    pad.slots[0].colour = 100;
    state.progression = progression({{0, 4}, {3, 4}, {5, 4}, {6, 4}, {4, 4}, {2, 4}});
    int coloured = 0;
    for (uint32_t seed = 1; seed <= 30; ++seed)
    {
        auto l = live;
        l.seed = seed;
        events = render(state, l, chordsOf(state.progression), 0, 24);
        for (int slot = 0; slot < 6; ++slot)
        {
            const auto tones = chordTones(state.progression.slots[(size_t) slot].chord, 9, kMinor);
            for (const auto& e : events)
            {
                if (std::abs(e.beat - slot * 4.0) > 1e-9 || tones.contains(e.pitch % 12)) continue;
                ++coloured;
                CHECK(inScale(e.pitch, 9, kMinor));
                for (int i = 0; i < tones.count; ++i) CHECK((e.pitch - tones.root - tones.intervals[(size_t) i] + 120) % 12 != 1);
            }
        }
    }
    CHECK(coloured > 30);
}

TEST_CASE("phrase answer options and mutate")
{
    PhraseState state;
    auto& arp = state.part(PhrasePart::Arp);
    arp.slots[0] = plain(16, {0, 2, 4, 6, 8, 10, 12, 14});
    for (int i = 0; i < 16; i += 2) arp.slots[0].steps[(size_t) i].tone = (int8_t) (i / 2 % 4);
    phraseAnswer(state, PhrasePart::Arp, 0, 1, AnswerKind::Mirror);
    CHECK(arp.slots[1].steps[2].tone == 0); // 2 * 0 - 1, clamped
    arp.slots[0].steps[0].tone = 3;
    phraseAnswer(state, PhrasePart::Arp, 0, 1, AnswerKind::Mirror);
    CHECK(arp.slots[1].steps[4].tone == 4); // 2 * 3 - 2
    phraseAnswer(state, PhrasePart::Arp, 0, 2, AnswerKind::Shift);
    for (int i = 0; i < 16; ++i)
    {
        CHECK(arp.slots[2].steps[(size_t) ((i + 2) % 16)].state == arp.slots[0].steps[(size_t) i].state); // an eighth later
        CHECK(arp.slots[2].steps[(size_t) ((i + 2) % 16)].tone == arp.slots[0].steps[(size_t) i].tone);
    }
    phraseAnswer(state, PhrasePart::Arp, 0, 3, AnswerKind::Sparse);
    int firstHalf = 0, secondHalf = 0;
    for (int i = 0; i < 16; ++i)
        if (arp.slots[3].steps[(size_t) i].state == StepState::Note) (i < 8 ? firstHalf : secondHalf)++;
    CHECK(firstHalf == 4);
    CHECK(secondHalf == 2);
    phraseAnswer(state, PhrasePart::Arp, 0, 4, AnswerKind::Counter);
    int notes = 0;
    for (int i = 0; i < 16; ++i)
        if (arp.slots[4].steps[(size_t) i].state == StepState::Note)
        {
            ++notes;
            CHECK(i % 2 == 1); // a sixteenth later
            CHECK(arp.slots[4].steps[(size_t) i].scaleStep == -4);
        }
    CHECK(notes == 4);
    // Mutate: about one step in eight.
    double changed = 0;
    for (uint32_t seed = 1; seed <= 40; ++seed)
    {
        auto copy = state;
        copy.part(PhrasePart::Arp).slots[0] = plain(16, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15});
        const auto before = copy.part(PhrasePart::Arp).slots[0];
        phraseMutate(copy, PhrasePart::Arp, 0, seed);
        for (int i = 0; i < 16; ++i)
        {
            auto a = before.steps[(size_t) i], b = copy.part(PhrasePart::Arp).slots[0].steps[(size_t) i];
            a.rank = b.rank = 0;
            changed += ! (a == b);
        }
    }
    CHECK(changed / (40 * 16) > 0.06);
    CHECK(changed / (40 * 16) < 0.2);
}

TEST_CASE("phrase engine: MIDI In plays the held chord, pads strike when it began; factory progressions")
{
    auto state = defaultPhraseState();
    PhraseChords chords;
    chords.fromHeld = true;
    const int held[] = {57, 60, 64};
    chords.held = chordFromHeldNotes(held, 3);
    auto live = liveFor(PhrasePart::Arp);
    CHECK(pitchClasses(render(state, live, chords, 0, 8), 0, 8).size() >= 2);
    for (int pc : pitchClasses(render(state, live, chords, 0, 8), 0, 8)) CHECK(inScale(pc, 9, kMinor));
    chords.held = {};
    CHECK(render(state, live, chords, 0, 8).empty()); // nothing held: rest
    chords.held = chordFromHeldNotes(held, 3);
    chords.heldSince = 5.25;
    live.part = PhrasePart::Pad;
    const auto pad = render(state, live, chords, 0, 16);
    REQUIRE(! pad.empty());
    CHECK_NEAR(pad.front().beat, 5.25, 0.05);
    CHECK(factoryProgressionCount() == 21);
    CHECK(factoryProgressionName(0) == "i - bVI - bVII - i, 2 bars each");
    CHECK(factoryProgressionName(20) == "i (drone), 16 bars");
    const auto phrygian = factoryProgression(13);
    CHECK(phrygian.count == 2 && phrygian.slots[1].chord.chromatic);
    CHECK_NEAR(phrygian.slots[0].beats, 16.0, 1e-9);
}

TEST_CASE("harmony link: lead, follow, take over, release; readers get whole copies")
{
    HarmonyLink link;
    const auto a = HarmonyLink::newId(), b = HarmonyLink::newId();
    HarmonyLinkPayload p;
    CHECK(link.read(0, p));
    CHECK(p.leader == 0);
    auto prog = progression({{0, 8}, {5, 8}});
    CHECK(link.publish(1, a, 4, kPhrygian, prog, true));
    CHECK(link.read(1, p));
    CHECK(p.leader == a);
    CHECK(p.key == 4 && p.scale == kPhrygian && p.progression == prog);
    // B takes over; A may no longer write without claiming.
    CHECK(link.publish(1, b, 7, kMinor, prog, true));
    CHECK(link.leader(1) == b);
    CHECK(! link.publish(1, a, 9, kMinor, prog, false));
    CHECK(link.read(1, p) && p.key == 7);
    link.release(1, a); // not the leader: nothing happens
    CHECK(link.leader(1) == b);
    link.release(1, b);
    CHECK(link.leader(1) == 0);
    CHECK(! link.publish(7, a, 0, 0, prog, true));
    // A writer and a reader at once: every copy is whole (progression count matches its key).
    std::atomic<bool> stop{false};
    std::thread writer([&] {
        for (int i = 0; ! stop.load(); ++i)
        {
            Progression q;
            q.count = 1 + i % 16;
            for (int s = 0; s < q.count; ++s) q.slots[(size_t) s] = {Chord::degree(i % 7), (double) q.count};
            link.publish(2, a, q.count, kMinor, q, true);
        }
    });
    bool whole = true;
    int reads = 0;
    for (int i = 0; i < 100000; ++i)
        if (link.read(2, p) && p.leader == a)
        {
            ++reads;
            whole &= p.progression.count == p.key;
            for (int s = 0; s < p.progression.count; ++s) whole &= std::abs(p.progression.slots[(size_t) s].beats - p.key) < 1e-12;
        }
    stop = true;
    writer.join();
    CHECK(whole);
    CHECK(reads > 0);
}

// ================================================================================================== the device
#include "core/DeviceCatalog.h"
#include "core/Edit.h"
#include "core/GeneratorClip.h"
#include "devices/DeviceFactory.h"
#include "devices/native/Phrase.h"
#include "io/als/AlsNewSet.h"
#include "io/als/AlsReader.h"
#include "io/als/AlsTemplates.h"
#include "io/als/AlsWriter.h"
#include "plugins/SonoraPluginState.h"

#include <juce_audio_basics/juce_audio_basics.h>

namespace
{
using devices::Phrase;

struct Played
{
    int64_t at;
    bool on;
    int pitch, velocity;
};

// Drives a device like the engine: host info per block (the loop wraps inside a block), then process().
struct DeviceRig
{
    std::unique_ptr<Phrase> owned;
    Phrase& phrase;
    double rate, tempo = 138, beat = 0;
    bool playing = true, looping = false;
    double loopStart = 0, loopEnd = 0;
    int64_t clock = 0;
    std::function<int()> blocks = [] { return 512; };
    std::vector<std::pair<int64_t, juce::MidiMessage>> input;
    std::vector<Played> out;

    explicit DeviceRig(double sampleRate = 48000, HarmonyLink* link = nullptr)
        : owned(std::make_unique<Phrase>(link ? *link : HarmonyLink::shared())), phrase(*owned), rate(sampleRate)
    {
        phrase.prepare(rate, 4096);
    }
    double perBeat() const { return rate * 60.0 / tempo; }
    void run(int64_t until)
    {
        juce::AudioBuffer<float> audio(2, 4096);
        juce::MidiBuffer midi;
        midi.ensureSize(65536);
        while (clock < until)
        {
            const int n = (int) std::min<int64_t>(std::clamp(blocks(), 1, 4096), until - clock);
            juce::AudioBuffer<float> view(audio.getArrayOfWritePointers(), 2, n);
            midi.clear();
            for (const auto& [at, message] : input)
                if (at >= clock && at < clock + n) midi.addEvent(message, (int) (at - clock));
            devices::HostInfo info;
            info.tempo = tempo;
            info.beats = beat;
            info.playing = playing;
            info.sampleRate = rate;
            info.looping = looping;
            info.loopStart = loopStart;
            info.loopEnd = loopEnd;
            phrase.setHostInfo(info);
            phrase.process(view, midi);
            for (const auto meta : midi)
            {
                const auto m = meta.getMessage();
                if (m.isNoteOnOrOff()) out.push_back({clock + meta.samplePosition, m.isNoteOn(), m.getNoteNumber(), m.getVelocity()});
            }
            clock += n;
            if (playing)
            {
                beat += n / perBeat();
                if (looping && beat >= loopEnd - 1e-9) beat = loopStart + (beat - loopEnd);
            }
        }
    }
    std::vector<Played> ons() const
    {
        std::vector<Played> r;
        for (const auto& e : out)
            if (e.on) r.push_back(e);
        std::stable_sort(r.begin(), r.end(), [](const Played& a, const Played& b) { return a.at < b.at || (a.at == b.at && a.pitch < b.pitch); });
        return r;
    }
    bool balanced() const
    {
        std::map<int, int> depth;
        for (const auto& e : out)
        {
            auto& d = depth[e.pitch];
            if (e.on) { if (++d > 1) return false; }
            else if (--d < 0) return false;
        }
        for (const auto& [pitch, d] : depth)
            if (d != 0) return false;
        return true;
    }
};

bool sameOns(const std::vector<Played>& a, const std::vector<Played>& b)
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::llabs(a[i].at - b[i].at) > 1 || a[i].pitch != b[i].pitch || a[i].velocity != b[i].velocity) return false;
    return true;
}
} // namespace

TEST_CASE("phrase device: the same notes in one block, random blocks at 44.1 / 48 / 96 kHz and after a locate, every part")
{
    for (int part = 0; part < 4; ++part)
        for (double rate : {44100.0, 48000.0, 96000.0})
        {
            auto make = [&](std::function<int()> sizes) {
                auto rig = std::make_unique<DeviceRig>(rate);
                rig->phrase.setParameter(Phrase::Part, (float) part);
                rig->phrase.setParameter(Phrase::Swing, 0.3f);
                rig->phrase.setParameter(Phrase::Humanize, 0.5f);
                rig->blocks = std::move(sizes);
                return rig;
            };
            const auto length = (int64_t) std::ceil(32 * rate * 60 / 138);
            auto whole = make([] { return 4096; });
            whole->run(length);
            whole->playing = false;
            whole->run(length + 30000);
            CHECK(whole->balanced());
            std::mt19937 random(5);
            auto pieces = make([&] { return (int) (random() % 4096) + 1; });
            pieces->run(length);
            CHECK(whole->ons().size() > 8);
            CHECK(sameOns(whole->ons(), pieces->ons()));
            // A locate to beat 16 plays what beat 16 plays in a continuous run.
            auto jumped = make([] { return 700; });
            jumped->beat = 16;
            jumped->run(length / 2);
            std::vector<Played> expected;
            const auto offset = (int64_t) std::floor(16 * whole->perBeat() + 1e-8);
            for (auto e : whole->ons())
                if (e.at >= offset && e.at < offset + length / 2 - 2000) { e.at -= offset; expected.push_back(e); }
            auto got = jumped->ons();
            got.resize(std::min(got.size(), expected.size()));
            CHECK(sameOns(expected, got));
        }
}

TEST_CASE("phrase device: a follower plays the leader's chords; a new leader takes over; no leader rests")
{
    HarmonyLink link;
    DeviceRig leader(48000, &link), follower(48000, &link), other(48000, &link);
    auto leaderState = leader.phrase.state();
    leaderState.progression = progression({{3, 16}}); // iv only: D minor over A minor
    leader.phrase.setState(leaderState);
    leader.phrase.setParameter(Phrase::Key, 9);
    leader.phrase.setParameter(Phrase::LinkChannel, 1);
    follower.phrase.setParameter(Phrase::Chords, Phrase::FromLink);
    follower.phrase.setParameter(Phrase::LinkChannel, 1);
    follower.phrase.setParameter(Phrase::Key, 0); // its own key does not matter while following
    leader.run(2048);
    CHECK(leader.phrase.linkStatus() == Phrase::LinkStatus::Leading);
    follower.run(48000 * 4);
    CHECK(follower.phrase.linkStatus() == Phrase::LinkStatus::Following);
    CHECK(follower.phrase.keyInUse() == 9);
    std::set<int> classes;
    for (const auto& e : follower.ons()) classes.insert(e.pitch % 12);
    CHECK(! classes.empty());
    for (int pc : classes) CHECK(inScale(pc, 9, kMinor));
    CHECK(classes.count(2) == 1); // D, the root of iv
    // The leader edits its progression: the follower hears it at its next block.
    leaderState.progression = progression({{5, 16}}); // VI: F
    leader.phrase.setState(leaderState);
    leader.run(4096);
    follower.out.clear();
    follower.run(follower.clock + 48000 * 4);
    classes.clear();
    for (const auto& e : follower.ons()) classes.insert(e.pitch % 12);
    CHECK(classes.count(5) == 1);
    // Another instance leads channel A: the first one follows it now.
    auto otherState = other.phrase.state();
    otherState.progression = progression({{6, 16}});
    other.phrase.setState(otherState);
    other.phrase.setParameter(Phrase::Key, 9);
    other.phrase.setParameter(Phrase::LinkChannel, 1);
    other.run(2048);
    leader.run(leader.clock + 2048);
    CHECK(other.phrase.linkStatus() == Phrase::LinkStatus::Leading);
    CHECK(leader.phrase.linkStatus() == Phrase::LinkStatus::Following);
    CHECK(leader.phrase.progressionInUse() == otherState.progression);
    // Nobody leads channel B: a follower there rests and says so.
    DeviceRig lonely(48000, &link);
    lonely.phrase.setParameter(Phrase::Chords, Phrase::FromLink);
    lonely.phrase.setParameter(Phrase::LinkChannel, 2);
    lonely.run(48000 * 2);
    CHECK(lonely.phrase.linkStatus() == Phrase::LinkStatus::NoLeader);
    CHECK(lonely.ons().empty());
    // A leader that is removed frees its channel.
    { DeviceRig temporary(48000, &link); temporary.phrase.setParameter(Phrase::LinkChannel, 4); temporary.run(1024); CHECK(link.leader(3) != 0); }
    CHECK(link.leader(3) == 0);
}

TEST_CASE("phrase device: MIDI In plays the held triad, then another; Latch keeps it; the old chord does not ring on")
{
    DeviceRig rig;
    rig.phrase.setParameter(Phrase::Chords, Phrase::FromMidi);
    rig.phrase.setParameter(Phrase::Part, (float) PhrasePart::Pad);
    for (int pitch : {57, 60, 64}) rig.input.push_back({0, juce::MidiMessage::noteOn(1, pitch, (juce::uint8) 100)});
    const auto bar = (int64_t) (4 * rig.perBeat());
    for (int pitch : {57, 60, 64}) rig.input.push_back({2 * bar, juce::MidiMessage::noteOff(1, pitch)});
    for (int pitch : {53, 57, 60}) rig.input.push_back({2 * bar, juce::MidiMessage::noteOn(1, pitch, (juce::uint8) 100)});
    for (int pitch : {53, 57, 60}) rig.input.push_back({4 * bar, juce::MidiMessage::noteOff(1, pitch)});
    rig.run(5 * bar);
    std::set<int> first, second;
    for (const auto& e : rig.ons()) (e.at < 2 * bar ? first : second).insert(e.pitch % 12);
    CHECK(first.count(9) && first.count(0) && first.count(4));
    CHECK(second.count(5) && second.count(9) && second.count(0));
    CHECK(second.count(4) == 0);
    // Nothing sounds after the release (no latch), and every note ended.
    for (const auto& e : rig.ons()) CHECK(e.at < 4 * bar);
    CHECK(rig.balanced());
    // Every note of the first chord ended at the change.
    for (const auto& e : rig.out)
        if (! e.on && (e.pitch % 12 == 4)) CHECK(e.at <= 2 * bar);
    // Latch: the chord stays after the keys are released.
    DeviceRig latched;
    latched.phrase.setParameter(Phrase::Chords, Phrase::FromMidi);
    latched.phrase.setParameter(Phrase::Latch, 1);
    for (int pitch : {57, 60, 64}) latched.input.push_back({0, juce::MidiMessage::noteOn(1, pitch, (juce::uint8) 100)});
    for (int pitch : {57, 60, 64}) latched.input.push_back({1000, juce::MidiMessage::noteOff(1, pitch)});
    latched.run(4 * bar);
    bool late = false;
    for (const auto& e : latched.ons()) late |= e.at > 2 * bar;
    CHECK(late);
    CHECK(latched.phrase.heldChord().count == 3);
}

TEST_CASE("phrase device: To Clip equals what the device plays; in MIDI In mode the clip's notes are the chords")
{
    Phrase phrase;
    phrase.setParameter(Phrase::Key, 9);
    model::Song song;
    song.tempo = 138;
    song.tracks.resize(1);
    song.tracks[0].kind = TrackKind::Midi;
    model::Clip clip;
    clip.uid = 3;
    clip.kind = model::Clip::Kind::Midi;
    clip.currentStart = clip.time = 32;
    clip.currentEnd = 48;
    clip.loop = {0, 16, 0, true};
    song.tracks[0].arrangementClips = {clip};
    auto spec = [&] {
        model::GeneratorClipSpec s;
        s.deviceName = phrase.name();
        s.usesInput = phrase.listensToInput();
        s.render = [&](const std::vector<MidiNote>& input, double from, double to) { return phrase.renderNotes(input, from, to, 138); };
        return s;
    };
    std::string message;
    auto command = applyGeneratorToClip(song, ClipLocation::arrangement(0, 3), spec(), message);
    REQUIRE(static_cast<bool>(command.apply));
    command.apply(song);
    const auto& written = song.tracks[0].arrangementClips[0].notes;
    const auto expected = phrase.renderNotes({}, 32, 48, 138);
    REQUIRE(written.size() == expected.size());
    CHECK(written.size() > 20);
    for (size_t i = 0; i < written.size(); ++i) CHECK_NEAR(written[i].start, expected[i].start - 32, 1e-9);
    // MIDI In: a clip holding F major for its whole length gives F-major tones.
    phrase.setParameter(Phrase::Chords, Phrase::FromMidi);
    CHECK(phrase.listensToInput());
    auto& c = song.tracks[0].arrangementClips[0];
    c.notes.clear();
    for (int pitch : {53, 57, 60})
    {
        MidiNote n;
        n.duration = 16;
        n.pitch = pitch;
        c.notes.push_back(n);
    }
    command = applyGeneratorToClip(song, ClipLocation::arrangement(0, 3), spec(), message);
    REQUIRE(static_cast<bool>(command.apply));
    command.apply(song);
    REQUIRE(! c.notes.empty());
    for (const auto& n : c.notes) CHECK(chordTones(Chord::degree(5), 9, kMinor).contains(n.pitch % 12) || inScale(n.pitch, 9, kMinor));
    std::set<int> classes;
    for (const auto& n : c.notes) classes.insert(n.pitch % 12);
    CHECK(classes.count(5) == 1);
    CHECK_NEAR(phrase.clipLengthBeats(), 8.0, 1e-9); // the arp's own form (MIDI In: no progression)
}

TEST_CASE("phrase device: state round trips (native content, .als, VST3 state, stand-in); catalog and factory")
{
    const auto* entry = findCatalogEntry("SonoraPhrase");
    REQUIRE(entry != nullptr);
    CHECK(entry->role == DeviceRole::MidiEffect);
    CHECK(deviceCategory(*entry) == "Generators");
    // A changed state survives the codec, and damaged bytes are refused.
    Phrase phrase;
    phrase.setParameter(Phrase::Part, (float) PhrasePart::Stabs);
    phrase.dice(0);
    phrase.answer(0, 1, AnswerKind::Counter);
    auto s = phrase.state();
    s.progression = factoryProgression(13);
    s.part(PhrasePart::Stabs).formLength = 2;
    s.part(PhrasePart::Stabs).form[1] = 1;
    s.part(PhrasePart::Pad).slots[3].motion = PadMotion::Pulse;
    phrase.setState(s);
    phrase.setParameter(Phrase::Density, 0.6f);
    const auto bytes = phrase.saveState();
    PhraseState decoded;
    std::string error;
    REQUIRE(Phrase::decodeState(bytes.data(), bytes.size(), decoded, error));
    CHECK(decoded == s);
    auto damaged = bytes;
    damaged.resize(damaged.size() / 2);
    CHECK(! Phrase::decodeState(damaged.data(), damaged.size(), decoded, error));
    CHECK(! error.empty());
    // The app's model device: native content, and the factory restores it.
    auto model = devices::nativeDeviceModel("SonoraPhrase");
    devices::captureDeviceState(model, &phrase);
    REQUIRE(model.nativeContent != nullptr);
    CHECK(model.nativeContent->format == Phrase::kStateFormat);
    std::vector<std::string> warnings;
    auto rebuilt = devices::createNative(model, &warnings);
    auto* restored = dynamic_cast<Phrase*>(rebuilt.get());
    REQUIRE(restored != nullptr);
    CHECK(warnings.empty());
    CHECK(restored->state() == s);
    CHECK_NEAR(restored->getParameter(Phrase::Density), 0.6f, 1e-6f);
    // VST3 state and the built-in stand-in for a missing VST3.
    const auto vst3 = plugins::encodeGeneratorState(phrase, "SonoraPhrase");
    Phrase fromVst3;
    REQUIRE(plugins::decodeGeneratorState(vst3.data(), vst3.size(), fromVst3, "SonoraPhrase", "Sonora Phrase", error));
    CHECK(fromVst3.state() == s);
    model::Device missing;
    missing.kind = DeviceKind::PluginVst3;
    missing.type = "PluginDevice";
    missing.displayName = "Sonora Phrase";
    model::PluginInfo info;
    info.name = "Sonora Phrase";
    info.vst3Uid = {0xABCDEF01u, 0x9182FAEBu, 0x536E7261u, 0x536E5068u}; // 'Snra', 'SnPh'
    info.state = vst3;
    missing.plugin = info;
    CHECK(plugins::sonoraPluginFor(info, DeviceKind::PluginVst3) == plugins::SonoraPlugin::Phrase);
    devices::BuildContext ctx;
    std::vector<std::string> log;
    ctx.log = &log;
    auto standIn = devices::createDevice(missing, ctx);
    auto* played = dynamic_cast<Phrase*>(standIn.get());
    REQUIRE(played != nullptr);
    CHECK(played->state() == s);
    // .als: a set with the device saves and reopens with its patterns.
    const auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("sonora-phrase-als");
    REQUIRE(dir.createDirectory().wasOk());
    const auto file = dir.getChildFile("Phrase.als");
    REQUIRE(io::writeAlsDom(*io::createNewSetDom(), file, error));
    auto loaded = io::loadAls(file);
    REQUIRE(loaded);
    auto& song = *loaded.song;
    int midiTrack = -1;
    for (size_t t = 0; t < song.tracks.size(); ++t)
        if (song.tracks[t].kind == TrackKind::Midi) { midiTrack = (int) t; break; }
    REQUIRE(midiTrack >= 0);
    model.id = -1;
    model::allocateDeviceIds(song, model);
    model.sourceXml = std::make_shared<const std::string>(io::deviceTemplate(model));
    model::EditHistory history;
    history.perform(song, model::insertDevice(midiTrack, model));
    REQUIRE(io::saveAls(song, file, {}).ok);
    auto reopened = io::loadAls(file);
    REQUIRE(reopened);
    const auto& devicesOnTrack = reopened.song->tracks[(size_t) midiTrack].devices;
    REQUIRE(! devicesOnTrack.empty());
    const auto& reread = devicesOnTrack.back();
    CHECK(reread.type == "SonoraPhrase");
    REQUIRE(reread.nativeContent != nullptr);
    CHECK(reread.nativeContent->data == model.nativeContent->data);
    dir.deleteRecursively();
}

TEST_CASE("phrase presets: at least 60 in the spec's categories; each plays; matching and Keep Chords")
{
    const auto& list = Phrase::presets();
    CHECK(list.size() >= 60);
    std::map<std::string, int> categories;
    std::set<std::string> names;
    for (const auto& p : list)
    {
        ++categories[p.category];
        names.insert(p.name);
        CHECK(! p.description.empty());
    }
    CHECK(names.size() == list.size());
    CHECK(categories["Arps"] >= 20);
    CHECK(categories["Stabs"] >= 14);
    CHECK(categories["Pads"] >= 12);
    CHECK(categories["Bass"] >= 10);
    for (int i = 0; i < (int) list.size(); ++i)
    {
        Phrase phrase;
        phrase.applyPreset(i);
        CHECK(phrase.matchingPreset() == i);
        CHECK(phrase.part() == list[(size_t) i].part);
        const auto notes = phrase.renderNotes({}, 0, 32, 138);
        CHECK(! notes.empty());
        for (const auto& n : notes) CHECK(inScale(n.pitch, 4, kMinor) || n.pitch % 12 == 5); // E minor, or a borrowed bII (F)
    }
    // An edit leaves the preset; Keep Chords keeps the progression.
    Phrase phrase;
    phrase.applyPreset(0);
    phrase.dice(0);
    CHECK(phrase.matchingPreset() == -1);
    auto s = phrase.state();
    s.progression = factoryProgression(9);
    s.keepChords = true;
    phrase.setState(s);
    phrase.applyPreset(1);
    CHECK(phrase.state().progression == factoryProgression(9));
}

// Demo renders for the owner's ears (docs/PHRASE.md "Session log"): only when SONORA_PHRASE_DEMOS names a folder.
// One 16-bar loop per preset category at 138 BPM in E minor, through Sonora's built-in wavetable synth (Operator for the
// bass) with a plain sine kick on the beat for context, and the four parts together.
#include <juce_audio_formats/juce_audio_formats.h>

TEST_CASE("phrase demos: 16-bar renders per category (SONORA_PHRASE_DEMOS)")
{
    const auto* folderName = std::getenv("SONORA_PHRASE_DEMOS");
    if (folderName == nullptr || *folderName == 0) return;
    const juce::File folder(folderName);
    REQUIRE(folder.createDirectory().wasOk());
    constexpr double kRate = 44100, kTempo = 138;
    constexpr int kBlock = 512;
    const auto total = (int64_t) std::ceil(64 * kRate * 60 / kTempo) + (int64_t) kRate; // 16 bars and a second of tail
    struct Part { const char* preset; const char* synth; float gain; };
    auto render = [&](std::vector<Part> parts, bool kick, const std::string& file) {
        juce::AudioBuffer<float> mix(2, (int) total);
        mix.clear();
        for (const auto& part : parts)
        {
            Phrase phrase;
            const auto& list = Phrase::presets();
            int index = -1;
            for (int i = 0; i < (int) list.size(); ++i)
                if (list[(size_t) i].name == part.preset) index = i;
            REQUIRE(index >= 0);
            phrase.applyPreset(index);
            phrase.setParameter(Phrase::Key, 4);
            phrase.setParameter(Phrase::Scale, kMinor);
            auto synth = devices::createNative(devices::nativeDeviceModel(part.synth));
            REQUIRE(synth != nullptr);
            phrase.prepare(kRate, kBlock);
            synth->prepare(kRate, kBlock);
            juce::AudioBuffer<float> block(2, kBlock);
            juce::MidiBuffer midi;
            midi.ensureSize(65536);
            double beat = 0;
            for (int64_t at = 0; at < total; at += kBlock)
            {
                const int n = (int) std::min<int64_t>(kBlock, total - at);
                juce::AudioBuffer<float> view(block.getArrayOfWritePointers(), 2, n);
                view.clear();
                midi.clear();
                devices::HostInfo info;
                info.tempo = kTempo;
                info.beats = beat;
                info.playing = beat < 64.0;
                info.sampleRate = kRate;
                phrase.setHostInfo(info);
                synth->setHostInfo(info);
                phrase.process(view, midi);
                synth->process(view, midi);
                for (int ch = 0; ch < 2; ++ch) mix.addFrom(ch, (int) at, view, ch, 0, n, part.gain);
                beat += n * kTempo / 60.0 / kRate;
            }
        }
        if (kick)
            for (int b = 0; b < 64; ++b)
            {
                const auto start = (int64_t) std::llround(b * 60.0 / kTempo * kRate);
                double phase = 0;
                for (int i = 0; i < (int) (0.22 * kRate) && start + i < total; ++i)
                {
                    const double t = i / kRate;
                    phase += 2 * juce::MathConstants<double>::pi * (45 + 110 * std::exp(-t * 30)) / kRate;
                    const float v = (float) (0.8 * std::sin(phase) * std::exp(-t * 9));
                    for (int ch = 0; ch < 2; ++ch) mix.addSample(ch, (int) (start + i), v);
                }
            }
        const float peak = mix.getMagnitude(0, (int) total);
        CHECK(peak > 0.01f);
        if (peak > 0.95f) mix.applyGain(0.95f / peak);
        const auto out = folder.getChildFile(file);
        out.deleteFile();
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream>(out);
        auto writer = wav.createWriterFor(stream, juce::AudioFormatWriterOptions{}.withSampleRate(kRate).withNumChannels(2).withBitsPerSample(24));
        REQUIRE(writer != nullptr);
        writer->writeFromAudioSampleBuffer(mix, 0, mix.getNumSamples());
        std::printf("  wrote %s (peak %.2f)\n", out.getFullPathName().toRawUTF8(), peak);
    };
    render({{"Pedal Rider", "InstrumentVector", 0.5f}}, true, "Phrase - Arps - Pedal Rider.wav");
    render({{"Pedal Glide", "InstrumentVector", 0.5f}}, true, "Phrase - Arps - Pedal Glide.wav");
    render({{"Triplet Cascade", "InstrumentVector", 0.5f}}, true, "Phrase - Arps - Triplet Cascade.wav");
    render({{"Offbeat Q A", "InstrumentVector", 0.35f}}, true, "Phrase - Stabs - Offbeat Q A.wav");
    render({{"Q Q Q A", "InstrumentVector", 0.35f}}, true, "Phrase - Stabs - Q Q Q A.wav");
    render({{"Wide Bed", "InstrumentVector", 0.3f}}, false, "Phrase - Pads - Wide Bed.wav");
    render({{"Breathing Wide", "InstrumentVector", 0.3f}}, false, "Phrase - Pads - Breathing Wide.wav");
    render({{"KBBB Roller", "Operator", 0.6f}}, true, "Phrase - Bass - KBBB Roller.wav");
    render({{"Gallop", "Operator", 0.6f}}, true, "Phrase - Bass - Gallop.wav");
    render({{"KBBB Roller", "Operator", 0.5f}, {"Pedal Rider", "InstrumentVector", 0.3f}, {"Offbeat Q A", "InstrumentVector", 0.2f},
            {"Wide Bed", "InstrumentVector", 0.15f}}, true, "Phrase - All four parts.wav");
}
