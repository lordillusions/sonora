#pragma once
// Automation recording (Live's "Automation Arm"): while the Arrangement records, moving a mixer or native device control
// writes breakpoints into that control's automation lane.
//
// The capture polls the control atomics from the message thread (about 60 times a second): any control whose value changed
// since the previous poll is being moved by hand. Controls that follow an existing lane count as moved only after the
// audio thread flagged them as overridden (see Automation.h), so the lane's own movement is never recorded.
// finish() turns what was touched into envelopes: the recorded points replace the lane between the first touch and the
// stop, a touched control holds its last value until the recording ends (like Live).
//
// Hosted plugins (UI-65) cannot be read back: their parameters are recorded from the edits the plugin reports from its own
// window (DeviceProcessor::handEdit). A burst of more than a few edits outside a gesture is a preset or program change
// and is not recorded. A parameter the set does not list yet comes back with targetId -1 and its host id: the caller
// gives it a target (model::pluginParameterTarget) before storing the envelope.

#include "core/Model.h"
#include "engine/Automation.h"

#include <map>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace sonora::devices
{
class DeviceProcessor;
}

namespace sonora::engine
{

struct EngineGraph;

class AutomationCapture
{
public:
    void begin();
    // Message thread; `beat` is the transport position. With `record` false (UI-77c: Automation Write mode with the
    // transport stopped) moves are only reported through takeMoved(), nothing is recorded.
    void poll(const model::Song& song, const EngineGraph& graph, double beat, bool record = true);
    bool touched() const { return ! series_.empty(); }

    // UI-77c: the controls moved by hand since the last call, in the order they were noticed (each once).
    struct Moved
    {
        int track = 0;
        AutomationBinding::Kind kind = AutomationBinding::Kind::Volume;
        int index = 0, deviceSlot = 0;
        bool hostedPlugin = false;
        int targetId = -1; // -1: a plugin parameter the set does not list yet
        double value = 0;
    };
    std::vector<Moved> takeMoved() { return std::exchange(moved_, {}); }

    struct TouchedEnvelope
    {
        int track = 0; // index in the track / control space of the song
        model::AutomationEnvelope envelope;
        AutomationBinding::Kind kind = AutomationBinding::Kind::Volume;
        int index = 0, deviceSlot = 0;
        bool hostedPlugin = false;  // a plugin parameter: hostParameterId/Name identify it when envelope.targetId < 0
        int hostParameterId = -1;
        std::string hostParameterName;
    };
    // The envelopes to store (existing lanes with the recorded stretch replaced) and forget what was captured.
    std::vector<TouchedEnvelope> finish(const model::Song& song, const EngineGraph& graph, double endBeat);
    // UI-77c: what finish() would store if the take ended at `beat`, without ending it (the lanes draw it while recording).
    std::vector<TouchedEnvelope> preview(const model::Song& song, const EngineGraph& graph, double beat) const;

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
        bool hostedPlugin = false;
        int hostParameterId = -1;
        std::string hostParameterName;
    };
    struct HostedState
    {
        const devices::DeviceProcessor* processor = nullptr;
        uint32_t total = 0;
        std::vector<uint32_t> serials;
    };
    void pollHosted(int ci, int slot, const model::Track& track, const model::Device& device,
                    const devices::DeviceProcessor& processor, double beat, bool record);
    void noteMoved(const Key& key, bool hostedPlugin, int targetId, double value);
    std::vector<Moved> moved_;
    std::map<std::pair<int, int>, HostedState> hosted_; // (control index, device slot)
    std::map<Key, double> seen_, prevDriven_;
    std::map<Key, bool> wasOverridden_;
    std::map<Key, Series> series_;
};

} // namespace sonora::engine
