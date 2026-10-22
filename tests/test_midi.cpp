#include "TestFramework.h"

#include "core/FaderCurve.h"
#include "engine/AudioEngine.h"
#include "engine/ControlRouter.h"
#include "io/als/AlsReader.h"
#include "midi/MappingStore.h"
#include "midi/MidiClockOut.h"
#include "midi/MidiManager.h"
#include "plugins/PluginHost.h"

#include <juce_events/juce_events.h>

using namespace sonora;
using midi::ControlChange;
using midi::ControlTarget;
using Kind = ControlTarget::Kind;

namespace
{
struct Rig
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    plugins::PluginHost host;
    midi::MidiManager midi;
    engine::AudioEngine engine{host, midi};
    std::unique_ptr<model::Song> song;

    Rig()
    {
        auto r = io::loadAls(juce::File(SONORA_TEST_FIXTURES).getChildFile("minimal_set.xml"));
        song = std::move(r.song);
        engine.loadSong(*song);
    }
};

void pumpMessages() {} // callbacks are posted inline in these tests
} // namespace

TEST_CASE("midi: relative encoder decoding")
{
    CHECK(midi::decodeRelative(midi::MapMode::Relative2sComplement, 1) == 1);
    CHECK(midi::decodeRelative(midi::MapMode::Relative2sComplement, 127) == -1);
    CHECK(midi::decodeRelative(midi::MapMode::RelativeBinaryOffset, 65) == 1);
    CHECK(midi::decodeRelative(midi::MapMode::RelativeBinaryOffset, 60) == -4);
    CHECK(midi::decodeRelative(midi::MapMode::RelativeSignedBit, 3) == 3);
    CHECK(midi::decodeRelative(midi::MapMode::RelativeSignedBit, 0x43) == -3);
    CHECK(midi::mapModeFromLive(0) == midi::MapMode::Absolute);
    CHECK(midi::mapModeFromLive(6) == midi::MapMode::Relative2sComplement);
}

TEST_CASE("midi: events are placed in the block by arrival time")
{
    CHECK(midi::placeInBlock(10.0, 10.0, 512, 48000.0) == 0);
    CHECK(midi::placeInBlock(10.005, 10.0, 512, 48000.0) == 240);
    CHECK(midi::placeInBlock(9.9, 10.0, 512, 48000.0) == 0);     // older than the block: sample 0
    CHECK(midi::placeInBlock(11.0, 10.0, 512, 48000.0) == 511);  // clamps into the block
}

TEST_CASE("midi: mappings survive a JSON round trip, unknown targets are skipped")
{
    std::vector<midi::Mapping> in;
    midi::Mapping a;
    a.channel = 3;
    a.number = 21;
    a.mode = midi::MapMode::RelativeBinaryOffset;
    a.target = {Kind::DeviceParameter, 2, 1, 0, "Bands.0/ParameterA/Freq"};
    in.push_back(a);
    midi::Mapping b;
    b.channel = 10;
    b.isNote = true;
    b.number = 36;
    b.target.kind = Kind::TransportToggle;
    in.push_back(b);

    juce::TemporaryFile tmp(".json");
    REQUIRE(midi::saveMappings(tmp.getFile(), in));
    std::vector<midi::Mapping> out;
    REQUIRE(midi::loadMappings(tmp.getFile(), out));
    REQUIRE(out.size() == 2);
    CHECK(out[0] == in[0]);
    CHECK(out[1] == in[1]);

    CHECK(midi::mappingsFromVar(juce::JSON::parse(R"([{"channel":1,"number":1,"target":{"kind":"FromTheFuture"}}])")).empty());
    std::vector<midi::Mapping> none;
    CHECK(! midi::loadMappings(juce::File("/nonexistent/x.json"), none));
    const auto folder = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("a/b"); // absolute on every OS
    CHECK(midi::sidecarFor(folder.getChildFile("My Set.als")) == folder.getChildFile("My Set.sonora.json"));
}

TEST_CASE("midi: mappings stored in the .als are imported")
{
    auto r = io::loadAls(juce::File(SONORA_TEST_FIXTURES).getChildFile("minimal_set.xml"));
    REQUIRE(r);
    const auto maps = midi::importLiveMappings(*r.song);
    REQUIRE(maps.size() == 3);
    auto find = [&](Kind k) -> const midi::Mapping* {
        for (const auto& m : maps)
            if (m.target.kind == k)
                return &m;
        return nullptr;
    };
    const auto* vol = find(Kind::TrackVolume);
    REQUIRE(vol != nullptr);
    CHECK(vol->channel == 16); // stored 0-based (15) in the file
    CHECK(vol->number == 7);
    CHECK(vol->target.track == 0);
    const auto* gain = find(Kind::DeviceParameter);
    REQUIRE(gain != nullptr);
    CHECK(gain->target.param == "GlobalGain");
    CHECK(gain->target.device == 1); // second device of the track (EQ Eight)
    CHECK(gain->mode == midi::MapMode::Relative2sComplement);
    const auto* act = find(Kind::TrackActivator);
    REQUIRE(act != nullptr);
    CHECK(act->isNote);
    CHECK(act->number == 36);
}

TEST_CASE("router: volume uses the fader curve, pan/send/buttons/transport work")
{
    Rig rig;
    auto* g = rig.engine.latestGraph();
    auto& ctl = (*g->controls)[0];

    // Absolute volume: position 0.75 -> fader law gain.
    CHECK(engine::applyControl({Kind::TrackVolume, 0, 0, 0, {}}, {0.75f, false, 0, false}, g, rig.engine.transport()));
    CHECK_NEAR(ctl.volume.load(), model::faderPositionToGain(0.75f), 1e-5);
    // Relative: +16 ticks == +0.25 of the travel.
    engine::applyControl({Kind::TrackVolume, 0, 0, 0, {}}, {0.0f, true, 16, false}, g, rig.engine.transport());
    CHECK_NEAR(model::faderGainToPosition(ctl.volume.load()), 1.0, 1e-3);

    engine::applyControl({Kind::TrackPan, 0, 0, 0, {}}, {0.0f, false, 0, false}, g, rig.engine.transport());
    CHECK_NEAR(ctl.pan.load(), -1.0, 1e-6);
    engine::applyControl({Kind::TrackSend, 0, 0, 0, {}}, {0.5f, false, 0, false}, g, rig.engine.transport());
    CHECK_NEAR(ctl.sends[0].load(), model::faderPositionToGain(0.5f), 1e-5);
    CHECK(! engine::applyControl({Kind::TrackSend, 0, 0, 7, {}}, {0.5f, false, 0, false}, g, rig.engine.transport())); // no such send
    CHECK(! engine::applyControl({Kind::TrackVolume, 99, 0, 0, {}}, {0.5f, false, 0, false}, g, rig.engine.transport()));

    // Buttons: a note toggles, a CC follows its value.
    CHECK(ctl.active.load());
    engine::applyControl({Kind::TrackActivator, 0, 0, 0, {}}, {1.0f, false, 0, true}, g, rig.engine.transport());
    CHECK(! ctl.active.load());
    engine::applyControl({Kind::TrackActivator, 0, 0, 0, {}}, {1.0f, false, 0, false}, g, rig.engine.transport());
    CHECK(ctl.active.load());
    engine::applyControl({Kind::TrackSolo, 0, 0, 0, {}}, {1.0f, false, 0, false}, g, rig.engine.transport());
    CHECK(ctl.solo.load());

    // Device parameter on the EQ (device index 1): -12..12 dB, normalised 0.75 -> +6 dB.
    ControlTarget gain{Kind::DeviceParameter, 0, 1, 0, "GlobalGain"};
    CHECK(engine::applyControl(gain, {0.75f, false, 0, false}, g, rig.engine.transport()));
    CHECK_NEAR(*engine::readControl(gain, g, rig.engine.transport()), 0.75, 1e-4);
    auto* eq = g->nodes[0].processorForModelDevice[1];
    REQUIRE(eq != nullptr);
    int idx = -1;
    for (size_t i = 0; i < eq->parameters().size(); ++i)
        if (eq->parameters()[i].id == "GlobalGain")
            idx = (int) i;
    CHECK_NEAR(eq->getParameter(idx), 6.0, 1e-3);
    CHECK(! engine::applyControl({Kind::DeviceParameter, 0, 1, 0, "NoSuchParam"}, {0.5f, false, 0, false}, g, rig.engine.transport()));

    // Device on/off and transport.
    engine::applyControl({Kind::DeviceOn, 0, 1, 0, {}}, {0.0f, false, 0, false}, g, rig.engine.transport());
    CHECK(eq->isBypassed());
    CHECK(! rig.engine.transport().isPlaying());
    engine::applyControl({Kind::TransportToggle, 0, 0, 0, {}}, {1.0f, false, 0, true}, g, rig.engine.transport());
    rig.engine.renderBlock(std::array<float*, 2>{std::vector<float>(64).data(), std::vector<float>(64).data()}.data(), 2, 64);
    CHECK(rig.engine.transport().isPlaying());
}

TEST_CASE("midi manager: learn, roles and routing of simulated input")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    midi::MidiManager mm;
    mm.postToMessageThread = [](std::function<void()> f) { f(); };
    std::vector<std::pair<ControlTarget, ControlChange>> got;
    mm.onControl = [&](const ControlTarget& t, const ControlChange& c) { got.push_back({t, c}); };
    int changed = 0;
    mm.onMappingsChanged = [&] { ++changed; };

    // Learn: the next CC becomes the mapping.
    ControlTarget vol{Kind::TrackVolume, 2, 0, 0, {}};
    mm.beginLearn(vol);
    mm.handleMessage(juce::MidiMessage::controllerEvent(5, 74, 100), false, true);
    pumpMessages();
    REQUIRE(mm.mappings().size() == 1);
    CHECK(mm.mappings()[0].channel == 5);
    CHECK(mm.mappings()[0].number == 74);
    CHECK(changed >= 1);

    // The mapped CC controls the target (and is not forwarded as a track message).
    mm.handleMessage(juce::MidiMessage::controllerEvent(5, 74, 127), true, true);
    pumpMessages();
    REQUIRE(got.size() == 1);
    CHECK(got[0].first == vol);
    CHECK_NEAR(got[0].second.value, 1.0, 1e-6);
    juce::MidiBuffer buf;
    mm.popForAudio(buf, 256, 48000.0);
    CHECK(buf.getNumEvents() == 0);

    // A Remote-only input never reaches the instruments; a Track-only input never maps.
    mm.handleMessage(juce::MidiMessage::noteOn(1, 60, (juce::uint8) 100), false, true);
    mm.popForAudio(buf, 256, 48000.0);
    CHECK(buf.getNumEvents() == 0);
    mm.handleMessage(juce::MidiMessage::controllerEvent(5, 74, 10), true, false);
    pumpMessages();
    CHECK(got.size() == 1);
    mm.popForAudio(buf, 256, 48000.0);
    CHECK(buf.getNumEvents() == 1); // the unmapped-role CC went to the track side

    // Relative mapping and note buttons.
    midi::Mapping rel;
    rel.channel = 1;
    rel.number = 10;
    rel.mode = midi::MapMode::Relative2sComplement;
    rel.target = {Kind::TrackPan, 0, 0, 0, {}};
    mm.addMapping(rel);
    midi::Mapping note;
    note.channel = 1;
    note.isNote = true;
    note.number = 40;
    note.target.kind = Kind::TransportToggle;
    mm.addMapping(note);
    mm.handleMessage(juce::MidiMessage::controllerEvent(1, 10, 127), false, true); // -1 tick
    mm.handleMessage(juce::MidiMessage::noteOn(1, 40, (juce::uint8) 100), false, true);
    pumpMessages();
    REQUIRE(got.size() == 3);
    CHECK(got[1].second.relative);
    CHECK(got[1].second.ticks == -1);
    CHECK(got[2].second.trigger);
}

TEST_CASE("midi clock: 24 ticks per beat land on the right samples")
{
    // 100 samples per beat: ticks every 4.1667 samples. Block of 100 samples from beat 0.
    std::vector<int> offsets;
    midi::forEachClockTick(0.0, 100, 100.0, [&](int o) { offsets.push_back(o); });
    CHECK(offsets.size() == 24);
    CHECK(offsets.front() == 0);
    CHECK(offsets[6] == 25); // quarter of a beat
    // A block starting mid-way only gets the ticks inside it.
    offsets.clear();
    midi::forEachClockTick(0.5, 50, 100.0, [&](int o) { offsets.push_back(o); });
    CHECK(offsets.size() == 12);
    CHECK(offsets.front() == 0);
    // Consecutive blocks never duplicate or skip a tick.
    int total = 0;
    for (int b = 0; b < 8; ++b)
        midi::forEachClockTick(b * 0.3, 30, 100.0, [&](int) { ++total; });
    CHECK(total == 58); // ticks in [0, 2.4) beats: ceil(2.4 * 24) = 58
}

TEST_CASE("midi clock: start/continue/stop/SPP and ticks reach the sink in order")
{
    midi::MidiClockOut clock;
    std::mutex m;
    std::vector<std::vector<int>> got;
    clock.sink = [&](const juce::uint8* b, int n) {
        std::lock_guard lock(m);
        got.push_back({b, b + n});
    };
    clock.setEnabled(true);

    const double t0 = juce::Time::getMillisecondCounterHiRes() * 0.001;
    clock.transportBlock({true, false, 0.0}, t0);           // play from 0 -> Start
    clock.clockSegment(0.0, 0, 100, 100.0, 48000.0, t0);    // 24 ticks within ~2 ms
    clock.transportBlock({false, true, 1.0}, t0 + 0.001);   // stop
    clock.transportBlock({true, false, 2.0}, t0 + 0.002);   // play from beat 2 -> SPP 8 + Continue
    juce::Thread::sleep(120);

    std::lock_guard lock(m);
    REQUIRE(got.size() >= 28);
    CHECK(got[0] == std::vector<int>{0xFA});
    int ticks = 0, stops = 0, conts = 0;
    for (const auto& e : got)
    {
        ticks += e[0] == 0xF8;
        stops += e[0] == 0xFC;
        conts += e[0] == 0xFB;
        if (e[0] == 0xF2)
        {
            REQUIRE(e.size() == 3);
            CHECK(e[1] == 8); // beat 2 == 8 sixteenth notes
        }
    }
    CHECK(ticks == 24);
    CHECK(stops == 1);
    CHECK(conts == 1);
}

#include "core/InputRouting.h"

TEST_CASE("MIDI input: port and channel metadata survive FIFO ordering and bounded variable-size messages")
{
    juce::ScopedJuceInitialiser_GUI init;
    midi::MidiManager manager;
    const auto a = model::midiPortToken("port-a"), b = model::midiPortToken("port-b");
    manager.handleMessage(juce::MidiMessage::noteOn(2, 60, (juce::uint8) 100), true, false, a);
    manager.handleMessage(juce::MidiMessage::noteOn(3, 61, (juce::uint8) 100), true, false, a);
    manager.handleMessage(juce::MidiMessage::noteOn(2, 62, (juce::uint8) 100), true, false, b);
    const uint8_t data[] = {1, 2, 3, 4, 5};
    manager.handleMessage(juce::MidiMessage::createSysExMessage(data, 5), true, false, a);
    midi::InputBlock sources;
    juce::MidiBuffer all, routed;
    all.ensureSize(midi::InputBlock::bufferCapacity);
    routed.ensureSize(midi::InputBlock::bufferCapacity);
    manager.popForAudio(all, 128, 48000, &sources);
    CHECK(all.getNumEvents() == 4);
    sources.addTo(routed, a, 2);
    CHECK(routed.getNumEvents() == 2); // channel 2 note and channel-neutral SysEx from port a
    for (const auto event : routed)
        if (event.numBytes == 3) CHECK(event.data[1] == 60);
    std::vector<uint8_t> huge((size_t) midi::InputBlock::maxBytes, 0);
    manager.handleMessage(juce::MidiMessage::createSysExMessage(huge.data(), (int) huge.size()), true, false, b);
    all.clear();
    manager.popForAudio(all, 128, 48000, &sources);
    CHECK(all.isEmpty());
    CHECK(sources.count == 0);
    CHECK(manager.droppedInputEvents.load() == 1);
}

#include "midi/MidiCapture.h"

TEST_CASE("capture midi: the latest passage becomes notes on whole bars; pauses and earlier takes are left out")
{
    midi::MidiCapture capture;
    CHECK(! capture.available());
    auto on = [](int note, float vel) { return juce::MidiMessage::noteOn(1, note, vel); };
    auto off = [](int note) { return juce::MidiMessage::noteOff(1, note); };

    // an old passage, then a long pause, then the passage we want (transport stopped: wall clock timing at 120 BPM)
    capture.record(on(40, 0.5f), 1.0, 0, false);
    capture.record(off(40), 1.5, 0, false);
    capture.record(on(60, 0.8f), 100.0, 0, false);
    capture.record(off(60), 100.5, 0, false);
    capture.record(on(64, 1.0f), 101.0, 0, false);
    capture.record(off(64), 101.25, 0, false);
    CHECK(capture.available());
    const auto result = capture.take(102.0, 120.0, 4.0);
    REQUIRE(result.has_value());
    REQUIRE(result->notes.size() == 2);
    CHECK(! result->wasPlaying);
    CHECK(result->notes[0].pitch == 60);
    CHECK_NEAR(result->notes[0].start, 0.0, 1e-9);      // free playing starts at the first note
    CHECK_NEAR(result->notes[0].duration, 1.0, 1e-9);    // 0.5 s at 120 BPM
    CHECK(result->notes[1].pitch == 64);
    CHECK_NEAR(result->notes[1].start, 2.0, 1e-9);       // one second later
    CHECK_NEAR(result->notes[1].duration, 0.5, 1e-9);
    CHECK_NEAR(result->length, 4.0, 1e-9);               // rounded up to a whole bar
    CHECK(! capture.available());                         // taking empties the buffer
    CHECK(! capture.take(103.0, 120.0, 4.0).has_value());

    // played while the transport ran: timeline positions, starting on the bar of the first note; a held note is closed at the end
    capture.record(on(48, 0.6f), 10.0, 9.5, true);
    capture.record(off(48), 10.5, 10.5, true);
    capture.record(on(52, 0.6f), 11.0, 11.0, true); // still held
    const auto timeline = capture.take(11.5, 120.0, 4.0);
    REQUIRE(timeline.has_value());
    CHECK(timeline->wasPlaying);
    CHECK_NEAR(timeline->startBeats, 8.0, 1e-9);          // bar containing beat 9.5
    REQUIRE(timeline->notes.size() == 2);
    CHECK_NEAR(timeline->notes[0].start, 1.5, 1e-9);      // 9.5 - 8
    CHECK_NEAR(timeline->notes[0].duration, 1.0, 1e-9);
    CHECK(timeline->notes[1].pitch == 52);
    CHECK_NEAR(timeline->notes[1].start, 3.0, 1e-9);
    CHECK(timeline->notes[1].duration >= 0.25);
    CHECK_NEAR(timeline->length, 4.0, 1e-9);

    // releases of notes held before the passage are dropped
    capture.record(off(70), 20.0, 0, false);
    capture.record(on(61, 0.5f), 20.5, 0, false);
    capture.record(off(61), 21.0, 0, false);
    const auto stray = capture.take(22.0, 120.0, 4.0);
    REQUIRE(stray.has_value());
    CHECK(stray->notes.size() == 1);
}

TEST_CASE("track mappings: regular returns Main device and send controls follow deletion and undo identities")
{
    std::vector<midi::Mapping> mappings(5);
    mappings[0].target = {Kind::TrackVolume, 1, 0, 0, {}};
    mappings[1].target = {Kind::TrackSend, 0, 0, 1, {}};
    mappings[2].target = {Kind::DeviceParameter, 2, 0, 0, "Gain"};
    mappings[3].target = {Kind::TrackVolume, 4, 0, 0, {}};
    mappings[4].target = {Kind::Tempo, 0, 0, 0, {}};
    const std::vector<int> before{10, 11, 20, 21, -1}, after{10, 21, -1};
    midi::rebindTrackMappings(before, after, {20, 21}, {21}, mappings);
    CHECK(mappings[0].target.track == -1); CHECK(mappings[1].target.index == 0);
    CHECK(mappings[2].target.track == -1); CHECK(mappings[3].target.track == 2);
    CHECK(mappings[4].target.track == 0);
    midi::rebindTrackMappings(after, before, {21}, {20, 21}, mappings);
    CHECK(mappings[0].target.track == 1); CHECK(mappings[1].target.index == 1);
    CHECK(mappings[2].target.track == 2); CHECK(mappings[3].target.track == 4);
    midi::rebindTrackMappings(before, {10, 11, 20, -1}, {20, 21}, {20}, mappings);
    CHECK(mappings[1].target.index == -1); // never redirects B's mapping to A
}
