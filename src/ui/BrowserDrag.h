#pragma once

#include <string>
#include <vector>

namespace sonora::ui
{
struct BrowserDrag
{
    // TrackDevice (UI-7c): a device's title dragged out of the device view onto another track. Opt held at the drop
    // copies it (same parameters and state); otherwise it moves.
    enum class Kind { Sample, NativeDevice, Plugin, TrackDevice } kind = Kind::Sample;
    std::string value, label; // absolute sample path, native tag, or scanned plugin identifier
    std::vector<std::string> files{}; // Sample drags of several files (Finder): every path, `value` is the first
    bool oneTrackPerFile = false;   // Cmd held at the drop: each further file gets its own new audio track
    int sourceTrack = -1, deviceId = -1; // TrackDevice: where the device is now (UI track index, model device id)
    bool copy = false;                   // TrackDevice: Opt held (follows the latest drag event, then the drop)
};
} // namespace sonora::ui
