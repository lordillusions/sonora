#pragma once
// Turns model::Device (what the .als says) into a runnable DeviceProcessor.
// To add a native device: implement it in src/devices/native/, register it in
// DeviceFactory.cpp::createNative(), and flip `implemented` in core/DeviceCatalog.cpp.

#include "core/Model.h"
#include "devices/DeviceProcessor.h"

#include <memory>
#include <string>
#include <vector>

namespace sonora::plugins
{
class PluginHost;
}
namespace sonora::engine
{
class SampleCache;
}

namespace sonora::devices
{
// Message thread: a fresh native device/rack with its editable parameter defaults. A "max:<patch>" key (DEV-23)
// gives the MxDevice model of a Max for Live patch Sonora replaces natively.
model::Device nativeDeviceModel(const std::string& type);
// Message thread, with the audio callback excluded by the caller. Recurses through rack identities.
void captureDeviceState(model::Device& device, DeviceProcessor* processor);

struct BuildContext
{
    plugins::PluginHost* pluginHost = nullptr; // may be null (tests): plugins are skipped
    double sampleRate = 44100.0;
    int blockSize = 512;
    bool placeholderForMissingInstruments = true;
    engine::SampleCache* samples = nullptr; // sample-based devices (Simpler) request their files here
    const model::Song* song = nullptr;      // to resolve sample paths relative to the project
    std::vector<std::string>* log = nullptr; // human-readable notes, shown in the status bar
};

// Returns nullptr when the device cannot run; the chain then just skips it (pass-through).
std::unique_ptr<DeviceProcessor> createDevice(const model::Device& device, BuildContext& ctx);

// Native-only part (no plugin host needed). Exposed for tests.
std::unique_ptr<DeviceProcessor> createNative(const model::Device& device, std::vector<std::string>* warnings = nullptr);

} // namespace sonora::devices
