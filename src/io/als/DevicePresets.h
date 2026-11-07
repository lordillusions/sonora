#pragma once
// LIB-2: the reference application's device presets (.adv files: gzip XML `<Ableton ...><DeviceTag>...</DeviceTag></Ableton>`,
// the same device element a set's device chain holds, with every pointee id 0). They are read at run time from the installed
// library, the User Library and installed packs (platform/ReferenceLibrary.h) and never copied anywhere (golden rule 5).
// Format notes: docs/ALS_FORMAT.md "Device presets (.adv)".

#include "core/Model.h"

#include <juce_core/juce_core.h>

#include <string>
#include <vector>

namespace sonora::io
{

struct PresetLibrary
{
    std::string path; // the library folder (holds Devices/ and/or Presets/)
    std::string name; // "Core Library", "User Library" or the pack's folder name
};

struct DevicePreset
{
    std::string path;       // absolute .adv
    std::string name;       // file name without extension (what the reference lists)
    std::string folder;     // the first folder below the device folder ("Bass"); empty for a preset directly in it
    std::string deviceTag;  // catalog element tag ("InstrumentVector", "StringStudio")
    std::string deviceName; // catalog display name ("Wavetable", "Tension")
    std::string library;    // PresetLibrary::name
};

// The libraries to scan. SONORA_REFERENCE_LIBRARY (one or more folders separated by ':' — ';' on Windows) replaces every
// default (tests, other installs). Otherwise: each installed Core Library, `userLibrary`, and every pack folder inside
// `packFolders` (plus the platform's default pack folders). Only existing folders, each once.
std::vector<PresetLibrary> presetLibraries(const std::string& userLibrary, const std::vector<std::string>& packFolders);

// Every preset of an implemented, non-rack catalog device in `libraries`:
// <library>/{Devices,Presets}/{Instruments,Audio Effects,MIDI Effects}/<Device>/**.adv ("Ableton Folder Info" skipped).
// A device folder maps to the catalog entry with that display name and role, else to the root tag of its first preset.
// Sorted by device tag, folder, name (case-insensitive). Only directory listings plus one file per unknown folder.
std::vector<DevicePreset> scanDevicePresets(const std::vector<PresetLibrary>& libraries);

// The root device tag of a preset file ("InstrumentVector"), empty when the file is not a device preset.
std::string devicePresetTag(const juce::File& preset);

// Library-relative FileRefs (RelativePathType 5 or 6, e.g. a factory sample "Samples/..." or a LastPresetRef) get `Path`
// pointing at the local file below the first of `libraries` that has it. Returns how many were repointed.
int localizeLibraryRefs(juce::XmlElement& element, const std::vector<juce::File>& libraries);

struct DevicePresetLoad
{
    model::Device device;
    int pointeeEnd = 0; // raise song.nextPointeeId to this when the device is inserted
    std::string error;
    std::vector<std::string> warnings;
};

// Reads `preset` as a new device of `song`: device id song.nextDeviceId; every pointee element (AutomationTarget,
// *ModulationTarget, Pointee, ControllerTargets.N) gets its own fresh id from song.nextPointeeId on (presets store 0 for all
// of them); library-relative references point at the local library (the preset's own library first, then `libraries`);
// an empty UserName becomes the preset's name. Device::sourceXml is that element, so every leaf Sonora does not model
// survives a save. Unsupported devices (not implemented, racks) and non-preset files return an error.
DevicePresetLoad readDevicePreset(const juce::File& preset, const model::Song& song, const std::vector<juce::File>& libraries);

} // namespace sonora::io
