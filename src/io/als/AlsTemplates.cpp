#include "io/als/AlsTemplates.h"
#include <juce_core/juce_core.h>

namespace sonora::io
{
namespace
{
using juce::XmlElement;
XmlElement* value(XmlElement& parent, const char* tag, const juce::String& text)
{
    auto* result = parent.getChildByName(tag);
    if (! result) result = parent.createNewChildElement(tag);
    result->setAttribute("Value", text);
    return result;
}
XmlElement* path(XmlElement& parent, const std::string& key)
{
    auto* result = &parent;
    const auto tokens = juce::StringArray::fromTokens(juce::String(key), "/", "");
    for (const auto& token : tokens)
    {
        auto* next = result->getChildByName(token);
        result = next ? next : result->createNewChildElement(token);
    }
    return result;
}
void parameter(XmlElement& parent, const std::string& key, double number, int target, bool boolean = false)
{
    auto* result = path(parent, key);
    value(*result, "Manual", boolean ? juce::String(number >= 0.5 ? "true" : "false") : juce::String::formatted("%.17g", number));
    if (target >= 0) result->createNewChildElement("AutomationTarget")->setAttribute("Id", target);
}
juce::String hex(const std::vector<uint8_t>& bytes)
{
    return juce::String::toHexString(bytes.data(), (int) bytes.size(), 0).toUpperCase();
}
}

std::string audioClipTemplate(const model::Clip& clip)
{
    XmlElement root("AudioClip");
    root.setAttribute("Id", 0);
    root.setAttribute("Time", clip.currentStart);
    value(root, "LomId", "0");
    value(root, "LomIdView", "0");
    value(root, "CurrentStart", juce::String(clip.currentStart, 17));
    value(root, "CurrentEnd", juce::String(clip.currentEnd, 17));
    value(root, "Name", juce::String(clip.name));
    value(root, "Color", juce::String(clip.colorIndex));
    value(root, "Disabled", "false");
    auto* loop = root.createNewChildElement("Loop");
    value(*loop, "LoopStart", "0");
    value(*loop, "LoopEnd", juce::String(clip.loop.loopEnd, 17));
    value(*loop, "StartRelative", "0");
    value(*loop, "LoopOn", "false");
    value(*loop, "OutMarker", juce::String(clip.loop.loopEnd, 17));
    value(*loop, "HiddenLoopStart", "0");
    value(*loop, "HiddenLoopEnd", juce::String(clip.loop.loopEnd, 17));
    value(root, "IsWarped", clip.warped ? "true" : "false");
    value(root, "WarpMode", juce::String((int) clip.warpMode));
    value(root, "PitchCoarse", "0");
    value(root, "PitchFine", "0");
    value(root, "SampleVolume", "1");
    value(root, "HiQ", clip.hiQ ? "true" : "false");
    value(root, "Fade", clip.clipFade ? "true" : "false");
    value(root, "Ram", clip.ram ? "true" : "false");
    auto* sample = root.createNewChildElement("SampleRef");
    auto* ref = sample->createNewChildElement("FileRef");
    value(*ref, "RelativePathType", juce::String(clip.sample.relativePathType));
    value(*ref, "RelativePath", juce::String(clip.sample.relativePath));
    value(*ref, "Path", juce::String(clip.sample.absolutePath));
    value(*ref, "Type", "1");
    value(*ref, "LivePackName", "");
    value(*ref, "LivePackId", "");
    value(*ref, "OriginalFileSize", "0");
    value(*ref, "OriginalCrc", "0");
    value(*sample, "DefaultDuration", juce::String(clip.lengthSamples));
    value(*sample, "DefaultSampleRate", juce::String((int) clip.sampleRate));
    value(*sample, "LastModDate", "0");
    sample->createNewChildElement("SourceContext");
    auto* markers = root.createNewChildElement("WarpMarkers");
    const double seconds = clip.sampleRate > 0 ? (double) clip.lengthSamples / clip.sampleRate : clip.loop.loopEnd;
    for (int i = 0; i < 2; ++i)
    {
        auto* marker = markers->createNewChildElement("WarpMarker");
        marker->setAttribute("Id", i);
        marker->setAttribute("SecTime", i == 0 ? 0 : seconds);
        marker->setAttribute("BeatTime", i == 0 ? 0 : clip.currentEnd - clip.currentStart);
    }
    root.createNewChildElement("Onsets");
    root.createNewChildElement("Envelopes")->createNewChildElement("Envelopes");
    return root.toString().toStdString();
}

std::string deviceTemplate(const model::Device& device)
{
    XmlElement root(juce::String(device.type));
    root.setAttribute("Id", device.id);
    value(root, "LomId", "0");
    value(root, "LomIdView", "0");
    value(root, "IsExpanded", "true");
    parameter(root, "On", device.enabled ? 1 : 0, device.enabledTargetId, true);
    value(root, "UserName", device.kind == model::DeviceKind::Rack || device.type == "SonoraWavetable" || device.nativeContent
                              ? juce::String(device.displayName) : juce::String());
    value(root, "Annotation", "");
    root.createNewChildElement("SourceContext");
    value(root, "IsFolded", "false");
    value(root, "ShouldShowPresetName", "false");
    value(root, "OverwriteProtectionNumber", "2816");
    if (device.kind == model::DeviceKind::MaxForLive && ! device.maxPatch.empty())
    {
        // DEV-23: the patch reference Live needs to load the real device; relative to the project, as Live's Collect
        // All and Save places imported patches, plus the absolute path of a copy found on disk.
        auto* patch = root.createNewChildElement("PatchSlot")->createNewChildElement("Value")->createNewChildElement("MxPatchRef");
        patch->setAttribute("Id", 1);
        auto* ref = patch->createNewChildElement("FileRef");
        value(*ref, "RelativePathType", "3");
        value(*ref, "RelativePath", "Presets/Audio Effects/Max Audio Effect/Imported/" + juce::String(device.maxPatch) + ".amxd");
        value(*ref, "Path", juce::String(device.maxPatchPath));
        value(*ref, "Type", "2");
        value(*ref, "LivePackName", "");
        value(*ref, "LivePackId", "");
        value(*ref, "OriginalFileSize", "0");
        value(*ref, "OriginalCrc", "0");
        value(*patch, "LastModDate", "0");
        patch->createNewChildElement("SourceContext");
        value(*patch, "SampleUsageHint", "0");
        root.createNewChildElement("ParameterList")->createNewChildElement("ParameterList");
        root.createNewChildElement("FileDropList")->createNewChildElement("FileDropList");
        root.createNewChildElement("IdRefList")->createNewChildElement("IdRefList");
        root.createNewChildElement("BlobSlot")->createNewChildElement("Value");
        auto* routables = root.createNewChildElement("Routables");
        for (const char* tag : {"InRoutings", "OutRoutings", "MidiInRoutings", "MidiOutRoutings"}) routables->createNewChildElement(tag);
        value(root, "MpeEnabled", "false");
        value(root, "MpeTuningEnabled", "false");
    }
    if (device.kind == model::DeviceKind::Native || device.kind == model::DeviceKind::Rack)
    {
        if (device.nativeContent)
        {
            const auto& content = *device.nativeContent;
            auto* state = root.createNewChildElement("SonoraState");
            state->setAttribute("Format", juce::String(content.format));
            state->setAttribute("SourceDirectory", juce::String(content.sourceDirectory));
            state->createNewChildElement("ProcessorState")->addTextElement(hex(content.data));
            state->createNewChildElement("ControllerState")->addTextElement(hex(content.controllerData));
        }
        for (const auto& param : device.parameters)
            if (param.path != "On")
            {
                parameter(root, param.path, param.value, param.automationTargetId, param.booleanValue);
                if (param.macroIndex >= 0)
                {
                    auto* target = path(root, param.path);
                    auto* key = target->createNewChildElement("KeyMidi");
                    value(*key, "Channel", "16"); value(*key, "IsNote", "false");
                    value(*key, "NoteOrController", juce::String(param.macroIndex));
                    auto* range = target->createNewChildElement("MidiControllerRange");
                    value(*range, "Min", juce::String(param.rangeMin, 17)); value(*range, "Max", juce::String(param.rangeMax, 17));
                }
            }
        for (const auto& [key, number] : device.plain)
        {
            const auto slash = key.find_last_of('/');
            auto* parent = slash == std::string::npos ? &root : path(root, key.substr(0, slash));
            const auto leaf = key.substr(slash == std::string::npos ? 0 : slash + 1);
            const bool boolean = leaf == "RetriggerMode" || leaf == "DisplayPitchInHz" || leaf == "StrobeMode" || leaf == "UseConstPowCrossfade"
                                 || leaf == "MidSideBalanceOn" || leaf == "BassMonoAudition";
            value(*parent, leaf.c_str(), boolean ? juce::String(number >= 0.5 ? "true" : "false") : juce::String(number, 17));
        }
        if (! device.sampleParts.empty() || device.samplePartsEdited)
        {
            auto* parts = path(root, "Player/MultiSampleMap/SampleParts");
            int index = 0;
            for (const auto& part : device.sampleParts)
            {
                auto* p = parts->createNewChildElement("MultiSamplePart");
                if (part.sourceXml)
                    if (auto source = juce::XmlDocument::parse(juce::String(*part.sourceXml)))
                    {
                        parts->removeChildElement(p, true); p = source.release(); parts->addChildElement(p);
                    }
                p->setAttribute("Id", index++);
                auto leaf = [&](const std::string& key, double n) {
                    const auto slash = key.find_last_of('/');
                    value(slash == std::string::npos ? *p : *path(*p, key.substr(0, slash)),
                          key.substr(slash == std::string::npos ? 0 : slash + 1).c_str(), juce::String(n, 17));
                };
                value(*p, "Name", juce::String(part.name));
                leaf("KeyRange/Min", part.keyMin); leaf("KeyRange/Max", part.keyMax);
                leaf("VelocityRange/Min", part.velMin); leaf("VelocityRange/Max", part.velMax);
                leaf("RootKey", part.rootKey); leaf("Detune", part.detuneCents); leaf("Panorama", part.pan); leaf("Volume", part.volume);
                leaf("SampleStart", (double) part.sampleStart); leaf("SampleEnd", (double) part.sampleEnd);
                leaf("SustainLoop/Start", (double) part.loopStart); leaf("SustainLoop/End", (double) part.loopEnd);
                leaf("SustainLoop/Mode", part.loopMode); leaf("SustainLoop/Crossfade", (double) part.loopCrossfade);
                auto* ref = path(*p, "SampleRef/FileRef");
                value(*ref, "Path", juce::String(part.sample.absolutePath)); value(*ref, "RelativePath", juce::String(part.sample.relativePath));
                value(*ref, "RelativePathType", juce::String(part.sample.relativePathType));
                leaf("SampleRef/DefaultSampleRate", part.sampleRate); leaf("SampleRef/DefaultDuration", (double) part.lengthSamples);
            }
        }
        if (device.kind == model::DeviceKind::Rack)
        {
            value(root, "NumVisibleMacroControls", juce::String(device.numVisibleMacros));
            for (size_t i = 0; i < device.macroNames.size(); ++i)
                value(root, ("MacroDisplayNames." + std::to_string(i)).c_str(), juce::String(device.macroNames[i]));
            auto* branches = root.createNewChildElement("Branches");
            int index = 0;
            for (const auto& chain : device.chains)
            {
                const bool drum = device.type == "DrumGroupDevice";
                auto* branch = branches->createNewChildElement(drum ? "DrumBranch" : "InstrumentBranch");
                if (chain.sourceXml)
                    if (auto source = juce::XmlDocument::parse(juce::String(*chain.sourceXml)))
                    {
                        branches->removeChildElement(branch, true); branch = source.release(); branches->addChildElement(branch);
                    }
                branch->setAttribute("Id", index++);
                value(*path(*branch, "Name"), "EffectiveName", juce::String(chain.name));
                parameter(*branch, "MixerDevice/Volume", chain.volume, -1);
                parameter(*branch, "MixerDevice/Panorama", chain.pan, -1);
                parameter(*branch, "MixerDevice/Speaker", chain.speakerOn ? 1 : 0, -1, true);
                auto range = [&](const char* key, int min, int max) {
                    auto* r = path(*branch, key); value(*r, "Min", juce::String(min)); value(*r, "Max", juce::String(max));
                };
                range("ZoneSettings/KeyRange", chain.keyMin, chain.keyMax);
                range("ZoneSettings/VelocityRange", chain.velMin, chain.velMax);
                range("BranchSelectorRange", chain.selMin, chain.selMax);
                if (drum)
                {
                    auto* info = path(*branch, "BranchInfo");
                    value(*info, "ReceivingNote", juce::String(chain.receivingNote >= 0 ? 128 - chain.receivingNote : 0));
                    value(*info, "SendingNote", juce::String(chain.sendingNote)); value(*info, "ChokeGroup", juce::String(chain.chokeGroup));
                }
                auto findDevices = [](auto&& self, XmlElement* parent, int depth) -> XmlElement* {
                    if (depth > 4) return nullptr;
                    if (parent->hasTagName("Devices")) return parent;
                    for (auto* c : parent->getChildIterator()) if (auto* found = self(self, c, depth + 1)) return found;
                    return nullptr;
                };
                auto* children = findDevices(findDevices, branch, 0);
                if (! children) children = path(*branch, "DeviceChain/Devices");
                children->deleteAllChildElements();
                for (const auto& child : chain.devices)
                {
                    auto xml = child.sourceXml ? juce::XmlDocument::parse(juce::String(*child.sourceXml)) : nullptr;
                    if (! xml) xml = juce::XmlDocument::parse(juce::String(deviceTemplate(child)));
                    if (xml) { xml->setAttribute("Id", child.id); children->addChildElement(xml.release()); }
                }
            }
        }
    }
    else if (device.kind == model::DeviceKind::PluginVst3 && device.plugin)
    {
        const auto& plugin = *device.plugin;
        auto* info = root.createNewChildElement("PluginDesc")->createNewChildElement("Vst3PluginInfo");
        info->setAttribute("Id", 0); // Slot<PluginInfo> members require list identities
        value(*info, "Name", juce::String(plugin.name));
        value(*info, "DeviceType", plugin.isInstrument ? "1" : "2");
        auto* uid = info->createNewChildElement("Uid");
        for (int i = 0; i < 4; ++i)
            value(*uid, ("Fields." + std::to_string(i)).c_str(), juce::String((int32_t) plugin.vst3Uid[(size_t) i]));
        auto* preset = info->createNewChildElement("Preset")->createNewChildElement("Vst3Preset");
        preset->setAttribute("Id", 0);
        value(*preset, "Name", juce::String(plugin.name));
        value(*preset, "IsOn", "true");
        value(*preset, "DeviceType", plugin.isInstrument ? "1" : "2");
        auto* presetUid = preset->createNewChildElement("Uid");
        for (int i = 0; i < 4; ++i)
            value(*presetUid, ("Fields." + std::to_string(i)).c_str(), juce::String((int32_t) plugin.vst3Uid[(size_t) i]));
        preset->createNewChildElement("ProcessorState")->addTextElement(hex(plugin.state));
        preset->createNewChildElement("ControllerState")->addTextElement(hex(plugin.controllerState));
        root.createNewChildElement("ParameterList");
    }
    return root.toString().toStdString();
}
} // namespace sonora::io
