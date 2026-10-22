#pragma once
// Per-user data folder (COM-4c, COM-7): macOS ~/Library/Application Support/<name>, Linux ~/.config/<name>,
// Windows %APPDATA%\<name>. Settings, plug-in cache, mappings, agent settings,
// untitled projects and the automation registry all live under it.
//
// "Sonora" by default. The app sets "Sonora Nightly" at startup in nightly builds (SONORA_APP_DATA_FOLDER_STRING from
// src/CMakeLists.txt), so a nightly never reads or writes the stable app's data. Tools and tests keep the default and so
// share the stable app's cache unless they are told otherwise (plugin_probe --data-folder).
#include <juce_core/juce_core.h>

#include <string>

namespace sonora::core
{
inline constexpr const char* kStableDataFolder = "Sonora";

// A single path component: not empty, not "."/"..", no '/' or ':', no leading '.', at most 64 bytes.
bool isValidDataFolderName(const std::string& name);

// Call once at startup, before anything touches the data folder. An invalid name is ignored (returns false).
bool setAppDataFolderName(const std::string& name);
std::string appDataFolderName();

// The per-OS base above plus appDataFolderName(); not created.
juce::File appDataDirectory();
} // namespace sonora::core
