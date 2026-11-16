// DEV-15c: Group (Cmd+G) / Ungroup (Shift+Cmd+G) devices into racks: the model edit, undo/redo, nesting, plug-in state,
// save and reload, and the MIDI Effect Rack's processing.
#include "TestFramework.h"
#include "core/DeviceGroups.h"
#include "devices/DeviceFactory.h"
#include "devices/RackProcessor.h"
#include "io/als/AlsNewSet.h"
#include "io/als/AlsReader.h"
#include "io/als/AlsTemplates.h"
#include "io/als/AlsWriter.h"

#include <juce_core/juce_core.h>

#include <set>

using namespace sonora;

namespace
{
model::Device native(model::Song& song, const std::string& type)
{
    auto d = devices::nativeDeviceModel(type);
    model::allocateDeviceIds(song, d);
    return d;
}

model::Device vst3(model::Song& song, const std::string& name, std::vector<uint8_t> state, bool instrument = false)
{
    model::Device d;
    d.kind = model::DeviceKind::PluginVst3;
    d.type = "PluginDevice";
    d.displayName = name;
    model::PluginInfo info;
    info.name = name;
    info.vst3Uid = {0x12345678u, 0x9abcdef0u, 0x0fedcba9u, 0x87654321u};
    info.isInstrument = instrument;
    info.state = std::move(state);
    info.controllerState = {9, 8, 7};
    d.plugin = info;
    model::allocateDeviceIds(song, d);
    return d;
}

model::Device groupRack(model::Song& song, const std::string& type)
{
    auto rack = devices::nativeDeviceModel(type);
    rack.displayName = model::groupRackName(type);
    rack.numVisibleMacros = 8;
    rack.id = -1;
    model::allocateDeviceIds(song, rack);
    return rack;
}

std::vector<std::string> types(const std::vector<model::Device>& list)
{
    std::vector<std::string> out;
    for (const auto& d : list) out.push_back(d.type);
    return out;
}

double paramOf(const model::Device& d, const std::string& path) { return d.param(path, -999.0); }

void setParam(model::Device& d, const std::string& path, double value)
{
    for (auto& p : d.parameters)
        if (p.path == path) p.value = value;
}

juce::File freshDirectory(const char* name)
{
    auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("sonora_device_groups").getChildFile(name);
    dir.deleteRecursively();
    dir.createDirectory();
    return dir;
}

std::unique_ptr<model::Song> newSet(const juce::File& file)
{
    std::string error;
    REQUIRE(io::writeAlsDom(*io::createNewSetDom(), file, error));
    auto loaded = io::loadAls(file);
    REQUIRE((bool) loaded);
    return std::move(loaded.song);
}

int firstTrackOf(const model::Song& song, model::TrackKind kind)
{
    for (size_t i = 0; i < song.tracks.size(); ++i)
        if (song.tracks[i].kind == kind) return (int) i;
    return -1;
}

void collectPointeeIds(const juce::XmlElement& e, std::multiset<int>& ids)
{
    if (e.hasTagName("AutomationTarget") || e.hasTagName("ModulationTarget") || e.hasTagName("Pointee"))
        ids.insert(e.getIntAttribute("Id", -1));
    for (const auto* c : e.getChildIterator()) collectPointeeIds(*c, ids);
}
} // namespace

TEST_CASE("device groups: the rack type follows the devices, only adjacent devices group (DEV-15c)")
{
    model::Song song;
    std::vector<model::Device> list{native(song, "MidiPitcher"), native(song, "OriginalSimpler"), native(song, "Eq8"),
                                    native(song, "Reverb"), vst3(song, "Some Synth", {1}, true)};
    CHECK(model::groupRackType(list, 2, 2) == "AudioEffectGroupDevice");
    CHECK(model::groupRackType(list, 0, 1) == "MidiEffectGroupDevice");
    CHECK(model::groupRackType(list, 0, 2) == "InstrumentGroupDevice"); // a MIDI effect and its instrument
    CHECK(model::groupRackType(list, 0, 4) == "InstrumentGroupDevice");
    CHECK(model::groupRackType(list, 4, 1) == "InstrumentGroupDevice"); // a hosted instrument
    CHECK(model::groupRackType(list, 3, 3).empty());                     // out of range
    std::vector<model::Device> noInstrument{native(song, "MidiPitcher"), native(song, "Eq8")};
    CHECK(model::groupRackType(noInstrument, 0, 2).empty());              // MIDI and audio effects with nothing between
    model::Device unknown;
    unknown.type = "SomeFutureDevice";
    noInstrument.push_back(unknown);
    CHECK(model::groupRackType(noInstrument, 1, 2).empty());              // a device we know nothing about
    model::Device max;
    max.kind = model::DeviceKind::MaxForLive;
    max.type = "MxDeviceAudioEffect";
    std::vector<model::Device> withMax{native(song, "Eq8"), max};
    CHECK(model::groupRackType(withMax, 0, 2) == "AudioEffectGroupDevice");

    int first = -1, count = 0;
    CHECK(model::groupableDevices(list, {list[3].id, list[2].id}, first, count));
    CHECK(first == 2);
    CHECK(count == 2);
    CHECK(! model::groupableDevices(list, {list[1].id, list[3].id}, first, count)); // a gap
    CHECK(! model::groupableDevices(list, {}, first, count));
    CHECK(! model::groupableDevices(list, {12345}, first, count));
    CHECK(model::groupRackName("MidiEffectGroupDevice") == "MIDI Effect Rack");
}

TEST_CASE("device groups: Group moves the devices into one chain of a new rack, undo and redo keep their state (DEV-15c)")
{
    model::Song song;
    model::Track track;
    track.kind = model::TrackKind::Audio;
    track.id = 4;
    track.devices = {native(song, "StereoGain"), native(song, "Eq8"), vst3(song, "Sonora Phase Align", {1, 2, 3}), native(song, "Reverb")};
    song.tracks.push_back(track);
    const auto ids = std::vector<int>{song.tracks[0].devices[1].id, song.tracks[0].devices[2].id};
    auto rack = groupRack(song, "AudioEffectGroupDevice");
    const int rackId = rack.id;
    CHECK(rackId != ids[0]);
    CHECK(! model::groupDevices(song, 0, ids, devices::nativeDeviceModel("InstrumentGroupDevice")).apply); // wrong rack, no id

    model::EditHistory history;
    auto command = model::groupDevices(song, 0, ids, rack);
    REQUIRE((bool) command.apply);
    CHECK(command.devicesChanged);
    CHECK(command.label == "Group Devices");
    history.perform(song, std::move(command));
    auto& devices = song.tracks[0].devices;
    REQUIRE(devices.size() == 3);
    CHECK(types(devices) == (std::vector<std::string>{"StereoGain", "AudioEffectGroupDevice", "Reverb"}));
    const auto& grouped = devices[1];
    CHECK(grouped.id == rackId);
    CHECK(grouped.kind == model::DeviceKind::Rack);
    CHECK(grouped.displayName == "Audio Effect Rack");
    CHECK(grouped.chainDevicesVisible);
    REQUIRE(grouped.chains.size() == 1);
    CHECK(grouped.chains[0].name == "EQ Eight");
    REQUIRE(grouped.chains[0].devices.size() == 2);
    CHECK(grouped.chains[0].devices[0].id == ids[0]); // ids kept: the engine hands the running processors over
    CHECK(grouped.chains[0].devices[1].id == ids[1]);
    CHECK(grouped.chains[0].devices[1].plugin->state == (std::vector<uint8_t>{1, 2, 3}));

    // The state at undo time goes back to the track (the app copies the engine state into the model first).
    devices[1].chains[0].devices[1].plugin->state = {4, 5};
    setParam(devices[1].chains[0].devices[0], "Bands.0/ParameterA/Gain", 6.5);
    history.undo(song);
    REQUIRE(song.tracks[0].devices.size() == 4);
    CHECK(types(song.tracks[0].devices) == (std::vector<std::string>{"StereoGain", "Eq8", "PluginDevice", "Reverb"}));
    CHECK(song.tracks[0].devices[2].plugin->state == (std::vector<uint8_t>{4, 5}));
    CHECK(paramOf(song.tracks[0].devices[1], "Bands.0/ParameterA/Gain") == 6.5);
    CHECK(song.tracks[0].devices[1].id == ids[0]);

    // Redo: the same rack (id, macros as they were left) with the state of now.
    song.tracks[0].devices[2].plugin->state = {6};
    history.redo(song);
    REQUIRE(song.tracks[0].devices.size() == 3);
    CHECK(song.tracks[0].devices[1].id == rackId);
    CHECK(song.tracks[0].devices[1].chains[0].devices[1].plugin->state == (std::vector<uint8_t>{6}));

    // A single device groups too (named after it).
    const int reverbId = song.tracks[0].devices[2].id;
    auto single = model::groupDevices(song, 0, {reverbId}, groupRack(song, "AudioEffectGroupDevice"));
    REQUIRE((bool) single.apply);
    CHECK(single.label == "Group Reverb");
    history.perform(song, std::move(single));
    CHECK(song.tracks[0].devices.size() == 3);
    CHECK(song.tracks[0].devices[2].chains[0].devices[0].id == reverbId);
}

TEST_CASE("device groups: Ungroup puts a one-chain rack's devices back, macro mappings go, ids stay unique (DEV-15c)")
{
    model::Song song;
    model::Track track;
    track.kind = model::TrackKind::Midi;
    track.id = 2;
    auto rack = groupRack(song, "InstrumentGroupDevice");
    model::Device::Chain chain;
    chain.name = "Keys";
    chain.devices = {native(song, "MidiChord"), native(song, "OriginalSimpler"), native(song, "Eq8")};
    auto& mapped = chain.devices[2].parameters.front();
    mapped.macroIndex = 3;
    mapped.rangeMin = 0.0;
    mapped.rangeMax = 1.0;
    const auto mappedPath = mapped.path;
    rack.chains.push_back(chain);
    auto reverb = native(song, "Reverb");
    reverb.id = chain.devices[1].id; // an imported set repeats ids across nesting levels
    track.devices = {rack, reverb};
    song.tracks.push_back(track);

    // Not ungroupable: two chains, a Drum Rack, an empty chain, a plain device.
    auto twoChains = rack;
    twoChains.chains.push_back(chain);
    CHECK(! model::canUngroupDevice(twoChains));
    auto drums = rack;
    drums.type = "DrumGroupDevice";
    CHECK(! model::canUngroupDevice(drums));
    auto empty = rack;
    empty.chains[0].devices.clear();
    CHECK(! model::canUngroupDevice(empty));
    CHECK(! model::canUngroupDevice(reverb));
    CHECK(model::canUngroupDevice(rack));
    CHECK(! model::ungroupDevice(song, 0, reverb.id).apply);

    const int deviceIdBefore = song.nextDeviceId;
    model::EditHistory history;
    auto command = model::ungroupDevice(song, 0, rack.id);
    REQUIRE((bool) command.apply);
    CHECK(command.label == "Ungroup Instrument Rack");
    history.perform(song, std::move(command));
    auto& devices = song.tracks[0].devices;
    REQUIRE(devices.size() == 4);
    CHECK(types(devices) == (std::vector<std::string>{"MidiChord", "OriginalSimpler", "Eq8", "Reverb"}));
    std::set<int> ids;
    for (const auto& d : devices) ids.insert(d.id);
    CHECK(ids.size() == 4);                              // the Simpler that shared the Reverb's id got a fresh one
    CHECK(devices[1].id >= deviceIdBefore);
    CHECK(song.nextDeviceId > devices[1].id);
    CHECK(devices[0].id == chain.devices[0].id);         // the others keep theirs
    for (const auto& p : devices[2].parameters)
        if (p.path == mappedPath) { CHECK(p.macroIndex == -1); CHECK(p.macroMappingEdited); }

    // Undo: the rack again, with its chain name, mapping and original ids.
    history.undo(song);
    REQUIRE(song.tracks[0].devices.size() == 2);
    const auto& back = song.tracks[0].devices[0];
    CHECK(back.id == rack.id);
    REQUIRE(back.chains.size() == 1);
    CHECK(back.chains[0].name == "Keys");
    REQUIRE(back.chains[0].devices.size() == 3);
    CHECK(back.chains[0].devices[1].id == chain.devices[1].id);
    for (const auto& p : back.chains[0].devices[2].parameters)
        if (p.path == mappedPath) { CHECK(p.macroIndex == 3); CHECK_NEAR(p.rangeMax, 1.0, 1e-9); }
    history.redo(song);
    CHECK(song.tracks[0].devices.size() == 4);
    CHECK(song.tracks[0].devices[1].id == devices[1].id);   // redo gives the same fresh id
}

TEST_CASE("device groups: groups nest, inside a rack chain and around a rack (DEV-15c)")
{
    model::Song song;
    model::Track track;
    track.kind = model::TrackKind::Audio;
    track.devices = {native(song, "Eq8"), native(song, "StereoGain"), native(song, "Reverb")};
    song.tracks.push_back(track);
    model::EditHistory history;
    history.perform(song, model::groupDevices(song, 0, {song.tracks[0].devices[0].id, song.tracks[0].devices[1].id, song.tracks[0].devices[2].id},
                                              groupRack(song, "AudioEffectGroupDevice")));
    REQUIRE(song.tracks[0].devices.size() == 1);
    const int outerId = song.tracks[0].devices[0].id;

    // Group the last two devices of the rack's chain: a rack in the rack (the app edits the track-level rack).
    auto outer = song.tracks[0].devices[0];
    auto* chain = model::chainListAt(outer, {0});
    REQUIRE(chain != nullptr);
    CHECK(model::groupRackType(*chain, 1, 2) == "AudioEffectGroupDevice");
    auto inner = groupRack(song, "AudioEffectGroupDevice");
    const int innerId = inner.id;
    REQUIRE(model::groupDevicesInList(*chain, 1, 2, inner));
    CHECK(types(*chain) == (std::vector<std::string>{"Eq8", "AudioEffectGroupDevice"}));
    history.perform(song, model::replaceDevice(song, 0, 0, outer, "Group Devices"));
    const auto* nested = model::deviceListAt(song.tracks[0], {0, 0, 1, 0});
    REQUIRE(nested != nullptr);
    CHECK(types(*nested) == (std::vector<std::string>{"StereoGain", "Reverb"}));
    CHECK(model::deviceListAt(song.tracks[0], {0, 1}) == nullptr); // no second chain
    CHECK(model::deviceListAt(song.tracks[0], {0}) == nullptr);    // odd path

    // Group the outer rack itself: three levels.
    history.perform(song, model::groupDevices(song, 0, {outerId}, groupRack(song, "AudioEffectGroupDevice")));
    REQUIRE(model::deviceListAt(song.tracks[0], {0, 0, 0, 0, 1, 0}) != nullptr);
    CHECK(model::deviceListAt(song.tracks[0], {0, 0, 0, 0, 1, 0})->size() == 2);

    // Ungroup the inner rack inside the chain, then the outermost one.
    auto top = song.tracks[0].devices[0];
    auto* innerChain = model::chainListAt(top, {0, 0, 0});
    REQUIRE(innerChain != nullptr);
    REQUIRE((*innerChain)[1].id == innerId);
    int next = song.nextDeviceId;
    CHECK(model::ungroupDeviceInList(*innerChain, 1, next) == 2);
    CHECK(types(*innerChain) == (std::vector<std::string>{"Eq8", "StereoGain", "Reverb"}));
    history.perform(song, model::ungroupDevice(song, 0, song.tracks[0].devices[0].id));
    CHECK(song.tracks[0].devices[0].id == outerId);
    // Undo all of it: back to the three devices.
    while (history.canUndo()) history.undo(song);
    CHECK(types(song.tracks[0].devices) == (std::vector<std::string>{"Eq8", "StereoGain", "Reverb"}));
}

TEST_CASE("device groups: a grouped set saves the reference's rack chain and reloads with devices and plug-in state (DEV-15c)")
{
    const auto dir = freshDirectory("save");
    auto song = newSet(dir.getChildFile("Group.als"));
    const int audio = firstTrackOf(*song, model::TrackKind::Audio);
    const int midiTrack = firstTrackOf(*song, model::TrackKind::Midi);
    REQUIRE(audio >= 0);
    REQUIRE(midiTrack >= 0);
    model::EditHistory history;
    auto eq = native(*song, "Eq8");
    setParam(eq, "Bands.0/ParameterA/Gain", 4.25);
    history.perform(*song, model::insertTrackDevice(*song, audio, eq));
    history.perform(*song, model::insertTrackDevice(*song, audio, vst3(*song, "Sonora Phase Align", {10, 20, 30, 40})));
    history.perform(*song, model::insertTrackDevice(*song, audio, native(*song, "Reverb")));
    // A MIDI track: a MIDI effect and an instrument (Instrument Rack), and a MIDI effect alone (MIDI Effect Rack).
    history.perform(*song, model::insertTrackDevice(*song, midiTrack, native(*song, "MidiChord")));
    history.perform(*song, model::insertTrackDevice(*song, midiTrack, native(*song, "MidiPitcher"))); // MIDI effects go first
    history.perform(*song, model::insertTrackDevice(*song, midiTrack, native(*song, "OriginalSimpler")));
    const auto& onAudio = song->tracks[(size_t) audio].devices;
    REQUIRE(onAudio.size() == 3);
    history.perform(*song, model::groupDevices(*song, audio, {onAudio[0].id, onAudio[1].id}, groupRack(*song, "AudioEffectGroupDevice")));
    const auto& onMidi = song->tracks[(size_t) midiTrack].devices;
    REQUIRE(types(onMidi) == (std::vector<std::string>{"MidiPitcher", "MidiChord", "OriginalSimpler"}));
    history.perform(*song, model::groupDevices(*song, midiTrack, {onMidi[1].id, onMidi[2].id}, groupRack(*song, "InstrumentGroupDevice")));
    history.perform(*song, model::groupDevices(*song, midiTrack, {onMidi[0].id}, groupRack(*song, "MidiEffectGroupDevice")));
    REQUIRE(types(song->tracks[(size_t) midiTrack].devices) == (std::vector<std::string>{"MidiEffectGroupDevice", "InstrumentGroupDevice"}));

    const auto saved = dir.getChildFile("Saved.als");
    REQUIRE(io::saveAls(*song, saved).ok);
    // The reference's classes: <AudioEffectBranch><DeviceChain><AudioToAudioDeviceChain Id="0"><Devices>, and so on.
    std::string error;
    auto dom = io::readAlsDom(saved, error);
    REQUIRE(dom != nullptr);
    CHECK(! io::rackBranchesNeedRepair(*dom));
    std::multiset<int> pointees;
    collectPointeeIds(*dom, pointees);
    for (int id : pointees) CHECK(pointees.count(id) == 1);

    auto again = io::loadAls(saved);
    REQUIRE((bool) again);
    const auto& audioDevices = again.song->tracks[(size_t) audio].devices;
    REQUIRE(types(audioDevices) == (std::vector<std::string>{"AudioEffectGroupDevice", "Reverb"}));
    REQUIRE(audioDevices[0].chains.size() == 1);
    const auto& chain = audioDevices[0].chains[0];
    CHECK(chain.name == "EQ Eight");
    REQUIRE(types(chain.devices) == (std::vector<std::string>{"Eq8", "PluginDevice"}));
    CHECK_NEAR(paramOf(chain.devices[0], "Bands.0/ParameterA/Gain"), 4.25, 1e-6);
    REQUIRE(chain.devices[1].plugin.has_value());
    CHECK(chain.devices[1].kind == model::DeviceKind::PluginVst3);
    CHECK(chain.devices[1].plugin->name == "Sonora Phase Align");
    CHECK(chain.devices[1].plugin->state == (std::vector<uint8_t>{10, 20, 30, 40}));
    CHECK(chain.devices[1].plugin->controllerState == (std::vector<uint8_t>{9, 8, 7}));
    const auto& midiDevices = again.song->tracks[(size_t) midiTrack].devices;
    REQUIRE(types(midiDevices) == (std::vector<std::string>{"MidiEffectGroupDevice", "InstrumentGroupDevice"}));
    CHECK(types(midiDevices[0].chains.at(0).devices) == (std::vector<std::string>{"MidiPitcher"}));
    CHECK(types(midiDevices[1].chains.at(0).devices) == (std::vector<std::string>{"MidiChord", "OriginalSimpler"}));
    CHECK(midiDevices[1].displayName == "Instrument Rack");

    // Ungroup on the reloaded set and save again: the devices are back on the track with the plug-in's state.
    model::EditHistory second;
    second.perform(*again.song, model::ungroupDevice(*again.song, audio, audioDevices[0].id));
    second.perform(*again.song, model::ungroupDevice(*again.song, midiTrack, midiDevices[1].id));
    const auto flat = dir.getChildFile("Flat.als");
    REQUIRE(io::saveAls(*again.song, flat).ok);
    auto last = io::loadAls(flat);
    REQUIRE((bool) last);
    const auto& flatAudio = last.song->tracks[(size_t) audio].devices;
    REQUIRE(types(flatAudio) == (std::vector<std::string>{"Eq8", "PluginDevice", "Reverb"}));
    CHECK(flatAudio[1].plugin->state == (std::vector<uint8_t>{10, 20, 30, 40}));
    CHECK_NEAR(paramOf(flatAudio[0], "Bands.0/ParameterA/Gain"), 4.25, 1e-6);
    std::set<int> flatIds;
    for (const auto& d : flatAudio) flatIds.insert(d.id);
    CHECK(flatIds.size() == flatAudio.size());
    CHECK(types(last.song->tracks[(size_t) midiTrack].devices) == (std::vector<std::string>{"MidiEffectGroupDevice", "MidiChord", "OriginalSimpler"}));
}

TEST_CASE("device groups: a MIDI Effect Rack runs its chain's MIDI effects and leaves the audio alone (DEV-15c)")
{
    model::Song song;
    auto pitch = native(song, "MidiPitcher");
    setParam(pitch, "Pitch", 7.0);
    auto rackModel = groupRack(song, "MidiEffectGroupDevice");
    std::vector<model::Device> list{pitch};
    REQUIRE(model::groupDevicesInList(list, 0, 1, rackModel));
    devices::BuildContext context;
    auto processor = devices::createDevice(list[0], context);
    auto* rack = dynamic_cast<devices::RackProcessor*>(processor.get());
    REQUIRE(rack != nullptr);
    CHECK(rack->midiEffectRack());
    CHECK(! rack->isInstrument());
    rack->prepare(48000.0, 256);
    juce::AudioBuffer<float> audio(2, 256);
    for (int i = 0; i < 256; ++i) { audio.setSample(0, i, 0.25f); audio.setSample(1, i, -0.5f); }
    juce::MidiBuffer midi;
    midi.ensureSize(4096);
    midi.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8) 100), 10);
    midi.addEvent(juce::MidiMessage::noteOff(1, 60), 200);
    rack->process(audio, midi);
    std::vector<int> notes;
    for (const auto meta : midi)
        if (meta.getMessage().isNoteOnOrOff()) notes.push_back(meta.getMessage().getNoteNumber());
    CHECK(notes == (std::vector<int>{67, 67})); // transposed by the Pitch inside the rack
    CHECK_NEAR(audio.getSample(0, 100), 0.25f, 0.0f);
    CHECK_NEAR(audio.getSample(1, 100), -0.5f, 0.0f);
    // A muted chain lets no notes through.
    rack->chainMix(0)->active.store(false);
    midi.clear();
    midi.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8) 100), 0);
    rack->process(audio, midi);
    CHECK(midi.getNumEvents() == 0);
}

TEST_CASE("device groups: an ungrouped device saves without the rack's macro mappings, also on On (DEV-15c, Live crashed)")
{
    // A rack child's On switch mapped to a macro (<KeyMidi> channel 16) went out on the track's own chain after Ungroup, and
    // the reference crashed opening the set (no rack for the macro). On is not a modelled parameter, so the writer drops it.
    const auto dir = freshDirectory("macro_on");
    auto song = newSet(dir.getChildFile("Macro.als"));
    const int audio = firstTrackOf(*song, model::TrackKind::Audio);
    REQUIRE(audio >= 0);
    auto reverb = native(*song, "Reverb");
    auto xml = juce::XmlDocument::parse(juce::String(io::deviceTemplate(reverb)));
    REQUIRE(xml != nullptr);
    auto* on = xml->getChildByName("On");
    REQUIRE(on != nullptr);
    auto* key = on->createNewChildElement("KeyMidi");
    key->createNewChildElement("PersistentKeyString")->setAttribute("Value", "");
    key->createNewChildElement("IsNote")->setAttribute("Value", "false");
    key->createNewChildElement("Channel")->setAttribute("Value", 16);
    key->createNewChildElement("NoteOrController")->setAttribute("Value", 2);
    reverb.sourceXml = std::make_shared<const std::string>(xml->toString().toStdString());
    auto rack = groupRack(*song, "AudioEffectGroupDevice");
    std::vector<model::Device> list{reverb};
    REQUIRE(model::groupDevicesInList(list, 0, 1, rack));
    model::EditHistory history;
    history.perform(*song, model::insertTrackDevice(*song, audio, list[0]));
    const auto countMacroKeys = [](const juce::File& file, bool topLevelOnly) {
        std::string error;
        auto dom = io::readAlsDom(file, error);
        REQUIRE(dom != nullptr);
        int found = 0;
        std::function<void(const juce::XmlElement&, bool)> visit = [&](const juce::XmlElement& e, bool inRack) {
            for (const auto* c : e.getChildIterator())
            {
                if (c->hasTagName("KeyMidi") && c->getChildByName("Channel") && c->getChildByName("Channel")->getIntAttribute("Value") == 16
                    && (! topLevelOnly || ! inRack))
                    ++found;
                visit(*c, inRack || c->hasTagName("Branches"));
            }
        };
        visit(*dom, false);
        return found;
    };
    const auto grouped = dir.getChildFile("Grouped.als");
    REQUIRE(io::saveAls(*song, grouped).ok);
    CHECK(countMacroKeys(grouped, false) == 1); // inside the rack: the mapping stays
    auto again = io::loadAls(grouped);
    REQUIRE((bool) again);
    const auto& devices = again.song->tracks[(size_t) audio].devices;
    REQUIRE(devices.size() == 1);
    model::EditHistory second;
    second.perform(*again.song, model::ungroupDevice(*again.song, audio, devices[0].id));
    const auto flat = dir.getChildFile("Flat.als");
    REQUIRE(io::saveAls(*again.song, flat).ok);
    CHECK(countMacroKeys(flat, false) == 0);
    auto last = io::loadAls(flat);
    REQUIRE((bool) last);
    CHECK(last.song->tracks[(size_t) audio].devices.at(0).type == "Reverb");
}
