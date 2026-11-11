#pragma once
// PLG-20: drum fills written into a MIDI clip. Reads the clip's groove (grid, swing, velocities, density) and its kit
// (Drum Rack pad names, else General MIDI numbers), then replaces the notes of the clip's last `beats` with a fill in one
// of a few styles, chosen at random (weighted by the tempo and the pieces the kit has). The notes stay ordinary notes:
// edit them, or ask again for another fill (each request is one undo step). Model layer only: no audio, no UI.

#include "core/Edit.h"
#include "core/Model.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace sonora::model
{
enum class DrumRole { Kick, Snare, Clap, ClosedHat, OpenHat, TomHigh, TomMid, TomLow, Crash, Ride, Count };

struct DrumKit
{
    std::array<int, (size_t) DrumRole::Count> pitch; // -1: the kit has no such piece
    DrumKit() { pitch.fill(-1); }
    bool has(DrumRole r) const { return pitch[(size_t) r] >= 0; }
    int operator[](DrumRole r) const { return pitch[(size_t) r]; }
    int toms() const;
};

// The kit on `track`: its first Drum Rack's pads by name ("Kick", "Snare 2", "Hi Tom"...), then General MIDI numbers for
// the pitches `notes` uses that no pad name explained, then General MIDI defaults for what is still missing.
DrumKit drumKitFor(const Track& track, const std::vector<MidiNote>& notes);
DrumRole roleForName(const std::string& padName, bool& known);

struct Groove
{
    bool triplet = false;   // the clip moves in triplets
    double swing = 0.0;     // beats the off 16ths are late by (0 = straight)
    float kickVelocity = 100, snareVelocity = 100, hatVelocity = 85;
    double notesPerBeat = 2.0;
};
Groove analyseGroove(const std::vector<MidiNote>& notes, const DrumKit& kit, double from, double to);

enum class FillStyle { SnareRoll, TomRun, KickSnare, HatBuild, Triplet, Break, Count };
const char* fillStyleName(FillStyle);

struct FillResult
{
    std::vector<MidiNote> notes; // every note of the clip: outside the region untouched, the fill inside it
    FillStyle style = FillStyle::SnareRoll;
};
// `style` < 0: chosen from `seed`. `tempo` steers the choice (fast: rolls and hat builds; slow: tom runs and breaks).
FillResult generateFill(const std::vector<MidiNote>& notes, double regionStart, double regionEnd, const DrumKit& kit,
                        const Groove& groove, double tempo, uint32_t seed, int style = -1);

// The clip at `location` (MIDI) gets a fill over its last `beats` (of its loop / content). `message` says which style.
EditCommand drumFill(const Song& song, const ClipLocation& location, double beats, uint32_t seed, std::string& message);
} // namespace sonora::model
