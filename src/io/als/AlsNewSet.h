#pragma once
// Authoring of brand-new Live Sets (File > New Project).
//
// The writer (AlsWriter) always patches an existing document, so a Sonora-created project starts
// from a generated empty Live Set: default tracks, scenes, transport and view state written from
// the documented file format (element names follow the Live 12 schema). Nothing is copied from
// Live's own template files. The result is parsed with parseAls() and written to disk once so the
// normal save path (patch the source document, then write) applies unchanged.

#include <juce_core/juce_core.h>

#include <memory>
#include <string>

namespace sonora::io
{

struct NewSetOptions
{
    int midiTracks = 2;
    int audioTracks = 2;
    int returnTracks = 2;
    int scenes = 8;
    double tempo = 120.0;
};

// Root <Ableton> element of an empty set.
std::unique_ptr<juce::XmlElement> createNewSetDom(const NewSetOptions& options = {});

// An authored <GroupTrack> element (Sonora's "Group Tracks"): `scenes` GroupTrackSlots, `sends` send holders, automation
// ids numbered from `firstPointeeId`; `pointeeEnd` receives the first id it did not use.
std::string groupTrackXml(int trackId, const std::string& name, int colour, int scenes, int sends, int firstPointeeId, int& pointeeEnd);

// An authored empty <AudioTrack> (used for stems and other tracks Sonora adds): same conventions as groupTrackXml.
std::string audioTrackXml(int trackId, const std::string& name, int colour, int scenes, int sends, int firstPointeeId, int& pointeeEnd);

// TRK-2: an authored empty <ReturnTrack> with `sends` disabled return-to-return holders, and one <TrackSendHolder> at -inf
// (`enabled` = EnabledByUser; false for holders on return tracks). Same automation-id conventions as groupTrackXml.
std::string returnTrackXml(int trackId, const std::string& name, int colour, int sends, int firstPointeeId, int& pointeeEnd);
std::string sendHolderXml(int index, bool enabled, int firstPointeeId, int& pointeeEnd);

// A fresh, empty <MidiClip> element (4 beats, looping) used as the format template when the set
// has no MIDI clip to copy from. Callers set name/colour/loop through the normal clip edit path.
std::string midiClipTemplate();
// AGT-7: authored MIDI track for composition workflows.
std::string midiTrackXml(int trackId, const std::string& name, int colour, int scenes, int sends, int firstPointeeId, int& pointeeEnd);

} // namespace sonora::io
