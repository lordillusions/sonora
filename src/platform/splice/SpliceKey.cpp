#include "platform/splice/SpliceKey.h"

#include "core/Scales.h"

#include <array>
#include <cctype>

namespace sonora::platform::splice
{

namespace
{
int wrap12(int v) { return ((v % 12) + 12) % 12; }

// Fold a pitch-class distance into the smallest signed shift in [-6, +6] (ties resolve downward, like Live's pitch nudge).
int smallestShift(int delta)
{
    delta = wrap12(delta);
    return delta > 6 ? delta - 12 : delta;
}

// The tonic of the relative major: a minor key shares its notes with the major a minor third above it.
int relativeMajorTonic(const MusicalKey& key)
{
    return key.mode == KeyMode::Minor ? wrap12(key.pitchClass + 3) : key.pitchClass;
}

bool scaleIsMinor(int scaleName)
{
    const auto& scale = model::scaleAt(scaleName);
    bool hasMinorThird = false, hasMajorThird = false;
    for (int i = 0; i < scale.size; ++i)
    {
        if (scale.intervals[(size_t) i] == 3) hasMinorThird = true;
        if (scale.intervals[(size_t) i] == 4) hasMajorThird = true;
    }
    return hasMinorThird && ! hasMajorThird;
}
} // namespace

MusicalKey parseSpliceKey(const std::string& note, const std::string& chordType)
{
    MusicalKey key;
    if (note.empty()) return key;
    static constexpr std::array<int, 7> letters{9, 11, 0, 2, 4, 5, 7}; // a b c d e f g
    const char letter = (char) std::tolower((unsigned char) note[0]);
    if (letter < 'a' || letter > 'g') return key;
    int pc = letters[(size_t) (letter - 'a')];
    for (size_t i = 1; i < note.size(); ++i)
    {
        const char c = (char) std::tolower((unsigned char) note[i]);
        if (c == '#' || c == 's') ++pc;
        else if (c == 'b' || c == 'f') --pc;
    }
    key.pitchClass = wrap12(pc);
    key.valid = true;
    std::string chord;
    for (char c : chordType) chord += (char) std::tolower((unsigned char) c);
    if (chord.find("min") != std::string::npos) key.mode = KeyMode::Minor;
    else if (chord.find("maj") != std::string::npos) key.mode = KeyMode::Major;
    return key;
}

MusicalKey projectKey(int scaleRoot, int scaleName)
{
    MusicalKey key;
    key.pitchClass = wrap12(scaleRoot);
    key.mode = scaleIsMinor(scaleName) ? KeyMode::Minor : KeyMode::Major;
    key.valid = true;
    return key;
}

int transposeToProject(const MusicalKey& sample, const MusicalKey& project)
{
    if (! sample.valid || ! project.valid) return 0;
    // With a mode on both sides, match the relative-major tonics so a relative minor/major needs no shift.
    if (sample.mode != KeyMode::None && project.mode != KeyMode::None)
        return smallestShift(relativeMajorTonic(project) - relativeMajorTonic(sample));
    return smallestShift(project.pitchClass - sample.pitchClass);
}

} // namespace sonora::platform::splice
