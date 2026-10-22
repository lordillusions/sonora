#pragma once
// Saves a (possibly edited) Song back to a Live Set (ROADMAP ALS-10).
//
// The writer never builds a file from scratch: it re-reads the XML of the set the song was loaded
// from, patches the elements Sonora can edit, and writes the result. Everything it does not touch —
// Max devices, untouched automation, view data, unknown elements — passes through unchanged.
//
// Patched: tempo, loop, mixer values (volume, pan, activator, solo, sends), device on/off and
// native device parameters, arrangement clips (position, length, loop, name, colour, enabled, MIDI
// notes; existing/duplicated clips use immutable retained source elements), tracks' clip lists.
// Also patched: track automation, session clips, scenes, locators, track names, recorded sample refs.
// Device lists retain their source elements; inserted native/VST3 devices and first audio clips use
// Sonora-authored templates. Hosted VST3 component/controller state is patched before saving.
// Also patched: changed warp-marker lists/modes and MIDI note probability, release velocity and deviation.
//
// Disposable edited/insertion probes open in Live 12; every possible edited output is not verified.

#include "core/Model.h"

#include <juce_core/juce_core.h>

#include <cstdint>
#include <map>
#include <string>

namespace sonora::io
{

struct AlsSaveResult
{
    bool ok = false;
    std::string error;
    int clipsAdded = 0, clipsRemoved = 0, clipsUpdated = 0;
    // Project/sample management (AlsSaveOptions): files copied, referenced files that could not be found,
    // and old absolute path -> new absolute path for every sample moved into the destination project.
    int samplesCopied = 0, samplesMissing = 0;
    std::uint64_t bytesCopied = 0;
    std::map<std::string, std::string> movedSamples;
};

struct AlsSaveOptions
{
    // Anchor project-relative sample references that would otherwise dangle in another folder.
    bool portableReferences = false;
    // Copy the source project's Samples folder (recordings, imports) into the destination project and keep
    // those references project-relative. Used by Save As and by saving a never-saved project.
    bool copyProjectSamples = false;
    // "Collect All and Save": copy every referenced sample that lives outside the destination project into
    // <project>/Samples/Imported and point the references at the copies (relative to the project).
    bool collectAll = false;
    // Files belonging to Live Packs are normally left where they are; collect them as well when set.
    bool collectLivePackFiles = false;
};

// Writes `song` (patched onto the XML of song.sourcePath) to `destination`. A destination ending in
// ".xml" is written uncompressed (tests), anything else gzip-compressed like Live does.
// When `destination` is the source file itself, a copy is stored in a sibling "Backup" folder first.
// portableReferences anchors existing project-relative sample refs when saving into another folder.
// The app enables it; format regression tools leave it off to compare untouched XML verbatim.
AlsSaveResult saveAls(const model::Song& song, const juce::File& destination, bool portableReferences = false);
AlsSaveResult saveAls(const model::Song& song, const juce::File& destination, const AlsSaveOptions& options);

// Writes `root` as a Live Set (gzip, or plain XML for a ".xml" destination) through a temporary file.
bool writeAlsDom(const juce::XmlElement& root, const juce::File& destination, std::string& error);

// Refresh current-file clip locations after saving. This map is separate from undo snapshots, whose
// stable UIDs and retained templates survive deletion, reordering, and subsequent saves.
void refreshClipIdentities(model::Song& song);

// The patching step alone (tests): applies `song` onto `root` in memory.
AlsSaveResult patchAlsDom(const model::Song& song, juce::XmlElement& root);

struct TrackDuplication
{
    std::vector<model::Track> tracks;
    int before = -1, pointeeEnd = 0;
    std::string error;
};
// Materialize current edits in memory, clone complete track XML and rebind internal identities. Groups include descendants.
// Caller captures live device/mixer state first, then inserts the result through model::insertTracks as one history entry.
TrackDuplication prepareTrackDuplication(const model::Song&, const std::vector<int>& selection);

} // namespace sonora::io
