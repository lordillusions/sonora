// PLG-25a: the app and plug-in plumbing of the pattern generators, with the test-only generator: the clip menu's
// "Apply <device> to Clip", a generator face's To Clip button (wireProFace), the VST3 MIDI bridge and the Drag MIDI file.
#include "TestFramework.h"
#include "TestGenerator.h"

#include "engine/EngineGraph.h"
#include "plugin/MidiDragOut.h"
#include "plugin/PluginCommon.h"
#include "ui/GeneratorClipUi.h"
#include "ui/TrackMenu.h"
#include "ui/UiContext.h"
#include "ui/views/ProFace.h"
#include "ui/views/ProFaceWindow.h"

#include <juce_audio_basics/juce_audio_basics.h>

using namespace sonora;
using testkit::TestGenerator;

namespace
{
struct App
{
    model::Song song;
    engine::EngineGraph graph;
    TestGenerator gen;
    ui::UiContext ctx;
    model::EditHistory history;
    int edits = 0;

    App()
    {
        song.tempo = 138;
        song.midiClipTemplate = std::make_shared<std::string>("<MidiClip/>");
        song.tracks.resize(2);
        song.tracks[0].kind = model::TrackKind::Midi;
        song.tracks[0].id = 1;
        song.tracks[0].devices.resize(1);
        song.tracks[1].kind = model::TrackKind::Midi;
        song.tracks[1].id = 2;
        model::Clip clip;
        clip.kind = model::Clip::Kind::Midi;
        clip.uid = 7;
        clip.currentStart = clip.time = 8;
        clip.currentEnd = 16;
        clip.loop = {0, 8, 0, true};
        for (int pitch : {57, 60, 64})
        {
            model::MidiNote n;
            n.duration = 8;
            n.pitch = pitch;
            clip.notes.push_back(n);
        }
        song.tracks[0].arrangementClips = {clip};
        clip.uid = 8;
        song.tracks[1].arrangementClips = {clip};
        gen.prepare(48000, 512);
        graph.nodes.resize(2);
        graph.nodes[0].processorForModelDevice = {&gen};
        ctx.song = &song;
        ctx.graph = &graph;
        ctx.applyEdit = [this](model::EditCommand command) { history.perform(song, std::move(command)); ++edits; };
    }
    const model::Clip& clip(int track = 0) const { return song.tracks[(size_t) track].arrangementClips.front(); }
};

class BareFace final : public ui::ProFace
{
public:
    explicit BareFace(devices::DeviceProcessor& d) : ProFace(d, {400, 400}, {300, 100}, 0xFF35C2B0) {}

protected:
    void build(Layout) override {}
};
} // namespace

TEST_CASE("generator clip menu: Apply <device> to Clip on tracks with a generator, one undo step, device switched off")
{
    App app;
    const auto actions = ui::clipContextActions(app.ctx, model::ClipLocation::arrangement(0, 7));
    const ui::MenuAction* apply = nullptr;
    for (const auto& a : actions)
        if (a.label == "Apply Test Generator to Clip") apply = &a;
    REQUIRE(apply != nullptr);
    CHECK(apply->enabled);
    const auto before = app.clip().notes;
    apply->run();
    CHECK(app.edits == 1);
    CHECK(app.history.canUndo());
    // What the device plays over song beats 8..16 with the clip's chord held.
    std::vector<model::MidiNote> held = before;
    for (auto& n : held) n.start += 8;
    const auto expected = app.gen.renderNotes(held, 8, 16, 138);
    REQUIRE(app.clip().notes.size() == expected.size());
    for (size_t i = 0; i < expected.size(); ++i)
    {
        CHECK_NEAR(app.clip().notes[i].start, expected[i].start - 8, 1e-9);
        CHECK(app.clip().notes[i].pitch == expected[i].pitch);
    }
    CHECK(app.gen.isBypassed());
    CHECK(app.ctx.statusText.find("is now off") != std::string::npos);
    app.history.undo(app.song);
    CHECK(app.clip().notes.size() == before.size());
    // A track without a generator has no such entry.
    for (const auto& a : ui::clipContextActions(app.ctx, model::ClipLocation::arrangement(1, 8)))
        CHECK(a.label.find("Apply ") != 0);
    CHECK(ui::generatorDevices(app.ctx, 0) == std::vector<int>({0}));
    CHECK(ui::generatorDevices(app.ctx, 1).empty());
}

TEST_CASE("generator face: To Clip writes into the selected clip, or a new clip at the loop start")
{
    App app;
    BareFace face(app.gen);
    CHECK(! face.onToClip);
    ui::wireProFace(face, [&app] { return &app.ctx; }, 0, 0);
    REQUIRE(face.onToClip != nullptr);
    // The selected clip on the device's track.
    app.ctx.selectedClipTrack = 0;
    app.ctx.selectedClipUid = 7;
    face.onToClip();
    CHECK(app.edits == 1);
    CHECK(app.song.tracks[0].arrangementClips.size() == 1);
    CHECK(app.clip().notes.size() > 10);
    // No clip selected (or one on another track): a new clip of the device's length at the loop start.
    app.gen.setBypassed(false);
    app.gen.setParameter(TestGenerator::Fixed, 1);
    app.ctx.selectedClipTrack = 1;
    app.ctx.selectedClipUid = 8;
    app.song.loopOn = true;
    app.song.loopStart = 32;
    face.onToClip();
    CHECK(app.edits == 2);
    REQUIRE(app.song.tracks[0].arrangementClips.size() == 2);
    const auto& fresh = app.song.tracks[0].arrangementClips.back();
    CHECK_NEAR(fresh.currentStart, 32.0, 1e-9);
    CHECK_NEAR(fresh.currentEnd - fresh.currentStart, app.gen.clipLengthBeats(), 1e-9);
    CHECK(fresh.name == "Test Generator");
    CHECK(! fresh.notes.empty());
    // A clip in the way: refused, nothing changes, the status line says why.
    app.gen.setBypassed(false);
    app.song.loopStart = 36;
    face.onToClip();
    CHECK(app.edits == 2);
    CHECK(app.ctx.statusText.find("in the way") != std::string::npos);
    CHECK(! app.gen.isBypassed());
}

TEST_CASE("generator plugin: the MIDI bridge runs the device in prepared chunks and returns its notes in place of the input")
{
    TestGenerator gen;
    gen.prepare(48000, 256);
    plugin::GeneratorMidiBridge bridge;
    bridge.prepare(256);
    devices::HostInfo info;
    info.tempo = 120;
    info.playing = true;
    info.sampleRate = 48000;
    juce::AudioBuffer<float> audio(2, 6000); // larger than announced: the bridge cuts it
    juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8) 100), 0);
    midi.addEvent(juce::MidiMessage::controllerEvent(1, 74, 33), 10);
    bridge.process(gen, audio, midi, info);
    std::vector<int> onsAt;
    int controllers = 0;
    for (const auto meta : midi)
    {
        const auto m = meta.getMessage();
        if (m.isNoteOn()) { onsAt.push_back(meta.samplePosition); CHECK(m.getNoteNumber() >= 60 && m.getNoteNumber() <= 62); }
        if (m.isController()) ++controllers;
    }
    CHECK(controllers == 1);
    // A sixteenth at 120 BPM is 6000 samples: only step 0 starts in this block; its gate (50 %) ends at 3000.
    REQUIRE(onsAt.size() == 1);
    CHECK(onsAt[0] == 0);
    int firstOffs = 0;
    for (const auto meta : midi) firstOffs += meta.getMessage().isNoteOff() && meta.samplePosition == 3000;
    CHECK(firstOffs == 1);
    // The next block continues the song position: step 1 at sample 6000 (= 0 of this block).
    juce::MidiBuffer next;
    info.beats = 6000.0 / 24000.0;
    bridge.process(gen, audio, next, info);
    int ons = 0, offs = 0;
    for (const auto meta : next) { ons += meta.getMessage().isNoteOn(); offs += meta.getMessage().isNoteOff(); }
    CHECK(ons == 1);
    CHECK(offs == 1);
}

TEST_CASE("generator plugin: Drag MIDI writes a Standard MIDI File that reads back with the same notes")
{
    std::vector<model::MidiNote> notes;
    for (int i = 0; i < 8; ++i)
    {
        model::MidiNote n;
        n.start = i * 0.5;
        n.duration = 0.25;
        n.pitch = 57 + i;
        n.velocity = (float) (80 + i);
        notes.push_back(n);
    }
    std::string error;
    const auto file = plugin::writeDragMidiFile(notes, 4.0, 138, "Test Part: arp/1", error);
    REQUIRE(file.existsAsFile());
    CHECK(error.empty());
    CHECK(file.getFileName().endsWith(".mid"));
    juce::FileInputStream in(file);
    juce::MidiFile midi;
    REQUIRE(midi.readFrom(in));
    CHECK(midi.getTimeFormat() == 960);
    juce::MidiMessageSequence all;
    for (int t = 0; t < midi.getNumTracks(); ++t) all.addSequence(*midi.getTrack(t), 0);
    all.updateMatchedPairs();
    std::vector<std::pair<double, int>> ons;
    double tempo = 0;
    for (int i = 0; i < all.getNumEvents(); ++i)
    {
        const auto& m = all.getEventPointer(i)->message;
        if (m.isTempoMetaEvent()) tempo = 60.0 / m.getTempoSecondsPerQuarterNote();
        if (m.isNoteOn())
        {
            ons.push_back({m.getTimeStamp() / 960.0, m.getNoteNumber()});
            CHECK(std::abs(all.getTimeOfMatchingKeyUp(i) - m.getTimeStamp() - 240.0) < 1e-6);
        }
    }
    CHECK(std::abs(tempo - 138.0) < 0.01);
    REQUIRE(ons.size() == notes.size());
    for (size_t i = 0; i < ons.size(); ++i)
    {
        CHECK(std::abs(ons[i].first - notes[i].start) < 1e-9);
        CHECK(ons[i].second == notes[i].pitch);
    }
    file.deleteFile();
}
