#include "control/SerumLead.h"
#include "plugins/Vst3PresetBuilder.h"
#include "plugins/SerumState.h"
#include "plugins/PluginDevice.h"

namespace sonora::control
{
juce::var describeHostedParameters(devices::DeviceProcessor* device)
{
    juce::Array<juce::var> parameters;
    if (auto* plugin = dynamic_cast<plugins::PluginDevice*>(device))
        for (auto* parameter : plugin->instance().getParameters())
        {
            const auto* hosted = dynamic_cast<juce::HostedAudioProcessorParameter*>(parameter);
            parameters.add(object({{"parameterId", hosted ? hosted->getParameterID() : juce::String()},
                {"name", parameter->getName(256)}, {"unit", parameter->getLabel()},
                {"normalized", parameter->getValue()}, {"display", parameter->getText(parameter->getValue(), 256)},
                {"automatable", parameter->isAutomatable()}, {"steps", parameter->getNumSteps()},
                {"writable", plugin->instance().getVST3Client() != nullptr && parameter->isAutomatable()
                    && (static_cast<int>(parameter->getCategory()) >> 16) != 2}}));
        }
    return juce::var(parameters);
}
ApplyResult prepareSerumLead(plugins::PluginHost& host, double sampleRate, int blockSize, model::Device& output, juce::var& report)
{
    const auto types = host.knownPlugins().getTypes();
    const auto found = std::find_if(types.begin(), types.end(), [](const auto& plugin) {
        return plugin.isInstrument && plugin.pluginFormatName == "VST3" && plugin.name.trim() == "Serum 2";
    });
    if (found == types.end()) return ApplyResult::failure("missing_plugin", "Scan the installed Serum 2 instrument VST3 first");
    if (found->version != "2.0.24") return ApplyResult::failure("unsupported_plugin_version", "Supersaw profile is qualified for Serum 2.0.24 only");
    juce::String error;
    auto plugin = host.formats().createPluginInstance(*found, sampleRate, blockSize, error);
    if (! plugin) return ApplyResult::failure("plugin_load_failed", error.toStdString());
    plugin->setRateAndBufferSizeDetails(sampleRate, blockSize);
    plugin->prepareToPlay(sampleRate, blockSize);
    struct Release { juce::AudioPluginInstance& plugin; ~Release() { plugin.releaseResources(); } } release{*plugin};
    struct Setting { const char* id; const char* name; float value; };
    const Setting settings[] = {
        {"0", "Main Vol", .35f}, {"10", "Mono Toggle", 0}, {"11", "Legato", 0},
        {"1000000", "A Enable", 1}, {"1000024", "A Unison", .4f},
        {"1000026", "A Uni Detune", std::sqrt(.12f)}, {"1000027", "A Uni Blend", .75f},
        {"1000028", "A Uni Width", 1}, {"1000039", "A WT Pos", 0},
        {"1001000", "B Enable", 0}, {"1002000", "C Enable", 0},
        {"3000000", "Env 1 Attack", static_cast<float>(std::pow(.008 / 32.0, .2))},
        {"3000003", "Env 1 Sustain", 1},
        {"3000004", "Env 1 Release", static_cast<float>(std::pow(.25 / 32.0, .2))}
    };
    juce::Array<juce::var> values;
    for (const auto& setting : settings)
    {
        juce::AudioProcessorParameter* target = nullptr;
        for (auto* parameter : plugin->getParameters())
            if (auto* hosted = dynamic_cast<juce::HostedAudioProcessorParameter*>(parameter);
                hosted && hosted->getParameterID() == setting.id && parameter->getName(256) == setting.name) target = parameter;
        if (! target) return ApplyResult::failure("plugin_profile_mismatch", std::string("Missing or changed Serum parameter: ") + setting.name);
        target->beginChangeGesture(); target->setValueNotifyingHost(setting.value); target->endChangeGesture();
        if (std::abs(target->getValue() - setting.value) > 1e-4f)
            return ApplyResult::failure("plugin_write_failed", std::string("Readback failed: ") + setting.name);
        values.add(object({{"parameterId", setting.id}, {"name", setting.name}, {"normalized", target->getValue()},
                           {"display", target->getText(target->getValue(), 256)}}));
    }
    // Flush queued host writes without touching the live graph or sending any notes.
    juce::AudioBuffer<float> silent(std::max(2, plugin->getTotalNumOutputChannels()), blockSize);
    juce::MidiBuffer midi;
    for (int block = 0; block < 4; ++block) { silent.clear(); plugin->processBlock(silent, midi); }
    const auto* client = plugin->getVST3Client();
    const auto preset = client ? client->getPreset() : juce::MemoryBlock{};
    model::PluginInfo info;
    info.name = found->name.toStdString(); info.vendor = found->manufacturerName.toStdString(); info.isInstrument = true;
    const auto* bytes = static_cast<const uint8_t*>(preset.getData());
    if (preset.isEmpty() || ! plugins::readVstPreset({bytes, bytes + preset.getSize()}, info))
        return ApplyResult::failure("plugin_state_failed", "Serum did not provide a restorable VST3 state");
    const auto decoded = plugins::decodeSerumState(info.state);
    if (! decoded) return ApplyResult::failure("plugin_state_failed", decoded.error);
    const auto savedUnison = decoded.state->plainParameter("Oscillator0", "kParamUnison");
    if (! savedUnison || std::abs(*savedUnison - 7) > .01)
        return ApplyResult::failure("plugin_state_failed", "Serum component state did not retain seven unison voices");
    const auto polyphony = decoded.state->plainParameter("Global0", "kParamPolyCount").value_or(8);
    if (! std::isfinite(polyphony) || polyphony < 6)
        return ApplyResult::failure("plugin_profile_mismatch", "Serum initialization needs at least six polyphonic voices for overlapping chords");
    const auto* oscillator = decoded.state->root().find("Oscillator0");
    const auto* table = oscillator ? oscillator->find("WTOsc0") : nullptr;
    if (! table) return ApplyResult::failure("plugin_profile_mismatch", "Expected initialized Serum wavetable oscillator");
    for (const auto* key : {"pathToWT", "relativePathToWT"})
        if (const auto* path = table->find(key); path && path->text() && ! path->text()->empty()
            && ! juce::String(*path->text()).endsWithIgnoreCase("Default Shapes.wav"))
            return ApplyResult::failure("plugin_profile_mismatch", "Serum initialization uses a custom wavetable; expected Default Shapes");
    if (table->find("embeddedWTData")) return ApplyResult::failure("plugin_profile_mismatch", "Custom embedded initialization is unsupported");
    output.kind = model::DeviceKind::PluginVst3; output.type = "PluginDevice"; output.displayName = "Serum 2";
    output.plugin = std::move(info);
    report = object({{"plugin", found->name}, {"pluginVersion", found->version}, {"profile", "serum-2.0.24-supersaw-v1"},
                     {"parameters", values}, {"assumptions", "D# natural minor, 4/4, existing tempo; saw / 7 voices / detune 0.12 / 250 ms release"}});
    return ApplyResult::success();
}
} // namespace sonora::control
