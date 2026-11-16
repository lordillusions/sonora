#pragma once
// Device grouping (ROADMAP DEV-15c): Group (Cmd+G) wraps adjacent devices of a chain in a rack, like the reference's
// Edit > Group; Ungroup (Shift+Cmd+G) puts the devices of a one-chain rack back into the chain that holds it.
//
// A device list is addressed by a path from the track: {} is the track's own chain, {rack, chain} a rack chain on the
// track, {rack, chain, rack2, chain2} one nested inside it, and so on (indices into the lists).

#include "core/Edit.h"

#include <string>
#include <vector>

namespace sonora::model
{

// The rack type that groups list[first, first + count), as the reference picks it: an Instrument Rack when one of the
// devices is an instrument, a MIDI Effect Rack for MIDI effects only, an Audio Effect Rack for audio effects only. Empty when
// they cannot be grouped: out of range, MIDI and audio effects without an instrument, or a device whose role is unknown.
std::string groupRackType(const std::vector<Device>& list, int first, int count);
// The default name of a rack of that type ("Audio Effect Rack", "Instrument Rack", "MIDI Effect Rack").
std::string groupRackName(const std::string& rackType);
// `ids` (any order) as one contiguous range of `list`; false when one is missing or they are not adjacent.
bool groupableDevices(const std::vector<Device>& list, const std::vector<int>& ids, int& first, int& count);
// Whether `device` can be ungrouped: an Instrument / Audio Effect / MIDI Effect Rack with exactly one chain that holds devices.
bool canUngroupDevice(const Device& device);

// The device list at `path` (see above); nullptr when the path does not lead to one.
std::vector<Device>* deviceListAt(Track& track, const std::vector<int>& path);
const std::vector<Device>* deviceListAt(const Track& track, const std::vector<int>& path);
// The same inside a track-level rack: `path` without its first index ({chain, rack2, chain2, ...}).
std::vector<Device>* chainListAt(Device& rack, const std::vector<int>& path);

// Moves list[first, first + count) into the first chain of `rack` (an authored rack of groupRackType, with an id; the
// app makes it with devices::nativeDeviceModel). The rack takes their place; its chain is named after the first device
// when it has no name, and shows its devices. False (list untouched) when the range is invalid.
bool groupDevicesInList(std::vector<Device>& list, int first, int count, Device rack);
// Replaces list[index] (canUngroupDevice) by its chain's devices. They leave the rack's macros (their macro mappings are
// removed); a device whose id another device of `list` already has gets a fresh one from `nextDeviceId`. Returns how
// many devices took the rack's place, -1 when list[index] cannot be ungrouped.
int ungroupDeviceInList(std::vector<Device>& list, int index, int& nextDeviceId);

// Undoable Group of the track-level devices `deviceIds` (adjacent, see groupableDevices) into `rack`. The devices move
// with the state they have when the command runs (after the engine state was copied into the model) and keep their ids,
// so the engine hands their running plug-in instances to the rack. Undo puts them back with their state at that time.
// Empty when the selection cannot be grouped or `rack` has no id.
EditCommand groupDevices(const Song& song, int track, const std::vector<int>& deviceIds, Device rack);
// Undoable Ungroup of the track-level rack `rackId` (canUngroupDevice). Ids that would repeat on the track are replaced by
// fresh ones from song.nextDeviceId (raised when the command runs). Empty when it cannot be ungrouped.
EditCommand ungroupDevice(const Song& song, int track, int rackId);

} // namespace sonora::model
