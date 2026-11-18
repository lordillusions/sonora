#pragma once
// AGT-7e: helpers shared by the control service translation units (validation, track addressing, inspect, edit and live
// builders). Not part of the public control contract.
#include "control/ControlService.h"
#include "core/Op.h"
#include <cmath>
#include <stdexcept>

namespace sonora::control::detail
{
// Strict argument validation. These throw std::runtime_error (reported as invalid_request).
void fields(const juce::var& value, std::initializer_list<const char*> allowed);
double number(const juce::var& value, const char* name, double lo, double hi);
int integer(const juce::var& value, const char* name, int lo, int hi);
juce::String string(const juce::var& value, const char* name, int maximum = 256);
bool boolean(const juce::var& value, const char* name);
bool has(const juce::var& value, const char* name);

const char* kindName(model::TrackKind kind);
// Track addressing. Unknown IDs throw model::OpError("target_replaced").
int regularTrackIndex(const model::Song& song, int id);              // song.tracks index
int controlIndexOf(const model::Song& song, int id, bool allowMain); // tracks, then returns (then main)
const model::Track* findAnyTrack(const model::Song& song, int id);   // tracks, returns, main
// Whether any lane (track, Arrangement/Session/take clip envelopes) holds points for the target.
bool hasAutomation(const model::Track& track, int target);

// ---- inspect (Inspect.cpp)
juce::var inspectSnapshot(const State& state);
juce::var automationLanes(const model::Track& track);
juce::var automationDetail(const model::Song& song, const model::Track& track, int targetId, int offset, int limit);
juce::var statusSnapshot(const State& state, const Backend& backend);

// ---- edits.apply builders (EditCommands.cpp)
struct StagedEdit
{
    model::EditCommand command;
    juce::Array<juce::var> tracks; // tracks this command created: {trackId, kind, name}
    juce::Array<juce::var> clips;  // clips this command created
    juce::var extra;               // other receipt members merged into the batch receipt (object or void)
};
// Types built by EditCommands.cpp (the AGT-7e additions); the older op-backed types stay in ControlService.cpp.
bool isNewEditType(const juce::String& type);
const std::vector<std::string>& newEditTypeNames();
// Validates `item` and builds the undoable command against `staged` (not modified). Throws OpError / runtime_error.
StagedEdit buildEdit(const model::Song& staged, const juce::var& item);
// A type whose command reads or rewrites the track list or device chains: the live state goes into the model first.
bool editNeedsLiveSync(const juce::String& type);

// ---- live actions (LiveActions.cpp)
const std::vector<std::string>& liveOperationNames();
bool isLiveOperation(const juce::String& name);
// Validates arguments and returns the normalised form handed to the backend (a named track also gets `trackIndex`).
juce::var normaliseLive(const juce::String& operation, const juce::var& args, const State& state);
} // namespace sonora::control::detail
