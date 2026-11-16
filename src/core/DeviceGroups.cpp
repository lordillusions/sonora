#include "core/DeviceGroups.h"

#include "core/DeviceCatalog.h"

#include <algorithm>
#include <memory>
#include <set>
#include <tuple>

namespace sonora::model
{
namespace
{
// Max for Live devices are not in the catalogue: their element says what they are.
DeviceRole groupRole(const Device& device)
{
    if (device.kind == DeviceKind::MaxForLive)
        return device.type == "MxDeviceInstrument" ? DeviceRole::Instrument
               : device.type == "MxDeviceMidiEffect" ? DeviceRole::MidiEffect : DeviceRole::AudioEffect;
    return deviceRole(device);
}

int indexOfId(const std::vector<Device>& list, int id)
{
    for (size_t i = 0; i < list.size(); ++i)
        if (list[i].id == id) return (int) i;
    return -1;
}

// The macro mapping of one parameter, kept so Undo of an Ungroup gives it back.
struct MacroMapping
{
    std::string path;
    int macroIndex = -1;
    double rangeMin = 0.0, rangeMax = 0.0;
    bool edited = false;
};

std::vector<MacroMapping> macroMappings(const Device& device)
{
    std::vector<MacroMapping> out;
    for (const auto& p : device.parameters)
        if (p.macroIndex >= 0 || p.macroMappingEdited) out.push_back({p.path, p.macroIndex, p.rangeMin, p.rangeMax, p.macroMappingEdited});
    return out;
}

void restoreMacroMappings(Device& device, const std::vector<MacroMapping>& mappings)
{
    for (auto& p : device.parameters)
    {
        const auto found = std::find_if(mappings.begin(), mappings.end(), [&](const MacroMapping& m) { return m.path == p.path; });
        if (found == mappings.end()) continue;
        p.macroIndex = found->macroIndex;
        p.rangeMin = found->rangeMin;
        p.rangeMax = found->rangeMax;
        p.macroMappingEdited = found->edited;
    }
}
} // namespace

std::string groupRackType(const std::vector<Device>& list, int first, int count)
{
    if (first < 0 || count <= 0 || (size_t) first + (size_t) count > list.size()) return {};
    bool instrument = false, midi = false, audio = false;
    for (int i = first; i < first + count; ++i)
        switch (groupRole(list[(size_t) i]))
        {
            case DeviceRole::Instrument: instrument = true; break;
            case DeviceRole::MidiEffect: midi = true; break;
            case DeviceRole::AudioEffect: audio = true; break;
            case DeviceRole::Unknown:
            default: return {};
        }
    if (instrument) return "InstrumentGroupDevice";
    if (midi && audio) return {}; // the reference has no rack that takes notes and gives audio without an instrument
    return midi ? "MidiEffectGroupDevice" : "AudioEffectGroupDevice";
}

std::string groupRackName(const std::string& rackType)
{
    if (rackType == "InstrumentGroupDevice") return "Instrument Rack";
    if (rackType == "MidiEffectGroupDevice") return "MIDI Effect Rack";
    if (rackType == "DrumGroupDevice") return "Drum Rack";
    return "Audio Effect Rack";
}

bool groupableDevices(const std::vector<Device>& list, const std::vector<int>& ids, int& first, int& count)
{
    std::set<int> indices;
    for (int id : ids)
    {
        const int index = indexOfId(list, id);
        if (index < 0) return false;
        indices.insert(index);
    }
    if (indices.empty() || *indices.rbegin() - *indices.begin() + 1 != (int) indices.size()) return false;
    first = *indices.begin();
    count = (int) indices.size();
    return true;
}

bool canUngroupDevice(const Device& device)
{
    return device.kind == DeviceKind::Rack && device.type != "DrumGroupDevice" && device.chains.size() == 1
           && ! device.chains.front().devices.empty();
}

std::vector<Device>* deviceListAt(Track& track, const std::vector<int>& path)
{
    return const_cast<std::vector<Device>*>(deviceListAt(static_cast<const Track&>(track), path));
}

const std::vector<Device>* deviceListAt(const Track& track, const std::vector<int>& path)
{
    if (path.size() % 2 != 0) return nullptr;
    const std::vector<Device>* list = &track.devices;
    for (size_t i = 0; i < path.size(); i += 2)
    {
        if (path[i] < 0 || (size_t) path[i] >= list->size()) return nullptr;
        const auto& rack = (*list)[(size_t) path[i]];
        if (path[i + 1] < 0 || (size_t) path[i + 1] >= rack.chains.size()) return nullptr;
        list = &rack.chains[(size_t) path[i + 1]].devices;
    }
    return list;
}

std::vector<Device>* chainListAt(Device& rack, const std::vector<int>& path)
{
    if (path.empty() || path.size() % 2 == 0 || path[0] < 0 || (size_t) path[0] >= rack.chains.size()) return nullptr;
    auto* list = &rack.chains[(size_t) path[0]].devices;
    for (size_t i = 1; i + 1 < path.size(); i += 2)
    {
        if (path[i] < 0 || (size_t) path[i] >= list->size()) return nullptr;
        auto& inner = (*list)[(size_t) path[i]];
        if (path[i + 1] < 0 || (size_t) path[i + 1] >= inner.chains.size()) return nullptr;
        list = &inner.chains[(size_t) path[i + 1]].devices;
    }
    return list;
}

bool groupDevicesInList(std::vector<Device>& list, int first, int count, Device rack)
{
    if (first < 0 || count <= 0 || (size_t) first + (size_t) count > list.size() || rack.kind != DeviceKind::Rack) return false;
    if (rack.chains.empty()) rack.chains.emplace_back();
    auto& chain = rack.chains.front();
    const auto begin = list.begin() + first, end = begin + count;
    if (chain.name.empty()) chain.name = begin->displayName;
    chain.devices.insert(chain.devices.end(), std::make_move_iterator(begin), std::make_move_iterator(end));
    rack.chainDevicesVisible = true; // the reference shows the grouped devices in the rack
    *begin = std::move(rack);
    list.erase(begin + 1, end);
    return true;
}

int ungroupDeviceInList(std::vector<Device>& list, int index, int& nextDeviceId)
{
    if (index < 0 || (size_t) index >= list.size() || ! canUngroupDevice(list[(size_t) index])) return -1;
    auto devices = std::move(list[(size_t) index].chains.front().devices);
    std::set<int> taken;
    for (size_t i = 0; i < list.size(); ++i)
        if ((int) i != index) taken.insert(list[i].id);
    for (auto& device : devices)
    {
        for (auto& p : device.parameters) // the rack and its macros are gone
            if (p.macroIndex >= 0) { p.macroIndex = -1; p.rangeMin = p.rangeMax = 0.0; p.macroMappingEdited = true; }
        if (device.id < 0 || taken.count(device.id) > 0) device.id = nextDeviceId++;
        taken.insert(device.id);
    }
    const int count = (int) devices.size();
    list.erase(list.begin() + index);
    list.insert(list.begin() + index, std::make_move_iterator(devices.begin()), std::make_move_iterator(devices.end()));
    return count;
}

EditCommand groupDevices(const Song& song, int track, const std::vector<int>& deviceIds, Device rack)
{
    const auto* target = trackAt(song, track);
    int first = 0, count = 0;
    if (! target || rack.id < 0 || ! groupableDevices(target->devices, deviceIds, first, count)
        || groupRackType(target->devices, first, count) != rack.type)
        return {};
    std::vector<int> members; // in chain order
    for (int i = first; i < first + count; ++i) members.push_back(target->devices[(size_t) i].id);
    const int rackId = rack.id;
    // The rack without its devices: kept across undo/redo so a redone Group has the macros and chain it had.
    const auto shell = std::make_shared<Device>(std::move(rack));
    EditCommand command;
    command.label = count == 1 ? "Group " + target->devices[(size_t) first].displayName : "Group Devices";
    command.tracks = {track};
    command.devicesChanged = true;
    command.apply = [track, members, shell](Song& s) {
        auto* t = trackAt(s, track);
        if (! t) return;
        const int at = indexOfId(t->devices, members.front());
        if (at < 0 || (size_t) at + members.size() > t->devices.size()) return;
        for (size_t i = 0; i < members.size(); ++i)
            if (t->devices[(size_t) at + i].id != members[i]) return;
        groupDevicesInList(t->devices, at, (int) members.size(), *shell);
    };
    command.revert = [track, rackId, shell](Song& s) {
        auto* t = trackAt(s, track);
        if (! t) return;
        const int at = indexOfId(t->devices, rackId);
        if (at < 0 || t->devices[(size_t) at].chains.empty()) return;
        auto& grouped = t->devices[(size_t) at];
        auto devices = std::move(grouped.chains.front().devices);
        grouped.chains.front().devices.clear();
        *shell = std::move(grouped);
        t->devices.erase(t->devices.begin() + at);
        t->devices.insert(t->devices.begin() + at, std::make_move_iterator(devices.begin()), std::make_move_iterator(devices.end()));
    };
    return command;
}

EditCommand ungroupDevice(const Song& song, int track, int rackId)
{
    const auto* target = trackAt(song, track);
    if (! target) return {};
    const int index = indexOfId(target->devices, rackId);
    if (index < 0 || ! canUngroupDevice(target->devices[(size_t) index])) return {};
    // The ids the devices get, worked out now so redo gives the same ones.
    std::vector<int> originalIds, newIds;
    for (const auto& d : target->devices[(size_t) index].chains.front().devices) originalIds.push_back(d.id);
    int deviceEnd = song.nextDeviceId;
    {
        auto copy = target->devices;
        const int count = ungroupDeviceInList(copy, index, deviceEnd);
        for (int i = 0; i < count; ++i) newIds.push_back(copy[(size_t) (index + i)].id);
    }
    struct State
    {
        Device shell; // the rack without its devices
        std::vector<std::vector<MacroMapping>> mappings;
    };
    const auto state = std::make_shared<State>();
    EditCommand command;
    command.label = "Ungroup " + target->devices[(size_t) index].displayName;
    command.tracks = {track};
    command.devicesChanged = true;
    command.apply = [track, rackId, originalIds, newIds, deviceEnd, state](Song& s) {
        auto* t = trackAt(s, track);
        if (! t) return;
        const int at = indexOfId(t->devices, rackId);
        if (at < 0 || ! canUngroupDevice(t->devices[(size_t) at])) return;
        auto& rack = t->devices[(size_t) at];
        auto& devices = rack.chains.front().devices;
        if (devices.size() != originalIds.size()) return;
        state->mappings.clear();
        for (const auto& d : devices) state->mappings.push_back(macroMappings(d));
        auto moved = std::move(devices);
        devices.clear();
        state->shell = std::move(rack);
        for (size_t i = 0; i < moved.size(); ++i)
        {
            for (auto& p : moved[i].parameters)
                if (p.macroIndex >= 0) { p.macroIndex = -1; p.rangeMin = p.rangeMax = 0.0; p.macroMappingEdited = true; }
            moved[i].id = newIds[i];
        }
        t->devices.erase(t->devices.begin() + at);
        t->devices.insert(t->devices.begin() + at, std::make_move_iterator(moved.begin()), std::make_move_iterator(moved.end()));
        s.nextDeviceId = std::max(s.nextDeviceId, deviceEnd);
    };
    command.revert = [track, originalIds, newIds, state](Song& s) {
        auto* t = trackAt(s, track);
        if (! t) return;
        const int at = newIds.empty() ? -1 : indexOfId(t->devices, newIds.front());
        if (at < 0 || (size_t) at + newIds.size() > t->devices.size()) return;
        for (size_t i = 0; i < newIds.size(); ++i)
            if (t->devices[(size_t) at + i].id != newIds[i]) return;
        auto rack = state->shell;
        if (rack.chains.empty()) rack.chains.emplace_back();
        auto& chain = rack.chains.front().devices;
        chain.clear();
        for (size_t i = 0; i < newIds.size(); ++i)
        {
            auto device = std::move(t->devices[(size_t) at + i]);
            device.id = originalIds[i];
            if (i < state->mappings.size()) restoreMacroMappings(device, state->mappings[i]);
            chain.push_back(std::move(device));
        }
        t->devices.erase(t->devices.begin() + at, t->devices.begin() + at + (std::ptrdiff_t) newIds.size());
        t->devices.insert(t->devices.begin() + at, std::move(rack));
    };
    return command;
}

} // namespace sonora::model
