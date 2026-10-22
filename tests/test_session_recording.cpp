#include "TestFramework.h"
#include "core/Edit.h"
#include "engine/AudioEngine.h"
#include "engine/ClipLauncher.h"
#include "io/als/AlsReader.h"
#include "io/als/AlsWriter.h"
#include "midi/MidiManager.h"
#include "plugins/PluginHost.h"

using namespace sonora;
namespace
{
struct SessionFixture
{
    juce::TemporaryFile file{".xml"};
    model::Song song;
    SessionFixture()
    {
        std::string error;
        auto xml = io::readAlsDom(juce::File(SONORA_TEST_FIXTURES).getChildFile("recording_set.xml"), error); REQUIRE(xml);
        auto* tracks = xml->getChildByName("LiveSet")->getChildByName("Tracks");
        auto* second = new juce::XmlElement(*tracks->getChildByName("MidiTrack")); second->setAttribute("Id", 3);
        second->getChildByName("Name")->getChildByName("EffectiveName")->setAttribute("Value", "Second Keys"); tracks->addChildElement(second);
        REQUIRE(file.getFile().replaceWithText(xml->toString()));
        auto loaded = io::loadAls(file.getFile()); REQUIRE(loaded); song = std::move(*loaded.song);
        REQUIRE(song.tracks.size() == 3);
    }
};

bool finished(engine::RecordingManager& manager)
{
    for (int i = 0; i < 2000 && ! manager.finished(); ++i) juce::Thread::sleep(1);
    return manager.finished();
}

void tick(engine::RecordingManager& manager, double beat, int samples, const juce::MidiBuffer& input,
          const float* const* audio = nullptr, int channels = 0)
{
    manager.collectRetiredPreviews(); manager.beginBlock();
    const engine::BlockSegment segment{0, samples, beat, 100}; manager.prepareBlock(&segment, 1, true, false);
    manager.capture(audio, channels, input, beat, 100, samples, true, false, false);
}

void commit(model::Song& song, model::EditHistory& history, engine::Recorder::Result& result)
{
    std::vector<model::EditCommand> commands;
    for (auto& take : result.session)
    {
        auto targets = model::assignRecordedMidiTargets(song, take.track, {&take.clip}); if (targets.apply) commands.push_back(std::move(targets));
        commands.push_back(take.uid >= 0 ? model::modifyClip(song, model::ClipLocation::session(take.track, take.uid, take.scene), std::move(take.clip), "Overdub")
                                        : model::setSessionClip(song, take.track, take.scene, std::move(take.clip)));
    }
    history.perform(song, model::compose("Record Session Take", std::move(commands)));
}

class SessionObserver final : public devices::DeviceProcessor
{
public:
    std::array<int, 128> ons{}; int releases = 0;
    std::string name() const override { return "Session recording observer"; }
    void prepare(double, int) override {}
    void process(juce::AudioBuffer<float>&, juce::MidiBuffer& input) override
    {
        for (const auto e : input)
        {
            if (e.numBytes == 3 && (e.data[0] & 0xf0) == 0x90 && e.data[2] > 0) ++ons[e.data[1]];
            if (e.numBytes == 3 && (e.data[0] & 0xf0) == 0xb0 && e.data[1] == 123) ++releases;
        }
    }
};
}

TEST_CASE("independent Session recording: staggered quantised MIDI tracks finish and save without ending the other take")
{
    SessionFixture fixture; auto& song = fixture.song; engine::ControlBank controls(4);
    controls[0].armed.store(true); controls[2].armed.store(true);
    controls[0].inputRoute.store(model::packInput({model::InputKind::Midi, 1, {}}));
    controls[2].inputRoute.store(model::packInput({model::InputKind::Midi, 2, {}}));
    engine::RecordingManager manager; engine::Recorder::SessionPlan first; first.targets = {{0, 1, -1}}; first.quant = 1;
    juce::TemporaryFile folder; std::string error; REQUIRE(manager.start(song, controls, folder.getFile(), 1000, error, false, &first));
    juce::MidiBuffer input; input.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8) 90), 30);
    tick(manager, 0.5, 100, input); CHECK(manager.sessionPhase(0) == 2); // held input is promoted at beat 1
    auto second = first; second.targets = {{2, 3, -1}};
    REQUIRE(manager.start(song, controls, folder.getFile(), 1000, error, false, &second));
    input.clear(); input.addEvent(juce::MidiMessage::noteOn(2, 67, (juce::uint8) 100), 40);
    input.addEvent(juce::MidiMessage::controllerEvent(2, 64, 127), 60); tick(manager, 1.5, 100, input);
    CHECK(manager.sessionPhase(0) == 2); CHECK(manager.sessionPhase(2) == 2);
    manager.requestSessionStop(0, false); input.clear(); tick(manager, 2.5, 100, input);
    REQUIRE(finished(manager)); CHECK(manager.capturesTrack(0)); CHECK(manager.sessionScene(0) == 1); // protected through drain
    auto a = manager.takeResult(); REQUIRE(a.session.size() == 1); CHECK(a.session[0].track == 0); CHECK(! a.session[0].launchAfterRecording);
    CHECK_NEAR(a.session[0].clip.loop.loopEnd, 2, 1e-12); REQUIRE(a.session[0].clip.notes.size() == 1);
    CHECK_NEAR(a.session[0].clip.notes[0].duration, 2, 1e-12); CHECK(manager.active()); CHECK(manager.busy()); CHECK(! manager.capturesTrack(0));
    model::EditHistory history; commit(song, history, a);
    juce::TemporaryFile saved(".als"); REQUIRE(io::saveAls(song, saved.getFile()).ok); auto reload = io::loadAls(saved.getFile()); REQUIRE(reload);
    CHECK(reload.song->tracks[0].slots[1].clip.has_value());
    CHECK(std::none_of(song.tracks[2].slots.begin(), song.tracks[2].slots.end(), [](const auto& slot) { return slot.clip.has_value(); }));
    input.addEvent(juce::MidiMessage::noteOff(2, 67), 20); input.addEvent(juce::MidiMessage::controllerEvent(2, 64, 0), 40);
    tick(manager, 3.5, 100, input); manager.requestSessionStop(2); input.clear(); tick(manager, 4.5, 100, input);
    REQUIRE(finished(manager)); auto b = manager.takeResult(); REQUIRE(b.session.size() == 1); CHECK(b.session[0].track == 2); CHECK(b.session[0].launchAfterRecording);
    CHECK_NEAR(b.session[0].clip.loop.loopEnd, 3, 1e-12); REQUIRE(b.session[0].clip.notes.size() == 1);
    CHECK_NEAR(b.session[0].clip.notes[0].duration, 1.7, 1e-12); CHECK(! manager.busy()); commit(song, history, b);
    REQUIRE(io::saveAls(song, saved.getFile()).ok); reload = io::loadAls(saved.getFile()); REQUIRE(reload);
    CHECK(reload.song->tracks[2].slots[3].clip->notes[0].pitch == 67); REQUIRE(reload.song->tracks[2].slots[3].clip->envelopes.size() == 1);
    REQUIRE(history.undo(song)); CHECK(song.tracks[2].slots.size() < 4 || ! song.tracks[2].slots[3].clip); CHECK(song.tracks[0].slots[1].clip.has_value());
    REQUIRE(history.redo(song)); CHECK(song.tracks[2].slots[3].clip.has_value());
}

TEST_CASE("independent Session recording: audio stop WAV drain and MIDI queued cancellation do not stop a running track")
{
    SessionFixture fixture; engine::ControlBank controls(4); controls[0].armed.store(true); controls[1].armed.store(true); controls[2].armed.store(true);
    controls[1].inputRoute.store(model::packInput({model::InputKind::AudioMono, 1, {}}));
    engine::RecordingManager manager; engine::Recorder::SessionPlan plan; plan.targets = {{0, 0, -1}, {1, 2, -1}}; plan.quant = 1;
    juce::TemporaryFile directory; std::string error; REQUIRE(manager.start(fixture.song, controls, directory.getFile(), 1000, error, false, &plan));
    float left[200]{}, right[200]; std::fill_n(right, 200, 0.25f); const float* audio[] = {left, right}; juce::MidiBuffer input;
    tick(manager, 0, 100, input, audio, 2); manager.requestSessionStop(1, false); tick(manager, 1, 100, input, audio, 2);
    REQUIRE(finished(manager)); auto result = manager.takeResult(); REQUIRE(result.session.size() == 1); CHECK(result.session[0].track == 1);
    juce::WavAudioFormat format; const juce::File recorded(juce::String::fromUTF8(result.session[0].clip.sample.absolutePath.c_str()));
    std::unique_ptr<juce::AudioFormatReader> wav(format.createReaderFor(new juce::FileInputStream(recorded), true)); REQUIRE(wav);
    CHECK(wav->lengthInSamples == 100); CHECK(wav->numChannels == 1); juce::AudioBuffer<float> read(1, 100); REQUIRE(wav->read(&read, 0, 100, 0, true, false)); CHECK_NEAR(read.getSample(0, 20), 0.25, 1e-6);
    auto queued = plan; queued.targets = {{2, 1, -1}}; queued.quant = 4;
    REQUIRE(manager.start(fixture.song, controls, directory.getFile(), 1000, error, false, &queued)); manager.requestSessionStop(2);
    tick(manager, 2, 100, input); REQUIRE(finished(manager)); CHECK(manager.takeResult().session.empty()); CHECK(manager.sessionPhase(0) == 2);
    manager.requestSessionStop(-1, false); tick(manager, 3, 100, input); REQUIRE(finished(manager)); result = manager.takeResult(); REQUIRE(result.session.size() == 1);
    CHECK(result.session[0].track == 0); CHECK_NEAR(result.session[0].clip.loop.loopEnd, 3, 1e-12); CHECK(! manager.busy());
}

TEST_CASE("independent Session recording: actual callbacks retain another track's live overdub and voices during partial save and restart")
{
    juce::ScopedJuceInitialiser_GUI init; SessionFixture fixture; auto& song = fixture.song;
    plugins::PluginHost host; midi::MidiManager input; engine::AudioEngine audio(host, input);
    for (const int track : {0, 2})
    {
        auto clip = song.tracks[(size_t) track].arrangementClips[0]; clip.loop = {0, 0.03125, 0, true}; clip.notes = {{0, 0.03, 60 + track, 80}};
        model::setSessionClip(song, track, 1, clip).apply(song); model::setInputSelection(song.tracks[(size_t) track], {model::InputKind::Midi, track == 0 ? 1 : 2, {}});
    }
    song.loopOn = true; song.loopStart = 0; song.loopLength = 0.015625; audio.loadSong(song);
    auto device = std::make_unique<SessionObserver>(); auto* observer = device.get(); audio.latestGraph()->nodes[2].devices.clear(); audio.latestGraph()->nodes[2].devices.push_back(std::move(device));
    (*audio.controls())[0].armed.store(true); (*audio.controls())[2].armed.store(true);
    engine::Recorder::SessionPlan plan; plan.targets = {{0, 1, song.tracks[0].slots[1].clip->uid}, {2, 1, song.tracks[2].slots[1].clip->uid}};
    juce::TemporaryFile folder; std::string error; REQUIRE(audio.recorder().start(song, *audio.controls(), folder.getFile(), 44100, error, false, &plan));
    for (const int track : {0, 2}) engine::launchClip(*audio.latestGraph(), audio.transport(), track, 1, 0);
    juce::AudioBuffer<float> output(2, 128); auto run = [&] { audio.renderBlock(output.getArrayOfWritePointers(), 2, 128); };
    input.handleMessage(juce::MidiMessage::noteOn(1, 65, (juce::uint8) 100), true, false); input.handleMessage(juce::MidiMessage::noteOn(2, 67, (juce::uint8) 100), true, false); run();
    input.handleMessage(juce::MidiMessage::noteOff(1, 65), true, false); input.handleMessage(juce::MidiMessage::noteOff(2, 67), true, false); run();
    for (int i = 0; i < 8; ++i) run(); CHECK(observer->ons[67] > 1);
    audio.recorder().requestSessionStop(0); run(); REQUIRE(finished(audio.recorder())); auto first = audio.recorder().takeResult(); REQUIRE(first.session.size() == 1);
    model::EditHistory history; commit(song, history, first); const int releases = observer->releases, ons = observer->ons[67];
    audio.updateTrackClips(0, song); for (int i = 0; i < 8; ++i) run(); CHECK(observer->releases == releases); CHECK(observer->ons[67] > ons); CHECK(audio.recorder().sessionPhase(2) == 2);
    auto restart = plan; restart.targets.resize(1); REQUIRE(audio.recorder().start(song, *audio.controls(), folder.getFile(), 44100, error, false, &restart));
    // No message timer has run between publications: the pending bank still owns both generations.
    for (int i = 0; i < 4; ++i) { audio.recorder().collectRetiredPreviews(); run(); }
    CHECK(audio.recorder().sessionPhase(0) == 2); CHECK(observer->releases == releases);
    audio.recorder().requestSessionStop(); run(); engine::Recorder::Result final;
    for (int i = 0; i < 2000 && audio.recorder().busy(); ++i)
    {
        if (audio.recorder().finished())
        {
            auto ready = audio.recorder().takeResult();
            for (auto& take : ready.session) final.session.push_back(std::move(take));
        }
        else juce::Thread::sleep(1);
    }
    REQUIRE(final.session.size() == 1); CHECK(final.session[0].track == 2); CHECK(! audio.recorder().busy());
}

TEST_CASE("Session recording: starting an overdub cancels held Repeat and preserves the take clock")
{
    juce::ScopedJuceInitialiser_GUI init;
    SessionFixture fixture;
    auto& song = fixture.song;
    auto clip = song.tracks[0].arrangementClips[0];
    clip.launchMode = 3;
    model::setSessionClip(song, 0, 1, clip).apply(song);
    plugins::PluginHost host; midi::MidiManager midi; engine::AudioEngine audio(host, midi);
    audio.loadSong(song);
    auto& graph = *audio.latestGraph();
    const auto& target = *song.tracks[0].slots[1].clip;
    engine::launchClipWithMode(graph, audio.transport(), target, 0, 1, 0.005);
    juce::AudioBuffer<float> output(2, 128);
    auto run = [&] { audio.renderBlock(output.getArrayOfWritePointers(), 2, 128); };
    for (int i = 0; i < 4; ++i) run();
    CHECK((*audio.controls())[0].repeatSlot.load() == 1);
    const double before = (*audio.controls())[0].sessionElapsed.load();
    (*audio.controls())[0].armed.store(true);
    engine::Recorder::SessionPlan plan; plan.targets = {{0, 1, target.uid}};
    juce::TemporaryFile folder; std::string error;
    REQUIRE(audio.recorder().start(song, *audio.controls(), folder.getFile(), 44100, error, false, &plan));
    midi.handleMessage(juce::MidiMessage::noteOn(1, 69, (juce::uint8) 100), true, false);
    for (int i = 0; i < 8; ++i) run();
    CHECK((*audio.controls())[0].repeatSlot.load() == engine::TrackControls::kNoSlot);
    CHECK_NEAR((*audio.controls())[0].sessionElapsed.load(), before + 1024.0 / 22050, 1e-10);
    midi.handleMessage(juce::MidiMessage::noteOff(1, 69), true, false);
    audio.recorder().requestSessionStop(0, false); run();
    REQUIRE(finished(audio.recorder()));
    const auto result = audio.recorder().takeResult();
    REQUIRE(result.session.size() == 1);
    CHECK(result.session[0].uid == target.uid);
}

TEST_CASE("independent Session recording: target protection and failed group preparation leave active captures and disk intact")
{
    SessionFixture fixture; engine::ControlBank controls(4); for (const int track : {0, 1, 2}) controls[(size_t) track].armed.store(true);
    engine::RecordingManager manager; engine::Recorder::SessionPlan plan; plan.targets = {{0, 1, -1}};
    juce::TemporaryFile folder; std::string error; REQUIRE(manager.start(fixture.song, controls, folder.getFile(), 1000, error, false, &plan));
    juce::MidiBuffer input; tick(manager, 0, 100, input);
    CHECK(! manager.start(fixture.song, controls, folder.getFile(), 1000, error, false, &plan));
    CHECK(! manager.start(fixture.song, controls, folder.getFile(), 1000, error));
    auto invalid = plan; invalid.targets = {{1, 2, -1}, {2, 3, -1}};
    controls[2].inputRoute.store(model::packInput({model::InputKind::None, 0, {}}));
    CHECK(! manager.start(fixture.song, controls, folder.getFile(), 1000, error, false, &invalid));
    CHECK(manager.sessionScene(1) == -1); CHECK(manager.sessionScene(2) == -1); CHECK(manager.sessionPhase(0) == 2);
    CHECK(folder.getFile().findChildFiles(juce::File::findFiles, false, "*.wav").isEmpty());
    manager.requestStop(); tick(manager, 1, 100, input); REQUIRE(finished(manager)); CHECK(manager.takeResult().session.size() == 1); CHECK(! manager.busy());
    // Device disconnect before the first audio bank adoption also cancels every queued child.
    controls[2].inputRoute.store(0); REQUIRE(manager.start(fixture.song, controls, folder.getFile(), 1000, error, false, &invalid));
    manager.deviceStopped();
    for (int i = 0; i < 2000 && manager.busy(); ++i)
    {
        if (manager.finished()) CHECK(manager.takeResult().session.empty());
        else juce::Thread::sleep(1);
    }
    CHECK(! manager.busy()); CHECK(folder.getFile().findChildFiles(juce::File::findFiles, false, "*.wav").isEmpty());
}
