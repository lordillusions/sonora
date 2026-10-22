#pragma once
// UI-38: media edits build immutable, reversible model changes off the audio thread.
#include "core/Edit.h"
namespace sonora::model
{
EditCommand replaceTrack(const Song&, int track, Track replacement, std::string label, int pointeeEnd = -1);
EditCommand replaceArrangementRange(const Song&, int track, double start, double end, Clip replacement);
std::vector<AutomationEnvelope> consolidatedEnvelopes(const Song&, int track, double start, double end);
Clip consolidatedMidi(const Song&, int track, double start, double end);
// Audible Arrangement bounds for the supplied track/group members; Session positions are independent.
std::optional<std::pair<Beats, Beats>> arrangementClipSpan(const Song&, const std::vector<int>& tracks);
// Frozen playback retains the editable original track and replaces only the engine's input/device chain.
Track frozenPlaybackTrack(const Track&);
}
