#pragma once
// Applies a MIDI control change (or any other "a control moved" event) to the engine.
// Message thread. Pure functions of (target, change, graph, transport) so they are unit tested.
// ROADMAP MIDI-2.

#include "engine/EngineGraph.h"
#include "engine/Transport.h"
#include "midi/ControlTarget.h"

#include <optional>

namespace sonora::engine
{
class RecordingManager;

// Current value of a target normalised to 0..1 (volume uses Live's fader curve), or nullopt
// when the target does not exist in this graph (track out of range, unknown parameter...).
std::optional<float> readControl(const midi::ControlTarget& target, const EngineGraph* graph, const Transport& transport);

// Returns true when something was changed.
bool applyControl(const midi::ControlTarget& target, const midi::ControlChange& change, EngineGraph* graph,
                  Transport& transport);
// Session launches need the message-thread model to resolve stable clip identity and launch settings.
bool applySessionClipControl(const midi::ControlTarget&, const midi::ControlChange&, const model::Song&, EngineGraph*,
                             Transport&, const RecordingManager* recording = nullptr);

} // namespace sonora::engine
