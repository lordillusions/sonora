#include "TestFramework.h"
#include "core/Edit.h"
#include "devices/PlaceholderSynth.h"
#include "engine/AudioEngine.h"
#include "engine/ClipPlayers.h"
#include "io/als/AlsReader.h"
#include "io/als/AlsWriter.h"
#include "midi/MidiManager.h"
#include "plugins/PluginHost.h"

using namespace sonora;
namespace
{
model::Song controllerSong()
{
    auto loaded = io::loadAls(juce::File(SONORA_TEST_FIXTURES).getChildFile("recording_set.xml"));
    return *loaded.song;
}
model::AutomationEnvelope controller(int index, std::vector<model::AutomationPoint> points)
{
    model::AutomationEnvelope env; env.midiController = index; env.points = std::move(points); return env;
}
const model::AutomationEnvelope* lane(const model::Clip& clip, int index)
{
    for (const auto& env : clip.envelopes) if (env.midiController == index) return &env;
    return nullptr;
}
template <class Recording>
bool drained(Recording& recorder)
{
    for (int i = 0; i < 2000 && ! recorder.finished(); ++i) juce::Thread::sleep(1);
    return recorder.finished();
}
std::vector<std::pair<int, int>> values(const juce::MidiBuffer& midi, int index)
{
    std::vector<std::pair<int, int>> out;
    for (const auto event : midi)
        if (model::midiControllerIndex(event.data, event.numBytes) == index && (event.data[0] & 15) == 0)
            out.emplace_back(event.samplePosition, model::midiControllerValue(index, event.data));
    return out;
}
class ControllerRecorder final : public devices::DeviceProcessor
{
public:
    struct Event { int sample = 0, index = 0, value = 0; };
    std::array<Event, 16384> events{}; size_t count = 0;
    std::string name() const override { return "Controller recorder"; }
    void prepare(double, int) override {}
    void process(juce::AudioBuffer<float>&, juce::MidiBuffer& midi) override
    {
        for (const auto event : midi)
        {
            const int index = model::midiControllerIndex(event.data, event.numBytes);
            if (index >= 0 && (event.data[0] & 15) == 0 && count < events.size())
                events[count++] = {event.samplePosition, index, model::midiControllerValue(index, event.data)};
        }
    }
    bool has(int index, int value) const
    {
        for (size_t i = 0; i < count; ++i) if (events[i].index == index && events[i].value == value) return true;
        return false;
    }
};
}

TEST_CASE("MIDI envelopes: target mapping defaults signed bend and two-byte pressure")
{
    const uint8_t cc[] = {0xb7, 64, 127}, bend[] = {0xe0, 127, 127}, pressure[] = {0xd2, 95};
    CHECK(model::midiControllerIndex(cc, 3) == 66); CHECK(model::midiControllerValue(66, cc) == 127);
    CHECK(model::midiControllerIndex(bend, 3) == 0); CHECK(model::midiControllerValue(0, bend) == 8191);
    CHECK(model::midiControllerIndex(pressure, 2) == 1); CHECK(model::midiControllerValue(1, pressure) == 95);
    CHECK(model::midiControllerIndex(bend, 2) == -1); CHECK(model::midiControllerIndex(cc, 1) == -1);
    CHECK(model::midiControllerDefault(9) == 100); CHECK(model::midiControllerDefault(12) == 64);
    CHECK(model::midiControllerDefault(13) == 127); CHECK(! model::isMidiController(130));
    CHECK(model::quantiseMidiController(0, -9000) == -8192); CHECK(model::quantiseMidiController(3, 9000) == 127);
}

TEST_CASE("MIDI envelopes: exact steps loop restart seek refresh and end releases")
{
    model::Clip clip; clip.currentStart = 4; clip.currentEnd = 8; clip.loop = {0, 2, 0, true};
    clip.envelopes = {controller(66, {{-63072000, 0}, {0.5, 0}, {0.5, 127}, {1.5000000000000002, 127}, {1.5000000000000002, 0}}),
                      controller(0, {{-63072000, 0}, {0.25, 0}, {0.25, -8192}, {1, -8192}, {1, 8191}})};
    auto prepared = engine::prepareClip(clip, std::shared_ptr<engine::SampleSlot>{});
    juce::MidiBuffer midi; midi.ensureSize(150000);
    engine::renderMidiClip(prepared, {10, 100, 4, 100}, midi);
    const auto pedal = values(midi, 66); REQUIRE(pedal.size() == 2);
    CHECK(pedal[0] == std::make_pair(10, 0)); CHECK(pedal[1] == std::make_pair(60, 127));
    const auto bends = values(midi, 0); REQUIRE(bends.size() == 2); CHECK(bends[1] == std::make_pair(35, -8192));
    midi.clear(); engine::renderMidiClip(prepared, {0, 20, 5.4, 100}, midi);
    REQUIRE(values(midi, 66).size() == 1); CHECK(values(midi, 66)[0] == std::make_pair(10, 0));
    midi.clear(); engine::renderMidiClip(prepared, {0, 50, 5.75, 100}, midi);
    auto v = values(midi, 0); REQUIRE(v.size() == 1); CHECK(v[0] == std::make_pair(25, 0)); // loop starts at 6
    midi.clear(); engine::renderMidiClip(prepared, {0, 10, 4.75, 100}, midi, true);
    REQUIRE(values(midi, 66).size() == 1); CHECK(values(midi, 66)[0].second == 127);
    midi.clear(); engine::renderMidiClip(prepared, {0, 50, 7.75, 100}, midi);
    CHECK(values(midi, 0).back() == std::make_pair(25, 0)); CHECK(values(midi, 66).back() == std::make_pair(25, 0));
}

TEST_CASE("MIDI envelopes: linear interpolation block splitting and bounded dense playback")
{
    model::Clip clip; clip.currentEnd = 4; clip.loop.enabled = false;
    clip.envelopes = {controller(3, {{0, 0}, {1, 100}})};
    auto prepared = engine::prepareClip(clip, std::shared_ptr<engine::SampleSlot>{});
    juce::MidiBuffer whole, split; whole.ensureSize(150000); split.ensureSize(150000);
    engine::renderMidiClip(prepared, {0, 100, 0, 100}, whole);
    engine::renderMidiClip(prepared, {0, 37, 0, 100}, split);
    engine::renderMidiClip(prepared, {37, 63, 0.37, 100}, split);
    CHECK(values(whole, 3) == values(split, 3)); CHECK(values(whole, 3).back() == std::make_pair(99, 99));
    for (int index = 0; index < 130; ++index)
        clip.envelopes.push_back(controller(index, {{0, 0}, {0.0001, index == 0 ? 8191.0 : 127.0}, {4, 0}}));
    prepared = engine::prepareClip(clip, std::shared_ptr<engine::SampleSlot>{});
    whole.clear(); bool overflow = false;
    engine::renderMidiClip(prepared, {0, 2000, 0, 500}, whole, true, &overflow);
    CHECK(overflow); CHECK(whole.data.size() <= engine::midiControllerByteLimit);
    engine::releaseMidiControllers(whole, 1999); CHECK(whole.data.size() < 150000);
}

TEST_CASE("MIDI recording: preroll controllers channel filtering pressure and note-off timing")
{
    auto song = controllerSong(); song.punchIn = song.punchOut = true; song.loopStart = 1; song.loopLength = 1;
    engine::ControlBank controls(3); controls[0].armed.store(true);
    controls[0].inputRoute.store(model::packInput({model::InputKind::Midi, 2, {}}));
    engine::Recorder recorder; std::string error; juce::TemporaryFile folder;
    REQUIRE(recorder.start(song, controls, folder.getFile(), 1000, error));
    juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::controllerEvent(2, 64, 127), 10);
    midi.addEvent(juce::MidiMessage::controllerEvent(1, 1, 55), 110); // wrong channel
    midi.addEvent(juce::MidiMessage::noteOn(2, 60, (juce::uint8) 90), 120);
    midi.addEvent(juce::MidiMessage::channelPressureChange(2, 95), 130);
    midi.addEvent(juce::MidiMessage::pitchWheel(2, 0), 140);
    midi.addEvent(juce::MidiMessage::noteOff(2, 60), 150);
    midi.addEvent(juce::MidiMessage::controllerEvent(2, 64, 0), 180);
    recorder.capture(nullptr, 0, midi, 0, 100, 250, true, false, false);
    REQUIRE(drained(recorder)); auto result = recorder.takeResult(); REQUIRE(result.clips.size() == 1);
    const auto& clip = result.clips[0].second; REQUIRE(clip.notes.size() == 1);
    CHECK_NEAR(clip.notes[0].duration, 0.3, 1e-9); // sustain does not stretch the saved note
    REQUIRE(lane(clip, 66)); REQUIRE(lane(clip, 1)); REQUIRE(lane(clip, 0)); CHECK(! lane(clip, 3));
    CHECK_NEAR(engine::AutomationLane(*lane(clip, 66)).valueAt(0), 127, 1e-9);
    CHECK_NEAR(engine::AutomationLane(*lane(clip, 66)).valueAt(0.8), 0, 1e-9);
    CHECK_NEAR(engine::AutomationLane(*lane(clip, 1)).valueAt(0.3), 95, 1e-9);
    CHECK_NEAR(engine::AutomationLane(*lane(clip, 0)).valueAt(0.4), -8192, 1e-9);
}

TEST_CASE("MIDI overdub: controller-only partial replacement retains other lanes and restores boundaries")
{
    auto song = controllerSong(); auto clip = song.tracks[0].arrangementClips[0];
    clip.loop = {0, 4, 0, true}; clip.envelopes = {controller(3, {{-63072000, 10}, {0, 10}, {1.25, 22.5}, {1.25, 0}, {1.25, 22.5}, {4, 50}}), controller(66, {{0, 127}})};
    model::setSessionClip(song, 0, 1, clip).apply(song); const int uid = song.tracks[0].slots[1].clip->uid;
    engine::ControlBank controls(3); controls[0].armed.store(true); engine::Recorder recorder;
    engine::Recorder::SessionPlan plan; plan.targets = {{0, 1, uid}};
    std::string error; juce::TemporaryFile folder; REQUIRE(recorder.start(song, controls, folder.getFile(), 1000, error, false, &plan));
    recorder.beginSessionContent(0, 1);
    juce::MidiBuffer midi; midi.addEvent(juce::MidiMessage::controllerEvent(1, 1, 100), 25);
    recorder.capture(nullptr, 0, midi, 0, 100, 100, true, false, false);
    recorder.requestStop(); recorder.capture(nullptr, 0, midi, 1, 100, 0, true, false, false);
    REQUIRE(drained(recorder)); auto result = recorder.takeResult(); REQUIRE(result.session.size() == 1);
    const auto& after = result.session[0].clip; CHECK(after.uid == uid); CHECK(after.notes.size() == clip.notes.size());
    REQUIRE(lane(after, 3)); const engine::AutomationLane mod(*lane(after, 3));
    CHECK_NEAR(mod.valueAt(1), 20, 1e-9); CHECK_NEAR(mod.valueAt(1.249), 22.49, 1e-9); CHECK_NEAR(mod.valueAt(1.25), 100, 1e-9);
    CHECK_NEAR(mod.valueAt(1.9), 100, 1e-9); CHECK_NEAR(mod.valueAt(2), 30, 1e-9); CHECK_NEAR(mod.valueAt(3), 40, 1e-9);
    REQUIRE(lane(after, 66)); CHECK(lane(after, 66)->points.size() == 1); CHECK_NEAR(lane(after, 66)->points[0].value, 127, 1e-9);
}

TEST_CASE("MIDI envelopes: descriptors and clip lanes save undo redo with stable IDs and unknown metadata")
{
    auto song = controllerSong(); auto clip = song.tracks[0].arrangementClips[0];
    clip.envelopes = {controller(0, {{-63072000, 0}, {0.5, 0}, {0.5, -4096}}), controller(66, {{0, 127}})};
    auto targets = model::assignRecordedMidiTargets(song, 0, {&clip}); REQUIRE(targets.apply);
    const int bendId = clip.envelopes[0].targetId; CHECK(bendId >= 0); const int next = song.nextPointeeId;
    model::EditHistory history; std::vector<model::EditCommand> edits; edits.push_back(std::move(targets));
    edits.push_back(model::setSessionClip(song, 0, 1, clip)); history.perform(song, model::compose("Record Take", std::move(edits)));
    juce::TemporaryFile saved(".als"); REQUIRE(io::saveAls(song, saved.getFile()).ok);
    auto loaded = io::loadAls(saved.getFile()); REQUIRE(loaded); REQUIRE(loaded.song->tracks[0].slots[1].clip);
    const auto& parsed = *loaded.song->tracks[0].slots[1].clip; REQUIRE(lane(parsed, 0)); REQUIRE(lane(parsed, 66));
    CHECK(lane(parsed, 0)->targetId == bendId); CHECK(lane(parsed, 0)->points.size() == 3);
    CHECK(loaded.song->tracks[0].midiControllerTargets[130] >= 0);
    REQUIRE(history.undo(song)); REQUIRE(io::saveAls(song, saved.getFile()).ok); loaded = io::loadAls(saved.getFile()); REQUIRE(loaded);
    CHECK(! loaded.song->tracks[0].midiControllersPresent); CHECK(song.tracks[0].slots.size() <= 1 || ! song.tracks[0].slots[1].clip); CHECK(song.nextPointeeId == next);
    REQUIRE(history.redo(song)); REQUIRE(io::saveAls(song, saved.getFile()).ok); loaded = io::loadAls(saved.getFile()); REQUIRE(loaded);
    CHECK(loaded.song->tracks[0].midiControllerTargets[0] == bendId);
    auto edited = *loaded.song->tracks[0].slots[1].clip;
    auto xml = juce::parseXML(juce::String(*edited.sourceXml)); REQUIRE(xml);
    auto* env = xml->getChildByName("Envelopes")->getChildByName("Envelopes")->getFirstChildElement();
    env->createNewChildElement("OwnUnknownMetadata")->setAttribute("Value", "retain");
    edited.sourceXml = std::make_shared<const std::string>(xml->toString().toStdString());
    edited.envelopes[1].points[0].value = 0;
    model::modifyClip(*loaded.song, 0, edited.uid, edited, "Controller edit", 1).apply(*loaded.song);
    REQUIRE(io::saveAls(*loaded.song, saved.getFile()).ok); auto again = io::loadAls(saved.getFile()); REQUIRE(again);
    CHECK(again.song->tracks[0].slots[1].clip->sourceXml->find("OwnUnknownMetadata") != std::string::npos);
    CHECK_NEAR(lane(*again.song->tracks[0].slots[1].clip, 66)->points[0].value, 0, 1e-9);
}

TEST_CASE("MIDI recording: controllers survive loop passes and capacity failure retains captured material")
{
    auto song = controllerSong(); song.loopOn = true; song.loopStart = 0; song.loopLength = 1;
    engine::ControlBank controls(3); controls[0].armed.store(true); engine::Recorder recorder; std::string error; juce::TemporaryFile folder;
    REQUIRE(recorder.start(song, controls, folder.getFile(), 1000, error));
    juce::MidiBuffer midi; midi.addEvent(juce::MidiMessage::controllerEvent(1, 64, 127), 25);
    recorder.capture(nullptr, 0, midi, 0, 100, 100, true, false, true);
    midi.clear(); midi.addEvent(juce::MidiMessage::controllerEvent(1, 64, 0), 50);
    recorder.capture(nullptr, 0, midi, 0, 100, 100, true, false, true);
    recorder.requestStop(); recorder.capture(nullptr, 0, midi, 0, 100, 0, true, false, false);
    REQUIRE(drained(recorder)); auto result = recorder.takeResult(); REQUIRE(result.passes.size() == 1); REQUIRE(result.passes[0].second.size() == 2);
    const auto& second = result.passes[0].second[1]; REQUIRE(lane(second, 66));
    CHECK_NEAR(engine::AutomationLane(*lane(second, 66)).valueAt(second.loop.loopStart), 127, 1e-9);
    CHECK_NEAR(engine::AutomationLane(*lane(second, 66)).valueAt(second.loop.loopStart + 0.5), 0, 1e-9);
    song.loopOn = false; REQUIRE(recorder.start(song, controls, folder.getFile(), 1000, error)); midi.clear();
    for (int i = 0; i < 66000; ++i) midi.addEvent(juce::MidiMessage::controllerEvent(1, 1, i & 127), i);
    recorder.capture(nullptr, 0, midi, 0, 100, 66000, true, false, false);
    REQUIRE(drained(recorder)); result = recorder.takeResult(); CHECK(! result.warning.empty()); REQUIRE(result.clips.size() == 1);
    REQUIRE(lane(result.clips[0].second, 3)); CHECK(lane(result.clips[0].second, 3)->points.size() == 131073);
}

TEST_CASE("MIDI envelopes: actual callback refreshes seek and edit, releases stop, and records selected port")
{
    juce::ScopedJuceInitialiser_GUI init; plugins::PluginHost host; midi::MidiManager input; engine::AudioEngine audio(host, input);
    auto song = controllerSong(); song.loopOn = false; song.tracks[0].monitoring = 2;
    song.tracks[0].arrangementClips[0].envelopes = {controller(66, {{0, 127}}), controller(0, {{0, 2048}})};
    audio.loadSong(song); auto device = std::make_unique<ControllerRecorder>(); auto* observed = device.get();
    audio.latestGraph()->nodes[0].devices.clear(); audio.latestGraph()->nodes[0].devices.push_back(std::move(device));
    juce::AudioBuffer<float> output(2, 128); auto run = [&] { audio.renderBlock(output.getArrayOfWritePointers(), 2, 128); };
    audio.transport().play(); run(); CHECK(observed->has(66, 127)); CHECK(observed->has(0, 2048));
    observed->count = 0; run(); CHECK(observed->count == 0); // sustained envelope does not override live input every block
    audio.transport().setPosition(0.5); run(); CHECK(observed->has(66, 127)); CHECK(observed->has(0, 2048));
    observed->count = 0; audio.transport().stop(); run(); CHECK(observed->has(66, 0)); CHECK(observed->has(0, 0));
    observed->count = 0; audio.transport().play(); run(); CHECK(observed->has(66, 127));
    observed->count = 0; song.tracks[0].arrangementClips[0].envelopes[0].points[0].value = 0;
    audio.updateTrackClips(0, song); run(); CHECK(observed->has(66, 0));
    audio.transport().stop(); run(); (*audio.controls())[0].armed.store(true);
    (*audio.controls())[0].inputRoute.store(model::packInput({model::InputKind::Midi, 2, "own-port"}));
    engine::Recorder::SessionPlan plan; plan.targets = {{0, 2, -1}}; plan.immediateStart = true;
    std::string error; juce::TemporaryFile folder; REQUIRE(audio.recorder().start(song, *audio.controls(), folder.getFile(), 44100, error, false, &plan));
    input.handleMessage(juce::MidiMessage::controllerEvent(2, 1, 120), true, false, model::midiPortToken("other-port"));
    input.handleMessage(juce::MidiMessage::controllerEvent(2, 1, 77), true, false, model::midiPortToken("own-port"));
    audio.transport().play(); run(); audio.recorder().requestStop(); run(); REQUIRE(drained(audio.recorder()));
    auto result = audio.recorder().takeResult(); REQUIRE(result.session.size() == 1); REQUIRE(lane(result.session[0].clip, 3));
    CHECK_NEAR(lane(result.session[0].clip, 3)->points.back().value, 77, 1e-9);
}

TEST_CASE("MIDI overdub: replacing an existing jump preserves its left limit")
{
    auto song = controllerSong(); auto clip = song.tracks[0].arrangementClips[0];
    clip.loop = {0, 4, 0, true}; clip.envelopes = {controller(66, {{-63072000, 0}, {0, 0}, {0, 125}, {0.25, 125}, {0.25, 0}})};
    model::setSessionClip(song, 0, 1, clip).apply(song); const int uid = song.tracks[0].slots[1].clip->uid;
    engine::ControlBank controls(3); controls[0].armed.store(true); engine::Recorder recorder;
    engine::Recorder::SessionPlan plan; plan.targets = {{0, 1, uid}};
    std::string error; juce::TemporaryFile folder; REQUIRE(recorder.start(song, controls, folder.getFile(), 1000, error, false, &plan));
    recorder.beginSessionContent(0, 0);
    juce::MidiBuffer midi; midi.addEvent(juce::MidiMessage::controllerEvent(1, 64, 127), 25);
    recorder.capture(nullptr, 0, midi, 0, 100, 100, true, false, false);
    recorder.requestStop(); recorder.capture(nullptr, 0, midi, 1, 100, 0, true, false, false);
    REQUIRE(drained(recorder)); auto result = recorder.takeResult(); REQUIRE(result.session.size() == 1);
    const auto* pedal = lane(result.session[0].clip, 66); REQUIRE(pedal); const engine::AutomationLane saved(*pedal);
    CHECK_NEAR(saved.valueAt(0.125), 125, 1e-9); CHECK_NEAR(saved.valueAt(0.249), 125, 1e-9);
    CHECK_NEAR(saved.valueAt(0.25), 127, 1e-9); CHECK_NEAR(saved.valueAt(1), 0, 1e-9);
}

TEST_CASE("Placeholder: captured sustain and bend change rendered audio without stretching note timing")
{
    devices::PlaceholderSynth synth("own test"); synth.prepare(44100, 4096);
    juce::AudioBuffer<float> output(2, 4096); output.clear(); juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::controllerEvent(1, 64, 127), 0);
    midi.addEvent(juce::MidiMessage::pitchWheel(1, 16383), 0);
    midi.addEvent(juce::MidiMessage::noteOn(1, 69, (juce::uint8) 100), 0);
    midi.addEvent(juce::MidiMessage::noteOff(1, 69), 100);
    synth.process(output, midi); CHECK(output.getRMSLevel(0, 2048, 2048) > 0.03);
    int crossings = 0; for (int i = 1025; i < 4096; ++i)
        if (output.getSample(0, i - 1) <= 0 && output.getSample(0, i) > 0) ++crossings;
    CHECK_NEAR(crossings * 44100.0 / (4096 - 1024), 493.88, 20);
    output.clear(); midi.clear(); midi.addEvent(juce::MidiMessage::controllerEvent(1, 64, 0), 128);
    synth.process(output, midi); CHECK(output.getRMSLevel(0, 3000, 1096) < 0.002);
    synth.reset();
}

TEST_CASE("MIDI controllers: Arrangement lane swaps and dense engine playback retain buffer storage and signal overflow")
{
    juce::ScopedJuceInitialiser_GUI init; plugins::PluginHost host; midi::MidiManager input; engine::AudioEngine audio(host, input);
    auto song = controllerSong(); song.tracks[0].monitoring = 2;
    song.tracks[0].automation = {controller(3, {{0, 55}})};
    audio.loadSong(song); auto device = std::make_unique<ControllerRecorder>(); auto* observed = device.get();
    audio.latestGraph()->nodes[0].devices.clear(); audio.latestGraph()->nodes[0].devices.push_back(std::move(device));
    juce::AudioBuffer<float> output(2, 512); auto run = [&] { audio.renderBlock(output.getArrayOfWritePointers(), 2, 512); };
    audio.transport().play(); run(); CHECK(observed->has(3, 55));
    observed->count = 0; song.tracks[0].automation[0].points[0].value = 90;
    audio.updateTrackClips(0, song); run(); CHECK(observed->has(3, 90));
    auto& clip = song.tracks[0].arrangementClips[0]; clip.envelopes.clear();
    for (int index = 0; index < 130; ++index)
        clip.envelopes.push_back(controller(index, {{0, 0}, {0.01, index == 0 ? 8191.0 : 127.0}, {0.02, 0}, {0.03, 127}}));
    audio.updateTrackClips(0, song); audio.transport().setPosition(0);
    const auto* storage = audio.latestGraph()->nodes[0].midi.data.getRawDataPointer(); run();
    CHECK(storage == audio.latestGraph()->nodes[0].midi.data.getRawDataPointer());
    CHECK((*audio.controls())[0].midiControllerOverflow.load());
    CHECK(observed->has(66, 0)); CHECK(observed->has(0, 0));
}

TEST_CASE("MIDI clip transfers: destination controller targets rebind and one undo restores missing descriptors")
{
    auto song = controllerSong(); song.tracks[1].kind = model::TrackKind::Midi;
    auto& original = song.tracks[0].arrangementClips[0];
    original.envelopes = {controller(0, {{0, 4096}}), controller(66, {{0, 127}})};
    auto setup = model::assignRecordedMidiTargets(song, 0, {&original}); REQUIRE(setup.apply); setup.apply(song);
    const int uid = original.uid, oldTarget = original.envelopes[0].targetId;
    model::EditHistory history; history.perform(song, model::copyClipToTrack(song, 0, uid, 1, 8));
    const auto& copy = song.tracks[1].arrangementClips.back(); REQUIRE(lane(copy, 0));
    const int newTarget = lane(copy, 0)->targetId;
    CHECK(newTarget != oldTarget); CHECK(newTarget == song.tracks[1].midiControllerTargets[0]);
    const int allocator = song.nextPointeeId; REQUIRE(history.undo(song)); CHECK(song.tracks[1].midiControllerTargets[0] == -1);
    REQUIRE(history.redo(song)); CHECK(song.tracks[1].arrangementClips.back().envelopes[0].targetId == newTarget); CHECK(song.nextPointeeId == allocator);
    CHECK(song.tracks[0].arrangementClips[0].envelopes[0].targetId == oldTarget);
}

TEST_CASE("clip envelope edit: allocation occurs on perform and save undo redo retain Float jumps and IDs")
{
    auto song = controllerSong(); auto location = model::ClipLocation::arrangement(0, song.tracks[0].arrangementClips[0].uid);
    const auto notes = song.tracks[0].arrangementClips[0].notes;
    auto env = controller(0, {{2, -4096}, {1, 100.25}, {1, -12000}, {3, 12000}});
    const int next = song.nextPointeeId;
    auto command = model::setClipMidiEnvelope(song, location, env); REQUIRE(command.apply);
    CHECK(song.nextPointeeId == next); CHECK(song.tracks[0].midiControllerTargets[0] < 0);
    model::EditHistory history; history.perform(song, std::move(command));
    const auto* savedLane = lane(*model::findClip(song, location), 0); REQUIRE(savedLane);
    REQUIRE(savedLane->points.size() == 6); CHECK_NEAR(savedLane->points[1].time, 0, 1e-12);
    CHECK_NEAR(savedLane->points[1].value, 0, 1e-12); CHECK_NEAR(savedLane->points[2].value, 100.25, 1e-12);
    CHECK_NEAR(savedLane->points[3].value, -8192, 1e-12); CHECK_NEAR(savedLane->points[5].value, 8191, 1e-12);
    CHECK_NEAR(engine::AutomationLane(*savedLane).valueAt(0.5), 50.125, 1e-12);
    const int id = savedLane->targetId, allocated = song.nextPointeeId;
    CHECK(id == song.tracks[0].midiControllerTargets[0]);
    juce::TemporaryFile output(".als"); REQUIRE(io::saveAls(song, output.getFile()).ok);
    auto loaded = io::loadAls(output.getFile()); REQUIRE(loaded);
    REQUIRE(lane(loaded.song->tracks[0].arrangementClips[0], 0));
    CHECK_NEAR(lane(loaded.song->tracks[0].arrangementClips[0], 0)->points[2].value, 100.25, 1e-12);
    REQUIRE(history.undo(song)); CHECK(model::findClip(song, location)->envelopes.empty());
    CHECK(song.tracks[0].midiControllerTargets[0] < 0); CHECK(song.nextPointeeId == allocated);
    REQUIRE(io::saveAls(song, output.getFile()).ok); loaded = io::loadAls(output.getFile()); REQUIRE(loaded);
    CHECK(! loaded.song->tracks[0].midiControllersPresent);
    REQUIRE(history.redo(song)); CHECK(lane(*model::findClip(song, location), 0)->targetId == id);
    REQUIRE(io::saveAls(song, output.getFile()).ok); loaded = io::loadAls(output.getFile()); REQUIRE(loaded);
    CHECK(loaded.song->tracks[0].midiControllerTargets[0] == id);
    CHECK(model::findClip(song, location)->notes.size() == notes.size());
}

TEST_CASE("clip envelope edit: Session and take locations preserve unrelated lanes and remove only the chosen controller")
{
    auto song = controllerSong(); auto original = song.tracks[0].arrangementClips[0];
    original.envelopes = {controller(3, {{-63072000, 0}, {1, 40.25}, {2, 70}}), controller(66, {{0, 127}})};
    auto targets = model::assignRecordedMidiTargets(song, 0, {&original}); REQUIRE(targets.apply); targets.apply(song);
    model::setSessionClip(song, 0, 1, original).apply(song);
    model::TakeLane take; take.id = 7; take.name = "own take"; original.uid = model::allocateClipUid(); take.clips = {original};
    song.tracks[0].takeLanes = {take};
    const auto session = model::ClipLocation::session(0, song.tracks[0].slots[1].clip->uid, 1);
    const auto takeLocation = model::ClipLocation::take(0, original.uid, 7);
    model::EditHistory history;
    history.perform(song, model::setClipMidiEnvelope(song, session, controller(3, {{1, 120}})));
    CHECK_NEAR(lane(*model::findClip(song, session), 66)->points[0].value, 127, 1e-12);
    CHECK_NEAR(lane(*model::findClip(song, takeLocation), 3)->points[1].value, 40.25, 1e-12);
    auto remove = model::setClipMidiEnvelope(song, takeLocation, controller(66, {}));
    REQUIRE(remove.apply); CHECK(remove.takeLanesOnly); history.perform(song, std::move(remove));
    CHECK(! lane(*model::findClip(song, takeLocation), 66)); REQUIRE(lane(*model::findClip(song, takeLocation), 3));
    CHECK_NEAR(lane(*model::findClip(song, takeLocation), 3)->points[1].value, 40.25, 1e-12);
    REQUIRE(lane(*model::findClip(song, session), 66));
    CHECK(! model::setClipMidiEnvelope(song, model::ClipLocation::session(0, original.uid, 1), controller(3, {{0, 0}})).apply);
    REQUIRE(history.undo(song)); REQUIRE(lane(*model::findClip(song, takeLocation), 66));
    juce::TemporaryFile output(".als"); REQUIRE(io::saveAls(song, output.getFile()).ok);
    auto loaded = io::loadAls(output.getFile()); REQUIRE(loaded); REQUIRE(loaded.song->tracks[0].takeLanes.size() == 1);
    const auto& parsedTake = loaded.song->tracks[0].takeLanes[0].clips[0];
    REQUIRE(lane(parsedTake, 3)); REQUIRE(lane(parsedTake, 66));
    CHECK_NEAR(lane(parsedTake, 3)->points[1].value, 40.25, 1e-12);
    CHECK_NEAR(lane(*loaded.song->tracks[0].slots[1].clip, 3)->points.back().value, 120, 1e-12);
}
