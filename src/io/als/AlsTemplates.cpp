#include "io/als/AlsTemplates.h"
#include "core/WavetableFormat.h"
#include "core/DeviceCatalog.h"
#include <juce_core/juce_core.h>

#include <algorithm>
#include <string_view>
#include <tuple>

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
        // "Tag#n": the n-th child with that tag, created with its siblings when missing (InstrumentVector matrix, DEV-28b).
        const int hash = token.indexOfChar('#');
        const auto name = hash < 0 ? token : token.substring(0, hash);
        const int index = hash < 0 ? 0 : token.substring(hash + 1).getIntValue();
        int seen = 0;
        XmlElement* next = nullptr;
        for (auto* c : result->getChildWithTagNameIterator(name))
            if (seen++ == index) { next = c; break; }
        while (next == nullptr)
        {
            auto* made = result->createNewChildElement(name);
            // The reference refuses a slot value without an Id ("Not all list members have Ids"): every element it
            // writes under <Value> (SimplerFilter, SimplerLfo, FilePresetRef...) carries one.
            if (result->hasTagName("Value")) made->setAttribute("Id", 0);
            if (seen++ == index) next = made;
        }
        result = next;
    }
    return result;
}
void parameter(XmlElement& parent, const std::string& key, double number, int target, bool boolean = false)
{
    auto* result = path(parent, key);
    value(*result, "Manual", boolean ? juce::String(number >= 0.5 ? "true" : "false") : juce::String::formatted("%.17g", number));
    if (target >= 0) result->createNewChildElement("AutomationTarget")->setAttribute("Id", target);
}
// "%.17g": exact, and whole numbers come out as "0", "60"... The reference refuses a set whose int nodes hold
// "0.00000000000000000" (juce::String(double, 17)), e.g. Delay DryWetMode or a Sampler part's KeyRange.
juce::String number(double n)
{
    return juce::String::formatted("%.17g", n);
}
juce::String hex(const std::vector<uint8_t>& bytes)
{
    return juce::String::toHexString(bytes.data(), (int) bytes.size(), 0).toUpperCase();
}
}

namespace
{
void slicePoints(XmlElement& parent, const char* tag, const std::vector<double>& seconds, const std::vector<double>* energies)
{
    auto* list = parent.getChildByName(tag);
    if (! list) list = parent.createNewChildElement(tag);
    list->deleteAllChildElements();
    for (size_t i = 0; i < seconds.size(); ++i)
    {
        auto* point = list->createNewChildElement("SlicePoint");
        point->setAttribute("TimeInSeconds", number(seconds[i]));
        point->setAttribute("Rank", 0);
        point->setAttribute("NormalizedEnergy", number(energies && i < energies->size() ? (*energies)[i] : 1.0));
    }
}
// A <MultiSamplePart> with every node the reference writes for a sample it loads itself (ALS-15b): a part with only the
// zone, region and file path opens without an error but loses its sample in the reference.
std::unique_ptr<XmlElement> freshSamplePart(const model::SamplePart& part)
{
    auto p = std::make_unique<XmlElement>("MultiSamplePart");
    p->setAttribute("InitUpdateAreSlicesFromOnsetsEditableAfterRead", "false");
    p->setAttribute("HasImportedSlicePoints", part.slicePoints.empty() ? "false" : "true");
    p->setAttribute("NeedsAnalysisData", "true");
    const juce::String last = number((double) std::max<int64_t>(0, part.sampleEnd > 0 ? part.sampleEnd : part.lengthSamples - 1));
    value(*p, "LomId", "0");
    value(*p, "Name", "");
    value(*p, "Selection", "true");
    value(*p, "IsActive", "true");
    value(*p, "Solo", "false");
    for (const auto& [tag, low, high] : {std::tuple{"KeyRange", part.keyMin, part.keyMax}, std::tuple{"VelocityRange", part.velMin, part.velMax},
                                         std::tuple{"SelectorRange", 0, 127}})
    {
        auto* range = p->createNewChildElement(tag);
        value(*range, "Min", juce::String(low)); value(*range, "Max", juce::String(high));
        value(*range, "CrossfadeMin", juce::String(low)); value(*range, "CrossfadeMax", juce::String(high));
    }
    value(*p, "RootKey", "60");
    value(*p, "Detune", "0");
    value(*p, "TuneScale", "100");
    value(*p, "Panorama", "0");
    value(*p, "Volume", "1");
    value(*p, "Link", "false");
    value(*p, "SampleStart", "0");
    value(*p, "SampleEnd", last);
    for (const auto& [tag, mode] : {std::pair{"SustainLoop", "0"}, std::pair{"ReleaseLoop", "3"}})
    {
        auto* loop = p->createNewChildElement(tag);
        value(*loop, "Start", "0"); value(*loop, "End", last); value(*loop, "Mode", mode);
        value(*loop, "Crossfade", "0"); value(*loop, "Detune", "0");
    }
    auto* sample = p->createNewChildElement("SampleRef");
    auto* ref = sample->createNewChildElement("FileRef");
    const juce::File file(juce::String(part.sample.absolutePath));
    value(*ref, "RelativePathType", "0");
    value(*ref, "RelativePath", "");
    value(*ref, "Path", "");
    value(*ref, "Type", "2");
    value(*ref, "LivePackName", "");
    value(*ref, "LivePackId", "");
    value(*ref, "OriginalFileSize", juce::String(part.sample.absolutePath.empty() ? (juce::int64) 0 : file.getSize()));
    value(*ref, "OriginalCrc", "0");
    value(*ref, "SourceHint", "");
    value(*sample, "LastModDate", juce::String(part.sample.absolutePath.empty() || ! file.existsAsFile() ? (juce::int64) 0
                                                                                                           : file.getLastModificationTime().toMilliseconds() / 1000));
    sample->createNewChildElement("SourceContext");
    value(*sample, "SampleUsageHint", "0");
    value(*sample, "DefaultDuration", "0");
    value(*sample, "DefaultSampleRate", "44100");
    value(*sample, "SamplesToAutoWarp", "1");
    value(*p, "SlicingThreshold", "100");
    value(*p, "SlicingBeatGrid", "4");
    value(*p, "SlicingRegions", "8");
    value(*p, "SlicingStyle", "0");
    auto* warp = p->createNewChildElement("SampleWarpProperties");
    auto* markers = warp->createNewChildElement("WarpMarkers");
    const double seconds = part.sampleRate > 0 ? (double) part.lengthSamples / part.sampleRate : 0.0;
    const double perBeat = part.secondsPerBeat > 0 ? part.secondsPerBeat : 0.5;
    for (int i = 0; i < 2; ++i)
    {
        auto* marker = markers->createNewChildElement("WarpMarker");
        marker->setAttribute("Id", i);
        marker->setAttribute("SecTime", number(i == 0 ? 0.0 : std::max(seconds, 1e-3)));
        marker->setAttribute("BeatTime", number(i == 0 ? 0.0 : std::max(seconds, 1e-3) / perBeat));
    }
    for (const auto& [tag, text] : std::initializer_list<std::pair<const char*, const char*>>{
             {"WarpMode", "0"}, {"GranularityTones", "30"}, {"GranularityTexture", "65"}, {"FluctuationTexture", "25"},
             {"ComplexProFormants", "100"}, {"ComplexProEnvelope", "128"}, {"TransientResolution", "6"},
             {"TransientLoopMode", "2"}, {"TransientEnvelope", "100"}, {"IsWarped", "false"}})
        value(*warp, tag, text);
    auto* onsets = warp->createNewChildElement("Onsets");
    onsets->createNewChildElement("UserOnsets");
    value(*onsets, "HasUserOnsets", "false");
    auto* signature = warp->createNewChildElement("TimeSignature")->createNewChildElement("TimeSignatures")->createNewChildElement("RemoteableTimeSignature");
    signature->setAttribute("Id", 0);
    value(*signature, "Numerator", "4"); value(*signature, "Denominator", "4"); value(*signature, "Time", "0");
    auto* grid = warp->createNewChildElement("BeatGrid");
    for (const auto& [tag, text] : std::initializer_list<std::pair<const char*, const char*>>{
             {"FixedNumerator", "1"}, {"FixedDenominator", "16"}, {"GridIntervalPixel", "20"}, {"Ntoles", "2"},
             {"SnapToGrid", "true"}, {"Fixed", "false"}})
        value(*grid, tag, text);
    for (const char* tag : {"InitialSlicePointsFromOnsets", "SlicePoints", "ManualSlicePoints", "BeatSlicePoints", "RegionSlicePoints"})
        p->createNewChildElement(tag);
    slicePoints(*p, "SlicePoints", part.slicePoints, &part.sliceEnergies);
    value(*p, "UseDynamicBeatSlices", "true");
    value(*p, "UseDynamicRegionSlices", "true");
    value(*p, "AreSlicesFromOnsetsEditable", "false");
    return p;
}
}

std::unique_ptr<juce::XmlElement> samplePartXml(const model::SamplePart& part, int id)
{
    std::unique_ptr<XmlElement> p;
    if (part.sourceXml) p = juce::parseXML(juce::String(*part.sourceXml));
    const bool fresh = p == nullptr;
    if (fresh) p = freshSamplePart(part);
    p->setAttribute("Id", id);
    auto leaf = [&](const std::string& key, double n) {
        const auto slash = key.find_last_of('/');
        value(slash == std::string::npos ? *p : *path(*p, key.substr(0, slash)), key.substr(slash == std::string::npos ? 0 : slash + 1).c_str(), number(n));
    };
    value(*p, "Name", juce::String(part.name));
    leaf("KeyRange/Min", part.keyMin); leaf("KeyRange/Max", part.keyMax);
    leaf("VelocityRange/Min", part.velMin); leaf("VelocityRange/Max", part.velMax);
    // Crossfades stay inside their range (a zone edited smaller keeps no fade outside itself; DEV-14b).
    const auto fades = [&](const char* tag, int low, int high, int fadeLow, int fadeHigh) {
        leaf(std::string(tag) + "/CrossfadeMin", std::clamp(fadeLow, low, std::max(low, high)));
        leaf(std::string(tag) + "/CrossfadeMax", std::clamp(fadeHigh, low, std::max(low, high)));
    };
    fades("KeyRange", part.keyMin, part.keyMax, part.keyFadeMin, part.keyFadeMax);
    fades("VelocityRange", part.velMin, part.velMax, part.velFadeMin, part.velFadeMax);
    leaf("SelectorRange/Min", part.selMin); leaf("SelectorRange/Max", part.selMax);
    fades("SelectorRange", part.selMin, part.selMax, part.selFadeMin, part.selFadeMax);
    leaf("RootKey", part.rootKey); leaf("Detune", part.detuneCents); leaf("Panorama", part.pan); leaf("Volume", part.volume);
    leaf("TuneScale", part.tuneScale);
    value(*p, "Link", part.link ? "true" : "false");
    value(*p, "IsActive", part.active ? "true" : "false");
    value(*p, "Solo", part.solo ? "true" : "false");
    leaf("SampleStart", (double) part.sampleStart); leaf("SampleEnd", (double) part.sampleEnd);
    // The reference writes an unused sustain loop over the whole region (mode 0), not 0..0.
    const bool unusedLoop = fresh && part.loopMode == 0 && part.loopEnd <= part.loopStart;
    leaf("SustainLoop/Start", (double) (unusedLoop ? part.sampleStart : part.loopStart));
    leaf("SustainLoop/End", (double) (unusedLoop ? part.sampleEnd : part.loopEnd));
    leaf("SustainLoop/Mode", part.loopMode); leaf("SustainLoop/Crossfade", (double) part.loopCrossfade);
    leaf("SustainLoop/Detune", part.loopDetune);
    if (! fresh || part.releaseMode != 3 || part.releaseStart != 0)
    {
        leaf("ReleaseLoop/Start", (double) part.releaseStart);
        leaf("ReleaseLoop/End", (double) (part.releaseEnd > 0 ? part.releaseEnd : part.sampleEnd));
        leaf("ReleaseLoop/Mode", part.releaseMode); leaf("ReleaseLoop/Crossfade", (double) part.releaseCrossfade);
        leaf("ReleaseLoop/Detune", part.releaseDetune);
    }
    auto* ref = path(*p, "SampleRef/FileRef");
    value(*ref, "RelativePathType", juce::String(part.sample.relativePathType));
    value(*ref, "RelativePath", juce::String(part.sample.relativePath));
    value(*ref, "Path", juce::String(part.sample.absolutePath));
    leaf("SampleRef/DefaultDuration", (double) part.lengthSamples); leaf("SampleRef/DefaultSampleRate", part.sampleRate);
    // Simpler slicing (DEV-4c): the Slice By settings and the manual slices are Sonora's to write; the transients the
    // reference found stay as it saved them.
    leaf("SlicingThreshold", part.slicingThreshold); leaf("SlicingBeatGrid", part.slicingBeatGrid);
    leaf("SlicingRegions", part.slicingRegions); leaf("SlicingStyle", part.slicingStyle);
    if (! fresh || ! part.manualSlicePoints.empty()) slicePoints(*p, "ManualSlicePoints", part.manualSlicePoints, nullptr);
    return p;
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

void addSwissArmyMeterRoutings(juce::XmlElement& routables)
{
    // ALS-15d: what the reference writes for the patch's sidechain input and its output (both unrouted).
    for (const auto& [list, kind, target] : {std::tuple{"InRoutings", "MxDInRoutable", "AudioIn/None"},
                                             std::tuple{"OutRoutings", "MxDOutRoutable", "AudioOut/None"}})
    {
        auto* holder = routables.getChildByName(list);
        if (! holder) holder = routables.createNewChildElement(list);
        if (holder->getNumChildElements() > 0) continue;
        auto* item = holder->createNewChildElement(kind);
        item->setAttribute("Id", 0);
        auto* routable = item->createNewChildElement("Routable");
        value(*routable, "Target", target);
        value(*routable, "UpperDisplayString", "No Output");
        value(*routable, "LowerDisplayString", "");
        auto* mpe = routable->createNewChildElement("MpeSettings");
        value(*mpe, "ZoneType", "0"); value(*mpe, "FirstNoteChannel", "1"); value(*mpe, "LastNoteChannel", "15");
        value(*routable, "MpePitchBendUsesTuning", "true");
        value(*item, "LomId", "0");
    }
}

std::pair<const char*, const char*> rackBranchClasses(const std::string& rackType)
{
    if (rackType == "DrumGroupDevice") return {"DrumBranch", "MidiToAudioDeviceChain"};
    if (rackType == "AudioEffectGroupDevice") return {"AudioEffectBranch", "AudioToAudioDeviceChain"};
    if (rackType == "MidiEffectGroupDevice") return {"MidiEffectBranch", "MidiToMidiDeviceChain"};
    return {"InstrumentBranch", "MidiToAudioDeviceChain"};
}

juce::XmlElement* rackBranchDevices(juce::XmlElement& branch, const std::string& rackType)
{
    const auto [branchTag, chainTag] = rackBranchClasses(rackType);
    branch.setTagName(branchTag);
    auto* deviceChain = branch.getChildByName("DeviceChain");
    if (! deviceChain) deviceChain = branch.createNewChildElement("DeviceChain");
    auto* inner = deviceChain->getChildByName(chainTag);
    if (! inner)
    {
        // A wrong chain class, or none (0.1.28 and earlier wrote Devices straight into DeviceChain): its content moves inside.
        auto* stray = deviceChain->getFirstChildElement();
        inner = new juce::XmlElement(chainTag);
        if (stray && ! stray->hasTagName("Devices") && stray->getChildByName("Devices"))
        {
            for (auto* c = stray->getFirstChildElement(); c != nullptr; c = stray->getFirstChildElement())
            {
                stray->removeChildElement(c, false); inner->addChildElement(c);
            }
        }
        for (auto* c = deviceChain->getFirstChildElement(); c != nullptr; c = deviceChain->getFirstChildElement())
        {
            deviceChain->removeChildElement(c, false);
            if (c->hasTagName("Devices") && ! inner->getChildByName("Devices")) inner->addChildElement(c);
            else delete c;
        }
        inner->setAttribute("Id", 0);
        deviceChain->addChildElement(inner);
    }
    auto* devices = inner->getChildByName("Devices");
    return devices ? devices : inner->createNewChildElement("Devices");
}

bool rackBranchesNeedRepair(const juce::XmlElement& x)
{
    const auto tag = x.getTagName().toStdString();
    if (model::isRackTag(tag))
        if (const auto* branches = x.getChildByName("Branches"))
            for (const auto* branch : branches->getChildIterator())
            {
                const auto [branchTag, chainTag] = rackBranchClasses(tag);
                const auto* deviceChain = branch->getChildByName("DeviceChain");
                const auto* inner = deviceChain ? deviceChain->getChildByName(chainTag) : nullptr;
                if (! branch->hasTagName(branchTag) || ! inner || ! inner->getChildByName("Devices")) return true;
            }
    for (const auto* c : x.getChildIterator())
        if (rackBranchesNeedRepair(*c)) return true;
    return false;
}

void repairRackBranches(juce::XmlElement& x)
{
    const auto tag = x.getTagName().toStdString();
    if (model::isRackTag(tag))
        if (auto* branches = x.getChildByName("Branches"))
            for (auto* branch : branches->getChildIterator())
                rackBranchDevices(*branch, tag);
    for (auto* c : x.getChildIterator())
        repairRackBranches(*c);
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
        if (device.maxPatch == "Swiss Army Meter") addSwissArmyMeterRoutings(*routables);
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
                    value(*range, "Min", number(param.rangeMin)); value(*range, "Max", number(param.rangeMax));
                }
            }
        if (device.type == "InstrumentVector" && ! device.sourceXml)
        {
            // DEV-28b: the default table and the matrix list with its target ids, which the reference requires.
            value(root, "SpriteName1", model::wavetable::kDefaultTable);
            value(root, "SpriteName2", model::wavetable::kDefaultTable);
            auto* list = root.createNewChildElement("ModulationConnections");
            for (size_t i = 0; i < model::wavetable::kTargetIds.size(); ++i)
            {
                auto* item = list->createNewChildElement("ModulationConnectionsForInstrumentVector");
                item->setAttribute("Id", (int) i);
                item->setAttribute("TargetId", model::wavetable::kTargetIds[i]);
                value(*item, "TargetName", model::wavetable::kTargetNames[i]);
            }
        }
        for (const auto& [key, number] : device.plain)
        {
            const auto slash = key.find_last_of('/');
            auto* parent = slash == std::string::npos ? &root : path(root, key.substr(0, slash));
            const auto leaf = key.substr(slash == std::string::npos ? 0 : slash + 1);
            const bool boolean = leaf == "RetriggerMode" || leaf == "DisplayPitchInHz" || leaf == "StrobeMode" || leaf == "UseConstPowCrossfade"
                                 || leaf == "MidSideBalanceOn" || leaf == "BassMonoAudition" || leaf == "EcoProcessing"
                                 // Sampler (DEV-14b): RemoteableBool leaves of the map, globals and view settings
                                 || leaf == "LoadInRam" || leaf == "RoundRobin" || leaf == "NumVoicesEnvTimeControl" || leaf == "IsSimpler"
                                 || leaf == "LegacyMode" || leaf == "IsModulated" || leaf == "ZoneEditorVisible" || leaf == "Seconds"
                                 || leaf == "IsAutoSelectEnabled" || leaf == "SimplerBreakoutVisible";
            value(*parent, leaf.c_str(), boolean ? juce::String(number >= 0.5 ? "true" : "false") : io::number(number));
        }
        if (! device.sampleParts.empty() || device.samplePartsEdited)
        {
            auto* parts = path(root, "Player/MultiSampleMap/SampleParts");
            int index = 0;
            for (const auto& part : device.sampleParts) parts->addChildElement(samplePartXml(part, index++).release());
        }
        if ((device.type == "OriginalSimpler" || device.type == "MultiSampler") && ! device.sourceXml)
        {
            // ALS-15b: the rest of the reference's sample map and the Simpler globals it writes for a new device.
            auto* map = path(root, "Player/MultiSampleMap");
            if (! map->getChildByName("SampleParts")) map->createNewChildElement("SampleParts");
            for (const auto& [tag, text] : std::initializer_list<std::pair<const char*, const char*>>{
                     {"LoadInRam", "false"}, {"LayerCrossfade", "0"}, {"SourceContext", ""}, {"RoundRobin", "false"},
                     {"RoundRobinMode", "0"}, {"RoundRobinResetPeriod", "0"}, {"RoundRobinRandomSeed", "0"}})
                if (! map->getChildByName(tag))
                {
                    if (std::string_view(tag) == "SourceContext") map->createNewChildElement(tag);
                    else value(*map, tag, text);
                }
            {
                // DEV-14b: a new Sampler is the same engine with IsSimpler false.
                const bool simpler = device.type == "OriginalSimpler";
                auto* globals = path(root, "Globals");
                for (const auto& [tag, text] : std::initializer_list<std::pair<const char*, const char*>>{
                         {"PitchBendRange", "5"}, {"MpePitchBendRange", "48"}, {"IsSimpler", simpler ? "true" : "false"}, {"PlaybackMode", "0"},
                         {"LegacyMode", "false"}})
                    if (! globals->getChildByName(tag)) value(*globals, tag, text);
                if (! root.getChildByName("SimplerSlicing")) value(*path(root, "SimplerSlicing"), "PlaybackMode", "0");
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
                // ALS-15d: each rack kind has its own branch class and chain class in the reference; the devices live in
                // <DeviceChain><ChainClass Id="0"><Devices>, never directly in <DeviceChain> ("Unknown class 'Devices'").
                const bool drum = device.type == "DrumGroupDevice";
                const bool midiRack = device.type == "MidiEffectGroupDevice";
                auto* branch = branches->createNewChildElement(rackBranchClasses(device.type).first);
                if (chain.sourceXml)
                    if (auto source = juce::XmlDocument::parse(juce::String(*chain.sourceXml)))
                    {
                        branches->removeChildElement(branch, true); branch = source.release(); branches->addChildElement(branch);
                    }
                branch->setAttribute("Id", index++);
                value(*path(*branch, "Name"), "EffectiveName", juce::String(chain.name));
                if (! midiRack) // MidiBranchMixerDevice has only Speaker
                {
                    parameter(*branch, "MixerDevice/Volume", chain.volume, -1);
                    parameter(*branch, "MixerDevice/Panorama", chain.pan, -1);
                }
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
                auto* children = rackBranchDevices(*branch, device.type);
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
    else if (device.kind == model::DeviceKind::PluginVst2 && device.plugin)
    {
        // PLG-15: <PluginDevice><PluginDesc><VstPluginInfo>, the fields Live 12 writes (real-world sets).
        const auto& plugin = *device.plugin;
        auto* info = root.createNewChildElement("PluginDesc")->createNewChildElement("VstPluginInfo");
        info->setAttribute("Id", 0);
        value(*info, "WinPosX", "0");
        value(*info, "WinPosY", "0");
        value(*info, "Path", juce::String(plugin.path));
        value(*info, "PlugName", juce::String(plugin.name));
        value(*info, "UniqueId", juce::String(plugin.vst2UniqueId));
        value(*info, "Inputs", "0");
        value(*info, "Outputs", "0");
        value(*info, "NumberOfParameters", juce::String(plugin.vst2ParameterCount));
        value(*info, "NumberOfPrograms", juce::String(plugin.vst2ProgramCount));
        value(*info, "Flags", "0");
        auto* preset = info->createNewChildElement("Preset")->createNewChildElement("VstPreset");
        preset->setAttribute("Id", 0);
        value(*preset, "IsOn", "true");
        value(*preset, "Type", juce::String((int64_t) plugin.vst2PresetType));
        value(*preset, "ProgramCount", juce::String(plugin.vst2ProgramCount));
        value(*preset, "ParameterCount", juce::String(plugin.vst2ParameterCount));
        value(*preset, "ProgramNumber", juce::String(plugin.vst2ProgramNumber));
        preset->createNewChildElement("Buffer")->addTextElement(hex(plugin.state));
        value(*preset, "Name", "");
        value(*preset, "PluginVersion", juce::String(plugin.vst2PluginVersion));
        value(*preset, "UniqueId", juce::String(plugin.vst2UniqueId));
        value(*preset, "ByteOrder", "2");
        value(*info, "Version", "1");
        value(*info, "VstVersion", "2400");
        value(*info, "IsShellClient", "false");
        value(*info, "Category", plugin.isInstrument ? "2" : "1");
        root.createNewChildElement("ParameterList");
    }
    else if (device.kind == model::DeviceKind::PluginAu && device.plugin)
    {
        // PLG-15: <AuPluginDevice><PluginDesc><AuPluginInfo>; the preset buffer is the ClassInfo XML plist.
        const auto& plugin = *device.plugin;
        auto* info = root.createNewChildElement("PluginDesc")->createNewChildElement("AuPluginInfo");
        info->setAttribute("Id", 0);
        value(*info, "WinPosX", "0");
        value(*info, "WinPosY", "0");
        value(*info, "ComponentType", juce::String((int64_t) plugin.auType));
        value(*info, "ComponentSubType", juce::String((int64_t) plugin.auSubType));
        value(*info, "ComponentManufacturer", juce::String((int64_t) plugin.auManufacturer));
        value(*info, "ComponentFlags", "2");
        value(*info, "Name", juce::String(plugin.name));
        value(*info, "Manufacturer", juce::String(plugin.vendor));
        auto* preset = info->createNewChildElement("Preset")->createNewChildElement("AuPreset");
        preset->setAttribute("Id", 0);
        value(*preset, "IsOn", "true");
        preset->createNewChildElement("Buffer")->addTextElement(hex(plugin.state));
        value(*preset, "Name", "Default");
        value(*preset, "Manufacturer", juce::String((int64_t) plugin.auManufacturer));
        value(*preset, "SubType", juce::String((int64_t) plugin.auSubType));
        value(*preset, "Type", juce::String((int64_t) plugin.auType));
        value(*info, "IsUnusable", "false");
        root.createNewChildElement("ParameterList");
    }
    return root.toString().toStdString();
}
} // namespace sonora::io
