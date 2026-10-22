#pragma once
// Static knowledge about Live's built-in device element names.
// Source of truth for the names: the schema shipped with Live
// (Contents/App-Resources/Schema/12.0_*.txt) and real .als files. See docs/ALS_FORMAT.md.

#include <string_view>
#include <span>

namespace sonora::model
{

struct Device;
struct Track;

enum class DeviceRole
{
    Instrument,
    AudioEffect,
    MidiEffect,
    Unknown
};

struct CatalogEntry
{
    std::string_view xmlTag;      // element name in .als
    std::string_view displayName; // what Live shows
    DeviceRole role;
    bool implemented; // true when Sonora has a native implementation (src/devices/native)
};

// Shared role resolution uses plugin metadata, never display-name guesses.
DeviceRole deviceRole(const Device& device);
int instrumentIndex(const Track& track);
bool isMidiOnly(const Track& track);

// Returns nullptr when the tag is not in the catalog (or a "max:" replacement key).
const CatalogEntry* findCatalogEntry(std::string_view xmlTag);
std::span<const CatalogEntry> deviceCatalog();
std::string_view deviceCategory(const CatalogEntry& entry);

bool isRackTag(std::string_view xmlTag);
bool isMaxForLiveTag(std::string_view xmlTag);
// Max for Live patches Sonora replaces with a native device, keyed by the .amxd file name without extension
// (Device::maxPatch). Returns the device's display name, or an empty view when the patch is not supported (DEV-23).
std::string_view maxForLiveNativeName(std::string_view patchName);
// The same replacements as browser entries: xmlTag is "max:<patch>" (findCatalogEntry resolves it, and
// devices::nativeDeviceModel builds the MxDevice model). Not part of deviceCatalog().
std::span<const CatalogEntry> maxForLiveReplacements();

} // namespace sonora::model
