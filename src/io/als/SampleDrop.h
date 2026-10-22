#pragma once
// Placing audio files on tracks (browser drags, Finder drops, File > Import Audio). Builds one undoable edit,
// following Live: several files stack on the target track (Arrangement: end to end; Session: consecutive slots),
// or, with "one track per file" (Cmd while dropping), each further file gets a new audio track after the target.
// Message thread; the files are decoded beforehand, so this never touches the disk.

#include "core/Edit.h"

#include <cstdint>
#include <string>
#include <vector>

namespace sonora::io
{

struct DroppedSample
{
    std::string path;          // absolute
    double sampleRate = 0;
    int64_t frames = 0;
    double detectedBpm = 0;    // UI-47: tempo found while reading the file (0: none, the set's tempo is assumed)
    double tempoConfidence = 0; // UI-47b: 0..1, how clear that tempo was
};

struct SampleDropTarget
{
    int track = -1;            // an audio track index, or -1 for a new audio track after the last track
    double beat = 0;           // Arrangement start (scene < 0)
    int scene = -1;            // >= 0: Session slot of the first file
    bool oneTrackPerFile = false;
};

struct SampleDropPlan
{
    model::EditCommand command; // no apply when nothing could be placed
    int firstTrack = -1;        // track index that receives the first file once the command is applied
    int placed = 0;
    bool newTrackForKind = false; // the target held no audio (MIDI/group track): a new audio track after it took the files
    std::string error;          // why nothing was placed
};

// A new audio clip referencing `sample`, with a set's existing audio clip XML reused as its template when present.
model::Clip makeSampleClip(const model::Song& song, const DroppedSample& sample, int colorIndex, double start);

// UI-47b: song-length files (>= 30 s) dropped together with the same length and rate are stems of one song (vocals, bass, drums...): they all
// take the clearest tempo among them, so they stay in sync even where one stem has no detectable pulse.
// Returns how many samples changed.
int shareStemTempo(std::vector<DroppedSample>& samples);

SampleDropPlan placeSamples(const model::Song& song, const std::vector<DroppedSample>& samples, SampleDropTarget target);

} // namespace sonora::io
