// DEV-15c: Group (Cmd+G) / Ungroup (Shift+Cmd+G) of devices in the Device View, like the reference's Edit > Group.
//
// The selected adjacent devices of one chain (the track's, or a rack chain shown in the Device View) move into a new
// Instrument / Audio Effect / MIDI Effect Rack (model::groupRackType) with one chain; Ungroup puts the devices of a
// one-chain rack back. Track-level edits are model::groupDevices / ungroupDevice; inside a rack chain the edit goes through
// editDeviceIf (the track-level rack is rebuilt). Devices keep their ids, so updateTrackDevices hands their running
// plug-in instances to the new processors.
#include "app/AppController.h"
#include "core/DeviceGroups.h"
#include "devices/DeviceFactory.h"

namespace sonora::app
{

void AppController::groupSelectedDevices()
{
    if (! song_ || editingDevices_ || syncingModel_ || exporting_) return;
    if (ui_.recording || ui_.recordingRequested) { ui_.statusText = "Finish recording before grouping devices"; return; }
    const int trackIndex = ui_.selectedTrack;
    const auto path = ui_.selectedDevicePath();
    const auto ids = ui_.selectedDeviceIds();
    if (ids.empty()) { ui_.statusText = "Select devices (Shift+click a title for a range, Cmd+click to add), then Cmd+G"; return; }
    const int generation = documentGeneration_;
    syncModelFromEngine(); // the devices move into the rack with the state they have now
    if (generation != documentGeneration_ || ! song_) return;
    const auto* track = model::trackAt(*song_, trackIndex);
    const auto* list = track ? model::deviceListAt(*track, path) : nullptr;
    int first = 0, count = 0;
    if (list == nullptr || ! model::groupableDevices(*list, ids, first, count))
    { ui_.statusText = "Only devices next to each other can be grouped"; return; }
    const auto type = model::groupRackType(*list, first, count);
    if (type.empty()) { ui_.statusText = "These devices cannot share a rack (MIDI and audio effects need an instrument between them)"; return; }

    auto rack = devices::nativeDeviceModel(type);
    rack.displayName = model::groupRackName(type);
    rack.numVisibleMacros = 8; // a new rack of the reference shows 8 macros
    rack.macrosVisible = true;
    rack.chainListVisible = false;
    rack.id = -1;
    model::allocateDeviceIds(*song_, rack);
    const int rackId = rack.id;
    if (path.empty())
    {
        if (! applyEdit(model::groupDevices(*song_, trackIndex, ids, std::move(rack))).ok) return;
        ui_.selectedRackId = rackId; // its chain's devices show next to it
        ui_.selectedRackChain = 0;
    }
    else
    {
        bool grouped = false;
        const std::vector<int> inner(path.begin() + 1, path.end());
        editDeviceIf(trackIndex, path[0], [&](model::Device& root) {
            auto* chain = model::chainListAt(root, inner);
            grouped = chain != nullptr && model::groupDevicesInList(*chain, first, count, rack);
            return grouped;
        }, count == 1 ? "Group " + (*list)[(size_t) first].displayName : "Group Devices");
        if (! grouped) return;
    }
    ui_.rackViews[rackId] = {true, false, 0};
    ui_.selectDevice(trackIndex, path, rackId, false, false);
    ui_.statusText = "Grouped " + std::to_string(count) + (count == 1 ? " device into " : " devices into ")
                     + (type == "MidiEffectGroupDevice" ? "a " : "an ") + model::groupRackName(type);
    if (ui_.modelChanged) ui_.modelChanged();
}

void AppController::ungroupSelectedDevice()
{
    if (! song_ || editingDevices_ || syncingModel_ || exporting_) return;
    if (ui_.recording || ui_.recordingRequested) { ui_.statusText = "Finish recording before ungrouping devices"; return; }
    if (! ui_.canUngroupSelectedDevice()) { ui_.statusText = "Select a rack with one chain to ungroup it"; return; }
    const int trackIndex = ui_.selectedTrack;
    const auto path = ui_.selectedDevicePath();
    const int rackId = ui_.selectedDeviceIds().front();
    const int generation = documentGeneration_;
    syncModelFromEngine();
    if (generation != documentGeneration_ || ! song_) return;
    const auto* track = model::trackAt(*song_, trackIndex);
    const auto* list = track ? model::deviceListAt(*track, path) : nullptr;
    if (list == nullptr) return;
    int index = -1;
    for (size_t i = 0; i < list->size(); ++i) if ((*list)[i].id == rackId) index = (int) i;
    if (index < 0 || ! model::canUngroupDevice((*list)[(size_t) index])) return;
    const auto name = (*list)[(size_t) index].displayName;
    const int count = (int) (*list)[(size_t) index].chains.front().devices.size();
    if (path.empty())
    {
        if (! applyEdit(model::ungroupDevice(*song_, trackIndex, rackId)).ok) return;
    }
    else
    {
        bool ungrouped = false;
        const std::vector<int> inner(path.begin() + 1, path.end());
        editDeviceIf(trackIndex, path[0], [&](model::Device& root) {
            auto* chain = model::chainListAt(root, inner);
            ungrouped = chain != nullptr && model::ungroupDeviceInList(*chain, index, song_->nextDeviceId) > 0;
            return ungrouped;
        }, "Ungroup " + name);
        if (! ungrouped) return;
    }
    if (ui_.selectedRackId == rackId) { ui_.selectedRackId = -1; ui_.selectedRackChain = 0; }
    // The devices that took the rack's place are the selection now.
    track = model::trackAt(*song_, trackIndex);
    list = track ? model::deviceListAt(*track, path) : nullptr;
    if (list != nullptr && index + count <= (int) list->size())
    {
        ui_.selectDevice(trackIndex, path, (*list)[(size_t) index].id, false, false);
        if (count > 1) ui_.selectDevice(trackIndex, path, (*list)[(size_t) (index + count - 1)].id, true, false);
    }
    ui_.statusText = "Ungrouped " + name;
    if (ui_.modelChanged) ui_.modelChanged();
}

} // namespace sonora::app
