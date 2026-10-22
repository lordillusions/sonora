#include "io/als/AlsWriter.h"
#include "core/Scales.h"

#include "io/als/AlsReader.h"
#include "io/als/AlsTemplates.h"
#include "core/Edit.h"
#include "core/TimeUtils.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <set>
#include <tuple>

namespace sonora::io
{

using juce::XmlElement;

namespace
{
// ---------------------------------------------------------------- small DOM helpers

XmlElement* child(XmlElement* e, const char* name)
{
    return e != nullptr ? e->getChildByName(name) : nullptr;
}

// "A/B/C" -> element, or nullptr.
XmlElement* at(XmlElement* e, const juce::String& path)
{
    juce::StringArray parts;
    parts.addTokens(path, "/", "");
    for (const auto& p : parts)
    {
        e = child(e, p.toRawUTF8());
        if (e == nullptr)
            return nullptr;
    }
    return e;
}

// The shortest decimal text that parses back to exactly `v` (what a careful writer should emit).
juce::String num(double v)
{
    for (int precision : {15, 16, 17})
    {
        const auto text = juce::String::formatted("%.*g", precision, v);
        const double back = text.getDoubleValue();
        if (! (back < v) && ! (back > v)) // exactly equal
            return text;
    }
    return juce::String::formatted("%.17g", v);
}

// Sets <X Value="..."/> below `parent`/`path` (the element must exist). Returns whether it changed.
bool setValueAttr(XmlElement* e, const juce::String& value)
{
    if (e == nullptr || e->getStringAttribute("Value") == value)
        return false;
    e->setAttribute("Value", value);
    return true;
}

bool setNumber(XmlElement* e, double v)
{
    if (e == nullptr)
        return false;
    if (e->hasAttribute("Value") && std::fabs(e->getDoubleAttribute("Value") - v) < 1e-9)
        return false;
    e->setAttribute("Value", num(v));
    return true;
}

// <ScaleInformation><Root/><Name/></ScaleInformation> plus the Scale Mode flag (`inKeyTag`: InKey on the set, IsInKey on
// clips; UI-30b). Existing elements follow the model; missing ones are only created for non-default values.
void patchScale(XmlElement& owner, int root, int name, const char* inKeyTag, bool inKey)
{
    auto* scale = owner.getChildByName("ScaleInformation");
    if (scale == nullptr && (root != 0 || name != 0)) scale = owner.createNewChildElement("ScaleInformation");
    if (auto* oldRoot = scale ? scale->getChildByName("RootNote") : nullptr)
    {
        // Live 11 layout: <RootNote/> and the scale's name. Keep it; rewrite the name only when the scale changed.
        setNumber(oldRoot, root);
        auto* oldName = scale->getChildByName("Name");
        if (oldName == nullptr) oldName = scale->createNewChildElement("Name");
        if (std::max(0, model::scaleIndexByName(oldName->getStringAttribute("Value").toStdString())) != name)
            setValueAttr(oldName, juce::String(std::string(model::scaleAt(name).name)));
    }
    else if (scale != nullptr)
        for (const auto& [tag, value] : {std::pair{"Root", root}, std::pair{"Name", name}})
        {
            auto* element = scale->getChildByName(tag);
            if (element == nullptr) element = scale->createNewChildElement(tag);
            setNumber(element, value);
        }
    if (auto* flag = owner.getChildByName(inKeyTag))
        setValueAttr(flag, inKey ? "true" : "false");
    else if (inKey)
        owner.createNewChildElement(inKeyTag)->setAttribute("Value", "true");
}

// <Param><Manual Value="..."/></Param>: numbers or booleans (written the way the file already writes them).
void setManual(XmlElement* param, double v)
{
    auto* m = child(param, "Manual");
    if (m == nullptr)
        return;
    const auto current = m->getStringAttribute("Value");
    if (current.equalsIgnoreCase("true") || current.equalsIgnoreCase("false"))
    {
        const bool b = v >= 0.5;
        if (current.equalsIgnoreCase("true") != b)
            m->setAttribute("Value", b ? "true" : "false");
    }
    else
    {
        setNumber(m, v);
    }
}

// ---------------------------------------------------------------- devices

void patchDevice(XmlElement* x, const model::Device& d)
{
    if (d.sidechain.edited)
    {
        setManual(at(x, "SideChain/OnOff"), d.sidechain.on ? 1.0 : 0.0);
        if (auto* target = at(x, "SideChain/RoutedInput/Routable/Target"))
            setValueAttr(target, d.sidechain.sourceTrackId < 0 ? "AudioIn/None"
                           : juce::String("AudioIn/Track.") + juce::String(d.sidechain.sourceTrackId)
                                 + (d.sidechain.tap == model::Sidechain::Tap::PreFx ? "/PreFxOut" : d.sidechain.tap == model::Sidechain::Tap::PostFx ? "/PostFxOut" : "/PostMixerOut"));
    }

    if (x == nullptr)
        return;
    setManual(child(x, "On"), d.enabled ? 1.0 : 0.0);
    if (d.type == "SonoraWavetable")
    {
        auto* user = child(x, "UserName");
        if (! user) user = x->createNewChildElement("UserName");
        user->setAttribute("Value", juce::String(d.displayName));
    }
    if (d.nativeContent)
    {
        const auto& content = *d.nativeContent;
        auto* native = child(x, "SonoraState");
        if (! native) native = x->createNewChildElement("SonoraState");
        native->setAttribute("Format", juce::String(content.format));
        native->setAttribute("SourceDirectory", juce::String(content.sourceDirectory));
        const auto patch = [&](const char* tag, const std::vector<uint8_t>& bytes)
        {
            auto* element = child(native, tag);
            if (element && decodeHexBlob(element->getAllSubText().toStdString()) == bytes) return;
            if (! element) element = native->createNewChildElement(tag);
            element->deleteAllChildElements();
            element->addTextElement(juce::String::toHexString(bytes.data(), (int) bytes.size(), 0).toUpperCase());
        };
        patch("ProcessorState", content.data);
        patch("ControllerState", content.controllerData);
    }
    else if (d.nativeContentEdited)
        if (auto* native = child(x, "SonoraState")) x->removeChildElement(native, true);
    if (d.kind == model::DeviceKind::PluginVst3 && d.plugin)
        if (auto* preset = at(x, "PluginDesc/Vst3PluginInfo/Preset/Vst3Preset"))
        {
            auto state = [&](const char* tag, const std::vector<uint8_t>& bytes) {
                auto* e = child(preset, tag);
                if (e && decodeHexBlob(e->getAllSubText().toStdString()) == bytes) return;
                if (! e) e = preset->createNewChildElement(tag);
                e->deleteAllChildElements();
                e->addTextElement(juce::String::toHexString(bytes.data(), (int) bytes.size(), 0).toUpperCase());
            };
            state("ProcessorState", d.plugin->state);
            state("ControllerState", d.plugin->controllerState);
        }
    if (auto* list = child(x, "ParameterList"))
        for (auto* parameter : list->getChildWithTagNameIterator("PluginFloatParameter"))
            if (const auto* id = child(parameter, "ParameterId"))
                for (const auto& stored : d.pluginParams)
                    if (d.kind == model::DeviceKind::PluginVst3 && stored.valueEdited && stored.parameterId >= 0
                        && id->getIntAttribute("Value", -1) == stored.parameterId)
                        setManual(child(parameter, "ParameterValue"), stored.value);
    for (const auto& p : d.parameters)
    {
        if (p.path == "On")
            continue;
        auto* target = at(x, p.path);
        if (! target)
        {
            target = x;
            for (const auto& name : juce::StringArray::fromTokens(juce::String(p.path), "/", ""))
            {
                auto* next = target->getChildByName(name); target = next ? next : target->createNewChildElement(name);
            }
            auto* manual = target->createNewChildElement("Manual");
            manual->setAttribute("Value", p.booleanValue ? juce::String(p.value >= 0.5 ? "true" : "false") : num(p.value));
        }
        if (target)
        {
            setManual(target, p.value);
            if (p.macroMappingEdited && p.macroIndex >= 0)
            {
                auto* key = child(target, "KeyMidi"); if (! key) key = target->createNewChildElement("KeyMidi");
                auto leaf = [](XmlElement* parent, const char* name, double value) {
                    auto* e = child(parent, name); if (! e) e = parent->createNewChildElement(name); setNumber(e, value);
                };
                leaf(key, "Channel", 16); leaf(key, "NoteOrController", p.macroIndex);
                auto* note = child(key, "IsNote"); if (! note) note = key->createNewChildElement("IsNote"); note->setAttribute("Value", "false");
                auto* range = child(target, "MidiControllerRange"); if (! range) range = target->createNewChildElement("MidiControllerRange");
                leaf(range, "Min", p.rangeMin); leaf(range, "Max", p.rangeMax);
            }
        }
    }
    for (const auto& [key, value] : d.plain)
    {
        auto* target = at(x, key);
        if (! target)
        {
            target = x;
            for (const auto& name : juce::StringArray::fromTokens(juce::String(key), "/", ""))
            {
                auto* next = target->getChildByName(name); target = next ? next : target->createNewChildElement(name);
            }
        }
        const auto current = target->getStringAttribute("Value");
        if (current.equalsIgnoreCase("true") || current.equalsIgnoreCase("false"))
        {
            if (current.equalsIgnoreCase("true") != (value >= 0.5)) target->setAttribute("Value", value >= 0.5 ? "true" : "false");
        }
        else setNumber(target, value);
    }
    if (d.samplePartsEdited || d.chainsEdited)
    {
        const auto authored = juce::XmlDocument::parse(juce::String(deviceTemplate(d)));
        if (authored)
        {
            auto replace = [&](XmlElement* parent, const char* tag, const XmlElement* source) {
                if (! parent || ! source) return;
                if (auto* old = child(parent, tag)) parent->replaceChildElement(old, new XmlElement(*source));
                else parent->addChildElement(new XmlElement(*source));
            };
            if (d.samplePartsEdited)
            {
                auto* player = child(x, "Player");
                if (! player) player = x->createNewChildElement("Player");
                auto* map = child(player, "MultiSampleMap");
                if (! map) map = player->createNewChildElement("MultiSampleMap");
                replace(map, "SampleParts", at(authored.get(), "Player/MultiSampleMap/SampleParts"));
            }
            if (d.chainsEdited) replace(x, "Branches", child(authored.get(), "Branches"));
        }
    }
    // Racks: recurse into the chains in order.
    if (auto* branches = child(x, "Branches"))
    {
        int ci = 0;
        for (auto* br : branches->getChildIterator())
        {
            if (ci >= (int) d.chains.size())
                break;
            const auto& chain = d.chains[(size_t) ci++];
            // chain mixer
            setManual(at(br, "MixerDevice/Volume"), chain.volume);
            setManual(at(br, "MixerDevice/Panorama"), chain.pan);
            setManual(at(br, "MixerDevice/Speaker"), chain.speakerOn ? 1.0 : 0.0);
            // devices of the chain: first <Devices> below the branch
            std::function<XmlElement*(XmlElement*, int)> findDevices = [&](XmlElement* e, int depth) -> XmlElement* {
                if (e == nullptr || depth > 4)
                    return nullptr;
                for (auto* c : e->getChildIterator())
                {
                    if (c->hasTagName("Devices"))
                        return c;
                    if (auto* f = findDevices(c, depth + 1))
                        return f;
                }
                return nullptr;
            };
            if (auto* devices = findDevices(br, 0))
            {
                int di = 0;
                for (auto* dx : devices->getChildIterator())
                {
                    if (di >= (int) chain.devices.size())
                        break;
                    patchDevice(dx, chain.devices[(size_t) di++]);
                }
            }
        }
    }
}

// ---------------------------------------------------------------- clips

// True when the notes already in the XML are exactly `notes` (so an untouched clip is left byte-for-byte alone).
bool notesUnchanged(XmlElement* notesX, const std::vector<model::MidiNote>& notes)
{
    struct Key
    {
        int pitch;
        double start, duration, velocity, probability, releaseVelocity, velocityDeviation;
        bool enabled;
        bool operator<(const Key& o) const
        {
            return std::tie(pitch, start, duration, velocity, probability, releaseVelocity, velocityDeviation, enabled)
                   < std::tie(o.pitch, o.start, o.duration, o.velocity, o.probability, o.releaseVelocity, o.velocityDeviation, o.enabled);
        }
    };
    std::vector<Key> inFile, inModel;
    if (auto* keyTracks = child(notesX, "KeyTracks"))
        for (auto* kt : keyTracks->getChildWithTagNameIterator("KeyTrack"))
        {
            const int pitch = at(kt, "MidiKey") ? at(kt, "MidiKey")->getIntAttribute("Value", 60) : 60;
            if (auto* inner = child(kt, "Notes"))
                for (auto* n : inner->getChildWithTagNameIterator("MidiNoteEvent"))
                    inFile.push_back({pitch, n->getDoubleAttribute("Time"), n->getDoubleAttribute("Duration", 0.25),
                                      (float) n->getDoubleAttribute("Velocity", 100.0),
                                      (float) n->getDoubleAttribute("Probability", 1.0),
                                      (float) n->getDoubleAttribute("OffVelocity", 64.0),
                                      (float) n->getDoubleAttribute("VelocityDeviation", 0.0),
                                      ! n->getStringAttribute("IsEnabled", "true").equalsIgnoreCase("false")});
        }
    for (const auto& n : notes)
        inModel.push_back({n.pitch, n.start, n.duration, n.velocity, n.probability, n.releaseVelocity, n.velocityDeviation, n.enabled});
    if (inFile.size() != inModel.size())
        return false;
    std::sort(inFile.begin(), inFile.end());
    std::sort(inModel.begin(), inModel.end());
    for (size_t i = 0; i < inFile.size(); ++i)
        if (inFile[i].pitch != inModel[i].pitch || inFile[i].enabled != inModel[i].enabled
            || std::fabs(inFile[i].start - inModel[i].start) > 1e-9 || std::fabs(inFile[i].duration - inModel[i].duration) > 1e-9
            || std::fabs(inFile[i].velocity - inModel[i].velocity) > 1e-6
            || std::fabs(inFile[i].probability - inModel[i].probability) > 1e-6
            || std::fabs(inFile[i].releaseVelocity - inModel[i].releaseVelocity) > 1e-6
            || std::fabs(inFile[i].velocityDeviation - inModel[i].velocityDeviation) > 1e-6)
            return false;
    return true;
}

void setNotesXml(XmlElement* clipX, const std::vector<model::MidiNote>& notes, bool force = false)
{
    auto* notesX = child(clipX, "Notes");
    if (notesX == nullptr || (! force && notesUnchanged(notesX, notes)))
        return;

    // Note ids: keep the ones the file gave us, number new notes from the generator.
    int nextId = 1;
    if (auto* gen = at(notesX, "NoteIdGenerator/NextId"))
        nextId = gen->getIntAttribute("Value", 1);
    int maxId = 0;
    for (const auto& n : notes)
        maxId = std::max(maxId, n.noteId);
    nextId = std::max(nextId, maxId + 1);

    std::map<int, std::vector<const model::MidiNote*>> byPitch;
    for (const auto& n : notes)
        byPitch[n.pitch].push_back(&n);

    auto* keyTracks = child(notesX, "KeyTracks");
    if (keyTracks == nullptr)
    {
        keyTracks = new XmlElement("KeyTracks");
        notesX->insertChildElement(keyTracks, 0);
    }
    keyTracks->deleteAllChildElements();
    int kt = 0;
    for (const auto& [pitch, list] : byPitch)
    {
        auto* track = new XmlElement("KeyTrack");
        track->setAttribute("Id", kt++);
        auto* inner = new XmlElement("Notes");
        for (const auto* n : list)
        {
            auto* e = new XmlElement("MidiNoteEvent");
            e->setAttribute("Time", num(n->start));
            e->setAttribute("Duration", num(n->duration));
            e->setAttribute("Velocity", num(n->velocity));
            e->setAttribute("VelocityDeviation", num(n->velocityDeviation));
            e->setAttribute("OffVelocity", num(n->releaseVelocity));
            e->setAttribute("Probability", num(n->probability));
            e->setAttribute("IsEnabled", n->enabled ? "true" : "false");
            e->setAttribute("NoteId", n->noteId >= 0 ? n->noteId : nextId++);
            inner->addChildElement(e);
        }
        track->addChildElement(inner);
        auto* key = new XmlElement("MidiKey");
        key->setAttribute("Value", pitch);
        track->addChildElement(key);
        keyTracks->addChildElement(track);
    }
    if (auto* gen = at(notesX, "NoteIdGenerator/NextId"))
        gen->setAttribute("Value", nextId);
}

void patchEnvelopes(XmlElement*, const std::vector<model::AutomationEnvelope>&, const char*, const char*);

void patchClip(XmlElement* x, const model::Clip& c)
{
    if (auto* alias = child(x, "SonoraAlias")) x->removeChildElement(alias, true);
    if (c.linkGroup >= 0) x->createNewChildElement("SonoraAlias")->setAttribute("Group", c.linkGroup);
    if (c.takeId >= 0)
    {
        auto* id = child(x, "TakeId");
        if (! id) id = x->createNewChildElement("TakeId");
        setNumber(id, c.takeId);
    }
    if (c.stripSourceEnvelopes)
        if (auto* envelopes = at(x, "Envelopes/Envelopes")) envelopes->deleteAllChildElements();
    patchEnvelopes(x, c.envelopes, "Envelopes", "ClipEnvelope");
    if (std::fabs(x->getDoubleAttribute("Time") - c.currentStart) > 1e-9)
        x->setAttribute("Time", num(c.currentStart)); // untouched clips keep the file's own text
    setNumber(child(x, "CurrentStart"), c.currentStart);
    setNumber(child(x, "CurrentEnd"), c.currentEnd);
    if (auto* loop = child(x, "Loop"))
    {
        setNumber(child(loop, "LoopStart"), c.loop.loopStart);
        setNumber(child(loop, "LoopEnd"), c.loop.loopEnd);
        setNumber(child(loop, "StartRelative"), c.loop.startRelative);
        setValueAttr(child(loop, "LoopOn"), c.loop.enabled ? "true" : "false");
        if (c.resetSourceMarkers)
            for (const auto& [tag, number] : std::initializer_list<std::pair<const char*, double>>{
                     {"OutMarker", c.loop.loopEnd}, {"HiddenLoopStart", c.loop.loopStart}, {"HiddenLoopEnd", c.loop.loopEnd}})
            {
                auto* marker = child(loop, tag);
                if (! marker) marker = loop->createNewChildElement(tag);
                setNumber(marker, number);
            }
    }
    setValueAttr(child(x, "Name"), juce::String::fromUTF8(c.name.c_str()));
    setNumber(child(x, "Color"), c.colorIndex);
    setValueAttr(child(x, "Disabled"), c.disabled ? "true" : "false");
    if (auto* analyzed = child(x, "SonoraTempoAnalyzed")) setValueAttr(analyzed, c.tempoAnalyzed ? "true" : "false");
    else if (c.tempoAnalyzed && c.resetSourceMarkers) setValueAttr(x->createNewChildElement("SonoraTempoAnalyzed"), "true");
    {
        // Launch settings: existing elements follow the model; missing ones are created only for non-default values.
        auto patchOrCreateValue = [&](const char* tag, double value, double defaultValue) {
            if (auto* element = child(x, tag)) { if (std::fabs(element->getDoubleAttribute("Value") - value) > 1e-9) setNumber(element, value); }
            else if (std::fabs(value - defaultValue) > 1e-9) setNumber(x->createNewChildElement(tag), value);
        };
        patchOrCreateValue("LaunchMode", c.launchMode, 0.0);
        patchOrCreateValue("LaunchQuantisation", c.launchQuantisation, 0.0);
        patchOrCreateValue("VelocityAmount", c.velocityAmount, 0.0);
        if (auto* legato = child(x, "Legato"))
        {
            if (legato->getStringAttribute("Value").equalsIgnoreCase("true") != c.legato) setValueAttr(legato, c.legato ? "true" : "false");
        }
        else if (c.legato)
            setValueAttr(x->createNewChildElement("Legato"), "true");
        patchScale(*x, c.scaleRoot, c.scaleName, "IsInKey", c.inKey);
        if (auto* signature = at(x, "TimeSignature/TimeSignatures/RemoteableTimeSignature"))
        {
            if (auto* numerator = child(signature, "Numerator")) { if (numerator->getIntAttribute("Value") != c.timeSigNumerator) setNumber(numerator, c.timeSigNumerator); }
            if (auto* denominator = child(signature, "Denominator")) { if (denominator->getIntAttribute("Value") != c.timeSigDenominator) setNumber(denominator, c.timeSigDenominator); }
        }
    }
    if (c.kind == model::Clip::Kind::Midi)
    {
        // Rebase expression XML on the immutable clip source before applying authored lanes. This also
        // removes a newly added lane after Save -> Undo -> Save, while retaining unknown original metadata.
        const auto* currentLists = at(x, "Notes/PerNoteEventStore/EventLists");
        if (c.noteExpressionsEdited || (currentLists && currentLists->getNumChildElements() > 0))
        {
            auto original = c.sourceXml ? juce::XmlDocument::parse(juce::String::fromUTF8(c.sourceXml->c_str())) : nullptr;
            if (auto* notes = child(x, "Notes"))
            {
                const auto* sourceStore = original ? at(original.get(), "Notes/PerNoteEventStore") : nullptr;
                auto* existing = child(notes, "PerNoteEventStore");
                if (sourceStore)
                {
                    if (! existing || existing->toString() != sourceStore->toString())
                    { if (existing) notes->removeChildElement(existing, true); notes->addChildElement(new XmlElement(*sourceStore)); }
                }
                else if (existing) notes->removeChildElement(existing, true);
            }
        }
        setNotesXml(x, c.notes, c.noteExpressionsEdited);
        if (c.stripSourceEnvelopes && c.noteExpressions.empty())
            if (auto* notes = child(x, "Notes"))
                if (auto* lists = at(notes, "PerNoteEventStore/EventLists")) lists->deleteAllChildElements();
        if (c.noteExpressionsEdited)
        {
            auto* notes = child(x, "Notes");
            if (notes)
            {
                auto* store = child(notes, "PerNoteEventStore"); if (! store) store = notes->createNewChildElement("PerNoteEventStore");
                auto* lists = child(store, "EventLists"); if (! lists) lists = store->createNewChildElement("EventLists");
                int index = 0;
                for (const auto* list : lists->getChildIterator()) index = std::max(index, list->getIntAttribute("Id", -1) + 1);
                for (const auto& expression : c.noteExpressions)
                {
                    if (std::none_of(c.notes.begin(), c.notes.end(), [&](const auto& note) { return note.noteId == expression.noteId; })) continue;
                    XmlElement* list = nullptr;
                    for (auto* candidate : lists->getChildWithTagNameIterator("PerNoteEventList"))
                        if (candidate->getIntAttribute("NoteId", -1) == expression.noteId && candidate->getIntAttribute("CC", -999) == expression.cc) { list = candidate; break; }
                    if (list && ! expression.edited) continue;
                    if (! list) { list = lists->createNewChildElement("PerNoteEventList"); list->setAttribute("Id", index++); }
                    list->setAttribute("NoteId", expression.noteId); list->setAttribute("CC", expression.cc);
                    auto* events = child(list, "Events"); if (! events) events = list->createNewChildElement("Events");
                    events->deleteAllChildElements();
                    for (const auto& point : expression.points)
                    {
                        auto* event = events->createNewChildElement("PerNoteEvent");
                        event->setAttribute("TimeOffset", num(point.time)); event->setAttribute("Value", num(point.value));
                        event->setAttribute("CurveControl1X", num(point.x1)); event->setAttribute("CurveControl1Y", num(point.y1));
                        event->setAttribute("CurveControl2X", num(point.x2)); event->setAttribute("CurveControl2Y", num(point.y2));
                    }
                }
            }
        }
    }
    else
    {
        setValueAttr(child(x, "IsWarped"), c.warped ? "true" : "false");
        // Existing elements are always rewritten; a missing one is created only for a non-default value.
        auto patchOrCreate = [&](const char* tag, double value, double defaultValue) {
            if (auto* element = child(x, tag)) setNumber(element, value);
            else if (std::fabs(value - defaultValue) > 1e-9) setNumber(x->createNewChildElement(tag), value);
        };
        patchOrCreate("WarpMode", (double) (int) c.warpMode, 0.0);
        patchOrCreate("PitchCoarse", (double) c.pitchCoarse, 0.0);
        patchOrCreate("PitchFine", c.pitchFine, 0.0);
        patchOrCreate("SampleVolume", c.gain, 1.0);
        // Warp-mode settings: patch existing elements when they differ; create missing ones only for non-default values.
        const struct { const char* tag; double value, defaultValue; } settings[] = {
            {"TransientResolution", (double) c.transientResolution, 6.0}, {"TransientLoopMode", (double) c.transientLoopMode, 2.0},
            {"TransientEnvelope", c.transientEnvelope, 100.0}, {"GranularityTones", c.granularityTones, 30.0},
            {"GranularityTexture", c.granularityTexture, 65.0}, {"FluctuationTexture", c.fluctuationTexture, 25.0},
            {"ComplexProFormants", c.complexProFormants, 100.0}, {"ComplexProEnvelope", c.complexProEnvelope, 128.0}};
        for (const auto& setting : settings)
        {
            auto* element = child(x, setting.tag);
            if (element != nullptr)
            {
                if (std::fabs(element->getDoubleAttribute("Value") - setting.value) > 1e-9)
                    setNumber(element, setting.value);
            }
            else if (std::fabs(setting.value - setting.defaultValue) > 1e-9)
                setNumber(x->createNewChildElement(setting.tag), setting.value);
        }
        // Sample box switches: patch existing elements; create missing ones only for non-default values.
        for (const auto& [tag, value, defaultValue] : std::initializer_list<std::tuple<const char*, bool, bool>>{
                 {"Fade", c.clipFade, true}, {"Ram", c.ram, false}, {"HiQ", c.hiQ, true}})
        {
            if (auto* element = child(x, tag))
            { if (element->getStringAttribute("Value").equalsIgnoreCase("true") != value) setValueAttr(element, value ? "true" : "false"); }
            else if (value != defaultValue)
                setValueAttr(x->createNewChildElement(tag), value ? "true" : "false");
        }
        // Fades: patch the existing <Fades>; author it (in Live's field order) only when a fade was set and the clip has none.
        if (auto* fades = child(x, "Fades"))
        {
            auto patchLength = [&](const char* tag, double value) {
                auto* element = child(fades, tag);
                if (! element && std::abs(value) > 1e-9) element = fades->createNewChildElement(tag);
                if (element && std::fabs(element->getDoubleAttribute("Value") - value) > 1e-9) setNumber(element, value);
            };
            auto patchFlag = [&](const char* tag, bool value) {
                if (auto* element = child(fades, tag))
                    if (element->getStringAttribute("Value").equalsIgnoreCase("true") != value) setValueAttr(element, value ? "true" : "false");
            };
            patchLength("FadeInLength", c.fadeIn);
            patchLength("FadeOutLength", c.fadeOut);
            patchLength("FadeInCurveSkew", c.fadeInCurve);
            patchLength("FadeOutCurveSkew", c.fadeOutCurve);
            patchFlag("IsDefaultFadeIn", c.fadeInDefault);
            patchFlag("IsDefaultFadeOut", c.fadeOutDefault);
        }
        else if (c.fadeIn > 0.0 || c.fadeOut > 0.0 || ! c.fadeInDefault || ! c.fadeOutDefault)
        {
            auto* newFades = new XmlElement("Fades");
            auto add = [&](const char* tag, const juce::String& value) { newFades->createNewChildElement(tag)->setAttribute("Value", value); };
            add("FadeInLength", num(c.fadeIn));
            add("FadeOutLength", num(c.fadeOut));
            add("ClipFadesAreInitialized", "true");
            add("CrossfadeInState", "0");
            add("FadeInCurveSkew", num(c.fadeInCurve));
            add("FadeInCurveSlope", "0");
            add("FadeOutCurveSkew", num(c.fadeOutCurve));
            add("FadeOutCurveSlope", "0");
            add("IsDefaultFadeIn", c.fadeInDefault ? "true" : "false");
            add("IsDefaultFadeOut", c.fadeOutDefault ? "true" : "false");
            const auto index = [&] {
                int i = 0;
                for (auto* e : x->getChildIterator()) { if (e->hasTagName("PitchCoarse")) return i; ++i; }
                return -1;
            }();
            x->insertChildElement(newFades, index); // before PitchCoarse like Live; appended when the clip has no such element
        }
        auto* ref = at(x, "SampleRef/FileRef");
        const bool sampleReplaced = ref && child(ref, "Path") && child(ref, "Path")->getStringAttribute("Value").toStdString() != c.sample.absolutePath;
        if (sampleReplaced)
        {
            setValueAttr(child(ref, "Path"), juce::String::fromUTF8(c.sample.absolutePath.c_str()));
            setValueAttr(child(ref, "RelativePath"), juce::String::fromUTF8(c.sample.relativePath.c_str()));
            setNumber(child(ref, "RelativePathType"), c.sample.relativePathType);
            setValueAttr(child(ref, "LivePackName"), "");
            setValueAttr(child(ref, "LivePackId"), "");
            setNumber(child(ref, "OriginalFileSize"), 0);
            setNumber(child(ref, "OriginalCrc"), 0);
            setNumber(at(x, "SampleRef/DefaultDuration"), (double) c.lengthSamples);
            setNumber(at(x, "SampleRef/DefaultSampleRate"), c.sampleRate);
            setNumber(at(x, "SampleRef/LastModDate"), 0);
            if (auto* context = at(x, "SampleRef/SourceContext")) context->deleteAllChildElements();
            if (! c.warped)
            {
                auto* markers = child(x, "WarpMarkers");
                if (! markers) markers = x->createNewChildElement("WarpMarkers");
                markers->deleteAllChildElements();
                // Live requires warp anchors even with Warp off. Discard the old sample's
                // transient map and provide a neutral map for the replacement source.
                const double seconds = c.sampleRate > 0 ? (double) c.lengthSamples / c.sampleRate : c.loop.loopEnd;
                for (int i = 0; i < 2; ++i)
                {
                    auto* marker = markers->createNewChildElement("WarpMarker");
                    marker->setAttribute("Id", i);
                    marker->setAttribute("SecTime", i == 0 ? 0 : seconds);
                    marker->setAttribute("BeatTime", i == 0 ? 0 : seconds * 2);
                }
                if (auto* onsets = child(x, "Onsets")) onsets->deleteAllChildElements();
            }
        }
        if (! c.warpMarkers.empty() && (! sampleReplaced || c.warped))
        {
            auto* markers = child(x, "WarpMarkers");
            bool changed = markers == nullptr;
            size_t index = 0;
            if (markers)
                for (const auto* marker : markers->getChildWithTagNameIterator("WarpMarker"))
                {
                    changed |= index >= c.warpMarkers.size();
                    if (index < c.warpMarkers.size())
                        changed |= std::fabs(marker->getDoubleAttribute("SecTime") - c.warpMarkers[index].seconds) > 1e-9
                                   || std::fabs(marker->getDoubleAttribute("BeatTime") - c.warpMarkers[index].beats) > 1e-9;
                    ++index;
                }
            changed |= index != c.warpMarkers.size();
            if (changed)
            {
                if (! markers) markers = x->createNewChildElement("WarpMarkers");
                markers->deleteAllChildElements();
                for (size_t i = 0; i < c.warpMarkers.size(); ++i)
                {
                    auto* marker = markers->createNewChildElement("WarpMarker");
                    marker->setAttribute("Id", (int) i);
                    marker->setAttribute("SecTime", num(c.warpMarkers[i].seconds));
                    marker->setAttribute("BeatTime", num(c.warpMarkers[i].beats));
                }
                setValueAttr(child(x, "MarkersGenerated"), "false");
            }
        }
    }
}

XmlElement* arrangementEvents(XmlElement* track)
{
    auto* chain = child(track, "DeviceChain");
    if (auto* e = at(chain, "MainSequencer/ClipTimeable/ArrangerAutomation/Events"))
        return e;
    return at(chain, "MainSequencer/Sample/ArrangerAutomation/Events");
}

// ---------------------------------------------------------------- tracks

void patchMixer(XmlElement* mixer, const model::Mixer& m)
{
    if (mixer == nullptr)
        return;
    auto target = [&](const char* name, int id) {
        if (auto* element = at(mixer, juce::String(name) + "/AutomationTarget"); element && id >= 0)
            if (element->getIntAttribute("Id", -1) != id) element->setAttribute("Id", id);
    };
    target("Volume", m.volumeTargetId); target("Pan", m.panTargetId); target("Speaker", m.speakerTargetId);
    setManual(child(mixer, "Volume"), m.volume);
    setManual(child(mixer, "Pan"), m.pan);
    setManual(child(mixer, "Speaker"), m.speakerOn ? 1.0 : 0.0);
    setValueAttr(child(mixer, "SoloSink"), m.solo ? "true" : "false");
    if (m.sendsEdited)
    {
        auto* sends = child(mixer, "Sends");
        if (! sends) sends = mixer->createNewChildElement("Sends");
        sends->deleteAllChildElementsWithTagName("TrackSendHolder");
        for (size_t i = 0; i < m.sends.size(); ++i)
        {
            auto holder = i < m.sendSourceXml.size() && m.sendSourceXml[i] ? juce::XmlDocument::parse(juce::String(*m.sendSourceXml[i])) : nullptr;
            if (! holder) holder = std::make_unique<XmlElement>("TrackSendHolder");
            holder->setAttribute("Id", (int) i);
            if (! child(holder.get(), "Send"))
            {
                auto* send = holder->createNewChildElement("Send");
                send->createNewChildElement("Manual")->setAttribute("Value", m.sends[i]);
                if (i < m.sendTargetIds.size() && m.sendTargetIds[i] >= 0)
                    send->createNewChildElement("AutomationTarget")->setAttribute("Id", m.sendTargetIds[i]);
            }
            sends->addChildElement(holder.release());
        }
    }
    if (auto* sends = child(mixer, "Sends"))
    {
        size_t i = 0;
        for (auto* holder : sends->getChildWithTagNameIterator("TrackSendHolder"))
        {
            if (i >= m.sends.size())
                break;
            if (auto* automation = at(holder, "Send/AutomationTarget"); automation && i < m.sendTargetIds.size() && m.sendTargetIds[i] >= 0)
                if (automation->getIntAttribute("Id", -1) != m.sendTargetIds[i]) automation->setAttribute("Id", m.sendTargetIds[i]);
            setManual(at(holder, "Send"), m.sends[i++]);
        }
    }
}

struct ClipPlan
{
    XmlElement* events = nullptr;
    std::vector<std::pair<XmlElement*, const model::Clip*>> patch;
    std::vector<std::unique_ptr<XmlElement>> add;
    std::vector<XmlElement*> remove;
    std::vector<XmlElement*> replaced;
};

// Clip elements of every arrangement, by track id and position, as they are in the file.
using ClipIndex = std::map<int, std::vector<XmlElement*>>;
using SessionIndex = std::map<std::pair<int, int>, XmlElement*>;

SessionIndex indexSessionClips(XmlElement& liveSet)
{
    SessionIndex out;
    if (auto* tracks = child(&liveSet, "Tracks"))
        for (auto* track : tracks->getChildIterator())
            if (auto* slots = at(track, "DeviceChain/MainSequencer/ClipSlotList"))
            {
                int i = 0;
                for (auto* slot : slots->getChildWithTagNameIterator("ClipSlot"))
                {
                    if (auto* value = at(slot, "ClipSlot/Value"))
                        for (auto* clip : value->getChildIterator())
                            if (clip->hasTagName("MidiClip") || clip->hasTagName("AudioClip"))
                                out[{track->getIntAttribute("Id"), i}] = clip;
                    ++i;
                }
            }
    return out;
}

XmlElement* clipTemplate(const model::Clip& clip, int ownTrack, const ClipIndex& index, const SessionIndex& sessions)
{
    const int track = clip.cloneOfTrackId >= 0 ? clip.cloneOfTrackId : ownTrack;
    if (clip.cloneOfSessionSlot >= 0)
    {
        const auto it = sessions.find({track, clip.cloneOfSessionSlot});
        return it == sessions.end() ? nullptr : it->second;
    }
    const auto it = index.find(track);
    return it != index.end() && clip.cloneOfXmlId >= 0 && (size_t) clip.cloneOfXmlId < it->second.size()
               ? it->second[(size_t) clip.cloneOfXmlId] : nullptr;
}

std::unique_ptr<XmlElement> retainedClip(const model::Clip& clip, int track, const ClipIndex& index,
                                        const SessionIndex& sessions)
{
    if (clip.sourceXml)
        return juce::XmlDocument::parse(juce::String::fromUTF8(clip.sourceXml->c_str()));
    if (auto* source = clipTemplate(clip, track, index, sessions))
        return std::make_unique<XmlElement>(*source);
    return nullptr;
}

XmlElement* savedClip(const model::Song& song, const model::Clip& clip, int track, int slot,
                      const ClipIndex& index, const SessionIndex& sessions)
{
    const auto found = song.savedClips.find(clip.uid);
    if (found == song.savedClips.end() || found->second.trackId != track || found->second.sessionSlot != slot)
        return nullptr;
    if (slot >= 0)
    {
        const auto existing = sessions.find({track, slot});
        return existing == sessions.end() ? nullptr : existing->second;
    }
    const auto existing = index.find(track);
    const int ordinal = found->second.ordinal;
    return existing != index.end() && ordinal >= 0 && (size_t) ordinal < existing->second.size()
               ? existing->second[(size_t) ordinal] : nullptr;
}

ClipIndex indexClips(XmlElement& liveSet)
{
    ClipIndex out;
    if (auto* tracksX = liveSet.getChildByName("Tracks"))
        for (auto* tx : tracksX->getChildIterator())
            if (auto* events = arrangementEvents(tx))
                for (auto* e : events->getChildIterator())
                    if (e->hasTagName("MidiClip") || e->hasTagName("AudioClip"))
                        out[tx->getIntAttribute("Id", -1)].push_back(e);
    return out;
}

// Patch only changed lanes: untouched event attributes (including curves) survive verbatim.
void patchEnvelopes(XmlElement* track, const std::vector<model::AutomationEnvelope>& envelopes, const char* holderTag, const char* envelopeTag)
{
    if (track == nullptr) return;
    auto* parent = at(track, juce::String(holderTag) + "/Envelopes");
    if (parent == nullptr)
    {
        if (envelopes.empty()) return;
        auto* holder = child(track, holderTag);
        if (holder == nullptr) holder = track->createNewChildElement(holderTag);
        parent = holder->createNewChildElement("Envelopes");
    }
    std::vector<XmlElement*> removed;
    for (auto* e : parent->getChildWithTagNameIterator(envelopeTag))
        if (auto* target = at(e, "EnvelopeTarget/PointeeId"))
        {
            const int id = target->getIntAttribute("Value", -1);
            if (std::none_of(envelopes.begin(), envelopes.end(), [id](const auto& env) { return env.targetId == id; }))
                removed.push_back(e);
        }
    for (auto* e : removed) parent->removeChildElement(e, true);
    int nextId = 0;
    for (auto* e : parent->getChildIterator()) nextId = std::max(nextId, e->getIntAttribute("Id") + 1);
    for (const auto& env : envelopes)
    {
        XmlElement* target = nullptr;
        for (auto* e : parent->getChildIterator())
            if (auto* id = at(e, "EnvelopeTarget/PointeeId"))
                if (id->getIntAttribute("Value", -1) == env.targetId) { target = e; break; }
        if (target == nullptr)
        {
            target = parent->createNewChildElement(envelopeTag);
            target->setAttribute("Id", nextId++);
            target->createNewChildElement("EnvelopeTarget")->createNewChildElement("PointeeId")->setAttribute("Value", env.targetId);
            auto* automation = target->createNewChildElement("Automation");
            automation->createNewChildElement("Events");
            if (juce::String(envelopeTag) == "ClipEnvelope")
            {
                auto* transform = automation->createNewChildElement("AutomationTransformViewState");
                transform->createNewChildElement("IsTransformPending")->setAttribute("Value", "false");
                transform->createNewChildElement("TimeAndValueTransforms");
                target->createNewChildElement("LoopSlot")->createNewChildElement("Value");
                auto* scroller = target->createNewChildElement("ScrollerTimePreserver");
                scroller->createNewChildElement("LeftTime")->setAttribute("Value", "0");
                scroller->createNewChildElement("RightTime")->setAttribute("Value", "0");
            }
        }
        auto* automation = child(target, "Automation");
        if (automation == nullptr) automation = target->createNewChildElement("Automation");
        auto* events = child(automation, "Events");
        if (events == nullptr) events = automation->createNewChildElement("Events");
        std::vector<XmlElement*> old;
        for (auto* e : events->getChildIterator()) old.push_back(e);
        auto valueOf = [](const XmlElement* e) {
            return e->hasTagName("BoolEvent") ? (e->getBoolAttribute("Value") ? 1.0 : 0.0) : e->getDoubleAttribute("Value");
        };
        std::stable_sort(old.begin(), old.end(), [](const auto* a, const auto* b) { return a->getDoubleAttribute("Time") < b->getDoubleAttribute("Time"); });
        bool equal = old.size() == env.points.size();
        for (size_t i = 0; equal && i < old.size(); ++i)
            equal = std::abs(old[i]->getDoubleAttribute("Time") - env.points[i].time) < 1e-12 && std::abs(valueOf(old[i]) - env.points[i].value) < 1e-12;
        if (equal) continue;
        std::vector<std::unique_ptr<XmlElement>> replacements;
        for (size_t i = 0; i < env.points.size(); ++i)
        {
            const auto& p = env.points[i];
            const auto found = std::find_if(old.begin(), old.end(), [&](const XmlElement* e) {
                return std::abs(e->getDoubleAttribute("Time") - p.time) < 1e-12 && std::abs(valueOf(e) - p.value) < 1e-12;
            });
            auto e = found != old.end() ? std::make_unique<XmlElement>(**found)
                                       : std::make_unique<XmlElement>(juce::String(env.eventType));
            e->setAttribute("Id", (int) i);
            e->setAttribute("Time", num(p.time));
            e->setAttribute("Value", env.eventType == "BoolEvent" ? juce::String(p.value >= 0.5 ? "true" : "false") : num(p.value));
            if (found == old.end() && env.eventType == "FloatEvent")
            {
                e->setAttribute("CurveControl1X", num(p.x1));
                e->setAttribute("CurveControl1Y", num(p.y1));
                e->setAttribute("CurveControl2X", num(p.x2));
                e->setAttribute("CurveControl2Y", num(p.y2));
            }
            replacements.push_back(std::move(e));
        }
        events->deleteAllChildElements();
        for (auto& e : replacements) events->addChildElement(e.release());
    }
}

// Brings the <Tracks> list in line with the model: new tracks (groups) come from their authored XML, removed groups
// disappear and the order follows the model. When nothing changed the elements are left exactly as they are.
void reconcileTracks(XmlElement& tracksX, const model::Song& song, AlsSaveResult& result)
{
    std::map<int, XmlElement*> byId;
    std::vector<int> current, desired;
    for (auto* tx : tracksX.getChildIterator())
    {
        const int id = tx->getIntAttribute("Id", -1);
        byId[id] = tx;
        current.push_back(id);
    }
    std::vector<const model::Track*> wanted;
    for (const auto& t : song.tracks) wanted.push_back(&t);
    for (const auto& t : song.returns) wanted.push_back(&t);
    for (const auto* t : wanted) desired.push_back(t->id);
    auto tagFor = [](model::TrackKind kind) {
        switch (kind)
        {
            case model::TrackKind::Midi: return "MidiTrack";
            case model::TrackKind::Audio: return "AudioTrack";
            case model::TrackKind::Group: return "GroupTrack";
            case model::TrackKind::Return: return "ReturnTrack";
            case model::TrackKind::Main: return "MainTrack";
        }
        return "AudioTrack";
    };
    const bool typesMatch = std::all_of(wanted.begin(), wanted.end(), [&](const auto* track) {
        const auto found = byId.find(track->id); return found != byId.end() && found->second->hasTagName(tagFor(track->kind));
    });
    if (current == desired && typesMatch) return;

    std::vector<std::unique_ptr<XmlElement>> ordered;
    std::set<int> kept;
    for (const auto* t : wanted)
    {
        const auto found = byId.find(t->id);
        if (found != byId.end() && found->second->hasTagName(tagFor(t->kind)))
        {
            tracksX.removeChildElement(found->second, false);
            ordered.emplace_back(found->second);
            kept.insert(t->id);
            continue;
        }
        auto fresh = t->sourceXml ? juce::XmlDocument::parse(juce::String(*t->sourceXml)) : nullptr;
        if (! fresh)
        {
            result.error = "No XML template for the new track " + t->name;
            return;
        }
        fresh->setAttribute("Id", t->id);
        ordered.push_back(std::move(fresh));
    }
    // Elements the model no longer has: removed group tracks go away, anything unknown to us stays at the end.
    std::vector<std::unique_ptr<XmlElement>> leftovers;
    for (auto& [id, element] : byId)
        if (! kept.count(id) && element->getTagName() != "GroupTrack" && element->getTagName() != "AudioTrack"
            && element->getTagName() != "MidiTrack" && element->getTagName() != "ReturnTrack" && tracksX.containsChildElement(element))
        {
            tracksX.removeChildElement(element, false);
            leftovers.emplace_back(element);
        }
    tracksX.deleteAllChildElements();
    for (auto& e : ordered) tracksX.addChildElement(e.release());
    for (auto& e : leftovers) tracksX.addChildElement(e.release());
}

void patchRoutingTarget(XmlElement* routing, const std::string& target)
{
    if (routing == nullptr) return;
    setValueAttr(child(routing, "Target"), juce::String::fromUTF8(target.c_str()));
    const bool group = target == "AudioOut/GroupTrack";
    if (auto* upper = child(routing, "UpperDisplayString"))
    {
        const auto old = upper->getStringAttribute("Value");
        if (group) upper->setAttribute("Value", "Group");
        else if (target == "AudioOut/Main") upper->setAttribute("Value", "Main");
        else if (target == "MidiOut/None") upper->setAttribute("Value", "No Output");
        else if (old == "Group") upper->setAttribute("Value", "Master");
    }
    if (auto* lower = child(routing, "LowerDisplayString"))
        if (group || target == "AudioOut/Main" || target == "MidiOut/None") lower->setAttribute("Value", "");
}

void patchTrackMixerAndDevices(XmlElement* x, const model::Track& t)
{
    if (auto* color = child(x, "Color"); color && color->getIntAttribute("Value") != t.colorIndex)
        setNumber(color, t.colorIndex);
    if (auto* unfolded = child(x, "TrackUnfolded")) // a group opened or closed in Sonora
        if (unfolded->getStringAttribute("Value").equalsIgnoreCase("true") == t.folded)
            setValueAttr(unfolded, t.folded ? "false" : "true");
    if (t.kind != model::TrackKind::Main)
        if (auto* parent = child(x, "TrackGroupId"))
            if (parent->getIntAttribute("Value", -1) != t.groupId)
                setNumber(parent, t.groupId);
    if (auto* name = child(x, "Name"))
    {
        const auto user = child(name, "UserName") ? child(name, "UserName")->getStringAttribute("Value") : juce::String();
        const auto effective = child(name, "EffectiveName") ? child(name, "EffectiveName")->getStringAttribute("Value") : juce::String();
        const auto userName = t.nameIsAutomatic ? std::string{} : t.name;
        if ((user.toStdString() != userName || (t.nameIsAutomatic && effective.toStdString() != t.name)) && t.kind != model::TrackKind::Main)
        {
            setValueAttr(child(name, "UserName"), juce::String::fromUTF8(userName.c_str()));
            setValueAttr(child(name, "EffectiveName"), juce::String::fromUTF8(t.name.c_str()));
        }
    }
    if (auto* oldSonora = child(x, "SonoraTrack")) x->removeChildElement(oldSonora, true);
    if (t.hidden || t.deactivated)
    {
        auto* sonora = x->createNewChildElement("SonoraTrack");
        sonora->setAttribute("Version", 1);
        if (t.hidden) sonora->setAttribute("Hidden", "true");
        if (t.deactivated) sonora->setAttribute("Deactivated", "true");
    }
    if (auto* oldFreeze = child(x, "SonoraFreeze")) x->removeChildElement(oldFreeze, true);
    if (t.frozen)
    {
        auto* freeze = x->createNewChildElement("SonoraFreeze");
        freeze->setAttribute("Version", 1);
        auto writeRender = [&](const model::Clip& clip, int scene) {
            auto* render = freeze->createNewChildElement("Render");
            render->setAttribute("Scene", scene);
            auto audio = juce::XmlDocument::parse(juce::String(audioClipTemplate(clip)));
            if (audio) { patchClip(audio.get(), clip); render->addChildElement(audio.release()); }
        };
        for (const auto& clip : t.frozenArrangement) writeRender(clip, -1);
        for (size_t i = 0; i < t.frozenSlots.size(); ++i) if (t.frozenSlots[i].clip) writeRender(*t.frozenSlots[i].clip, (int) i);
    }
    patchEnvelopes(x, t.automation, "AutomationEnvelopes", "AutomationEnvelope");
    auto* chain = child(x, "DeviceChain");
    if (! chain) return;
    if (t.outputRoutingEdited)
        patchRoutingTarget(child(chain, "AudioOutputRouting"), t.outputRouting);
    patchMixer(child(chain, "Mixer"), t.mixer);
    if (t.midiControllersEdited)
        if (auto* seq = child(chain, "MainSequencer"))
        {
            auto* controllers = child(seq, "MidiControllers");
            const bool any = std::any_of(t.midiControllerTargets.begin(), t.midiControllerTargets.end(), [](int id) { return id >= 0; });
            if (! any && ! t.midiControllersPresent)
            { if (controllers) seq->removeChildElement(controllers, true); }
            else
            {
                if (! controllers) controllers = seq->createNewChildElement("MidiControllers");
                for (int index = 0; index < model::midiControllerTargetCount; ++index)
                {
                    const auto tag = "ControllerTargets." + juce::String(index);
                    auto* target = child(controllers, tag.toRawUTF8());
                    const int id = t.midiControllerTargets[(size_t) index];
                    if (id < 0) { if (target) controllers->removeChildElement(target, true); continue; }
                    if (! target)
                    {
                        target = controllers->createNewChildElement(tag);
                        target->createNewChildElement("LockEnvelope")->setAttribute("Value", "0");
                    }
                    target->setAttribute("Id", id);
                }
            }
        }
    if (t.kind == model::TrackKind::Audio || t.kind == model::TrackKind::Midi)
    {
        if (t.takeCounterAdvanced)
            if (auto* seq = child(chain, "MainSequencer"))
            {
                auto* recorder = child(seq, "Recorder");
                if (! recorder)
                {
                    recorder = seq->createNewChildElement("Recorder");
                    recorder->createNewChildElement("IsArmed")->setAttribute("Value", "false");
                }
                auto* counter = child(recorder, "TakeCounter");
                if (! counter) counter = recorder->createNewChildElement("TakeCounter");
                setNumber(counter, t.recorderTakeCounter);
            }
        const char* tag = t.kind == model::TrackKind::Midi ? "MidiInputRouting" : "AudioInputRouting";
        auto* input = child(chain, tag);
        if (! t.inputRoutingPresent && ! t.inputRoutingEdited) { if (input) chain->removeChildElement(input, true); }
        else if (input || t.inputRoutingEdited)
        {
            if (! input) input = chain->createNewChildElement(tag);
            for (const auto& [key, value] : std::initializer_list<std::pair<const char*, const std::string&>>{
                     {"Target", t.inputRouting}, {"UpperDisplayString", t.inputUpperDisplay}, {"LowerDisplayString", t.inputLowerDisplay}})
            {
                auto* field = child(input, key);
                if (! field && t.inputRoutingEdited) field = input->createNewChildElement(key);
                setValueAttr(field, juce::String::fromUTF8(value.c_str()));
            }
        }
        if (auto* sequencer = child(chain, "MainSequencer"))
        {
            auto* monitor = child(sequencer, "MonitoringEnum");
            if (! t.monitoringPresent && ! t.monitoringEdited) { if (monitor) sequencer->removeChildElement(monitor, true); }
            else if (monitor || t.monitoringEdited)
            {
                if (! monitor) monitor = sequencer->createNewChildElement("MonitoringEnum");
                setNumber(monitor, t.monitoring);
            }
        }
    }
    if (auto* devices = at(chain, "DeviceChain/Devices"))
    {
        std::vector<XmlElement*> old;
        for (auto* dx : devices->getChildIterator()) old.push_back(dx);
        std::vector<std::unique_ptr<XmlElement>> ordered;
        for (const auto& device : t.devices)
        {
            const auto found = std::find_if(old.begin(), old.end(), [&](const auto* dx) {
                return dx->getIntAttribute("Id", -1) == device.id && dx->getTagName().toStdString() == device.type;
            });
            auto dx = found != old.end() ? std::make_unique<XmlElement>(**found)
                                         : device.sourceXml ? juce::XmlDocument::parse(juce::String(*device.sourceXml)) : nullptr;
            if (! dx) continue; // validated before patching
            dx->setAttribute("Id", device.id);
            patchDevice(dx.get(), device);
            ordered.push_back(std::move(dx));
        }
        devices->deleteAllChildElements();
        for (auto& dx : ordered) devices->addChildElement(dx.release());
    }
}

// Decides what happens to the clip elements of one track. Clones are made here, from the untouched
// original elements, before anything is removed (a clip may move to another track).
ClipPlan planClips(XmlElement* x, const model::Track& t, const model::Song& song, const ClipIndex& index,
                   const SessionIndex& sessions, AlsSaveResult& stats)
{
    ClipPlan plan;
    plan.events = arrangementEvents(x);
    if (plan.events == nullptr)
    {
        if (! t.arrangementClips.empty()) stats.error = "No arrangement container for track " + t.name;
        return plan;
    }

    std::vector<XmlElement*> own;
    int maxId = 0;
    for (auto* e : plan.events->getChildIterator())
        if (e->hasTagName("MidiClip") || e->hasTagName("AudioClip"))
        {
            own.push_back(e);
            maxId = std::max(maxId, e->getIntAttribute("Id", 0));
        }

    for (const auto& c : t.arrangementClips)
    {
        auto retained = retainedClip(c, t.id, index, sessions);
        if (! retained)
        {
            stats.error = "No XML template for clip " + c.name + " on track " + t.name;
            return plan;
        }
        auto* existing = savedClip(song, c, t.id, -1, index, sessions);
        auto* target = retained.get();
        if (existing)
        {
            target->setAttribute("Id", existing->getStringAttribute("Id"));
            plan.replaced.push_back(existing);
            ++stats.clipsUpdated;
        }
        else
        {
            target->setAttribute("Id", ++maxId);
            ++stats.clipsAdded;
            // A fresh clip does not inherit the template's clip envelopes.
            if (c.envelopes.empty())
                if (auto* envs = at(target, "Envelopes/Envelopes")) envs->deleteAllChildElements();
        }
        plan.add.push_back(std::move(retained));
        plan.patch.push_back({target, &c});
    }
    for (auto* e : own)
        plan.remove.push_back(e);
    return plan;
}

// Build session plans before arrangement elements are removed: recording may clone either kind.
std::vector<ClipPlan> planSessionClips(XmlElement* x, const model::Track& track, const model::Song& song, const ClipIndex& index,
                                      const SessionIndex& sessions, AlsSaveResult& stats)
{
    std::vector<ClipPlan> plans;
    auto* seq = at(x, "DeviceChain/MainSequencer");
    if (seq == nullptr) return plans;
    auto* list = child(seq, "ClipSlotList");
    if (! list && track.slots.empty()) return plans;
    if (! list) list = seq->createNewChildElement("ClipSlotList");
    std::vector<XmlElement*> slots;
    for (auto* slot : list->getChildWithTagNameIterator("ClipSlot")) slots.push_back(slot);
    for (size_t i = 0; i < track.slots.size(); ++i)
    {
        XmlElement* slot = nullptr;
        if (i < slots.size()) slot = slots[i];
        else
        {
            slot = list->createNewChildElement("ClipSlot");
            slot->setAttribute("Id", (int) i);
            slot->createNewChildElement("LomId")->setAttribute("Value", 0);
            slot->createNewChildElement("ClipSlot")->createNewChildElement("Value");
            slot->createNewChildElement("HasStop")->setAttribute("Value", "true");
            slot->createNewChildElement("NeedRefreeze")->setAttribute("Value", "true");
            slots.push_back(slot);
        }
        auto* value = at(slot, "ClipSlot/Value");
        if (! value) continue;
        ClipPlan plan;
        plan.events = value;
        XmlElement* existing = nullptr;
        for (auto* c : value->getChildIterator())
            if (c->hasTagName("MidiClip") || c->hasTagName("AudioClip")) { existing = c; break; }
        const auto& clip = track.slots[i].clip;
        if (clip)
        {
            auto retained = retainedClip(*clip, track.id, index, sessions);
            if (! retained)
            {
                stats.error = "No XML template for session clip " + clip->name + " on track " + track.name;
                return plans;
            }
            auto* target = retained.get();
            if (auto* saved = savedClip(song, *clip, track.id, (int) i, index, sessions))
            {
                target->setAttribute("Id", saved->getStringAttribute("Id"));
                plan.replaced.push_back(saved);
                ++stats.clipsUpdated;
            }
            else
            {
                target->setAttribute("Id", (int) i);
                if (clip->envelopes.empty())
                    if (auto* envs = at(target, "Envelopes/Envelopes")) envs->deleteAllChildElements();
                ++stats.clipsAdded;
            }
            plan.patch.push_back({target, &*clip});
            plan.add.push_back(std::move(retained));
            if (existing) plan.remove.push_back(existing);
        }
        else if (existing) plan.remove.push_back(existing);
        plans.push_back(std::move(plan));
    }
    ClipPlan trim;
    trim.events = list;
    for (size_t i = track.slots.size(); i < slots.size(); ++i) trim.remove.push_back(slots[i]);
    if (! trim.remove.empty()) plans.push_back(std::move(trim));
    return plans;
}

void patchScenesAndLocators(XmlElement& liveSet, const model::Song& song)
{
    auto ensure = [](XmlElement* parent, const char* name) {
        auto* e = child(parent, name);
        return e ? e : parent->createNewChildElement(name);
    };
    auto* scenes = child(&liveSet, "Scenes");
    if (! scenes && ! song.scenes.empty()) scenes = liveSet.createNewChildElement("Scenes");
    if (scenes)
    {
        std::vector<XmlElement*> old;
        for (auto* e : scenes->getChildWithTagNameIterator("Scene")) old.push_back(e);
        for (size_t i = 0; i < song.scenes.size(); ++i)
        {
            const auto& scene = song.scenes[i];
            auto* e = i < old.size() ? old[i] : nullptr;
            if (! e)
            {
                e = old.empty() ? new XmlElement("Scene") : new XmlElement(*old.front());
                e->setAttribute("Id", (int) i);
                scenes->addChildElement(e);
                setValueAttr(at(e, "FollowAction/FollowActionEnabled"), "false");
                setValueAttr(child(e, "IsTempoEnabled"), "false");
            }
            setValueAttr(ensure(e, "Name"), juce::String::fromUTF8(scene.name.c_str()));
            if (child(e, "Color") || scene.colorIndex >= 0) setNumber(ensure(e, "Color"), scene.colorIndex);
            if (child(e, "IsTempoEnabled") || scene.tempo)
                setValueAttr(ensure(e, "IsTempoEnabled"), scene.tempo ? "true" : "false");
            if (scene.tempo) setNumber(ensure(e, "Tempo"), *scene.tempo);
        }
        for (size_t i = song.scenes.size(); i < old.size(); ++i) scenes->removeChildElement(old[i], true);
    }
    auto* holder = child(&liveSet, "Locators");
    if (! holder && ! song.locators.empty()) holder = liveSet.createNewChildElement("Locators");
    auto* locators = holder ? child(holder, "Locators") : nullptr;
    if (! locators && holder && ! song.locators.empty()) locators = holder->createNewChildElement("Locators");
    if (locators)
    {
        std::vector<XmlElement*> old;
        for (auto* e : locators->getChildWithTagNameIterator("Locator")) old.push_back(e);
        int nextLocatorId = 0;
        for (const auto* e : old) nextLocatorId = std::max(nextLocatorId, e->getIntAttribute("Id", -1) + 1);
        for (size_t i = 0; i < song.locators.size(); ++i)
        {
            const auto& locator = song.locators[i];
            auto* e = i < old.size() ? old[i] : locators->createNewChildElement("Locator");
            if (i >= old.size())
            {
                e->setAttribute("Id", nextLocatorId++);
                e->createNewChildElement("LomId")->setAttribute("Value", 0);
                e->createNewChildElement("IsSongStart")->setAttribute("Value", "false");
            }
            setNumber(ensure(e, "Time"), locator.time);
            setValueAttr(ensure(e, "Name"), juce::String::fromUTF8(locator.name.c_str()));
        }
        for (size_t i = song.locators.size(); i < old.size(); ++i) locators->removeChildElement(old[i], true);
    }
}

void applyClipPlan(ClipPlan& plan, AlsSaveResult& stats)
{
    if (plan.events == nullptr)
        return;
    for (auto* e : plan.remove)
    {
        plan.events->removeChildElement(e, true);
        if (std::find(plan.replaced.begin(), plan.replaced.end(), e) == plan.replaced.end()) ++stats.clipsRemoved;
    }
    for (auto& e : plan.add)
        plan.events->addChildElement(e.release());
    for (auto& [e, c] : plan.patch)
        patchClip(e, *c);

    // Keep the elements in timeline order like Live writes them.
    juce::Array<XmlElement*> ordered;
    for (auto* e : plan.events->getChildIterator())
        ordered.add(e);
    std::stable_sort(ordered.begin(), ordered.end(), [](XmlElement* a, XmlElement* b) {
        return a->getDoubleAttribute("Time") < b->getDoubleAttribute("Time");
    });
    std::vector<XmlElement*> detached;
    for (auto* e : ordered)
    {
        plan.events->removeChildElement(e, false);
        detached.push_back(e);
    }
    for (auto* e : detached)
        plan.events->addChildElement(e);
}
// ENG-9b: take lanes have independent clip containers and retain unknown lane/clip metadata.
void patchTakeLanes(XmlElement& trackXml, const model::Track& track, AlsSaveResult& result)
{
    if (! track.takeLanesNeedPatch) return;
    auto replacement = track.takeLanesSourceXml ? juce::XmlDocument::parse(juce::String(*track.takeLanesSourceXml)) : nullptr;
    if (track.takeLanesSourceXml && (! replacement || ! replacement->hasTagName("TakeLanes")))
    { result.error = "Invalid retained take-lane container"; return; }
    if (! replacement && ! track.takeLanes.empty())
    {
        replacement = std::make_unique<XmlElement>("TakeLanes");
        replacement->createNewChildElement("TakeLanes");
        replacement->createNewChildElement("AreTakeLanesFolded")->setAttribute("Value", "true");
    }
    if (replacement)
    {
        auto* list = child(replacement.get(), "TakeLanes");
        if (! list) list = replacement->createNewChildElement("TakeLanes");
        for (auto* element = list->getFirstChildElement(); element;)
        {
            auto* next = element->getNextElement();
            if (element->hasTagName("TakeLane")) list->removeChildElement(element, true);
            element = next;
        }
        for (const auto& lane : track.takeLanes)
        {
            auto xml = lane.sourceXml ? juce::XmlDocument::parse(juce::String(*lane.sourceXml)) : std::make_unique<XmlElement>("TakeLane");
            if (! xml || ! xml->hasTagName("TakeLane")) { result.error = "Invalid take-lane template"; return; }
            xml->setAttribute("Id", lane.id);
            auto scalar = [&](const char* key, const juce::String& value) {
                auto* element = child(xml.get(), key);
                if (! element) element = xml->createNewChildElement(key);
                setValueAttr(element, value);
            };
            if (! lane.sourceXml)
            {
                scalar("LomId", "0"); scalar("IsContentSelectedInDocument", "false");
                xml->createNewChildElement("ArrangementClipsListWrapper")->setAttribute("LomId", 0);
            }
            scalar("Height", juce::String(lane.height)); scalar("Name", juce::String::fromUTF8(lane.name.c_str()));
            scalar("Annotation", juce::String::fromUTF8(lane.annotation.c_str())); scalar("Audition", lane.audition ? "true" : "false");
            auto* automation = child(xml.get(), "ClipAutomation");
            if (! automation)
            {
                automation = xml->createNewChildElement("ClipAutomation");
                auto* transform = automation->createNewChildElement("AutomationTransformViewState");
                transform->createNewChildElement("IsTransformPending")->setAttribute("Value", "false");
                transform->createNewChildElement("TimeAndValueTransforms");
            }
            auto* events = child(automation, "Events");
            if (! events) events = automation->createNewChildElement("Events");
            for (auto* element = events->getFirstChildElement(); element;)
            {
                auto* next = element->getNextElement();
                if (element->hasTagName("AudioClip") || element->hasTagName("MidiClip")) events->removeChildElement(element, true);
                element = next;
            }
            int id = 0;
            for (const auto& clip : lane.clips)
            {
                auto clipXml = clip.sourceXml ? juce::XmlDocument::parse(juce::String(*clip.sourceXml)) : nullptr;
                if (! clipXml || ! clipXml->hasTagName(clip.kind == model::Clip::Kind::Audio ? "AudioClip" : "MidiClip"))
                { result.error = "No retained XML for take clip " + clip.name; return; }
                if (! lane.sourceXml) clipXml->setAttribute("Id", id++);
                patchClip(clipXml.get(), clip);
                events->addChildElement(clipXml.release());
            }
            list->addChildElement(xml.release());
        }
    }
    if (auto* old = child(&trackXml, "TakeLanes")) trackXml.removeChildElement(old, true);
    if (replacement) trackXml.addChildElement(replacement.release());
}
} // namespace

AlsSaveResult patchAlsDom(const model::Song& song, XmlElement& root)
{
    AlsSaveResult r;
    auto* liveSet = root.getChildByName("LiveSet");
    if (liveSet == nullptr)
    {
        r.error = "Not a Live Set (no <LiveSet>)";
        return r;
    }

    if (auto* tracksX = liveSet->getChildByName("Tracks"))
    {
        reconcileTracks(*tracksX, song, r);
        if (! r.error.empty()) return r;
        std::map<int, XmlElement*> byId;
        for (auto* tx : tracksX->getChildIterator())
            byId[tx->getIntAttribute("Id", -1)] = tx;
        auto validateDevices = [&](XmlElement* tx, const model::Track& track) {
            auto* list = at(tx, "DeviceChain/DeviceChain/Devices");
            for (const auto& device : track.devices)
            {
                bool found = false;
                if (list)
                    for (auto* dx : list->getChildIterator())
                        found |= dx->getIntAttribute("Id", -1) == device.id && dx->getTagName().toStdString() == device.type;
                auto source = ! found && device.sourceXml ? juce::XmlDocument::parse(juce::String(*device.sourceXml)) : nullptr;
                if ((! found && (! source || source->getTagName().toStdString() != device.type)) || (! list && ! track.devices.empty()))
                    r.error = "No XML template/container for device " + device.displayName;
            }
        };
        for (const auto& track : song.tracks) validateDevices(byId[track.id], track);
        for (const auto& track : song.returns) validateDevices(byId[track.id], track);
        auto* main = child(liveSet, "MainTrack");
        if (! main) main = child(liveSet, "MasterTrack");
        validateDevices(main, song.main);
        if (! r.error.empty()) return r;
        const auto index = indexClips(*liveSet);
        const auto sessions = indexSessionClips(*liveSet);

        // Phase 1: everything that needs the untouched elements (clones). Phase 2: change the file.
        std::vector<ClipPlan> plans;
        for (const auto& t : song.tracks)
            if (byId.count(t.id))
            {
                patchTrackMixerAndDevices(byId[t.id], t);
                patchTakeLanes(*byId[t.id], t, r);
                plans.push_back(planClips(byId[t.id], t, song, index, sessions, r));
                auto sessionPlans = planSessionClips(byId[t.id], t, song, index, sessions, r);
                for (auto& plan : sessionPlans) plans.push_back(std::move(plan));
            }
        if (! r.error.empty()) return r;
        for (const auto& t : song.returns)
            if (byId.count(t.id))
                patchTrackMixerAndDevices(byId[t.id], t);
        for (auto& plan : plans)
            applyClipPlan(plan, r);
    }

    patchScenesAndLocators(*liveSet, song);
    if (song.pointeeIdsAllocated)
    {
        if (auto* next = child(liveSet, "NextPointeeId"))
        {
            if (song.nextPointeeId > next->getIntAttribute("Value")) setNumber(next, song.nextPointeeId);
        }
        else liveSet->createNewChildElement("NextPointeeId")->setAttribute("Value", song.nextPointeeId);
    }

    patchScale(*liveSet, song.scaleRoot, song.scaleName, "InKey", song.inKey);

    if (child(liveSet, "GlobalQuantisation") || song.globalQuantisation != 4)
    {
        auto* quantisation = child(liveSet, "GlobalQuantisation");
        if (! quantisation) quantisation = liveSet->createNewChildElement("GlobalQuantisation");
        setNumber(quantisation, song.globalQuantisation);
    }

    auto* mainX = liveSet->getChildByName("MainTrack");
    if (mainX == nullptr)
        mainX = liveSet->getChildByName("MasterTrack");
    if (mainX != nullptr)
    {
        patchTrackMixerAndDevices(mainX, song.main);
        setManual(at(mainX, "DeviceChain/Mixer/Tempo"), song.tempo);
        setManual(at(mainX, "DeviceChain/Mixer/TimeSignature"), model::encodeTimeSignature(song.timeSigNumerator, song.timeSigDenominator));
    }

    if (song.sendsPreEdited)
    {
        auto* pre = child(liveSet, "SendsPre");
        if (! pre) pre = liveSet->createNewChildElement("SendsPre");
        pre->deleteAllChildElementsWithTagName("SendPreBool");
        for (size_t i = 0; i < song.sendPreXml.size(); ++i)
            if (song.sendPreXml[i])
                if (auto value = juce::XmlDocument::parse(juce::String(*song.sendPreXml[i])))
                {
                    value->setAttribute("Id", (int) i); pre->addChildElement(value.release());
                }
    }
    if (auto* tr = liveSet->getChildByName("Transport"))
    {
        setValueAttr(child(tr, "LoopOn"), song.loopOn ? "true" : "false");
        setNumber(child(tr, "LoopStart"), song.loopStart);
        setNumber(child(tr, "LoopLength"), song.loopLength);
        for (const auto& [tag, enabled, present] : {std::tuple{"PunchIn", song.punchIn, song.punchInPresent},
                                                  std::tuple{"PunchOut", song.punchOut, song.punchOutPresent}})
        {
            auto* field = child(tr, tag);
            if (! present && ! song.punchEdited) { if (field) tr->removeChildElement(field, true); }
            else
            {
                if (! field) field = tr->createNewChildElement(tag);
                setValueAttr(field, enabled ? "true" : "false");
            }
        }
    }
    r.ok = true;
    return r;
}

TrackDuplication prepareTrackDuplication(const model::Song& song, const std::vector<int>& selection)
{
    TrackDuplication result;
    std::vector<int> members;
    if (! model::groupableSelection(song, selection, members))
    { result.error = "Select audio, MIDI or group tracks with the same parent to duplicate"; return result; }
    auto root = readAlsDom(juce::File(song.sourcePath), result.error);
    if (! root) return result;
    auto patched = patchAlsDom(song, *root);
    if (! patched.ok) { result.error = patched.error; return result; }
    auto* list = at(root.get(), "LiveSet/Tracks");
    if (! list) { result.error = "No track container"; return result; }
    int nextTrack = song.main.id + 1;
    for (auto* element : list->getChildIterator()) nextTrack = std::max(nextTrack, element->getIntAttribute("Id", -1) + 1);
    std::map<int, int> tracks, targets;
    for (int index : members) tracks[song.tracks[(size_t) index].id] = nextTrack++;
    int nextTarget = std::max(1, song.nextPointeeId);
    auto isTarget = [](const XmlElement& e) {
        return e.hasTagName("AutomationTarget") || e.hasTagName("ModulationTarget") || e.hasTagName("Pointee")
               || e.getTagName().startsWith("ControllerTargets.");
    };
    std::function<void(const XmlElement&)> reserve = [&](const auto& e) {
        if (isTarget(e)) nextTarget = std::max(nextTarget, e.getIntAttribute("Id", -1) + 1);
        for (const auto* childElement : e.getChildIterator()) reserve(*childElement);
    };
    reserve(*root);
    std::vector<std::unique_ptr<XmlElement>> copies;
    for (int index : members)
    {
        const int id = song.tracks[(size_t) index].id;
        const auto* source = list->getChildByAttribute("Id", juce::String(id));
        if (! source) { result.error = "Missing source track for duplication"; return result; }
        copies.push_back(std::make_unique<XmlElement>(*source));
    }
    std::function<void(const XmlElement&)> allocate = [&](const auto& e) {
        if (isTarget(e) && e.getIntAttribute("Id", -1) >= 0)
            targets.try_emplace(e.getIntAttribute("Id"), nextTarget++);
        for (const auto* childElement : e.getChildIterator()) allocate(*childElement);
    };
    for (const auto& copy : copies) allocate(*copy);
    std::function<void(XmlElement&)> rebind = [&](auto& e) {
        if (isTarget(e))
            if (const auto found = targets.find(e.getIntAttribute("Id", -1)); found != targets.end()) e.setAttribute("Id", found->second);
        if (e.hasTagName("PointeeId"))
            if (const auto found = targets.find(e.getIntAttribute("Value", -1)); found != targets.end()) e.setAttribute("Value", found->second);
        if (e.hasTagName("TrackGroupId"))
            if (const auto found = tracks.find(e.getIntAttribute("Value", -1)); found != tracks.end()) e.setAttribute("Value", found->second);
        if (e.hasTagName("Target"))
        {
            const auto value = e.getStringAttribute("Value");
            const auto marker = value.indexOf("/Track.");
            if (marker >= 0)
            {
                const auto suffix = value.substring(marker + 7);
                const auto digits = suffix.initialSectionContainingOnly("0123456789");
                if (digits.isNotEmpty())
                    if (const auto found = tracks.find(digits.getIntValue()); found != tracks.end())
                        e.setAttribute("Value", value.substring(0, marker + 7) + juce::String(found->second) + suffix.substring(digits.length()));
            }
        }
        for (auto* childElement : e.getChildIterator()) rebind(*childElement);
    };
    std::map<int, std::shared_ptr<const std::string>> sources;
    for (size_t i = 0; i < copies.size(); ++i)
    {
        auto& copy = *copies[i];
        const int id = tracks.at(song.tracks[(size_t) members[i]].id);
        copy.setAttribute("Id", id);
        rebind(copy);
        sources[id] = std::make_shared<const std::string>(copy.toString().toStdString());
        list->addChildElement(copies[i].release());
    }
    auto loaded = parseAls(*root, song.sourcePath);
    if (! loaded) { result.error = loaded.error; return result; }
    for (auto& track : loaded.song->tracks)
        if (const auto found = sources.find(track.id); found != sources.end())
        { track.sourceXml = found->second; result.tracks.push_back(std::move(track)); }
    result.before = members.back() + 1;
    result.pointeeEnd = nextTarget;
    return result;
}

void refreshClipIdentities(model::Song& song)
{
    std::string error;
    bool needsTemplates = false;
    for (const auto& track : song.tracks)
    {
        for (const auto& clip : track.arrangementClips) needsTemplates |= ! clip.sourceXml;
        for (const auto& slot : track.slots) if (slot.clip) needsTemplates |= ! slot.clip->sourceXml;
    }
    auto root = needsTemplates ? readAlsDom(juce::File(song.sourcePath), error) : nullptr;
    auto* liveSet = root ? root->getChildByName("LiveSet") : nullptr;
    const auto index = liveSet ? indexClips(*liveSet) : ClipIndex{};
    const auto sessions = liveSet ? indexSessionClips(*liveSet) : SessionIndex{};
    song.savedClips.clear();
    for (auto& t : song.tracks)
    {
        for (size_t slot = 0; slot < t.slots.size(); ++slot)
            if (auto& clip = t.slots[slot].clip)
            {
                clip->xmlId = (int) slot;
                clip->cloneOfXmlId = clip->cloneOfSessionSlot = clip->cloneOfTrackId = -1;
                song.savedClips[clip->uid] = {t.id, -1, (int) slot};
                if (! clip->sourceXml)
                    if (auto* saved = savedClip(song, *clip, t.id, (int) slot, index, sessions))
                        clip->sourceXml = std::make_shared<const std::string>(saved->toString().toStdString());
            }
        int ordinal = 0;
        for (auto& c : t.arrangementClips)
        {
            c.xmlId = ordinal++;
            c.cloneOfXmlId = c.cloneOfSessionSlot = -1;
            c.cloneOfTrackId = -1;
            song.savedClips[c.uid] = {t.id, c.xmlId, -1};
            if (! c.sourceXml)
                if (auto* saved = savedClip(song, c, t.id, -1, index, sessions))
                    c.sourceXml = std::make_shared<const std::string>(saved->toString().toStdString());
        }
    }
}

namespace
{
// keepCopied: references whose file was copied along with the project (same name and size already in the destination)
// stay project-relative instead of being anchored to the original absolute location.
void relocateFileRefs(XmlElement& root, const juce::File& base, const juce::File& destinationDirectory, bool keepCopied)
{
    if (base == destinationDirectory) return;
    if (root.hasTagName("FileRef"))
    {
        auto* relative = child(&root, "RelativePath");
        auto* type = child(&root, "RelativePathType");
        if (relative && type && type->getIntAttribute("Value") == 3)
        {
            const auto name = relative->getStringAttribute("Value");
            if (keepCopied && ! name.isEmpty())
            {
                const auto copied = destinationDirectory.getChildFile(name);
                const auto original = base.getChildFile(name);
                if (copied.existsAsFile() && original.existsAsFile() && copied.getSize() == original.getSize())
                    return; // the file travelled with the project: stay project-relative
            }
            auto file = base.getChildFile(name);
            if (! name.isEmpty() && ! file.existsAsFile())
            {
                const auto absolute = child(&root, "Path") ? child(&root, "Path")->getStringAttribute("Value") : juce::String{};
                if (juce::File::isAbsolutePath(absolute)) file = juce::File(absolute);
            }
            if (! name.isEmpty() && file.existsAsFile())
            {
                auto* path = child(&root, "Path");
                if (! path) path = root.createNewChildElement("Path");
                path->setAttribute("Value", file.getFullPathName());
                relative->setAttribute("Value", "");
                type->setAttribute("Value", 0);
            }
        }
    }
    for (auto* descendant : root.getChildIterator()) relocateFileRefs(*descendant, base, destinationDirectory, keepCopied);
}

// Copies `from/Samples/**` to `to/Samples/**`, skipping files that already exist with the same size.
void copyProjectSamples(const juce::File& from, const juce::File& to, AlsSaveResult& result)
{
    const auto source = from.getChildFile("Samples");
    if (! source.isDirectory() || from == to) return;
    for (const auto& file : source.findChildFiles(juce::File::findFiles, true))
    {
        if (file.getFileName().startsWith(".")) continue;
        const auto target = to.getChildFile(file.getRelativePathFrom(from));
        if (target.existsAsFile() && target.getSize() == file.getSize()) continue;
        if (! target.getParentDirectory().createDirectory() || ! file.copyFileTo(target))
        {
            ++result.samplesMissing;
            continue;
        }
        ++result.samplesCopied;
        result.bytesCopied += (std::uint64_t) file.getSize();
    }
}

// Collect All and Save: every sample reference that points outside `project` is copied into
// Samples/Imported and rewritten as a project-relative reference.
void collectSamples(XmlElement& root, const juce::File& project, const AlsSaveOptions& options, AlsSaveResult& result)
{
    if (root.hasTagName("SampleRef"))
        if (auto* ref = child(&root, "FileRef"))
        {
            auto* path = child(ref, "Path");
            auto* relative = child(ref, "RelativePath");
            auto* type = child(ref, "RelativePathType");
            const auto pack = child(ref, "LivePackName") ? child(ref, "LivePackName")->getStringAttribute("Value") : juce::String{};
            juce::File file;
            if (relative && type && type->getIntAttribute("Value") == 3 && relative->getStringAttribute("Value").isNotEmpty())
                file = project.getChildFile(relative->getStringAttribute("Value"));
            if (! file.existsAsFile() && path && juce::File::isAbsolutePath(path->getStringAttribute("Value")))
                file = juce::File(path->getStringAttribute("Value"));
            if (! file.existsAsFile())
                ++result.samplesMissing;
            else if (pack.isNotEmpty() && ! options.collectLivePackFiles)
            {
                // left in place: reproducible from the pack
            }
            else
            {
                juce::File target = file;
                if (! file.isAChildOf(project))
                {
                    const auto key = file.getFullPathName().toStdString();
                    if (const auto moved = result.movedSamples.find(key); moved != result.movedSamples.end())
                        target = juce::File(juce::String(moved->second));
                    else
                    {
                        const auto folder = project.getChildFile("Samples/Imported");
                        target = folder.getChildFile(file.getFileName());
                        if (target.existsAsFile() && target.getSize() != file.getSize())
                            target = folder.getNonexistentChildFile(file.getFileNameWithoutExtension(), file.getFileExtension(), false);
                        if (! target.existsAsFile())
                        {
                            if (! folder.createDirectory() || ! file.copyFileTo(target))
                            {
                                ++result.samplesMissing;
                                target = file;
                            }
                            else
                            {
                                ++result.samplesCopied;
                                result.bytesCopied += (std::uint64_t) file.getSize();
                            }
                        }
                        if (target != file)
                            result.movedSamples[key] = target.getFullPathName().toStdString();
                    }
                }
                if (target.isAChildOf(project))
                {
                    if (! path) path = ref->createNewChildElement("Path");
                    if (! relative) relative = ref->createNewChildElement("RelativePath");
                    if (! type) type = ref->createNewChildElement("RelativePathType");
                    path->setAttribute("Value", target.getFullPathName());
                    relative->setAttribute("Value", target.getRelativePathFrom(project).replaceCharacter('\\', '/'));
                    type->setAttribute("Value", 3);
                }
            }
        }
    for (auto* descendant : root.getChildIterator()) collectSamples(*descendant, project, options, result);
}

void relocateSamples(const model::Song& song, XmlElement& root, const juce::File& destination, bool keepCopied)
{
    auto* liveSet = child(&root, "LiveSet");
    if (! liveSet) return;
    const auto fallback = juce::File(song.sourcePath).getParentDirectory();
    const auto directory = destination.getParentDirectory();
    const auto clips = indexClips(*liveSet);
    const auto sessions = indexSessionClips(*liveSet);
    for (const auto& track : song.tracks)
    {
        if (const auto found = clips.find(track.id); found != clips.end())
            for (size_t i = 0; i < track.arrangementClips.size() && i < found->second.size(); ++i)
            {
                const auto& origin = track.arrangementClips[i].sample.sourceDirectory;
                relocateFileRefs(*found->second[i], origin.empty() ? fallback : juce::File(origin), directory, keepCopied);
            }
        for (size_t slot = 0; slot < track.slots.size(); ++slot)
            if (track.slots[slot].clip)
                if (const auto found = sessions.find({track.id, (int) slot}); found != sessions.end())
                {
                    const auto& origin = track.slots[slot].clip->sample.sourceDirectory;
                    relocateFileRefs(*found->second, origin.empty() ? fallback : juce::File(origin), directory, keepCopied);
                }
    }
    auto devices = [&](XmlElement* xml, const model::Track& track) {
        if (auto* list = at(xml, "DeviceChain/DeviceChain/Devices"))
        {
            size_t i = 0;
            for (auto* device : list->getChildIterator())
            {
                if (i >= track.devices.size()) break;
                const auto& origin = track.devices[i++].sourceDirectory;
                relocateFileRefs(*device, origin.empty() ? fallback : juce::File(origin), directory, keepCopied);
            }
        }
    };
    if (auto* tracks = child(liveSet, "Tracks"))
        for (auto* xml : tracks->getChildIterator())
        {
            for (const auto& track : song.tracks) if (track.id == xml->getIntAttribute("Id"))
            {
                devices(xml, track);
                if (auto* freeze = child(xml, "SonoraFreeze"))
                    relocateFileRefs(*freeze, fallback, directory, keepCopied);
                if (auto* lanes = at(xml, "TakeLanes/TakeLanes"))
                    for (auto* laneXml : lanes->getChildWithTagNameIterator("TakeLane"))
                        for (const auto& lane : track.takeLanes)
                            if (lane.id == laneXml->getIntAttribute("Id"))
                                if (auto* events = at(laneXml, "ClipAutomation/Events"))
                                {
                                    size_t ordinal = 0;
                                    for (auto* clip : events->getChildIterator())
                                        if (clip->hasTagName("AudioClip") || clip->hasTagName("MidiClip"))
                                        {
                                            if (ordinal >= lane.clips.size()) break;
                                            const auto& origin = lane.clips[ordinal++].sample.sourceDirectory;
                                            relocateFileRefs(*clip, origin.empty() ? fallback : juce::File(origin), directory, keepCopied);
                                        }
                                }
            }
            for (const auto& track : song.returns) if (track.id == xml->getIntAttribute("Id")) devices(xml, track);
        }
    auto* main = child(liveSet, "MainTrack");
    devices(main ? main : child(liveSet, "MasterTrack"), song.main);
}
} // namespace

bool writeAlsDom(const juce::XmlElement& root, const juce::File& destination, std::string& error)
{
    juce::XmlElement::TextFormat format;
    format.newLineChars = "\n";
    format.lineWrapLength = 100000; // never wrap attribute lists: Live writes one element per line
    const auto text = root.toString(format);
    juce::TemporaryFile temp(destination);
    {
        std::unique_ptr<juce::FileOutputStream> file(temp.getFile().createOutputStream());
        if (file == nullptr)
        {
            error = "Cannot write " + destination.getFullPathName().toStdString();
            return false;
        }
        bool written = false;
        if (destination.hasFileExtension("xml"))
        {
            written = file->write(text.toRawUTF8(), text.getNumBytesAsUTF8());
        }
        else
        {
            // Level 3: a 43 MB set takes 0.26 s instead of 1.5 s at level 9 for a file ~8% larger (level 6: 0.65 s, +1%).
            juce::GZIPCompressorOutputStream gz(*file, 3, 15 + 16); // +16 = gzip header (what .als files are)
            written = gz.write(text.toRawUTF8(), text.getNumBytesAsUTF8());
            gz.flush();
        }
        file->flush();
        if (! written || file->getStatus().failed())
        {
            error = "Could not finish writing " + destination.getFullPathName().toStdString();
            return false;
        }
    }
    if (! temp.overwriteTargetFileWithTemporary())
    {
        error = "Could not replace " + destination.getFullPathName().toStdString();
        return false;
    }
    return true;
}

AlsSaveResult saveAls(const model::Song& song, const juce::File& destination, bool portableReferences)
{
    AlsSaveOptions options;
    options.portableReferences = portableReferences;
    return saveAls(song, destination, options);
}

AlsSaveResult saveAls(const model::Song& song, const juce::File& destination, const AlsSaveOptions& options)
{
    AlsSaveResult r;
    const juce::File source(juce::String::fromUTF8(song.sourcePath.c_str()));
    std::string error;
    auto root = readAlsDom(source, error);
    if (root == nullptr)
    {
        r.error = "Cannot read the original set to patch: " + error;
        return r;
    }
    r = patchAlsDom(song, *root);
    if (! r.ok)
        return r;
    const auto project = destination.getParentDirectory();
    if (options.copyProjectSamples) copyProjectSamples(source.getParentDirectory(), project, r);
    if (options.portableReferences || options.copyProjectSamples || options.collectAll) relocateSamples(song, *root, destination, options.copyProjectSamples);
    if (options.collectAll) collectSamples(*root, project, options, r);

    if (destination == source)
    {
        // Never overwrite the only copy: keep a timestamped backup next to it (like Live's Backup folder).
        const auto backupDir = source.getParentDirectory().getChildFile("Backup");
        const auto stamp = juce::Time::getCurrentTime().formatted("%Y-%m-%d %H-%M-%S");
        const auto backup = backupDir.getNonexistentChildFile(source.getFileNameWithoutExtension() + " [" + stamp + "]",
                                                             source.getFileExtension());
        if (! backupDir.createDirectory() || ! source.copyFileTo(backup))
        {
            r.ok = false;
            r.error = "Could not create a backup of " + source.getFullPathName().toStdString();
            return r;
        }
    }

    if (! writeAlsDom(*root, destination, r.error))
        r.ok = false;
    return r;
}

} // namespace sonora::io
