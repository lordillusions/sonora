#pragma once
// File > Export MIDI Clip (UI-25b): a MIDI clip as a Standard MIDI File (format 1: a tempo/meter track and a note track,
// 960 ticks per quarter note). The clip is exported from its start marker to its end (the loop end), once, so the file
// starts with what plays first; notes that cross the end are cut there, muted notes are left out. No JUCE audio module.

#include "core/Model.h"

#include <cstdint>
#include <string>
#include <vector>

namespace sonora::io
{

std::vector<uint8_t> midiClipToSmf(const model::Clip& clip, double tempo, int timeSigNumerator, int timeSigDenominator,
                                   int ticksPerQuarter = 960);

// Writes the file (replacing an existing one). Returns false and sets `error` on failure.
bool exportMidiClip(const model::Clip& clip, const model::Song& song, const std::string& path, std::string& error);

} // namespace sonora::io
