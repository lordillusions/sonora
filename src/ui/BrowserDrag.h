#pragma once

#include <string>
#include <vector>

namespace sonora::ui
{
struct BrowserDrag
{
    enum class Kind { Sample, NativeDevice, Plugin } kind = Kind::Sample;
    std::string value, label; // absolute sample path, native tag, or scanned plugin identifier
    std::vector<std::string> files{}; // Sample drags of several files (Finder): every path, `value` is the first
    bool oneTrackPerFile = false;   // Cmd held at the drop: each further file gets its own new audio track
};
} // namespace sonora::ui
