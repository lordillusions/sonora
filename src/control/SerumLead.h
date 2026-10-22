#pragma once
#include "control/ControlService.h"
#include "plugins/PluginHost.h"
#include "devices/DeviceProcessor.h"

namespace sonora::control
{
// AGT-7a: prepares an isolated real VST3 instance, then captures its state before editing the song.
ApplyResult prepareSerumLead(plugins::PluginHost&, double sampleRate, int blockSize, model::Device&, juce::var& report);
juce::var describeHostedParameters(devices::DeviceProcessor*);
}
