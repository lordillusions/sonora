#pragma once
// Named musical scales in the order of the reference app's scale choosers (Live 12 set files store the index:
// 0 = Major ... 34 = Messiaen 7). Interval sets are music theory, not copied data. No JUCE, header only.

#include <array>
#include <cstdint>
#include <string_view>

namespace sonora::model
{

struct ScaleDef
{
    std::string_view name;
    std::array<int8_t, 12> intervals; // semitones above the root, ascending; `size` used
    int size;
};

inline constexpr std::array<ScaleDef, 35> kScales{{
    {"Major", {0, 2, 4, 5, 7, 9, 11}, 7},
    {"Minor", {0, 2, 3, 5, 7, 8, 10}, 7},
    {"Dorian", {0, 2, 3, 5, 7, 9, 10}, 7},
    {"Mixolydian", {0, 2, 4, 5, 7, 9, 10}, 7},
    {"Lydian", {0, 2, 4, 6, 7, 9, 11}, 7},
    {"Phrygian", {0, 1, 3, 5, 7, 8, 10}, 7},
    {"Locrian", {0, 1, 3, 5, 6, 8, 10}, 7},
    {"Whole Tone", {0, 2, 4, 6, 8, 10}, 6},
    {"Half-whole Dim.", {0, 1, 3, 4, 6, 7, 9, 10}, 8},
    {"Whole-half Dim.", {0, 2, 3, 5, 6, 8, 9, 11}, 8},
    {"Minor Blues", {0, 3, 5, 6, 7, 10}, 6},
    {"Minor Pentatonic", {0, 3, 5, 7, 10}, 5},
    {"Major Pentatonic", {0, 2, 4, 7, 9}, 5},
    {"Harmonic Minor", {0, 2, 3, 5, 7, 8, 11}, 7},
    {"Harmonic Major", {0, 2, 4, 5, 7, 8, 11}, 7},
    {"Dorian #4", {0, 2, 3, 6, 7, 9, 10}, 7},
    {"Phrygian Dominant", {0, 1, 4, 5, 7, 8, 10}, 7},
    {"Melodic Minor", {0, 2, 3, 5, 7, 9, 11}, 7},
    {"Lydian Augmented", {0, 2, 4, 6, 8, 9, 11}, 7},
    {"Lydian Dominant", {0, 2, 4, 6, 7, 9, 10}, 7},
    {"Super Locrian", {0, 1, 3, 4, 6, 8, 10}, 7},
    {"8-Tone Spanish", {0, 1, 3, 4, 5, 6, 8, 10}, 8},
    {"Bhairav", {0, 1, 4, 5, 7, 8, 11}, 7},
    {"Hungarian Minor", {0, 2, 3, 6, 7, 8, 11}, 7},
    {"Hirajoshi", {0, 2, 3, 7, 8}, 5},
    {"In-Sen", {0, 1, 5, 7, 10}, 5},
    {"Iwato", {0, 1, 5, 6, 10}, 5},
    {"Kumoi", {0, 2, 3, 7, 9}, 5},
    {"Pelog Selisir", {0, 1, 3, 7, 8}, 5},
    {"Pelog Tembung", {0, 1, 5, 7, 8}, 5},
    {"Messiaen 3", {0, 2, 3, 4, 6, 7, 8, 10, 11}, 9},
    {"Messiaen 4", {0, 1, 2, 5, 6, 7, 8, 11}, 8},
    {"Messiaen 5", {0, 1, 5, 6, 7, 11}, 6},
    {"Messiaen 6", {0, 2, 4, 5, 6, 8, 10, 11}, 8},
    {"Messiaen 7", {0, 1, 2, 3, 5, 6, 7, 8, 9, 11}, 10},
}};

// The scale for a stored index (out-of-range indices, e.g. a newer file's custom scale, fall back to Major).
constexpr const ScaleDef& scaleAt(int index)
{
    return kScales[(size_t) (index >= 0 && index < (int) kScales.size() ? index : 0)];
}

// Index of a scale stored by name (Live 11 sets write <Name Value="Dorian"/>), or -1. Case-insensitive.
inline int scaleIndexByName(std::string_view name)
{
    for (size_t i = 0; i < kScales.size(); ++i)
    {
        const auto& candidate = kScales[i].name;
        if (candidate.size() != name.size()) continue;
        bool same = true;
        for (size_t c = 0; c < name.size() && same; ++c)
            same = (candidate[c] | 0x20) == (name[c] | 0x20);
        if (same) return (int) i;
    }
    return -1;
}

// Whether `pitch` belongs to scale `index` rooted at pitch class `root`.
constexpr bool inScale(int pitch, int root, int index)
{
    const auto& scale = scaleAt(index);
    const int pitchClass = ((pitch - root) % 12 + 12) % 12;
    for (int i = 0; i < scale.size; ++i)
        if (scale.intervals[(size_t) i] == pitchClass) return true;
    return false;
}

// Moves `note` by `degrees` steps of `scale` rooted at pitch class `root` (0 = C). A note outside the scale keeps its
// distance above the scale step below it, so chromatic input stays chromatic relative to the moved step.
constexpr int transposeInScale(int note, int degrees, int root, const ScaleDef& scale)
{
    auto floorDiv = [](int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); };
    const int relative = note - root;
    const int octave = floorDiv(relative, 12);
    const int pitchClass = relative - octave * 12;
    int step = 0;
    for (int i = 0; i < scale.size; ++i)
        if (scale.intervals[(size_t) i] <= pitchClass) step = i;
    const int extra = pitchClass - scale.intervals[(size_t) step];
    const int total = step + degrees;
    const int octaves = floorDiv(total, scale.size);
    const int index = total - octaves * scale.size;
    return root + (octave + octaves) * 12 + scale.intervals[(size_t) index] + extra;
}

} // namespace sonora::model
