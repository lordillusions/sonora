#include "TestFramework.h"
#include "core/Edit.h"

#include "engine/AudioEngine.h"
#include "engine/AudioFileFormats.h"
#include "devices/RackProcessor.h"
#include "devices/native/Utility.h"
#include "engine/AudioClipPlayer.h"
#include "engine/ClipPlayers.h"
#include "engine/DelayLine.h"
#include "engine/EngineGraph.h"
#include "engine/SampleCache.h"
#include "engine/PeakCache.h"
#include "engine/stretch/Transients.h"
#include "io/als/AlsReader.h"
#include "io/als/AlsWriter.h"
#include "midi/MidiManager.h"
#include "plugins/PluginHost.h"
#include "plugins/Vst3PresetBuilder.h"
#include "devices/DeviceFactory.h"
#include "midi/MappingStore.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <cmath>

using namespace sonora;

namespace
{
engine::PreparedClip makeMidiClip(double start, double end, double loopStart, double loopEnd, bool looping,
                                  std::vector<model::MidiNote> notes)
{
    model::Clip c;
    c.kind = model::Clip::Kind::Midi;
    c.currentStart = start;
    c.currentEnd = end;
    c.loop = {loopStart, loopEnd, 0.0, looping};
    c.notes = std::move(notes);
    return engine::prepareClip(c, std::shared_ptr<engine::SampleSlot>{});
}

struct Ev
{
    bool on;
    int note;
    int sample;
};

std::vector<Ev> collect(const juce::MidiBuffer& b)
{
    std::vector<Ev> out;
    for (const auto m : b)
        out.push_back({m.getMessage().isNoteOn(), m.getMessage().getNoteNumber(), m.samplePosition});
    return out;
}
} // namespace

TEST_CASE("midi clip: note on/off land on the right samples")
{
    // 1 beat == 100 samples to keep numbers readable.
    auto clip = makeMidiClip(4.0, 12.0, 0.0, 8.0, true, {{0.5, 1.0, 60, 100.0f}});
    juce::MidiBuffer buf;
    engine::renderMidiClip(clip, {0, 200, 4.0, 100.0}, buf); // beats 4..6
    auto ev = collect(buf);
    REQUIRE(ev.size() == 2);
    CHECK(ev[0].on);
    CHECK(ev[0].sample == 50);
    CHECK(! ev[1].on);
    CHECK(ev[1].sample == 150);
}

TEST_CASE("midi clip: loops repeat notes")
{
    auto clip = makeMidiClip(0.0, 16.0, 0.0, 4.0, true, {{1.0, 0.5, 64, 100.0f}});
    juce::MidiBuffer buf;
    engine::renderMidiClip(clip, {0, 100, 9.0, 100.0}, buf); // beats 9..10 -> 2nd loop pass, note at 9.0
    auto ev = collect(buf);
    REQUIRE(ev.size() == 2);
    CHECK(ev[0].on);
    CHECK(ev[0].sample == 0);
}

TEST_CASE("midi clip: nothing outside the clip")
{
    auto clip = makeMidiClip(4.0, 8.0, 0.0, 4.0, true, {{0.0, 1.0, 60, 100.0f}});
    juce::MidiBuffer buf;
    engine::renderMidiClip(clip, {0, 100, 8.5, 100.0}, buf);
    CHECK(buf.getNumEvents() == 0);
    engine::renderMidiClip(clip, {0, 100, 2.0, 100.0}, buf);
    CHECK(buf.getNumEvents() == 0);
}

TEST_CASE("midi clip: notes truncated at clip end")
{
    auto clip = makeMidiClip(0.0, 2.0, 0.0, 8.0, true, {{1.5, 4.0, 60, 100.0f}});
    juce::MidiBuffer buf;
    engine::renderMidiClip(clip, {0, 400, 0.0, 100.0}, buf);
    auto ev = collect(buf);
    REQUIRE(ev.size() == 2);
    CHECK(ev[1].sample == 200); // off exactly at clip end (beat 2.0)
}

TEST_CASE("warp markers map beats to seconds")
{
    std::vector<model::WarpMarker> m{{0.0, 0.0}, {1.0, 2.0}}; // 2 beats per second -> 120 bpm
    CHECK_NEAR(engine::warpBeatsToSeconds(m, 1.0, 0.5), 0.5, 1e-9);
    CHECK_NEAR(engine::warpBeatsToSeconds(m, 4.0, 0.5), 2.0, 1e-9); // extrapolated
    CHECK_NEAR(engine::warpBeatsToSeconds({}, 4.0, 0.5), 2.0, 1e-9);
}

TEST_CASE("audio clip: unwarped plays samples at native rate")
{
    auto data = std::make_shared<engine::SampleData>();
    data->sampleRate = 100.0;
    data->audio.setSize(2, 1000);
    for (int i = 0; i < 1000; ++i)
    {
        data->audio.setSample(0, i, (float) i);
        data->audio.setSample(1, i, (float) i);
    }
    model::Clip c;
    c.kind = model::Clip::Kind::Audio;
    c.currentStart = 0.0;
    c.currentEnd = 10.0;
    c.loop = {0.0, 10.0, 0.0, false}; // seconds (unwarped)
    c.warped = false;
    auto clip = engine::prepareClip(c, data);

    juce::AudioBuffer<float> out(2, 10);
    out.clear();
    // tempo 60 -> 1 beat == 1 s; output rate 100 samples/beat == 100 Hz, same as source.
    engine::renderAudioClip(clip, {0, 10, 1.0, 100.0}, 60.0, out);
    CHECK_NEAR(out.getSample(0, 0), 100.0, 1e-3); // 1 s into the file
    CHECK_NEAR(out.getSample(0, 9), 109.0, 1e-3);
}

TEST_CASE("vstpreset builder layout")
{
    std::array<uint32_t, 4> uid{0x56535476, 1, 2, 0xFFFFFFFF};
    CHECK(plugins::vst3ClassIdString(uid) == "565354760000000100000002FFFFFFFF");

    auto bytes = plugins::buildVstPreset(uid, {1, 2, 3}, {});
    REQUIRE(bytes.size() == 48 + 3 + 4 + 4 + 20);
    CHECK(bytes[0] == 'V' && bytes[3] == '3');
    CHECK(bytes[4] == 1);
    CHECK(bytes[48] == 1 && bytes[50] == 3);
    // chunk list offset (int64 LE at byte 40) == 51
    CHECK(bytes[40] == 51);
    CHECK(bytes[51] == 'L');
}

TEST_CASE("vstpreset decoder restores component/controller state and rejects corrupt bounds")
{
    const std::array<uint32_t, 4> uid{0x12345678, 0xabcdef01, 0xdeadbeef, 0xffaabb00};
    const auto preset = plugins::buildVstPreset(uid, {1, 2, 3}, {4, 5});
    model::PluginInfo info;
    REQUIRE(plugins::readVstPreset(preset, info));
    CHECK(info.vst3Uid == uid);
    CHECK(info.state == std::vector<uint8_t>({1, 2, 3}));
    CHECK(info.controllerState == std::vector<uint8_t>({4, 5}));
    const auto original = info.state;
    for (size_t i = 0; i < preset.size(); ++i)
    {
        const std::vector<uint8_t> truncated(preset.begin(), preset.begin() + (ptrdiff_t) i);
        CHECK(! plugins::readVstPreset(truncated, info));
        CHECK(info.state == original);
    }
    auto corrupted = preset;
    corrupted[40] = 0xff;
    CHECK(! plugins::readVstPreset(corrupted, info));
    corrupted = preset;
    corrupted[8] = 'X';
    CHECK(! plugins::readVstPreset(corrupted, info));
}

TEST_CASE("plugin matching prefers VST3 class identity over duplicate display names")
{
    juce::ScopedJuceInitialiser_GUI gui;
    plugins::PluginHost host;
    juce::PluginDescription wrong;
    wrong.name = "Twin";
    wrong.pluginFormatName = "VST3";
    wrong.fileOrIdentifier = "/tmp/Wrong.vst3";
    wrong.uniqueId = 777;
    host.knownPlugins().addType(wrong);
    auto correct = wrong;
    correct.uniqueId = 31810; // JUCE's normalised FUID hash for {1,2,3,4}
    correct.fileOrIdentifier = "/tmp/Correct.vst3";
    host.knownPlugins().addType(correct);
    model::PluginInfo info;
    info.name = "Twin";
    info.vst3Uid = {1, 2, 3, 4};
    auto match = host.findMatch(info, model::DeviceKind::PluginVst3);
    REQUIRE(match);
    CHECK(match->fileOrIdentifier == correct.fileOrIdentifier);
    info.vst3Uid = {};
    match = host.findMatch(info, model::DeviceKind::PluginVst3);
    REQUIRE(match);
    CHECK(match->name == "Twin"); // name fallback remains available when a set has no uid
}

TEST_CASE("device insertion preserves existing processors controls and transport across undo")
{
    juce::ScopedJuceInitialiser_GUI gui;
    auto loaded = io::loadAls(juce::File(SONORA_TEST_FIXTURES).getChildFile("minimal_set.xml"));
    REQUIRE(loaded);
    auto& song = *loaded.song;
    plugins::PluginHost host;
    midi::MidiManager midi;
    engine::AudioEngine engine(host, midi);
    engine.loadSong(song);
    auto* original = engine.latestGraph()->nodes[0].processorForModelDevice[1];
    REQUIRE(original != nullptr);
    original->setParameter(0, 123.0f);
    const float value = original->getParameter(0);
    const auto controls = engine.controls();
    (*controls)[0].armed.store(true);
    engine.transport().advanceTo(9);
    model::Device utility;
    utility.id = 999;
    utility.type = "StereoGain";
    utility.displayName = "Utility";
    model::EditHistory history;
    history.perform(song, model::insertDevice(0, utility));
    engine.updateTrackDevices(0, song);
    auto& node = engine.latestGraph()->nodes[0];
    CHECK(node.processorForModelDevice[1] == original);
    CHECK_NEAR(original->getParameter(0), value, 1e-6);
    REQUIRE(node.processorForModelDevice.back() != nullptr);
    CHECK(engine.controls() == controls);
    CHECK((*controls)[0].armed.load());
    CHECK_NEAR(engine.transport().positionBeats(), 9, 1e-9);
    CHECK(! node.clipsEdited);
    history.undo(song);
    engine.updateTrackDevices(0, song);
    CHECK(node.processorForModelDevice.size() == song.tracks[0].devices.size());
    CHECK(node.processorForModelDevice[1] == original);
    CHECK_NEAR(original->getParameter(0), value, 1e-6);
    history.redo(song);
    engine.updateTrackDevices(0, song);
    CHECK(node.processorForModelDevice[1] == original);
    engine.reprepareGraph();
    CHECK(node.processorForModelDevice[1] == original);
    CHECK_NEAR(original->getParameter(0), value, 1e-6);
    CHECK(engine.controls() == controls);
    CHECK((*controls)[0].armed.load());
    CHECK_NEAR(engine.transport().positionBeats(), 9, 1e-9);
    std::array<float, 128> left{}, right{};
    float* channels[]{left.data(), right.data()};
    engine.renderBlock(channels, 2, 128);
    for (float sample : left) CHECK(std::isfinite(sample));
}

TEST_CASE("graph: builds from fixture without plugin host")
{
    auto r = io::loadAls(juce::File(SONORA_TEST_FIXTURES).getChildFile("minimal_set.xml"));
    REQUIRE(r);
    engine::SampleCache samples;
    devices::BuildContext ctx;
    ctx.pluginHost = nullptr;
    auto g = engine::buildGraph(*r.song, ctx, samples);
    REQUIRE(g.graph != nullptr);
    CHECK(g.graph->nodes.size() == 3); // 1 track + 1 return + main
    CHECK(g.graph->mainNode == 2);
    CHECK(g.graph->controls->size() == 3);
    // VST3 instrument -> placeholder synth, EQ Eight -> native
    CHECK(g.graph->nodes[0].devices.size() == 2);
    CHECK(g.graph->nodes[0].clips.size() == 1);

    g.graph->prepare(48000.0, 256);
    CHECK(g.graph->nodes[0].buffer.getNumSamples() == 256);
}

// ---------------------------------------------------------------- automation (ENG-11)

namespace
{
engine::AutomationLane makeLane(std::vector<model::AutomationPoint> pts, bool stepped = false)
{
    model::AutomationEnvelope e;
    e.stepped = stepped;
    e.points = std::move(pts);
    return engine::AutomationLane(e);
}
} // namespace

TEST_CASE("automation lane: linear interpolation, holds before first and after last point")
{
    const auto lane = makeLane({{-63072000.0, 1.0}, {4.0, 1.0}, {8.0, 0.0}, {12.0, 0.5}});
    CHECK_NEAR(lane.valueAt(-100.0), 1.0, 1e-9);
    CHECK_NEAR(lane.valueAt(0.0), 1.0, 1e-9);
    CHECK_NEAR(lane.valueAt(4.0), 1.0, 1e-9);
    CHECK_NEAR(lane.valueAt(6.0), 0.5, 1e-9);
    CHECK_NEAR(lane.valueAt(8.0), 0.0, 1e-9);
    CHECK_NEAR(lane.valueAt(11.0), 0.375, 1e-9);
    CHECK_NEAR(lane.valueAt(500.0), 0.5, 1e-9);
}

TEST_CASE("automation lane: stepped lanes hold, equal times jump")
{
    const auto stepped = makeLane({{-63072000.0, 1.0}, {16.0, 0.0}}, true);
    CHECK_NEAR(stepped.valueAt(15.99), 1.0, 1e-9);
    CHECK_NEAR(stepped.valueAt(16.0), 0.0, 1e-9);

    const auto jump = makeLane({{0.0, 0.0}, {4.0, 0.0}, {4.0, 1.0}, {8.0, 1.0}});
    CHECK_NEAR(jump.valueAt(3.999), 0.0, 1e-9);
    CHECK_NEAR(jump.valueAt(4.0), 1.0, 1e-9);
    CHECK_NEAR(jump.valueAt(6.0), 1.0, 1e-9);
}

TEST_CASE("automation lane: empty and single point")
{
    CHECK(makeLane({}).empty());
    const auto one = makeLane({{2.0, 0.7}});
    CHECK_NEAR(one.valueAt(-5.0), 0.7, 1e-9);
    CHECK_NEAR(one.valueAt(50.0), 0.7, 1e-9);
}

TEST_CASE("automation lane: bezier curve controls shape the segment that starts at their point (ENG-11b)")
{
    model::AutomationPoint straight{0.0, 0.0};
    CHECK(! straight.curved());
    CHECK_NEAR(model::segmentShape(straight, 0.25), 0.25, 1e-12);
    model::AutomationPoint diagonal{0.0, 0.0, 0.2, 0.2, 0.9, 0.9}; // control points on the diagonal: still a line
    CHECK(! diagonal.curved());
    // Real-world sets: a late, exponential-looking rise ...
    model::AutomationPoint late{0.0, 0.0, 0.578125, 0.140625, 0.859375, 0.421875};
    CHECK(late.curved());
    CHECK_NEAR(model::segmentShape(late, 0.0), 0.0, 1e-9);
    CHECK_NEAR(model::segmentShape(late, 1.0), 1.0, 1e-9);
    double previous = -1;
    for (int i = 0; i <= 100; ++i) // monotonic, and below the straight line everywhere inside
    {
        const double f = i / 100.0, y = model::segmentShape(late, f);
        CHECK(y >= previous - 1e-12);
        if (i > 0 && i < 100) CHECK(y < f);
        previous = y;
    }
    // ... and an extreme one that holds the start value until the very end.
    model::AutomationPoint hold{0.0, 0.0, 0.976190472504778306, 0.0238095274952217321, 0.976190472504778306, 0.0238095274952217321};
    CHECK(model::segmentShape(hold, 0.5) < 0.05);
    CHECK(model::segmentShape(hold, 0.99) > 0.3);

    // The curve belongs to the segment after its point; a plain point after it keeps the next segment straight.
    model::AutomationPoint curvedStart{4.0, 100.0, 0.578125, 0.140625, 0.859375, 0.421875};
    const auto lane = makeLane({{0.0, 0.0}, curvedStart, {8.0, 300.0}, {12.0, 100.0}});
    CHECK_NEAR(lane.valueAt(2.0), 50.0, 1e-9);                                               // straight
    CHECK_NEAR(lane.valueAt(6.0), 100.0 + 200.0 * model::segmentShape(curvedStart, 0.5), 1e-9); // curved
    CHECK(lane.valueAt(6.0) < 200.0);
    CHECK_NEAR(lane.valueAt(10.0), 200.0, 1e-9);                                             // straight again
}

TEST_CASE("automation: curve controls are read from FloatEvents and kept through save")
{
    auto xml = juce::File(SONORA_TEST_FIXTURES).getChildFile("minimal_set.xml").loadFileAsString();
    xml = xml.replace("<FloatEvent Id=\"1\" Time=\"4\" Value=\"1\" />",
                      "<FloatEvent Id=\"1\" Time=\"4\" Value=\"1\" CurveControl1X=\"0.578125\" CurveControl1Y=\"0.140625\" "
                      "CurveControl2X=\"0.859375\" CurveControl2Y=\"0.421875\" />");
    REQUIRE(xml.contains("CurveControl1X"));
    juce::TemporaryFile source(".xml");
    REQUIRE(source.getFile().replaceWithText(xml));
    auto loaded = io::loadAls(source.getFile());
    REQUIRE(loaded);
    const model::AutomationPoint* found = nullptr;
    for (const auto& track : loaded.song->tracks)
        for (const auto& env : track.automation)
            for (const auto& point : env.points)
                if (point.curved()) found = &point;
    REQUIRE(found != nullptr);
    CHECK_NEAR(found->time, 4.0, 1e-12);
    CHECK_NEAR(found->x1, 0.578125, 1e-12);
    CHECK_NEAR(found->y2, 0.421875, 1e-12);

    // Saved: unchanged points keep their element; an edited point writes its own curve from the model.
    auto edited = *loaded.song;
    for (auto& track : edited.tracks)
        for (auto& env : track.automation)
            for (auto& point : env.points)
                if (point.curved()) { point.value = 0.75; point.y1 = 0.3; }
    juce::TemporaryFile saved(".als");
    REQUIRE(io::saveAls(edited, saved.getFile()).ok);
    auto reloaded = io::loadAls(saved.getFile());
    REQUIRE(reloaded);
    found = nullptr;
    for (const auto& track : reloaded.song->tracks)
        for (const auto& env : track.automation)
            for (const auto& point : env.points)
                if (point.curved()) found = &point;
    REQUIRE(found != nullptr);
    CHECK_NEAR(found->value, 0.75, 1e-12);
    CHECK_NEAR(found->y1, 0.3, 1e-12);
    CHECK_NEAR(found->x2, 0.859375, 1e-12);
}

TEST_CASE("automation: fixture envelopes bind to mixer and device targets")
{
    auto r = io::loadAls(juce::File(SONORA_TEST_FIXTURES).getChildFile("minimal_set.xml"));
    REQUIRE(r);
    engine::SampleCache samples;
    devices::BuildContext ctx;
    auto g = engine::buildGraph(*r.song, ctx, samples);
    REQUIRE(g.graph != nullptr);
    auto& node = g.graph->nodes[0];
    // volume, speaker, EQ global gain, EQ on/off; the send has no envelope, target 999 is unknown
    REQUIRE(node.automation.size() == 4);

    bool logged = false;
    for (const auto& l : g.log)
        logged |= l.find("Automation not played") != std::string::npos;
    CHECK(logged);

    auto& ctl = (*g.graph->controls)[0];
    auto* eq = node.devices[1].get();
    REQUIRE(eq != nullptr);
    int gainIndex = -1;
    for (size_t i = 0; i < eq->parameters().size(); ++i)
        if (eq->parameters()[i].id == "GlobalGain")
            gainIndex = (int) i;
    REQUIRE(gainIndex >= 0);

    engine::applyAutomation(node.automation, ctl, 1.0);
    CHECK_NEAR(ctl.volume.load(), 1.0, 1e-6);
    CHECK(ctl.active.load());
    CHECK_NEAR(eq->getParameter(gainIndex), -6.0, 1e-4);
    CHECK(! eq->isBypassed());

    engine::applyAutomation(node.automation, ctl, 6.0);
    CHECK_NEAR(ctl.volume.load(), 0.75, 1e-6);
    CHECK(eq->isBypassed()); // BoolEvent false at beat 2

    engine::applyAutomation(node.automation, ctl, 20.0);
    CHECK_NEAR(ctl.volume.load(), 0.5, 1e-6);
    CHECK(! ctl.active.load()); // track activator off at beat 16
}

TEST_CASE("automation: audio callback applies it while playing, not while stopped")
{
    juce::ScopedJuceInitialiser_GUI juce; // AudioEngine owns a juce::Timer
    plugins::PluginHost host;
    midi::MidiManager midi;
    engine::AudioEngine engine(host, midi);

    auto r = io::loadAls(juce::File(SONORA_TEST_FIXTURES).getChildFile("minimal_set.xml"));
    REQUIRE(r);
    engine.loadSong(*r.song); // no device open -> graph is installed directly

    auto controls = engine.controls();
    REQUIRE(controls != nullptr);
    std::vector<float> l(256), rr(256);
    float* out[2] = {l.data(), rr.data()};

    // Stopped: only the initial locate (to beat 0) applies automation; volume@0 == 1.0
    engine.renderBlock(out, 2, 256);
    CHECK_NEAR((*controls)[0].volume.load(), 1.0, 1e-6);

    // A manual change while stopped is left alone.
    (*controls)[0].volume.store(0.25f);
    engine.renderBlock(out, 2, 256);
    CHECK_NEAR((*controls)[0].volume.load(), 0.25, 1e-6);

    // Playing from beat 6 (volume lane: 1.0 at 4, 0.5 at 8): the lane wins.
    engine.transport().setPosition(6.0);
    engine.transport().play();
    engine.renderBlock(out, 2, 256);
    CHECK_NEAR((*controls)[0].volume.load(), 0.75, 1e-3);
}

TEST_CASE("pan law: centre is unity, far channel attenuates linearly, clamps")
{
    auto g = engine::panGains(0.5f, 0.0f);
    CHECK_NEAR(g.left, 0.5, 1e-6);
    CHECK_NEAR(g.right, 0.5, 1e-6);
    g = engine::panGains(1.0f, 0.5f);
    CHECK_NEAR(g.left, 0.5, 1e-6);
    CHECK_NEAR(g.right, 1.0, 1e-6);
    g = engine::panGains(1.0f, -1.0f);
    CHECK_NEAR(g.left, 1.0, 1e-6);
    CHECK_NEAR(g.right, 0.0, 1e-6);
    g = engine::panGains(1.0f, 7.0f);
    CHECK_NEAR(g.left, 0.0, 1e-6);
}

TEST_CASE("solo: nested groups, parent groups stay audible, soloing a group plays its children")
{
    // Layout: 0 group G1 { 1 audio A, 2 group G2 { 3 audio B } }, 4 audio C
    model::Song song;
    auto add = [&](model::TrackId id, model::TrackKind kind, model::TrackId group) {
        model::Track t;
        t.id = id;
        t.kind = kind;
        t.groupId = group;
        t.name = "t" + std::to_string(id);
        song.tracks.push_back(t);
    };
    add(10, model::TrackKind::Group, -1);
    add(11, model::TrackKind::Audio, 10);
    add(12, model::TrackKind::Group, 10);
    add(13, model::TrackKind::Audio, 12);
    add(14, model::TrackKind::Audio, -1);

    engine::SampleCache samples;
    devices::BuildContext ctx;
    auto g = engine::buildGraph(song, ctx, samples);
    auto& ctl = *g.graph->controls;
    auto audible = [&](size_t i, bool anySolo) { return engine::isTrackAudible(*g.graph, ctl, g.graph->nodes[i], anySolo); };

    for (size_t i = 0; i < 5; ++i)
        CHECK(audible(i, false));

    ctl[3].solo.store(true); // solo B (inside G2 inside G1)
    CHECK(audible(3, true));
    CHECK(audible(2, true));  // parent groups pass
    CHECK(audible(0, true));
    CHECK(! audible(1, true)); // sibling A muted by solo
    CHECK(! audible(4, true)); // unrelated C muted

    ctl[3].solo.store(false);
    ctl[2].solo.store(true); // solo group G2 -> its child plays
    CHECK(audible(3, true));
    CHECK(! audible(1, true));

    ctl[2].solo.store(false);
    ctl[1].active.store(false); // deactivated track is silent even without solo
    CHECK(! audible(1, false));
}

TEST_CASE("audio file formats: m4a, caf and the other CoreAudio containers are accepted and decode")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    for (const char* extension : {"wav", "aif", "aiff", "flac", "ogg", "mp3"})
        CHECK(engine::audioFileExtensions().contains(extension));
    CHECK(! engine::isAudioFile(juce::File("/tmp/Set.als")));
    CHECK(! engine::isAudioFile(juce::File("/tmp/noextension")));
#if JUCE_MAC // CoreAudio containers; Windows/Linux decode the formats above only (docs/PLATFORMS.md)
    for (const char* extension : {"m4a", "aac", "caf", "mp4", "aifc"})
        CHECK(engine::audioFileExtensions().contains(extension));
    CHECK(engine::isAudioFile(juce::File("/tmp/Song.M4A")));
    CHECK(engine::isAudioFile(juce::File("/tmp/Loop.caf")));
    CHECK(engine::audioFileWildcard().contains("*.m4a"));
#else
    CHECK(engine::isAudioFile(juce::File("/tmp/Song.MP3")));
    CHECK(engine::audioFileWildcard().contains("*.mp3"));
#endif

    // Encode a known stereo WAV with the system encoder, then decode it through SampleCache like a dropped clip.
    const juce::File afconvert("/usr/bin/afconvert");
    if (! afconvert.existsAsFile())
        return;
    const auto folder = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("sonora_audio_formats");
    folder.deleteRecursively();
    REQUIRE(folder.createDirectory());
    const auto source = folder.getChildFile("source.wav");
    constexpr int frames = 44100;
    juce::AudioBuffer<float> original(2, frames);
    for (int i = 0; i < frames; ++i)
    {
        original.setSample(0, i, 0.5f * std::sin(2.0f * juce::MathConstants<float>::pi * 440.0f * (float) i / 44100.0f));
        original.setSample(1, i, 0.25f * std::sin(2.0f * juce::MathConstants<float>::pi * 220.0f * (float) i / 44100.0f));
    }
    {
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> stream(source.createOutputStream());
        REQUIRE(stream != nullptr);
        auto writer = wav.createWriterFor(stream, juce::AudioFormatWriterOptions{}.withSampleRate(44100.0).withNumChannels(2).withBitsPerSample(16));
        REQUIRE(writer != nullptr);
        writer->writeFromAudioSampleBuffer(original, 0, frames);
    }

    struct Encoding { const char* name; const char* container; const char* codec; bool lossless; };
    engine::SampleCache cache;
    for (const auto& encoding : {Encoding{"aac.m4a", "m4af", "aac", false}, Encoding{"alac.m4a", "m4af", "alac", true},
                                 Encoding{"pcm.caf", "caff", "LEI16", true}})
    {
        const auto target = folder.getChildFile(encoding.name);
        juce::ChildProcess process;
        REQUIRE(process.start(juce::StringArray{afconvert.getFullPathName(), "-f", encoding.container, "-d", encoding.codec,
                                                source.getFullPathName(), target.getFullPathName()}));
        REQUIRE(process.waitForProcessToFinish(30000));
        REQUIRE(target.existsAsFile());
        CHECK(engine::isAudioFile(target));

        const auto data = cache.load(target.getFullPathName().toStdString());
        REQUIRE(data != nullptr);
        CHECK(data->sampleRate == 44100.0);
        if (encoding.lossless)
        {
            REQUIRE(data->audio.getNumSamples() == frames);
            float worst = 0.0f;
            for (int channel = 0; channel < 2; ++channel)
                for (int i = 0; i < frames; ++i)
                    worst = std::max(worst, std::abs(data->audio.getSample(channel, i) - original.getSample(channel, i)));
            CHECK(worst < 1.0e-3f);
        }
        else
        {
            CHECK(std::abs(data->audio.getNumSamples() - frames) < 4096); // AAC frames pad the tail
            CHECK(data->audio.getRMSLevel(0, 0, std::min(frames, data->audio.getNumSamples())) > 0.3f);
            CHECK(data->audio.getRMSLevel(1, 0, std::min(frames, data->audio.getNumSamples())) > 0.15f);
        }
    }
    folder.deleteRecursively();
}

TEST_CASE("sample cache: async load publishes the slot, failures are reported, clip is silent until ready")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    juce::TemporaryFile tmp(".wav");
    {
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> stream(tmp.getFile().createOutputStream());
        REQUIRE(stream != nullptr);
        auto writer = wav.createWriterFor(stream, juce::AudioFormatWriterOptions{}
                                                       .withSampleRate(100.0)
                                                       .withNumChannels(1)
                                                       .withBitsPerSample(16));
        REQUIRE(writer != nullptr);
        juce::AudioBuffer<float> mono(1, 100);
        for (int i = 0; i < 100; ++i)
            mono.setSample(0, i, 0.5f);
        writer->writeFromAudioSampleBuffer(mono, 0, 100);
    }

    engine::SampleCache cache;
    juce::ThreadPool pool(1);
    cache.setLoaderPool(&pool);

    auto good = cache.request(tmp.getFile().getFullPathName().toStdString());
    auto bad = cache.request("/definitely/not/here.wav");
    REQUIRE(cache.waitForLoads(5000));
    REQUIRE(good->get() != nullptr);
    CHECK(good->get()->audio.getNumChannels() == 2); // mono duplicated
    CHECK(good->get()->audio.getNumSamples() == 100);
    CHECK(bad->get() == nullptr);
    CHECK(bad->failed());
    CHECK(cache.failedFiles().size() == 1);

    // Same file again is served from the cache immediately.
    auto again = cache.request(tmp.getFile().getFullPathName().toStdString());
    CHECK(again->get() == good->get());

    // A clip whose slot is not ready renders nothing, then sound once published.
    model::Clip c;
    c.kind = model::Clip::Kind::Audio;
    c.currentStart = 0.0;
    c.currentEnd = 10.0;
    c.loop = {0.0, 10.0, 0.0, false};
    auto slot = std::make_shared<engine::SampleSlot>();
    auto clip = engine::prepareClip(c, slot);
    juce::AudioBuffer<float> out(2, 10);
    out.clear();
    engine::renderAudioClip(clip, {0, 10, 0.0, 100.0}, 60.0, out);
    CHECK_NEAR(out.getMagnitude(0, 10), 0.0, 1e-9);
    slot->publish(good->get() != nullptr ? cache.load(tmp.getFile().getFullPathName().toStdString()) : nullptr);
    engine::renderAudioClip(clip, {0, 10, 0.0, 100.0}, 60.0, out);
    CHECK(out.getMagnitude(0, 10) > 0.4f);
}

// ---------------------------------------------------------------- delay compensation (ENG-12)

TEST_CASE("delay line: fixed delay across block boundaries, in place and additive")
{
    engine::DelayLine d;
    d.prepare(3);
    juce::AudioBuffer<float> in(2, 4), out(2, 4);
    out.clear();
    for (int i = 0; i < 4; ++i)
        in.setSample(0, i, (float) (i + 1)), in.setSample(1, i, (float) (i + 1));
    d.addDelayed(in, out, 4, 1.0f); // block 1: 1 2 3 4 -> 0 0 0 1
    CHECK_NEAR(out.getSample(0, 2), 0.0, 1e-9);
    CHECK_NEAR(out.getSample(0, 3), 1.0, 1e-9);
    out.clear();
    d.addDelayed(in, out, 4, 0.5f); // block 2 = tail of block 1 (2 3 4) then block 2's own start (1), all * 0.5
    CHECK_NEAR(out.getSample(1, 0), 1.0, 1e-9);
    CHECK_NEAR(out.getSample(1, 1), 1.5, 1e-9);
    CHECK_NEAR(out.getSample(1, 2), 2.0, 1e-9);
    CHECK_NEAR(out.getSample(1, 3), 0.5, 1e-9);

    engine::DelayLine z;
    z.prepare(0);
    out.clear();
    z.addDelayed(in, out, 4, 1.0f);
    CHECK_NEAR(out.getSample(0, 0), 1.0, 1e-9); // zero delay is a plain add

    engine::DelayLine ip;
    ip.prepare(2);
    ip.processInPlace(in, 4);
    CHECK_NEAR(in.getSample(0, 0), 0.0, 1e-9);
    CHECK_NEAR(in.getSample(0, 2), 1.0, 1e-9);
    CHECK_NEAR(in.getSample(0, 3), 2.0, 1e-9);
}

namespace
{
// Emits a unit impulse `physicalDelay` samples after sample 10 of its first block, and
// reports `reportedLatency` like a look-ahead plugin would.
class ImpulseDevice final : public devices::DeviceProcessor
{
public:
    ImpulseDevice(int physicalDelay, int reportedLatency) : physical_(physicalDelay), reported_(reportedLatency) {}
    std::string name() const override { return "impulse"; }
    void prepare(double, int) override { fired_ = false; }
    int latencySamples() const override { return reported_; }
    void process(juce::AudioBuffer<float>& audio, juce::MidiBuffer&) override
    {
        const int at = 10 + physical_;
        if (! fired_ && at < audio.getNumSamples())
        {
            audio.setSample(0, at, 1.0f);
            audio.setSample(1, at, 1.0f);
            fired_ = true;
        }
    }

private:
    int physical_, reported_;
    bool fired_ = false;
};

std::vector<int> impulseIndices(const std::vector<float>& l, float threshold = 0.1f)
{
    std::vector<int> out;
    for (size_t i = 0; i < l.size(); ++i)
        if (std::fabs(l[i]) > threshold)
            out.push_back((int) i);
    return out;
}
} // namespace

TEST_CASE("pdc: a latent track stays aligned with a dry track at the main output")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    plugins::PluginHost host;
    midi::MidiManager midi;
    engine::AudioEngine eng(host, midi);

    model::Song song;
    for (int i = 0; i < 2; ++i)
    {
        model::Track t;
        t.id = i + 1;
        t.kind = model::TrackKind::Audio;
        t.name = "t" + std::to_string(i);
        song.tracks.push_back(t);
    }
    engine::SampleCache samples;
    devices::BuildContext ctx;
    auto built = engine::buildGraph(song, ctx, samples);
    // Track 0 is dry. Track 1 behaves like a 64-sample look-ahead plugin: its audio really is
    // 64 samples late and it reports 64 samples of latency.
    built.graph->nodes[0].devices.push_back(std::make_unique<ImpulseDevice>(0, 0));
    built.graph->nodes[1].devices.push_back(std::make_unique<ImpulseDevice>(64, 64));
    eng.installGraph(std::move(built.graph));
    REQUIRE(eng.latestGraph() != nullptr);
    CHECK(eng.latestGraph()->nodes[1].latency == 64);
    CHECK(eng.latestGraph()->nodes[0].outDelay.delay() == 64);
    CHECK(eng.latestGraph()->nodes[1].outDelay.delay() == 0);

    std::vector<float> l(256), r(256);
    float* out[2] = {l.data(), r.data()};
    eng.renderBlock(out, 2, 256);
    const auto hits = impulseIndices(l);
    REQUIRE(hits.size() == 1); // both impulses coincide (a misaligned mix would show two)
    CHECK(hits[0] == 74);
    CHECK_NEAR(l[(size_t) hits[0]], 2.0, 1e-5);
}

TEST_CASE("pdc: rack chains with different latency are aligned inside the rack")
{
    class Fixed final : public devices::DeviceProcessor
    {
    public:
        explicit Fixed(int lat) : lat_(lat) {}
        std::string name() const override { return "fixed"; }
        void prepare(double, int) override {}
        int latencySamples() const override { return lat_; }
        void process(juce::AudioBuffer<float>&, juce::MidiBuffer&) override {}

    private:
        int lat_;
    };

    std::vector<devices::RackProcessor::Chain> chains(2);
    chains[0].devices.push_back(std::make_unique<Fixed>(10));
    chains[1].devices.push_back(std::make_unique<Fixed>(40));
    devices::RackProcessor rack("r", false, false, std::move(chains));
    rack.prepare(48000.0, 256);
    CHECK(rack.latencySamples() == 40);
    CHECK(rack.chains()[0].alignDelay.delay() == 30);
    CHECK(rack.chains()[1].alignDelay.delay() == 0);
}

// ---------------------------------------------------------------- time stretching (ENG-5)

namespace
{
// Plays `clip` through an AudioPlayState from timeline beat 0 for `seconds` of output.
std::vector<float> renderThroughPlayer(const engine::PreparedClip& clip, double tempo, double outRate, double seconds)
{
    engine::AudioPlayState state;
    state.prepare(outRate, 256);
    const double spb = outRate * 60.0 / tempo;
    const int total = (int) (seconds * outRate);
    std::vector<float> result((size_t) total, 0.0f);
    juce::AudioBuffer<float> buf(2, 256);
    for (int pos = 0; pos < total; pos += 256)
    {
        const int n = std::min(256, total - pos);
        buf.clear();
        state.render(clip, {0, n, pos / spb, spb}, tempo, buf);
        for (int i = 0; i < n; ++i)
            result[(size_t) (pos + i)] = buf.getSample(0, i);
    }
    return result;
}

double zeroCrossingHz(const std::vector<float>& x, int from, int to, double rate)
{
    int crossings = 0;
    for (int i = from + 1; i < to; ++i)
        if (x[(size_t) i - 1] <= 0.0f && x[(size_t) i] > 0.0f)
            ++crossings;
    return crossings / ((to - from) / rate);
}

double rms(const std::vector<float>& x, int from, int to)
{
    double sum = 0.0;
    for (int i = from; i < to; ++i)
        sum += (double) x[(size_t) i] * x[(size_t) i];
    return std::sqrt(sum / (to - from));
}

// A warped clip: a mono 440 Hz sine of 4 s that Live shows as 8 beats at 120 BPM.
engine::PreparedClip makeSineClip(model::WarpMode mode, double lengthBeats, std::shared_ptr<engine::SampleData>& keep)
{
    keep = std::make_shared<engine::SampleData>();
    keep->sampleRate = 44100.0;
    keep->audio.setSize(2, 44100 * 4);
    for (int i = 0; i < keep->audio.getNumSamples(); ++i)
    {
        const float v = 0.5f * std::sin(2.0f * 3.14159265f * 440.0f * (float) i / 44100.0f);
        keep->audio.setSample(0, i, v);
        keep->audio.setSample(1, i, v);
    }
    model::Clip c;
    c.kind = model::Clip::Kind::Audio;
    c.warped = true;
    c.warpMode = mode;
    c.warpMarkers = {{0.0, 0.0}, {4.0, 8.0}};
    c.currentStart = 0.0;
    c.currentEnd = lengthBeats;
    c.loop = {0.0, 8.0, 0.0, true};
    return engine::prepareClip(c, std::shared_ptr<const engine::SampleData>(keep));
}
} // namespace

TEST_CASE("stretch: complex/tones/texture keep pitch while the clip is slowed to half speed")
{
    for (auto mode : {model::WarpMode::Complex, model::WarpMode::ComplexPro, model::WarpMode::Tones,
                      model::WarpMode::Texture})
    {
        std::shared_ptr<engine::SampleData> keep;
        auto clip = makeSineClip(mode, 8.0, keep);
        // Project tempo 60: a beat lasts 1 s, the source beat 0.5 s -> everything takes twice as long.
        const auto out = renderThroughPlayer(clip, 60.0, 44100.0, 6.0);
        const double hz = zeroCrossingHz(out, 44100, 44100 * 5, 44100.0);
        CHECK_NEAR(hz, 440.0, mode == model::WarpMode::Texture ? 25.0 : 8.0);
        const double level = rms(out, 44100, 44100 * 5); // sine of 0.5 -> rms 0.354
        // Texture randomises grain positions on purpose, so a pure sine loses some energy there.
        CHECK(level > (mode == model::WarpMode::Texture ? 0.18 : 0.29) && level < 0.42);
    }
}

TEST_CASE("stretch: transposing +12 semitones doubles the frequency without changing speed")
{
    std::shared_ptr<engine::SampleData> keep;
    model::Clip c;
    auto clip = makeSineClip(model::WarpMode::Complex, 8.0, keep);
    clip.pitchRatio = 2.0f;
    const auto out = renderThroughPlayer(clip, 120.0, 44100.0, 3.0); // tempo 120: natural speed
    CHECK_NEAR(zeroCrossingHz(out, 44100, 44100 * 2, 44100.0), 880.0, 20.0);
}

TEST_CASE("stretch: clip is silent outside its range and fades at the edges")
{
    std::shared_ptr<engine::SampleData> keep;
    auto clip = makeSineClip(model::WarpMode::Complex, 2.0, keep); // 2 beats at tempo 120 = 1 s
    const auto out = renderThroughPlayer(clip, 120.0, 44100.0, 2.0);
    CHECK(rms(out, 44100 + 2000, 44100 * 2) < 1e-6); // after the clip end
    CHECK(rms(out, 22050, 30000) > 0.25);              // inside
    CHECK(std::fabs(out[0]) < 0.05f);                  // starts faded in, no click
    CHECK(std::fabs(out[44099]) < 0.05f);              // ends faded out
}

TEST_CASE("transients: noise bursts are found at their onsets")
{
    const double sr = 44100.0;
    juce::AudioBuffer<float> a(2, (int) (sr * 3));
    a.clear();
    juce::Random rnd(7);
    const double starts[] = {0.5, 1.2, 2.0};
    for (double t : starts)
        for (int i = 0; i < (int) (0.08 * sr); ++i)
        {
            const float env = std::exp(-(float) i / (0.02f * (float) sr));
            const float v = (rnd.nextFloat() - 0.5f) * 1.6f * env;
            a.setSample(0, (int) (t * sr) + i, v);
            a.setSample(1, (int) (t * sr) + i, v);
        }
    for (int i = 0; i < a.getNumSamples(); ++i) // faint noise floor
        a.addSample(0, i, (rnd.nextFloat() - 0.5f) * 0.002f);
    const auto on = engine::detectTransients(a, sr);
    REQUIRE(on.size() == 4); // 0.0 + three bursts
    for (size_t i = 0; i < 3; ++i)
        CHECK_NEAR(on[i + 1], starts[i], 0.012);
}

TEST_CASE("beats mode: slices keep their length when the clip is stretched")
{
    const double sr = 44100.0;
    auto data = std::make_shared<engine::SampleData>();
    data->sampleRate = sr;
    data->audio.setSize(2, (int) (sr * 4));
    data->audio.clear();
    juce::Random rnd(3);
    for (int k = 0; k < 8; ++k) // a 25 ms noise hit every 0.5 s
        for (int i = 0; i < (int) (0.025 * sr); ++i)
        {
            const float v = (rnd.nextFloat() - 0.5f) * 1.5f;
            data->audio.setSample(0, (int) (k * 0.5 * sr) + i, v);
            data->audio.setSample(1, (int) (k * 0.5 * sr) + i, v);
        }
    data->onsets = engine::detectTransients(data->audio, sr);
    REQUIRE(data->onsets.size() >= 7);

    model::Clip c;
    c.kind = model::Clip::Kind::Audio;
    c.warped = true;
    c.warpMode = model::WarpMode::Beats;
    c.transientLoopMode = 0; // Loop Off: silence in the gaps
    c.warpMarkers = {{0.0, 0.0}, {4.0, 8.0}};
    c.currentStart = 0.0;
    c.currentEnd = 8.0;
    c.loop = {0.0, 8.0, 0.0, true};
    auto clip = engine::prepareClip(c, std::shared_ptr<const engine::SampleData>(data));

    // Half speed (tempo 60): hits should now be 1 s apart but still ~25 ms long.
    const auto out = renderThroughPlayer(clip, 60.0, sr, 4.0);
    const auto hits = engine::detectTransients(
        [&] {
            juce::AudioBuffer<float> b(2, (int) out.size());
            for (size_t i = 0; i < out.size(); ++i)
                b.setSample(0, (int) i, out[i]), b.setSample(1, (int) i, out[i]);
            return b;
        }(),
        sr);
    REQUIRE(hits.size() >= 4);
    CHECK_NEAR(hits[2] - hits[1], 1.0, 0.03);
    CHECK_NEAR(hits[3] - hits[2], 1.0, 0.03);
    // The gap between hits is silent (Loop Off) and the hit itself is as loud as the source.
    CHECK(rms(out, (int) (0.3 * sr), (int) (0.9 * sr)) < 1e-3);
    CHECK(rms(out, (int) (hits[1] * sr), (int) (hits[1] * sr + 0.02 * sr)) > 0.2);
}

// ---------------------------------------------------------------- session clip launching (ENG-6)

#include "engine/ClipLauncher.h"

namespace
{
// Records note on/off events (absolute sample position) arriving at the track's devices.
class NoteRecorder final : public devices::DeviceProcessor
{
public:
    std::string name() const override { return "recorder"; }
    void prepare(double, int) override {}
    void process(juce::AudioBuffer<float>& audio, juce::MidiBuffer& midi) override
    {
        for (const auto m : midi)
        {
            if (m.getMessage().isNoteOn())
                events.push_back({counter + m.samplePosition, m.getMessage().getNoteNumber()});
            else if (m.getMessage().isNoteOff() || m.getMessage().isAllNotesOff())
                events.push_back({counter + m.samplePosition, -1});
        }
        counter += audio.getNumSamples();
    }
    std::vector<std::pair<long long, int>> events; // pitch or -1 for a note-off / all-notes-off
    long long counter = 0;
    std::vector<long long> noteOns(int pitch) const
    {
        std::vector<long long> out;
        for (const auto& e : events)
            if (e.second == pitch)
                out.push_back(e.first);
        return out;
    }
};

model::Clip midiClip(int pitch, double length, bool loop, std::vector<double> starts)
{
    model::Clip c;
    c.kind = model::Clip::Kind::Midi;
    c.currentStart = 0.0;
    c.currentEnd = length;
    c.loop = {0.0, length, 0.0, loop};
    for (double s : starts)
        c.notes.push_back({s, 0.25, pitch, 100.0f});
    return c;
}

struct LaunchRig
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    plugins::PluginHost host;
    midi::MidiManager midi;
    engine::AudioEngine eng{host, midi};
    model::Song song;
    std::vector<NoteRecorder*> rec;

    // Track 0: session scene 0 = pitch 60 (notes at 0 and 2, 4-beat loop), scene 1 = pitch 61;
    //          arrangement clip: pitch 72 every beat from 0.
    // Track 1: session scene 0 = pitch 80, scene 1 empty.
    explicit LaunchRig(bool legatoSecondScene = false)
    {
        song.tempo = 120.0;
        for (int i = 0; i < 2; ++i)
        {
            model::Track t;
            t.id = i + 1;
            t.kind = model::TrackKind::Midi;
            t.name = "t" + std::to_string(i);
            song.tracks.push_back(t);
        }
        song.tracks[0].slots = {{midiClip(60, 4.0, true, {0.0, 2.0})}, {midiClip(61, 4.0, true, {0.0})}};
        if (legatoSecondScene)
        {
            song.tracks[0].slots[1].clip = midiClip(61, 4.0, true, {0.0, 1.0, 2.0, 3.0});
            song.tracks[0].slots[1].clip->legato = true;
        }
        std::vector<double> everyBeat;
        for (int b = 0; b < 64; ++b)
            everyBeat.push_back((double) b);
        song.tracks[0].arrangementClips = {midiClip(72, 64.0, true, everyBeat)};
        song.tracks[1].slots = {{midiClip(80, 4.0, true, {0.0})}, {}};
        song.scenes = {{"A", -1, std::nullopt}, {"B", -1, 90.0}};

        engine::SampleCache samples;
        devices::BuildContext ctx;
        auto built = engine::buildGraph(song, ctx, samples);
        for (int i = 0; i < 2; ++i)
        {
            auto r = std::make_unique<NoteRecorder>();
            rec.push_back(r.get());
            built.graph->nodes[(size_t) i].devices.push_back(std::move(r));
        }
        eng.installGraph(std::move(built.graph));
    }

    // Renders `samples` of audio in 512-sample blocks.
    void run(long long samples)
    {
        std::vector<float> l(512), r(512);
        float* out[2] = {l.data(), r.data()};
        for (long long done = 0; done < samples; done += 512)
            eng.renderBlock(out, 2, 512);
    }
    static constexpr double kSamplesPerBeat = 22050.0; // 120 BPM @ 44.1 kHz
};
} // namespace

TEST_CASE("launch: quantisation table")
{
    CHECK_NEAR(engine::quantisationBeats(0, 4, 4), 0.0, 1e-9);
    CHECK_NEAR(engine::quantisationBeats(4, 4, 4), 4.0, 1e-9);   // 1 bar
    CHECK_NEAR(engine::quantisationBeats(4, 3, 4), 3.0, 1e-9);   // 1 bar of 3/4
    CHECK_NEAR(engine::quantisationBeats(1, 4, 4), 32.0, 1e-9);  // 8 bars
    CHECK_NEAR(engine::quantisationBeats(7, 4, 4), 1.0, 1e-9);   // 1/4
    CHECK_NEAR(engine::quantisationBeats(10, 4, 4), 1.0 / 3.0, 1e-9); // 1/8T
}

TEST_CASE("piano audition: unarmed track with monitoring off receives only its own preview note and releases it")
{
    LaunchRig rig;
    auto controls = rig.eng.controls(); REQUIRE(controls);
    (*controls)[0].armed.store(false); (*controls)[0].monitoring.store(2);
    (*controls)[0].previewPitch.store(66);
    const auto midiBefore = (*controls)[0].midiActivity.load();
    rig.run(512);
    CHECK((*controls)[0].midiActivity.load() > midiBefore);
    CHECK(rig.rec[0]->noteOns(66).size() == 1);
    CHECK(rig.rec[1]->noteOns(66).empty());
    const auto midiAfter = (*controls)[0].midiActivity.load();
    rig.run(512); CHECK(rig.rec[0]->noteOns(66).size() == 1);
    CHECK((*controls)[0].midiActivity.load() == midiAfter);
    const auto before = rig.rec[0]->events.size();
    (*controls)[0].previewPitch.store(69); rig.run(512);
    REQUIRE(rig.rec[0]->events.size() == before + 2);
    CHECK(rig.rec[0]->events[before].second == -1);
    CHECK(rig.rec[0]->events.back().second == 69);
    (*controls)[0].previewPitch.store(-1); rig.run(512);
    CHECK(rig.rec[0]->events.back().second == -1);
    CHECK(! rig.eng.transport().isPlaying());
}

TEST_CASE("launch: a stopped transport starts and the clip plays from its first beat")
{
    LaunchRig rig;
    auto& g = *rig.eng.latestGraph();
    CHECK(engine::slotState(g, 0, 0) == engine::SlotState::Stopped);
    engine::launchClip(g, rig.eng.transport(), 0, 0, 4.0);
    CHECK(engine::slotState(g, 0, 0) == engine::SlotState::Queued);
    rig.run(2 * 22050);
    CHECK(rig.eng.transport().isPlaying());
    CHECK(engine::slotState(g, 0, 0) == engine::SlotState::Playing);
    const auto n60 = rig.rec[0]->noteOns(60);
    REQUIRE(n60.size() >= 2);
    CHECK(n60[0] == 0);                                  // starts immediately: no quantisation wait while stopped
    CHECK(std::llabs(n60[1] - 2 * 22050) <= 1);          // second note two beats later
    CHECK(rig.rec[0]->noteOns(72).empty());              // the session clip took the track over from the arrangement
}

TEST_CASE("launch: quantised launch waits for the bar, stop is quantised too, arrangement can be restored")
{
    LaunchRig rig;
    auto& g = *rig.eng.latestGraph();
    auto& tr = rig.eng.transport();
    tr.play();
    rig.run((long long) (1.3 * LaunchRig::kSamplesPerBeat)); // somewhere inside beat 1; the arrangement plays
    CHECK(! rig.rec[0]->noteOns(72).empty());

    engine::launchClip(g, tr, 0, 0, 4.0); // 1 bar
    rig.run((long long) (2.4 * LaunchRig::kSamplesPerBeat)); // beat ~3.7: still before the bar, nothing of the session clip yet
    CHECK(rig.rec[0]->noteOns(60).empty());
    rig.run(1 * 22050);                   // crosses beat 4
    const auto n60 = rig.rec[0]->noteOns(60);
    REQUIRE(! n60.empty());
    if (std::llabs(n60[0] - 4 * 22050) > 1)
        std::printf("    first session note at %lld, expected %d\n", n60[0], 4 * 22050);
    CHECK(std::llabs(n60[0] - 4 * 22050) <= 1);          // exactly on the bar
    for (auto at : rig.rec[0]->noteOns(72))
        CHECK(at <= 4 * 22050 + 1);                      // the arrangement went silent at the launch
    CHECK(engine::arrangementOverridden(g));

    // Quantised stop at the next bar (beat 8): notes keep coming until then, none after.
    engine::stopTrack(g, tr, 0, 4.0);
    CHECK(engine::slotState(g, 0, 0) == engine::SlotState::QueuedStop);
    rig.run(5 * 22050); // to beat ~10.1
    CHECK(engine::slotState(g, 0, 0) == engine::SlotState::Stopped);
    for (auto at : rig.rec[0]->noteOns(60))
        CHECK(at < 8 * 22050);
    CHECK(! rig.rec[0]->noteOns(60).empty());

    // Back to Arrangement: the track plays its arrangement clip again.
    const size_t before = rig.rec[0]->noteOns(72).size();
    engine::backToArrangement(g);
    rig.run(3 * 22050);
    CHECK(rig.rec[0]->noteOns(72).size() > before);
    CHECK(! engine::arrangementOverridden(g));
}

TEST_CASE("launch: legato continues at the previous clip's position, trigger restarts")
{
    // Launching at beat ~1.5 of clip 0: the legato clip's next note is at its own beat 2, not at the launch.
    LaunchRig rig(true);
    auto& g = *rig.eng.latestGraph();
    auto& tr = rig.eng.transport();
    engine::launchClip(g, tr, 0, 0, 0.0);
    rig.run((long long) (1.5 * LaunchRig::kSamplesPerBeat) / 512 * 512);
    const long long launchAt = rig.eng.transport().isPlaying() ? 1 : 0;
    CHECK(launchAt == 1);
    engine::launchClip(g, tr, 0, 1, 0.0);
    rig.run(3 * 22050);
    const auto n61 = rig.rec[0]->noteOns(61);
    REQUIRE(n61.size() >= 2);
    // first 61 is a whole beat boundary of the shared clip clock (position 2), later than the launch block
    const double firstBeat = (double) n61[0] / LaunchRig::kSamplesPerBeat;
    CHECK(std::fabs(firstBeat - std::round(firstBeat)) < 1e-3);
    CHECK(firstBeat >= 2.0 - 1e-6);
    CHECK_NEAR((double) (n61[1] - n61[0]), LaunchRig::kSamplesPerBeat, 2.0);

    LaunchRig plain(false);
    auto& g2 = *plain.eng.latestGraph();
    engine::launchClip(g2, plain.eng.transport(), 0, 0, 0.0);
    plain.run(((long long) (1.5 * LaunchRig::kSamplesPerBeat) / 512 + 1) * 512);
    engine::launchClip(g2, plain.eng.transport(), 0, 1, 0.0);
    plain.run(22050);
    const auto p61 = plain.rec[0]->noteOns(61);
    REQUIRE(! p61.empty());
    CHECK(p61[0] < 3 * 22050); // restarts at the launch (note at clip beat 0) well before beat 3
}

TEST_CASE("launch: toggle stops a playing clip, gate stops on release, per-clip quantisation overrides the global one")
{
    LaunchRig rig;
    auto& g = *rig.eng.latestGraph();
    auto& tr = rig.eng.transport();
    model::Clip toggle = midiClip(60, 4.0, true, {0.0});
    toggle.launchMode = 2;
    engine::launchClipWithMode(g, tr, toggle, 0, 0, 0.0);
    rig.run(2 * 22050);
    CHECK(engine::slotState(g, 0, 0) == engine::SlotState::Playing);
    engine::launchClipWithMode(g, tr, toggle, 0, 0, 0.0);
    rig.run(22050);
    CHECK(engine::slotState(g, 0, 0) == engine::SlotState::Stopped);

    model::Clip gate = midiClip(60, 4.0, true, {0.0});
    gate.launchMode = 1;
    engine::releaseClip(g, tr, gate, 0, 0, 0.0); // release without a launch does nothing
    engine::launchClipWithMode(g, tr, gate, 0, 0, 0.0);
    rig.run(22050);
    CHECK(engine::slotState(g, 0, 0) == engine::SlotState::Playing);
    engine::releaseClip(g, tr, gate, 0, 0, 0.0);
    rig.run(22050);
    CHECK(engine::slotState(g, 0, 0) == engine::SlotState::Stopped);

    model::Clip trigger = midiClip(60, 4.0, true, {0.0});
    engine::releaseClip(g, tr, trigger, 0, 0, 0.0);
    engine::launchClipWithMode(g, tr, trigger, 0, 0, 0.0);
    rig.run(22050);
    engine::releaseClip(g, tr, trigger, 0, 0, 0.0); // Trigger ignores the release
    rig.run(22050);
    CHECK(engine::slotState(g, 0, 0) == engine::SlotState::Playing);

    model::Clip q;
    CHECK_NEAR(engine::clipQuantisationBeats(q, 4, 4, 4), 4.0, 1e-9);   // Global -> the set's 1 Bar
    q.launchQuantisation = 1;                                            // None
    CHECK_NEAR(engine::clipQuantisationBeats(q, 4, 4, 4), 0.0, 1e-9);
    q.launchQuantisation = 8;                                            // 1/4
    CHECK_NEAR(engine::clipQuantisationBeats(q, 4, 4, 4), 1.0, 1e-9);
    q.launchQuantisation = 3;                                            // 4 Bars
    CHECK_NEAR(engine::clipQuantisationBeats(q, 0, 3, 4), 12.0, 1e-9);
}

TEST_CASE("launch: scenes launch every track, an empty slot stops its track, scene tempo applies")
{
    LaunchRig rig;
    auto& g = *rig.eng.latestGraph();
    auto& tr = rig.eng.transport();
    engine::launchScene(g, tr, rig.song, 0, 4.0);
    rig.run(22050);
    CHECK(! rig.rec[0]->noteOns(60).empty());
    CHECK(! rig.rec[1]->noteOns(80).empty());

    engine::launchScene(g, tr, rig.song, 1, 0.0); // scene B: track 1's slot is empty; tempo 90
    CHECK_NEAR(tr.tempo(), 90.0, 1e-9);
    rig.run(22050);
    CHECK(! rig.rec[0]->noteOns(61).empty());
    CHECK(engine::slotState(g, 1, 0) == engine::SlotState::Stopped);
    CHECK((*g.controls)[1].playingSlot.load() == engine::TrackControls::kNoSlot);
}

TEST_CASE("launch: held Repeat preserves fractional timing across blocks, release continues the last pass")
{
    LaunchRig rig;
    auto& g = *rig.eng.latestGraph();
    auto& tr = rig.eng.transport();
    auto clip = *rig.song.tracks[0].slots[0].clip;
    clip.launchMode = 3;
    constexpr double interval = 0.125 * LaunchRig::kSamplesPerBeat; // 2756.25 samples
    engine::launchClipWithMode(g, tr, clip, 0, 0, 0.125);
    rig.run(16 * 512);
    const auto before = rig.rec[0]->noteOns(60);
    REQUIRE(before.size() == 3);
    for (size_t i = 0; i < before.size(); ++i)
        CHECK_NEAR((double) before[i], std::ceil((double) i * interval - 1e-8), 0.0);

    engine::releaseClip(g, tr, clip, 0, 0, 0.125);
    rig.run(16 * 512);
    CHECK(rig.rec[0]->noteOns(60).size() == before.size());
    CHECK(engine::slotState(g, 0, 0) == engine::SlotState::Playing);
    rig.run(2 * 22050);
    const auto after = rig.rec[0]->noteOns(60);
    REQUIRE(after.size() > before.size());
    CHECK_NEAR((double) after[before.size()], (double) before.back() + 2 * 22050, 1.0);
}

TEST_CASE("launch: Repeat handles multiple boundaries per block and releases voices before retriggering")
{
    LaunchRig rig;
    auto& g = *rig.eng.latestGraph();
    auto clip = *rig.song.tracks[0].slots[0].clip;
    clip.launchMode = 3;
    // Artificially short interval to put several boundaries in a single callback.
    constexpr double quant = 0.005;
    engine::launchClipWithMode(g, rig.eng.transport(), clip, 0, 0, quant);
    rig.run(4 * 512);
    const auto notes = rig.rec[0]->noteOns(60);
    REQUIRE(notes.size() == 19);
    for (size_t i = 0; i < notes.size(); ++i)
    {
        CHECK_NEAR((double) notes[i], std::ceil((double) i * quant * LaunchRig::kSamplesPerBeat - 1e-8), 0.0);
        if (i > 0)
        {
            const auto& events = rig.rec[0]->events;
            const auto on = std::find(events.begin(), events.end(), std::pair<long long, int>{notes[i], 60});
            REQUIRE(on != events.begin());
            CHECK((on - 1)->first == notes[i]);
            CHECK((on - 1)->second == -1);
        }
    }
}

TEST_CASE("launch: Repeat clock survives shorter Arrangement loops and quantised initial launches")
{
    LaunchRig rig;
    auto& g = *rig.eng.latestGraph();
    auto& tr = rig.eng.transport();
    auto clip = *rig.song.tracks[0].slots[0].clip;
    clip.launchMode = 3;
    tr.setLoop(true, 0, 0.2);
    engine::launchClipWithMode(g, tr, clip, 0, 0, 1.0);
    rig.run(4 * 22050);
    const auto notes = rig.rec[0]->noteOns(60);
    REQUIRE(notes.size() >= 4);
    for (size_t i = 0; i < notes.size(); ++i)
        CHECK_NEAR((double) notes[i], (double) i * LaunchRig::kSamplesPerBeat, 1.0);

    LaunchRig playing;
    auto& pg = *playing.eng.latestGraph();
    auto& pt = playing.eng.transport();
    pt.play();
    playing.run(28 * 512); // ~0.65 beats, wait for beat 1
    engine::launchClipWithMode(pg, pt, clip, 0, 0, 1.0);
    playing.run(6 * 512);
    CHECK(playing.rec[0]->noteOns(60).empty());
    playing.run(3 * 22050);
    const auto quantised = playing.rec[0]->noteOns(60);
    REQUIRE(quantised.size() >= 3);
    for (size_t i = 0; i < quantised.size(); ++i)
        CHECK_NEAR((double) quantised[i], (double) (i + 1) * LaunchRig::kSamplesPerBeat, 1.0);
}

TEST_CASE("launch: Repeat waits for a queued launch boundary even when that clip is already playing")
{
    LaunchRig rig;
    auto& g = *rig.eng.latestGraph();
    auto& tr = rig.eng.transport();
    auto clip = *rig.song.tracks[0].slots[0].clip;
    engine::launchClip(g, tr, 0, 0, 0.0);
    rig.run(512);
    clip.launchMode = 3;
    engine::launchClipWithMode(g, tr, clip, 0, 0, 1.0);
    rig.run(16 * 512);
    CHECK(rig.rec[0]->noteOns(60).size() == 1);
    rig.run(22050);
    auto notes = rig.rec[0]->noteOns(60);
    REQUIRE(notes.size() == 2);
    CHECK(notes[1] == 22050);
    engine::releaseClip(g, tr, clip, 0, 0, 1.0);
    rig.run(512);
    engine::launchClipWithMode(g, tr, clip, 0, 0, 1.0);
    rig.run(2 * 22050);
    notes = rig.rec[0]->noteOns(60);
    REQUIRE(notes.size() == 4);
    CHECK(notes[2] == 2 * 22050);
    CHECK(notes[3] == 3 * 22050);
}

TEST_CASE("launch: quick Repeat release, None, track/transport stop, scene, arrangement and audition cancel holding")
{
    for (int action = 0; action < 8; ++action)
    {
        LaunchRig rig;
        auto& g = *rig.eng.latestGraph();
        auto& tr = rig.eng.transport();
        auto clip = *rig.song.tracks[0].slots[0].clip;
        clip.launchMode = 3;
        engine::launchClipWithMode(g, tr, clip, 0, 0, action == 1 ? 0.0 : 0.125);
        if (action == 0) engine::releaseClip(g, tr, clip, 0, 0, 0.125); // release before the callback
        rig.run(512);
        if (action == 2) engine::stopTrack(g, tr, 0, 0.0);
        if (action == 3) tr.stop();
        if (action == 4) engine::launchScene(g, tr, rig.song, 1, 0.0);
        if (action == 5) engine::backToArrangement(g);
        if (action == 6) engine::launchClip(g, tr, 0, 1, 0.0);
        if (action == 7)
        {
            engine::PreparedTakeLane lane; lane.id = 42;
            lane.clips.push_back(engine::prepareClip(midiClip(67, 4, true, {0}), std::shared_ptr<engine::SampleSlot>{}));
            g.nodes[0].takes.push_back(std::move(lane));
            g.nodes[0].auditionLane = 42;
        }
        rig.run(22050);
        CHECK((*g.controls)[0].repeatSlot.load() == engine::TrackControls::kNoSlot);
        CHECK(rig.rec[0]->noteOns(60).size() == 1);
    }
}

TEST_CASE("launch: scene clips honour individual quantisation, empty slots cancel pending launches")
{
    LaunchRig rig;
    auto& g = *rig.eng.latestGraph();
    auto& tr = rig.eng.transport();
    rig.song.tracks[0].slots[0].clip->launchQuantisation = 8; // quarter note
    rig.song.tracks[1].slots[0].clip->launchQuantisation = 1; // None
    tr.play();
    rig.run(28 * 512);
    const long long immediate = rig.rec[0]->counter;
    engine::launchScene(g, tr, rig.song, 0, 4.0);
    CHECK_NEAR((*g.controls)[0].queuedQuant.load(), 1.0, 0.0);
    CHECK_NEAR((*g.controls)[1].queuedQuant.load(), 0.0, 0.0);
    rig.run(22050);
    const auto first = rig.rec[0]->noteOns(60), second = rig.rec[1]->noteOns(80);
    REQUIRE(! first.empty()); REQUIRE(! second.empty());
    CHECK(first[0] == 22050);
    CHECK(second[0] == immediate);

    // The bar-based enum respects the set's denominator; Global keeps the caller's quantisation.
    rig.song.timeSigNumerator = 3; rig.song.timeSigDenominator = 8;
    rig.song.tracks[0].slots[0].clip->launchQuantisation = 3; // four bars of 3/8
    rig.song.tracks[1].slots[0].clip->launchQuantisation = 0; // Global
    engine::launchScene(g, tr, rig.song, 0, 0.5);
    CHECK_NEAR((*g.controls)[0].queuedQuant.load(), 6.0, 0.0);
    CHECK_NEAR((*g.controls)[1].queuedQuant.load(), 0.5, 0.0);
    tr.stop(); rig.run(512);
    engine::launchScene(g, tr, rig.song, 0, 0.5);
    CHECK_NEAR((*g.controls)[0].queuedQuant.load(), 0.0, 0.0); // stopped scene starts immediately
    CHECK_NEAR((*g.controls)[1].queuedQuant.load(), 0.0, 0.0);

    LaunchRig pending;
    auto& pg = *pending.eng.latestGraph();
    auto& pt = pending.eng.transport();
    pt.play(); pending.run(512);
    engine::launchClip(pg, pt, 1, 0, 4.0);
    engine::launchScene(pg, pt, pending.song, 1, 0.0); // empty on track 1
    pending.run(5 * 22050);
    CHECK(pending.rec[1]->noteOns(80).empty());
    CHECK((*pg.controls)[1].playingSlot.load() == engine::TrackControls::kNoSlot);
}

TEST_CASE("launch: Repeat restarts audio source and stretch state at each boundary")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    plugins::PluginHost host;
    midi::MidiManager midi;
    for (const auto mode : {model::WarpMode::RePitch, model::WarpMode::Beats, model::WarpMode::Complex})
    {
        engine::AudioEngine audio(host, midi);
        model::Song song; song.tempo = 120;
        model::Track track; track.kind = model::TrackKind::Audio;
        model::Clip clip; clip.kind = model::Clip::Kind::Audio;
        clip.currentEnd = 4; clip.loop = {0, 4, 0, true}; clip.launchMode = 3;
        clip.warped = true; clip.warpMode = mode; clip.warpMarkers = {{0, 0}, {2, 4}};
        track.slots = {{clip}}; song.tracks.push_back(track);
        auto data = std::make_shared<engine::SampleData>();
        data->sampleRate = 44100; data->audio.setSize(2, 88200); data->onsets = {0};
        for (int i = 0; i < 88200; ++i)
            for (int ch = 0; ch < 2; ++ch)
                data->audio.setSample(ch, i, (ch == 0 ? 1.0f : -1.0f) * (float) (0.25 + 0.2 * std::sin(i * 0.021)));
        engine::SampleCache cache; devices::BuildContext context;
        auto built = engine::buildGraph(song, context, cache);
        built.graph->nodes[0].sessionClips[0] = engine::prepareSessionClip(clip, engine::SampleSlot::makeReady(data));
        audio.installGraph(std::move(built.graph));
        auto& g = *audio.latestGraph();
        engine::launchClipWithMode(g, audio.transport(), clip, 0, 0, 0.125);
        std::vector<float> rendered;
        for (int b = 0; b < 24; ++b)
        {
            float left[512]{}, right[512]{}; float* out[]{left, right};
            audio.renderBlock(out, 2, 512);
            rendered.insert(rendered.end(), left, left + 512);
            for (int i = 0; i < 512; ++i) CHECK_NEAR(left[i], -right[i], 1e-6);
        }
        CHECK(rms(rendered, 0, 256) > 0.01);
        for (int restart = 1; restart <= 4; ++restart)
        {
            const int offset = (int) std::ceil(restart * 0.125 * 22050.0 - 1e-8);
            for (int i = 0; i < 256; ++i) CHECK_NEAR(rendered[(size_t) offset + (size_t) i], rendered[(size_t) i], 1e-5);
        }
    }
}

TEST_CASE("engine: devices only see the block they must process, even when the buffers are larger")
{
    // Regression: nodes are allocated for the largest block (512) while the audio device may call
    // back with fewer samples; devices used to process (and learn from) the uninitialised tail.
    class SizeProbe final : public devices::DeviceProcessor
    {
    public:
        std::string name() const override { return "size probe"; }
        void prepare(double, int) override {}
        void process(juce::AudioBuffer<float>& audio, juce::MidiBuffer&) override
        {
            seen = audio.getNumSamples();
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < audio.getNumSamples(); ++i)
                    if (! std::isfinite(audio.getSample(ch, i)) || audio.getSample(ch, i) != 0.0f)
                        dirty = true;
        }
        int seen = 0;
        bool dirty = false;
    };

    juce::ScopedJuceInitialiser_GUI juceInit;
    plugins::PluginHost host;
    midi::MidiManager midi;
    engine::AudioEngine eng(host, midi);
    model::Song song;
    model::Track t;
    t.id = 1;
    t.kind = model::TrackKind::Audio;
    song.tracks.push_back(t);
    engine::SampleCache samples;
    devices::BuildContext ctx;
    auto built = engine::buildGraph(song, ctx, samples);
    auto probe = std::make_unique<SizeProbe>();
    auto* p = probe.get();
    built.graph->nodes[0].devices.push_back(std::move(probe));
    eng.installGraph(std::move(built.graph)); // prepared for 512 samples

    // Poison the unused part of the buffer the way real memory can look.
    auto& buffer = eng.latestGraph()->nodes[0].buffer;
    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < buffer.getNumSamples(); ++i)
            buffer.setSample(ch, i, std::numeric_limits<float>::quiet_NaN());

    std::vector<float> l(128), r(128);
    float* out[2] = {l.data(), r.data()};
    eng.renderBlock(out, 2, 128); // a smaller block than the 512 the graph was prepared for
    CHECK(p->seen == 128);
    CHECK(! p->dirty); // the 128 samples it processed were cleared, nothing from the tail leaked in
    CHECK(std::isfinite(l[127]));
}

TEST_CASE("metronome: a click on every beat, accent on the bar, silent when off")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    plugins::PluginHost host;
    midi::MidiManager midi;
    engine::AudioEngine eng(host, midi);
    model::Song song;
    song.tempo = 120.0;
    eng.loadSong(song);
    auto& tr = eng.transport();

    auto render = [&](long long samples) {
        std::vector<float> out;
        std::vector<float> l(512), r(512);
        float* o[2] = {l.data(), r.data()};
        for (long long d = 0; d < samples; d += 512)
        {
            eng.renderBlock(o, 2, 512);
            out.insert(out.end(), l.begin(), l.end());
        }
        return out;
    };
    auto peakIn = [](const std::vector<float>& x, size_t from, size_t to) {
        float p = 0;
        for (size_t i = from; i < to && i < x.size(); ++i)
            p = std::max(p, std::fabs(x[i]));
        return p;
    };

    tr.play();
    auto off = render(44100);
    CHECK(peakIn(off, 0, off.size()) < 1e-9f); // metronome off: nothing

    tr.setMetronome(true);
    tr.setPosition(0.0);
    auto on = render(4 * 22050 + 2048); // beats 0..4 at 120 BPM (22050 samples per beat)
    CHECK(peakIn(on, 0, 600) > 0.3f);                    // beat 0 (accent)
    CHECK(peakIn(on, 22050, 22050 + 600) > 0.2f);        // beat 1
    CHECK(peakIn(on, 22050 + 5000, 22050 + 20000) < 1e-3f); // silence between clicks
    CHECK(peakIn(on, 2 * 22050, 2 * 22050 + 600) > 0.2f);
    CHECK(peakIn(on, 4 * 22050, 4 * 22050 + 600) > 0.3f);   // the next bar's downbeat is louder than a beat click
    CHECK(peakIn(on, 4 * 22050, 4 * 22050 + 600) > peakIn(on, 22050, 22050 + 600) * 1.2f);
}

TEST_CASE("clip envelopes: follow their lane while the clip plays and restore the parameter afterwards")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    plugins::PluginHost host;
    midi::MidiManager midi;
    engine::AudioEngine eng(host, midi);

    model::Song song;
    song.tempo = 120.0;
    model::Track t;
    t.id = 1;
    t.kind = model::TrackKind::Midi;
    t.mixer.volumeTargetId = 500;
    t.mixer.volume = 0.8;
    // A MIDI clip from beat 4 to 8 with a volume envelope ramping 0 -> 1 over its 4 beats.
    model::Clip c = midiClip(60, 4.0, false, {0.0});
    c.currentStart = 4.0;
    c.currentEnd = 8.0;
    model::AutomationEnvelope env;
    env.targetId = 500;
    env.points = {{-63072000.0, 0.0}, {0.0, 0.0}, {4.0, 1.0}};
    c.envelopes.push_back(env);
    t.arrangementClips.push_back(c);
    song.tracks.push_back(t);
    eng.loadSong(song);

    auto& ctl = (*eng.controls())[0];
    std::vector<float> l(512), r(512);
    float* out[2] = {l.data(), r.data()};
    auto runBeats = [&](double beats) {
        const long long samples = (long long) (beats * 22050.0);
        for (long long d = 0; d < samples; d += 512)
            eng.renderBlock(out, 2, 512);
    };

    eng.transport().play();
    runBeats(2.0);                         // before the clip: the mixer value from the file
    CHECK_NEAR(ctl.volume.load(), 0.8, 1e-6);
    runBeats(3.0);                         // beat ~5: a quarter into the envelope
    CHECK_NEAR(ctl.volume.load(), 0.25, 0.03);
    runBeats(2.0);                         // beat ~7: three quarters
    CHECK_NEAR(ctl.volume.load(), 0.75, 0.03);
    runBeats(2.0);                         // beat ~9: the clip is over, volume returns
    CHECK_NEAR(ctl.volume.load(), 0.8, 1e-6);
}

// ---------------------------------------------------------------- routing, sidechain, track delay (ALS-12)

TEST_CASE("routing: output None silences the track in the mix, track delay shifts it, sidechain orders and feeds")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    plugins::PluginHost host;
    midi::MidiManager midi;
    engine::AudioEngine eng(host, midi);

    // Three tracks: 0 "kick" (the sidechain source), 1 "bass" with a Compressor listening to track 0, 2 muted-output track.
    model::Song song;
    for (int i = 0; i < 3; ++i)
    {
        model::Track t;
        t.id = 10 + i;
        t.kind = model::TrackKind::Audio;
        t.name = "t" + std::to_string(i);
        song.tracks.push_back(t);
    }
    song.tracks[2].outputRouting = "AudioOut/None";
    song.tracks[1].trackDelayMs = 10.0;
    model::Device comp;
    comp.kind = model::DeviceKind::Native;
    comp.type = "Compressor2";
    comp.sidechain.on = true;
    comp.sidechain.sourceTrackId = 10;
    comp.sidechain.tap = model::Sidechain::Tap::PostFx;
    comp.parameters.push_back({"Threshold", 0.1, -1, {}}); // -20 dB
    comp.parameters.push_back({"Ratio", 20.0, -1, {}});
    comp.parameters.push_back({"Attack", 0.1, -1, {}});
    comp.parameters.push_back({"Release", 50.0, -1, {}});
    comp.parameters.push_back({"Knee", 0.0, -1, {}});
    song.tracks[1].devices.push_back(comp);

    engine::SampleCache samples;
    devices::BuildContext ctx;
    auto built = engine::buildGraph(song, ctx, samples);
    auto& g = *built.graph;
    // Source track 0 must be processed before track 1 (its listener).
    const auto pos0 = std::find(g.trackOrder.begin(), g.trackOrder.end(), 0) - g.trackOrder.begin();
    const auto pos1 = std::find(g.trackOrder.begin(), g.trackOrder.end(), 1) - g.trackOrder.begin();
    CHECK(pos0 < pos1);
    CHECK(g.nodes[0].needTap[1]);
    REQUIRE(g.nodes[1].sidechains.size() == 1);
    CHECK(g.nodes[1].sidechains[0].sourceNode == 0);
    CHECK(! g.nodes[2].outputEnabled);
    CHECK(g.nodes[1].outputEnabled);

    // Constant-level sources: track 0 loud (the sidechain), tracks 1 and 2 steady 0.1.
    class Constant final : public devices::DeviceProcessor
    {
    public:
        explicit Constant(float v) : v_(v) {}
        std::string name() const override { return "const"; }
        bool isInstrument() const override { return true; }
        void prepare(double, int) override {}
        void process(juce::AudioBuffer<float>& a, juce::MidiBuffer&) override
        {
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < a.getNumSamples(); ++i)
                    a.setSample(ch, i, a.getSample(ch, i) + v_);
        }
        float v_;
    };
    g.nodes[0].devices.insert(g.nodes[0].devices.begin(), std::make_unique<Constant>(0.8f)); // the "kick"
    g.nodes[1].devices.insert(g.nodes[1].devices.begin(), std::make_unique<Constant>(0.1f));
    g.nodes[2].devices.push_back(std::make_unique<Constant>(0.3f));
    // processorForModelDevice must keep pointing at the compressor (index 0 of the model devices).
    eng.installGraph(std::move(built.graph));

    std::vector<float> l(512), r(512);
    float* out[2] = {l.data(), r.data()};
    eng.transport().play();
    std::vector<float> mix;
    for (int b = 0; b < 40; ++b)
    {
        eng.renderBlock(out, 2, 512);
        mix.insert(mix.end(), l.begin(), l.end());
    }
    // Track 2 (output None) contributes nothing; track 0 contributes 0.8; track 1 (0.1) is ducked by the
    // sidechain (kick at -2 dB, threshold -20 dB, ratio 20:1 -> almost 18 dB of reduction) and delayed 10 ms.
    const float steady = mix[mix.size() - 1];
    CHECK_NEAR(steady, 0.8f + 0.1f * 0.126f, 0.03f); // 0.8 + ducked 0.1 (about -18 dB = x0.126)
    CHECK(steady < 0.8f + 0.1f * 0.5f);              // clearly ducked, not the plain 0.9
    // Track delay counts as latency: delay compensation holds every other path back by the same 10 ms
    // (441 samples) so that all tracks stay aligned; the mix starts after that.
    CHECK_NEAR(mix[100], 0.0f, 1e-4f);
    CHECK(mix[1200] > 0.5f);
}

TEST_CASE("tempo automation on the main track moves the transport tempo")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    plugins::PluginHost host;
    midi::MidiManager midi;
    engine::AudioEngine eng(host, midi);
    model::Song song;
    song.tempo = 120.0;
    song.main.mixer.tempoTargetId = 777;
    model::AutomationEnvelope env;
    env.targetId = 777;
    env.points = {{-63072000.0, 120.0}, {0.0, 120.0}, {8.0, 60.0}};
    song.main.automation.push_back(env);
    eng.loadSong(song);

    std::vector<float> l(512), r(512);
    float* out[2] = {l.data(), r.data()};
    eng.transport().play();
    for (int b = 0; b < 100; ++b) // about 1.1 s: beat ~2 at the beginning tempo
        eng.renderBlock(out, 2, 512);
    const double midTempo = eng.transport().tempo();
    CHECK(midTempo < 118.0);
    CHECK(midTempo > 60.0);
    for (int b = 0; b < 600; ++b)
        eng.renderBlock(out, 2, 512);
    CHECK_NEAR(eng.transport().tempo(), 60.0, 1.0); // past beat 8 the envelope holds 60
}

TEST_CASE("tempo: Live's static tempo envelope does not undo a tempo change while playing")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    plugins::PluginHost host;
    midi::MidiManager midi;
    engine::AudioEngine eng(host, midi);
    model::Song song;
    song.tempo = 120.0;
    song.main.mixer.tempoTargetId = 8;
    model::AutomationEnvelope env; // what every Live set stores for a tempo that is not automated
    env.targetId = 8;
    env.points = {{-63072000.0, 120.0}};
    song.main.automation.push_back(env);
    eng.loadSong(song);

    std::vector<float> l(512), r(512);
    float* out[2] = {l.data(), r.data()};
    eng.transport().setTempo(145.0); // typed while stopped
    eng.transport().play();
    for (int b = 0; b < 20; ++b)
        eng.renderBlock(out, 2, 512);
    CHECK_NEAR(eng.transport().tempo(), 145.0, 1e-9);
    eng.transport().setTempo(98.0); // and while playing
    for (int b = 0; b < 20; ++b)
        eng.renderBlock(out, 2, 512);
    CHECK_NEAR(eng.transport().tempo(), 98.0, 1e-9);
}

TEST_CASE("tempo: a tempo set by hand overrides tempo automation until re-enabled")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    plugins::PluginHost host;
    midi::MidiManager midi;
    engine::AudioEngine eng(host, midi);
    model::Song song;
    song.tempo = 120.0;
    song.main.mixer.tempoTargetId = 8;
    model::AutomationEnvelope env;
    env.targetId = 8;
    env.points = {{-63072000.0, 120.0}, {0.0, 120.0}, {1000.0, 120.0}};
    song.main.automation.push_back(env);
    eng.loadSong(song);

    std::vector<float> l(512), r(512);
    float* out[2] = {l.data(), r.data()};
    eng.transport().play();
    for (int b = 0; b < 5; ++b)
        eng.renderBlock(out, 2, 512);
    CHECK_NEAR(eng.transport().tempo(), 120.0, 1e-9);
    CHECK(! engine::automationOverridden(*eng.latestGraph()));
    eng.transport().setTempo(145.0);
    for (int b = 0; b < 20; ++b)
        eng.renderBlock(out, 2, 512);
    CHECK_NEAR(eng.transport().tempo(), 145.0, 1e-9);
    CHECK(engine::automationOverridden(*eng.latestGraph())); // Re-Enable Automation lights
    engine::reEnableAutomation(*eng.latestGraph());
    for (int b = 0; b < 5; ++b)
        eng.renderBlock(out, 2, 512);
    CHECK_NEAR(eng.transport().tempo(), 120.0, 1e-9);
    CHECK(! engine::automationOverridden(*eng.latestGraph()));
}

TEST_CASE("engine: replacing a track's arrangement clips takes effect without a rebuild")
{
    LaunchRig rig; // two MIDI tracks with note recorders; track 0 has an arrangement clip with a note every beat
    auto& tr = rig.eng.transport();
    tr.play();
    rig.run((long long) (2.5 * LaunchRig::kSamplesPerBeat)); // arrangement notes (pitch 72) at beats 0, 1, 2
    CHECK(rig.rec[0]->noteOns(72).size() >= 3);

    // Edit: replace the clip with one that holds pitch 90 once per bar from here on.
    rig.song.tracks[0].arrangementClips.clear();
    rig.song.tracks[0].arrangementClips.push_back(midiClip(90, 64.0, true, {0.0, 4.0, 8.0, 12.0, 16.0}));
    rig.eng.updateTrackClips(0, rig.song);
    rig.run((long long) (6.0 * LaunchRig::kSamplesPerBeat)); // beats 2.5 .. 8.5
    CHECK(rig.rec[0]->noteOns(72).size() == 3 + 0);           // the old clip no longer plays (nothing after the swap)
    const auto n90 = rig.rec[0]->noteOns(90);
    REQUIRE(! n90.empty());
    CHECK(std::llabs(n90.front() - 4 * 22050) <= 1);          // the new clip's note on beat 4
    // A second edit right away (before the first was cleaned up) must not crash or leak.
    rig.song.tracks[0].arrangementClips.clear();
    rig.eng.updateTrackClips(0, rig.song);
    rig.eng.updateTrackClips(0, rig.song);
    rig.run(4 * 512);
}

TEST_CASE("peaks: min/max overview follows the signal and ignores spans outside the file")
{
    juce::AudioBuffer<float> audio(2, 1000);
    audio.clear();
    audio.setSample(0, 100, 0.5f);
    audio.setSample(1, 100, 0.5f);
    audio.setSample(0, 700, -0.8f);
    audio.setSample(1, 700, -0.8f);
    auto p = engine::computePeaks(audio, 1000.0);
    float lo = 0, hi = 0;
    REQUIRE(p->range(0.0, 0.2, lo, hi));
    CHECK_NEAR(hi, 0.5, 1e-6);
    CHECK_NEAR(lo, 0.0, 1e-6);
    REQUIRE(p->range(0.0, 1.0, lo, hi));
    CHECK_NEAR(lo, -0.8, 1e-6);
    CHECK(! p->range(2.0, 3.0, lo, hi));
}

TEST_CASE("automation editing: pending edits and undo reach playback without rebuilding devices")
{
    juce::ScopedJuceInitialiser_GUI init;
    plugins::PluginHost host;
    midi::MidiManager midi;
    engine::AudioEngine eng(host, midi);
    auto loaded = io::loadAls(juce::File(SONORA_TEST_FIXTURES).getChildFile("minimal_set.xml"));
    REQUIRE(loaded);
    auto& song = *loaded.song;
    eng.loadSong(song);
    auto* originalGraph = eng.latestGraph();
    auto* originalDevice = originalGraph->nodes[0].devices[0].get();
    juce::AudioBuffer<float> output(2, 256);
    auto render = [&] { eng.renderBlock(output.getArrayOfWritePointers(), 2, 256); };
    eng.transport().play();
    render();
    model::EditHistory history;
    auto env = song.tracks[0].automation[0];
    env.points = {{0, 0.25}};
    history.perform(song, model::setAutomation(song, 0, env));
    eng.updateTrackClips(0, song);
    render();
    CHECK_NEAR((*eng.controls())[0].volume.load(), 0.25, 1e-6);
    CHECK(eng.latestGraph() == originalGraph);
    CHECK(originalGraph->nodes[0].devices[0].get() == originalDevice);
    history.undo(song);
    eng.updateTrackClips(0, song);
    render();
    CHECK_NEAR((*eng.controls())[0].volume.load(), 1, 1e-6);
    // Tempo edits on main must take effect in the same block as the swap.
    song.main.mixer.tempoTargetId = 789;
    model::AutomationEnvelope tempo;
    tempo.targetId = 789;
    tempo.points = {{0, 90}};
    song.main.automation = {tempo};
    eng.updateTrackClips((int) song.tracks.size() + (int) song.returns.size(), song);
    render();
    CHECK_NEAR(eng.transport().tempo(), 90, 1e-6);
}

#include "io/als/AlsWriter.h"

namespace
{
template <class Recording>
bool awaitRecording(Recording& recorder)
{
    const auto until = juce::Time::getMillisecondCounter() + 2000;
    while (! recorder.finished() && juce::Time::getMillisecondCounter() < until) juce::Thread::sleep(2);
    return recorder.finished();
}
}

TEST_CASE("recording: MIDI sample offsets, retrigger, note-off, held notes and undoable save/reload")
{
    auto loaded = io::loadAls(juce::File(SONORA_TEST_FIXTURES).getChildFile("recording_set.xml"));
    REQUIRE(loaded);
    auto& song = *loaded.song;
    engine::ControlBank controls(3);
    controls[0].armed.store(true);
    engine::Recorder recorder;
    std::string error;
    REQUIRE(recorder.start(song, controls, juce::File::getSpecialLocation(juce::File::tempDirectory), 1000, error));
    juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8) 100), 20);
    midi.addEvent(juce::MidiMessage::noteOn(2, 60, (juce::uint8) 80), 40);
    recorder.capture(nullptr, 0, midi, 8, 500, 100, true, true, false);
    midi.clear();
    midi.addEvent(juce::MidiMessage::noteOff(1, 60, (juce::uint8) 55), 50);
    midi.addEvent(juce::MidiMessage::noteOn(2, 60, (juce::uint8) 90), 80); // closes and retriggers channel 2
    recorder.capture(nullptr, 0, midi, 8.2, 500, 100, true, false, false);
    recorder.requestStop();
    recorder.capture(nullptr, 0, midi, 8.4, 500, 100, true, false, false);
    REQUIRE(awaitRecording(recorder));
    auto result = recorder.takeResult();
    REQUIRE(result.clips.size() == 1);
    const auto& clip = result.clips[0].second;
    CHECK_NEAR(clip.currentStart, 8, 1e-9);
    CHECK_NEAR(clip.currentEnd, 8.4, 1e-9);
    REQUIRE(clip.notes.size() == 3);
    CHECK_NEAR(clip.notes[0].start, 0.04, 1e-9);
    CHECK_NEAR(clip.notes[0].duration, 0.26, 1e-9);
    CHECK_NEAR(clip.notes[0].releaseVelocity, 55, 1e-9);
    CHECK_NEAR(clip.notes[1].duration, 0.28, 1e-9);
    CHECK_NEAR(clip.notes[2].duration, 0.04, 1e-9); // closes held note at take end
    model::EditHistory history;
    history.perform(song, model::insertClip(0, clip, "Record Take"));
    juce::TemporaryFile saved(".als");
    REQUIRE(io::saveAls(song, saved.getFile()).ok);
    auto again = io::loadAls(saved.getFile());
    REQUIRE(again);
    CHECK(again.song->tracks[0].arrangementClips.back().notes.size() == 3);
    history.undo(song);
    CHECK(song.tracks[0].arrangementClips.size() == 1);
    history.redo(song);
    CHECK(song.tracks[0].arrangementClips.size() == 2);
}

TEST_CASE("recording: stereo WAV on the worker, sample replacement and engine input monitoring")
{
    juce::ScopedJuceInitialiser_GUI init;
    auto loaded = io::loadAls(juce::File(SONORA_TEST_FIXTURES).getChildFile("recording_set.xml"));
    REQUIRE(loaded);
    auto& song = *loaded.song;
    plugins::PluginHost host;
    midi::MidiManager midi;
    engine::AudioEngine eng(host, midi);
    eng.loadSong(song);
    (*eng.controls())[1].armed.store(true);
    const auto folder = juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("sonora-recording", "", false);
    std::string error;
    REQUIRE(eng.recorder().start(song, *eng.controls(), folder, eng.sampleRate(), error));
    juce::AudioBuffer<float> input(2, 256), output(2, 256);
    for (int i = 0; i < 256; ++i) { input.setSample(0, i, 0.25f); input.setSample(1, i, -0.5f); }
    eng.transport().setPosition(8);
    eng.transport().play();
    auto render = [&] { eng.renderBlock(output.getArrayOfWritePointers(), 2, 256, input.getArrayOfReadPointers(), 2); };
    render();
    CHECK_NEAR(output.getSample(0, 200), 0.25, 1e-6);
    CHECK_NEAR(output.getSample(1, 200), -0.5, 1e-6);
    render();
    eng.transport().stop();
    render(); // finalisation at a block boundary
    REQUIRE(awaitRecording(eng.recorder()));
    auto result = eng.recorder().takeResult();
    REQUIRE(result.clips.size() == 1);
    const auto& clip = result.clips[0].second;
    CHECK(clip.lengthSamples == 512);
    CHECK(clip.warped); CHECK(clip.warpMode == model::WarpMode::Beats);
    CHECK_NEAR(clip.loop.loopEnd, 512.0 / (eng.sampleRate() * 60 / song.tempo), 1e-9);
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    auto reader = std::unique_ptr<juce::AudioFormatReader>(formats.createReaderFor(juce::File(clip.sample.absolutePath)));
    REQUIRE(reader != nullptr);
    CHECK(reader->lengthInSamples == 512);
    juce::AudioBuffer<float> recorded(2, 512);
    REQUIRE(reader->read(&recorded, 0, 512, 0, true, true));
    CHECK_NEAR(recorded.getSample(0, 511), 0.25, 1e-6);
    CHECK_NEAR(recorded.getSample(1, 511), -0.5, 1e-6);
    model::EditHistory history;
    history.perform(song, model::insertClip(1, clip, "Record Take"));
    const auto saved = folder.getChildFile("recorded.als");
    REQUIRE(io::saveAls(song, saved).ok);
    auto again = io::loadAls(saved);
    REQUIRE(again);
    const auto& reloaded = again.song->tracks[1].arrangementClips.back();
    CHECK(reloaded.sample.absolutePath == clip.sample.absolutePath);
    CHECK(reloaded.warped); CHECK(reloaded.warpMode == model::WarpMode::Beats);
    CHECK(reloaded.pitchCoarse == 0);
    CHECK_NEAR(reloaded.gain, 1, 1e-9);
    CHECK(reloaded.lengthSamples == 512);
    CHECK_NEAR(reloaded.sampleRate, eng.sampleRate(), 1e-9);
    REQUIRE(reloaded.warpMarkers.size() == 2);
    CHECK_NEAR(reloaded.warpMarkers[0].seconds, 0, 1e-9);
    CHECK_NEAR(reloaded.warpMarkers[1].seconds, 512 / eng.sampleRate(), 1e-9);
    folder.deleteRecursively();
}

TEST_CASE("recording: empty arm, stop before playback, loop boundary and capacity exhaustion")
{
    auto loaded = io::loadAls(juce::File(SONORA_TEST_FIXTURES).getChildFile("recording_set.xml"));
    REQUIRE(loaded);
    engine::ControlBank controls(3);
    engine::Recorder recorder;
    std::string error;
    const auto directory = juce::File::getSpecialLocation(juce::File::tempDirectory);
    CHECK(! recorder.start(*loaded.song, controls, directory, 1000, error));
    controls[0].armed.store(true);
    REQUIRE(recorder.start(*loaded.song, controls, directory, 1000, error));
    juce::MidiBuffer midi;
    recorder.requestStop();
    recorder.capture(nullptr, 0, midi, 0, 500, 100, false, false, false);
    REQUIRE(awaitRecording(recorder));
    CHECK(recorder.takeResult().clips.empty());
    REQUIRE(recorder.start(*loaded.song, controls, directory, 1000, error));
    recorder.capture(nullptr, 0, midi, 3.8, 500, 100, true, false, true);
    REQUIRE(awaitRecording(recorder));
    auto loop = recorder.takeResult();
    REQUIRE(loop.clips.size() == 1);
    CHECK_NEAR(loop.clips[0].second.currentEnd, 4, 1e-9);
    CHECK(! loop.warning.empty());
    REQUIRE(recorder.start(*loaded.song, controls, directory, 1000, error));
    midi.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8) 100), 0);
    for (int i = 0; i < 65537; ++i) recorder.capture(nullptr, 0, midi, i * 0.002, 500, 1, true, false, false);
    REQUIRE(awaitRecording(recorder));
    auto full = recorder.takeResult();
    CHECK(! full.warning.empty());
    REQUIRE(full.clips.size() == 1);
    CHECK(full.clips[0].second.notes.size() == 65536);
}

TEST_CASE("recording: a completed take installs in Session, launches and undoes without rebuilding devices")
{
    LaunchRig rig;
    const auto* graph = rig.eng.latestGraph();
    model::Clip clip = midiClip(90, 4, false, {0, 1, 2});
    model::EditHistory history;
    history.perform(rig.song, model::setSessionClip(rig.song, 0, 3, clip));
    rig.eng.updateTrackClips(0, rig.song);
    rig.eng.transport().play();
    rig.run(512); // publish the slot before launching it
    engine::launchClip(*rig.eng.latestGraph(), rig.eng.transport(), 0, 3, 0);
    rig.run(2 * 22050);
    CHECK(rig.rec[0]->noteOns(90).size() >= 2);
    CHECK(rig.eng.latestGraph() == graph);
    history.undo(rig.song);
    rig.eng.updateTrackClips(0, rig.song);
    rig.run(512);
    CHECK(rig.eng.latestGraph()->nodes[0].sessionClips.size() == rig.song.tracks[0].slots.size());
}

TEST_CASE("peaks: stereo pyramid queries agree with base buckets over arbitrary spans")
{
    juce::AudioBuffer<float> audio(2, 64 * 137 + 11);
    for (int i = 0; i < audio.getNumSamples(); ++i)
    {
        audio.setSample(0, i, (float) std::sin(i * 0.03));
        audio.setSample(1, i, (float) (-0.7 * std::cos(i * 0.017)));
    }
    const auto peaks = engine::computePeaks(audio, 1000);
    REQUIRE(peaks->levels.size() > 1);
    for (int a = 0; a < 137; a += 7)
        for (int b = a; b < 138; b += 13)
            for (int channel : {-1, 0, 1})
            {
                float lo = 0, hi = 0, expectedLo = 0, expectedHi = 0;
                REQUIRE(peaks->range(a * 0.064, b * 0.064, lo, hi, channel));
                const size_t c = (size_t) (channel + 1);
                for (int i = a; i <= b; ++i)
                {
                    expectedLo = std::min(expectedLo, peaks->levels[0].mins[c][(size_t) i]);
                    expectedHi = std::max(expectedHi, peaks->levels[0].maxs[c][(size_t) i]);
                }
                CHECK_NEAR(lo, expectedLo, 1e-6);
                CHECK_NEAR(hi, expectedHi, 1e-6);
            }
    // Antiphase stereo must remain visible in individual channel queries.
    audio.clear();
    audio.setSample(0, 100, 0.8f);
    audio.setSample(1, 100, -0.8f);
    const auto antiphase = engine::computePeaks(audio, 1000);
    float lo = 0, hi = 0;
    REQUIRE(antiphase->range(0, 0.2, lo, hi, 0));
    CHECK_NEAR(hi, 0.8, 1e-6);
    REQUIRE(antiphase->range(0, 0.2, lo, hi, 1));
    CHECK_NEAR(lo, -0.8, 1e-6);
}

TEST_CASE("browser preview: asynchronous decode, resampling, stop and replacement without a song")
{
    juce::ScopedJuceInitialiser_GUI init;
    plugins::PluginHost host;
    midi::MidiManager midi;
    engine::AudioEngine eng(host, midi);
    juce::TemporaryFile sample(".wav");
    juce::WavAudioFormat format;
    std::unique_ptr<juce::OutputStream> stream = sample.getFile().createOutputStream();
    auto writer = format.createWriterFor(stream, juce::AudioFormatWriterOptions{}.withSampleRate(22050).withNumChannels(2).withBitsPerSample(24));
    REQUIRE(writer != nullptr);
    juce::AudioBuffer<float> audio(2, 2048), output(2, 256);
    for (int i = 0; i < 2048; ++i) { audio.setSample(0, i, 0.8f); audio.setSample(1, i, -0.4f); }
    REQUIRE(writer->writeFromAudioSampleBuffer(audio, 0, 2048));
    writer.reset();
    eng.previewSample(sample.getFile().getFullPathName().toStdString());
    REQUIRE(eng.sampleCache().waitForLoads(2000));
    auto render = [&] { eng.renderBlock(output.getArrayOfWritePointers(), 2, 256); };
    render();
    CHECK(eng.previewPlaying());
    CHECK_NEAR(output.getSample(0, 200), 0.2, 1e-5);
    CHECK_NEAR(output.getSample(1, 200), -0.1, 1e-5);
    for (int i = 0; i < 15; ++i) render();
    CHECK(! eng.previewPlaying()); // 2048 frames at 22050 => 4096 output frames at 44100
    eng.previewSample(sample.getFile().getFullPathName().toStdString());
    render();
    CHECK(eng.previewPlaying());
    eng.previewSample("");
    render();
    CHECK(! eng.previewPlaying());
    CHECK_NEAR(output.getMagnitude(0, 0, 256), 0, 1e-7);
}

TEST_CASE("MIDI probability: zero and one, paired events and decisions independent of block splits")
{
    model::Clip clip;
    clip.uid = 1234;
    clip.currentEnd = 128;
    clip.loop = {0, 1, 0, true};
    model::MidiNote note;
    note.start = 0.125;
    note.duration = 0.5;
    note.pitch = 60;
    note.probability = 0;
    clip.notes = {note};
    auto prepared = engine::prepareClip(clip, std::shared_ptr<const engine::SampleData>{});
    juce::MidiBuffer buffer;
    buffer.ensureSize(8192);
    engine::renderMidiClip(prepared, {0, 2048, 0, 16}, buffer);
    CHECK(buffer.isEmpty());
    clip.notes[0].probability = 1;
    prepared = engine::prepareClip(clip, std::shared_ptr<const engine::SampleData>{});
    engine::renderMidiClip(prepared, {0, 2048, 0, 16}, buffer);
    CHECK(buffer.getNumEvents() == 256);
    clip.notes[0].probability = 0.5f;
    prepared = engine::prepareClip(clip, std::shared_ptr<const engine::SampleData>{});
    buffer.clear();
    engine::renderMidiClip(prepared, {0, 2048, 0, 16}, buffer);
    CHECK(buffer.getNumEvents() > 64);
    CHECK(buffer.getNumEvents() < 192);
    std::vector<std::pair<int, bool>> whole, split;
    int ons = 0, offs = 0;
    for (const auto event : buffer)
    {
        whole.emplace_back(event.samplePosition, event.getMessage().isNoteOn());
        ons += event.getMessage().isNoteOn();
        offs += event.getMessage().isNoteOff();
    }
    CHECK(ons == offs);
    for (int position = 0; position < 2048; position += 7)
    {
        buffer.clear();
        engine::renderMidiClip(prepared, {0, std::min(7, 2048 - position), position / 16.0, 16}, buffer);
        for (const auto event : buffer) split.emplace_back(position + event.samplePosition, event.getMessage().isNoteOn());
    }
    CHECK(whole == split);
}

TEST_CASE("MIDI clip: a start marker before the loop plays a lead-in once")
{
    auto clip = makeMidiClip(0, 8, 2, 4, true, {{0, 0.5, 60, 100}, {2, 0.5, 62, 100}});
    clip.loop.startRelative = -2; // absolute start marker at content beat 0
    juce::MidiBuffer buffer;
    engine::renderMidiClip(clip, {0, 800, 0, 100}, buffer);
    int lead = 0, repeat = 0;
    for (const auto event : buffer)
        if (event.getMessage().isNoteOn())
        {
            lead += event.getMessage().getNoteNumber() == 60;
            repeat += event.getMessage().getNoteNumber() == 62;
        }
    CHECK(lead == 1);
    CHECK(repeat == 3);
}

TEST_CASE("audio input: mono stereo channels Auto In Off and empty-track recording")
{
    juce::ScopedJuceInitialiser_GUI init;
    auto loaded = io::loadAls(juce::File(SONORA_TEST_FIXTURES).getChildFile("recording_set.xml"));
    REQUIRE(loaded);
    auto& song = *loaded.song;
    song.tracks[0].devices.clear();
    song.tracks[0].arrangementClips.clear();
    song.tracks[0].monitoring = 2;
    auto& track = song.tracks[1];
    track.arrangementClips.clear(); track.slots.clear(); track.devices.clear();
    model::setInputSelection(track, {model::InputKind::AudioMono, 2, {}});
    track.monitoring = 0;
    plugins::PluginHost host; midi::MidiManager midi;
    engine::AudioEngine engine(host, midi);
    engine.loadSong(song);
    auto& controls = (*engine.controls())[1];
    juce::AudioBuffer<float> input(4, 128), output(2, 128);
    for (int channel = 0; channel < 4; ++channel)
        for (int sample = 0; sample < 128; ++sample) input.setSample(channel, sample, 0.1f * (float) (channel + 1));
    auto render = [&](int channels = 4) { engine.renderBlock(output.getArrayOfWritePointers(), 2, 128, input.getArrayOfReadPointers(), channels); };
    render();
    CHECK_NEAR(output.getSample(0, 64), 0.3, 1e-6); // In monitors an unarmed track
    CHECK_NEAR(output.getSample(1, 64), 0.3, 1e-6); // selected mono input duplicated for processing
    controls.monitoring.store(1); render(); CHECK_NEAR(output.getSample(0, 64), 0, 1e-6);
    controls.armed.store(true); render(); CHECK_NEAR(output.getSample(0, 64), 0.3, 1e-6);
    controls.monitoring.store(2); render(); CHECK_NEAR(output.getSample(0, 64), 0, 1e-6);
    controls.inputRoute.store(model::packInput({model::InputKind::AudioStereo, 2, {}}));
    controls.monitoring.store(0); render();
    CHECK_NEAR(output.getSample(0, 64), 0.3, 1e-6); CHECK_NEAR(output.getSample(1, 64), 0.4, 1e-6);
    render(3); CHECK_NEAR(output.getSample(1, 64), 0, 1e-6); // missing channel is silence, never a wrong-channel alias
    controls.inputRoute.store(model::packInput({model::InputKind::AudioMono, 2, {}}));
    controls.monitoring.store(2); // record even when monitoring is off
    const auto folder = juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("sonora-input-record", "", false);
    std::string error;
    REQUIRE(engine.recorder().start(song, *engine.controls(), folder, engine.sampleRate(), error));
    engine.transport().play(); render();
    CHECK_NEAR(output.getSample(0, 64), 0, 1e-6);
    engine.transport().stop(); render();
    REQUIRE(awaitRecording(engine.recorder()));
    auto result = engine.recorder().takeResult(); REQUIRE(result.clips.size() == 1);
    const auto& clip = result.clips[0].second;
    CHECK(clip.sourceXml != nullptr); // no audio clip existed anywhere in the set
    juce::AudioFormatManager formats; formats.registerBasicFormats();
    auto reader = std::unique_ptr<juce::AudioFormatReader>(formats.createReaderFor(juce::File(clip.sample.absolutePath)));
    REQUIRE(reader);
    CHECK(reader->numChannels == 1);
    CHECK(reader->lengthInSamples == 128);
    juce::AudioBuffer<float> recorded(1, 128); REQUIRE(reader->read(&recorded, 0, 128, 0, true, false));
    CHECK_NEAR(recorded.getSample(0, 127), 0.3, 1e-6);
    model::EditHistory history; history.perform(song, model::recordArrangementTake(song, 1, clip));
    const auto saved = folder.getChildFile("recorded.als"); REQUIRE(io::saveAls(song, saved).ok);
    auto reloaded = io::loadAls(saved); REQUIRE(reloaded);
    REQUIRE(reloaded.song->tracks[1].arrangementClips.size() == 1);
    CHECK(reloaded.song->tracks[1].arrangementClips[0].lengthSamples == 128);
    CHECK(reloaded.song->tracks[1].inputRouting == "AudioIn/External/M2");
    history.undo(song); CHECK(song.tracks[1].arrangementClips.empty());
    folder.deleteRecursively();
}

TEST_CASE("MIDI input: selected port and channel reach recording while Off suppresses monitoring")
{
    juce::ScopedJuceInitialiser_GUI init;
    auto loaded = io::loadAls(juce::File(SONORA_TEST_FIXTURES).getChildFile("recording_set.xml")); REQUIRE(loaded);
    auto& song = *loaded.song;
    auto& track = song.tracks[0];
    model::setInputSelection(track, {model::InputKind::Midi, 2, "selected-port"});
    track.monitoring = 2; track.devices.clear(); track.arrangementClips.clear();
    song.tracks[1].arrangementClips.clear();
    plugins::PluginHost host; midi::MidiManager midi;
    engine::AudioEngine engine(host, midi); engine.loadSong(song);
    // Keep a MIDI template in Session for recording on an empty arrangement track.
    auto source = loaded.song->tracks[0].slots;
    if (source.empty())
    {
        auto templateSet = io::loadAls(juce::File(SONORA_TEST_FIXTURES).getChildFile("two_tracks.xml")); REQUIRE(templateSet);
        song.tracks[0].slots = templateSet.song->tracks[0].slots;
    }
    (*engine.controls())[0].armed.store(true);
    std::string error;
    REQUIRE(engine.recorder().start(song, *engine.controls(), juce::File::getSpecialLocation(juce::File::tempDirectory), engine.sampleRate(), error));
    const auto selected = model::midiPortToken("selected-port"), other = model::midiPortToken("other-port");
    midi.handleMessage(juce::MidiMessage::noteOn(2, 60, (juce::uint8) 100), true, false, selected);
    midi.handleMessage(juce::MidiMessage::noteOn(3, 61, (juce::uint8) 100), true, false, selected);
    midi.handleMessage(juce::MidiMessage::noteOn(2, 62, (juce::uint8) 100), true, false, other);
    juce::AudioBuffer<float> output(2, 128);
    engine.transport().play(); engine.renderBlock(output.getArrayOfWritePointers(), 2, 128);
    CHECK(engine.latestGraph()->nodes[0].inputMidi.getNumEvents() == 1);
    for (const auto event : engine.latestGraph()->nodes[0].midi) CHECK(event.data[0] != 0x91);
    engine.transport().stop(); engine.renderBlock(output.getArrayOfWritePointers(), 2, 128);
    REQUIRE(awaitRecording(engine.recorder()));
    auto result = engine.recorder().takeResult(); REQUIRE(result.clips.size() == 1);
    REQUIRE(result.clips[0].second.notes.size() == 1);
    CHECK(result.clips[0].second.notes[0].pitch == 60);
}

TEST_CASE("monitoring: Auto follows sample boundaries and In consumes queued Session launches while suppressing clips")
{
    juce::ScopedJuceInitialiser_GUI init;
    auto loaded = io::loadAls(juce::File(SONORA_TEST_FIXTURES).getChildFile("recording_set.xml")); REQUIRE(loaded);
    auto& song = *loaded.song;
    song.tempo = 120;
    song.tracks[0].arrangementClips.clear(); song.tracks[0].devices.clear(); song.tracks[0].monitoring = 2;
    auto& track = song.tracks[1]; track.devices.clear();
    auto clip = track.arrangementClips[0];
    clip.sample.absolutePath = "/missing-monitor-test.wav";
    clip.sample.relativePath.clear();
    clip.currentStart = 32.0 / 22050;
    clip.currentEnd = 96.0 / 22050;
    track.arrangementClips = {clip};
    track.slots = {{clip}};
    model::setInputSelection(track, {model::InputKind::AudioMono, 1, {}});
    track.monitoring = 1;
    plugins::PluginHost host; midi::MidiManager midi;
    engine::AudioEngine engine(host, midi); engine.loadSong(song);
    auto& control = (*engine.controls())[1]; control.armed.store(true);
    juce::AudioBuffer<float> input(2, 128), output(2, 128);
    input.clear(); for (int sample = 0; sample < 128; ++sample) input.setSample(1, sample, 0.25f);
    auto render = [&] { engine.renderBlock(output.getArrayOfWritePointers(), 2, 128, input.getArrayOfReadPointers(), 2); };
    engine.transport().play(); render();
    CHECK_NEAR(output.getSample(0, 16), 0.25, 1e-6);
    CHECK_NEAR(output.getSample(0, 64), 0, 1e-6); // clip inhibits monitoring precisely inside this block
    CHECK_NEAR(output.getSample(0, 112), 0.25, 1e-6);
    control.monitoring.store(0); control.armed.store(false);
    engine::launchClip(*engine.latestGraph(), engine.transport(), 1, 0, 0);
    render();
    CHECK(control.playingSlot.load() == 0);
    CHECK(control.queuedSlot.load() == engine::TrackControls::kNothingQueued);
    CHECK_NEAR(output.getSample(0, 64), 0.25, 1e-6); // input only, even with a Session clip active
    control.monitoring.store(2); render(); CHECK_NEAR(output.getSample(0, 64), 0, 1e-6);
}

TEST_CASE("recording: resampling captures the completed Main output without external inputs or input feedback")
{
    juce::ScopedJuceInitialiser_GUI init;
    auto loaded = io::loadAls(juce::File(SONORA_TEST_FIXTURES).getChildFile("recording_set.xml")); REQUIRE(loaded);
    auto& song = *loaded.song;
    song.tracks[0].kind = model::TrackKind::Audio;
    song.tracks[0].devices.clear(); song.tracks[0].arrangementClips.clear();
    song.tracks[0].mixer = {}; song.tracks[0].monitoring = 0;
    model::setInputSelection(song.tracks[0], {model::InputKind::AudioMono, 0, {}});
    song.tracks[1].devices.clear(); song.tracks[1].arrangementClips.clear(); song.tracks[1].monitoring = 2;
    model::setInputSelection(song.tracks[1], {model::InputKind::Resampling, 0, {}});
    plugins::PluginHost host; midi::MidiManager midi;
    engine::AudioEngine engine(host, midi); engine.loadSong(song);
    (*engine.controls())[1].armed.store(true);
    const auto folder = juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("sonora-resample", "", false);
    std::string error; REQUIRE(engine.recorder().start(song, *engine.controls(), folder, engine.sampleRate(), error));
    juce::AudioBuffer<float> input(1, 128), output(2, 128);
    for (int sample = 0; sample < 128; ++sample) input.setSample(0, sample, 0.125f);
    engine.transport().play();
    engine.renderBlock(output.getArrayOfWritePointers(), 2, 128, input.getArrayOfReadPointers(), 1);
    CHECK_NEAR(output.getSample(0, 64), 0.125, 1e-6);
    engine.transport().stop(); engine.renderBlock(output.getArrayOfWritePointers(), 2, 128);
    REQUIRE(awaitRecording(engine.recorder()));
    auto result = engine.recorder().takeResult(); REQUIRE(result.clips.size() == 1);
    juce::AudioFormatManager formats; formats.registerBasicFormats();
    auto reader = std::unique_ptr<juce::AudioFormatReader>(formats.createReaderFor(juce::File(result.clips[0].second.sample.absolutePath))); REQUIRE(reader);
    CHECK(reader->numChannels == 2);
    juce::AudioBuffer<float> recorded(2, 128); REQUIRE(reader->read(&recorded, 0, 128, 0, true, true));
    CHECK_NEAR(recorded.getSample(0, 127), 0.125, 1e-6);
    CHECK_NEAR(recorded.getSample(1, 127), 0.125, 1e-6);
    folder.deleteRecursively();
}

TEST_CASE("count-in: transport holds position and starts within a block, cancellation and running bypass")
{
    engine::Transport transport;
    transport.setPosition(8);
    transport.setTimeSignature(7, 8);
    transport.playWithCountIn(1);
    auto block = transport.beginBlock(128, 100);
    CHECK(! block.playing); CHECK(block.countInSamples == 128); CHECK_NEAR(block.position, 8, 1e-9);
    block = transport.beginBlock(128, 100);
    CHECK(! block.playing); CHECK_NEAR(transport.countInPosition(), -0.94, 1e-9);
    block = transport.beginBlock(128, 100);
    CHECK(block.playing); CHECK(block.playOffset == 94); CHECK(block.countInSamples == 94);
    CHECK(! transport.isCountingIn()); CHECK_NEAR(block.position, 8, 1e-9);
    transport.playWithCountIn(2); block = transport.beginBlock(128, 100);
    CHECK(block.playing); CHECK(block.countInSamples == 0); // recording during playback has no count-in
    transport.cancelCountIn(); block = transport.beginBlock(128, 100); CHECK(block.playing);
    transport.stop(); block = transport.beginBlock(128, 100); CHECK(block.justStopped); CHECK(! block.playing);
    transport.playWithCountIn(4); block = transport.beginBlock(128, 100); CHECK(transport.isCountingIn());
    transport.stop(); block = transport.beginBlock(128, 100); CHECK(block.justStopped); CHECK(! transport.isCountingIn());
    transport.playWithCountIn(1); transport.cancelCountIn(); block = transport.beginBlock(128, 100);
    CHECK(! block.playing); CHECK(! transport.isCountingIn());
}

TEST_CASE("punch recording: sample slicing, held MIDI through punch-in and truncation at punch-out")
{
    auto loaded = io::loadAls(juce::File(SONORA_TEST_FIXTURES).getChildFile("recording_set.xml")); REQUIRE(loaded);
    auto& song = *loaded.song;
    song.punchIn = song.punchOut = true; song.loopStart = 8.5; song.loopLength = 1;
    song.tracks[1].arrangementClips.clear();
    model::setInputSelection(song.tracks[1], {model::InputKind::AudioMono, 0, {}});
    engine::ControlBank controls(3); controls[0].armed.store(true); controls[1].armed.store(true);
    engine::Recorder recorder;
    const auto folder = juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("sonora-punch", "", false);
    std::string error; REQUIRE(recorder.start(song, controls, folder, 1000, error));
    juce::AudioBuffer<float> input(1, 100); juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8) 90), 20);
    recorder.capture(nullptr, 0, midi, 8, 100, 100, false, false, false); // held through count-in too
    midi.clear();
    for (int i = 0; i < 100; ++i) input.setSample(0, i, 0.1f + (float) i * 0.001f);
    midi.addEvent(juce::MidiMessage::noteOn(1, 62, (juce::uint8) 70), 10);
    midi.addEvent(juce::MidiMessage::noteOff(1, 62), 40);
    midi.addEvent(juce::MidiMessage::noteOn(1, 61, (juce::uint8) 80), 80);
    const auto range = recorder.captureRange(8, 100, 100, true, false);
    CHECK(range.offset == 50); CHECK(range.count == 50); CHECK(! range.punchEnd);
    recorder.capture(input.getArrayOfReadPointers(), 1, midi, 8, 100, 100, true, false, false);
    for (int i = 0; i < 100; ++i) input.setSample(0, i, 0.2f + (float) i * 0.001f);
    midi.clear(); midi.addEvent(juce::MidiMessage::noteOff(1, 60, (juce::uint8) 55), 20);
    midi.addEvent(juce::MidiMessage::noteOff(1, 61), 80);
    midi.addEvent(juce::MidiMessage::noteOn(1, 63, (juce::uint8) 99), 90);
    recorder.capture(input.getArrayOfReadPointers(), 1, midi, 9, 100, 100, true, false, false);
    REQUIRE(awaitRecording(recorder)); auto result = recorder.takeResult(); REQUIRE(result.clips.size() == 2);
    CHECK(result.warning.empty());
    const auto& notes = result.clips[0].second; REQUIRE(notes.notes.size() == 2);
    CHECK_NEAR(notes.currentStart, 8.5, 1e-9); CHECK_NEAR(notes.currentEnd, 9.5, 1e-9);
    CHECK(notes.notes[0].pitch == 60); CHECK_NEAR(notes.notes[0].start, 0, 1e-9);
    CHECK_NEAR(notes.notes[0].duration, 0.7, 1e-9); CHECK_NEAR(notes.notes[0].releaseVelocity, 55, 1e-9);
    CHECK(notes.notes[1].pitch == 61); CHECK_NEAR(notes.notes[1].start, 0.3, 1e-9); CHECK_NEAR(notes.notes[1].duration, 0.7, 1e-9);
    juce::AudioFormatManager formats; formats.registerBasicFormats();
    auto reader = std::unique_ptr<juce::AudioFormatReader>(formats.createReaderFor(juce::File(result.clips[1].second.sample.absolutePath))); REQUIRE(reader);
    CHECK(reader->lengthInSamples == 100); CHECK(reader->numChannels == 1);
    juce::AudioBuffer<float> recorded(1, 100); REQUIRE(reader->read(&recorded, 0, 100, 0, true, false));
    CHECK_NEAR(recorded.getSample(0, 0), 0.15, 1e-6); CHECK_NEAR(recorded.getSample(0, 49), 0.199, 1e-6);
    CHECK_NEAR(recorded.getSample(0, 50), 0.2, 1e-6); CHECK_NEAR(recorded.getSample(0, 99), 0.249, 1e-6);
    for (double start : {9.0, 10.0})
    {
        REQUIRE(recorder.start(song, controls, folder, 1000, error)); midi.clear();
        recorder.capture(input.getArrayOfReadPointers(), 1, midi, start, 100, 100, true, false, false);
        REQUIRE(awaitRecording(recorder)); result = recorder.takeResult();
        if (start > 9.5) CHECK(result.clips.empty());
        else { REQUIRE(result.clips.size() == 2); CHECK_NEAR(result.clips[0].second.currentStart, 9, 1e-9); CHECK_NEAR(result.clips[0].second.currentEnd, 9.5, 1e-9); }
    }
    REQUIRE(recorder.start(song, controls, folder, 1000, error, false));
    recorder.capture(input.getArrayOfReadPointers(), 1, midi, 0, 100, 100, true, false, false);
    recorder.requestStop(); recorder.capture(nullptr, 0, midi, 1, 100, 0, true, false, false);
    REQUIRE(awaitRecording(recorder)); result = recorder.takeResult(); REQUIRE(result.clips.size() == 2);
    CHECK_NEAR(result.clips[0].second.currentStart, 0, 1e-9); CHECK_NEAR(result.clips[0].second.currentEnd, 1, 1e-9);
    reader.reset(); folder.deleteRecursively();
}

TEST_CASE("count-in: engine clicks without timeline playback or capture and records only the post-count samples")
{
    juce::ScopedJuceInitialiser_GUI init;
    auto loaded = io::loadAls(juce::File(SONORA_TEST_FIXTURES).getChildFile("recording_set.xml")); REQUIRE(loaded);
    auto& song = *loaded.song; song.timeSigNumerator = 3; song.timeSigDenominator = 8;
    song.tracks[1].arrangementClips.clear(); song.tracks[1].monitoring = 2;
    model::setInputSelection(song.tracks[1], {model::InputKind::AudioMono, 0, {}});
    plugins::PluginHost host; midi::MidiManager midi; engine::AudioEngine eng(host, midi); eng.loadSong(song);
    (*eng.controls())[1].armed.store(true);
    const auto folder = juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("sonora-count", "", false);
    std::string error; REQUIRE(eng.recorder().start(song, *eng.controls(), folder, eng.sampleRate(), error));
    eng.transport().setPosition(8); eng.transport().playWithCountIn(1);
    juce::AudioBuffer<float> input(1, 128), output(2, 128);
    for (int i = 0; i < 128; ++i) input.setSample(0, i, 0.1f + (float) i * 0.001f);
    const int countFrames = (int) std::ceil(1.5 * eng.sampleRate() * 60 / song.tempo);
    const int blocks = (countFrames + 127) / 128;
    bool clicked = false;
    for (int b = 0; b < blocks; ++b)
    {
        eng.renderBlock(output.getArrayOfWritePointers(), 2, 128, input.getArrayOfReadPointers(), 1);
        clicked |= output.getMagnitude(0, 128) > 0.01f;
        if (b < blocks - 1) { CHECK(! eng.transport().isPlaying()); CHECK_NEAR(eng.transport().positionBeats(), 8, 1e-9); }
    }
    CHECK(clicked); CHECK(eng.transport().isPlaying()); CHECK(! eng.transport().isCountingIn());
    const int offset = countFrames % 128, frames = blocks * 128 - countFrames;
    CHECK_NEAR(eng.transport().positionBeats(), 8 + frames / (eng.sampleRate() * 60 / song.tempo), 1e-8);
    eng.transport().stop(); eng.renderBlock(output.getArrayOfWritePointers(), 2, 128);
    REQUIRE(awaitRecording(eng.recorder())); auto result = eng.recorder().takeResult(); REQUIRE(result.clips.size() == 1);
    CHECK(result.clips[0].second.lengthSamples == frames); CHECK_NEAR(result.clips[0].second.currentStart, 8, 1e-9);
    juce::AudioFormatManager formats; formats.registerBasicFormats();
    auto reader = std::unique_ptr<juce::AudioFormatReader>(formats.createReaderFor(juce::File(result.clips[0].second.sample.absolutePath))); REQUIRE(reader);
    juce::AudioBuffer<float> recorded(1, frames); REQUIRE(reader->read(&recorded, 0, frames, 0, true, false));
    CHECK_NEAR(recorded.getSample(0, 0), input.getSample(0, offset), 1e-6);
    reader.reset();
    REQUIRE(eng.recorder().start(song, *eng.controls(), folder, eng.sampleRate(), error));
    eng.transport().playWithCountIn(1); eng.renderBlock(output.getArrayOfWritePointers(), 2, 128);
    eng.transport().stop(); eng.renderBlock(output.getArrayOfWritePointers(), 2, 128);
    REQUIRE(awaitRecording(eng.recorder())); CHECK(eng.recorder().takeResult().clips.empty());
    CHECK(folder.findChildFiles(juce::File::findFiles, false, "*.wav").size() == 1); // cancelled count-in leaves no empty WAV
    folder.deleteRecursively();
}

TEST_CASE("punch monitoring: clips play before and after the exact recording interval, input replaces only inside")
{
    juce::ScopedJuceInitialiser_GUI init;
    auto loaded = io::loadAls(juce::File(SONORA_TEST_FIXTURES).getChildFile("recording_set.xml")); REQUIRE(loaded);
    auto& song = *loaded.song; const double spb = 22050;
    song.punchIn = song.punchOut = true; song.loopStart = 32 / spb; song.loopLength = 64 / spb;
    auto& track = song.tracks[1]; track.devices.clear(); track.monitoring = 1;
    model::setInputSelection(track, {model::InputKind::AudioMono, 0, {}});
    auto& clip = track.arrangementClips[0]; clip.currentStart = 0; clip.currentEnd = 1;
    clip.warped = false; clip.loop = {0, 1, 0, false}; clip.gain = 1; clip.pitchCoarse = 0; clip.pitchFine = 0;
    auto sample = std::make_shared<engine::SampleData>(); sample->sampleRate = 44100; sample->audio.setSize(2, 44100);
    for (int channel = 0; channel < 2; ++channel) for (int i = 0; i < 44100; ++i) sample->audio.setSample(channel, i, 0.125f);
    plugins::PluginHost host; midi::MidiManager midi; engine::AudioEngine eng(host, midi); eng.loadSong(song);
    eng.latestGraph()->nodes[1].clips[0] = engine::prepareClip(clip, std::shared_ptr<const engine::SampleData>(sample));
    (*eng.controls())[1].armed.store(true);
    const auto folder = juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("sonora-punch-mask", "", false);
    std::string error; REQUIRE(eng.recorder().start(song, *eng.controls(), folder, eng.sampleRate(), error));
    juce::AudioBuffer<float> input(1, 128), output(2, 128);
    for (int i = 0; i < 128; ++i) input.setSample(0, i, 0.25f);
    eng.transport().play(); eng.renderBlock(output.getArrayOfWritePointers(), 2, 128, input.getArrayOfReadPointers(), 1);
    CHECK_NEAR(output.getSample(0, 16), 0.125, 1e-6); CHECK_NEAR(output.getSample(0, 31), 0.125, 1e-6);
    CHECK_NEAR(output.getSample(0, 32), 0.25, 1e-6); CHECK_NEAR(output.getSample(0, 95), 0.25, 1e-6);
    CHECK_NEAR(output.getSample(0, 96), 0.125, 1e-6); CHECK_NEAR(output.getSample(0, 112), 0.125, 1e-6);
    REQUIRE(awaitRecording(eng.recorder())); auto result = eng.recorder().takeResult(); REQUIRE(result.clips.size() == 1);
    CHECK(result.clips[0].second.lengthSamples == 64); CHECK(eng.transport().isPlaying());
    CHECK_NEAR(result.clips[0].second.currentStart, 32 / spb, 1e-9); CHECK_NEAR(result.clips[0].second.currentEnd, 96 / spb, 1e-9);
    folder.deleteRecursively();
}

TEST_CASE("punch recording: independent in/out flags and cancelling while waiting for punch-in")
{
    auto loaded = io::loadAls(juce::File(SONORA_TEST_FIXTURES).getChildFile("recording_set.xml")); REQUIRE(loaded);
    auto& song = *loaded.song; song.loopStart = 8; song.loopLength = 1;
    engine::ControlBank controls(3); controls[0].armed.store(true);
    engine::Recorder recorder; std::string error; juce::MidiBuffer midi;
    const auto folder = juce::File::getSpecialLocation(juce::File::tempDirectory);
    for (bool in : {false, true}) for (bool out : {false, true})
    {
        song.punchIn = in; song.punchOut = out;
        REQUIRE(recorder.start(song, controls, folder, 1000, error));
        const auto before = recorder.captureRange(7, 100, 100, true, false);
        CHECK(before.count == (in ? 0 : 100));
        const auto after = recorder.captureRange(9.5, 100, 100, true, false);
        CHECK(after.count == (out ? 0 : 100)); CHECK(after.punchEnd == out);
        recorder.capture(nullptr, 0, midi, 7, 100, 100, true, false, false);
        recorder.requestStop(); recorder.capture(nullptr, 0, midi, 8, 100, 0, true, false, false);
        REQUIRE(awaitRecording(recorder)); const auto result = recorder.takeResult();
        CHECK(result.clips.size() == (in ? 0u : 1u));
    }
}

TEST_CASE("metronome: denominator ticks and fractional-quarter bar accents in 3/8")
{
    engine::Metronome metronome; metronome.prepare(1000);
    const engine::BlockSegment segment{0, 400, 0, 200};
    std::array<float, 400> left{}, right{};
    metronome.render(&segment, 1, 1.5, 400, left.data(), right.data(), 0.5);
    auto peak = [&](int start) { float value = 0; for (int i = start; i < start + 20; ++i) value = std::max(value, std::abs(left[(size_t) i])); return value; };
    CHECK(peak(0) > 0.3f); CHECK(peak(100) > 0.2f); CHECK(peak(200) > 0.2f); CHECK(peak(300) > 0.3f);
    CHECK(peak(300) > peak(100) * 1.2f); CHECK_NEAR(left[99], 0, 1e-9);
}

TEST_CASE("loop recording: continuous WAV, held MIDI, chronological passes and one undoable comp save")
{
    auto loaded = io::loadAls(juce::File(SONORA_TEST_FIXTURES).getChildFile("recording_set.xml")); REQUIRE(loaded);
    auto& song = *loaded.song; song.loopOn = true; song.loopStart = 0; song.loopLength = 1;
    song.tracks[1].arrangementClips.clear(); model::setInputSelection(song.tracks[1], {model::InputKind::AudioMono, 0, {}});
    engine::ControlBank controls(3); controls[0].armed.store(true); controls[1].armed.store(true);
    engine::Recorder recorder; std::string error;
    const auto folder = juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("sonora-loop", "", false);
    REQUIRE(recorder.start(song, controls, folder, 1000, error));
    juce::AudioBuffer<float> input(1, 128); juce::MidiBuffer midi;
    for (int i = 0; i < 128; ++i) input.setSample(0, i, 0.1f + (float) i * 0.001f);
    midi.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8) 90), 20);
    midi.addEvent(juce::MidiMessage::noteOn(1, 61, (juce::uint8) 80), 90);
    midi.addEvent(juce::MidiMessage::noteOff(1, 60), 110);
    recorder.capture(input.getArrayOfReadPointers(), 1, midi, 0, 100, 100, true, false, true);
    CHECK(recorder.active());
    recorder.capture(input.getArrayOfReadPointers(), 1, midi, 0, 100, 28, true, false, false, nullptr, 100);
    for (int i = 0; i < 128; ++i) input.setSample(0, i, 0.2f + (float) i * 0.001f);
    midi.clear(); midi.addEvent(juce::MidiMessage::noteOff(1, 61), 10);
    recorder.capture(input.getArrayOfReadPointers(), 1, midi, 0.28, 100, 72, true, false, true);
    recorder.capture(input.getArrayOfReadPointers(), 1, midi, 0, 100, 50, true, false, false, nullptr, 72);
    recorder.requestStop(); recorder.capture(nullptr, 0, midi, 0.5, 100, 0, true, false, false);
    REQUIRE(awaitRecording(recorder)); auto result = recorder.takeResult(); CHECK(result.warning.empty());
    REQUIRE(result.passes.size() == 2); REQUIRE(result.passes[0].second.size() == 3); REQUIRE(result.passes[1].second.size() == 3);
    const auto& first = result.passes[0].second[0]; const auto& second = result.passes[0].second[1];
    REQUIRE(first.notes.size() == 2); REQUIRE(second.notes.size() == 2);
    CHECK_NEAR(first.notes[0].start, 0.2, 1e-9); CHECK_NEAR(first.notes[0].duration, 0.8, 1e-9);
    CHECK_NEAR(second.notes[0].start, 1, 1e-9); CHECK_NEAR(second.notes[0].duration, 0.1, 1e-9);
    CHECK_NEAR(second.notes[1].duration, 0.38, 1e-9);
    CHECK(result.passes[0].second[2].notes.empty());
    CHECK_NEAR(result.passes[1].second[2].loop.loopStart, 2, 1e-9); CHECK_NEAR(result.passes[1].second[2].currentEnd, 0.5, 1e-9);
    CHECK(result.passes[1].second[0].sample.absolutePath == result.passes[1].second[2].sample.absolutePath);
    juce::AudioFormatManager formats; formats.registerBasicFormats();
    auto reader = std::unique_ptr<juce::AudioFormatReader>(formats.createReaderFor(juce::File(result.clips[1].second.sample.absolutePath))); REQUIRE(reader);
    CHECK(reader->lengthInSamples == 250); juce::AudioBuffer<float> recorded(1, 250); REQUIRE(reader->read(&recorded, 0, 250, 0, true, false));
    CHECK_NEAR(recorded.getSample(0, 0), 0.1, 1e-6); CHECK_NEAR(recorded.getSample(0, 127), 0.227, 1e-6);
    CHECK_NEAR(recorded.getSample(0, 128), 0.2, 1e-6); CHECK_NEAR(recorded.getSample(0, 249), 0.321, 1e-6);
    model::EditHistory history; std::vector<model::EditCommand> commands;
    for (auto& [track, passes] : result.passes) commands.push_back(model::recordArrangementPasses(song, track, std::move(passes)));
    history.perform(song, model::compose("Record Loop", std::move(commands)));
    CHECK(song.tracks[0].takeLanes.size() == 3); CHECK(song.tracks[1].takeLanes.size() == 3);
    REQUIRE(song.tracks[1].arrangementClips.size() == 2);
    CHECK_NEAR(song.tracks[1].arrangementClips[0].currentEnd, 0.5, 1e-9); CHECK_NEAR(song.tracks[1].arrangementClips[0].loop.loopStart, 2, 1e-9);
    CHECK_NEAR(song.tracks[1].arrangementClips[1].currentStart, 0.5, 1e-9); CHECK_NEAR(song.tracks[1].arrangementClips[1].loop.startRelative, 0.5, 1e-9);
    CHECK(song.tracks[1].arrangementClips[0].takeId == song.tracks[1].takeLanes[2].clips[0].takeId);
    const int id = song.tracks[1].arrangementClips[0].uid;
    const auto saved = folder.getChildFile("loop.als"); REQUIRE(io::saveAls(song, saved).ok);
    song.sourcePath = saved.getFullPathName().toStdString(); io::refreshClipIdentities(song);
    auto again = io::loadAls(saved); REQUIRE(again); CHECK(again.song->tracks[1].takeLanes.size() == 3);
    CHECK(again.song->tracks[1].takeLanes[2].clips[0].takeId == song.tracks[1].takeLanes[2].clips[0].takeId);
    CHECK(again.song->tracks[1].recorderTakeCounter == 1);
    REQUIRE(history.undo(song)); CHECK(song.tracks[1].takeLanes.empty()); CHECK(song.tracks[1].arrangementClips.empty());
    REQUIRE(io::saveAls(song, saved).ok); again = io::loadAls(saved); REQUIRE(again); CHECK(again.song->tracks[1].takeLanes.empty());
    REQUIRE(history.redo(song)); CHECK(song.tracks[1].arrangementClips[0].uid == id); REQUIRE(io::saveAls(song, saved).ok);
    reader.reset(); folder.deleteRecursively();
}

TEST_CASE("loop recording: engine handles multiple and exact-boundary wraps with every input sample")
{
    juce::ScopedJuceInitialiser_GUI init;
    auto loaded = io::loadAls(juce::File(SONORA_TEST_FIXTURES).getChildFile("recording_set.xml")); REQUIRE(loaded);
    auto& song = *loaded.song; song.loopOn = true; song.loopStart = 0; song.loopLength = 64.0 / 22050;
    song.tracks[1].arrangementClips.clear(); song.tracks[1].monitoring = 2;
    model::setInputSelection(song.tracks[1], {model::InputKind::AudioMono, 0, {}});
    plugins::PluginHost host; midi::MidiManager midi; engine::AudioEngine eng(host, midi); eng.loadSong(song);
    (*eng.controls())[1].armed.store(true);
    const auto folder = juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("sonora-loop-engine", "", false);
    std::string error; REQUIRE(eng.recorder().start(song, *eng.controls(), folder, eng.sampleRate(), error));
    juce::AudioBuffer<float> input(1, 128), output(2, 128);
    for (int i = 0; i < 128; ++i) input.setSample(0, i, 0.125f);
    eng.transport().play();
    for (int count : {128, 128, 32})
    {
        eng.renderBlock(output.getArrayOfWritePointers(), 2, count, input.getArrayOfReadPointers(), 1);
        CHECK(eng.recorder().active());
    }
    CHECK_NEAR(eng.transport().positionBeats(), 32.0 / 22050, 1e-9);
    eng.transport().stop(); eng.renderBlock(output.getArrayOfWritePointers(), 2, 128);
    REQUIRE(awaitRecording(eng.recorder())); auto result = eng.recorder().takeResult(); REQUIRE(result.passes.size() == 1);
    CHECK(result.passes[0].second.size() == 5); CHECK(result.clips[0].second.lengthSamples == 288);
    CHECK_NEAR(result.passes[0].second[4].currentEnd, 32.0 / 22050, 1e-9);
    folder.deleteRecursively();
}

TEST_CASE("loop recording: tempo changes retain source mapping, pass capacity fails cleanly and seek ends capture")
{
    auto loaded = io::loadAls(juce::File(SONORA_TEST_FIXTURES).getChildFile("recording_set.xml")); REQUIRE(loaded);
    auto& song = *loaded.song; song.loopOn = true;
    engine::ControlBank controls(3); controls[0].armed.store(true); controls[1].armed.store(true);
    engine::Recorder recorder; std::string error;
    const auto folder = juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("sonora-loop-map", "", false);
    REQUIRE(recorder.start(song, controls, folder, 1000, error));
    juce::AudioBuffer<float> input(2, 100); input.clear(); juce::MidiBuffer midi;
    recorder.capture(input.getArrayOfReadPointers(), 2, midi, 0, 100, 100, true, false, true);
    recorder.capture(input.getArrayOfReadPointers(), 2, midi, 0, 200, 100, true, false, false);
    recorder.capture(nullptr, 0, midi, 4, 200, 0, true, true, false);
    REQUIRE(awaitRecording(recorder)); auto result = recorder.takeResult(); CHECK(! result.warning.empty());
    REQUIRE(result.passes[1].second.size() == 2); const auto& map = result.passes[1].second[0].warpMarkers;
    REQUIRE(map.size() == 3); CHECK_NEAR(map[1].seconds, 0.1, 1e-9); CHECK_NEAR(map[1].beats, 1, 1e-9);
    CHECK_NEAR(map[2].seconds, 0.2, 1e-9); CHECK_NEAR(map[2].beats, 1.5, 1e-9);
    controls[1].armed.store(false); REQUIRE(recorder.start(song, controls, folder, 1000, error));
    for (int i = 0; i < 4097; ++i) recorder.capture(nullptr, 0, midi, 0, 100, 1, true, false, true);
    REQUIRE(awaitRecording(recorder)); result = recorder.takeResult(); CHECK(! result.warning.empty());
    REQUIRE(result.passes.size() == 1); CHECK(result.passes[0].second.size() == 4096);
    folder.deleteRecursively();
}

TEST_CASE("transport loops: fractional-sample periods retain clock phase across many wraps")
{
    juce::ScopedJuceInitialiser_GUI init;
    plugins::PluginHost host; midi::MidiManager midi; engine::AudioEngine engine(host, midi);
    model::Song song; song.loopOn = true; song.loopStart = 0; song.loopLength = 53.5 / 22050;
    engine.loadSong(song); engine.transport().play();
    juce::AudioBuffer<float> out(2, 128);
    for (int i = 0; i < 8; ++i) engine.renderBlock(out.getArrayOfWritePointers(), 2, 128);
    CHECK_NEAR(engine.transport().positionBeats(), std::fmod(1024.0, 53.5) / 22050, 1e-9);
    engine.transport().setLoop(true, 0, 0.25 / 22050); engine.transport().setPosition(0);
    engine.renderBlock(out.getArrayOfWritePointers(), 2, 128);
    CHECK_NEAR(engine.transport().positionBeats(), 0, 1e-9); // physical one-sample floor must not run past the loop region
}

TEST_CASE("recording: a single Arrangement pass retains a take lane, Session capture remains flat")
{
    auto loaded = io::loadAls(juce::File(SONORA_TEST_FIXTURES).getChildFile("recording_set.xml")); REQUIRE(loaded);
    engine::ControlBank controls(3); controls[0].armed.store(true);
    engine::Recorder recorder; std::string error; juce::MidiBuffer midi;
    const auto folder = juce::File::getSpecialLocation(juce::File::tempDirectory);
    for (bool arrangement : {true, false})
    {
        REQUIRE(recorder.start(*loaded.song, controls, folder, 1000, error, arrangement));
        recorder.capture(nullptr, 0, midi, 8, 100, 50, true, false, false);
        recorder.requestStop(); recorder.capture(nullptr, 0, midi, 8.5, 100, 0, true, false, false);
        REQUIRE(awaitRecording(recorder)); auto result = recorder.takeResult(); REQUIRE(result.clips.size() == 1);
        CHECK(result.passes.size() == (arrangement ? 1u : 0u));
        if (arrangement) { REQUIRE(result.passes[0].second.size() == 1); CHECK_NEAR(result.passes[0].second[0].currentStart, 8, 1e-9); }
    }
}

TEST_CASE("loop recording: punch waits through pre-roll and repeats the punch region until stop")
{
    auto loaded = io::loadAls(juce::File(SONORA_TEST_FIXTURES).getChildFile("recording_set.xml")); REQUIRE(loaded);
    auto& song = *loaded.song; song.loopOn = song.punchIn = song.punchOut = true; song.loopStart = 1; song.loopLength = 1;
    engine::ControlBank controls(3); controls[0].armed.store(true);
    engine::Recorder recorder; std::string error; juce::MidiBuffer midi;
    REQUIRE(recorder.start(song, controls, juce::File::getSpecialLocation(juce::File::tempDirectory), 1000, error));
    midi.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8) 90), 0);
    recorder.capture(nullptr, 0, midi, 0, 100, 50, false, false, false); // count-in
    midi.clear(); recorder.capture(nullptr, 0, midi, 0, 100, 100, true, false, false); // pre-roll
    CHECK(recorder.active());
    recorder.capture(nullptr, 0, midi, 1, 100, 100, true, false, true); CHECK(recorder.active());
    recorder.capture(nullptr, 0, midi, 1, 100, 50, true, false, false);
    recorder.requestStop(); recorder.capture(nullptr, 0, midi, 1.5, 100, 0, true, false, false);
    REQUIRE(awaitRecording(recorder)); auto result = recorder.takeResult(); CHECK(result.warning.empty());
    REQUIRE(result.passes.size() == 1); REQUIRE(result.passes[0].second.size() == 2);
    const auto& passes = result.passes[0].second;
    CHECK_NEAR(passes[0].currentStart, 1, 1e-9); CHECK_NEAR(passes[1].currentStart, 1, 1e-9);
    CHECK_NEAR(passes[1].currentEnd, 1.5, 1e-9); CHECK_NEAR(passes[1].loop.loopStart, 1, 1e-9);
    REQUIRE(passes[0].notes.size() == 1); REQUIRE(passes[1].notes.size() == 1);
    CHECK_NEAR(passes[0].notes[0].duration, 1, 1e-9); CHECK_NEAR(passes[1].notes[0].duration, 0.5, 1e-9);
}

TEST_CASE("take audition: prepared lane swaps preserve main voices and coalesce pending main edits")
{
    LaunchRig rig;
    model::TakeLane lane; lane.name = "Alternative"; lane.clips = {midiClip(65, 8, false, {0, 1})};
    lane.clips[0].notes[0].duration = 4;
    model::appendTakeLanes(rig.song, 0, {lane}).apply(rig.song);
    const int id = rig.song.tracks[0].takeLanes[0].id;
    rig.eng.updateTrackClips(0, rig.song);
    rig.eng.transport().play(); rig.run(512);
    auto* graph = rig.eng.latestGraph(); auto& node = graph->nodes[0];
    REQUIRE(! node.clips.empty()); const auto* original = node.clips.data();
    rig.rec[0]->events.clear();
    model::renameTakeLane(rig.song, 0, id, "New name").apply(rig.song);
    rig.eng.updateTrackClips(0, rig.song, true); rig.run(512);
    CHECK(node.clips.data() == original); CHECK(rig.rec[0]->events.empty()); // no all-notes-off on an inaudible lane edit
    model::auditionTakeLane(rig.song, 0, id).apply(rig.song);
    rig.eng.updateTrackClips(0, rig.song, true); rig.eng.transport().setPosition(0); rig.run(512);
    CHECK((*graph->controls)[0].auditionLane.load() == id);
    CHECK(! rig.rec[0]->noteOns(65).empty());
    rig.rec[0]->events.clear();
    model::auditionTakeLane(rig.song, 0, -1).apply(rig.song);
    rig.eng.updateTrackClips(0, rig.song, true); rig.eng.transport().setPosition(0); rig.run(512);
    CHECK((*graph->controls)[0].auditionLane.load() == -1); CHECK(! rig.rec[0]->noteOns(72).empty());
    // Publish a main edit, then a lane edit before the callback consumes either.
    rig.song.tracks[0].arrangementClips[0].notes[0].pitch = 74;
    rig.eng.updateTrackClips(0, rig.song);
    rig.eng.updateTrackClips(0, rig.song, true);
    rig.rec[0]->events.clear(); rig.eng.transport().setPosition(0); rig.run(512);
    CHECK(! rig.rec[0]->noteOns(74).empty());
    // Audition temporarily overrides a Session clip, preserving its launcher state.
    (*graph->controls)[0].arrangementOn.store(false); (*graph->controls)[0].playingSlot.store(0);
    model::auditionTakeLane(rig.song, 0, id).apply(rig.song);
    rig.eng.updateTrackClips(0, rig.song, true); rig.rec[0]->events.clear(); rig.eng.transport().setPosition(0); rig.run(512);
    CHECK(! rig.rec[0]->noteOns(65).empty()); CHECK(rig.rec[0]->noteOns(60).empty());
    CHECK((*graph->controls)[0].playingSlot.load() == 0);
}

TEST_CASE("take audition: envelope overrides restore on lane exit and stopped swaps publish atomically")
{
    juce::ScopedJuceInitialiser_GUI init; plugins::PluginHost host; midi::MidiManager midi;
    engine::AudioEngine eng(host, midi); model::Song song; song.tracks.resize(1);
    auto& track = song.tracks[0]; track.kind = model::TrackKind::Midi; track.mixer.volumeTargetId = 501;
    auto clip = midiClip(60, 4, false, {0});
    model::AutomationEnvelope envelope; envelope.targetId = 501; envelope.points = {{0, 0.25}, {4, 0.25}};
    clip.envelopes = {envelope}; model::TakeLane lane; lane.id = 9; lane.audition = true; lane.clips = {clip};
    track.takeLanes = {lane}; eng.loadSong(song); eng.transport().play();
    juce::AudioBuffer<float> out(2, 128); eng.renderBlock(out.getArrayOfWritePointers(), 2, 128);
    CHECK_NEAR((*eng.controls())[0].volume.load(), 0.25, 1e-6);
    model::auditionTakeLane(song, 0, -1).apply(song); eng.updateTrackClips(0, song, true);
    eng.renderBlock(out.getArrayOfWritePointers(), 2, 128); CHECK_NEAR((*eng.controls())[0].volume.load(), 1, 1e-6);
    // An active Session override must restore before the take applies its own override.
    auto session = clip; session.envelopes[0].points = {{0, 0.5}, {4, 0.5}};
    track.slots.resize(1); track.slots[0].clip = session; eng.updateTrackClips(0, song);
    auto& controls = (*eng.controls())[0]; controls.arrangementOn.store(false); controls.playingSlot.store(0);
    eng.renderBlock(out.getArrayOfWritePointers(), 2, 128); CHECK_NEAR(controls.volume.load(), 0.5, 1e-6);
    model::auditionTakeLane(song, 0, 9).apply(song); eng.updateTrackClips(0, song, true);
    eng.renderBlock(out.getArrayOfWritePointers(), 2, 128); CHECK_NEAR(controls.volume.load(), 0.25, 1e-6);
    model::auditionTakeLane(song, 0, -1).apply(song); eng.updateTrackClips(0, song, true);
    eng.renderBlock(out.getArrayOfWritePointers(), 2, 128); CHECK_NEAR(controls.volume.load(), 0.5, 1e-6);
    eng.transport().stop(); eng.renderBlock(out.getArrayOfWritePointers(), 2, 128);
    model::auditionTakeLane(song, 0, 9).apply(song); eng.updateTrackClips(0, song, true);
    eng.renderBlock(out.getArrayOfWritePointers(), 2, 128); CHECK((*eng.controls())[0].auditionLane.load() == 9);
    CHECK_NEAR((*eng.controls())[0].volume.load(), 1, 1e-6); // stopped audition does not apply envelopes
}

TEST_CASE("take audition: audio uses the prepared lane source and returns to the main waveform")
{
    juce::ScopedJuceInitialiser_GUI init; plugins::PluginHost host; midi::MidiManager midi;
    engine::AudioEngine eng(host, midi); engine::SampleCache samples; devices::BuildContext context;
    model::Song song; song.tracks.resize(1); auto& track = song.tracks[0]; track.kind = model::TrackKind::Audio; track.monitoring = 2;
    model::Clip clip; clip.kind = model::Clip::Kind::Audio; clip.currentEnd = 4; clip.loop = {0, 2, 0, false};
    track.arrangementClips = {clip}; model::TakeLane lane; lane.id = 31; lane.clips = {clip}; track.takeLanes = {lane};
    auto built = engine::buildGraph(song, context, samples);
    auto data = [](float value) { auto sample = std::make_shared<engine::SampleData>(); sample->sampleRate = 44100;
        sample->audio.setSize(2, 4096); for (int ch = 0; ch < 2; ++ch) for (int i = 0; i < 4096; ++i) sample->audio.setSample(ch, i, value);
        return engine::SampleSlot::makeReady(std::move(sample)); };
    const auto main = data(0.2f), take = data(0.7f);
    built.graph->nodes[0].clips[0].sample = main; built.graph->nodes[0].takes[0].clips[0].sample = take;
    eng.installGraph(std::move(built.graph)); eng.transport().play(); juce::AudioBuffer<float> out(2, 128);
    eng.renderBlock(out.getArrayOfWritePointers(), 2, 128); CHECK_NEAR(out.getSample(0, 64), 0.2, 1e-6);
    auto publish = [&](bool audition) {
        track.takeLanes[0].audition = audition;
        auto* node = &eng.latestGraph()->nodes[0]; std::vector<std::string> log;
        auto set = engine::buildClipSet(track, *node, song, samples, log, true);
        set->takes[0].clips[0].sample = take;
        delete node->clipSwap->retired.exchange(nullptr);
        node->clipSwap->pending.store(set.release());
    };
    publish(true); eng.renderBlock(out.getArrayOfWritePointers(), 2, 128); CHECK_NEAR(out.getSample(0, 64), 0.7, 1e-6);
    CHECK((*eng.controls())[0].auditionLane.load() == 31);
    publish(false); eng.renderBlock(out.getArrayOfWritePointers(), 2, 128); CHECK_NEAR(out.getSample(1, 64), 0.2, 1e-6);
}


TEST_CASE("session clock: clips advance and hold notes across short Arrangement wraps")
{
    LaunchRig rig; rig.song.loopOn = true; rig.song.loopStart = 1; rig.song.loopLength = 1;
    rig.eng.transport().setLoop(true, 1, 1);
    rig.song.tracks[0].slots[0].clip->notes = {{0, 3, 60, 100}, {2, 0.25, 67, 100}};
    rig.song.tracks[0].slots[0].clip->loop.loopEnd = 8;
    rig.eng.updateTrackClips(0, rig.song);
    engine::launchClip(*rig.eng.latestGraph(), rig.eng.transport(), 0, 0, 4);
    rig.run(4 * 22050);
    CHECK(rig.rec[0]->noteOns(60).size() == 1); CHECK(rig.rec[0]->noteOns(67).size() == 1);
    for (const auto& [sample, pitch] : rig.rec[0]->events)
        if (pitch == -1 && sample > 0 && sample <= 2 * 22050) CHECK(false); // no global-loop all-notes-off
    CHECK((*rig.eng.controls())[0].sessionElapsed.load() >= 4);
    // A bar-quantised launch must not starve in a one-beat Arrangement loop.
    engine::launchClip(*rig.eng.latestGraph(), rig.eng.transport(), 0, 1, 4);
    rig.run(22050); CHECK((*rig.eng.controls())[0].playingSlot.load() == 1);
}

TEST_CASE("session recording: quantised start stop held input and cancellation use exact sample ranges")
{
    auto loaded = io::loadAls(juce::File(SONORA_TEST_FIXTURES).getChildFile("recording_set.xml")); REQUIRE(loaded);
    auto& song = *loaded.song; engine::ControlBank controls(3); controls[0].armed.store(true);
    engine::Recorder recorder; engine::Recorder::SessionPlan plan; plan.targets = {{0, 2, -1}}; plan.quant = 2;
    std::string error; juce::TemporaryFile folder; REQUIRE(recorder.start(song, controls, folder.getFile(), 1000, error, false, &plan));
    juce::MidiBuffer midi; midi.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8) 90), 20);
    auto range = recorder.prepareCaptureRange({0, 100, 1.5, 100}, true, false);
    CHECK(range.offset == 50); CHECK(range.count == 50); CHECK(recorder.sessionPhase() == 2);
    recorder.capture(nullptr, 0, midi, 1.5, 100, 100, true, false, false, nullptr, 0, &range);
    recorder.requestSessionStop(); midi.clear(); midi.addEvent(juce::MidiMessage::noteOff(1, 60), 30);
    range = recorder.prepareCaptureRange({0, 200, 2.5, 100}, true, false);
    CHECK(range.count == 150); CHECK(range.punchEnd);
    recorder.capture(nullptr, 0, midi, 2.5, 100, 200, true, false, false, nullptr, 0, &range);
    for (int i = 0; i < 1000 && ! recorder.finished(); ++i) juce::Thread::sleep(1);
    REQUIRE(recorder.finished()); auto result = recorder.takeResult(); REQUIRE(result.session.size() == 1);
    CHECK(result.clips.empty()); CHECK(result.session[0].scene == 2); CHECK(result.session[0].clip.loop.enabled);
    CHECK_NEAR(result.session[0].clip.loop.loopEnd, 2, 1e-9);
    REQUIRE(result.session[0].clip.notes.size() == 1); CHECK_NEAR(result.session[0].clip.notes[0].start, 0, 1e-9);
    CHECK_NEAR(result.session[0].clip.notes[0].duration, 0.8, 1e-9);
    REQUIRE(recorder.start(song, controls, folder.getFile(), 1000, error, false, &plan));
    recorder.requestSessionStop(); range = recorder.prepareCaptureRange({0, 100, 0.5, 100}, true, false);
    recorder.capture(nullptr, 0, midi, 0.5, 100, 100, true, false, false, nullptr, 0, &range);
    for (int i = 0; i < 1000 && ! recorder.finished(); ++i) juce::Thread::sleep(1);
    REQUIRE(recorder.finished()); result = recorder.takeResult(); CHECK(result.session.empty());
}

TEST_CASE("session overdub: content phase loop folding and exact clip identity survive capture")
{
    auto loaded = io::loadAls(juce::File(SONORA_TEST_FIXTURES).getChildFile("recording_set.xml")); REQUIRE(loaded);
    auto& song = *loaded.song; auto clip = song.tracks[0].arrangementClips[0];
    clip.notes = {{0.5, 0.3, 60, 70}, {2, 0.5, 65, 80}}; clip.loop = {0, 4, 0, true};
    model::setSessionClip(song, 0, 1, clip).apply(song); const int uid = song.tracks[0].slots[1].clip->uid;
    engine::ControlBank controls(3); controls[0].armed.store(true); engine::Recorder recorder;
    engine::Recorder::SessionPlan plan; plan.targets = {{0, 1, uid}};
    std::string error; juce::TemporaryFile folder; REQUIRE(recorder.start(song, controls, folder.getFile(), 1000, error, false, &plan));
    recorder.beginSessionContent(0, 3.5);
    juce::MidiBuffer midi; midi.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8) 100), 100); midi.addEvent(juce::MidiMessage::noteOff(1, 60), 150);
    auto range = recorder.prepareCaptureRange({0, 200, 1.5, 100}, true, false);
    recorder.capture(nullptr, 0, midi, 1.5, 100, 200, true, false, true, nullptr, 0, &range); CHECK(recorder.active());
    recorder.requestSessionStop(); range = recorder.prepareCaptureRange({0, 10, 1, 100}, true, false);
    recorder.capture(nullptr, 0, midi, 1, 100, 10, true, false, false, nullptr, 0, &range);
    for (int i = 0; i < 1000 && ! recorder.finished(); ++i) juce::Thread::sleep(1);
    REQUIRE(recorder.finished()); auto result = recorder.takeResult(); REQUIRE(result.session.size() == 1);
    CHECK(result.session[0].uid == uid); CHECK(result.session[0].clip.uid == uid);
    const auto& notes = result.session[0].clip.notes; REQUIRE(notes.size() == 2);
    CHECK_NEAR(notes[0].start, 0.5, 1e-9); CHECK_NEAR(notes[0].duration, 0.5, 1e-9); CHECK_NEAR(notes[0].velocity, 100, 1e-6);
    CHECK(notes[1].pitch == 65); CHECK_NEAR(result.session[0].clip.loop.loopEnd, 4, 1e-9);
}

TEST_CASE("session overdub: actual callback plays existing notes monitors input and saves one undo")
{
    LaunchRig rig; auto loaded = io::loadAls(juce::File(SONORA_TEST_FIXTURES).getChildFile("recording_set.xml")); REQUIRE(loaded);
    rig.song = *loaded.song; const auto originalNotes = rig.song.tracks[0].arrangementClips[0].notes.size();
    auto clip = rig.song.tracks[0].arrangementClips[0];
    clip.notes = {{0, 1.5, 60, 80}}; clip.loop = {0, 4, 0, true};
    model::setSessionClip(rig.song, 0, 1, clip).apply(rig.song);
    const int uid = rig.song.tracks[0].slots[1].clip->uid;
    rig.song.tracks[0].monitoring = 1; rig.eng.loadSong(rig.song);
    auto device = std::make_unique<NoteRecorder>(); auto* events = device.get();
    rig.eng.latestGraph()->nodes[0].devices.push_back(std::move(device));
    (*rig.eng.controls())[0].armed.store(true);
    engine::Recorder::SessionPlan plan; plan.targets = {{0, 1, uid}};
    juce::TemporaryFile folder; std::string error;
    REQUIRE(rig.eng.recorder().start(rig.song, *rig.eng.controls(), folder.getFile(), 44100, error, false, &plan));
    engine::launchClip(*rig.eng.latestGraph(), rig.eng.transport(), 0, 1, 0);
    rig.midi.handleMessage(juce::MidiMessage::noteOn(1, 67, (juce::uint8) 100), true, false);
    rig.run(512);
    CHECK(! events->noteOns(60).empty()); CHECK(! events->noteOns(67).empty());
    events->events.clear(); rig.run(512);
    for (const auto& event : events->events) CHECK(event.second != -1); // existing and input voices survive Auto monitoring
    rig.midi.handleMessage(juce::MidiMessage::noteOff(1, 67), true, false); rig.run(512);
    rig.eng.recorder().requestSessionStop(); rig.run(512);
    for (int i = 0; i < 1000 && ! rig.eng.recorder().finished(); ++i) juce::Thread::sleep(1);
    REQUIRE(rig.eng.recorder().finished()); auto result = rig.eng.recorder().takeResult(); REQUIRE(result.session.size() == 1);
    REQUIRE(result.session[0].clip.notes.size() == 2);
    model::EditHistory history;
    history.perform(rig.song, model::modifyClip(rig.song, 0, uid, result.session[0].clip, "Record Session Take", 1));
    auto verify = [&](size_t notes) {
        juce::TemporaryFile saved(".als"); REQUIRE(io::saveAls(rig.song, saved.getFile()).ok);
        auto again = io::loadAls(saved.getFile()); REQUIRE(again); const auto& target = *again.song->tracks[0].slots[1].clip;
        CHECK(target.notes.size() == notes); CHECK_NEAR(target.loop.loopEnd, 4, 1e-9);
        CHECK(again.song->tracks[0].arrangementClips[0].notes.size() == originalNotes);
    };
    verify(2); REQUIRE(history.undo(rig.song)); CHECK(rig.song.tracks[0].slots[1].clip->uid == uid); verify(1);
    REQUIRE(history.redo(rig.song)); verify(2);
}

TEST_CASE("Session immediate start retains quantised stop under Arrangement looping")
{
    auto loaded = io::loadAls(juce::File(SONORA_TEST_FIXTURES).getChildFile("recording_set.xml")); REQUIRE(loaded);
    engine::ControlBank controls(3); controls[0].armed.store(true); engine::Recorder recorder;
    engine::Recorder::SessionPlan plan; plan.targets = {{0, 2, -1}};
    plan.quant = 4; plan.immediateStart = true; plan.looping = true; plan.loopStart = 1; plan.loopEnd = 2;
    juce::TemporaryFile folder; std::string error;
    REQUIRE(recorder.start(*loaded.song, controls, folder.getFile(), 1000, error, false, &plan));
    auto range = recorder.prepareCaptureRange({0, 100, 0.5, 100}, true, false);
    CHECK(range.offset == 0); CHECK(range.count == 100);
    juce::MidiBuffer midi; recorder.capture(nullptr, 0, midi, 0.5, 100, 100, true, false, false, nullptr, 0, &range);
    recorder.requestSessionStop(); range = recorder.prepareCaptureRange({0, 100, 1.5, 100}, true, false);
    CHECK(range.count == 50); CHECK(range.punchEnd);
    recorder.capture(nullptr, 0, midi, 1.5, 100, 100, true, false, true, nullptr, 0, &range);
    for (int i = 0; i < 1000 && ! recorder.finished(); ++i) juce::Thread::sleep(1);
    REQUIRE(recorder.finished()); const auto result = recorder.takeResult(); REQUIRE(result.session.size() == 1);
    CHECK_NEAR(result.session[0].clip.loop.loopEnd, 1.5, 1e-9);
}

TEST_CASE("Session transport stop: queued and count-in takes cancel without material")
{
    LaunchRig rig; auto loaded = io::loadAls(juce::File(SONORA_TEST_FIXTURES).getChildFile("recording_set.xml")); REQUIRE(loaded);
    rig.song = *loaded.song; rig.eng.loadSong(rig.song); (*rig.eng.controls())[0].armed.store(true);
    engine::Recorder::SessionPlan plan; plan.targets = {{0, 2, -1}}; plan.quant = 4;
    juce::TemporaryFile folder; std::string error;
    auto finish = [&] {
        rig.eng.transport().stop(); rig.run(512);
        for (int i = 0; i < 1000 && ! rig.eng.recorder().finished(); ++i) juce::Thread::sleep(1);
        REQUIRE(rig.eng.recorder().finished()); const auto result = rig.eng.recorder().takeResult();
        CHECK(result.clips.empty()); CHECK(result.session.empty()); CHECK(rig.eng.recorder().sessionPhase() == 0);
    };
    REQUIRE(rig.eng.recorder().start(rig.song, *rig.eng.controls(), folder.getFile(), 44100, error, false, &plan));
    rig.eng.transport().setPosition(0.5); rig.eng.transport().play(); rig.run(512);
    CHECK(rig.eng.recorder().sessionPhase() == 1); finish();
    plan.immediateStart = true;
    REQUIRE(rig.eng.recorder().start(rig.song, *rig.eng.controls(), folder.getFile(), 44100, error, false, &plan));
    rig.eng.transport().playWithCountIn(1); rig.run(512); CHECK(rig.eng.transport().isCountingIn()); finish();
}

TEST_CASE("automation override: a control moved by hand while its lane plays is left alone until Re-Enable Automation")
{
    juce::ScopedJuceInitialiser_GUI juce;
    plugins::PluginHost host;
    midi::MidiManager midi;
    engine::AudioEngine engine(host, midi);
    auto r = io::loadAls(juce::File(SONORA_TEST_FIXTURES).getChildFile("minimal_set.xml"));
    REQUIRE(r);
    engine.loadSong(*r.song);
    auto controls = engine.controls();
    REQUIRE(controls != nullptr);
    auto* graph = engine.latestGraph();
    REQUIRE(graph != nullptr);
    std::vector<float> l(256), rr(256);
    float* out[2] = {l.data(), rr.data()};

    CHECK(! engine::automationOverridden(*graph));
    engine.transport().setPosition(6.0);
    engine.transport().play();
    engine.renderBlock(out, 2, 256);
    CHECK_NEAR((*controls)[0].volume.load(), 0.75, 1e-3); // the lane drives the volume (1.0 at beat 4, 0.5 at 8)
    engine.renderBlock(out, 2, 256);
    CHECK(! engine::automationOverridden(*graph));

    // The user grabs the fader while it plays.
    (*controls)[0].volume.store(0.3f);
    engine.renderBlock(out, 2, 256);
    engine.renderBlock(out, 2, 256);
    CHECK_NEAR((*controls)[0].volume.load(), 0.3, 1e-6); // not overwritten any more
    CHECK(engine::automationOverridden(*graph));
    // other automated controls keep following their lanes
    (*controls)[0].pan.store(0.0f);

    // Re-Enable Automation: the lane takes the control back and the button goes dark.
    engine::reEnableAutomation(*graph);
    CHECK(! engine::automationOverridden(*graph));
    engine.renderBlock(out, 2, 256);
    const double expected = (*controls)[0].volume.load();
    CHECK(expected > 0.45);
    CHECK(expected < 0.8); // back on the 1.0 -> 0.5 slope
    engine.renderBlock(out, 2, 256);
    CHECK(! engine::automationOverridden(*graph));

    // A hand edit while stopped is not an override: the next start lets the lane win again.
    engine.transport().stop();
    engine.renderBlock(out, 2, 256);
    (*controls)[0].volume.store(0.1f);
    engine.renderBlock(out, 2, 256);
    CHECK_NEAR((*controls)[0].volume.load(), 0.1, 1e-6);
    engine.transport().setPosition(6.0);
    engine.transport().play();
    engine.renderBlock(out, 2, 256);
    CHECK_NEAR((*controls)[0].volume.load(), 0.75, 1e-3);
    CHECK(! engine::automationOverridden(*graph));
}

#include "engine/AutomationCapture.h"

TEST_CASE("automation capture: hand moves while recording become breakpoints; lanes that merely play are not recorded")
{
    juce::ScopedJuceInitialiser_GUI juce;
    plugins::PluginHost host;
    midi::MidiManager midi;
    engine::AudioEngine engine(host, midi);
    auto r = io::loadAls(juce::File(SONORA_TEST_FIXTURES).getChildFile("minimal_set.xml"));
    REQUIRE(r);
    model::Song& song = *r.song;
    engine.loadSong(song);
    auto controls = engine.controls();
    auto* graph = engine.latestGraph();
    REQUIRE(controls != nullptr);
    REQUIRE(graph != nullptr);
    std::vector<float> l(256), rr(256);
    float* out[2] = {l.data(), rr.data()};

    engine::AutomationCapture capture;
    capture.begin();
    engine.transport().setPosition(6.0);
    engine.transport().play();
    engine.renderBlock(out, 2, 256);
    capture.poll(song, *graph, 6.0);
    engine.renderBlock(out, 2, 256);
    capture.poll(song, *graph, 6.05);
    CHECK(! capture.touched()); // the volume lane moves by itself; that is not recorded

    // Pan has no lane: dragging it records. The volume fader is grabbed while its lane plays.
    (*controls)[0].pan.store(0.0f);
    capture.poll(song, *graph, 6.1);
    (*controls)[0].pan.store(0.5f);
    capture.poll(song, *graph, 6.2);
    (*controls)[0].volume.store(0.3f);
    engine.renderBlock(out, 2, 256); // the audio thread notices the hand move (override)
    capture.poll(song, *graph, 6.3);
    (*controls)[0].volume.store(0.2f);
    capture.poll(song, *graph, 6.4);
    CHECK(capture.touched());

    const auto touched = capture.finish(song, *graph, 7.0);
    CHECK(! capture.touched());
    const model::AutomationEnvelope* pan = nullptr;
    const model::AutomationEnvelope* volume = nullptr;
    for (const auto& t : touched)
    {
        CHECK(t.track == 0);
        if (t.envelope.targetId == song.tracks[0].mixer.panTargetId) pan = &t.envelope;
        if (t.envelope.targetId == song.tracks[0].mixer.volumeTargetId) volume = &t.envelope;
    }
    REQUIRE(pan != nullptr);
    REQUIRE(volume != nullptr);
    // pan: pre-roll value, the value before the first move, the moves, and a hold until the end of the take
    CHECK(pan->eventType == "FloatEvent");
    REQUIRE(pan->points.size() >= 4);
    CHECK(pan->points.front().time < 0.0);
    CHECK_NEAR(pan->points.back().time, 7.0, 1e-9);
    CHECK_NEAR(pan->points.back().value, 0.5, 1e-6);
    bool sawZero = false;
    for (const auto& p : pan->points) if (p.time >= 6.0 && std::fabs(p.value) < 1e-6) sawZero = true;
    CHECK(sawZero);
    // volume: the old lane before beat 6.3 stays, the touched stretch is replaced by the hand moves and holds 0.2
    bool holdsUntilEnd = false;
    for (const auto& p : volume->points) if (std::fabs(p.time - 7.0) < 1e-9 && std::fabs(p.value - 0.2) < 1e-6) holdsUntilEnd = true;
    CHECK(holdsUntilEnd);
    CHECK_NEAR(volume->points.back().value, 0.5, 1e-6); // the old lane resumes after the take (its point at beat 8)
    for (const auto& p : volume->points)
        if (p.time > 6.3 && p.time < 7.0)
            CHECK(std::fabs(p.value - 0.3) < 1e-6 || std::fabs(p.value - 0.2) < 1e-6);
    bool keptOld = false;
    for (const auto& p : volume->points) if (std::fabs(p.time - 4.0) < 1e-9) keptOld = true;
    CHECK(keptOld);
}

TEST_CASE("audio clip fades: fade in and out ramp the gain over the chosen beats, the automatic edge fade stays for clips without one")
{
    auto data = std::make_shared<engine::SampleData>();
    data->sampleRate = 1000.0;
    data->audio.setSize(2, 20000);
    for (int i = 0; i < 20000; ++i)
        for (int ch = 0; ch < 2; ++ch) data->audio.setSample(ch, i, 1.0f); // DC: the output is the gain
    model::Clip c;
    c.kind = model::Clip::Kind::Audio;
    c.currentStart = 0.0;
    c.currentEnd = 8.0;
    c.loop = {0.0, 8.0, 0.0, false}; // seconds, unwarped
    c.warped = false;
    c.fadeIn = 2.0;
    c.fadeOut = 4.0;
    c.fadeInDefault = c.fadeOutDefault = false;
    // 60 BPM: one beat is one second, 1000 samples per beat
    auto render = [&](const model::Clip& clip) {
        auto prepared = engine::prepareClip(clip, data);
        juce::AudioBuffer<float> out(2, 8000);
        out.clear();
        engine::renderAudioClip(prepared, {0, 8000, 0.0, 1000.0}, 60.0, out);
        return out;
    };
    const auto faded = render(c);
    CHECK_NEAR(faded.getSample(0, 0), 0.0, 0.01);
    CHECK_NEAR(faded.getSample(0, 1000), 0.75, 0.01);   // curved fade, halfway in time
    CHECK_NEAR(faded.getSample(0, 2000), 1.0, 0.01);    // fade-in complete
    CHECK_NEAR(faded.getSample(0, 3500), 1.0, 0.01);    // untouched middle
    CHECK_NEAR(faded.getSample(0, 6000), 0.75, 0.01);   // matching curved fade-out
    CHECK_NEAR(faded.getSample(0, 7990), 0.0, 0.01);

    model::Clip plain = c;
    plain.fadeIn = plain.fadeOut = 0.0;
    plain.fadeInDefault = plain.fadeOutDefault = true;
    const auto flat = render(plain);
    CHECK_NEAR(flat.getSample(0, 1000), 1.0, 1e-3);      // no clip fade: full level right after the short automatic edge fade
    CHECK_NEAR(flat.getSample(0, 6000), 1.0, 1e-3);
    // The stateful stretch paths must use the same curve as unwarped/Re-Pitch playback.
    for (const auto mode : {model::WarpMode::Beats, model::WarpMode::Tones})
    {
        auto warped = c; warped.warped = true; warped.warpMode = mode; warped.warpMarkers = {{0, 0}, {8, 8}};
        auto warpedFlat = warped; warpedFlat.fadeIn = warpedFlat.fadeOut = 0;
        auto stretched = [&](const model::Clip& clip) {
            auto prepared = engine::prepareClip(clip, data);
            engine::AudioPlayState state; state.prepare(1000, 8000);
            juce::AudioBuffer<float> out(2, 8000); out.clear();
            state.render(prepared, {0, 8000, 0, 1000}, 60, out);
            return out;
        };
        const auto curved = stretched(warped), reference = stretched(warpedFlat);
        for (const int sample : {1000, 6000})
        {
            CHECK(reference.getSample(0, sample) > 0.1f);
            CHECK_NEAR(curved.getSample(0, sample) / reference.getSample(0, sample), 0.75, 1e-3);
        }
    }
}

namespace
{
class ConstantSource final : public devices::DeviceProcessor
{
public:
    std::string name() const override { return "constant"; }
    void prepare(double, int) override {}
    void process(juce::AudioBuffer<float>& audio, juce::MidiBuffer&) override
    {
        for (int ch = 0; ch < audio.getNumChannels(); ++ch)
            for (int i = 0; i < audio.getNumSamples(); ++i) audio.setSample(ch, i, 0.5f);
    }
};
} // namespace

TEST_CASE("channel meters read the signal after the effects and the fader")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    plugins::PluginHost host;
    midi::MidiManager midi;
    engine::AudioEngine eng(host, midi);
    model::Song song;
    model::Track t;
    t.id = 1; t.kind = model::TrackKind::Midi; t.name = "t";
    song.tracks.push_back(t);
    engine::SampleCache samples;
    devices::BuildContext ctx;
    auto built = engine::buildGraph(song, ctx, samples);
    auto& node = built.graph->nodes[0];
    node.devices.push_back(std::make_unique<ConstantSource>());
    auto util = std::make_unique<devices::Utility>();
    auto* utility = util.get();
    node.devices.push_back(std::move(util));
    eng.installGraph(std::move(built.graph));
    auto controls = eng.controls();
    std::vector<float> l(512), r(512);
    float* out[2] = {l.data(), r.data()};

    for (int i = 0; i < 4; ++i) eng.renderBlock(out, 2, 512); // the first block ramps the gains up from silence
    const float open = (*controls)[0].peakL.load();
    CHECK(open > 0.4f); // 0.5 through a unity chain

    utility->setParameter(devices::Utility::Mute, 1.0f); // a device after the source silences the track
    for (int i = 0; i < 6; ++i) eng.renderBlock(out, 2, 512);
    std::printf("muted peak %f\n", (*controls)[0].peakL.load());
    CHECK((*controls)[0].peakL.load() < 0.01f); // the meter follows the chain output, not the source

    utility->setParameter(devices::Utility::Mute, 0.0f);
    (*controls)[0].volume.store(0.25f);
    for (int i = 0; i < 4; ++i) eng.renderBlock(out, 2, 512);
    CHECK((*controls)[0].peakL.load() < open * 0.4f); // post-fader as well
}

TEST_CASE("audio effect reorder preserves processor instances live values controls and playback across undo redo")
{
    juce::ScopedJuceInitialiser_GUI gui;
    model::Song song; model::Track track; track.kind = model::TrackKind::Midi;
    for (const auto* type : {"OriginalSimpler", "StereoGain", "Delay"})
    {
        model::Device device; device.type = type; device.displayName = type; device.id = (int) track.devices.size();
        track.devices.push_back(device);
    }
    song.tracks.push_back(track);
    plugins::PluginHost host; midi::MidiManager midi; engine::AudioEngine engine(host, midi);
    engine.loadSong(song);
    auto& node = engine.latestGraph()->nodes[0];
    auto* instrument = node.processorForModelDevice[0];
    auto* utility = node.processorForModelDevice[1];
    auto* delay = node.processorForModelDevice[2];
    REQUIRE(instrument && utility && delay);
    utility->setParameter(devices::Utility::Gain, 0.42f);
    const auto controls = engine.controls(); (*controls)[0].armed.store(true);
    engine.transport().advanceTo(12);
    auto originalOrder = midi::deviceOrder(song);
    midi::Mapping mapping; mapping.target.kind = midi::ControlTarget::Kind::DeviceParameter;
    mapping.target.track = 0; mapping.target.device = 1; mapping.target.param = "Gain";
    std::vector<midi::Mapping> mappings{mapping};
    model::EditHistory history;
    history.perform(song, model::moveAudioEffect(song, 0, 2, 1)); engine.updateTrackDevices(0, song);
    auto movedOrder = midi::deviceOrder(song); midi::rebindDeviceMappings(originalOrder, movedOrder, mappings);
    CHECK(mappings[0].target.device == 2);
    devices::captureDeviceState(song.tracks[0].devices[2], utility);
    CHECK_NEAR(song.tracks[0].devices[2].param("Gain", 0), 0.42, 1e-6);
    CHECK(node.processorForModelDevice[0] == instrument); CHECK(node.processorForModelDevice[1] == delay);
    CHECK(node.processorForModelDevice[2] == utility); CHECK(engine.controls() == controls);
    CHECK_NEAR(utility->getParameter(devices::Utility::Gain), 0.42, 1e-6);
    CHECK((*controls)[0].armed.load()); CHECK_NEAR(engine.transport().positionBeats(), 12, 1e-9);
    REQUIRE(history.undo(song)); engine.updateTrackDevices(0, song);
    midi::rebindDeviceMappings(movedOrder, midi::deviceOrder(song), mappings); CHECK(mappings[0].target.device == 1);
    CHECK(node.processorForModelDevice[1] == utility); CHECK(node.processorForModelDevice[2] == delay);
    REQUIRE(history.redo(song)); engine.updateTrackDevices(0, song);
    CHECK(node.processorForModelDevice[1] == delay); CHECK(node.processorForModelDevice[2] == utility);
    CHECK_NEAR(utility->getParameter(devices::Utility::Gain), 0.42, 1e-6);
    std::array<float, 128> left{}, right{}; float* channels[]{left.data(), right.data()};
    engine.renderBlock(channels, 2, 128); for (float sample : left) CHECK(std::isfinite(sample));
}

#include "io/als/AlsNewSet.h"

TEST_CASE("tempo: a new set plays at a typed tempo and saves it")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    const auto file = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("sonora_tempo_test.als");
    std::string error;
    REQUIRE(io::writeAlsDom(*io::createNewSetDom(), file, error)); // File > New Project
    auto loaded = io::loadAls(file);
    REQUIRE(loaded);
    auto& song = *loaded.song;
    REQUIRE(song.main.mixer.tempoTargetId >= 0);
    plugins::PluginHost host;
    midi::MidiManager midi;
    engine::AudioEngine eng(host, midi);
    eng.loadSong(song);

    model::EditHistory history; // what TopBar::commitTempo does
    eng.transport().setTempo(145.0);
    history.perform(song, model::setTempo(song, 145.0));
    std::vector<float> l(512), r(512);
    float* out[2] = {l.data(), r.data()};
    eng.transport().play();
    for (int b = 0; b < 20; ++b)
        eng.renderBlock(out, 2, 512);
    CHECK_NEAR(eng.transport().tempo(), 145.0, 1e-9);

    REQUIRE(io::saveAls(song, file).ok);
    auto reloaded = io::loadAls(file);
    REQUIRE(reloaded);
    CHECK_NEAR(reloaded.song->tempo, 145.0, 1e-9);
    for (const auto& env : reloaded.song->main.automation)
        if (env.targetId == reloaded.song->main.mixer.tempoTargetId)
            for (const auto& p : env.points)
                CHECK_NEAR(p.value, 145.0, 1e-9);
    file.deleteFile();
}

TEST_CASE("MPE playback: overlapping notes use independent member channels with pitch slide pressure and release velocity")
{
    model::Clip clip; clip.loop.enabled = false; clip.currentEnd = 4;
    model::MidiNote a; a.start = 0; a.duration = 1; a.pitch = 60; a.noteId = 10; a.releaseVelocity = 23;
    auto b = a; b.pitch = 64; b.noteId = 11;
    clip.notes = {a, b};
    clip.noteExpressions = {{10, -2, {{0, 12}, {1, 24}}}, {10, 74, {{0, 32}, {1, 96}}}, {11, -1, {{0, 127}, {1, 0}}}};
    auto prepared = engine::prepareClip(clip, std::shared_ptr<engine::SampleSlot>{});
    REQUIRE(prepared.expressionChannels.size() == 2); CHECK(prepared.expressionChannels[0] == 2); CHECK(prepared.expressionChannels[1] == 3);
    juce::MidiBuffer midi; midi.ensureSize(65536);
    engine::renderMidiClip(prepared, {0, 110, 0, 100}, midi);
    int ons = 0, offs = 0; bool bend = false, slide = false, pressure = false;
    for (const auto metadata : midi)
    {
        const auto message = metadata.getMessage();
        if (message.isNoteOn()) { ++ons; CHECK(message.getChannel() == (message.getNoteNumber() == 60 ? 2 : 3)); }
        if (message.isNoteOff()) { ++offs; CHECK(message.getChannel() == (message.getNoteNumber() == 60 ? 2 : 3)); CHECK(message.getVelocity() == 23); }
        if (message.isPitchWheel() && message.getChannel() == 2 && message.getPitchWheelValue() == 10240) bend = true;
        if (message.isController() && message.getChannel() == 2 && message.getControllerNumber() == 74) slide = true;
        if (message.isChannelPressure() && message.getChannel() == 3 && message.getChannelPressureValue() == 127) pressure = true;
    }
    CHECK(ons == 2); CHECK(offs == 2); CHECK(bend); CHECK(slide); CHECK(pressure);
    midi.clear(); engine::renderMidiClip(prepared, {0, 10, 0.5, 100}, midi);
    for (const auto metadata : midi) CHECK(! metadata.getMessage().isNoteOn());
    clip.notes.clear(); clip.noteExpressions.clear();
    for (int i = 0; i < 16; ++i)
    { auto note = a; note.noteId = i; note.pitch = 48 + i; clip.notes.push_back(note); clip.noteExpressions.push_back({i, -2, {{0, 0}, {1, 12}}}); }
    prepared = engine::prepareClip(clip, std::shared_ptr<engine::SampleSlot>{});
    CHECK(prepared.expressionOverflow); CHECK(prepared.expressionChannels.back() == 1);
    bool overflow = false; midi.clear(); engine::renderMidiClip(prepared, {0, 10000, 0, 10000}, midi, false, &overflow);
    CHECK(midi.data.size() <= engine::midiControllerByteLimit); CHECK(overflow);
}
