#pragma once
// COM-7: per-user folders, usable from every layer that links sonora_core.
//
//   appDataFolder():  macOS ~/Library/Application Support/Sonora   Linux ~/.config/Sonora   Windows %APPDATA%\Sonora
//                     ("Sonora Nightly" in nightly builds; the name lives in core/AppDataFolder, COM-4c)

#include "core/AppDataFolder.h"

namespace sonora::platform
{

inline juce::File appDataFolder() { return core::appDataDirectory(); }

} // namespace sonora::platform
