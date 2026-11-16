#pragma once
// SPL-1: musical-key math for Splice samples (docs/SPLICE_INTEROP.md).
//
// Splice reports a sample's root note ("a", "c#", "db"...) and chord_type ("minor"/"major"/empty). The rule:
// transpose by the SMALLEST number of semitones, counting a relative major/minor as the same key (A minor fits C major
// with no transpose), never more than +/-6 semitones. All pure, header + small .cpp, no JUCE.

#include <optional>
#include <string>

namespace sonora::platform::splice
{

enum class KeyMode { None, Major, Minor };

struct MusicalKey
{
    int pitchClass = 0;            // 0 = C .. 11 = B
    KeyMode mode = KeyMode::None;
    bool valid = false;
};

// Parses a Splice key letter (+ optional accidentals '#','s' sharp / 'b' flat) and chord_type into a MusicalKey.
// An empty / unrecognised note returns { valid = false }.
MusicalKey parseSpliceKey(const std::string& note, const std::string& chordType);

// The project's key from Song::scaleRoot (0..11) and Song::scaleName (index into model::kScales; 0 Major, 1 Minor, ...).
MusicalKey projectKey(int scaleRoot, int scaleName);

// Semitones to transpose `sample` so it sits in `project`, folded into [-6, +6] with the relative-key rule. When either
// key lacks a mode, tonic-to-tonic is used. Returns 0 when `sample` is invalid (nothing is known to match).
int transposeToProject(const MusicalKey& sample, const MusicalKey& project);

} // namespace sonora::platform::splice
