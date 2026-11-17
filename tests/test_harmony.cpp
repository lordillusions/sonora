// PLG-25a: core/Harmony, PatternRandom and PatternGrid (the shared foundation of Sonora Phrase and Pulse).

#include "TestFramework.h"

#include "core/Harmony.h"
#include "core/PatternGrid.h"
#include "core/PatternRandom.h"
#include "core/Scales.h"

#include <algorithm>
#include <set>
#include <vector>

using namespace sonora;
using namespace sonora::model;

namespace
{
constexpr int kMajor = 0, kMinor = 1, kDorian = 2, kPhrygian = 5, kHarmonicMinor = 13, kPhrygianDominant = 16;

std::vector<int> intervalsOf(const ChordTones& t)
{
    std::vector<int> out;
    for (int i = 0; i < t.count; ++i) out.push_back(t.intervals[(size_t) i]);
    return out;
}

std::string spelled(int degree, int key, int scale, ChordExtension e = ChordExtension::None)
{
    return chordName(chordTones(Chord::degree(degree, ChordQuality::Auto, e), key, scale), false);
}

ChordTones held(std::vector<int> pitches) { return chordFromHeldNotes(pitches.data(), (int) pitches.size()); }

bool allChordTones(const Voicing& v, const ChordTones& t)
{
    for (int i = 0; i < v.count; ++i)
        if (! t.contains(v.pitches[(size_t) i] % 12) && v.pitches[(size_t) i] % 12 != t.bass) return false;
    return true;
}

bool lowRuleHolds(const Voicing& v, const ChordTones& t)
{
    for (int i = 0; i < v.count; ++i)
    {
        const int p = v.pitches[(size_t) i];
        if (p >= kThirdFloor) break;
        if (p % 12 != t.root && p % 12 != (t.root + 7) % 12) return false;
        if (i + 1 < v.count && v.pitches[(size_t) i + 1] - p < 5) return false;
    }
    return true;
}
} // namespace

TEST_CASE("harmony: scale degree pitches wrap octaves")
{
    CHECK(scaleDegreePitch(0, kMajor, 0, 4) == 60);
    CHECK(scaleDegreePitch(0, kMinor, 2, 4) == 63);
    CHECK(scaleDegreePitch(9, kMinor, 0, 3) == 57);  // A3
    CHECK(scaleDegreePitch(0, kMajor, 7, 4) == 72);  // the octave
    CHECK(scaleDegreePitch(0, kMajor, -1, 4) == 59); // B below
    CHECK(scaleDegreePitch(4, kPhrygian, 1, 2) == 41); // F2 over E Phrygian
    CHECK(scaleDegreeOf(5, 4, kPhrygian) == 1);
    CHECK(scaleDegreeOf(6, 4, kPhrygian) == -1);
}

TEST_CASE("harmony: Auto chords on every degree equal the textbook spelling")
{
    // A minor: i ii° III iv v VI VII
    const std::vector<std::string> minor{"Am", "Bdim", "C", "Dm", "Em", "F", "G"};
    for (int d = 0; d < 7; ++d) CHECK(spelled(d, 9, kMinor) == minor[(size_t) d]);
    // E Phrygian: i bII bIII iv v° bVI bvii
    const std::vector<std::string> phrygian{"Em", "F", "G", "Am", "Bdim", "C", "Dm"};
    for (int d = 0; d < 7; ++d) CHECK(spelled(d, 4, kPhrygian) == phrygian[(size_t) d]);
    // A harmonic minor: i ii° III+ iv V VI vii°
    const std::vector<std::string> harmonic{"Am", "Bdim", "Caug", "Dm", "E", "F", "G#dim"};
    for (int d = 0; d < 7; ++d) CHECK(spelled(d, 9, kHarmonicMinor) == harmonic[(size_t) d]);
    // D Dorian: i ii bIII IV v vi° bVII
    const std::vector<std::string> dorian{"Dm", "Em", "F", "G", "Am", "Bdim", "C"};
    for (int d = 0; d < 7; ++d) CHECK(spelled(d, 2, kDorian) == dorian[(size_t) d]);
    // E Phrygian dominant: I bII iii° iv v° bVI+ bvii
    const std::vector<std::string> phrygianDominant{"E", "F", "G#dim", "Am", "Bdim", "Caug", "Dm"};
    for (int d = 0; d < 7; ++d) CHECK(spelled(d, 4, kPhrygianDominant) == phrygianDominant[(size_t) d]);
    // Sevenths from the scale: A minor i7 = Am7, III7 = Cmaj7, v7 = Em7, VII7 = G7, ii°7 = Bm7b5.
    CHECK(spelled(0, 9, kMinor, ChordExtension::Seventh) == "Am7");
    CHECK(spelled(2, 9, kMinor, ChordExtension::Seventh) == "Cmaj7");
    CHECK(spelled(4, 9, kMinor, ChordExtension::Seventh) == "Em7");
    CHECK(spelled(6, 9, kMinor, ChordExtension::Seventh) == "G7");
    CHECK(spelled(1, 9, kMinor, ChordExtension::Seventh) == "Bm7b5");
    CHECK(spelled(4, 9, kHarmonicMinor, ChordExtension::Seventh) == "E7"); // the harmonic minor's dominant
    // Ninths: i9 in A minor = A C E G B; bII in Phrygian with add9.
    CHECK(intervalsOf(chordTones(Chord::degree(0, ChordQuality::Auto, ChordExtension::Ninth), 9, kMinor)) == std::vector<int>({0, 3, 7, 10, 14}));
    CHECK(spelled(0, 9, kMinor, ChordExtension::Ninth) == "Am9");
    CHECK(spelled(1, 4, kPhrygian, ChordExtension::Add9) == "Fadd9");
}

TEST_CASE("harmony: explicit qualities and extensions")
{
    auto tones = [](ChordQuality q, ChordExtension e = ChordExtension::None) { return intervalsOf(chordTones(Chord::degree(0, q, e), 0, kMajor)); };
    CHECK(tones(ChordQuality::Major) == std::vector<int>({0, 4, 7}));
    CHECK(tones(ChordQuality::Minor) == std::vector<int>({0, 3, 7}));
    CHECK(tones(ChordQuality::Diminished) == std::vector<int>({0, 3, 6}));
    CHECK(tones(ChordQuality::Augmented) == std::vector<int>({0, 4, 8}));
    CHECK(tones(ChordQuality::Sus2) == std::vector<int>({0, 2, 7}));
    CHECK(tones(ChordQuality::Sus4) == std::vector<int>({0, 5, 7}));
    CHECK(tones(ChordQuality::Fifth) == std::vector<int>({0, 7}));
    CHECK(tones(ChordQuality::Major, ChordExtension::Sixth) == std::vector<int>({0, 4, 7, 9}));
    CHECK(tones(ChordQuality::Major, ChordExtension::Seventh) == std::vector<int>({0, 4, 7, 11})); // I of C major: maj7
    CHECK(tones(ChordQuality::Minor, ChordExtension::Ninth) == std::vector<int>({0, 3, 7, 11, 14}));
    CHECK(tones(ChordQuality::Major, ChordExtension::Add9) == std::vector<int>({0, 4, 7, 14}));
    CHECK(tones(ChordQuality::Minor, ChordExtension::Eleventh) == std::vector<int>({0, 3, 7, 11, 14, 17}));
    CHECK(! chordTones(Chord::degree(0, ChordQuality::Sus4), 0, kMajor).third);
    // A borrowed root (bII = F over E minor) with defaults outside the scale: F7 has a minor seventh.
    const auto borrowed = chordTones(Chord::borrowed(1, ChordQuality::Major, ChordExtension::Seventh), 4, kMinor);
    CHECK(borrowed.root == 5);
    CHECK(intervalsOf(borrowed) == std::vector<int>({0, 4, 7, 10}));
    CHECK(chordName(borrowed) == "F7");
    // Slash chord: VI over its third (F/A in A minor).
    auto slash = Chord::degree(5);
    slash.bass = 0;
    CHECK(chordName(chordTones(slash, 9, kMinor)) == "F/A");
    // The tone ladder: a triad's fourth tone is the octave; with a ninth the ladder cycle is two octaves.
    const auto triad = chordTones(Chord::degree(0), 9, kMinor);
    CHECK(triad.interval(3) == 12);
    CHECK(triad.interval(4) == 15);
    CHECK(triad.interval(-1) == -5);
    CHECK(chordTones(Chord::degree(0, ChordQuality::Auto, ChordExtension::Ninth), 9, kMinor).interval(5) == 24);
    // Fit colours.
    CHECK(noteFit(57, triad, 9, kMinor) == NoteFit::Root);
    CHECK(noteFit(64, triad, 9, kMinor) == NoteFit::ChordTone);
    CHECK(noteFit(62, triad, 9, kMinor) == NoteFit::InScale);
    CHECK(noteFit(63, triad, 9, kMinor) == NoteFit::Outside);
}

TEST_CASE("harmony: voicings use chord tones, honour the top tone and keep thirds above C3")
{
    const int key = 9;
    for (int style = 0; style < (int) VoicingStyle::Count; ++style)
        for (int degree = 0; degree < 7; ++degree)
            for (int voices = 3; voices <= 6; ++voices)
                for (int centre : {40, 52, 60, 72})
                {
                    const auto t = chordTones(Chord::degree(degree, ChordQuality::Auto, voices > 4 ? ChordExtension::Seventh : ChordExtension::None), key, kMinor);
                    VoicingRequest rq;
                    rq.style = (VoicingStyle) style;
                    rq.voices = voices;
                    rq.centre = centre;
                    rq.lowLimit = 28;
                    const auto v = voice(t, rq);
                    CHECK(v.count == voices);
                    CHECK(allChordTones(v, t));
                    CHECK(lowRuleHolds(v, t));
                    for (int i = 1; i < v.count; ++i) CHECK(v.pitches[(size_t) i] > v.pitches[(size_t) i - 1]);
                    if (style != (int) VoicingStyle::Fifths)
                    {
                        // Top tone: the fifth (tone 2), then the third (tone 1) on top.
                        for (int top : {1, 2})
                        {
                            rq.topTone = top;
                            const auto withTop = voice(t, rq);
                            CHECK(withTop.highest() % 12 == t.pitchClass(top));
                            CHECK(lowRuleHolds(withTop, t));
                        }
                    }
                }
    // Fifths: root and fifth stacked, a requested third on top.
    const auto am = chordTones(Chord::degree(0), key, kMinor);
    VoicingRequest fifths;
    fifths.style = VoicingStyle::Fifths;
    fifths.voices = 4;
    fifths.centre = 52;
    const auto f = voice(am, fifths);
    CHECK(f.pitches[1] - f.pitches[0] == 7);
    CHECK(f.pitches[2] - f.pitches[0] == 12);
    fifths.topTone = 1;
    CHECK(voice(am, fifths).highest() % 12 == 0); // C on top of A fifths
    // Inversion: the lowest voice is the requested tone.
    VoicingRequest inverted;
    inverted.voices = 3;
    inverted.inversion = 1;
    CHECK(voice(am, inverted).lowest() % 12 == 0);
    // A slash bass goes under the voicing.
    auto slash = Chord::degree(5);
    slash.bass = 0;
    inverted.inversion = 0; // F in the voicing's bass, the A under it
    const auto withBass = voice(chordTones(slash, key, kMinor), inverted);
    CHECK(withBass.count == 4);
    CHECK(withBass.lowest() % 12 == 9);
}

TEST_CASE("harmony: voices count and style spreads")
{
    const auto cm7 = chordTones(Chord::degree(0, ChordQuality::Auto, ChordExtension::Seventh), 0, kMinor);
    auto spread = [&](VoicingStyle s, int voices) {
        VoicingRequest rq;
        rq.style = s;
        rq.voices = voices;
        rq.centre = 62;
        const auto v = voice(cm7, rq);
        return v.highest() - v.lowest();
    };
    CHECK(spread(VoicingStyle::Close, 4) < 12);
    CHECK(spread(VoicingStyle::Open, 4) > 12);
    CHECK(spread(VoicingStyle::Drop2, 4) > 12);
    CHECK(spread(VoicingStyle::Wide, 5) >= 19);
    CHECK(spread(VoicingStyle::Close, 3) <= 10);
    // Fewer voices than tones: the fifth is dropped first.
    VoicingRequest three;
    three.voices = 3;
    const auto v = voice(cm7, three);
    std::set<int> classes;
    for (int i = 0; i < v.count; ++i) classes.insert(v.pitches[(size_t) i] % 12);
    CHECK(classes == std::set<int>({0, 3, 10}));
}

TEST_CASE("harmony: least motion voice leading over i-bVI-bVII-i")
{
    for (int key : {4, 5, 6, 7})          // E, F, F#, G minor: the style's favourite keys
        for (int voices : {3, 4, 5})
        {
            const int degrees[] = {0, 5, 6, 0, 5, 6, 0};
            Voicing previous;
            double motion = 0;
            int moves = 0;
            for (int d : degrees)
            {
                VoicingRequest rq;
                rq.voices = voices;
                rq.centre = 62;
                const auto t = chordTones(Chord::degree(d), key, kMinor);
                const auto v = voice(t, rq, previous.count > 0 ? &previous : nullptr);
                if (previous.count == v.count)
                {
                    for (int i = 0; i < v.count; ++i) motion += std::abs(v.pitches[(size_t) i] - previous.pitches[(size_t) i]);
                    moves += v.count;
                }
                previous = v;
            }
            CHECK(moves > 0);
            CHECK(motion / moves <= 2.0);
        }
    // A common tone stays where it was: Am -> F keeps A and C.
    VoicingRequest rq;
    rq.voices = 3;
    rq.centre = 64;
    const auto am = voice(chordTones(Chord::degree(0), 9, kMinor), rq);
    const auto f = voice(chordTones(Chord::degree(5), 9, kMinor), rq, &am);
    int common = 0;
    for (int i = 0; i < am.count; ++i)
        for (int j = 0; j < f.count; ++j) common += am.pitches[(size_t) i] == f.pitches[(size_t) j];
    CHECK(common == 2);
}

TEST_CASE("harmony: progression chordAt across slot boundaries and the loop")
{
    Progression p;
    p.count = 4;
    p.slots[0] = {Chord::degree(0), 8};
    p.slots[1] = {Chord::degree(5), 4};
    p.slots[2] = {Chord::degree(6), 4};
    p.slots[3] = {Chord::degree(0), 16};
    CHECK_NEAR(p.lengthBeats(), 32.0, 1e-12);
    CHECK(p.at(0).index == 0);
    CHECK(p.at(7.999).index == 0);
    CHECK(p.at(8).index == 1);
    CHECK(p.at(7.9999999999).index == 1); // within the epsilon of the boundary: the slot that starts
    CHECK(p.at(12).index == 2);
    CHECK(p.at(16).index == 3);
    CHECK(p.at(31.9).index == 3);
    CHECK(p.at(32).index == 0); // loops
    CHECK(p.at(40).index == 1);
    CHECK_NEAR(p.at(40).start, 40.0, 1e-12);
    CHECK_NEAR(p.at(41).end, 44.0, 1e-12);
    CHECK(p.chordAt(45).root == 6);
    CHECK(p.at(-1).index == 3); // before the start: the end of the previous lap
    // Zero-length slots are skipped.
    p.slots[1].beats = 0;
    CHECK(p.at(8).index == 2);
}

TEST_CASE("harmony: chordFromHeldNotes for triads, sevenths, inversions and two notes")
{
    CHECK(chordName(held({60, 64, 67})) == "C");
    CHECK(chordName(held({57, 60, 64})) == "Am");
    CHECK(chordName(held({64, 67, 72})) == "C/E");          // first inversion
    CHECK(chordName(held({55, 60, 64})) == "C/G");          // second inversion
    CHECK(chordName(held({57, 60, 64, 67})) == "Am7");
    CHECK(chordName(held({60, 64, 67, 69})) == "C6");       // same notes, C at the bottom
    CHECK(chordName(held({55, 59, 62, 65})) == "G7");
    CHECK(chordName(held({59, 62, 65, 69})) == "Bm7b5");
    CHECK(chordName(held({53, 57, 60, 64})) == "Fmaj7");
    CHECK(chordName(held({62, 67, 69})) == "Dsus4");
    CHECK(chordName(held({52, 64, 67, 71, 76})) == "Em");   // doubled octaves
    CHECK(chordName(held({48, 55})) == "C5");
    CHECK(chordName(held({55, 60})) == "C5/G");             // a fourth: a fifth chord over its fifth
    CHECK(chordName(held({60, 63})) == "Cm");               // a third: the fifth implied
    CHECK(intervalsOf(held({60, 63})) == std::vector<int>({0, 3, 7}));
    CHECK(chordName(held({60, 64, 70})) == "C7");           // seventh without its fifth
    CHECK(chordName(held({62})) == "D5");
    CHECK(held({}).count == 0);
}

TEST_CASE("pattern random: deterministic, spread and independent per purpose")
{
    CHECK(PatternRandom::hash(7, 1, 2, 3) == PatternRandom::hash(7, 1, 2, 3));
    CHECK(PatternRandom::hash(7, 1, 2, 3) != PatternRandom::hash(8, 1, 2, 3));
    CHECK(PatternRandom::hash(7, 1, 2, 3) != PatternRandom::hash(7, 1, 2, 4));
    CHECK(PatternRandom::hash(7, -1) != PatternRandom::hash(7, 1));
    static_assert(PatternRandom::hash(1, 2, 3, 4) == PatternRandom::hash(1, 2, 3, 4), "constexpr");
    double sum = 0;
    int below = 0, hits = 0;
    std::array<int, 8> buckets{};
    for (int i = 0; i < 20000; ++i)
    {
        const double u = PatternRandom::unit(42, i, 5, 9);
        CHECK(u >= 0.0 && u < 1.0);
        sum += u;
        below += u < 0.25;
        hits += PatternRandom::chance(0.3, 42, i, 1, 2);
        ++buckets[(size_t) PatternRandom::range(0, 7, 42, i)];
    }
    CHECK(std::abs(sum / 20000 - 0.5) < 0.01);
    CHECK(std::abs(below / 20000.0 - 0.25) < 0.015);
    CHECK(std::abs(hits / 20000.0 - 0.3) < 0.015);
    for (int b : buckets) CHECK(std::abs(b / 20000.0 - 0.125) < 0.01);
    CHECK(! PatternRandom::chance(0.0, 1, 2));
    CHECK(PatternRandom::chance(1.0, 1, 2));
    CHECK(PatternRandom::range(5, 5, 1, 2) == 5);
}

TEST_CASE("pattern grid: rates, swing warp and cycle arithmetic")
{
    CHECK_NEAR(grid::rateBeats(kRate1_16), 0.25, 1e-12);
    CHECK(std::abs(grid::rateBeats(kRate1_16T) - 1.0 / 6) < 1e-12);
    CHECK_NEAR(grid::rateBeats(kRate1_8D), 0.75, 1e-12);
    CHECK_NEAR(grid::rateBeats(99), 4.0, 1e-12);
    CHECK(grid::rateIndex("1/32T") == kRate1_32T);
    CHECK(grid::rateIndex("1/3") == -1);
    CHECK(grid::isTriplet(kRate1_8T) && ! grid::isTriplet(kRate1_8));
    CHECK_NEAR(grid::beatsPerBar(4, 4), 4.0, 1e-12);
    CHECK_NEAR(grid::beatsPerBar(6, 8), 3.0, 1e-12);
    CHECK(grid::indexAt(3.9999999999, 1.0) == 4);
    CHECK(grid::indexAt(-0.5, 1.0) == -1);
    CHECK_NEAR(grid::startOf(9.5, 8.0), 8.0, 1e-12);
    CHECK_NEAR(grid::positionIn(9.5, 8.0), 1.5, 1e-12);
    CHECK_NEAR(grid::positionIn(-1, 8.0), 7.0, 1e-12);
    // Swing: even steps stay, odd steps move a third of a step at 100 %, monotonic, 0 is straight.
    CHECK_NEAR(grid::swingWarp(0.5, 0.25, 1.0), 0.5, 1e-12);
    CHECK(std::abs(grid::swingWarp(0.25, 0.25, 1.0) - (0.25 + 0.25 / 3)) < 1e-12);
    CHECK(std::abs(grid::stepBeat(1, 0.25, 1.0) - (0.25 + 0.25 / 3)) < 1e-12);
    CHECK_NEAR(grid::stepBeat(2, 0.25, 1.0), 0.5, 1e-12);
    CHECK_NEAR(grid::swingWarp(0.3, 0.25, 0.0), 0.3, 1e-12);
    double last = -1;
    for (int i = 0; i <= 400; ++i)
    {
        const double w = grid::swingWarp(i * 0.01, 0.25, 0.6);
        CHECK(w > last);
        last = w;
    }
    const auto range = grid::stepsIn(1.0, 2.0, 0.25);
    CHECK(range.first <= 4 && range.end >= 8);
    CHECK(grid::stepsIn(2.0, 1.0, 0.25).end == 0);
}
