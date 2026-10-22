#include "io/als/AlsReader.h"
#include "core/Scales.h"

#include "io/als/AlsNewSet.h"

#include "core/DeviceCatalog.h"
#include "core/TimeUtils.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <mutex>

namespace sonora::io
{

using juce::XmlElement;
using namespace sonora::model;

namespace
{
// ---------------------------------------------------------------- XML helpers
// Live stores almost every scalar as <Tag Value="..."/>. Paths are "/"-separated
// element names relative to `e`.

const XmlElement* at(const XmlElement* e, const char* path)
{
    if (e == nullptr)
        return nullptr;
    juce::StringArray parts;
    parts.addTokens(path, "/", "");
    for (const auto& p : parts)
    {
        e = e->getChildByName(p);
        if (e == nullptr)
            return nullptr;
    }
    return e;
}

juce::String str(const XmlElement* e, const char* path, const juce::String& fallback = {})
{
    if (const auto* c = at(e, path))
        return c->getStringAttribute("Value", fallback);
    return fallback;
}

double num(const XmlElement* e, const char* path, double fallback = 0.0)
{
    if (const auto* c = at(e, path); c != nullptr && c->hasAttribute("Value"))
        return c->getDoubleAttribute("Value", fallback);
    return fallback;
}

int integer(const XmlElement* e, const char* path, int fallback = 0)
{
    if (const auto* c = at(e, path); c != nullptr && c->hasAttribute("Value"))
        return c->getIntAttribute("Value", fallback);
    return fallback;
}

bool boolean(const XmlElement* e, const char* path, bool fallback = false)
{
    if (const auto* c = at(e, path); c != nullptr && c->hasAttribute("Value"))
        return c->getStringAttribute("Value").equalsIgnoreCase("true");
    return fallback;
}

// <ScaleInformation>: Live 12 writes <Root/> and a scale index in <Name/>; Live 11 wrote <RootNote/> and the scale's
// name ("Major", "Dorian"). Unknown names read as Major (UI-30b).
void readScale(const XmlElement* owner, int& root, int& name)
{
    const auto* scale = at(owner, "ScaleInformation");
    if (scale == nullptr) return;
    root = at(scale, "Root") ? integer(scale, "Root", 0) : integer(scale, "RootNote", 0);
    const auto stored = str(scale, "Name");
    if (stored.containsOnly("0123456789-") && stored.isNotEmpty()) name = stored.getIntValue();
    else name = std::max(0, model::scaleIndexByName(stored.toStdString()));
}

int automationTargetId(const XmlElement* param)
{
    if (const auto* t = at(param, "AutomationTarget"))
        return t->getIntAttribute("Id", -1);
    return -1;
}

MidiBinding midiBinding(const XmlElement* param)
{
    MidiBinding b;
    if (const auto* k = at(param, "KeyMidi"))
    {
        b.channel = integer(k, "Channel", -1);
        b.isNote = boolean(k, "IsNote", false);
        b.number = integer(k, "NoteOrController", 0);
        b.mapMode = integer(k, "ControllerMapMode", 0);
        if (b.number < 0 || b.channel < 0)
            b.channel = -1;
    }
    return b;
}

// ---------------------------------------------------------------- clips

std::vector<AutomationEnvelope> readEnvelopes(const XmlElement* envelopesParent)
{
    std::vector<AutomationEnvelope> out;
    if (envelopesParent == nullptr)
        return out;
    for (const auto* env : envelopesParent->getChildIterator())
    {
        if (! env->hasTagName("AutomationEnvelope") && ! env->hasTagName("ClipEnvelope")) // track / clip envelopes
            continue;
        AutomationEnvelope ae;
        ae.targetId = integer(env, "EnvelopeTarget/PointeeId", -1);
        if (const auto* events = at(env, "Automation/Events"))
            for (const auto* ev : events->getChildIterator())
            {
                // BoolEvent Value="true"/"false", EnumEvent Value="<int>", FloatEvent Value="<double>".
                ae.eventType = ev->getTagName().toStdString();
                const auto raw = ev->getStringAttribute("Value");
                double value = raw.getDoubleValue();
                if (ev->hasTagName("BoolEvent"))
                {
                    value = raw.equalsIgnoreCase("true") ? 1.0 : 0.0;
                    ae.stepped = true;
                }
                else if (ev->hasTagName("EnumEvent"))
                {
                    ae.stepped = true;
                }
                AutomationPoint point{ev->getDoubleAttribute("Time"), value};
                if (ev->hasTagName("FloatEvent") && ev->hasAttribute("CurveControl1X"))
                {
                    point.x1 = ev->getDoubleAttribute("CurveControl1X", 0.5); point.y1 = ev->getDoubleAttribute("CurveControl1Y", 0.5);
                    point.x2 = ev->getDoubleAttribute("CurveControl2X", 0.5); point.y2 = ev->getDoubleAttribute("CurveControl2Y", 0.5);
                    if (! std::isfinite(point.x1 + point.y1 + point.x2 + point.y2)) point.x1 = point.y1 = point.x2 = point.y2 = 0.5;
                }
                ae.points.push_back(point);
            }
        std::stable_sort(ae.points.begin(), ae.points.end(),
                         [](const AutomationPoint& a, const AutomationPoint& b) { return a.time < b.time; });
        out.push_back(std::move(ae));
    }
    return out;
}

void readClipCommon(const XmlElement* x, Clip& c)
{
    c.uid = allocateClipUid();
    c.takeId = integer(x, "TakeId", -1);
    if (const auto* alias = x->getChildByName("SonoraAlias")) c.linkGroup = std::max(-1, alias->getIntAttribute("Group", -1));
    c.sourceXml = std::make_shared<const std::string>(x->toString().toStdString());
    c.xmlId = x->getIntAttribute("Id", -1);
    c.time = x->getDoubleAttribute("Time", 0.0);
    c.name = str(x, "Name").toStdString();
    c.colorIndex = integer(x, "Color", 0);
    c.currentStart = num(x, "CurrentStart", c.time);
    c.currentEnd = num(x, "CurrentEnd", c.currentStart + 4.0);
    c.loop.loopStart = num(x, "Loop/LoopStart", 0.0);
    c.loop.loopEnd = num(x, "Loop/LoopEnd", 4.0);
    c.loop.startRelative = num(x, "Loop/StartRelative", 0.0);
    c.loop.enabled = boolean(x, "Loop/LoopOn", true);
    c.disabled = boolean(x, "Disabled", false);
    c.launchMode = integer(x, "LaunchMode", 0);
    c.launchQuantisation = integer(x, "LaunchQuantisation", 0);
    c.legato = boolean(x, "Legato", false);
    readScale(x, c.scaleRoot, c.scaleName);
    c.inKey = boolean(x, "IsInKey", false);
    c.velocityAmount = num(x, "VelocityAmount", 0.0);
    if (const auto* signature = at(x, "TimeSignature/TimeSignatures/RemoteableTimeSignature"))
    {
        c.timeSigNumerator = std::max(1, integer(signature, "Numerator", 4));
        c.timeSigDenominator = std::max(1, integer(signature, "Denominator", 4));
    }
    c.envelopes = readEnvelopes(at(x, "Envelopes/Envelopes")); // clip envelopes (ALS-9)
}

Clip readMidiClip(const XmlElement* x)
{
    Clip c;
    c.kind = Clip::Kind::Midi;
    readClipCommon(x, c);

    // Live 11+: Notes/KeyTracks/KeyTrack{ MidiKey, Notes/MidiNoteEvent* }
    if (const auto* keyTracks = at(x, "Notes/KeyTracks"))
    {
        for (const auto* kt : keyTracks->getChildWithTagNameIterator("KeyTrack"))
        {
            const int pitch = integer(kt, "MidiKey", 60);
            if (const auto* notes = at(kt, "Notes"))
                for (const auto* n : notes->getChildWithTagNameIterator("MidiNoteEvent"))
                {
                    MidiNote note;
                    note.pitch = pitch;
                    note.start = n->getDoubleAttribute("Time");
                    note.duration = n->getDoubleAttribute("Duration", 0.25);
                    note.velocity = (float) n->getDoubleAttribute("Velocity", 100.0);
                    note.releaseVelocity = (float) n->getDoubleAttribute("OffVelocity", 64.0);
                    note.probability = (float) n->getDoubleAttribute("Probability", 1.0);
                    note.enabled = ! n->getStringAttribute("IsEnabled", "true").equalsIgnoreCase("false");
                    note.noteId = n->getIntAttribute("NoteId", -1);
                    note.velocityDeviation = (float) n->getDoubleAttribute("VelocityDeviation", 0.0);
                    c.notes.push_back(note);
                }
        }
    }
    if (const auto* lists = at(x, "Notes/PerNoteEventStore/EventLists"))
        for (const auto* list : lists->getChildWithTagNameIterator("PerNoteEventList"))
        {
            NoteExpression expression; expression.noteId = list->getIntAttribute("NoteId", -1); expression.cc = list->getIntAttribute("CC", -999);
            if (const auto* events = list->getChildByName("Events"))
                for (const auto* event : events->getChildWithTagNameIterator("PerNoteEvent"))
                {
                    NoteExpression::Point point;
                    point.time = event->getDoubleAttribute("TimeOffset"); point.value = event->getDoubleAttribute("Value");
                    point.x1 = event->getDoubleAttribute("CurveControl1X", 0); point.y1 = event->getDoubleAttribute("CurveControl1Y", 0);
                    point.x2 = event->getDoubleAttribute("CurveControl2X", 1); point.y2 = event->getDoubleAttribute("CurveControl2Y", 1);
                    if (std::isfinite(point.time) && std::isfinite(point.value)) expression.points.push_back(point);
                }
            std::stable_sort(expression.points.begin(), expression.points.end(), [](const auto& a, const auto& b) { return a.time < b.time; });
            c.noteExpressions.push_back(std::move(expression));
        }
    std::sort(c.notes.begin(), c.notes.end(), [](const MidiNote& a, const MidiNote& b) {
        return a.start < b.start || (! (b.start < a.start) && a.pitch < b.pitch);
    });
    assignNoteKeys(c.notes);
    return c;
}

Clip readAudioClip(const XmlElement* x)
{
    Clip c;
    c.kind = Clip::Kind::Audio;
    readClipCommon(x, c);
    c.warped = boolean(x, "IsWarped", false);
    c.tempoAnalyzed = boolean(x, "SonoraTempoAnalyzed", c.warped);
    c.warpMode = static_cast<WarpMode>(integer(x, "WarpMode", 0));
    c.gain = num(x, "SampleVolume", 1.0);
    c.pitchCoarse = integer(x, "PitchCoarse", 0);
    c.pitchFine = num(x, "PitchFine", 0.0);
    c.transientResolution = integer(x, "TransientResolution", 6);
    c.transientLoopMode = integer(x, "TransientLoopMode", 2);
    c.transientEnvelope = num(x, "TransientEnvelope", 100.0);
    c.granularityTones = num(x, "GranularityTones", 30.0);
    c.granularityTexture = num(x, "GranularityTexture", 65.0);
    c.fluctuationTexture = num(x, "FluctuationTexture", 25.0);
    c.complexProFormants = num(x, "ComplexProFormants", 100.0);
    c.complexProEnvelope = num(x, "ComplexProEnvelope", 128.0);
    c.clipFade = boolean(x, "Fade", true);
    c.ram = boolean(x, "Ram", false);
    c.hiQ = boolean(x, "HiQ", true);
    c.fadeIn = std::max(0.0, num(x, "Fades/FadeInLength", 0.0));
    c.fadeOut = std::max(0.0, num(x, "Fades/FadeOutLength", 0.0));
    c.fadeInCurve = num(x, "Fades/FadeInCurveSkew", 0.0);
    c.fadeOutCurve = num(x, "Fades/FadeOutCurveSkew", 0.0);
    c.fadeInDefault = boolean(x, "Fades/IsDefaultFadeIn", true);
    c.fadeOutDefault = boolean(x, "Fades/IsDefaultFadeOut", true);

    c.sample.absolutePath = str(x, "SampleRef/FileRef/Path").toStdString();
    c.sample.relativePath = str(x, "SampleRef/FileRef/RelativePath").toStdString();
    c.sample.relativePathType = integer(x, "SampleRef/FileRef/RelativePathType", 0);
    c.sampleRate = num(x, "SampleRef/DefaultSampleRate", 0.0);
    c.lengthSamples = (int64_t) num(x, "SampleRef/DefaultDuration", 0.0);

    if (const auto* markers = at(x, "WarpMarkers"))
        for (const auto* m : markers->getChildWithTagNameIterator("WarpMarker"))
            c.warpMarkers.push_back({m->getDoubleAttribute("SecTime"), m->getDoubleAttribute("BeatTime")});
    return c;
}

std::optional<Clip> readClipIn(const XmlElement* parent)
{
    if (parent == nullptr)
        return std::nullopt;
    if (const auto* m = parent->getChildByName("MidiClip"))
        return readMidiClip(m);
    if (const auto* a = parent->getChildByName("AudioClip"))
        return readAudioClip(a);
    return std::nullopt;
}

// ---------------------------------------------------------------- devices

// Collects every "<X><Manual Value=.../></X>" below a native device as a flat parameter list.
void collectParameters(const XmlElement* e, const juce::String& prefix, std::vector<Parameter>& out, int depth)
{
    if (depth > 6)
        return;
    for (const auto* child : e->getChildIterator())
    {
        const auto tag = child->getTagName();
        // Skip big subtrees that never hold device parameters.
        if (tag == "Branches" || tag == "ReturnBranches" || tag == "BranchPresets" || tag == "LastPresetRef" || tag == "SourceContext" || tag == "ViewData"
            || tag == "Pointee" || tag == "SampleParts" || tag == "MultiSampleMap")
            continue;

        const auto path = prefix.isEmpty() ? tag : prefix + "/" + tag;
        if (const auto* manual = child->getChildByName("Manual"); manual != nullptr && manual->hasAttribute("Value"))
        {
            const auto v = manual->getStringAttribute("Value");
            Parameter p;
            p.path = path.toStdString();
            p.value = v.equalsIgnoreCase("true") ? 1.0 : v.equalsIgnoreCase("false") ? 0.0 : v.getDoubleValue();
            p.booleanValue = v.equalsIgnoreCase("true") || v.equalsIgnoreCase("false");
            p.automationTargetId = automationTargetId(child);
            p.midi = midiBinding(child);
            if (p.midi.isMacro())
            {
                p.macroIndex = p.midi.number;
                p.rangeMin = num(child, "MidiControllerRange/Min", 0.0);
                p.rangeMax = num(child, "MidiControllerRange/Max", 1.0);
                p.midi = {}; // it is a macro, not a hardware mapping
            }
            out.push_back(std::move(p));
        }
        else if (child->getNumChildElements() > 0)
        {
            collectParameters(child, path, out, depth + 1);
        }
    }
}

std::vector<Device> readDevices(const XmlElement* devicesElement, std::vector<std::string>& warnings);

// <SideChain><OnOff/><RoutedInput><Routable><Target Value="AudioIn/Track.93/PreFxOut"/>...
Sidechain readSidechain(const XmlElement* device)
{
    Sidechain sc;
    const auto* side = device->getChildByName("SideChain");
    if (side == nullptr)
        return sc;
    sc.on = boolean(side, "OnOff/Manual", false);
    sc.gain = num(side, "RoutedInput/Volume/Manual", 1.0);
    const auto target = str(side, "RoutedInput/Routable/Target");
    if (target.startsWith("AudioIn/Track."))
    {
        const auto rest = target.fromFirstOccurrenceOf("AudioIn/Track.", false, false);
        sc.sourceTrackId = rest.upToFirstOccurrenceOf("/", false, false).getIntValue();
        const auto tapName = rest.fromFirstOccurrenceOf("/", false, false);
        sc.tap = tapName == "PreFxOut" ? Sidechain::Tap::PreFx : tapName == "PostMixerOut" ? Sidechain::Tap::PostMixer
                                                                                           : Sidechain::Tap::PostFx;
    }
    return sc;
}

// <Globals><PlaybackMode Value="1"/>... : plain leaves that are not "Manual" parameters.
void collectGlobals(const XmlElement* device, std::map<std::string, double>& out)
{
    if (const auto* g = device->getChildByName("Globals"))
        for (const auto* c : g->getChildIterator())
            if (c->hasAttribute("Value"))
            {
                const auto v = c->getStringAttribute("Value");
                out[("Globals/" + c->getTagName()).toStdString()] =
                    v.equalsIgnoreCase("true") ? 1.0 : v.equalsIgnoreCase("false") ? 0.0 : v.getDoubleValue();
            }
}

std::vector<SamplePart> readSampleParts(const XmlElement* device)
{
    std::vector<SamplePart> out;
    const auto* parts = at(device, "Player/MultiSampleMap/SampleParts");
    if (parts == nullptr)
        return out;
    for (const auto* p : parts->getChildWithTagNameIterator("MultiSamplePart"))
    {
        SamplePart sp;
        sp.sourceXml = std::make_shared<const std::string>(p->toString().toStdString());
        sp.name = str(p, "Name").toStdString();
        sp.keyMin = integer(p, "KeyRange/Min", 0);
        sp.keyMax = integer(p, "KeyRange/Max", 127);
        sp.velMin = integer(p, "VelocityRange/Min", 1);
        sp.velMax = integer(p, "VelocityRange/Max", 127);
        sp.rootKey = integer(p, "RootKey", 60);
        sp.detuneCents = num(p, "Detune", 0.0);
        sp.pan = num(p, "Panorama", 0.0);
        sp.volume = num(p, "Volume", 1.0);
        sp.sampleStart = (int64_t) num(p, "SampleStart", 0.0);
        sp.sampleEnd = (int64_t) num(p, "SampleEnd", 0.0);
        sp.loopStart = (int64_t) num(p, "SustainLoop/Start", 0.0);
        sp.loopEnd = (int64_t) num(p, "SustainLoop/End", 0.0);
        sp.loopMode = integer(p, "SustainLoop/Mode", 0);
        sp.loopCrossfade = (int64_t) num(p, "SustainLoop/Crossfade", 0.0);
        sp.sample.absolutePath = str(p, "SampleRef/FileRef/Path").toStdString();
        sp.sample.relativePath = str(p, "SampleRef/FileRef/RelativePath").toStdString();
        sp.sample.relativePathType = integer(p, "SampleRef/FileRef/RelativePathType", 0);
        sp.sampleRate = num(p, "SampleRef/DefaultSampleRate", 0.0);
        sp.lengthSamples = (int64_t) num(p, "SampleRef/DefaultDuration", 0.0);
        out.push_back(std::move(sp));
    }
    return out;
}

const XmlElement* findFirstDescendant(const XmlElement* e, const juce::String& tag, int maxDepth)
{
    if (e == nullptr || maxDepth < 0)
        return nullptr;
    for (const auto* c : e->getChildIterator())
    {
        if (c->getTagName() == tag)
            return c;
        if (const auto* found = findFirstDescendant(c, tag, maxDepth - 1))
            return found;
    }
    return nullptr;
}

PluginInfo readPluginInfo(const XmlElement* desc, DeviceKind& kindOut)
{
    PluginInfo info;
    if (const auto* v2 = desc->getChildByName("VstPluginInfo"))
    {
        kindOut = DeviceKind::PluginVst2;
        info.name = str(v2, "PlugName").toStdString();
        info.path = str(v2, "Path").toStdString();
        info.vst2UniqueId = (int64_t) num(v2, "UniqueId");
        info.isInstrument = integer(v2, "Category", 0) == 2; // TODO(ALS-7): confirm Category semantics
        if (const auto* buf = at(v2, "Preset/VstPreset/Buffer"))
            info.state = decodeHexBlob(buf->getAllSubText().toStdString());
    }
    else if (const auto* v3 = desc->getChildByName("Vst3PluginInfo"))
    {
        kindOut = DeviceKind::PluginVst3;
        info.name = str(v3, "Name").toStdString();
        for (int i = 0; i < 4; ++i)
            info.vst3Uid[(size_t) i] = (uint32_t) (int64_t) num(v3, ("Uid/Fields." + juce::String(i)).toRawUTF8());
        info.isInstrument = integer(v3, "DeviceType", 0) == 1; // TODO(ALS-7): 1=instrument? 2=effect seen
        if (const auto* p = at(v3, "Preset/Vst3Preset"))
        {
            if (const auto* s = p->getChildByName("ProcessorState"))
                info.state = decodeHexBlob(s->getAllSubText().toStdString());
            if (const auto* s = p->getChildByName("ControllerState"))
                info.controllerState = decodeHexBlob(s->getAllSubText().toStdString());
        }
    }
    else if (const auto* au = desc->getChildByName("AuPluginInfo"))
    {
        kindOut = DeviceKind::PluginAu;
        info.name = str(au, "Name").toStdString();
        info.vendor = str(au, "Manufacturer").toStdString();
        info.auType = (uint32_t) (int64_t) num(au, "ComponentType");
        info.auSubType = (uint32_t) (int64_t) num(au, "ComponentSubType");
        info.auManufacturer = (uint32_t) (int64_t) num(au, "ComponentManufacturer");
        info.isInstrument = info.auType == 0x61756d75; // 'aumu'
        if (const auto* buf = at(au, "Preset/AuPreset/Buffer"))
            info.state = decodeHexBlob(buf->getAllSubText().toStdString());
    }
    return info;
}

Device readDevice(const XmlElement* x, std::vector<std::string>& warnings)
{
    Device d;
    d.type = x->getTagName().toStdString();
    d.id = x->getIntAttribute("Id", -1);
    d.sourceXml = std::make_shared<const std::string>(x->toString().toStdString());
    d.enabled = boolean(x, "On/Manual", true);
    d.enabledTargetId = automationTargetId(at(x, "On"));
    const auto userName = str(x, "UserName").toStdString();

    if (d.type == "PluginDevice" || d.type == "AuPluginDevice")
    {
        if (const auto* desc = at(x, "PluginDesc"))
            d.plugin = readPluginInfo(desc, d.kind);
        if (const auto* list = x->getChildByName("ParameterList"))
            for (const auto* pp : list->getChildWithTagNameIterator("PluginFloatParameter"))
            {
                PluginParam p;
                p.parameterId = integer(pp, "ParameterId", -1);
                p.name = str(pp, "ParameterName").toStdString();
                p.value = num(pp, "ParameterValue/Manual", 0.0);
                p.automationTargetId = automationTargetId(at(pp, "ParameterValue"));
                // Automatable ones, and every parameter of VST2 devices (their state blob cannot be restored
                // into the VST3 build, so the stored values are what we can migrate: PLG-5).
                if (p.automationTargetId >= 0 || d.kind == DeviceKind::PluginVst2)
                    d.pluginParams.push_back(std::move(p));
            }
        d.displayName = d.plugin ? d.plugin->name : d.type;
    }
    else if (isMaxForLiveTag(d.type))
    {
        d.kind = DeviceKind::MaxForLive;
        auto patch = str(x, "PatchSlot/Value/MxPatchRef/FileRef/RelativePath");
        d.maxPatchPath = str(x, "PatchSlot/Value/MxPatchRef/FileRef/Path").toStdString();
        if (patch.isEmpty()) patch = juce::String(d.maxPatchPath);
        // The set's own path syntax, not this OS's: juce::File only splits on '\\' on Windows (COM-7).
        auto fileName = patch.fromLastOccurrenceOf("/", false, false).fromLastOccurrenceOf("\\", false, false);
        if (fileName.containsChar('.')) fileName = fileName.upToLastOccurrenceOf(".", false, false);
        d.maxPatch = fileName.toStdString();
        if (const auto native = maxForLiveNativeName(d.maxPatch); ! native.empty())
            d.displayName = std::string(native); // DEV-23: runs as a native replacement
        else
        {
            d.displayName = d.maxPatch.empty() ? "Max for Live device" : d.maxPatch;
            warnings.push_back("Max for Live device skipped (unsupported)" + (d.maxPatch.empty() ? std::string() : ": " + d.maxPatch));
        }
    }
    else if (isRackTag(d.type))
    {
        d.kind = DeviceKind::Rack;
        d.displayName = d.type == "DrumGroupDevice" ? "Drum Rack"
                        : d.type == "InstrumentGroupDevice" ? "Instrument Rack"
                        : d.type == "AudioEffectGroupDevice" ? "Audio Effect Rack"
                                                             : "MIDI Effect Rack";
        if (const auto* branches = at(x, "Branches"))
            for (const auto* br : branches->getChildIterator())
            {
                Device::Chain chain;
                chain.sourceXml = std::make_shared<const std::string>(br->toString().toStdString());
                chain.name = str(br, "Name/EffectiveName", str(br, "Name")).toStdString();
                chain.devices = readDevices(findFirstDescendant(br, "Devices", 4), warnings);
                chain.volume = num(br, "MixerDevice/Volume/Manual", 1.0);
                chain.pan = num(br, "MixerDevice/Panorama/Manual", 0.0);
                chain.speakerOn = boolean(br, "MixerDevice/Speaker/Manual", true);
                chain.keyMin = integer(br, "ZoneSettings/KeyRange/Min", 0);
                chain.keyMax = integer(br, "ZoneSettings/KeyRange/Max", 127);
                chain.velMin = integer(br, "ZoneSettings/VelocityRange/Min", 1);
                chain.velMax = integer(br, "ZoneSettings/VelocityRange/Max", 127);
                chain.selMin = integer(br, "BranchSelectorRange/Min", 0);
                chain.selMax = integer(br, "BranchSelectorRange/Max", 127);
                chain.chokeGroup = integer(br, "BranchInfo/ChokeGroup", 0);
                if (br->hasTagName("DrumBranch"))
                {
                    // Inferred encoding: stored value == 128 - MIDI note (92 == C1 == 36). ALS-8.
                    const int stored = integer(br, "BranchInfo/ReceivingNote", -1);
                    chain.receivingNote = stored > 0 ? 128 - stored : -1;
                    chain.sendingNote = (int) num(br, "BranchInfo/SendingNote", 60.0);
                }
                d.chains.push_back(std::move(chain));
            }
        collectParameters(x, {}, d.parameters, 0); // macros live here
        for (int i = 0; i < 16; ++i)
            d.macroNames.push_back(str(x, ("MacroDisplayNames." + juce::String(i)).toRawUTF8(), "Macro " + juce::String(i + 1))
                                       .toStdString());
        d.numVisibleMacros = integer(x, "NumVisibleMacroControls", 8);
    }
    else
    {
        d.kind = DeviceKind::Native;
        const auto* entry = findCatalogEntry(d.type);
        d.displayName = entry ? std::string(entry->displayName) : d.type;
        if (entry == nullptr || ! entry->implemented)
            warnings.push_back("Native device not implemented: " + d.type);
        collectParameters(x, {}, d.parameters, 0);
        collectGlobals(x, d.plain);
        static const std::vector<const char*> tunerKeys{"ViewMode", "DisplayPitchInHz", "StrobeMode", "AccidentalSpelling"},
            delayKeys{"DryWetMode", "EcoProcessing", "DelayLine_CompatibilityMode"},
            utilityKeys{"MidSideBalanceOn", "BassMonoAudition"}; // root-level plain leaves
        if (d.type == "Tuner" || d.type == "Delay" || d.type == "StereoGain")
            for (const auto* key : d.type == "Tuner" ? tunerKeys : d.type == "Delay" ? delayKeys : utilityKeys)
                if (const auto* leaf = x->getChildByName(key); leaf && leaf->hasAttribute("Value"))
                {
                    const auto value = leaf->getStringAttribute("Value");
                    d.plain[key] = value.equalsIgnoreCase("true") ? 1.0 : value.equalsIgnoreCase("false") ? 0.0 : value.getDoubleValue();
                }
        d.sidechain = readSidechain(x);
        if (const auto* state = x->getChildByName("SonoraState"))
        {
            auto content = std::make_shared<NativeContent>();
            content->format = state->getStringAttribute("Format").toStdString();
            content->sourceDirectory = state->getStringAttribute("SourceDirectory").toStdString();
            const auto* processor = state->getChildByName("ProcessorState");
            const auto* controller = state->getChildByName("ControllerState");
            const auto processorText = processor != nullptr ? processor->getAllSubText() : juce::String();
            const auto controllerText = controller != nullptr ? controller->getAllSubText() : juce::String();
            const auto characters = static_cast<uint64_t>(processorText.length())
                                  + static_cast<uint64_t>(controllerText.length());
            if (characters > NativeContent::maxBytes * 2)
                warnings.push_back("Native device content exceeds its size limit: " + d.type);
            else
            {
                content->data = decodeHexBlob(processorText.toStdString());
                content->controllerData = decodeHexBlob(controllerText.toStdString());
                d.nativeContent = std::move(content);
            }
        }
        if (d.type == "Eq8") // device-level plain values that are real settings
            for (const char* name : {"Mode", "Precision", "AdaptiveQFactor"})
                if (const auto* e = x->getChildByName(name); e != nullptr && e->hasAttribute("Value"))
                    d.parameters.push_back({name, e->getDoubleAttribute("Value"), -1, {}});
        if (d.type == "OriginalSimpler" || d.type == "MultiSampler" || d.type == "SonoraImpulse")
        {
            d.sampleParts = readSampleParts(x);
            for (const char* key : {"UseConstPowCrossfade", "InterpolationMode"}) // plain playback settings (DEV-22)
                if (const auto* leaf = at(x, (juce::String("Player/") + key).toRawUTF8()); leaf && leaf->hasAttribute("Value"))
                {
                    const auto value = leaf->getStringAttribute("Value");
                    d.plain[std::string("Player/") + key] = value.equalsIgnoreCase("true") ? 1.0 : value.equalsIgnoreCase("false") ? 0.0 : value.getDoubleValue();
                }
        }
    }

    if (! userName.empty())
        d.displayName = userName;
    return d;
}

std::vector<Device> readDevices(const XmlElement* devicesElement, std::vector<std::string>& warnings)
{
    std::vector<Device> out;
    if (devicesElement == nullptr)
        return out;
    for (const auto* dx : devicesElement->getChildIterator())
        out.push_back(readDevice(dx, warnings));
    return out;
}

// ---------------------------------------------------------------- tracks

Mixer readMixer(const XmlElement* mixer)
{
    Mixer m;
    m.volume = num(mixer, "Volume/Manual", 1.0);
    m.pan = num(mixer, "Pan/Manual", 0.0);
    m.speakerOn = boolean(mixer, "Speaker/Manual", true);
    m.solo = boolean(mixer, "SoloSink", false);
    m.volumeTargetId = automationTargetId(at(mixer, "Volume"));
    m.panTargetId = automationTargetId(at(mixer, "Pan"));
    m.speakerTargetId = automationTargetId(at(mixer, "Speaker"));
    m.tempoTargetId = automationTargetId(at(mixer, "Tempo"));
    m.timeSignatureTargetId = automationTargetId(at(mixer, "TimeSignature"));
    m.volumeMidi = midiBinding(at(mixer, "Volume"));
    m.panMidi = midiBinding(at(mixer, "Pan"));
    m.speakerMidi = midiBinding(at(mixer, "Speaker"));
    if (const auto* sends = at(mixer, "Sends"))
        for (const auto* holder : sends->getChildWithTagNameIterator("TrackSendHolder"))
        {
            m.sends.push_back(num(holder, "Send/Manual", 0.0));
            m.sendTargetIds.push_back(automationTargetId(at(holder, "Send")));
            m.sendMidi.push_back(midiBinding(at(holder, "Send")));
            m.sendSourceXml.push_back(std::make_shared<const std::string>(holder->toString().toStdString()));
        }
    return m;
}

Track readTrack(const XmlElement* x, TrackKind kind, std::vector<std::string>& warnings)
{
    Track t;
    t.sourceXml = std::make_shared<const std::string>(x->toString().toStdString());
    t.kind = kind;
    t.id = x->getIntAttribute("Id", -1);
    t.name = str(x, "Name/UserName").toStdString();
    t.nameIsAutomatic = t.name.empty();
    if (t.name.empty())
        t.name = str(x, "Name/EffectiveName").toStdString();
    t.colorIndex = integer(x, "Color", 0);
    t.groupId = integer(x, "TrackGroupId", -1);
    t.folded = ! boolean(x, "TrackUnfolded", true);

    const auto* chain = at(x, "DeviceChain");
    const auto* input = at(chain, kind == TrackKind::Midi ? "MidiInputRouting" : "AudioInputRouting");
    t.inputRoutingPresent = input != nullptr;
    t.inputRouting = str(input, "Target").toStdString();
    t.inputUpperDisplay = str(input, "UpperDisplayString").toStdString();
    t.inputLowerDisplay = str(input, "LowerDisplayString").toStdString();
    t.monitoringPresent = at(chain, "MainSequencer/MonitoringEnum") != nullptr;
    t.monitoring = integer(chain, "MainSequencer/MonitoringEnum", 1);
    t.recorderTakeCounter = integer(chain, "MainSequencer/Recorder/TakeCounter", 0);
    t.outputRouting = str(chain, "AudioOutputRouting/Target", "AudioOut/Main").toStdString();
    t.trackDelayMs = num(x, "TrackDelay/Value", 0.0);
    if (boolean(x, "TrackDelay/IsValueSampleBased", false))
        t.trackDelayMs = 0.0; // sample-based delays are not converted (no sample rate here); rare
    t.mixer = readMixer(at(chain, "Mixer"));
    t.devices = readDevices(at(chain, "DeviceChain/Devices"), warnings);
    t.automation = readEnvelopes(at(x, "AutomationEnvelopes/Envelopes"));

    if (const auto* slots = at(chain, "MainSequencer/ClipSlotList"))
        for (const auto* slot : slots->getChildWithTagNameIterator("ClipSlot"))
        {
            auto clip = readClipIn(at(slot, "ClipSlot/Value"));
            if (clip) clip->xmlId = (int) t.slots.size();
            t.slots.push_back(ClipSlot{std::move(clip)});
        }

    if (const auto* sonora = at(x, "SonoraTrack"); sonora && sonora->getIntAttribute("Version") == 1)
    {
        t.hidden = sonora->getBoolAttribute("Hidden");
        t.deactivated = sonora->getBoolAttribute("Deactivated");
    }
    if (const auto* freeze = at(x, "SonoraFreeze"); freeze && freeze->getIntAttribute("Version") == 1)
    {
        t.frozen = true; t.frozenSlots.resize(t.slots.size());
        for (const auto* render : freeze->getChildWithTagNameIterator("Render"))
        {
            const auto* audio = at(render, "AudioClip");
            if (! audio) continue;
            auto clip = readAudioClip(audio);
            const int scene = render->getIntAttribute("Scene", -1);
            if (clip.currentEnd <= clip.currentStart || clip.sampleRate <= 0 || clip.lengthSamples <= 0) continue;
            if (scene < 0) t.frozenArrangement.push_back(std::move(clip));
            else if ((size_t) scene < t.frozenSlots.size()) t.frozenSlots[(size_t) scene].clip = std::move(clip);
        }
    }
    // MIDI tracks keep arrangement clips under ClipTimeable, audio tracks under Sample.
    for (const char* path : {"MainSequencer/ClipTimeable/ArrangerAutomation/Events",
                             "MainSequencer/Sample/ArrangerAutomation/Events"})
        if (const auto* events = at(chain, path))
        {
            int ordinal = 0; // position among the clip elements: Live 11.0 sets can repeat clip Ids
            for (const auto* clipX : events->getChildIterator())
            {
                if (clipX->hasTagName("MidiClip"))
                    t.arrangementClips.push_back(readMidiClip(clipX));
                else if (clipX->hasTagName("AudioClip"))
                    t.arrangementClips.push_back(readAudioClip(clipX));
                else
                    continue;
                t.arrangementClips.back().xmlId = ordinal++;
            }
        }

    if (const auto* container = at(x, "TakeLanes"))
    {
        t.takeLanesSourceXml = std::make_shared<const std::string>(container->toString().toStdString());
        if (const auto* lanes = at(container, "TakeLanes"))
            for (const auto* laneXml : lanes->getChildWithTagNameIterator("TakeLane"))
            {
                TakeLane lane;
                lane.id = laneXml->getIntAttribute("Id", -1);
                lane.height = integer(laneXml, "Height", 51);
                lane.name = str(laneXml, "Name").toStdString();
                lane.annotation = str(laneXml, "Annotation").toStdString();
                lane.audition = boolean(laneXml, "Audition", false);
                lane.sourceXml = std::make_shared<const std::string>(laneXml->toString().toStdString());
                if (const auto* events = at(laneXml, "ClipAutomation/Events"))
                    for (const auto* clip : events->getChildIterator())
                    {
                        if (clip->hasTagName("AudioClip")) lane.clips.push_back(readAudioClip(clip));
                        else if (clip->hasTagName("MidiClip")) lane.clips.push_back(readMidiClip(clip));
                    }
                t.nextTakeLaneId = std::max(t.nextTakeLaneId, lane.id + 1);
                t.takeLanes.push_back(std::move(lane));
            }
    }
    if (const auto* controllers = at(chain, "MainSequencer/MidiControllers"))
    {
        t.midiControllersPresent = true;
        for (int index = 0; index < model::midiControllerTargetCount; ++index)
            if (const auto* target = controllers->getChildByName("ControllerTargets." + juce::String(index)))
                t.midiControllerTargets[(size_t) index] = target->getIntAttribute("Id", -1);
    }
    auto typeControllers = [&](std::vector<AutomationEnvelope>& envelopes) {
        for (auto& env : envelopes) if (env.targetId >= 0)
            for (int index = 0; index < 130; ++index)
                if (t.midiControllerTargets[(size_t) index] == env.targetId) { env.midiController = index; break; }
    };
    typeControllers(t.automation);
    for (auto& clip : t.arrangementClips) typeControllers(clip.envelopes);
    for (auto& slot : t.slots) if (slot.clip) typeControllers(slot.clip->envelopes);
    for (auto& lane : t.takeLanes) for (auto& clip : lane.clips) typeControllers(clip.envelopes);
    for (const auto& clip : t.arrangementClips) t.nextTakeId = std::max(t.nextTakeId, clip.takeId + 1);
    for (const auto& slot : t.slots) if (slot.clip) t.nextTakeId = std::max(t.nextTakeId, slot.clip->takeId + 1);
    for (const auto& lane : t.takeLanes) for (const auto& clip : lane.clips) t.nextTakeId = std::max(t.nextTakeId, clip.takeId + 1);
    std::sort(t.arrangementClips.begin(), t.arrangementClips.end(),
              [](const Clip& a, const Clip& b) { return a.currentStart < b.currentStart; });
    return t;
}

} // namespace

// ---------------------------------------------------------------- public API

std::vector<uint8_t> decodeHexBlob(const std::string& text)
{
    std::vector<uint8_t> out;
    out.reserve(text.size() / 2);
    int hi = -1;
    for (char ch : text)
    {
        int v;
        if (ch >= '0' && ch <= '9') v = ch - '0';
        else if (ch >= 'a' && ch <= 'f') v = ch - 'a' + 10;
        else if (ch >= 'A' && ch <= 'F') v = ch - 'A' + 10;
        else continue; // whitespace / newlines between rows
        if (hi < 0)
            hi = v;
        else
        {
            out.push_back((uint8_t) ((hi << 4) | v));
            hi = -1;
        }
    }
    return out;
}

AlsLoadResult parseAls(const XmlElement& root, const std::string& sourcePath)
{
    AlsLoadResult result;
    if (! root.hasTagName("Ableton"))
    {
        result.error = "Not an Ableton document (root is <" + root.getTagName().toStdString() + ">)";
        return result;
    }

    const auto* liveSet = root.getChildByName("LiveSet");
    if (liveSet == nullptr)
    {
        result.error = "No <LiveSet> element (is this a device preset .adv/.adg?)";
        return result;
    }

    auto song = std::make_unique<Song>();
    song->sourcePath = sourcePath;
    song->creator = root.getStringAttribute("Creator").toStdString();
    song->schemaVersion = root.getStringAttribute("MinorVersion").toStdString();

    if (const auto* tracks = liveSet->getChildByName("Tracks"))
        for (const auto* tx : tracks->getChildIterator())
        {
            const auto tag = tx->getTagName();
            if (tag == "AudioTrack")
                song->tracks.push_back(readTrack(tx, TrackKind::Audio, song->warnings));
            else if (tag == "MidiTrack")
                song->tracks.push_back(readTrack(tx, TrackKind::Midi, song->warnings));
            else if (tag == "GroupTrack")
                song->tracks.push_back(readTrack(tx, TrackKind::Group, song->warnings));
            else if (tag == "ReturnTrack")
                song->returns.push_back(readTrack(tx, TrackKind::Return, song->warnings));
            else
                song->warnings.push_back("Unknown track type: " + tag.toStdString());
        }

    // Live 12 calls it MainTrack; Live <= 11 used MasterTrack.
    const auto* mainX = liveSet->getChildByName("MainTrack");
    if (mainX == nullptr)
        mainX = liveSet->getChildByName("MasterTrack");
    if (mainX != nullptr)
    {
        song->main = readTrack(mainX, TrackKind::Main, song->warnings);
        song->main.name = "Main";
        song->tempo = num(mainX, "DeviceChain/Mixer/Tempo/Manual", 120.0);
        const auto [n, d] = decodeTimeSignature(integer(mainX, "DeviceChain/Mixer/TimeSignature/Manual", 201));
        song->timeSigNumerator = n;
        song->timeSigDenominator = d;
    }

    if (const auto* pre = liveSet->getChildByName("SendsPre"))
        for (const auto* value : pre->getChildWithTagNameIterator("SendPreBool"))
            song->sendPreXml.push_back(std::make_shared<const std::string>(value->toString().toStdString()));
    if (const auto* scenes = liveSet->getChildByName("Scenes"))
        for (const auto* sx : scenes->getChildWithTagNameIterator("Scene"))
        {
            Scene s;
            s.name = str(sx, "Name").toStdString();
            s.colorIndex = integer(sx, "Color", -1);
            if (boolean(sx, "IsTempoEnabled", false))
                s.tempo = num(sx, "Tempo", 120.0);
            song->scenes.push_back(std::move(s));
        }

    if (const auto* locators = at(liveSet, "Locators/Locators"))
        for (const auto* lx : locators->getChildWithTagNameIterator("Locator"))
            song->locators.push_back({num(lx, "Time"), str(lx, "Name").toStdString()});

    song->globalQuantisation = integer(liveSet, "GlobalQuantisation", 4);
    readScale(liveSet, song->scaleRoot, song->scaleName);
    song->inKey = boolean(liveSet, "InKey", false);
    song->nextPointeeId = integer(liveSet, "NextPointeeId", 1);
    std::function<void(const XmlElement*)> findTargetIds = [&](const auto* element) {
        if (element->hasTagName("AutomationTarget") || element->hasTagName("ModulationTarget") || element->hasTagName("Pointee"))
            song->nextPointeeId = std::max(song->nextPointeeId, element->getIntAttribute("Id", -1) + 1);
        for (const auto* descendant : element->getChildIterator()) findTargetIds(descendant);
    };
    findTargetIds(liveSet);

    if (const auto* tr = liveSet->getChildByName("Transport"))
    {
        song->loopOn = boolean(tr, "LoopOn", false);
        song->punchIn = boolean(tr, "PunchIn", false);
        song->punchOut = boolean(tr, "PunchOut", false);
        song->punchInPresent = tr->getChildByName("PunchIn") != nullptr;
        song->punchOutPresent = tr->getChildByName("PunchOut") != nullptr;
        song->loopStart = num(tr, "LoopStart", 0.0);
        song->loopLength = num(tr, "LoopLength", 16.0);
        song->currentTime = num(tr, "CurrentTime", 0.0);
    }

    // De-duplicate warnings, keep a count.
    std::sort(song->warnings.begin(), song->warnings.end());
    std::vector<std::string> compact;
    for (size_t i = 0; i < song->warnings.size();)
    {
        size_t j = i;
        while (j < song->warnings.size() && song->warnings[j] == song->warnings[i])
            ++j;
        compact.push_back(song->warnings[i] + (j - i > 1 ? " (x" + std::to_string(j - i) + ")" : ""));
        i = j;
    }
    song->warnings = std::move(compact);

    const auto sourceDirectory = juce::File::isAbsolutePath(juce::String(song->sourcePath))
                                     ? juce::File(song->sourcePath).getParentDirectory().getFullPathName().toStdString() : std::string{};
    std::function<void(std::vector<Device>&)> findDeviceIds = [&](auto& devices) {
        for (auto& device : devices)
        {
            song->nextDeviceId = std::max(song->nextDeviceId, device.id + 1);
            device.sourceDirectory = sourceDirectory;
            for (auto& sample : device.sampleParts) sample.sample.sourceDirectory = sourceDirectory;
            for (auto& chain : device.chains) findDeviceIds(chain.devices);
        }
    };
    for (auto& track : song->tracks) findDeviceIds(track.devices);
    for (auto& track : song->returns) findDeviceIds(track.devices);
    findDeviceIds(song->main.devices);
    for (auto& track : song->tracks)
    {
        for (auto& clip : track.frozenArrangement) clip.sample.sourceDirectory = sourceDirectory;
        for (auto& slot : track.frozenSlots) if (slot.clip) slot.clip->sample.sourceDirectory = sourceDirectory;
        for (auto& clip : track.arrangementClips)
        {
            song->savedClips[clip.uid] = {track.id, clip.xmlId, -1};
            clip.sample.sourceDirectory = sourceDirectory;
        }
        for (auto& lane : track.takeLanes)
            for (auto& clip : lane.clips) clip.sample.sourceDirectory = sourceDirectory;
        for (size_t slot = 0; slot < track.slots.size(); ++slot)
            if (auto& clip = track.slots[slot].clip)
            {
                song->savedClips[clip->uid] = {track.id, -1, (int) slot};
                clip->sample.sourceDirectory = sourceDirectory;
            }
    }
    result.song = std::move(song);
    return result;
}

std::unique_ptr<juce::XmlElement> readAlsDom(const juce::File& file, std::string& error)
{
    if (! file.existsAsFile())
    {
        error = "File not found: " + file.getFullPathName().toStdString();
        return nullptr;
    }
    juce::MemoryBlock raw;
    if (! file.loadFileAsData(raw))
    {
        error = "Cannot read file";
        return nullptr;
    }
    juce::String xmlText;
    const auto* bytes = static_cast<const uint8_t*>(raw.getData());
    if (raw.getSize() > 2 && bytes[0] == 0x1f && bytes[1] == 0x8b)
    {
        juce::MemoryInputStream compressed(raw, false);
        juce::GZIPDecompressorInputStream gz(&compressed, false, juce::GZIPDecompressorInputStream::gzipFormat);
        juce::MemoryOutputStream decoded;
        decoded.writeFromInputStream(gz, -1);
        xmlText = decoded.toUTF8();
    }
    else
    {
        xmlText = raw.toString();
    }
    juce::XmlDocument doc(xmlText);
    auto root = doc.getDocumentElement();
    if (root == nullptr)
        error = "XML parse error: " + doc.getLastParseError().toStdString();
    return root;
}

AlsLoadResult loadAls(const juce::File& file)
{
    AlsLoadResult result;
    if (! file.existsAsFile())
    {
        result.error = "File not found: " + file.getFullPathName().toStdString();
        return result;
    }

    juce::MemoryBlock raw;
    if (! file.loadFileAsData(raw))
    {
        result.error = "Cannot read file";
        return result;
    }

    juce::String xmlText;
    const auto* bytes = static_cast<const uint8_t*>(raw.getData());
    const bool gzipped = raw.getSize() > 2 && bytes[0] == 0x1f && bytes[1] == 0x8b;
    if (gzipped)
    {
        juce::MemoryInputStream compressed(raw, false);
        juce::GZIPDecompressorInputStream gz(&compressed, false, juce::GZIPDecompressorInputStream::gzipFormat);
        juce::MemoryOutputStream decoded;
        decoded.writeFromInputStream(gz, -1);
        xmlText = decoded.toUTF8();
    }
    else
    {
        xmlText = raw.toString();
    }

    juce::XmlDocument doc(xmlText);
    auto root = doc.getDocumentElement();
    if (root == nullptr)
    {
        result.error = "XML parse error: " + doc.getLastParseError().toStdString();
        return result;
    }
    return parseAls(*root, file.getFullPathName().toStdString());
}

namespace
{
std::mutex samplePathOverrideLock;
std::shared_ptr<const SamplePathOverride> samplePathOverride;
}

void setSamplePathOverride(std::shared_ptr<const SamplePathOverride> resolver)
{
    const std::lock_guard<std::mutex> lock(samplePathOverrideLock);
    samplePathOverride = std::move(resolver);
}

std::string resolveSamplePath(const Song& song, const FileRef& ref)
{
    std::shared_ptr<const SamplePathOverride> resolver;
    {
        const std::lock_guard<std::mutex> lock(samplePathOverrideLock);
        resolver = samplePathOverride;
    }
    if (resolver) return (*resolver)(song, ref);
    return resolveProjectSamplePath(song, ref);
}

std::string resolveProjectSamplePath(const Song& song, const FileRef& ref)
{
    const juce::File projectDir = ref.sourceDirectory.empty() ? juce::File(song.sourcePath).getParentDirectory()
                                                            : juce::File(ref.sourceDirectory);
    if (! ref.relativePath.empty())
    {
        auto f = projectDir.getChildFile(ref.relativePath);
        if (f.existsAsFile())
            return f.getFullPathName().toStdString();
    }
    if (! ref.absolutePath.empty() && juce::File::isAbsolutePath(ref.absolutePath))
    {
        juce::File f(ref.absolutePath);
        if (f.existsAsFile())
            return f.getFullPathName().toStdString();
    }
    return {};
}

model::Track createGroupTrack(const model::Song& song, const std::string& name, int colourIndex, int& pointeeEnd)
{
    int id = 0;
    for (const auto& t : song.tracks) id = std::max(id, t.id + 1);
    for (const auto& t : song.returns) id = std::max(id, t.id + 1);
    const auto xml = groupTrackXml(id, name, colourIndex, (int) song.scenes.size(), (int) song.returns.size(),
                                   std::max(1, song.nextPointeeId), pointeeEnd);
    auto element = juce::XmlDocument::parse(juce::String(xml));
    std::vector<std::string> warnings;
    model::Track track = element ? readTrack(element.get(), model::TrackKind::Group, warnings) : model::Track{};
    track.kind = model::TrackKind::Group;
    track.id = id;
    track.name = name;
    track.sourceXml = std::make_shared<const std::string>(xml);
    return track;
}

model::Track createMidiTrack(const model::Song& song, const std::string& name, int colourIndex, int& pointeeEnd)
{
    int id = std::max(0, song.main.id + 1);
    for (const auto& t : song.tracks) id = std::max(id, t.id + 1);
    for (const auto& t : song.returns) id = std::max(id, t.id + 1);
    const auto xml = midiTrackXml(id, name, colourIndex, static_cast<int>(song.scenes.size()),
        static_cast<int>(song.returns.size()), std::max(1, song.nextPointeeId), pointeeEnd);
    auto element = juce::XmlDocument::parse(juce::String(xml));
    std::vector<std::string> warnings;
    auto track = element ? readTrack(element.get(), model::TrackKind::Midi, warnings) : model::Track{};
    track.kind = model::TrackKind::Midi; track.id = id; track.name = name;
    track.sourceXml = std::make_shared<const std::string>(xml);
    return track;
}

model::Track createAudioTrack(const model::Song& song, const std::string& name, int colourIndex, int trackId, int firstPointeeId, int& pointeeEnd)
{
    int id = trackId;
    if (id < 0)
    {
        id = 0;
        for (const auto& t : song.tracks) id = std::max(id, t.id + 1);
        for (const auto& t : song.returns) id = std::max(id, t.id + 1);
    }
    const auto xml = audioTrackXml(id, name, colourIndex, (int) song.scenes.size(), (int) song.returns.size(),
                                   firstPointeeId >= 0 ? firstPointeeId : std::max(1, song.nextPointeeId), pointeeEnd);
    auto element = juce::XmlDocument::parse(juce::String(xml));
    std::vector<std::string> warnings;
    model::Track track = element ? readTrack(element.get(), model::TrackKind::Audio, warnings) : model::Track{};
    track.kind = model::TrackKind::Audio;
    track.id = id;
    track.name = name;
    track.sourceXml = std::make_shared<const std::string>(xml);
    return track;
}

model::ReturnInsert createReturnTrack(const model::Song& song)
{
    model::ReturnInsert insert;
    int id = 0;
    for (const auto& t : song.tracks) id = std::max(id, t.id + 1);
    for (const auto& t : song.returns) id = std::max(id, t.id + 1);
    const auto count = song.returns.size();
    // Live letters returns A..L; past Z Sonora numbers them.
    const std::string name = count < 26 ? std::string(1, (char) ('A' + count)) + "-Return" : "R" + std::to_string(count + 1) + "-Return";
    const bool returnsHaveHolders = std::all_of(song.returns.begin(), song.returns.end(),
                                                [count](const auto& r) { return r.mixer.sends.size() == count; });
    int next = std::max(1, song.nextPointeeId), end = next;
    for (const auto& t : song.tracks)
    {
        insert.holderTargets.push_back(next); // sendHolderXml: the Send's AutomationTarget takes the first id, ModulationTarget the next
        insert.holders.push_back(std::make_shared<const std::string>(sendHolderXml((int) t.mixer.sends.size(), true, next, end)));
        next = end;
    }
    for (const auto& r : song.returns)
    {
        if (! returnsHaveHolders) { insert.holders.push_back(nullptr); insert.holderTargets.push_back(-1); continue; }
        insert.holderTargets.push_back(next);
        insert.holders.push_back(std::make_shared<const std::string>(sendHolderXml((int) r.mixer.sends.size(), false, next, end)));
        next = end;
    }
    const auto xml = returnTrackXml(id, name, count % 2 == 0 ? 17 : 7, returnsHaveHolders ? (int) count + 1 : 0, next, end);
    insert.pointeeEnd = end;
    auto element = juce::XmlDocument::parse(juce::String(xml));
    std::vector<std::string> warnings;
    insert.track = element ? readTrack(element.get(), model::TrackKind::Return, warnings) : model::Track{};
    insert.track.kind = model::TrackKind::Return;
    insert.track.id = id;
    insert.track.name = name;
    insert.track.sourceXml = std::make_shared<const std::string>(xml);
    insert.track.mixer.sendsEdited = false;
    insert.sendPre = std::make_shared<const std::string>(
        "<SendPreBool Id=\"" + std::to_string(count) + "\" Value=\"false\" />");
    return insert;
}

} // namespace sonora::io
