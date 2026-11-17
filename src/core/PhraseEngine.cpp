#include "core/PhraseEngine.h"

#include "core/PatternRandom.h"
#include "core/Scales.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace sonora::model
{
namespace
{
constexpr int floorMod(int a, int b) { return ((a % b) + b) % b; }
constexpr double kNever = std::numeric_limits<double>::infinity();
constexpr double kEps = 1e-9;

// Purposes for PatternRandom draws: a decision on a step never shares its value with another one.
enum Purpose : int64_t
{
    PChance = 11, PHumanTime, PHumanVelocity, PColour, PColourPick, PBreathe, PBreatheDir, PShimmer, PShimmerTone,
    PShimmerVelocity, PRank, PRhythm, PShape, PVelocity, PGate, PAccent, PTension, PPassing, PPassingDir, PVariation,
    POctave, PMove, PMoveKind, PHits, PTop, PJump, PFifth, PPad, PMutate, PMutateKind, PSeed
};

double secondsToBeats(double seconds, double tempo) { return seconds * std::max(1.0, tempo) / 60.0; }

int placeRoot(int rootPc, int key, int octave) { return pitchOf(floorMod(key, 12), octave) + floorMod(rootPc - key, 12); }

int fifthInterval(const ChordTones& t)
{
    for (int candidate : {7, 6, 8})
        if (t.contains(t.root + candidate)) return candidate;
    return 7;
}

int bassInterval(const ChordTones& t, int tone)
{
    switch (tone)
    {
        case 0: return 0;
        case 1: return fifthInterval(t);
        case 2: return 12;
        default: return t.interval(tone);
    }
}

int clampPitch(int p) { return std::clamp(p, 0, 127); }

// The chords of one render call: tones per progression slot (or the held chord), and voicing chains per style.
struct ChordCache
{
    const PhraseLive* live = nullptr;
    const PhraseChords* chords = nullptr;
    std::array<ChordTones, Progression::kMaxSlots> tones{};
    int count = 0;

    struct At
    {
        const ChordTones* tones = nullptr;
        int index = 0;
        double start = -kNever, end = kNever;
    };

    void build(const PhraseLive& l, const PhraseChords& c)
    {
        live = &l;
        chords = &c;
        if (c.fromHeld || c.progression == nullptr) return;
        count = std::clamp(c.progression->count, 1, Progression::kMaxSlots);
        for (int i = 0; i < count; ++i) tones[(size_t) i] = chordTones(c.progression->slots[(size_t) i].chord, l.key, l.scale);
    }
    At at(double beat) const
    {
        At a;
        if (chords->fromHeld)
        {
            if (chords->held.count > 0) a.tones = &chords->held;
            return a;
        }
        if (chords->progression == nullptr) return a;
        const auto p = chords->progression->at(beat);
        a.index = p.index;
        a.start = p.start;
        a.end = p.end;
        a.tones = &tones[(size_t) std::clamp(p.index, 0, count - 1)];
        return a;
    }
};

// Least-motion voicings along the progression for one voicing request (the base the stabs' top lines are voiced from).
struct VoicingChain
{
    std::array<Voicing, Progression::kMaxSlots> voicings{};
    VoicingRequest request;
    bool valid = false;

    void build(const ChordCache& cache, const VoicingRequest& rq)
    {
        request = rq;
        valid = true;
        if (cache.chords->fromHeld)
        {
            voicings[0] = voice(cache.chords->held, rq);
            return;
        }
        for (int i = 0; i < cache.count; ++i)
            voicings[(size_t) i] = voice(cache.tones[(size_t) i], rq, i > 0 ? &voicings[(size_t) i - 1] : nullptr);
    }
};

bool sameRequest(const VoicingRequest& a, const VoicingRequest& b)
{
    return a.style == b.style && a.voices == b.voices && a.centre == b.centre && a.lowLimit == b.lowLimit && a.highLimit == b.highLimit;
}

struct Output
{
    PhraseEvent* events;
    int capacity, count = 0;
    double from, to;
    void note(double beat, double length, int pitch, int velocity)
    {
        if (! (beat >= from && beat < to) || count >= capacity || pitch < 0 || pitch > 127) return;
        events[(size_t) count++] = {beat, std::max(0.005, length), pitch, std::clamp(velocity, 1, 127)};
    }
};

int adjust(int pitch, int octaves, const PhraseLive& live)
{
    pitch += 12 * octaves;
    if (live.shift != 0) pitch = transposeInScale(pitch, live.shift, floorMod(live.key, 12), scaleAt(live.scale));
    return pitch;
}

// ---------------------------------------------------------------------------------------------- Arp, Stabs, Bass
void renderSteps(const PhraseState& state, const PhraseLive& live, const ChordCache& cache, Output& out)
{
    const auto& part = state.part(live.part);
    const double humanBeats = secondsToBeats(0.008, live.tempo) * live.humanize;
    const double lo = out.from - 1.6, hi = out.to + humanBeats + kEps; // the latest a step can move is a third of 1/1 + strum
    VoicingChain chain;
    auto seg = phraseSegmentAt(part, live.pattern, lo);
    for (int guard = 0; guard < 4096 && seg.start < hi; ++guard)
    {
        const auto& pat = part.slots[(size_t) std::clamp(seg.slot, 0, kPhraseSlots - 1)];
        const int length = std::clamp((int) pat.length, 1, kPhraseSteps);
        const double sb = pat.stepBeats();
        std::array<bool, kPhraseSteps> plays{};
        phraseDensityMask(pat, live.density, plays);
        if (live.part == PhrasePart::Stabs)
        {
            VoicingRequest rq;
            rq.style = pat.voicing;
            rq.voices = std::clamp((int) pat.voices, 3, 6);
            rq.centre = pitchOf(floorMod(live.key, 12), pat.octave) + 7;
            if (! chain.valid || ! sameRequest(chain.request, rq)) chain.build(cache, rq);
        }
        const auto range = grid::stepsIn(std::max(lo, seg.start) - seg.start, std::min(hi, seg.start + seg.length) - seg.start, sb);
        for (int64_t i64 = std::max<int64_t>(0, range.first); i64 < std::min<int64_t>(length, range.end); ++i64)
        {
            const int i = (int) i64;
            const auto& st = pat.steps[(size_t) i];
            if (st.state != StepState::Note || ! plays[(size_t) i]) continue;
            const double straight = seg.start + i * sb;
            if (straight < lo || straight >= hi) continue;
            if (! PatternRandom::chance(st.chance / 100.0, live.seed, seg.occurrence, i, PChance)) continue;
            const auto at = cache.at(straight + kEps);
            if (at.tones == nullptr || at.tones->count <= 0) continue;
            int ties = 0;
            while (i + 1 + ties < length && pat.steps[(size_t) (i + 1 + ties)].state == StepState::Tie) ++ties;
            const double swing = (i % 2 == 1) ? std::clamp((double) live.swing, 0.0, 1.0) * sb / 3.0 : 0.0;
            const double onset = straight + swing + PatternRandom::bipolar(live.seed, seg.occurrence, i, PHumanTime) * humanBeats;
            double velocity = st.velocity * (double) live.velocity + (st.accent ? live.accent : 0)
                              + PatternRandom::bipolar(live.seed, seg.occurrence, i, PHumanVelocity) * 10.0 * live.humanize;
            if (grid::positionIn(straight, 1.0) < 0.25 - kEps) // the kick's sixteenth
            {
                if (live.kickGuard >= 0.995f) continue;
                velocity *= 1.0 - std::clamp((double) live.kickGuard, 0.0, 1.0);
            }
            const double ownGate = st.gate / 100.0 * live.gate * sb;
            double noteLength = ties * sb + ownGate;
            if (st.slide) noteLength = std::max(noteLength, (ties + 1) * sb + 0.05);
            if (at.end < kNever) noteLength = std::min(noteLength, std::max(at.end - onset, ownGate)); // a tie stops at a chord change
            const int vel = (int) std::lround(velocity);
            if (live.part == PhrasePart::Stabs)
            {
                // The top line is a melody: the top note sits on the ladder (root, third, fifth, octave...) above the chord root,
                // so a rising contour rises; the voices below take the least motion from the progression's chain.
                auto rq = chain.request;
                rq.topTone = st.top;
                const int topPitch = placeRoot(at.tones->root, live.key, pat.octave) + at.tones->interval(st.top);
                rq.highLimit = topPitch;
                rq.centre = topPitch - 6;
                const auto& base = chain.voicings[(size_t) std::clamp(at.index, 0, Progression::kMaxSlots - 1)];
                const auto v = voice(*at.tones, rq, &base);
                const double strum = secondsToBeats(pat.strumMs / 1000.0, live.tempo);
                for (int k = 0; k < v.count; ++k)
                {
                    const int order = pat.strumDown ? v.count - 1 - k : k;
                    const double offset = v.count > 1 ? strum * order / (v.count - 1) : 0.0;
                    out.note(onset + offset, std::max(0.01, noteLength - offset), clampPitch(adjust(v.pitches[(size_t) k], st.octave + live.octave, live)), vel);
                }
                continue;
            }
            const int root = placeRoot(at.tones->root, live.key, pat.octave);
            int pitch = root + (live.part == PhrasePart::Bass ? bassInterval(*at.tones, st.tone) : at.tones->interval(st.tone));
            if (st.scaleStep != 0) pitch = transposeInScale(pitch, st.scaleStep, floorMod(live.key, 12), scaleAt(live.scale));
            out.note(onset, noteLength, clampPitch(adjust(pitch, st.octave + live.octave, live)), vel);
        }
        const double next = seg.start + seg.length;
        if (seg.length <= 0) break;
        seg = phraseSegmentAt(part, live.pattern, next);
        if (seg.start < next - 1e-6) break; // no progress (cannot happen with positive lengths)
    }
}

// --------------------------------------------------------------------------------------------------------- Pad
// One colour tone (add9, the 2nd for the third, 7th, 11th) inside the scale, never a minor ninth above a chord tone.
ChordTones withColour(ChordTones t, const PhraseLive& live, int64_t cycle, int slot, int chancePercent)
{
    if (! PatternRandom::chance(chancePercent / 100.0, live.seed, cycle, slot, PColour)) return t;
    struct Option { int interval; bool replaceThird; };
    std::array<Option, 4> options{};
    int n = 0;
    auto ok = [&](int interval) {
        if (! inScale(t.root + interval, floorMod(live.key, 12), live.scale)) return false;
        for (int i = 0; i < t.count; ++i)
            if (floorMod(interval - t.intervals[(size_t) i], 12) == 1) return false;
        return ! t.contains(t.root + interval);
    };
    if (ok(14)) options[(size_t) n++] = {14, false};
    if (t.third && t.count >= 3 && inScale(t.root + 2, floorMod(live.key, 12), live.scale) && ! t.contains(t.root + 2))
        options[(size_t) n++] = {2, true};
    for (int seventh : {10, 11})
        if (ok(seventh)) { options[(size_t) n++] = {seventh, false}; break; }
    if (ok(17)) options[(size_t) n++] = {17, false};
    if (n == 0) return t;
    const auto pick = options[(size_t) PatternRandom::range(0, n - 1, live.seed, cycle, slot, PColourPick)];
    if (pick.replaceThird)
    {
        t.intervals[1] = (int8_t) pick.interval;
        t.third = false;
        return t;
    }
    if (t.count < (int) t.intervals.size()) t.intervals[(size_t) t.count++] = (int8_t) pick.interval;
    return t;
}

bool holds(const Voicing& v, int pitch)
{
    for (int i = 0; i < v.count; ++i)
        if (v.pitches[(size_t) i] == pitch) return true;
    return false;
}

// A voice of `v` (an inner one) moved: to the nearest chord tone within a fourth that is not sounding yet, or, when the
// voicing is too dense for that, by one scale step (a suspension) that makes no unison and no minor ninth above a
// chord tone. The result is sorted again.
Voicing breathe(const Voicing& v, const ChordTones& t, const PhraseLive& live, int64_t cycle, int slot, int sub)
{
    if (v.count < 3) return v;
    const int voiceIndex = PatternRandom::range(1, v.count - 2, live.seed, cycle, slot * 64 + sub, PBreathe);
    const bool up = PatternRandom::chance(0.5, live.seed, cycle, slot * 64 + sub, PBreatheDir);
    const int p = v.pitches[(size_t) voiceIndex];
    auto withMove = [&](int to) {
        auto moved = v;
        moved.pitches[(size_t) voiceIndex] = (int8_t) to;
        std::sort(moved.pitches.begin(), moved.pitches.begin() + moved.count);
        return moved;
    };
    for (int d = 1; d <= 5; ++d)
        for (int sign : {up ? 1 : -1, up ? -1 : 1})
        {
            const int candidate = p + sign * d;
            if (t.contains(candidate) && ! holds(v, candidate) && candidate >= kThirdFloor) return withMove(candidate);
        }
    const int key = floorMod(live.key, 12);
    for (int sign : {up ? 1 : -1, up ? -1 : 1})
    {
        const int candidate = transposeInScale(p, sign, key, scaleAt(live.scale));
        bool clash = holds(v, candidate) || candidate < kThirdFloor;
        for (int i = 0; i < t.count && ! clash; ++i) clash = floorMod(candidate - t.root - t.intervals[(size_t) i], 12) == 1;
        if (! clash) return withMove(candidate);
    }
    return v;
}

struct SubSpan
{
    double start = 0, end = 0; // song beats
    Voicing voicing;
    ChordTones tones;
    const PhrasePattern* pattern = nullptr;
    int slot = 0, sub = 0;
    bool restrike = false;      // Pulse: nothing is held into it
    bool chordStart = false;
};

void renderPad(const PhraseState& state, const PhraseLive& live, const ChordCache& cache, Output& out)
{
    const auto& part = state.part(PhrasePart::Pad);
    if (live.density <= 0.0f) return;
    const double strumMax = secondsToBeats(0.25, live.tempo) + secondsToBeats(0.008, live.tempo);
    // Humanize moves onsets up to 8 ms either way: a chord just after the span can start inside it.
    const double lo = out.from - strumMax - kEps, hi = out.to + secondsToBeats(0.008, live.tempo) + kEps;

    // The chord occurrences of one cycle: the progression's lap, or the held chord from when it began.
    auto renderCycle = [&](int64_t cycle, double cycleStart, double cycleEnd) {
        static constexpr int kMaxSubs = 96;
        std::array<SubSpan, kMaxSubs> subs;
        int numSubs = 0;
        Voicing previous;
        const int slots = cache.chords->fromHeld ? 1 : cache.count;
        double offset = cycleStart;
        for (int j = 0; j < slots && numSubs < kMaxSubs; ++j)
        {
            const double length = cache.chords->fromHeld ? cycleEnd - cycleStart : std::max(0.0, cache.chords->progression->slots[(size_t) j].beats);
            const double s = offset, e = offset + length;
            offset = e;
            if (length <= 0) continue;
            const auto& pat = part.slots[(size_t) std::clamp(phraseSegmentAt(part, live.pattern, s + kEps).slot, 0, kPhraseSlots - 1)];
            const auto& base = cache.chords->fromHeld ? cache.chords->held : cache.tones[(size_t) j];
            const auto tones = withColour(base, live, 0, j, pat.colour); // the same colour every lap
            VoicingRequest rq;
            rq.style = pat.padVoicing;
            rq.voices = std::max(1, (int) std::lround(std::clamp((int) pat.padVoices, 3, 7) * std::clamp((double) live.density, 0.0, 1.0)));
            rq.centre = pat.padCentre;
            rq.lowLimit = 36;
            const auto v = voice(tones, rq, previous.count > 0 ? &previous : nullptr);
            previous = v;
            const double subLength = pat.motion == PadMotion::Hold ? length : std::max(1, (int) pat.motionBars) * 4.0;
            int sub = 0;
            for (double t = s; t < e - kEps && numSubs < kMaxSubs; t += subLength, ++sub)
            {
                auto& span = subs[(size_t) numSubs++];
                span.start = t;
                span.end = std::min(e, t + subLength);
                span.tones = tones;
                span.pattern = &pat;
                span.slot = j;
                span.sub = sub;
                span.chordStart = sub == 0;
                span.restrike = pat.motion == PadMotion::Pulse;
                span.voicing = pat.motion == PadMotion::Breathe && sub % 2 == 1 ? breathe(v, tones, live, 0, j, sub) : v;
            }
        }
        for (int m = 0; m < numSubs; ++m)
        {
            const auto& span = subs[(size_t) m];
            if (span.end < lo || span.start - strumMax > hi) continue;
            const auto& pat = *span.pattern;
            const double strum = secondsToBeats(pat.padStrumMs / 1000.0, live.tempo);
            const double overlap = pat.overlap / 100.0;
            const int velocity = (int) std::lround(pat.padVelocity * live.velocity);
            const auto& v = span.voicing;
            for (int k = 0; k < v.count; ++k)
            {
                const int pitch = v.pitches[(size_t) k];
                if (m > 0 && ! span.restrike && holds(subs[(size_t) m - 1].voicing, pitch)) continue; // held from before
                double end = span.end;
                for (int next = m + 1; next < numSubs && ! subs[(size_t) next].restrike && holds(subs[(size_t) next].voicing, pitch); ++next)
                    end = subs[(size_t) next].end;
                const double onset = span.start + (v.count > 1 ? strum * k / (v.count - 1) : 0.0)
                                     + PatternRandom::bipolar(live.seed, cycle, m * 16 + k, PHumanTime) * secondsToBeats(0.008, live.tempo) * live.humanize;
                const int vel = velocity + (int) std::lround(PatternRandom::bipolar(live.seed, cycle, m * 16 + k, PHumanVelocity) * 8.0 * live.humanize);
                out.note(onset, end - onset + overlap, clampPitch(adjust(pitch, live.octave, live)), vel);
            }
            if (span.chordStart && pat.padBass != PadBass::Off && v.count > 0)
            {
                int bass = v.lowest() - 12;
                bass -= floorMod(bass - span.tones.root, 12);
                while (bass < 28) bass += 12;
                if (bass >= v.lowest()) bass = -1; // the voicing already has its root at the bottom there
                double chordEnd = span.end;
                for (int next = m + 1; next < numSubs && ! subs[(size_t) next].chordStart; ++next) chordEnd = subs[(size_t) next].end;
                if (bass >= 0) out.note(span.start, chordEnd - span.start + overlap, clampPitch(adjust(bass, live.octave, live)), velocity);
                if (bass >= 0 && pat.padBass == PadBass::RootFifth)
                    out.note(span.start, chordEnd - span.start + overlap, clampPitch(adjust(bass + fifthInterval(span.tones), live.octave, live)), velocity - 6);
            }
            if (pat.shimmer > 0 && v.count > 0)
                for (double beat = std::floor(span.start) + 0.5; beat < span.end; beat += 1.0)
                {
                    if (beat < span.start) continue;
                    const auto beatIndex = (int64_t) std::floor(beat - cycleStart); // the same sparkle every lap
                    if (! PatternRandom::chance(pat.shimmer / 100.0 * 0.4 * live.density, live.seed, 0, beatIndex, PShimmer)) continue;
                    const int tone = PatternRandom::range(0, span.tones.count - 1, live.seed, 0, beatIndex, PShimmerTone);
                    int pitch = v.highest() + 1;
                    pitch += floorMod(span.tones.pitchClass(tone) - pitch, 12);
                    const int vel = 40 + PatternRandom::range(0, 12, live.seed, 0, beatIndex, PShimmerVelocity);
                    out.note(beat, 0.45, clampPitch(adjust(pitch, live.octave, live)), vel);
                }
        }
    };

    if (cache.chords->fromHeld)
    {
        if (cache.chords->held.count <= 0) return;
        renderCycle(0, cache.chords->heldSince, cache.chords->heldSince + 4096.0);
        return;
    }
    if (cache.chords->progression == nullptr) return;
    const double lap = cache.chords->progression->lengthBeats();
    if (lap <= 0) return;
    for (int64_t c = grid::indexAt(lo, lap); (double) c * lap < hi; ++c) renderCycle(c, (double) c * lap, (double) (c + 1) * lap);
}
} // namespace

// ------------------------------------------------------------------------------------------------------ public
double PhrasePartState::formBeats() const
{
    double total = 0;
    for (int j = 0; j < std::clamp((int) formLength, 1, kPhraseFormMax); ++j)
        total += slots[(size_t) std::clamp((int) form[(size_t) j], 0, kPhraseSlots - 1)].lengthBeats();
    return total;
}

PhraseSegment phraseSegmentAt(const PhrasePartState& part, int pattern, double beat)
{
    if (pattern >= 1 && pattern <= kPhraseSlots)
    {
        const int slot = pattern - 1;
        const double length = part.slots[(size_t) slot].lengthBeats();
        const auto k = grid::indexAt(beat, length);
        return {slot, k, (double) k * length, length};
    }
    const int n = std::clamp((int) part.formLength, 1, kPhraseFormMax);
    const double lap = part.formBeats();
    const auto c = grid::indexAt(beat, lap);
    const double position = beat - (double) c * lap;
    double acc = 0;
    for (int j = 0; j < n; ++j)
    {
        const int slot = std::clamp((int) part.form[(size_t) j], 0, kPhraseSlots - 1);
        const double length = part.slots[(size_t) slot].lengthBeats();
        if (position < acc + length - grid::kEpsilon || j == n - 1) return {slot, c * n + j, (double) c * lap + acc, length};
        acc += length;
    }
    return {0, 0, 0, lap};
}

int phraseRender(const PhraseState& state, const PhraseLive& live, const PhraseChords& chords, double fromBeat, double toBeat,
                 PhraseEvent* out, int capacity)
{
    if (! (toBeat > fromBeat) || out == nullptr || capacity <= 0 || live.density <= 0.0f) return 0;
    ChordCache cache;
    cache.build(live, chords);
    Output o{out, capacity, 0, fromBeat, toBeat};
    if (live.part == PhrasePart::Pad) renderPad(state, live, cache, o);
    else renderSteps(state, live, cache, o);
    std::sort(out, out + o.count, [](const PhraseEvent& a, const PhraseEvent& b) {
        return a.beat < b.beat || (! (b.beat < a.beat) && a.pitch < b.pitch);
    });
    return o.count;
}

void phraseDensityMask(const PhrasePattern& pattern, float density, std::array<bool, kPhraseSteps>& out)
{
    out.fill(false);
    const int length = std::clamp((int) pattern.length, 1, kPhraseSteps);
    std::array<int, kPhraseSteps> order{};
    int n = 0;
    for (int i = 0; i < length; ++i)
        if (pattern.steps[(size_t) i].state == StepState::Note) order[(size_t) n++] = i;
    std::sort(order.begin(), order.begin() + n, [&](int a, int b) {
        return pattern.steps[(size_t) a].rank < pattern.steps[(size_t) b].rank
               || (pattern.steps[(size_t) a].rank == pattern.steps[(size_t) b].rank && a < b);
    });
    const int keep = density >= 1.0f ? n : (int) std::lround(std::clamp((double) density, 0.0, 1.0) * n);
    for (int k = 0; k < keep; ++k) out[(size_t) order[(size_t) k]] = true;
}

void phraseRank(PhrasePattern& pattern, uint32_t seed)
{
    const int length = std::clamp((int) pattern.length, 1, kPhraseSteps);
    const double sb = pattern.stepBeats();
    int last = -1;
    for (int i = 0; i < length; ++i)
        if (pattern.steps[(size_t) i].state == StepState::Note) last = i;
    std::array<std::pair<double, int>, kPhraseSteps> keys{};
    int n = 0;
    for (int i = 0; i < length; ++i)
    {
        auto& st = pattern.steps[(size_t) i];
        if (st.state != StepState::Note) { st.rank = 255; continue; }
        const double at = i * sb;
        int group = 3;
        if (grid::positionIn(at, 4.0) < kEps || i == last) group = 0;
        else if (grid::positionIn(at, 1.0) < kEps || st.accent) group = 1;
        else if (grid::positionIn(at, 0.5) < kEps) group = 2;
        keys[(size_t) n++] = {group + PatternRandom::unit(seed, i, 0, PRank) * 0.9, i};
    }
    std::sort(keys.begin(), keys.begin() + n);
    for (int k = 0; k < n; ++k) pattern.steps[(size_t) keys[(size_t) k].second].rank = (uint8_t) std::min(k, 254);
}

// ---------------------------------------------------------------------------------------------------- generators
namespace
{
bool inSet(int position, std::initializer_list<int> set)
{
    for (int p : set)
        if (p == position) return true;
    return false;
}

bool euclid(int i, int hits, int steps)
{
    if (hits <= 0) return false;
    return (i * hits) % steps < hits;
}

// The question's ending: the last note on a chord tone that is not the root, short.
void endAsQuestion(PhrasePattern& pat, bool topLane, uint32_t seed)
{
    for (int i = std::clamp((int) pat.length, 1, kPhraseSteps) - 1; i >= 0; --i)
    {
        auto& st = pat.steps[(size_t) i];
        if (st.state != StepState::Note) continue;
        const int tone = PatternRandom::chance(0.5, seed, i, 0, PTop) ? 1 : 2;
        // On a beat it would sound settled: a sixteenth later when that step is free.
        const int length = std::clamp((int) pat.length, 1, kPhraseSteps);
        if (grid::positionIn(i * pat.stepBeats(), 1.0) < 1e-9 && i + 1 < length && pat.steps[(size_t) i + 1].state == StepState::Rest)
        {
            pat.steps[(size_t) i + 1] = st;
            st = PhraseStep{};
            endAsQuestion(pat, topLane, seed);
            return;
        }
        if (topLane) { if (st.top % 3 == 0) st.top = (int8_t) (st.top + tone); }
        else
        {
            st.tone = (int8_t) (st.tone % 3 == 0 ? st.tone + tone : st.tone);
            st.scaleStep = 0;
        }
        st.gate = std::min<uint16_t>(st.gate, 30);
        st.accent = false;
        st.slide = false;
        return;
    }
}

void generateArp(PhrasePartState& part, int slot, uint32_t seed, DiceKeep keep)
{
    const auto& style = part.arp;
    auto& pat = part.slots[(size_t) slot];
    const auto previous = pat;
    const bool triplets = style.rhythm == ArpRhythm::Triplets;
    int bars = style.bars >= 8 ? 4 : std::clamp((int) style.bars, 1, 4);
    const int perBar = triplets ? 24 : 16;
    bars = std::min(bars, kPhraseSteps / perBar);
    pat.rate = triplets ? (uint8_t) kRate1_16T : (uint8_t) kRate1_16;
    pat.length = (uint8_t) (bars * perBar);
    if (pat.octave < 3 || pat.octave > 5) pat.octave = 4;
    const int range = std::clamp((int) style.range, 1, 3);
    const int top = 3 * range; // the ladder index of the top root
    const auto r = [&](int64_t a, int64_t b, int64_t purpose) { return PatternRandom::unit(seed, a, b, purpose); };

    // Rhythm (bar 1, copied to the others below).
    std::array<bool, kPhraseSteps> on{};
    for (int i = 0; i < perBar; ++i)
    {
        const int q = triplets ? i : i % 16;
        switch (style.rhythm)
        {
            case ArpRhythm::Sixteenths: on[(size_t) i] = true; break;
            case ArpRhythm::Offbeats: on[(size_t) i] = q % 4 != 0; break;
            case ArpRhythm::ThreeThreeTwo: on[(size_t) i] = inSet(q % 8, {0, 3, 6}); break; // tied through below
            case ArpRhythm::Gallop: on[(size_t) i] = q % 4 != 1; break;
            case ArpRhythm::Euclid: on[(size_t) i] = euclid(q, std::clamp((int) style.euclidHits, 1, 16), 16); break;
            case ArpRhythm::Triplets: on[(size_t) i] = true; break;
            case ArpRhythm::Count: on[(size_t) i] = true; break;
        }
    }
    // 3-3-2: dotted eighths and an eighth, each note tied through its group.
    std::array<bool, kPhraseSteps> tie{};
    if (style.rhythm == ArpRhythm::ThreeThreeTwo)
        for (int i = 0; i < perBar; ++i) tie[(size_t) i] = ! on[(size_t) i];
    // Pitches along the note steps by shape.
    std::array<PhraseStep, kPhraseSteps> bar{};
    int k = 0, walk = 0;
    const int movedCount = 4 + PatternRandom::range(0, 2, seed, 0, 0, PMove); // Pedal Low: 4-6 of 16 move
    std::array<bool, kPhraseSteps> moved{};
    if (style.shape == ArpShape::PedalLow)
    {
        // Moved steps prefer the weak sixteenths; never the downbeat.
        std::array<std::pair<double, int>, kPhraseSteps> keys{};
        int n = 0;
        for (int i = 1; i < perBar; ++i)
            if (on[(size_t) i]) keys[(size_t) n++] = {(i % 2 == 1 ? 0.0 : 0.6) + r(i, 1, PMove), i};
        std::sort(keys.begin(), keys.begin() + n);
        for (int m = 0; m < std::min(n, movedCount * perBar / 16); ++m) moved[(size_t) keys[(size_t) m].second] = true;
    }
    for (int i = 0; i < perBar; ++i)
    {
        auto& st = bar[(size_t) i];
        if (! on[(size_t) i]) { st.state = tie[(size_t) i] ? StepState::Tie : StepState::Rest; continue; }
        st.state = StepState::Note;
        int tone = 0;
        switch (style.shape)
        {
            case ArpShape::Up: tone = k % top; break;
            case ArpShape::Down: tone = top - k % top; break;
            case ArpShape::UpDown: { const int lap = 2 * top; const int p = k % lap; tone = p <= top ? p : lap - p; break; }
            case ArpShape::PedalLow:
                if (moved[(size_t) i])
                {
                    // The fifth first, then the b2 / 2 neighbour, then the third; some an octave up.
                    const double u = r(i, 2, PMoveKind);
                    tone = u < 0.45 ? 2 : u < 0.75 ? 0 : 1;
                    if (u >= 0.45 && u < 0.75) st.scaleStep = 1;
                    if (r(i, 3, POctave) < 0.35 && range >= 2) tone += 3;
                }
                break;
            case ArpShape::PedalHigh: tone = k % 2 == 0 ? top : (k / 2) % top; break;
            case ArpShape::Thirds: tone = (k / 2) % top + (k % 2); break;
            case ArpShape::RootFifthOctave: { const int cycle[4] = {0, 2, 3, 2}; tone = cycle[k % 4]; break; }
            case ArpShape::Spiral: tone = (k / 2 + (k % 2 == 1 ? 2 : 0)) % (top + 1); break;
            case ArpShape::Walk:
                if (k > 0) walk += r(k, 4, PShape) < 0.5 ? -1 : 1;
                if (walk < 0) walk = 1;
                if (walk > top) walk = top - 1;
                tone = walk;
                break;
            case ArpShape::Count: break;
        }
        st.tone = (int8_t) std::clamp(tone, 0, 12);
        const bool pedal = style.shape == ArpShape::PedalLow && ! moved[(size_t) i];
        st.velocity = (uint8_t) (pedal ? 58 + PatternRandom::range(0, 6, seed, i, 0, PVelocity) : 74 + PatternRandom::range(0, 14, seed, i, 0, PVelocity));
        ++k;
    }
    // Accents.
    for (int i = 0; i < perBar; ++i)
    {
        auto& st = bar[(size_t) i];
        if (st.state != StepState::Note) continue;
        const int q = triplets ? i * 16 / perBar : i;
        switch (style.accents)
        {
            case ArpAccents::None: break;
            case ArpAccents::ThreeThreeTwo: st.accent = inSet(q % 16, {0, 3, 6, 8, 11, 14}); break;
            case ArpAccents::OffBeat: st.accent = inSet(q % 16, {2, 6, 10, 13}); break;
            case ArpAccents::PhraseEnd: break;
            case ArpAccents::Count: break;
        }
    }
    if (style.accents == ArpAccents::PhraseEnd)
        for (int i = perBar - 1; i >= 0; --i)
            if (bar[(size_t) i].state == StepState::Note) { bar[(size_t) i].accent = true; break; }
    for (int i = 0; i < perBar; ++i)
    {
        auto& st = bar[(size_t) i];
        if (st.state != StepState::Note) continue;
        if (st.accent) st.velocity = (uint8_t) std::max<int>(st.velocity, 100);
        if (style.accentGlide)
        {
            st.gate = st.accent ? (uint16_t) (90 + PatternRandom::range(0, 10, seed, i, 1, PGate)) : (uint16_t) (25 + PatternRandom::range(0, 15, seed, i, 1, PGate));
            st.slide = st.accent;
        }
        else
            st.gate = (uint16_t) (st.accent ? 60 : 30 + PatternRandom::range(0, 15, seed, i, 1, PGate));
    }
    // Passing tones on weak steps.
    for (int i = 1; i < perBar; i += 2)
    {
        auto& st = bar[(size_t) i];
        if (st.state != StepState::Note || st.scaleStep != 0) continue;
        if (r(i, 5, PPassing) < style.passing / 100.0) st.scaleStep = (int8_t) (r(i, 6, PPassingDir) < 0.5 ? -1 : 1);
    }
    // Copy bar 1 to every bar, then vary.
    PhrasePattern fresh = pat;
    for (int b = 0; b < bars; ++b)
        for (int i = 0; i < perBar; ++i) fresh.steps[(size_t) (b * perBar + i)] = bar[(size_t) i];
    for (int i = bars * perBar; i < kPhraseSteps; ++i) fresh.steps[(size_t) i] = PhraseStep{};
    for (int b = 1; b < bars; b += 2)
    {
        // Bars 2 and 4 turn: their last two beats re-voiced (more on bar 4).
        const double amount = style.variation / 100.0 * (b == 3 ? 1.0 : 0.7);
        for (int i = perBar / 2; i < perBar; ++i)
        {
            auto& st = fresh.steps[(size_t) (b * perBar + i)];
            if (st.state != StepState::Note || r(b * perBar + i, 7, PVariation) >= amount) continue;
            st.tone = (int8_t) std::clamp(st.tone + (r(b * perBar + i, 8, PVariation) < 0.5 ? 1 : 2), 0, top);
        }
    }
    if (bars >= 2 && style.variation > 0) // the highest note once per phrase
        for (int i = bars * perBar - perBar / 2; i < bars * perBar; ++i)
            if (fresh.steps[(size_t) i].state == StepState::Note && fresh.steps[(size_t) i].accent) { fresh.steps[(size_t) i].tone = (int8_t) top; break; }
    // Tension: an accented neighbour on step 7 or 15 resolving to the root on the next step.
    for (int b = 0; b < bars && ! triplets; ++b)
    {
        if (r(b, 9, PTension) >= style.tension / 100.0) continue;
        const int at = b * 16 + (r(b, 10, PTension) < 0.5 ? 6 : 14);
        auto& st = fresh.steps[(size_t) at];
        st.state = StepState::Note;
        st.tone = 0;
        st.scaleStep = 1;
        st.accent = true;
        st.velocity = 112;
        st.gate = style.accentGlide ? 95 : 55;
        st.slide = style.accentGlide;
        auto& resolve = fresh.steps[(size_t) (at + 1)];
        resolve.state = StepState::Note;
        resolve.tone = 0;
        resolve.scaleStep = 0;
        resolve.accent = false;
        resolve.velocity = 80;
        resolve.gate = 40;
    }
    endAsQuestion(fresh, false, seed);

    if (keep == DiceKeep::Rhythm && previous.length > 0)
    {
        // Keep the old rhythm; take the new pitches in order.
        std::array<int8_t, kPhraseSteps> tones{}, steps{};
        int n = 0;
        for (int i = 0; i < fresh.length; ++i)
            if (fresh.steps[(size_t) i].state == StepState::Note) { tones[(size_t) n] = fresh.steps[(size_t) i].tone; steps[(size_t) n++] = fresh.steps[(size_t) i].scaleStep; }
        auto kept = previous;
        int m = 0;
        for (int i = 0; i < kept.length && n > 0; ++i)
            if (kept.steps[(size_t) i].state == StepState::Note) { kept.steps[(size_t) i].tone = tones[(size_t) (m % n)]; kept.steps[(size_t) i].scaleStep = steps[(size_t) (m % n)]; ++m; }
        pat = kept;
        endAsQuestion(pat, false, seed);
        return;
    }
    if (keep == DiceKeep::Pitches && previous.length > 0)
    {
        std::array<int8_t, kPhraseSteps> tones{}, steps{};
        int n = 0;
        for (int i = 0; i < previous.length; ++i)
            if (previous.steps[(size_t) i].state == StepState::Note) { tones[(size_t) n] = previous.steps[(size_t) i].tone; steps[(size_t) n++] = previous.steps[(size_t) i].scaleStep; }
        int m = 0;
        for (int i = 0; i < fresh.length && n > 0; ++i)
            if (fresh.steps[(size_t) i].state == StepState::Note) { fresh.steps[(size_t) i].tone = tones[(size_t) (m % n)]; fresh.steps[(size_t) i].scaleStep = steps[(size_t) (m % n)]; ++m; }
    }
    phraseRank(fresh, seed);
    pat = fresh;
}

void generateStabs(PhrasePartState& part, int slot, uint32_t seed, DiceKeep keep)
{
    const auto& style = part.stabs;
    auto& pat = part.slots[(size_t) slot];
    const auto previous = pat;
    const int bars = std::clamp((int) style.bars, 1, 4);
    pat.rate = (uint8_t) kRate1_16;
    pat.length = (uint8_t) (bars * 16);
    if (pat.octave < 3 || pat.octave > 5) pat.octave = 4;
    PhrasePattern fresh = pat;
    for (auto& st : fresh.steps) st = PhraseStep{};
    const int hits = std::clamp((int) style.hits, 2, 8);
    std::array<int, kPhraseSteps> positions{};
    int total = 0;
    for (int b = 0; b < bars; ++b)
    {
        std::array<int, 16> mask{};
        int n = 0;
        auto add = [&](std::initializer_list<int> set) { for (int p : set) if (n < 16 && std::find(mask.begin(), mask.begin() + n, p) == mask.begin() + n) mask[(size_t) n++] = p; };
        switch (style.rhythm)
        {
            case StabRhythm::Offbeats: add({2, 6, 10, 14}); break;
            case StabRhythm::ThreeThreeTwo: add({0, 3, 6, 8, 11, 14}); break;
            case StabRhythm::Dotted: add({2, 5, 8, 11, 14}); break;
            case StabRhythm::Sparse: add({6, 14, 3}); break;
            case StabRhythm::Push: add({3, 7, 11, 15}); break;
            case StabRhythm::Count: break;
        }
        const int own = n; // the style's own positions come first
        add({2, 6, 10, 14, 3, 7, 11, 15, 1, 5, 9, 13}); // fill to the requested hits: offbeat eighths first
        // Keep `hits` of them: the style's own positions first (Sparse: its first two), then the fill in order; in time order.
        const int styleCount = style.rhythm == StabRhythm::Sparse ? std::min(own, 2) : own;
        std::array<std::pair<double, int>, 16> keys{};
        for (int m = 0; m < n; ++m)
            keys[(size_t) m] = {m < styleCount ? PatternRandom::unit(seed, b, mask[(size_t) m], PHits) : 1.0 + m * 0.01, mask[(size_t) m]};
        std::sort(keys.begin(), keys.begin() + n);
        std::array<int, 16> chosen{};
        const int take = std::min(hits, n);
        for (int m = 0; m < take; ++m) chosen[(size_t) m] = keys[(size_t) m].second;
        std::sort(chosen.begin(), chosen.begin() + take);
        for (int m = 0; m < take; ++m) positions[(size_t) total++] = b * 16 + chosen[(size_t) m];
    }
    for (int h = 0; h < total; ++h)
    {
        auto& st = fresh.steps[(size_t) positions[(size_t) h]];
        st.state = StepState::Note;
        const double t = total > 1 ? (double) h / (total - 1) : 0.0;
        double top = 2;
        switch (style.contour)
        {
            case StabContour::Rise: top = 1 + 3 * t; break;
            case StabContour::Fall: top = 4 - 3 * t; break;
            case StabContour::Arch: top = 1 + 3 * (1 - std::abs(2 * t - 1)); break;
            case StabContour::Flat: top = 2; break;
            case StabContour::Count: break;
        }
        st.top = (int8_t) std::clamp((int) std::lround(top), 0, 6);
        st.velocity = (uint8_t) (96 + PatternRandom::range(0, 12, seed, h, 0, PVelocity));
        st.gate = (uint16_t) (30 + PatternRandom::range(0, 12, seed, h, 0, PGate));
        // The first hit of each bar is accented, now and then.
        const bool firstInBar = h == 0 || positions[(size_t) h] / 16 != positions[(size_t) h - 1] / 16;
        st.accent = firstInBar && PatternRandom::chance(0.5, seed, h, 0, PAccent);
    }
    endAsQuestion(fresh, true, seed);
    if (keep == DiceKeep::Rhythm && previous.length > 0)
    {
        auto kept = previous;
        int m = 0;
        for (int i = 0; i < kept.length; ++i)
            if (kept.steps[(size_t) i].state == StepState::Note && total > 0)
                kept.steps[(size_t) i].top = fresh.steps[(size_t) positions[(size_t) (m++ % total)]].top;
        endAsQuestion(kept, true, seed);
        pat = kept;
        return;
    }
    if (keep == DiceKeep::Pitches && previous.length > 0)
    {
        std::array<int8_t, kPhraseSteps> tops{};
        int n = 0;
        for (int i = 0; i < previous.length; ++i)
            if (previous.steps[(size_t) i].state == StepState::Note) tops[(size_t) n++] = previous.steps[(size_t) i].top;
        for (int h = 0; h < total && n > 0; ++h) fresh.steps[(size_t) positions[(size_t) h]].top = tops[(size_t) (h % n)];
    }
    phraseRank(fresh, seed);
    pat = fresh;
}

void generateBass(PhrasePartState& part, int slot, uint32_t seed, DiceKeep keep)
{
    const auto& style = part.bass;
    auto& pat = part.slots[(size_t) slot];
    const auto previous = pat;
    int perBeat = 4;
    uint8_t rate = (uint8_t) kRate1_16;
    if (style.rhythm == BassRhythm::Gallop) { perBeat = 8; rate = (uint8_t) kRate1_32; }
    if (style.rhythm == BassRhythm::Triplet) { perBeat = 3; rate = (uint8_t) kRate1_8T; }
    const int perBar = perBeat * 4;
    const int bars = std::min(std::clamp((int) style.bars, 1, 4), kPhraseSteps / perBar);
    PhrasePattern fresh = pat;
    fresh.rate = rate;
    fresh.length = (uint8_t) (bars * perBar);
    if (fresh.octave < 1 || fresh.octave > 2) fresh.octave = 1;
    for (auto& st : fresh.steps) st = PhraseStep{};
    for (int i = 0; i < bars * perBar; ++i)
    {
        const int q = i % perBeat;
        bool on = false;
        int octave = 0;
        switch (style.rhythm)
        {
            case BassRhythm::KBBB: on = q != 0; break;
            case BassRhythm::KxBB: on = q >= 2; break;
            case BassRhythm::KBxB: on = q == 1 || q == 3; break;
            case BassRhythm::Gallop: on = q == 2 || q == 4 || q == 6 || q == 7; break;
            case BassRhythm::OffbeatOctaves: on = q >= 2; octave = q == 3 ? 1 : 0; break;
            case BassRhythm::Triplet: on = q != 0; break;
            case BassRhythm::Count: break;
        }
        if (! on) continue;
        auto& st = fresh.steps[(size_t) i];
        st.state = StepState::Note;
        st.tone = 0;
        st.octave = (int8_t) octave;
        st.velocity = (uint8_t) (112 + PatternRandom::range(0, 4, seed, i, 0, PVelocity) / 4); // one velocity, hardly varied
        st.gate = (uint16_t) (style.rhythm == BassRhythm::Gallop ? 60 : 62);
    }
    // Variation is rare and placed: an octave jump in the last beat of a bar, the fifth on the phrase end, the last
    // sixteenth of every fourth bar muted.
    for (int b = 0; b < bars; ++b)
    {
        if (PatternRandom::chance(style.octaveJumps / 100.0, seed, b, 0, PJump))
        {
            const int at = b * perBar + 3 * perBeat + perBeat - 1 - PatternRandom::range(0, 1, seed, b, 1, PJump);
            if (fresh.steps[(size_t) at].state == StepState::Note) fresh.steps[(size_t) at].octave = 1;
        }
    }
    if (PatternRandom::chance(style.fifths / 100.0, seed, 0, 0, PFifth))
        for (int i = (bars - 1) * perBar + 3 * perBeat; i < bars * perBar; ++i)
            if (fresh.steps[(size_t) i].state == StepState::Note) fresh.steps[(size_t) i].tone = 1;
    if (bars == 4 && fresh.steps[(size_t) (4 * perBar - 1)].state == StepState::Note) fresh.steps[(size_t) (4 * perBar - 1)].state = StepState::Rest;
    if (keep == DiceKeep::Rhythm && previous.length > 0)
    {
        auto kept = previous;
        for (int i = 0; i < kept.length; ++i)
            if (kept.steps[(size_t) i].state == StepState::Note) { kept.steps[(size_t) i].tone = fresh.steps[(size_t) (i % std::max(1, (int) fresh.length))].tone; }
        pat = kept;
        return;
    }
    phraseRank(fresh, seed);
    pat = fresh;
}

void generatePad(PhrasePartState& part, int slot, uint32_t seed)
{
    auto& pat = part.slots[(size_t) slot];
    pat.rate = (uint8_t) kRate1_1;
    pat.length = 4;
    pat.steps = {};
    const VoicingStyle styles[3] = {VoicingStyle::Wide, VoicingStyle::Open, VoicingStyle::Close};
    pat.padVoicing = styles[PatternRandom::range(0, 2, seed, 1, 0, PPad)];
    pat.padVoices = (uint8_t) PatternRandom::range(4, 6, seed, 2, 0, PPad);
    pat.colour = (uint8_t) PatternRandom::range(15, 60, seed, 3, 0, PPad);
    pat.motion = PatternRandom::chance(0.6, seed, 4, 0, PPad) ? PadMotion::Hold : PadMotion::Breathe;
    pat.motionBars = (uint8_t) (PatternRandom::chance(0.5, seed, 5, 0, PPad) ? 1 : 2);
    pat.padBass = PatternRandom::chance(0.7, seed, 6, 0, PPad) ? PadBass::Root : PadBass::RootFifth;
    pat.shimmer = (uint8_t) (PatternRandom::chance(0.4, seed, 7, 0, PPad) ? PatternRandom::range(10, 35, seed, 8, 0, PPad) : 0);
    pat.overlap = (uint8_t) PatternRandom::range(10, 50, seed, 9, 0, PPad);
    pat.padStrumMs = (uint8_t) (PatternRandom::chance(0.3, seed, 10, 0, PPad) ? PatternRandom::range(20, 120, seed, 11, 0, PPad) : 0);
    pat.padCentre = (uint8_t) PatternRandom::range(58, 66, seed, 12, 0, PPad);
}

int lastStrongStep(const PhrasePattern& pat, int from)
{
    const double sb = pat.stepBeats();
    for (int i = std::clamp((int) pat.length, 1, kPhraseSteps) - 1; i >= from; --i)
        if (grid::positionIn(i * sb, 1.0) < kEps) return i;
    return std::clamp((int) pat.length, 1, kPhraseSteps) - 1;
}

void rotate(PhrasePattern& pat, int by)
{
    const int n = std::clamp((int) pat.length, 1, kPhraseSteps);
    std::array<PhraseStep, kPhraseSteps> copy = pat.steps;
    for (int i = 0; i < n; ++i) pat.steps[(size_t) floorMod(i + by, n)] = copy[(size_t) i];
}
} // namespace

void phraseGenerate(PhraseState& state, PhrasePart which, int slot, uint32_t seed, DiceKeep keep)
{
    slot = std::clamp(slot, 0, kPhraseSlots - 1);
    auto& part = state.part(which);
    part.diceSeed = seed;
    switch (which)
    {
        case PhrasePart::Arp:
            generateArp(part, slot, seed, keep);
            if (part.arp.bars >= 8 && slot + 1 < kPhraseSlots)
            {
                phraseAnswer(state, which, slot, slot + 1, AnswerKind::Answer);
                if (part.formLength <= 1)
                {
                    part.formLength = 2;
                    part.form[0] = (uint8_t) slot;
                    part.form[1] = (uint8_t) (slot + 1);
                }
            }
            break;
        case PhrasePart::Stabs: generateStabs(part, slot, seed, keep); break;
        case PhrasePart::Pad: generatePad(part, slot, seed); break;
        case PhrasePart::Bass: generateBass(part, slot, seed, keep); break;
        case PhrasePart::Count: break;
    }
}

void phraseAnswer(PhraseState& state, PhrasePart which, int from, int to, AnswerKind kind)
{
    auto& part = state.part(which);
    from = std::clamp(from, 0, kPhraseSlots - 1);
    to = std::clamp(to, 0, kPhraseSlots - 1);
    auto pat = part.slots[(size_t) from];
    const int n = std::clamp((int) pat.length, 1, kPhraseSteps);
    const int half = n / 2;
    const bool topLane = which == PhrasePart::Stabs;
    auto get = [&](const PhraseStep& st) { return (int) (topLane ? st.top : st.tone); };
    auto set = [&](PhraseStep& st, int v) { if (topLane) st.top = (int8_t) std::clamp(v, 0, 7); else st.tone = (int8_t) std::clamp(v, 0, 7); };
    switch (kind)
    {
        case AnswerKind::Answer:
        {
            if (which == PhrasePart::Pad) break;
            const int strong = lastStrongStep(pat, half);
            int first = -1, startTone = 0, count = 0;
            for (int i = half; i <= strong; ++i)
                if (pat.steps[(size_t) i].state == StepState::Note)
                {
                    if (first < 0) { first = i; startTone = get(pat.steps[(size_t) i]); }
                    ++count;
                }
            int j = 0;
            const int peak = std::min(std::max(startTone, 1), 4);
            for (int i = half; i < strong; ++i)
            {
                auto& st = pat.steps[(size_t) i];
                if (st.state != StepState::Note) continue;
                // Down by step towards the root: tones from the start of the second half to 1, the root comes on the strong step.
                const int remaining = count - 1 - j;
                set(st, which == PhrasePart::Bass ? 0 : std::clamp(std::min(peak, remaining), 1, 7));
                st.scaleStep = 0;
                st.velocity = (uint8_t) std::max(1, (int) std::lround(st.velocity * 0.92));
                ++j;
            }
            auto& close = pat.steps[(size_t) strong];
            close.state = StepState::Note;
            set(close, which == PhrasePart::Stabs ? 3 : 0); // stabs close with the root on top
            close.tone = 0;
            close.scaleStep = 0;
            close.octave = 0;
            close.gate = (uint16_t) std::min(400, std::max(40, (int) close.gate) * 3 / 2);
            close.velocity = (uint8_t) std::max(1, std::min((int) close.velocity, 100) * 92 / 100);
            close.slide = false;
            for (int i = strong + 1; i < n; ++i)
                if (which != PhrasePart::Bass) pat.steps[(size_t) i].state = StepState::Rest;
            break;
        }
        case AnswerKind::Mirror:
        {
            int pivot = -1;
            for (int i = 0; i < n; ++i)
            {
                auto& st = pat.steps[(size_t) i];
                if (st.state != StepState::Note) continue;
                if (pivot < 0) pivot = get(st);
                set(st, 2 * pivot - get(st));
                st.scaleStep = (int8_t) -st.scaleStep;
            }
            break;
        }
        case AnswerKind::Shift: rotate(pat, std::max(1, (int) std::lround(0.5 / pat.stepBeats()))); break;
        case AnswerKind::Sparse:
        {
            int k = 0;
            for (int i = half; i < n; ++i)
                if (pat.steps[(size_t) i].state == StepState::Note && (k++ % 2) == 1) pat.steps[(size_t) i].state = StepState::Rest;
            break;
        }
        case AnswerKind::Counter:
        {
            int notes = 0;
            for (int i = 0; i < n; ++i) notes += pat.steps[(size_t) i].state == StepState::Note;
            std::array<bool, kPhraseSteps> keepMask{};
            phraseDensityMask(pat, 0.5f, keepMask);
            for (int i = 0; i < n; ++i)
            {
                auto& st = pat.steps[(size_t) i];
                if (st.state != StepState::Note) continue;
                if (! keepMask[(size_t) i]) { st.state = StepState::Rest; continue; }
                st.scaleStep = (int8_t) std::clamp(st.scaleStep - 4, -7, 7);
                if (topLane) st.top = (int8_t) std::max(0, st.top - 1);
                st.velocity = (uint8_t) std::max(1, (int) st.velocity * 85 / 100);
            }
            rotate(pat, 1);
            (void) notes;
            break;
        }
        case AnswerKind::Count: break;
    }
    phraseRank(pat, part.diceSeed + (uint32_t) to);
    part.slots[(size_t) to] = pat;
}

void phraseMutate(PhraseState& state, PhrasePart which, int slot, uint32_t seed)
{
    auto& pat = state.part(which).slots[(size_t) std::clamp(slot, 0, kPhraseSlots - 1)];
    const int n = std::clamp((int) pat.length, 1, kPhraseSteps);
    for (int i = 0; i < n; ++i)
    {
        if (PatternRandom::unit(seed, i, 0, PMutate) >= 0.125) continue;
        auto& st = pat.steps[(size_t) i];
        const double kind = PatternRandom::unit(seed, i, 1, PMutateKind);
        if (st.state == StepState::Note)
        {
            if (which == PhrasePart::Stabs) st.top = (int8_t) std::clamp(st.top + (kind < 0.5 ? -1 : 1), 0, 6);
            else if (which == PhrasePart::Bass) st.octave = (int8_t) (st.octave == 0 ? 1 : 0);
            else if (kind < 0.4) st.tone = (int8_t) std::clamp(st.tone + (kind < 0.2 ? -1 : 1), 0, 9);
            else if (kind < 0.7) st.scaleStep = (int8_t) std::clamp(st.scaleStep + (kind < 0.55 ? -1 : 1), -7, 7);
            else st.state = StepState::Rest;
        }
        else if (st.state == StepState::Rest && which != PhrasePart::Pad)
        {
            st.state = StepState::Note;
            st.velocity = (uint8_t) (which == PhrasePart::Bass ? 114 : 80);
            st.gate = (uint16_t) (which == PhrasePart::Bass ? 62 : 35);
            st.top = 2;
            if (which == PhrasePart::Bass && grid::positionIn(i * pat.stepBeats(), 1.0) < 0.25 - kEps) st.state = StepState::Rest; // kick's sixteenth
        }
    }
    phraseRank(pat, seed);
}

PhraseState defaultPhraseState()
{
    PhraseState s;
    s.progression = factoryProgression(0);
    for (int p = 0; p < (int) PhrasePart::Count; ++p)
    {
        auto& part = s.parts[(size_t) p];
        part.formLength = 1;
        for (auto& pat : part.slots) pat.octave = p == (int) PhrasePart::Bass ? 1 : 4;
        part.stabs.bars = 2;
        part.bass.bars = 4;
        phraseGenerate(s, (PhrasePart) p, 0, 0x5eed + (uint32_t) p);
        if (p != (int) PhrasePart::Pad)
            for (int slot = 1; slot < kPhraseSlots; ++slot) s.parts[(size_t) p].slots[(size_t) slot] = s.parts[(size_t) p].slots[0];
    }
    return s;
}

// ---------------------------------------------------------------------------------------------------- progressions
namespace
{
struct Move
{
    const char* name;
    std::array<Chord, 4> chords;
    int count;
};
const std::array<Move, 7>& moves()
{
    static const std::array<Move, 7> list{{
        {"i - bVI - bVII - i", {Chord::degree(0), Chord::degree(5), Chord::degree(6), Chord::degree(0)}, 4},
        {"i - bVII - bVI - bVII", {Chord::degree(0), Chord::degree(6), Chord::degree(5), Chord::degree(6)}, 4},
        {"i - iv - bVI - v", {Chord::degree(0), Chord::degree(3), Chord::degree(5), Chord::degree(4)}, 4},
        {"i - bVI - bIII - bVII", {Chord::degree(0), Chord::degree(5), Chord::degree(2), Chord::degree(6)}, 4},
        {"i - bII", {Chord::degree(0), Chord::borrowed(1, ChordQuality::Major), {}, {}}, 2},
        {"i - bVI - iv - V", {Chord::degree(0), Chord::degree(5), Chord::degree(3), Chord::degree(4, ChordQuality::Major)}, 4},
        {"i (drone)", {Chord::degree(0), {}, {}, {}}, 1},
    }};
    return list;
}
constexpr int kBarsPerChord[3] = {2, 4, 8};
} // namespace

int factoryProgressionCount() { return (int) moves().size() * 3; }

std::string factoryProgressionName(int index)
{
    index = std::clamp(index, 0, factoryProgressionCount() - 1);
    const auto& move = moves()[(size_t) (index / 3)];
    const int bars = kBarsPerChord[index % 3] * (move.count == 1 ? 2 : 1);
    return std::string(move.name) + ", " + std::to_string(bars) + " bars" + (move.count == 1 ? "" : " each");
}

Progression factoryProgression(int index)
{
    index = std::clamp(index, 0, factoryProgressionCount() - 1);
    const auto& move = moves()[(size_t) (index / 3)];
    const int bars = kBarsPerChord[index % 3] * (move.count == 1 ? 2 : 1);
    Progression p;
    p.count = move.count;
    for (int i = 0; i < move.count; ++i) p.slots[(size_t) i] = {move.chords[(size_t) i], bars * 4.0};
    return p;
}

// ---------------------------------------------------------------------------------------------------------- names
const char* phrasePartName(PhrasePart p)
{
    switch (p)
    {
        case PhrasePart::Arp: return "Arp";
        case PhrasePart::Stabs: return "Stabs";
        case PhrasePart::Pad: return "Pad";
        case PhrasePart::Bass: return "Bass";
        case PhrasePart::Count: break;
    }
    return "";
}
const char* arpShapeName(ArpShape s)
{
    switch (s)
    {
        case ArpShape::Up: return "Up";
        case ArpShape::Down: return "Down";
        case ArpShape::UpDown: return "Up-Down";
        case ArpShape::PedalLow: return "Pedal Low";
        case ArpShape::PedalHigh: return "Pedal High";
        case ArpShape::Thirds: return "Thirds";
        case ArpShape::RootFifthOctave: return "Root-5-8";
        case ArpShape::Spiral: return "Spiral";
        case ArpShape::Walk: return "Walk";
        case ArpShape::Count: break;
    }
    return "";
}
const char* arpRhythmName(ArpRhythm r)
{
    switch (r)
    {
        case ArpRhythm::Sixteenths: return "16ths";
        case ArpRhythm::Offbeats: return "Offbeats";
        case ArpRhythm::ThreeThreeTwo: return "3-3-2";
        case ArpRhythm::Gallop: return "Gallop";
        case ArpRhythm::Euclid: return "Euclid";
        case ArpRhythm::Triplets: return "Triplets";
        case ArpRhythm::Count: break;
    }
    return "";
}
const char* arpAccentsName(ArpAccents a)
{
    switch (a)
    {
        case ArpAccents::None: return "None";
        case ArpAccents::ThreeThreeTwo: return "3-3-2";
        case ArpAccents::OffBeat: return "Off Beat";
        case ArpAccents::PhraseEnd: return "Phrase End";
        case ArpAccents::Count: break;
    }
    return "";
}
const char* stabRhythmName(StabRhythm r)
{
    switch (r)
    {
        case StabRhythm::Offbeats: return "Offbeats";
        case StabRhythm::ThreeThreeTwo: return "3-3-2";
        case StabRhythm::Dotted: return "Dotted";
        case StabRhythm::Sparse: return "Sparse";
        case StabRhythm::Push: return "Push";
        case StabRhythm::Count: break;
    }
    return "";
}
const char* stabContourName(StabContour c)
{
    switch (c)
    {
        case StabContour::Rise: return "Rise";
        case StabContour::Fall: return "Fall";
        case StabContour::Arch: return "Arch";
        case StabContour::Flat: return "Flat";
        case StabContour::Count: break;
    }
    return "";
}
const char* bassRhythmName(BassRhythm r)
{
    switch (r)
    {
        case BassRhythm::KBBB: return "KBBB";
        case BassRhythm::KxBB: return "K-BB";
        case BassRhythm::KBxB: return "KB-B";
        case BassRhythm::Gallop: return "Gallop";
        case BassRhythm::OffbeatOctaves: return "Off Octaves";
        case BassRhythm::Triplet: return "Triplet";
        case BassRhythm::Count: break;
    }
    return "";
}
const char* padMotionName(PadMotion m)
{
    switch (m)
    {
        case PadMotion::Hold: return "Hold";
        case PadMotion::Breathe: return "Breathe";
        case PadMotion::Pulse: return "Pulse";
        case PadMotion::Count: break;
    }
    return "";
}
const char* padBassName(PadBass b)
{
    switch (b)
    {
        case PadBass::Off: return "Off";
        case PadBass::Root: return "Root";
        case PadBass::RootFifth: return "Root + 5th";
        case PadBass::Count: break;
    }
    return "";
}
const char* answerKindName(AnswerKind k)
{
    switch (k)
    {
        case AnswerKind::Answer: return "Answer";
        case AnswerKind::Mirror: return "Mirror";
        case AnswerKind::Shift: return "Shift";
        case AnswerKind::Sparse: return "Sparse";
        case AnswerKind::Counter: return "Counter";
        case AnswerKind::Count: break;
    }
    return "";
}
} // namespace sonora::model
