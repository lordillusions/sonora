#include "TestFramework.h"
#include "core/Edit.h"
#include "core/InputRouting.h"
#include "engine/AudioEngine.h"
#include "engine/ClipLauncher.h"
#include "engine/ControlRouter.h"
#include "io/als/AlsReader.h"
#include "io/als/AlsWriter.h"
#include "midi/MappingStore.h"
#include "midi/MidiManager.h"
#include "plugins/PluginHost.h"
#include <limits>

using namespace sonora;

namespace
{
struct MappingRig
{
    juce::ScopedJuceInitialiser_GUI init;
    plugins::PluginHost host;
    midi::MidiManager midi;
    engine::AudioEngine audio{host, midi};
    model::Song song;
    MappingRig()
    {
        auto loaded = io::loadAls(juce::File(SONORA_TEST_FIXTURES).getChildFile("two_tracks.xml"));
        REQUIRE(loaded);
        song = std::move(*loaded.song);
        song.tempo = 120; song.loopOn = false; song.globalQuantisation = 0;
        for (auto& track : song.tracks) track.devices.clear();
        audio.loadSong(song);
        midi.postToMessageThread = [](std::function<void()> f) { f(); };
        midi.onControl = [&](const auto& target, const auto& change) {
            engine::applySessionClipControl(target, change, song, audio.latestGraph(), audio.transport(), &audio.recorder());
        };
    }
    void run(int samples = 128)
    {
        juce::AudioBuffer<float> out(2, samples);
        audio.renderBlock(out.getArrayOfWritePointers(), 2, samples);
    }
    midi::Mapping noteMapping() const
    {
        midi::Mapping mapping; mapping.channel = 2; mapping.isNote = true; mapping.number = 40;
        mapping.target = *midi::sessionClipTarget(song, 0, 1);
        return mapping;
    }
};

std::vector<int> noteVelocities(const juce::MidiBuffer& buffer)
{
    std::vector<int> values;
    for (const auto event : buffer)
        if (event.numBytes == 3 && (event.data[0] & 0xf0) == 0x90 && event.data[2] != 0) values.push_back(event.data[2]);
    return values;
}
}

TEST_CASE("clip mappings: note velocity, both release encodings and MIDI learn respect launch modes")
{
    for (int mode = 0; mode < 4; ++mode)
    {
        MappingRig rig;
        auto& clip = *rig.song.tracks[0].slots[1].clip;
        clip.launchMode = mode; clip.velocityAmount = 1;
        rig.audio.updateTrackClips(0, rig.song);
        auto mapping = rig.noteMapping();
        rig.midi.beginLearn(mapping.target);
        rig.midi.handleMessage(juce::MidiMessage::noteOff(2, 40), true, true);
        CHECK(rig.midi.isLearning());
        rig.midi.handleMessage(juce::MidiMessage::noteOn(2, 40, (juce::uint8) 64), true, true);
        CHECK(! rig.midi.isLearning());
        CHECK((*rig.audio.controls())[0].queuedSlot.load() == engine::TrackControls::kNothingQueued);
        rig.midi.handleMessage(juce::MidiMessage::noteOff(2, 40), true, true); // learning release never stops playback
        rig.midi.handleMessage(juce::MidiMessage::noteOn(2, 40, (juce::uint8) 64), true, true);
        rig.run();
        CHECK((*rig.audio.controls())[0].playingSlot.load() == 1);
        const auto velocities = noteVelocities(rig.audio.latestGraph()->nodes[0].midi);
        REQUIRE(velocities.size() == 1);
        CHECK(velocities[0] == 50); // stored note 100 times launch velocity scale 0.5
        if (mode == 2)
        {
            rig.midi.handleMessage(juce::MidiMessage::noteOn(2, 40, (juce::uint8) 100), false, true);
            rig.run(); CHECK((*rig.audio.controls())[0].playingSlot.load() == engine::TrackControls::kNoSlot);
        }
        // Alternate native note-off and note-on/zero velocity.
        rig.midi.handleMessage(mode % 2 == 0 ? juce::MidiMessage::noteOff(2, 40)
                                             : juce::MidiMessage::noteOn(2, 40, (juce::uint8) 0), true, true);
        rig.run();
        CHECK((*rig.audio.controls())[0].playingSlot.load() == (mode == 1 || mode == 2 ? engine::TrackControls::kNoSlot : 1));
        CHECK(rig.audio.latestGraph()->nodes[0].inputMidi.isEmpty()); // mapped down/up never reach instruments
    }
}

TEST_CASE("clip mappings: velocity amount endpoints preserve note-offs, controllers and live input")
{
    MappingRig rig;
    auto& clip = *rig.song.tracks[0].slots[1].clip;
    clip.velocityAmount = 1;
    clip.notes[0].duration = 64.0 / 22050;
    model::AutomationEnvelope controller; controller.midiController = 66; controller.points = {{0, 127}};
    clip.envelopes.push_back(controller);
    rig.audio.updateTrackClips(0, rig.song);
    const auto target = *midi::sessionClipTarget(rig.song, 0, 1);
    CHECK_NEAR(engine::clipLaunchGain(clip, 1.0f / 127), 0, 0);
    CHECK_NEAR(engine::clipLaunchGain(clip, 1), 1, 0);
    CHECK(engine::applySessionClipControl(target, {1.0f / 127, false, 0, true}, rig.song, rig.audio.latestGraph(), rig.audio.transport()));
    const auto* storage = rig.audio.latestGraph()->nodes[0].midi.data.begin();
    rig.run();
    const auto& events = rig.audio.latestGraph()->nodes[0].midi;
    CHECK(events.data.begin() == storage);
    CHECK(noteVelocities(events).empty());
    for (const auto event : events)
        CHECK(! event.getMessage().isNoteOnOrOff()); // do not manufacture note-offs at silence
    CHECK(engine::applySessionClipControl(target, {64.0f / 127, false, 0, true}, rig.song, rig.audio.latestGraph(), rig.audio.transport()));
    rig.run();
    bool off = false, pedal = false;
    for (const auto event : rig.audio.latestGraph()->nodes[0].midi)
    {
        if (event.getMessage().isNoteOff() && event.getMessage().getNoteNumber() == 60 && event.samplePosition == 64) off = true;
        if (event.numBytes == 3 && event.data[0] == 0xb0 && event.data[1] == 64 && event.data[2] == 127) pedal = true;
    }
    CHECK(off); CHECK(pedal);
    clip.velocityAmount = 0;
    CHECK_NEAR(engine::clipLaunchGain(clip, 1.0f / 127), 1, 0);
    clip.velocityAmount = 0.5;
    CHECK_NEAR(engine::clipLaunchGain(clip, 1.0f / 127), 0.5, 0);
    clip.velocityAmount = std::numeric_limits<double>::quiet_NaN();
    CHECK(std::isfinite(engine::clipLaunchGain(clip, 1.0f / 127)));
    CHECK_NEAR(engine::clipLaunchGain(clip, 1.0f / 127), 1, 0);
    clip.velocityAmount = 1;
    CHECK(std::isfinite(engine::clipLaunchGain(clip, std::numeric_limits<float>::quiet_NaN())));
    CHECK_NEAR(engine::clipLaunchGain(clip, std::numeric_limits<float>::quiet_NaN()), 1, 0);
    // Session velocity never changes the separately merged physical input.
    (*rig.audio.controls())[0].monitoring.store(0);
    rig.midi.handleMessage(juce::MidiMessage::noteOn(1, 72, (juce::uint8) 111), true, false);
    rig.run();
    const auto live = noteVelocities(rig.audio.latestGraph()->nodes[0].midi);
    REQUIRE(live.size() == 1); CHECK(live[0] == 111);
}

TEST_CASE("clip mappings: Repeat retains velocity and releasing before quantised Gate launch cancels it")
{
    MappingRig rig;
    auto& clip = *rig.song.tracks[0].slots[1].clip;
    clip.launchMode = 3; clip.velocityAmount = 1; clip.launchQuantisation = 14;
    rig.audio.updateTrackClips(0, rig.song);
    auto mapping = rig.noteMapping(); rig.midi.addMapping(mapping);
    rig.midi.handleMessage(juce::MidiMessage::noteOn(2, 40, (juce::uint8) 64), false, true);
    rig.run(512);
    for (int i = 0; i < 8; ++i) rig.run(512);
    auto velocities = noteVelocities(rig.audio.latestGraph()->nodes[0].midi);
    // A repeat landed in the preceding block; inspect the audio-owned scale as well.
    CHECK_NEAR(rig.audio.latestGraph()->nodes[0].sessionLaunchGain, 0.5, 1e-7);
    for (int velocity : velocities) CHECK(velocity == 50);
    CHECK((*rig.audio.controls())[0].repeatSlot.load() == 1);
    rig.midi.handleMessage(juce::MidiMessage::noteOff(2, 40), false, true); rig.run();
    CHECK((*rig.audio.controls())[0].repeatSlot.load() == engine::TrackControls::kNoSlot);
    CHECK((*rig.audio.controls())[0].playingSlot.load() == 1);
    clip.launchMode = 1; clip.launchQuantisation = 5; // wait for a bar
    rig.midi.handleMessage(juce::MidiMessage::noteOn(2, 40, (juce::uint8) 127), false, true);
    CHECK((*rig.audio.controls())[0].queuedSlot.load() == 1);
    rig.midi.handleMessage(juce::MidiMessage::noteOff(2, 40), false, true);
    CHECK((*rig.audio.controls())[0].queuedSlot.load() == engine::TrackControls::kQueuedStop);
}

TEST_CASE("clip mappings: CC edges, removal while held and legacy note buttons consume releases safely")
{
    juce::ScopedJuceInitialiser_GUI init;
    midi::MidiManager manager; manager.postToMessageThread = [](std::function<void()> f) { f(); };
    std::vector<midi::ControlChange> got;
    manager.onControl = [&](const auto&, const auto& change) { got.push_back(change); };
    midi::Mapping mapping; mapping.target.kind = midi::ControlTarget::Kind::SessionClip; mapping.target.clipUid = 123;
    mapping.number = 40;
    manager.addMapping(mapping);
    for (int value : {127, 127, 0, 0}) manager.handleMessage(juce::MidiMessage::controllerEvent(1, 40, value), true, true);
    REQUIRE(got.size() == 2);
    CHECK_NEAR(got[0].value, 1, 0); CHECK_NEAR(got[1].value, 0, 0);
    manager.handleMessage(juce::MidiMessage::controllerEvent(1, 40, 127), true, true);
    manager.removeMapping(mapping.target);
    REQUIRE(got.size() == 4); CHECK(got.back().released);
    manager.handleMessage(juce::MidiMessage::controllerEvent(1, 40, 0), true, true);
    CHECK(got.size() == 4);
    juce::MidiBuffer input; manager.popForAudio(input, 128, 44100); CHECK(input.isEmpty());
    mapping.isNote = true; mapping.target.kind = midi::ControlTarget::Kind::TransportToggle;
    manager.addMapping(mapping);
    manager.handleMessage(juce::MidiMessage::noteOn(1, 40, (juce::uint8) 10), true, true);
    manager.handleMessage(juce::MidiMessage::noteOff(1, 40), true, true);
    REQUIRE(got.size() == 5); CHECK(got.back().trigger); CHECK_NEAR(got.back().value, 1, 0);
    manager.popForAudio(input, 128, 44100); CHECK(input.isEmpty());
    mapping.isNote = false; mapping.target.kind = midi::ControlTarget::Kind::SessionClip;
    manager.addMapping(mapping);
    manager.handleMessage(juce::MidiMessage::controllerEvent(1, 40, 127), true, true);
    mapping.target.kind = midi::ControlTarget::Kind::TrackVolume;
    manager.addMapping(mapping); // reassign the held source to a legacy control
    manager.handleMessage(juce::MidiMessage::controllerEvent(1, 40, 127), true, true);
    manager.handleMessage(juce::MidiMessage::controllerEvent(1, 40, 0), true, true);
    REQUIRE(got.size() == 9);
    CHECK_NEAR(got[7].value, 1, 0); CHECK_NEAR(got[8].value, 0, 0);
}

TEST_CASE("clip mappings: disabling Remote consumes the old release while preserving subsequent Track-only notes")
{
    juce::ScopedJuceInitialiser_GUI init;
    midi::MidiManager manager; manager.postToMessageThread = [](std::function<void()> f) { f(); };
    std::vector<midi::ControlChange> changes;
    manager.onControl = [&](const auto&, const auto& change) { changes.push_back(change); };
    midi::Mapping mapping; mapping.isNote = true; mapping.number = 40;
    mapping.target.kind = midi::ControlTarget::Kind::SessionClip; mapping.target.clipUid = 123;
    manager.addMapping(mapping);
    const auto port = model::midiPortToken("sonora-test-remote-port");
    manager.handleMessage(juce::MidiMessage::noteOn(1, 40, (juce::uint8) 100), true, true, port);
    manager.setInputRoles("sonora-test-remote-port", "Test", {false, false});
    REQUIRE(changes.size() == 2); CHECK(changes.back().released);
    manager.handleMessage(juce::MidiMessage::noteOff(1, 40), true, false, port);
    juce::MidiBuffer input; manager.popForAudio(input, 128, 44100); CHECK(input.isEmpty());
    manager.handleMessage(juce::MidiMessage::noteOn(1, 40, (juce::uint8) 110), true, false, port);
    manager.handleMessage(juce::MidiMessage::noteOff(1, 40), true, false, port);
    manager.popForAudio(input, 128, 44100); CHECK(input.getNumEvents() == 2);
    CHECK(changes.size() == 2);
    input.clear();
    manager.handleMessage(juce::MidiMessage::noteOn(1, 40, (juce::uint8) 100), true, true, port);
    manager.removeMapping(mapping.target);
    REQUIRE(changes.size() == 4); CHECK(changes.back().released);
    // A fresh unmapped press can arrive before the old physical release (e.g. missing hardware off).
    manager.handleMessage(juce::MidiMessage::noteOn(1, 40, (juce::uint8) 110), true, true, port);
    manager.handleMessage(juce::MidiMessage::noteOff(1, 40), true, true, port);
    manager.popForAudio(input, 128, 44100); CHECK(input.getNumEvents() == 2);
}

TEST_CASE("clip mappings: UID targets follow moves and undo, sidecars bind saved locations and reject replacement clips")
{
    MappingRig rig;
    const auto mapping = rig.noteMapping();
    const auto original = *rig.song.tracks[0].slots[1].clip;
    rig.song.tracks[0].slots[1].clip.reset();
    rig.song.tracks[1].slots.resize(3); rig.song.tracks[1].slots[2].clip = original;
    auto location = midi::sessionClipLocation(rig.song, mapping.target);
    REQUIRE(location); CHECK(location->track == 1); CHECK(location->scene == 2);
    CHECK(*midi::sessionClipTarget(rig.song, 1, 2) == mapping.target);
    auto persisted = midi::mappingsForSave(rig.song, {mapping});
    CHECK(persisted[0].target.trackId == rig.song.tracks[0].id); CHECK(persisted[0].target.index == 1); // unsaved move
    juce::TemporaryFile output(".als");
    REQUIRE(io::saveAls(rig.song, output.getFile()).ok);
    rig.song.sourcePath = output.getFile().getFullPathName().toStdString(); io::refreshClipIdentities(rig.song);
    persisted = midi::mappingsForSave(rig.song, {mapping});
    auto loaded = io::loadAls(output.getFile()); REQUIRE(loaded);
    midi::bindMappingsToSong(*loaded.song, persisted);
    location = midi::sessionClipLocation(*loaded.song, persisted[0].target);
    REQUIRE(location); CHECK(location->track == 1); CHECK(location->scene == 2); CHECK(location->uid != original.uid);
    model::EditHistory history;
    history.perform(rig.song, model::setSessionClip(rig.song, 1, 2, original));
    CHECK(! midi::sessionClipLocation(rig.song, mapping.target));
    CHECK(! engine::applySessionClipControl(mapping.target, {1, false, 0, true}, rig.song, rig.audio.latestGraph(), rig.audio.transport()));
    REQUIRE(history.undo(rig.song)); CHECK(midi::sessionClipLocation(rig.song, mapping.target).has_value());
    REQUIRE(history.redo(rig.song));
    REQUIRE(io::saveAls(rig.song, output.getFile()).ok); io::refreshClipIdentities(rig.song);
    persisted = midi::mappingsForSave(rig.song, {mapping});
    CHECK(persisted[0].target.index == -1);
    loaded = io::loadAls(output.getFile()); REQUIRE(loaded);
    midi::bindMappingsToSong(*loaded.song, persisted);
    CHECK(! midi::sessionClipLocation(*loaded.song, persisted[0].target));
}

TEST_CASE("clip mappings: keyboard and MIDI coexist in sidecars, invalid keys and unknown targets are ignored")
{
    MappingRig rig;
    const auto mapping = rig.noteMapping();
    auto key = mapping; key.keyCode = 'A';
    rig.midi.addMapping(mapping); rig.midi.addMapping(key);
    REQUIRE(rig.midi.mappings().size() == 2);
    juce::TemporaryFile output(".json"); REQUIRE(midi::saveMappings(output.getFile(), rig.midi.mappings()));
    std::vector<midi::Mapping> loaded; REQUIRE(midi::loadMappings(output.getFile(), loaded));
    CHECK(loaded == rig.midi.mappings());
    rig.midi.removeMapping(mapping.target, true); REQUIRE(rig.midi.mappings().size() == 1);
    CHECK(rig.midi.mappings()[0].keyCode == 0);
    CHECK(midi::mappingsFromVar(juce::JSON::parse(R"([{"keyCode":32,"target":{"kind":"SessionClip"}},{"keyCode":65,"target":{"kind":"TrackArm"}}])")).empty());
}

TEST_CASE("clip mappings: audio velocity changes at the launch boundary, Repeat retains it, ordinary launches reset it")
{
    juce::ScopedJuceInitialiser_GUI init;
    plugins::PluginHost host; midi::MidiManager midi; engine::AudioEngine audio(host, midi);
    model::Song song; song.tempo = 120;
    model::Track track; track.kind = model::TrackKind::Audio;
    model::Clip clip; clip.uid = model::allocateClipUid(); clip.kind = model::Clip::Kind::Audio;
    clip.currentEnd = 4; clip.loop = {0, 2, 0, true}; clip.velocityAmount = 1; clip.gain = 0.4;
    track.slots = {{clip}, {clip}}; song.tracks.push_back(track);
    auto data = std::make_shared<engine::SampleData>(); data->sampleRate = 44100;
    data->audio.setSize(2, 88200);
    for (int i = 0; i < 88200; ++i) { data->audio.setSample(0, i, 1); data->audio.setSample(1, i, -1); }
    engine::SampleCache cache; devices::BuildContext context;
    auto built = engine::buildGraph(song, context, cache);
    for (auto& prepared : built.graph->nodes[0].sessionClips)
        prepared = engine::prepareSessionClip(clip, engine::SampleSlot::makeReady(data));
    audio.installGraph(std::move(built.graph));
    auto& graph = *audio.latestGraph();
    engine::launchClipWithMode(graph, audio.transport(), clip, 0, 0, 0, 64.0f / 127);
    juce::AudioBuffer<float> out(2, 512);
    audio.renderBlock(out.getArrayOfWritePointers(), 2, 64);
    for (int i = 0; i < 64; ++i) CHECK_NEAR(out.getSample(0, i), 0.2, 1e-6);
    engine::launchClipWithMode(graph, audio.transport(), clip, 0, 1, 0.01);
    audio.renderBlock(out.getArrayOfWritePointers(), 2, 512);
    for (int i = 0; i < 512; ++i)
    {
        CHECK_NEAR(out.getSample(0, i), i < 157 ? 0.2 : 0.4, 1e-6);
        CHECK_NEAR(out.getSample(1, i), -out.getSample(0, i), 1e-6);
    }
    clip.launchMode = 3;
    engine::launchClipWithMode(graph, audio.transport(), clip, 0, 0, 0.005, 64.0f / 127);
    for (int b = 0; b < 5; ++b) audio.renderBlock(out.getArrayOfWritePointers(), 2, 512);
    for (int i = 0; i < 512; ++i) CHECK_NEAR(out.getSample(0, i), 0.2, 1e-6);
    engine::launchClip(graph, audio.transport(), 0, 1, 0);
    audio.renderBlock(out.getArrayOfWritePointers(), 2, 512);
    for (int i = 0; i < 512; ++i) CHECK_NEAR(out.getSample(0, i), 0.4, 1e-6);
}

TEST_CASE("clip mappings: recording targets reject mapped launch and release through worker drain")
{
    MappingRig rig;
    auto mapping = rig.noteMapping();
    (*rig.audio.controls())[0].armed.store(true);
    engine::Recorder::SessionPlan plan; plan.targets = {{0, 1, mapping.target.clipUid}};
    juce::TemporaryFile folder; std::string error;
    REQUIRE(rig.audio.recorder().start(rig.song, *rig.audio.controls(), folder.getFile(), 44100, error, false, &plan));
    for (bool release : {false, true})
        CHECK(! engine::applySessionClipControl(mapping.target, {1, false, 0, true, release}, rig.song, rig.audio.latestGraph(), rig.audio.transport(), &rig.audio.recorder()));
    rig.audio.transport().play(); rig.run();
    rig.audio.recorder().requestSessionStop(0, false); rig.run();
    for (int i = 0; i < 2000 && ! rig.audio.recorder().finished(); ++i) juce::Thread::sleep(1);
    CHECK(rig.audio.recorder().finished());
    CHECK(! engine::applySessionClipControl(mapping.target, {1, false, 0, true}, rig.song, rig.audio.latestGraph(), rig.audio.transport(), &rig.audio.recorder()));
    (void) rig.audio.recorder().takeResult();
}
