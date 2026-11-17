#pragma once
// PLG-25a: harmony for the pattern generators (Sonora Phrase, Pulse, the bass line command). Builds on core/Scales.h.
//
// Conventions
//   - Pitches are MIDI numbers in scientific octaves: C4 = 60, C3 = 48, F1 = 29 (43.7 Hz). Pitch classes are 0..11 (C = 0).
//   - A key is a pitch class, a scale an index into model::kScales (1 = Minor). Scale degrees are 0-based (0 = the key's
//     root, the "i"); a degree below 0 or past the scale's size continues into the octave below / above.
//
// Rules (each one has a unit test in tests/test_harmony.cpp)
//   - chordTones(): Auto quality stacks thirds inside the scale (degree, +2, +4 scale steps; on scales that are not
//     seven-note this means every second step). Explicit qualities are fixed shapes (Major 0-4-7, Minor 0-3-7,
//     Diminished 0-3-6, Augmented 0-4-8, Sus2 0-2-7, Sus4 0-5-7, Fifth 0-7). Extensions take the scale's own 6th, 7th, 9th
//     and 11th above a root that is in a seven-note scale (so Major + Seventh on v of Minor is a dominant 7th and on III a
//     major 7th); otherwise the defaults 9, 10 (9 on Diminished), 14 and 17. Tones are listed in chord-function order:
//     root, third (or the sus tone), fifth, then the 6th / 7th, 9th, 11th.
//   - voice(): see VoicingRequest. Every voicing keeps the "low interval" rule: no tone other than the root and the
//     perfect fifth below C3 (48), and adjacent voices below C3 at least a fourth (5 semitones) apart. With a previous
//     voicing it takes the least total motion (common tones are held), with a small pull towards `centre` so a long
//     progression does not drift.
//   - chordFromHeldNotes(): the shape that explains the most held pitch classes with the fewest extra or missing ones;
//     ties go to the lowest note as root. A held third alone is a triad with its fifth implied; two notes a fourth apart
//     are a fifth chord over its fifth.
//
// No JUCE, no allocation (except chordName), usable on the audio thread.

#include <array>
#include <cstdint>
#include <string>

namespace sonora::model
{
inline constexpr int kThirdFloor = 48; // C3: no thirds or colour tones below here (voice())

constexpr int pitchOf(int pitchClass, int octave) { return 12 * (octave + 1) + pitchClass; }
constexpr int pitchClassOf(int pitch) { return ((pitch % 12) + 12) % 12; }

// Pitch of scale degree `degree` (0-based, wraps octaves) of scale index `scale` in `key`, where `octave` is the octave of
// the key's root: scaleDegreePitch(0, Minor, 2, 4) = 63 (E flat 4).
int scaleDegreePitch(int key, int scale, int degree, int octave);
// The 0-based degree of a pitch class in the scale, or -1 when it is not in it.
int scaleDegreeOf(int pitchClass, int key, int scale);

enum class ChordQuality : uint8_t { Auto, Major, Minor, Diminished, Augmented, Sus2, Sus4, Fifth, Count };
enum class ChordExtension : uint8_t { None, Sixth, Seventh, Ninth, Add9, Eleventh, Count };
const char* chordQualityName(ChordQuality);
const char* chordExtensionName(ChordExtension);

struct Chord
{
    int8_t root = 0;            // scale degree (0-based) when ! chromatic; semitones above the key when chromatic
    bool chromatic = false;     // a borrowed root that is not a degree of the scale (bII in Minor, a chromatic mediant...)
    ChordQuality quality = ChordQuality::Auto;
    ChordExtension extension = ChordExtension::None;
    int8_t inversion = -1;      // -1 free (least motion); 0 root position, 1 first, 2 second, 3 third inversion
    int8_t bass = -1;           // slash chord: scale degree of a bass note added under the voicing; -1 none

    static constexpr Chord degree(int d, ChordQuality q = ChordQuality::Auto, ChordExtension e = ChordExtension::None)
    {
        Chord c;
        c.root = (int8_t) d;
        c.quality = q;
        c.extension = e;
        return c;
    }
    static constexpr Chord borrowed(int semitones, ChordQuality q, ChordExtension e = ChordExtension::None)
    {
        Chord c;
        c.root = (int8_t) (((semitones % 12) + 12) % 12);
        c.chromatic = true;
        c.quality = q;
        c.extension = e;
        return c;
    }
    bool operator==(const Chord&) const = default;
};

struct ChordTones
{
    int root = 0;   // pitch class of the chord root
    int bass = -1;  // pitch class of a slash bass, -1 = none
    int count = 0;
    std::array<int8_t, 6> intervals{}; // semitones above the root, chord-function order (root first, always 0)
    bool third = true; // intervals[1] is a third (false: sus or fifth chords)

    // The tone ladder the generators climb: tone i < count is intervals[i]; past that the tones repeat one ladder cycle
    // higher (12 semitones when every tone is inside the octave, 24 with a 9th or 11th). Negative i goes down.
    int interval(int toneIndex) const;
    int pitchClass(int toneIndex) const { return (root + interval(toneIndex)) % 12; }
    bool contains(int pitchClass) const;
    // The tone index (0..count-1) whose pitch class this is, or -1.
    int toneOf(int pitchClass) const;
    bool operator==(const ChordTones&) const = default;
};

ChordTones chordTones(const Chord& chord, int key, int scale);

// How a pitch sits on a chord in a key (the faces' fit colours).
enum class NoteFit : uint8_t { Root, ChordTone, InScale, Outside };
NoteFit noteFit(int pitch, const ChordTones& chord, int key, int scale);

enum class VoicingStyle : uint8_t
{
    Close,  // every voice within an octave, stacked upwards
    Open,   // close position with the second voice from the bottom (and the fourth) raised an octave
    Drop2,  // close position with the second voice from the top dropped an octave
    Wide,   // root and fifth low, the third and colour tones close together an octave or more above (pads)
    Fifths, // root, fifth, octave, twelfth... (hard stabs); a requested top tone is put on top
    Count
};
const char* voicingStyleName(VoicingStyle);

struct Voicing
{
    static constexpr int kMax = 10;
    std::array<int8_t, kMax> pitches{}; // ascending
    int count = 0;
    int lowest() const { return count > 0 ? pitches[0] : -1; }
    int highest() const { return count > 0 ? pitches[(size_t) count - 1] : -1; }
    bool operator==(const Voicing&) const = default;
};

struct VoicingRequest
{
    VoicingStyle style = VoicingStyle::Close;
    int voices = 4;         // 1..8 (a slash bass is added on top of this count)
    int centre = 60;        // the voicing's mean pitch is pulled here
    int lowLimit = 36;      // no voice below / above these
    int highLimit = 96;
    int topTone = -1;       // a chord tone index (ladder index, any octave) that must be the highest voice; -1 free
    int inversion = -1;     // 0 root position, 1 first... (the chord tone index of the lowest voice); -1 free
};

// The voicing of `chord` for `request`; with `previous` the least motion from it. Constraints that cannot be met together
// are dropped in this order: the top tone, the inversion, the low interval rule (the range always holds).
Voicing voice(const ChordTones& chord, const VoicingRequest& request, const Voicing* previous = nullptr);

struct ProgressionSlot
{
    Chord chord;
    double beats = 8.0; // length (Phrase offers 2, 4, 8, 16, 32 beats = 1/2 .. 8 bars)
    bool operator==(const ProgressionSlot& o) const { return chord == o.chord && ! (beats < o.beats) && ! (o.beats < beats); }
};

struct Progression
{
    static constexpr int kMaxSlots = 16;
    std::array<ProgressionSlot, kMaxSlots> slots{};
    int count = 1;

    double lengthBeats() const;
    struct Position
    {
        int index = 0;      // the slot playing
        double start = 0;   // song beats where this occurrence of the slot starts / ends
        double end = 0;
    };
    // The slot playing at song beat `beat`; the progression loops from beat 0. Boundaries belong to the slot that starts.
    Position at(double beat) const;
    const Chord& chordAt(double beat) const { return slots[(size_t) at(beat).index].chord; }
    bool operator==(const Progression&) const = default;
};

// The chord a set of held pitches spells (any order, duplicates and octaves allowed). count == 0 for no notes; one note
// gives a fifth chord on it.
ChordTones chordFromHeldNotes(const int* pitches, int count);

// "Cm7", "F#sus4", "Ebmaj7/G" (sharps or flats by key: flats in keys whose signature has flats). Allocates: message thread.
std::string chordName(const ChordTones&, bool flats = false);
const char* pitchClassName(int pitchClass, bool flats = false);
} // namespace sonora::model
