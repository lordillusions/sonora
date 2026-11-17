#pragma once
// PLG-25a: "Apply <generator> to Clip" and the faces' To Clip button, model side. A pattern generator (Sonora Phrase,
// Sonora Pulse) is a function of the song position (TRANCE_SUITE.md "Decisions" 3), so what it plays over a clip's span
// can be written into the clip as ordinary notes: one undo step, like Generate Drum Fill.
//
// This layer knows no devices: the caller hands a GeneratorRender (devices::MidiGenerator::renderNotes on a clone of the
// running device, see ui/GeneratorClipUi.h). Time mapping: clip content beat c plays at song beat
// origin + (c - loopStart), where origin is the clip's arrangement start (minus its start offset) for Arrangement and
// take-lane clips and 0 for Session clips (a launched clip starts its loop on a bar, the generator's cycles start at 0).

#include "core/Edit.h"
#include "core/Model.h"

#include <functional>
#include <string>
#include <vector>

namespace sonora::model
{
// What the generator plays over song beats [from, to) with `input` held (notes in song beats). Returns song-beat notes.
using GeneratorRender = std::function<std::vector<MidiNote>(const std::vector<MidiNote>& input, Beats from, Beats to)>;

struct GeneratorClipSpec
{
    std::string deviceName;  // "Sonora Phrase": the undo label ("Apply Sonora Phrase to Clip") and a new clip's name
    bool usesInput = false;  // the clip's own notes are the generator's input (Phrase in MIDI In mode, Pulse)
    GeneratorRender render;
};

// Song beat where content beat `loopStart` of the clip plays (see the header comment).
Beats generatorClipOrigin(const Clip& clip, const ClipLocation& location);

// The MIDI clip at `location`: its notes inside the loop span [loopStart, loopEnd) become the generator's output over
// that span (notes outside the span stay). Empty command and a `message` saying why when it cannot.
EditCommand applyGeneratorToClip(const Song& song, const ClipLocation& location, const GeneratorClipSpec& spec, std::string& message);

// A new Arrangement MIDI clip on `track` over [start, start + length) holding the generator's output with no input.
// Refused (empty command, `message`) when the track is not a MIDI track or a clip is already there.
EditCommand insertGeneratorClip(const Song& song, int track, Beats start, Beats length, const GeneratorClipSpec& spec,
                                std::string& message);

// Where To Clip with no clip selected puts the new clip: the loop start when the loop is on, otherwise the bar the
// playhead is in.
Beats generatorClipStart(const Song& song, Beats playhead, bool loopOn, Beats loopStart);
} // namespace sonora::model
