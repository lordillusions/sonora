#pragma once

#include <string>
#include <vector>

namespace sonora::ui
{
struct BrowserDrag
{
    // TrackDevice (UI-7c): a device's title dragged out of the device view onto another track. Opt held at the drop
    // copies it (same parameters and state); otherwise it moves.
    // DevicePreset (LIB-2): a reference device preset (.adv); value = its absolute path, label = the preset's name.
    enum class Kind { Sample, NativeDevice, Plugin, TrackDevice, DevicePreset } kind = Kind::Sample;
    std::string value, label; // absolute sample/preset path, native tag, or scanned plugin identifier
    std::vector<std::string> files{}; // Sample drags of several files (Finder): every path, `value` is the first
    bool oneTrackPerFile = false;   // Cmd held at the drop: each further file gets its own new audio track
    int sourceTrack = -1, deviceId = -1; // TrackDevice: where the device is now (UI track index, model device id)
    bool copy = false;                   // TrackDevice: Opt held (follows the latest drag event, then the drop)
};
} // namespace sonora::ui
