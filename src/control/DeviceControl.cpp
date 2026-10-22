#include "control/DeviceControl.h"
#include "control/HostedParameters.h"
#include "control/SerumLead.h"
#include "core/DeviceCatalog.h"
#include "devices/DeviceFactory.h"
#include "devices/NativeDevice.h"
#include "devices/RackProcessor.h"
#include "plugins/PluginDevice.h"
#include "plugins/Vst3PresetBuilder.h"
#include <bit>

namespace sonora::control
{
namespace
{
bool nativeProcessor(devices::DeviceProcessor* p)
{ return dynamic_cast<devices::NativeDevice*>(p) || dynamic_cast<devices::RackProcessor*>(p); }
juce::String role(model::DeviceRole r)
{
    if (r == model::DeviceRole::Instrument) return "instrument";
    if (r == model::DeviceRole::MidiEffect) return "midiEffect";
    return "audioEffect";
}
bool automated(const model::Track& t, int id)
{
    if (id < 0) return false;
    const auto contains = [id](const auto& lanes) { return std::any_of(lanes.begin(), lanes.end(), [id](const auto& e) {
        return e.targetId == id && ! e.points.empty(); }); };
    if (contains(t.automation)) return true;
    for (const auto& c : t.arrangementClips) if (contains(c.envelopes)) return true;
    for (const auto& s : t.slots) if (s.clip && contains(s.clip->envelopes)) return true;
    for (const auto& l : t.takeLanes) for (const auto& c : l.clips) if (contains(c.envelopes)) return true;
    return false;
}
void preserveAutomatedBases(const model::Track& track, const model::Device& original, model::Device& captured)
{
    // Whole-device replacement must not author the current automation output as a new manual base.
    if (automated(track, original.enabledTargetId)) captured.enabled = original.enabled;
    for (auto& p : captured.parameters)
        if (automated(track, p.automationTargetId))
            for (const auto& old : original.parameters) if (old.path == p.path) p.value = old.value;
    for (size_t c = 0; c < captured.chains.size() && c < original.chains.size(); ++c)
        for (size_t d = 0; d < captured.chains[c].devices.size() && d < original.chains[c].devices.size(); ++d)
            preserveAutomatedBases(track, original.chains[c].devices[d], captured.chains[c].devices[d]);
}
ApplyResult captureChecked(model::Device& device, devices::DeviceProcessor* processor, int depth = 0)
{
    if (! processor || depth > 64) return ApplyResult::failure("plugin_state_failed", "Device state is unavailable or exceeds nesting limits");
    device.enabled = ! processor->isBypassed();
    if (auto* plugin = dynamic_cast<plugins::PluginDevice*>(processor))
        return plugin->captureState(device) ? ApplyResult::success() : ApplyResult::failure("plugin_state_failed", "VST3 capture failed");
    if (! nativeProcessor(processor)) return ApplyResult::failure("unsupported_device", "Native processor is unavailable");
    devices::captureDeviceState(device, processor);
    if (const auto* rack = dynamic_cast<devices::RackProcessor*>(processor))
    {
        if (device.chains.size() != rack->chains().size()) return ApplyResult::failure("plugin_state_failed", "Rack structure no longer matches");
        for (size_t c = 0; c < device.chains.size(); ++c)
        {
            auto& children = device.chains[c].devices; const auto& processors = rack->chains()[c].processorForModelDevice;
            if (children.size() != processors.size()) return ApplyResult::failure("plugin_state_failed", "Rack children no longer match");
            for (size_t d = 0; d < children.size(); ++d)
                if (processors[d]) // Unsupported, skipped children retain their original XML/state unchanged.
                    if (const auto result = captureChecked(children[d], processors[d], depth + 1); ! result.ok) return result;
        }
    }
    return ApplyResult::success();
}
ApplyResult prepareNative(const model::Device& before, const std::vector<Backend::ParameterWrite>& writes,
                          devices::BuildContext& context, model::Device& after, juce::var& report)
{
    auto processor = devices::createDevice(before, context);
    if (! nativeProcessor(processor.get())) return ApplyResult::failure("unsupported_device", "Native processor is unavailable");
    processor->prepare(context.sampleRate, context.blockSize);
    const auto& infos = processor->parameters();
    for (const auto& write : writes)
    {
        const auto found = std::find_if(infos.begin(), infos.end(), [&](const auto& p) { return p.id == write.parameterId && p.name == write.name; });
        if (found == infos.end()) return ApplyResult::failure("parameter_mismatch", "Native parameter ID/name pair no longer exists");
        if (found->maxValue <= found->minValue) return ApplyResult::failure("parameter_read_only", "Native parameter has no writable range");
        double value = write.value.value_or(found->minValue + static_cast<double>(write.normalized) * (found->maxValue - found->minValue));
        if (! std::isfinite(value) || value < found->minValue || value > found->maxValue
            || ((found->isToggle || ! found->choices.empty()) && std::abs(value - std::round(value)) > 1e-4))
            return ApplyResult::failure("invalid_parameter_value", "Native value is outside its range or discrete choices");
        if (found->isToggle || ! found->choices.empty()) value = std::round(value);
        const int index = static_cast<int>(found - infos.begin());
        processor->setParameter(index, static_cast<float>(value));
        if (std::abs(processor->getParameter(index) - value) > 1e-4 * std::max(1.0, std::abs(value)))
            return ApplyResult::failure("plugin_write_failed", "Native processor did not retain the requested value");
    }
    after = before; if (const auto captured = captureChecked(after, processor.get()); ! captured.ok) return captured;
    // Verify persistence through the same model/factory route used after save/reload.
    auto restored = devices::createDevice(after, context);
    if (! nativeProcessor(restored.get())) return ApplyResult::failure("plugin_state_failed", "Native state could not be restored");
    restored->prepare(context.sampleRate, context.blockSize);
    const auto all = describeDeviceParameters(after, restored.get()); juce::Array<juce::var> selected;
    for (const auto& write : writes)
        for (const auto& p : *all.getArray()) if (p["parameterId"].toString().toStdString() == write.parameterId)
        {
            const double expected = write.value.value_or(static_cast<double>(p["minimum"]) + write.normalized *
                (static_cast<double>(p["maximum"]) - static_cast<double>(p["minimum"])));
            if (std::abs(static_cast<double>(p["value"]) - expected) > 1e-4 * std::max(1.0, std::abs(expected)))
                return ApplyResult::failure("plugin_state_failed", "Native value did not survive state reload");
            selected.add(p);
        }
    report = object({{"parameters", selected}, {"stateReloadVerified", true}, {"processorRebuilt", true}, {"kind", "native"}});
    return ApplyResult::success();
}
}
juce::var describeDeviceParameters(const model::Device& device, devices::DeviceProcessor* processor)
{
    if (device.kind == model::DeviceKind::PluginVst3)
    {
        auto values = describeHostedParameters(processor);
        if (auto* list = values.getArray()) for (auto& p : *list) p.getDynamicObject()->setProperty("kind", "vst3");
        return values;
    }
    juce::Array<juce::var> out;
    if (! nativeProcessor(processor)) return juce::var(out);
    const auto& infos = processor->parameters();
    for (size_t i = 0; i < infos.size(); ++i)
    {
        const auto& p = infos[i]; const double value = processor->getParameter(static_cast<int>(i));
        juce::Array<juce::var> choices; for (const auto& name : p.choices) choices.add(juce::String(name));
        juce::String display(value, 6);
        if (! p.choices.empty() && value >= 0 && value < static_cast<double>(p.choices.size())) display = p.choices[static_cast<size_t>(value)];
        else if (p.isToggle) display = value >= .5 ? "On" : "Off";
        else if (! p.unit.empty()) display += " " + juce::String(p.unit);
        out.add(object({{"parameterId", p.id}, {"name", p.name}, {"kind", "native"}, {"unit", p.unit}, {"value", value},
            {"minimum", p.minValue}, {"maximum", p.maxValue}, {"defaultValue", p.defaultValue}, {"choices", choices},
            {"normalized", p.maxValue > p.minValue ? (value - p.minValue) / (p.maxValue - p.minValue) : 0},
            {"display", display}, {"normalization", "linear"}, {"valueDomain", "processorPlain"}, {"automatable", ! p.plainValue},
            {"shownInCurrentMode", processor->parameterShown(static_cast<int>(i))},
            {"steps", p.isToggle ? 2 : static_cast<int>(p.choices.size())}, {"writable", p.maxValue > p.minValue}}));
    }
    return juce::var(out);
}
bool sameDeviceState(const model::Device& a, const model::Device& b)
{
    if (a.kind != b.kind || a.type != b.type || a.enabled != b.enabled || a.plain != b.plain || a.parameters.size() != b.parameters.size()
        || a.chains.size() != b.chains.size()) return false;
    if (a.plugin || b.plugin) return a.plugin && b.plugin && a.plugin->state == b.plugin->state && a.plugin->controllerState == b.plugin->controllerState;
    for (size_t i = 0; i < a.parameters.size(); ++i)
        if (a.parameters[i].path != b.parameters[i].path
            || std::bit_cast<uint64_t>(a.parameters[i].value) != std::bit_cast<uint64_t>(b.parameters[i].value)) return false;
    if (a.nativeContent || b.nativeContent)
        if (! a.nativeContent || ! b.nativeContent || a.nativeContent->format != b.nativeContent->format
            || a.nativeContent->data != b.nativeContent->data || a.nativeContent->controllerData != b.nativeContent->controllerData) return false;
    for (size_t c = 0; c < a.chains.size(); ++c)
    {
        if (a.chains[c].devices.size() != b.chains[c].devices.size()) return false;
        for (size_t d = 0; d < a.chains[c].devices.size(); ++d) if (! sameDeviceState(a.chains[c].devices[d], b.chains[c].devices[d])) return false;
    }
    return true;
}
void configureDeviceBackend(Backend& backend, plugins::PluginHost& host, engine::AudioEngine& engine, std::function<const model::Song*()> song)
{
    const auto find = [&engine, song](int trackId, int deviceId, model::Device& copy) -> devices::DeviceProcessor* {
        const auto* s = song(); const auto* graph = engine.latestGraph();
        for (size_t t = 0; s && graph && t < s->tracks.size() && t < graph->nodes.size(); ++t)
            if (s->tracks[t].id == trackId)
                for (size_t d = 0; d < s->tracks[t].devices.size() && d < graph->nodes[t].processorForModelDevice.size(); ++d)
                    if (s->tracks[t].devices[d].id == deviceId) { copy = s->tracks[t].devices[d]; return graph->nodes[t].processorForModelDevice[d]; }
        return nullptr;
    };
    backend.parameters = [find](int track, int device) { model::Device model; auto* p = find(track, device, model); return describeDeviceParameters(model, p); };
    backend.captureDevice = [find, song, &engine](int track, int device, model::Device& captured) {
        const juce::ScopedLock lock(engine.deviceManager().getAudioCallbackLock());
        auto* p = find(track, device, captured); if (! p) return ApplyResult::failure("target_replaced", "Actual device instance is unavailable");
        const auto original = captured; if (const auto result = captureChecked(captured, p); ! result.ok) return result;
        if (const auto* s = song()) for (const auto& t : s->tracks) if (t.id == track) preserveAutomatedBases(t, original, captured);
        return ApplyResult::success();
    };
    const auto capture = backend.captureDevice;
    backend.prepareParameters = [capture, &host, &engine, song](int track, int device, const std::vector<Backend::ParameterWrite>& writes,
        model::Device& before, model::Device& after, juce::var& report) {
        if (const auto result = capture(track, device, before); ! result.ok) return result;
        if (before.kind == model::DeviceKind::PluginVst3)
        {
            for (const auto& write : writes) if (write.value) return ApplyResult::failure("invalid_parameter_value", "VST3 writes require normalized values");
            const auto match = host.findMatch(*before.plugin, before.kind);
            if (! match) return ApplyResult::failure("missing_plugin", "VST3 is no longer in the scan cache");
            return prepareHostedParameterEdit(host, *match, before, writes, engine.sampleRate(), engine.blockSize(), after, report);
        }
        engine::SampleCache samples; devices::BuildContext context; context.pluginHost = &host; context.samples = &samples;
        context.song = song(); context.sampleRate = engine.sampleRate(); context.blockSize = engine.blockSize(); context.placeholderForMissingInstruments = false;
        return prepareNative(before, writes, context, after, report);
    };
    backend.deviceCatalog = [&host] {
        juce::Array<juce::var> list;
        for (const auto& entry : model::deviceCatalog()) if (entry.implemented)
            list.add(object({{"catalogId", "native:" + std::string(entry.xmlTag)}, {"name", std::string(entry.displayName)}, {"kind", "native"},
                {"type", std::string(entry.xmlTag)}, {"role", role(entry.role)},
                {"contentRequired", entry.xmlTag == "OriginalSimpler" || entry.xmlTag == "MultiSampler" || entry.xmlTag == "SonoraImpulse" || entry.xmlTag == "SonoraResynth"}}));
        for (const auto& entry : host.knownPlugins().getTypes()) if (entry.pluginFormatName == "VST3")
            list.add(object({{"catalogId", "vst3:" + entry.createIdentifierString()}, {"name", entry.name}, {"kind", "vst3"},
                {"vendor", entry.manufacturerName}, {"version", entry.version}, {"role", entry.isInstrument ? "instrument" : "audioEffect"}}));
        return juce::var(list);
    };
    backend.prepareDevice = [&host, &engine](const std::string& id, model::Device& device, juce::var& report) {
        if (id.starts_with("native:"))
        {
            const auto type = id.substr(7); const auto* entry = model::findCatalogEntry(type);
            if (! entry || ! entry->implemented) return ApplyResult::failure("unsupported_device", "Native catalogue entry is unavailable");
            device = devices::nativeDeviceModel(type);
            report = object({{"kind", "native"}, {"role", role(entry->role)}}); return ApplyResult::success();
        }
        const auto types = host.knownPlugins().getTypes();
        const auto found = std::find_if(types.begin(), types.end(), [&](const auto& d) {
            return d.pluginFormatName == "VST3" && ("vst3:" + d.createIdentifierString()).toStdString() == id;
        });
        if (found == types.end()) return ApplyResult::failure("missing_plugin", "Select an exact VST3 catalogId from devices.catalog");
        juce::String error; auto instance = host.formats().createPluginInstance(*found, engine.sampleRate(), engine.blockSize(), error);
        if (! instance) return ApplyResult::failure("plugin_load_failed", error.toStdString());
        device.kind = model::DeviceKind::PluginVst3; device.type = "PluginDevice"; device.displayName = found->name.toStdString(); device.plugin.emplace();
        device.plugin->name = found->name.toStdString(); device.plugin->vendor = found->manufacturerName.toStdString(); device.plugin->isInstrument = found->isInstrument;
        plugins::PluginDevice initial(std::move(instance)); initial.prepare(engine.sampleRate(), engine.blockSize());
        if (! initial.captureState(device)) return ApplyResult::failure("plugin_state_failed", "VST3 did not provide restorable state");
        model::Device verified; const std::vector<Backend::ParameterWrite> none;
        const auto restored = prepareHostedParameterEdit(host, *found, device, none, engine.sampleRate(), engine.blockSize(), verified, report);
        if (! restored.ok) return restored;
        device = std::move(verified); report.getDynamicObject()->setProperty("role", found->isInstrument ? "instrument" : "audioEffect");
        return ApplyResult::success();
    };
}
}
