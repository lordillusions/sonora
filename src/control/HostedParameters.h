#pragma once
#include "control/ControlService.h"
#include "plugins/PluginHost.h"

namespace sonora::control
{
// AGT-7b: isolated VST3 preparation. Caller captures live state under the callback lock first.
ApplyResult prepareHostedParameterEdit(plugins::PluginHost&, const juce::PluginDescription&, model::Device& before,
    const std::vector<Backend::ParameterWrite>&, double sampleRate, int blockSize, model::Device& after, juce::var& report);
}
