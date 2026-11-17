#pragma once
// PLG-25: Sonora Phrase's note logic (docs/PHRASE.md), model layer: no JUCE, no allocation in phraseRun. The device
// (devices/native/Phrase) calls phraseRun on the audio thread with its MIDI sink; tests and the face call it headless.
//
// What plays is a function of the state, the live (performance) values, the chords and the song position only
// (TRANCE_SUITE.md "Decisions" 3): no history, so a locate, a loop or a bounce plays the same notes.
//
// Time: a part plays its form (segments of pattern slots, each once through) or, with Pattern = A..H, one slot looping;
// both start at song beat 0. Steps sit on the PatternGrid rate of their pattern, swung by Swing (odd steps of the
// segment late by up to a third of a step), humanized by up to 8 ms. A step's chord is the one playing at its onset;
// a note never sounds past the chord's end longer than its own gate (a tie stops at a chord change).
//
// Pitches: a pattern places the chord root in the octave above the key root of its `octave` (scientific: octave 4 and
// key E -> E4..D#5). Arp: tone (chord tone ladder index, Harmony's ChordTones::interval) + scale steps + octave.
// Bass: tone 0 root, 1 fifth, 2 octave. Stabs: a voicing (Harmony voice()) with the step's `top` tone highest, least
// motion along the progression. Pad: voicings per chord with common tones held (Hold), one inner voice moving (Breathe)
// or re-struck (Pulse), colour tones, bass, shimmer.

#include "core/Harmony.h"
#include "core/PatternGrid.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>

namespace sonora::model
{
enum class PhrasePart : uint8_t { Arp, Stabs, Pad, Bass, Count };
enum class StepState : uint8_t { Rest, Note, Tie };

inline constexpr int kPhraseSteps = 64, kPhraseSlots = 8, kPhraseFormMax = 16;

struct PhraseStep
{
    StepState state = StepState::Rest;
    int8_t tone = 0;        // chord tone ladder index (0 = root; the face shows 1-8). Bass: 0 root, 1 fifth, 2 octave
    int8_t scaleStep = 0;   // -7..+7 scale steps added to the tone
    int8_t octave = 0;      // -2..+2
    uint8_t velocity = 90;  // 1..127
    uint16_t gate = 50;     // % of the step, 5..400
    uint8_t chance = 100;   // %
    bool accent = false, slide = false;
    uint8_t rank = 0;       // Density order: lower ranks come in first
    int8_t top = 2;         // Stabs: the chord tone (ladder index) that is the highest note
    bool operator==(const PhraseStep&) const = default;
};

enum class PadMotion : uint8_t { Hold, Breathe, Pulse, Count };
enum class PadBass : uint8_t { Off, Root, RootFifth, Count };

struct PhrasePattern
{
    uint8_t rate = kRate1_16;
    uint8_t length = 16;    // steps, 1..64
    int8_t octave = 4;      // register (see the header comment)
    std::array<PhraseStep, kPhraseSteps> steps{};
    // Stabs
    uint8_t voices = 4;     // 3..6
    VoicingStyle voicing = VoicingStyle::Close;
    uint8_t strumMs = 0;    // 0..60
    bool strumDown = false;
    // Pad
    uint8_t padVoices = 5;  // 3..7
    VoicingStyle padVoicing = VoicingStyle::Wide;
    uint8_t padCentre = 62; // the voicing's centre pitch
    uint8_t colour = 30;    // % chance per chord of one colour tone
    PadBass padBass = PadBass::Root;
    PadMotion motion = PadMotion::Hold;
    uint8_t motionBars = 2; // Breathe / Pulse every n bars
    uint8_t overlap = 25;   // % of a beat of legato into the next chord
    uint8_t padStrumMs = 0; // 0..250
    uint8_t shimmer = 0;    // %
    uint8_t padVelocity = 80;

    double stepBeats() const { return grid::rateBeats(rate); }
    double lengthBeats() const { return std::max(1, (int) length) * stepBeats(); }
    bool operator==(const PhrasePattern&) const = default;
};

// ---- generator styles (the DICE panel)
enum class ArpShape : uint8_t { Up, Down, UpDown, PedalLow, PedalHigh, Thirds, RootFifthOctave, Spiral, Walk, Count };
enum class ArpRhythm : uint8_t { Sixteenths, Offbeats, ThreeThreeTwo, Gallop, Euclid, Triplets, Count };
enum class ArpAccents : uint8_t { None, ThreeThreeTwo, OffBeat, PhraseEnd, Count };
enum class StabRhythm : uint8_t { Offbeats, ThreeThreeTwo, Dotted, Sparse, Push, Count };
enum class StabContour : uint8_t { Rise, Fall, Arch, Flat, Count };
enum class BassRhythm : uint8_t { KBBB, KxBB, KBxB, Gallop, OffbeatOctaves, Triplet, Count };

struct ArpStyle
{
    ArpShape shape = ArpShape::PedalLow;
    ArpRhythm rhythm = ArpRhythm::Sixteenths;
    ArpAccents accents = ArpAccents::OffBeat;
    bool accentGlide = false;
    uint8_t tension = 30;   // %
    uint8_t range = 2;      // octaves 1..3
    uint8_t passing = 20;   // %
    uint8_t variation = 40; // %
    uint8_t bars = 2;       // 1, 2, 4, 8 (8: the slot gets 4 bars and the next slot their answer)
    uint8_t euclidHits = 7; // Euclid: hits per 16
    bool operator==(const ArpStyle&) const = default;
};
struct StabStyle
{
    StabRhythm rhythm = StabRhythm::Offbeats;
    uint8_t hits = 3;        // per bar, 2..8
    StabContour contour = StabContour::Rise;
    uint8_t bars = 2;        // 1, 2, 4
    bool operator==(const StabStyle&) const = default;
};
struct BassStyle
{
    BassRhythm rhythm = BassRhythm::KBBB;
    uint8_t octaveJumps = 20; // %
    uint8_t fifths = 30;      // %
    uint8_t bars = 4;         // 1, 2, 4
    bool operator==(const BassStyle&) const = default;
};

struct PhrasePartState
{
    std::array<PhrasePattern, kPhraseSlots> slots{};
    uint8_t formLength = 1;
    std::array<uint8_t, kPhraseFormMax> form{}; // slot per segment
    ArpStyle arp;
    StabStyle stabs;
    BassStyle bass;
    uint32_t diceSeed = 1;
    double formBeats() const;
    bool operator==(const PhrasePartState&) const = default;
};

struct PhraseState
{
    Progression progression;
    std::array<PhrasePartState, (size_t) PhrasePart::Count> parts{};
    bool keepChords = false; // presets leave the progression alone
    PhrasePartState& part(PhrasePart p) { return parts[(size_t) p]; }
    const PhrasePartState& part(PhrasePart p) const { return parts[(size_t) p]; }
    bool operator==(const PhraseState&) const = default;
};

// The performance parameters, read once per block.
struct PhraseLive
{
    PhrasePart part = PhrasePart::Arp;
    int pattern = 0;          // 0 = the form, 1..8 = hold slot A..H
    int key = 4, scale = 1;   // pitch class, kScales index
    double tempo = 138;
    float density = 1, swing = 0, gate = 1, velocity = 1, humanize = 0, kickGuard = 0;
    int accent = 20, octave = 0, shift = 0;
    uint32_t seed = 1;
};

// Where the chords come from.
struct PhraseChords
{
    const Progression* progression = nullptr; // Progression / Link: chords over song time (null: rest)
    bool fromHeld = false;                    // MIDI In: `held` is the chord now (count 0: rest)
    ChordTones held;
    double heldSince = 0;                     // MIDI In: when the held chord began (pads strike there)
};

struct PhraseEvent
{
    double beat = 0, length = 0;
    int pitch = 60, velocity = 100;
};

// Renders the events whose beat lies in [fromBeat, toBeat) into `out` (at most `capacity`; returns the count). Keep spans
// short (phraseRun cuts them to one beat).
int phraseRender(const PhraseState&, const PhraseLive&, const PhraseChords&, double fromBeat, double toBeat, PhraseEvent* out,
                 int capacity);

template <class Sink>
void phraseRun(const PhraseState& state, const PhraseLive& live, const PhraseChords& chords, double fromBeat, double toBeat, Sink& sink)
{
    std::array<PhraseEvent, 512> events;
    for (double from = fromBeat; from < toBeat;)
    {
        const double to = std::min(toBeat, std::floor(from + 1.0 + 1e-9));
        const int n = phraseRender(state, live, chords, from, to > from ? to : toBeat, events.data(), (int) events.size());
        for (int i = 0; i < n; ++i) sink.note(events[(size_t) i].beat, events[(size_t) i].length, events[(size_t) i].pitch, events[(size_t) i].velocity);
        if (! (to > from)) break;
        from = to;
    }
}

// ---- the form
struct PhraseSegment
{
    int slot = 0;           // pattern slot playing
    int64_t occurrence = 0; // unique per played segment (chance draws per occurrence)
    double start = 0, length = 0;
};
// The segment playing at `beat` for Pattern = `pattern` (0 form, 1..8 slot).
PhraseSegment phraseSegmentAt(const PhrasePartState&, int pattern, double beat);

// ---- editing (message thread)
enum class DiceKeep : uint8_t { None, Rhythm, Pitches };
// Writes concrete steps into `slot` from the part's style and `seed` (Dice). Rhythm keeps the rhythm and re-rolls pitches
// (Shift-click), Pitches keeps the pitch line and re-rolls the rhythm (Alt-click). Arp with 8 bars: the slot gets four
// bars and slot + 1 their answer, and a single-segment form becomes "A B".
void phraseGenerate(PhraseState&, PhrasePart, int slot, uint32_t seed, DiceKeep keep = DiceKeep::None);
enum class AnswerKind : uint8_t { Answer, Mirror, Shift, Sparse, Counter, Count };
const char* answerKindName(AnswerKind);
// Fills slot `to` from slot `from` (TRANCE_SUITE "Style facts": the answer closes on the root on a strong step).
void phraseAnswer(PhraseState&, PhrasePart, int from, int to, AnswerKind kind = AnswerKind::Answer);
// Changes about one step in eight of `slot`.
void phraseMutate(PhraseState&, PhrasePart, int slot, uint32_t seed);
// Ranks the note steps of a pattern: bar downbeats and the phrase's last note first, then beats and accents, eighths,
// the rest (ties broken by the seed).
void phraseRank(PhrasePattern&, uint32_t seed);
// The note steps that play at `density` (0..1), in rank order: out[i] true when step i plays. Monotonic in density.
void phraseDensityMask(const PhrasePattern&, float density, std::array<bool, kPhraseSteps>& out);

// A fresh instance: the Minor progression i-bVI-bVII-i at 2 bars per chord, every part generated from its default style.
PhraseState defaultPhraseState();

// Factory progressions (the strip's menu): name and slots. `index` < factoryProgressionCount().
int factoryProgressionCount();
std::string factoryProgressionName(int index);
Progression factoryProgression(int index);

const char* phrasePartName(PhrasePart);
const char* arpShapeName(ArpShape);
const char* arpRhythmName(ArpRhythm);
const char* arpAccentsName(ArpAccents);
const char* stabRhythmName(StabRhythm);
const char* stabContourName(StabContour);
const char* bassRhythmName(BassRhythm);
const char* padMotionName(PadMotion);
const char* padBassName(PadBass);
} // namespace sonora::model
