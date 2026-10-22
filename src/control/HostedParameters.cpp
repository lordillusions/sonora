#include "control/HostedParameters.h"
#include "plugins/PluginDevice.h"

namespace sonora::control
{
namespace
{
juce::AudioProcessorParameter* resolve(juce::AudioPluginInstance& instance, const Backend::ParameterWrite& write)
{
    juce::AudioProcessorParameter* found = nullptr;
    for (auto* p : instance.getParameters())
        if (auto* hosted = dynamic_cast<juce::HostedAudioProcessorParameter*>(p);
            hosted && hosted->getParameterID().toStdString() == write.parameterId)
        {
            if (found || p->getName(256).toStdString() != write.name) return nullptr;
            found = p;
        }
    return found;
}
void flush(plugins::PluginDevice& plugin, int blockSize)
{
    juce::AudioBuffer<float> silent(2, blockSize);
    juce::MidiBuffer midi;
    for (int block = 0; block < 4; ++block) { silent.clear(); plugin.process(silent, midi); }
}
}
ApplyResult prepareHostedParameterEdit(plugins::PluginHost& host, const juce::PluginDescription& description,
    model::Device& before, const std::vector<Backend::ParameterWrite>& writes,
    double sampleRate, int blockSize, model::Device& after, juce::var& report)
{
    if (before.kind != model::DeviceKind::PluginVst3 || ! before.plugin || description.pluginFormatName != "VST3")
        return ApplyResult::failure("unsupported_device", "Only an actual hosted VST3 instance is writable");
    juce::String error;
    auto instance = host.instantiate(description, *before.plugin, before.kind, sampleRate, blockSize, error);
    if (! instance || error.isNotEmpty()) return ApplyResult::failure("plugin_load_failed", error.toStdString());
    plugins::PluginDevice prepared(std::move(instance)); prepared.prepare(sampleRate, blockSize);
    // Resolve the entire batch before making any writes, even on this disposable instance.
    std::vector<juce::AudioProcessorParameter*> parameters;
    for (const auto& write : writes)
    {
        auto* p = resolve(prepared.instance(), write);
        if (! p) return ApplyResult::failure("parameter_mismatch", "Missing, ambiguous or renamed parameter: " + write.parameterId);
        if (! p->isAutomatable() || (static_cast<int>(p->getCategory()) >> 16) == 2)
            return ApplyResult::failure("parameter_read_only", "Parameter is not a writable automatable control: " + write.name);
        parameters.push_back(p);
    }
    // Capture the actual old manual values only for requested targets. Other authored bases, including
    // controls currently driven by automation, must not be rewritten as a side effect of this patch edit.
    for (size_t i = 0; i < writes.size(); ++i)
        for (auto& stored : before.pluginParams)
            if (std::to_string(stored.parameterId) == writes[i].parameterId)
            { stored.value = parameters[i]->getValue(); stored.valueEdited = true; }
    for (size_t i = 0; i < writes.size(); ++i)
    {
        auto* p = parameters[i];
        p->beginChangeGesture(); p->setValueNotifyingHost(writes[i].normalized); p->endChangeGesture();
    }
    flush(prepared, blockSize);
    after = before;
    if (! prepared.captureState(after)) return ApplyResult::failure("plugin_state_failed", "VST3 state capture failed");
    // Reopen the captured state: cached controller readback alone cannot prove persistence.
    auto reloaded = host.instantiate(description, *after.plugin, after.kind, sampleRate, blockSize, error);
    if (! reloaded || error.isNotEmpty()) return ApplyResult::failure("plugin_state_failed", error.toStdString());
    plugins::PluginDevice verified(std::move(reloaded)); verified.prepare(sampleRate, blockSize);
    flush(verified, blockSize);
    juce::Array<juce::var> actual;
    for (const auto& write : writes)
    {
        auto* p = resolve(verified.instance(), write);
        if (! p || ! std::isfinite(p->getValue()) || std::abs(p->getValue() - write.normalized) > 1e-4f)
            return ApplyResult::failure("plugin_write_failed", "State reload did not retain requested normalized value: " + write.name);
        for (auto& stored : after.pluginParams)
            if (std::to_string(stored.parameterId) == write.parameterId)
            { stored.value = p->getValue(); stored.valueEdited = true; }
        actual.add(object({{"parameterId", write.parameterId}, {"name", write.name}, {"normalized", p->getValue()},
            {"display", p->getText(p->getValue(), 256)}}));
    }
    report = object({{"parameters", actual}, {"stateReloadVerified", true}, {"processorRebuilt", true}});
    return ApplyResult::success();
}
}
