#pragma once
// Reads Ableton Live Set files (.als = gzip-compressed XML) into sonora::model::Song.
//
// Anything we cannot represent is skipped and reported in
// Song::warnings — loading must never fail just because a device is unknown.
// Format notes: docs/ALS_FORMAT.md

#include "core/Edit.h"
#include "core/Model.h"

#include <juce_core/juce_core.h>

#include <functional>
#include <memory>
#include <string>

namespace sonora::io
{

struct AlsLoadResult
{
    std::unique_ptr<model::Song> song; // null on failure
    std::string error;                 // set on failure

    explicit operator bool() const { return song != nullptr; }
};

// Reads the raw XML DOM of a .als (gzip) or plain XML file. nullptr + `error` on failure. The writer starts from this.
std::unique_ptr<juce::XmlElement> readAlsDom(const juce::File& file, std::string& error);

// Loads a .als file from disk. Also accepts an uncompressed XML file (handy for tests).
AlsLoadResult loadAls(const juce::File& file);

// Parses an already-decoded <Ableton> root element.
AlsLoadResult parseAls(const juce::XmlElement& abletonRoot, const std::string& sourcePath);

// A new, empty group track for `song` (unique track id, automation ids numbered from song.nextPointeeId, one launch slot per
// scene and one send per return track) together with the authored XML the writer will insert (Track::sourceXml).
// `pointeeEnd` receives the first automation id the track did not use; pass it to model::groupTracks.
model::Track createGroupTrack(const model::Song& song, const std::string& name, int colourIndex, int& pointeeEnd);
// An empty authored audio track. `trackId` < 0 picks the next free id; `firstPointeeId` < 0 starts at the song's next pointee id
// (pass the previous call's `pointeeEnd` when creating several tracks for one edit).
model::Track createAudioTrack(const model::Song& song, const std::string& name, int colourIndex, int trackId, int firstPointeeId, int& pointeeEnd);
model::Track createMidiTrack(const model::Song& song, const std::string& name, int colourIndex, int& pointeeEnd);
// TRK-2: a new return track ("C-Return", ...) at the end of the returns, with a -inf send holder on every track (disabled on
// returns, which only get one when their holders already match the return count) and a SendPreBool. Pass to insertReturnTrack.
model::ReturnInsert createReturnTrack(const model::Song& song);

// Resolves a sample FileRef against the project folder. Returns "" when nothing exists.
std::string resolveSamplePath(const model::Song& song, const model::FileRef& ref);

// The project-folder resolution alone, ignoring any override (a collab host resolving its own files in a process
// that also runs a guest: tests and tools/collab_probe).
std::string resolveProjectSamplePath(const model::Song& song, const model::FileRef& ref);

// COL-4: a collab guest resolves samples only through the session's shared assets (never through paths the host
// named on its own disk). The override answers every reference while installed ("" = not available yet); nullptr
// removes it. Thread-safe; called from loaders on any thread.
using SamplePathOverride = std::function<std::string(const model::Song&, const model::FileRef&)>;
void setSamplePathOverride(std::shared_ptr<const SamplePathOverride>);

// Decodes the whitespace-separated hex blobs Live uses for plugin state (<Buffer>, ...).
std::vector<uint8_t> decodeHexBlob(const std::string& text);

} // namespace sonora::io
