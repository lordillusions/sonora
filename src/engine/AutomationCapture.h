#pragma once
// Automation recording (Live's "Automation Arm"): while the Arrangement records, moving a mixer or native device control
// writes breakpoints into that control's automation lane.
//
// The capture polls the control atomics from the message thread (about 60 times a second): any control whose value changed
// since the previous poll is being moved by hand. Controls that follow an existing lane count as moved only after the
// audio thread flagged them as overridden (see Automation.h), so the lane's own movement is never recorded.
// finish() turns what was touched into envelopes: the recorded points replace the lane between the first touch and the
// stop, a touched control holds its last value until the recording ends (like Live).

#include "core/Model.h"
#include "engine/Automation.h"

#include <map>
#include <tuple>
#include <vector>

namespace sonora::engine
{

struct EngineGraph;

class AutomationCapture
{
public:
    void begin();
    // Message thread; `beat` is the transport position.
    void poll(const model::Song& song, const EngineGraph& graph, double beat);
    bool touched() const { return ! series_.empty(); }

    struct TouchedEnvelope
    {
        int track = 0; // index in the track / control space of the song
        model::AutomationEnvelope envelope;
        AutomationBinding::Kind kind = AutomationBinding::Kind::Volume;
        int index = 0, deviceSlot = 0;
    };
    // The envelopes to store (existing lanes with the recorded stretch replaced) and forget what was captured.
    std::vector<TouchedEnvelope> finish(const model::Song& song, const EngineGraph& graph, double endBeat);

private:
    struct Key
    {
        int track;
        AutomationBinding::Kind kind;
        int index, slot;
        bool operator<(const Key& o) const
        {
            return std::tie(track, kind, index, slot) < std::tie(o.track, o.kind, o.index, o.slot);
        }
    };
    struct Series
    {
        int targetId = -1;
        double before = 0.0, last = 0.0, lastTime = 0.0;
        std::vector<model::AutomationPoint> points;
    };
    std::map<Key, double> seen_, prevDriven_;
    std::map<Key, bool> wasOverridden_;
    std::map<Key, Series> series_;
};

} // namespace sonora::engine
