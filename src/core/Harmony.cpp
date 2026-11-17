#include "core/Harmony.h"

#include "core/PatternGrid.h"
#include "core/Scales.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace sonora::model
{
namespace
{
constexpr int floorDiv(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }
constexpr int floorMod(int a, int b) { return a - floorDiv(a, b) * b; }

struct Shape
{
    const char* suffix;
    std::array<int8_t, 6> intervals; // output, chord-function order
    int count;
    bool third;
    int required; // how many of the first intervals must be held to match (count: all; less: the rest is implied)
};

// Recognised shapes, most common first (the order breaks ties).
constexpr std::array<Shape, 28> kShapes{{
    {"", {0, 4, 7}, 3, true, 3},
    {"m", {0, 3, 7}, 3, true, 3},
    {"5", {0, 7}, 2, false, 2},
    {"sus4", {0, 5, 7}, 3, false, 3},
    {"sus2", {0, 2, 7}, 3, false, 3},
    {"dim", {0, 3, 6}, 3, true, 3},
    {"aug", {0, 4, 8}, 3, true, 3},
    {"7", {0, 4, 7, 10}, 4, true, 4},
    {"maj7", {0, 4, 7, 11}, 4, true, 4},
    {"m7", {0, 3, 7, 10}, 4, true, 4},
    {"m7b5", {0, 3, 6, 10}, 4, true, 4},
    {"dim7", {0, 3, 6, 9}, 4, true, 4},
    {"m(maj7)", {0, 3, 7, 11}, 4, true, 4},
    {"6", {0, 4, 7, 9}, 4, true, 4},
    {"m6", {0, 3, 7, 9}, 4, true, 4},
    {"add9", {0, 4, 7, 14}, 4, true, 4},
    {"m(add9)", {0, 3, 7, 14}, 4, true, 4},
    {"9", {0, 4, 7, 10, 14}, 5, true, 5},
    {"maj9", {0, 4, 7, 11, 14}, 5, true, 5},
    {"m9", {0, 3, 7, 10, 14}, 5, true, 5},
    {"7sus4", {0, 5, 7, 10}, 4, false, 4},
    {"11", {0, 4, 7, 10, 14, 17}, 6, true, 6},
    {"m11", {0, 3, 7, 10, 14, 17}, 6, true, 6},
    // Shapes with the fifth missing: matched on what is held, the fifth implied.
    {"", {0, 4, 7}, 3, true, 2},
    {"m", {0, 3, 7}, 3, true, 2},
    {"7", {0, 4, 7, 10}, 4, true, -1},  // -1: held = root, third, seventh
    {"m7", {0, 3, 7, 10}, 4, true, -1},
    {"maj7", {0, 4, 7, 11}, 4, true, -1},
}};

int maskOf(const int8_t* intervals, int n)
{
    int m = 0;
    for (int i = 0; i < n; ++i) m |= 1 << floorMod(intervals[i], 12);
    return m;
}
int requiredMask(const Shape& s)
{
    if (s.required >= 0) return maskOf(s.intervals.data(), s.required);
    const int8_t held[3] = {s.intervals[0], s.intervals[1], s.intervals[3]};
    return maskOf(held, 3);
}
int rotate(int mask, int by) // pitch-class mask seen from root `by`
{
    int out = 0;
    for (int pc = 0; pc < 12; ++pc)
        if ((mask >> pc) & 1) out |= 1 << floorMod(pc - by, 12);
    return out;
}
int popcount(int m)
{
    int n = 0;
    for (; m != 0; m &= m - 1) ++n;
    return n;
}
} // namespace

int scaleDegreePitch(int key, int scale, int degree, int octave)
{
    const auto& s = scaleAt(scale);
    const int shift = floorDiv(degree, s.size);
    const int index = degree - shift * s.size;
    return 12 * (octave + 1) + floorMod(key, 12) + s.intervals[(size_t) index] + 12 * shift;
}

int scaleDegreeOf(int pitchClass, int key, int scale)
{
    const auto& s = scaleAt(scale);
    const int relative = floorMod(pitchClass - key, 12);
    for (int i = 0; i < s.size; ++i)
        if (s.intervals[(size_t) i] == relative) return i;
    return -1;
}

const char* chordQualityName(ChordQuality q)
{
    switch (q)
    {
        case ChordQuality::Auto: return "Auto";
        case ChordQuality::Major: return "Major";
        case ChordQuality::Minor: return "Minor";
        case ChordQuality::Diminished: return "Dim";
        case ChordQuality::Augmented: return "Aug";
        case ChordQuality::Sus2: return "Sus2";
        case ChordQuality::Sus4: return "Sus4";
        case ChordQuality::Fifth: return "Fifth";
        case ChordQuality::Count: break;
    }
    return "";
}

const char* chordExtensionName(ChordExtension e)
{
    switch (e)
    {
        case ChordExtension::None: return "None";
        case ChordExtension::Sixth: return "6";
        case ChordExtension::Seventh: return "7";
        case ChordExtension::Ninth: return "9";
        case ChordExtension::Add9: return "Add9";
        case ChordExtension::Eleventh: return "11";
        case ChordExtension::Count: break;
    }
    return "";
}

const char* voicingStyleName(VoicingStyle v)
{
    switch (v)
    {
        case VoicingStyle::Close: return "Close";
        case VoicingStyle::Open: return "Open";
        case VoicingStyle::Drop2: return "Drop 2";
        case VoicingStyle::Wide: return "Wide";
        case VoicingStyle::Fifths: return "Fifths";
        case VoicingStyle::Count: break;
    }
    return "";
}

// ---------------------------------------------------------------------------------------------------------- ChordTones
int ChordTones::interval(int toneIndex) const
{
    if (count <= 0) return 0;
    int top = 0;
    for (int i = 0; i < count; ++i) top = std::max(top, (int) intervals[(size_t) i]);
    const int cycle = 12 * (top / 12 + 1);
    const int lap = floorDiv(toneIndex, count);
    return intervals[(size_t) (toneIndex - lap * count)] + cycle * lap;
}

bool ChordTones::contains(int pc) const { return toneOf(pc) >= 0; }

int ChordTones::toneOf(int pc) const
{
    for (int i = 0; i < count; ++i)
        if (floorMod(root + intervals[(size_t) i], 12) == floorMod(pc, 12)) return i;
    return -1;
}

ChordTones chordTones(const Chord& chord, int key, int scale)
{
    const auto& s = scaleAt(scale);
    key = floorMod(key, 12);
    ChordTones t;
    int degree = 0;
    bool inScale = true;
    if (! chord.chromatic)
    {
        degree = floorMod(chord.root, s.size);
        t.root = (key + s.intervals[(size_t) degree]) % 12;
    }
    else
    {
        t.root = floorMod(key + chord.root, 12);
        degree = scaleDegreeOf(t.root, key, scale);
        inScale = degree >= 0;
    }
    auto step = [&](int steps) { return scaleDegreePitch(key, scale, degree + steps, 4) - scaleDegreePitch(key, scale, degree, 4); };

    auto quality = chord.quality;
    if (quality == ChordQuality::Auto && ! inScale) quality = ChordQuality::Major;
    auto push = [&t](int interval) { if (t.count < (int) t.intervals.size()) t.intervals[(size_t) t.count++] = (int8_t) interval; };
    switch (quality)
    {
        case ChordQuality::Auto:
        {
            const int third = step(2), fifth = step(4);
            push(0); push(third); push(fifth);
            t.third = third == 3 || third == 4;
            break;
        }
        case ChordQuality::Major: push(0); push(4); push(7); break;
        case ChordQuality::Minor: push(0); push(3); push(7); break;
        case ChordQuality::Diminished: push(0); push(3); push(6); break;
        case ChordQuality::Augmented: push(0); push(4); push(8); break;
        case ChordQuality::Sus2: push(0); push(2); push(7); t.third = false; break;
        case ChordQuality::Sus4: push(0); push(5); push(7); t.third = false; break;
        case ChordQuality::Fifth: push(0); push(7); t.third = false; break;
        case ChordQuality::Count: push(0); push(4); push(7); break;
    }
    const bool fromScale = inScale && s.size == 7;
    const bool diminished = t.count >= 3 && t.intervals[1] == 3 && t.intervals[2] == 6;
    const int sixth = fromScale ? step(5) : 9;
    const int seventh = fromScale ? step(6) : (diminished ? 9 : 10);
    const int ninth = fromScale ? step(8) : 14;
    const int eleventh = fromScale ? step(10) : 17;
    switch (chord.extension)
    {
        case ChordExtension::Sixth: push(sixth); break;
        case ChordExtension::Seventh: push(seventh); break;
        case ChordExtension::Ninth: push(seventh); push(ninth); break;
        case ChordExtension::Add9: push(ninth); break;
        case ChordExtension::Eleventh: push(seventh); push(ninth); push(eleventh); break;
        case ChordExtension::None:
        case ChordExtension::Count: break;
    }
    if (chord.bass >= 0)
    {
        const int bassPc = (key + s.intervals[(size_t) floorMod(chord.bass, s.size)]) % 12;
        if (bassPc != t.root) t.bass = bassPc;
    }
    return t;
}

NoteFit noteFit(int pitch, const ChordTones& chord, int key, int scale)
{
    const int pc = pitchClassOf(pitch);
    if (chord.count > 0 && pc == chord.root) return NoteFit::Root;
    if (chord.contains(pc) || pc == chord.bass) return NoteFit::ChordTone;
    if (inScale(pitch, floorMod(key, 12), scale)) return NoteFit::InScale;
    return NoteFit::Outside;
}

// ------------------------------------------------------------------------------------------------------------ voicing
namespace
{
struct Candidate
{
    std::array<int, Voicing::kMax> p{};
    int n = 0;
    void sort() { std::sort(p.begin(), p.begin() + n); }
};

bool inRange(const Candidate& c, const VoicingRequest& rq)
{
    for (int i = 0; i < c.n; ++i)
        if (c.p[(size_t) i] < rq.lowLimit || c.p[(size_t) i] > rq.highLimit || c.p[(size_t) i] < 0 || c.p[(size_t) i] > 127) return false;
    return true;
}

bool lowRuleHolds(const Candidate& c, const ChordTones& chord)
{
    const int fifth = (chord.root + 7) % 12;
    for (int i = 0; i < c.n; ++i)
    {
        const int p = c.p[(size_t) i];
        if (p >= kThirdFloor) break;
        const int pc = pitchClassOf(p);
        if (pc != chord.root && pc != fifth) return false;
        if (i + 1 < c.n && c.p[(size_t) i + 1] - p < 5) return false;
    }
    return true;
}

double motion(const Candidate& c, const Voicing& prev)
{
    double sum = 0;
    if (prev.count == c.n)
    {
        for (int i = 0; i < c.n; ++i) sum += std::abs(c.p[(size_t) i] - prev.pitches[(size_t) i]);
        return sum;
    }
    // Different sizes: each note to its nearest partner, both ways, averaged.
    for (int i = 0; i < c.n; ++i)
    {
        int best = 128;
        for (int j = 0; j < prev.count; ++j) best = std::min(best, std::abs(c.p[(size_t) i] - prev.pitches[(size_t) j]));
        sum += best;
    }
    for (int j = 0; j < prev.count; ++j)
    {
        int best = 128;
        for (int i = 0; i < c.n; ++i) best = std::min(best, std::abs(c.p[(size_t) i] - prev.pitches[(size_t) j]));
        sum += best;
    }
    return sum * 0.5;
}

int nextAbove(int previous, int pc) // the lowest pitch with pitch class pc strictly above `previous`
{
    int p = previous + 1;
    p += floorMod(pc - p, 12);
    return p;
}
} // namespace

Voicing voice(const ChordTones& chord, const VoicingRequest& rq, const Voicing* previous)
{
    Voicing result;
    if (chord.count <= 0) return result;
    const int voices = std::clamp(rq.voices, 1, 8);

    // The tones that sound: by priority root, third, 7th/6th, 9th, 11th, fifth (the fifth goes first when voices are few).
    std::array<int, 6> chosen{};
    int numChosen = 0;
    {
        const bool powerChord = chord.count == 2 && ! chord.third;
        const int order[6] = {0, 1, 3, 4, 5, 2};
        for (int i = 0; i < 6 && numChosen < std::min(voices, chord.count); ++i)
        {
            const int tone = powerChord ? std::min(order[i], 1) : order[i];
            if (tone >= chord.count) continue;
            bool dup = false;
            for (int k = 0; k < numChosen; ++k) dup |= chosen[(size_t) k] == tone;
            if (! dup) chosen[(size_t) numChosen++] = tone;
        }
    }
    // Distinct pitch classes of the chosen tones, ascending from the root.
    std::array<int, 6> cycle{};
    int cycleLength = 0;
    for (int k = 0; k < numChosen; ++k)
    {
        const int pc = chord.pitchClass(chosen[(size_t) k]);
        bool dup = false;
        for (int j = 0; j < cycleLength; ++j) dup |= cycle[(size_t) j] == pc;
        if (! dup) cycle[(size_t) cycleLength++] = pc;
    }
    std::sort(cycle.begin(), cycle.begin() + cycleLength,
              [&](int a, int b) { return floorMod(a - chord.root, 12) < floorMod(b - chord.root, 12); });
    const int fifthPc = (chord.root + 7) % 12;
    const bool hasFifth = chord.contains(fifthPc);
    // The chord's own fifth (diminished or augmented when that is what it has), for the Fifths style.
    int fifthInterval = 7;
    if (! hasFifth)
        for (int candidate : {6, 8})
            if (chord.contains(chord.root + candidate)) { fifthInterval = candidate; break; }

    const int topPc = rq.topTone >= 0 ? chord.pitchClass(rq.topTone) : -1;
    const int lowPc = rq.inversion >= 0 ? chord.pitchClass(std::min(rq.inversion, chord.count - 1)) : -1;

    for (int pass = 0; pass < 4; ++pass)
    {
        bool found = false;
        double bestCost = 1e30;
        Candidate best;
        auto consider = [&](Candidate c, bool close) {
            c.sort();
            if (c.n <= 0 || ! inRange(c, rq)) return;
            for (int i = 1; i < c.n; ++i)
                if (c.p[(size_t) i] == c.p[(size_t) i - 1]) return; // no unisons (Open / Drop 2 over doubled tones)
            if (pass < 3 && ! lowRuleHolds(c, chord)) return;
            if (pass < 2 && close && lowPc >= 0 && pitchClassOf(c.p[0]) != lowPc) return;
            if (pass < 1 && topPc >= 0 && pitchClassOf(c.p[(size_t) c.n - 1]) != topPc) return;
            double mean = 0;
            for (int i = 0; i < c.n; ++i) mean += c.p[(size_t) i];
            mean /= c.n;
            double cost = std::abs(mean - rq.centre);
            if (previous != nullptr && previous->count > 0) cost = motion(c, *previous) + 0.25 * cost;
            cost += 0.01 * (c.p[(size_t) c.n - 1] - c.p[0]);
            if (cost < bestCost - 1e-9)
            {
                bestCost = cost;
                best = c;
                found = true;
            }
        };

        for (int octave = -1; octave <= 10; ++octave)
        {
            switch (rq.style)
            {
                case VoicingStyle::Close:
                case VoicingStyle::Open:
                case VoicingStyle::Drop2:
                    for (int r = 0; r < cycleLength; ++r)
                    {
                        Candidate c;
                        c.p[0] = 12 * octave + cycle[(size_t) r];
                        c.n = 1;
                        for (int v = 1; v < voices; ++v, ++c.n)
                            c.p[(size_t) v] = nextAbove(c.p[(size_t) v - 1], cycle[(size_t) ((r + v) % cycleLength)]);
                        const Candidate close = c;
                        if (rq.style == VoicingStyle::Open && c.n >= 3)
                        {
                            c.p[1] += 12;
                            if (c.n >= 5) c.p[3] += 12;
                        }
                        else if (rq.style == VoicingStyle::Drop2 && c.n >= 3)
                            c.p[(size_t) c.n - 2] -= 12;
                        c.sort();
                        for (int i = 1; i < c.n; ++i)
                            if (c.p[(size_t) i] == c.p[(size_t) i - 1]) { c = close; break; } // doubled tones: stay close
                        consider(c, true);
                    }
                    break;
                case VoicingStyle::Wide:
                {
                    // Upper tones: every chosen tone, starting from the third (so the third and colours lead, then the
                    // fifth and the root come back above; never the same tone three times over).
                    std::array<int, 6> upper{};
                    int numUpper = 0;
                    for (int j = 0; j < cycleLength; ++j)
                        if (cycle[(size_t) j] != chord.root && cycle[(size_t) j] != fifthPc) upper[(size_t) numUpper++] = cycle[(size_t) j];
                    for (int j = 0; j < cycleLength; ++j)
                        if (cycle[(size_t) j] == fifthPc || cycle[(size_t) j] == chord.root) upper[(size_t) numUpper++] = cycle[(size_t) j];
                    if (topPc >= 0 && pass < 1 && std::find(upper.begin(), upper.begin() + numUpper, topPc) == upper.begin() + numUpper)
                        upper[(size_t) numUpper++] = topPc; // a requested top tone joins the upper voices
                    if (numUpper == 0) for (int j = 0; j < cycleLength; ++j) upper[(size_t) numUpper++] = cycle[(size_t) j];
                    for (int r = 0; r < numUpper; ++r)
                    {
                        Candidate c;
                        c.p[(size_t) c.n++] = 12 * octave + chord.root;
                        if (hasFifth && voices >= 3) c.p[(size_t) c.n++] = c.p[0] + 7;
                        int last = c.p[0] + 12;
                        for (int k = 0; c.n < voices; ++k)
                        {
                            last = nextAbove(last, upper[(size_t) ((r + k) % numUpper)]);
                            c.p[(size_t) c.n++] = last;
                        }
                        consider(c, false);
                    }
                    break;
                }
                case VoicingStyle::Fifths:
                {
                    Candidate c;
                    const int base = 12 * octave + chord.root;
                    for (int v = 0; v < voices; ++v) c.p[(size_t) c.n++] = base + 12 * (v / 2) + (v % 2 == 1 ? fifthInterval : 0);
                    if (topPc >= 0 && topPc != chord.root && topPc != (chord.root + fifthInterval) % 12 && c.n >= 2)
                        c.p[(size_t) c.n - 1] = nextAbove(c.p[(size_t) c.n - 2], topPc);
                    consider(c, false);
                    break;
                }
                case VoicingStyle::Count: break;
            }
        }
        if (found)
        {
            for (int i = 0; i < best.n; ++i) result.pitches[(size_t) i] = (int8_t) best.p[(size_t) i];
            result.count = best.n;
            break;
        }
    }
    if (result.count == 0) // no candidate in range at all: one root near the centre
    {
        result.pitches[0] = (int8_t) std::clamp(rq.centre - floorMod(rq.centre - chord.root, 12), 0, 127);
        result.count = 1;
    }
    if (chord.bass >= 0 && pitchClassOf(result.pitches[0]) != chord.bass && result.count < Voicing::kMax)
    {
        int bass = result.pitches[0] - 1;
        bass -= floorMod(bass - chord.bass, 12);
        if (bass >= 0)
        {
            for (int i = result.count; i > 0; --i) result.pitches[(size_t) i] = result.pitches[(size_t) i - 1];
            result.pitches[0] = (int8_t) bass;
            ++result.count;
        }
    }
    return result;
}

// -------------------------------------------------------------------------------------------------------- Progression
double Progression::lengthBeats() const
{
    double total = 0;
    for (int i = 0; i < std::clamp(count, 1, kMaxSlots); ++i) total += std::max(0.0, slots[(size_t) i].beats);
    return total;
}

Progression::Position Progression::at(double beat) const
{
    const int n = std::clamp(count, 1, kMaxSlots);
    const double total = lengthBeats();
    if (total <= 0) return {0, beat, beat};
    const double cycleStart = grid::startOf(beat, total);
    const double position = beat - cycleStart;
    double acc = 0;
    int lastPlaying = 0;
    for (int i = 0; i < n; ++i)
    {
        const double length = std::max(0.0, slots[(size_t) i].beats);
        if (length <= 0) continue;
        lastPlaying = i;
        if (position < acc + length - grid::kEpsilon) return {i, cycleStart + acc, cycleStart + acc + length};
        acc += length;
    }
    const double length = std::max(0.0, slots[(size_t) lastPlaying].beats);
    return {lastPlaying, cycleStart + total - length, cycleStart + total};
}

// ------------------------------------------------------------------------------------------------- held notes, names
ChordTones chordFromHeldNotes(const int* pitches, int count)
{
    ChordTones t;
    if (pitches == nullptr || count <= 0) return t;
    int mask = 0, lowest = 1000;
    for (int i = 0; i < count; ++i)
    {
        if (pitches[i] < 0 || pitches[i] > 127) continue;
        mask |= 1 << pitchClassOf(pitches[i]);
        lowest = std::min(lowest, pitches[i]);
    }
    if (mask == 0) return t;
    const int lowestPc = pitchClassOf(lowest);
    if (popcount(mask) == 1)
    {
        t.root = lowestPc;
        t.count = 2;
        t.intervals[1] = 7;
        t.third = false;
        return t;
    }
    double bestScore = -1e30;
    int bestRoot = lowestPc;
    const Shape* bestShape = &kShapes[0];
    for (int root = 0; root < 12; ++root)
    {
        if (((mask >> root) & 1) == 0) continue;
        const int rel = rotate(mask, root);
        for (size_t s = 0; s < kShapes.size(); ++s)
        {
            const auto& shape = kShapes[s];
            const int need = requiredMask(shape), full = maskOf(shape.intervals.data(), shape.count);
            const int matched = popcount(need & rel), missing = popcount(need & ~rel), extra = popcount(rel & ~full);
            const double score = 3.0 * matched - 3.0 * missing - 4.0 * extra + (root == lowestPc ? 0.5 : 0.0) - 0.001 * (double) s;
            if (score > bestScore)
            {
                bestScore = score;
                bestRoot = root;
                bestShape = &shape;
            }
        }
    }
    t.root = bestRoot;
    t.count = bestShape->count;
    t.third = bestShape->third;
    for (int i = 0; i < t.count; ++i) t.intervals[(size_t) i] = bestShape->intervals[(size_t) i];
    if (lowestPc != bestRoot) t.bass = lowestPc;
    return t;
}

const char* pitchClassName(int pc, bool flats)
{
    static constexpr const char* sharpNames[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    static constexpr const char* flatNames[12] = {"C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B"};
    return (flats ? flatNames : sharpNames)[floorMod(pc, 12)];
}

std::string chordName(const ChordTones& t, bool flats)
{
    if (t.count <= 0) return {};
    std::string name = pitchClassName(t.root, flats);
    const int mask = maskOf(t.intervals.data(), t.count);
    const char* suffix = nullptr;
    for (const auto& shape : kShapes)
        if (shape.required == shape.count && maskOf(shape.intervals.data(), shape.count) == mask)
        {
            suffix = shape.suffix;
            break;
        }
    if (suffix == nullptr)
    {
        // Not a named shape: the triad's name and the added tones.
        const int triad = maskOf(t.intervals.data(), std::min(3, t.count));
        suffix = "";
        for (const auto& shape : kShapes)
            if (shape.required == shape.count && shape.count == std::min(3, t.count) && maskOf(shape.intervals.data(), shape.count) == triad)
            {
                suffix = shape.suffix;
                break;
            }
        name += suffix;
        for (int i = 3; i < t.count; ++i) name += "+" + std::to_string((int) t.intervals[(size_t) i]);
    }
    else
        name += suffix;
    if (t.bass >= 0) name += std::string("/") + pitchClassName(t.bass, flats);
    return name;
}
} // namespace sonora::model
