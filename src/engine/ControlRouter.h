#pragma once
// Applies a MIDI control change (or any other "a control moved" event) to the engine.
// Message thread. Pure functions of (target, change, graph, transport) so they are unit tested.
// ROADMAP MIDI-2.

#include "engine/EngineGraph.h"
#include "engine/Transport.h"
#include "midi/ControlTarget.h"

#include <map>
#include <optional>
#include <vector>

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

// MIDI-10: MIDI Map mode for hosted plug-ins. A plug-in's parameters have no controls in the device view, so Map mode
// learns the parameter the user last touched in the plug-in's own window (the edits it reports, UI-65). Message thread.
class TouchedParameterWatch
{
public:
    struct Touch
    {
        midi::ControlTarget target; // DeviceParameter of a top-level device
        std::string parameterName;
    };
    // The newest touch since the previous poll, or nullopt. Edits reported before a device was first seen do not count;
    // many parameters changing at once without a gesture is a preset load, not a touch.
    std::optional<Touch> poll(const EngineGraph* graph);
    void reset() { seen_.clear(); }

private:
    struct Seen
    {
        uint32_t total = 0;
        std::vector<uint32_t> serials;
    };
    std::map<const devices::DeviceProcessor*, Seen> seen_;
};

} // namespace sonora::engine
