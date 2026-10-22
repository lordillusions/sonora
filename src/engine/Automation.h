#pragma once
// Automation playback (ROADMAP ENG-11).
//
// An AutomationLane is an immutable, sorted point list evaluated at a beat position.
// An AutomationBinding ties one lane to the thing it drives (track volume, a device
// parameter, ...). Bindings are built on the message thread with the graph and evaluated on
// the audio thread once per block: no allocation, no locks.
//
// While the transport plays (or after a locate) the automated value overwrites the
// control, so faders/knobs follow the automation like in Live. A control that is moved by
// hand stops following its lane (it is "overridden", the Re-Enable Automation button lights)
// until engine::reEnableAutomation(): applyAutomation notices that the control no longer holds the
// value the lane wrote last and keeps its hands off it. Hosted plugin parameters are not read back.

#include "core/Model.h"

#include <vector>

namespace sonora::devices
{
class DeviceProcessor;
}

namespace sonora::engine
{

struct TrackControls;

class AutomationLane
{
public:
    AutomationLane() = default;
    explicit AutomationLane(const model::AutomationEnvelope& env);

    bool empty() const { return points_.empty(); }

    // Value at `beat`. Before the first point the first value holds, after the last the last.
    // Float lanes interpolate linearly (TODO(ENG-11b): bezier CurveControl*), stepped lanes
    // hold the previous point's value. Two points at the same time make a jump.
    double valueAt(model::Beats beat) const;

private:
    std::vector<model::AutomationPoint> points_;
    bool stepped_ = false;
};

struct AutomationBinding
{
    enum class Kind
    {
        Volume,      // TrackControls::volume (linear gain)
        Pan,         // TrackControls::pan (-1..1)
        Active,      // TrackControls::active (track activator, >= 0.5 == on)
        Send,        // TrackControls::sends[index] (linear gain)
        DeviceParam, // devices[device]->setParameter(index, value)
        DeviceOn,    // devices[device]->setBypassed(value < 0.5)
        Tempo,       // song tempo (main track); applied by the engine at the start of each block
    };

    Kind kind = Kind::Volume;
    int index = 0;                              // send / parameter index
    devices::DeviceProcessor* device = nullptr; // owned by the TrackNode
    AutomationLane lane;
    int deviceSlot = 0;        // position of the device in the track's model chain (override key)
    bool hostedPlugin = false; // plugin parameters cannot be read back reliably
};

// Index of what a binding drives in TrackControls' override tables; -1 for things that cannot be overridden (tempo).
int automationOverrideKey(AutomationBinding::Kind kind, int index, int deviceSlot);
// Current value of whatever the binding drives; false when it cannot be read back.
bool readBindingValue(const AutomationBinding& binding, const TrackControls& controls, double& value);
bool isAutomationOverridden(const TrackControls& controls, int key);
void setAutomationOverridden(TrackControls& controls, int key, bool on);

// Audio thread. Writes the value of every binding at `beat` into the track's controls / devices.
void applyAutomation(const std::vector<AutomationBinding>& bindings, TrackControls& controls, model::Beats beat);

// Writes one value into whatever the binding drives.
void applyBindingValue(const AutomationBinding& binding, TrackControls& controls, double value);

// Live keeps the song tempo as a main-track envelope holding one pre-roll point even when tempo is
// not automated. Such an envelope only mirrors Song::tempo and must not pin the transport to it.
bool isStaticTempoEnvelope(const model::AutomationEnvelope& env);

class Transport;
// Audio thread, main track. Moves the transport tempo along a Tempo binding unless the tempo was
// changed by hand while the lane drove it: then the lane lets go until reEnableAutomation().
void applyTempoAutomation(const AutomationBinding& binding, TrackControls& controls, Transport& transport, model::Beats beat);

struct EngineGraph;
// True while some automated control is held by hand (the Re-Enable Automation button lights).
bool automationOverridden(const EngineGraph& graph);
// Gives every overridden control back to its lane (message thread).
void reEnableAutomation(EngineGraph& graph);
// The same for one track, optionally one control key.
void reEnableAutomation(EngineGraph& graph, int controlIndex, int key = -1);

// A clip envelope (ALS-9): follows its lane (in clip beats) while the clip plays and puts the
// parameter back to `baseValue` when the clip stops.
struct ClipEnvelopeBinding
{
    bool session = false;
    int clipIndex = 0;      // arrangement clip index (TrackNode::clips) or session slot (TrackNode::sessionClips)
    AutomationBinding target; // lane + what it drives
    float baseValue = 0.0f;
    bool active = false;    // audio thread state
};

} // namespace sonora::engine
