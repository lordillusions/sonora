#include "TestFramework.h"
#include "core/Edit.h"
#include "engine/OverdubPreview.h"
#include "engine/AudioEngine.h"
#include "engine/ClipLauncher.h"
#include "io/als/AlsReader.h"
#include "io/als/AlsWriter.h"
#include "midi/MidiManager.h"
#include "plugins/PluginHost.h"
#include <algorithm>

using namespace sonora;
namespace
{
model::Clip loopClip(double length = 1)
{
    model::Clip clip; clip.uid = 52; clip.currentEnd = length; clip.loop = {0, length, 0, true}; return clip;
}
std::vector<int> onsets(const juce::MidiBuffer& midi, int pitch)
{
    std::vector<int> out; for (const auto event : midi)
        if (event.getMessage().isNoteOn() && event.getMessage().getNoteNumber() == pitch) out.push_back(event.samplePosition);
    return out;
}
bool hasController(const juce::MidiBuffer& midi, int sample, int cc, int value)
{
    for (const auto event : midi) if (event.samplePosition == sample && event.getMessage().isController()
        && event.getMessage().getControllerNumber() == cc && event.getMessage().getControllerValue() == value) return true;
    return false;
}
template <class Recording>
bool drained(Recording& recorder)
{
    for (int i = 0; i < 2000 && ! recorder.finished(); ++i) juce::Thread::sleep(1);
    return recorder.finished();
}
class MidiObserver final : public devices::DeviceProcessor
{
public:
    struct Event { int sample = 0, status = 0, first = 0, second = 0; };
    std::array<Event, 32768> events{}; int count = 0, elapsed = 0;
    std::string name() const override { return "Overdub observer"; }
    void prepare(double, int) override {}
    void process(juce::AudioBuffer<float>& audio, juce::MidiBuffer& midi) override
    {
        for (const auto event : midi) if (event.numBytes >= 2 && count < (int) events.size())
            events[(size_t) count++] = {elapsed + event.samplePosition, event.data[0] & 0xf0,
                                       event.data[1], event.numBytes > 2 ? event.data[2] : 0};
        elapsed += audio.getNumSamples();
    }
    int ons(int pitch) const
    { int n = 0; for (int i = 0; i < count; ++i) if (events[(size_t) i].status == 0x90 && events[(size_t) i].first == pitch && events[(size_t) i].second > 0) ++n; return n; }
    bool allOff() const
    { for (int i = 0; i < count; ++i) if (events[(size_t) i].status == 0xb0 && events[(size_t) i].first == 123) return true; return false; }
};
}

TEST_CASE("continuous overdub: newly closed notes replay on later passes within the same block")
{
    auto clip = loopClip(); clip.notes = {{0, 0.5, 60, 80}};
    engine::OverdubPreview preview(0, 1, clip); juce::MidiBuffer input, output; output.ensureSize(150000);
    input.addEvent(juce::MidiMessage::noteOn(1, 67, (juce::uint8) 100), 25);
    input.addEvent(juce::MidiMessage::noteOff(1, 67), 50);
    engine::BlockSegment seg{0, 350, 0, 100}; REQUIRE(preview.render(seg, input, 0, &seg, false, false, output, true, nullptr));
    CHECK(onsets(output, 67) == std::vector<int>({125, 225, 325}));
    CHECK(onsets(output, 60) == std::vector<int>({0, 100, 200, 300})); CHECK(preview.changed());
}

TEST_CASE("continuous overdub: block splits do not change note or controller replay")
{
    auto clip = loopClip(); engine::OverdubPreview whole(0, 1, clip), split(0, 1, clip);
    juce::MidiBuffer input, a, b; a.ensureSize(150000); b.ensureSize(150000);
    input.addEvent(juce::MidiMessage::noteOn(1, 67, (juce::uint8) 100), 25); input.addEvent(juce::MidiMessage::noteOff(1, 67), 50);
    input.addEvent(juce::MidiMessage::controllerEvent(1, 64, 127), 75); input.addEvent(juce::MidiMessage::controllerEvent(1, 64, 0), 90);
    engine::BlockSegment seg{0, 350, 0, 100}; whole.render(seg, input, 0, &seg, false, false, a, true, nullptr);
    for (int first = 0; first < 350; first += 35)
    {
        engine::BlockSegment part{first, std::min(35, 350 - first), first / 100.0, 100};
        split.render(part, input, 0, &part, false, false, b, first == 0, nullptr);
    }
    CHECK(onsets(a, 67) == onsets(b, 67)); CHECK(a.getNumEvents() == b.getNumEvents());
    auto x = a.begin(), y = b.begin();
    for (; x != a.end() && y != b.end(); ++x, ++y)
    { CHECK((*x).samplePosition == (*y).samplePosition); CHECK((*x).getMessage().getRawDataSize() == (*y).getMessage().getRawDataSize()); CHECK(std::equal((*x).data, (*x).data + (*x).numBytes, (*y).data)); }
}

TEST_CASE("continuous overdub: controller patterns survive empty passes and partial stop restores the original lane")
{
    auto clip = loopClip(); model::AutomationEnvelope env; env.midiController = 3; env.points = {{0, 0}, {1, 100}}; clip.envelopes = {env};
    engine::OverdubPreview preview(0, 1, clip); juce::MidiBuffer input, output; output.ensureSize(150000);
    input.addEvent(juce::MidiMessage::controllerEvent(1, 1, 110), 25);
    engine::BlockSegment capture{0, 75, 0, 100}, seg{0, 100, 0, 100};
    preview.render(seg, input, 0, &capture, true, false, output, true, nullptr);
    CHECK(hasController(output, 25, 1, 110)); CHECK(hasController(output, 75, 1, 75));
    input.clear(); output.clear(); seg = {0, 100, 1, 100}; preview.render(seg, input, 0, nullptr, false, true, output, false, nullptr);
    CHECK(hasController(output, 0, 1, 0)); CHECK(hasController(output, 25, 1, 110)); CHECK(hasController(output, 75, 1, 75));
    auto empty = loopClip(); engine::OverdubPreview pattern(0, 1, empty); input.addEvent(juce::MidiMessage::controllerEvent(1, 64, 127), 25);
    input.addEvent(juce::MidiMessage::controllerEvent(1, 64, 0), 50); seg = {0, 350, 0, 100}; output.clear();
    pattern.render(seg, input, 0, &seg, false, false, output, true, nullptr);
    CHECK(hasController(output, 125, 64, 127)); CHECK(hasController(output, 150, 64, 0));
    CHECK(hasController(output, 225, 64, 127)); CHECK(hasController(output, 250, 64, 0));
}

TEST_CASE("continuous overdub: pre-roll notes are promoted at capture-in, channel filters and stop close only input voices")
{
    auto clip = loopClip(2); clip.notes = {{0, 1.5, 60, 80}}; engine::OverdubPreview preview(0, 1, clip);
    juce::MidiBuffer input, output; output.ensureSize(150000);
    input.addEvent(juce::MidiMessage::noteOn(2, 67, (juce::uint8) 100), 10);
    input.addEvent(juce::MidiMessage::noteOn(1, 68, (juce::uint8) 100), 60);
    input.addEvent(juce::MidiMessage::controllerEvent(2, 64, 127), 20);
    engine::BlockSegment seg{0, 150, 0, 100}, capture{50, 100, 0.5, 100};
    preview.render(seg, input, 2, &capture, false, false, output, true, nullptr);
    CHECK(onsets(output, 67).empty()); CHECK(onsets(output, 68).empty()); CHECK(hasController(output, 50, 64, 127));
    preview.finish(1.5, 150, output);
    bool off = false; for (const auto event : output) { CHECK(! event.getMessage().isAllNotesOff()); if (event.getMessage().isNoteOff() && event.getMessage().getNoteNumber() == 67) off = true; }
    CHECK(off);
    input.clear(); seg = {150, 150, 1.5, 100}; preview.render(seg, input, 2, nullptr, false, true, output, false, nullptr);
    CHECK(onsets(output, 67) == std::vector<int>({250})); CHECK(onsets(output, 60) == std::vector<int>({0, 200}));
}

TEST_CASE("continuous overdub: actual callback replays notes and controllers through wraps and commits without all-notes-off")
{
    juce::ScopedJuceInitialiser_GUI init; plugins::PluginHost host; midi::MidiManager input; engine::AudioEngine audio(host, input);
    auto loaded = io::loadAls(juce::File(SONORA_TEST_FIXTURES).getChildFile("recording_set.xml")); REQUIRE(loaded); auto& song = *loaded.song;
    auto clip = song.tracks[0].arrangementClips[0]; clip.loop = {0, 0.03125, 0, true}; clip.notes = {{0, 0.03, 60, 80}};
    model::setSessionClip(song, 0, 1, clip).apply(song); const int uid = song.tracks[0].slots[1].clip->uid;
    song.loopOn = true; song.loopStart = 0; song.loopLength = 0.015625; song.tracks[0].monitoring = 1;
    audio.loadSong(song); auto device = std::make_unique<MidiObserver>(); auto* observed = device.get();
    audio.latestGraph()->nodes[0].devices.clear(); audio.latestGraph()->nodes[0].devices.push_back(std::move(device));
    (*audio.controls())[0].armed.store(true); engine::Recorder::SessionPlan plan; plan.targets = {{0, 1, uid}};
    juce::TemporaryFile folder; std::string error; REQUIRE(audio.recorder().start(song, *audio.controls(), folder.getFile(), 44100, error, false, &plan));
    engine::launchClip(*audio.latestGraph(), audio.transport(), 0, 1, 0);
    juce::AudioBuffer<float> output(2, 128); auto run = [&] { audio.renderBlock(output.getArrayOfWritePointers(), 2, 128); };
    input.handleMessage(juce::MidiMessage::noteOn(1, 67, (juce::uint8) 100), true, false);
    input.handleMessage(juce::MidiMessage::controllerEvent(1, 64, 127), true, false); run();
    input.handleMessage(juce::MidiMessage::noteOff(1, 67), true, false);
    input.handleMessage(juce::MidiMessage::controllerEvent(1, 64, 0), true, false); run();
    for (int i = 0; i < 20; ++i) run(); CHECK(audio.recorder().active()); CHECK(observed->ons(67) >= 4);
    bool laterPedal = false; for (int i = 0; i < observed->count; ++i) if (observed->events[(size_t) i].sample > 680
        && observed->events[(size_t) i].status == 0xb0 && observed->events[(size_t) i].first == 64 && observed->events[(size_t) i].second == 127) laterPedal = true;
    CHECK(laterPedal); audio.recorder().requestSessionStop(); run(); REQUIRE(drained(audio.recorder()));
    auto result = audio.recorder().takeResult(); REQUIRE(result.session.size() == 1); CHECK(result.session[0].clip.notes.size() == 2);
    const int before = observed->ons(67); for (int i = 0; i < 8; ++i) run(); CHECK(observed->ons(67) > before); // drain/model delay retains preview
    std::vector<model::EditCommand> commands; auto targets = model::assignRecordedMidiTargets(song, 0, {&result.session[0].clip}); if (targets.apply) commands.push_back(std::move(targets));
    commands.push_back(model::modifyClip(song, 0, uid, result.session[0].clip, "Overdub", 1)); model::EditHistory history; history.perform(song, model::compose("Record Take", std::move(commands)));
    observed->count = 0; audio.updateTrackClips(0, song); run(); CHECK(! observed->allOff()); CHECK((*audio.controls())[0].playingSlot.load() == 1);
    juce::TemporaryFile saved(".als"); REQUIRE(io::saveAls(song, saved.getFile()).ok); auto again = io::loadAls(saved.getFile()); REQUIRE(again);
    CHECK(again.song->tracks[0].slots[1].clip->notes.size() == 2); REQUIRE(history.undo(song)); CHECK(song.tracks[0].slots[1].clip->notes.size() == 1);
    REQUIRE(history.redo(song)); CHECK(song.tracks[0].slots[1].clip->uid == uid);
}

TEST_CASE("controller overdub saving: later silent passes retain the recorded pattern like the own Live reference")
{
    auto loaded = io::loadAls(juce::File(SONORA_TEST_FIXTURES).getChildFile("recording_set.xml")); REQUIRE(loaded); auto& song = *loaded.song;
    auto clip = song.tracks[0].arrangementClips[0]; clip.loop = {0, 4, 0, true}; model::setSessionClip(song, 0, 1, clip).apply(song);
    engine::ControlBank controls(3); controls[0].armed.store(true); engine::Recorder recorder; engine::Recorder::SessionPlan plan;
    plan.targets = {{0, 1, song.tracks[0].slots[1].clip->uid}}; juce::TemporaryFile folder; std::string error;
    REQUIRE(recorder.start(song, controls, folder.getFile(), 1000, error, false, &plan)); recorder.beginSessionContent(0, 0);
    juce::MidiBuffer midi; midi.addEvent(juce::MidiMessage::controllerEvent(1, 1, 110), 100); midi.addEvent(juce::MidiMessage::controllerEvent(1, 1, 15), 200);
    recorder.capture(nullptr, 0, midi, 0, 100, 400, true, false, false); midi.clear();
    for (int i = 0; i < 10; ++i) recorder.capture(nullptr, 0, midi, 4 + i * 4, 100, 400, true, false, false);
    recorder.requestStop(); recorder.capture(nullptr, 0, midi, 44, 100, 0, true, false, false); REQUIRE(drained(recorder));
    auto result = recorder.takeResult(); REQUIRE(result.session.size() == 1); REQUIRE(result.session[0].clip.envelopes.size() == 1);
    engine::AutomationLane lane(result.session[0].clip.envelopes[0]); CHECK_NEAR(lane.valueAt(0.5), 0, 1e-9); CHECK_NEAR(lane.valueAt(1.5), 110, 1e-9); CHECK_NEAR(lane.valueAt(3), 15, 1e-9);
}

TEST_CASE("continuous controllers: a later touched pass replaces only its interval and preserves previous replay")
{
    auto clip = loopClip(); engine::OverdubPreview preview(0, 1, clip); juce::MidiBuffer input, out; out.ensureSize(150000);
    input.addEvent(juce::MidiMessage::controllerEvent(1, 1, 100), 25); input.addEvent(juce::MidiMessage::controllerEvent(1, 1, 15), 75);
    engine::BlockSegment seg{0, 100, 0, 100}; preview.render(seg, input, 0, &seg, false, false, out, true, nullptr);
    input.clear(); out.clear(); input.addEvent(juce::MidiMessage::controllerEvent(1, 1, 60), 50);
    seg = {0, 100, 1, 100}; engine::BlockSegment range{0, 70, 1, 100}; preview.render(seg, input, 0, &range, true, false, out, false, nullptr);
    CHECK(hasController(out, 25, 1, 100)); CHECK(hasController(out, 50, 1, 60)); CHECK(hasController(out, 75, 1, 15));
    input.clear(); out.clear(); seg = {0, 100, 2, 100}; preview.render(seg, input, 0, nullptr, false, true, out, false, nullptr);
    CHECK(hasController(out, 25, 1, 100)); CHECK(hasController(out, 50, 1, 60)); CHECK(hasController(out, 70, 1, 100)); CHECK(hasController(out, 75, 1, 15));
}

TEST_CASE("continuous overdub: dense replay stays in reserved MIDI storage and graph replacement retires its preview")
{
    auto clip = loopClip(); engine::OverdubPreview preview(0, 1, clip); juce::MidiBuffer input, out; out.ensureSize(150000);
    for (int i = 0; i < 9000; ++i)
    {
        input.addEvent(juce::MidiMessage::noteOn(1, i % 128, (juce::uint8) 100), i);
        input.addEvent(juce::MidiMessage::noteOff(1, i % 128), i + 1);
    }
    engine::BlockSegment seg{0, 20000, 0, 10000}; bool overflow = false; const auto* storage = out.data.getRawDataPointer();
    preview.render(seg, input, 0, &seg, false, false, out, true, &overflow);
    CHECK(overflow); CHECK(storage == out.data.getRawDataPointer()); CHECK(out.data.size() <= engine::midiControllerByteLimit); // overlapped note history is bounded/suppressed
    auto loaded = io::loadAls(juce::File(SONORA_TEST_FIXTURES).getChildFile("recording_set.xml")); REQUIRE(loaded); auto& song = *loaded.song;
    model::setSessionClip(song, 0, 1, song.tracks[0].arrangementClips[0]).apply(song);
    engine::ControlBank controls(3); controls[0].armed.store(true); engine::Recorder recorder; engine::Recorder::SessionPlan plan;
    plan.targets = {{0, 1, song.tracks[0].slots[1].clip->uid}}; juce::TemporaryFile folder; std::string error;
    REQUIRE(recorder.start(song, controls, folder.getFile(), 1000, error, false, &plan));
    engine::SampleCache samples; devices::BuildContext context; auto graph = engine::buildGraph(song, context, samples);
    recorder.beginPreviewBlock(*graph.graph); CHECK(recorder.hasOverdubPreview(0, 1, (uint32_t) plan.targets[0].uid) == false); // queued, not yet monitoring
    graph.graph->nodes[0].sessionClips[1].probabilitySeed = 123456;
    recorder.beginPreviewBlock(*graph.graph); recorder.collectRetiredPreviews();
    recorder.requestStop(); juce::MidiBuffer empty; recorder.capture(nullptr, 0, empty, 0, 100, 0, false, false, false); REQUIRE(drained(recorder)); CHECK(recorder.takeResult().session.empty());
}

TEST_CASE("controller wrap overwrite: receiving-pass input erases later points as in the own Live reference")
{
    auto clip = loopClip(2); model::AutomationEnvelope env; env.midiController = 3;
    env.points = {{-63072000, 0}, {0.4, 0}, {0.4, 110}, {1.4, 110}, {1.4, 15}}; clip.envelopes = {env};
    engine::OverdubPreview preview(0, 1, clip); juce::MidiBuffer input, output; output.ensureSize(150000);
    input.addEvent(juce::MidiMessage::controllerEvent(1, 1, 64), 150); input.addEvent(juce::MidiMessage::controllerEvent(1, 1, 32), 250);
    engine::BlockSegment seg{0, 650, 0, 100}; preview.render(seg, input, 0, &seg, false, false, output, true, nullptr);
    CHECK(hasController(output, 150, 1, 64)); CHECK(hasController(output, 240, 1, 110)); CHECK(hasController(output, 250, 1, 32));
    CHECK(hasController(output, 440, 1, 110)); CHECK(hasController(output, 450, 1, 32)); CHECK(! hasController(output, 550, 1, 64));
    auto loaded = io::loadAls(juce::File(SONORA_TEST_FIXTURES).getChildFile("recording_set.xml")); REQUIRE(loaded); auto& song = *loaded.song;
    auto source = song.tracks[0].arrangementClips[0]; source.loop = clip.loop; source.envelopes = clip.envelopes; model::setSessionClip(song, 0, 1, source).apply(song);
    engine::ControlBank controls(3); controls[0].armed.store(true); engine::Recorder recorder; engine::Recorder::SessionPlan plan;
    plan.targets = {{0, 1, song.tracks[0].slots[1].clip->uid}}; juce::TemporaryFile folder; std::string error;
    REQUIRE(recorder.start(song, controls, folder.getFile(), 1000, error, false, &plan)); recorder.beginSessionContent(0, 0);
    recorder.capture(nullptr, 0, input, 0, 100, 650, true, false, false); recorder.requestStop(); input.clear();
    recorder.capture(nullptr, 0, input, 6.5, 100, 0, true, false, false); REQUIRE(drained(recorder)); auto result = recorder.takeResult(); REQUIRE(result.session.size() == 1);
    engine::AutomationLane saved(result.session[0].clip.envelopes[0]); CHECK_NEAR(saved.valueAt(0.45), 110, 1e-9); CHECK_NEAR(saved.valueAt(0.6), 32, 1e-9); CHECK_NEAR(saved.valueAt(1.6), 32, 1e-9);
}
