#pragma once
#include "control/ControlService.h"
#include "engine/AudioEngine.h"

namespace sonora::control
{
// AGT-7c: same production adapters for app, offline acceptance and MCP probes. No audio callback work.
void configureDeviceBackend(Backend&, plugins::PluginHost&, engine::AudioEngine&, std::function<const model::Song*()>);
juce::var describeDeviceParameters(const model::Device&, devices::DeviceProcessor*);
bool sameDeviceState(const model::Device&, const model::Device&);
}
