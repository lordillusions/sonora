#include "TestFramework.h"

#include "core/Scales.h"
#include "devices/DeviceFactory.h"
#include "devices/native/Arpeggiator.h"
#include "engine/AudioEngine.h"
#include "io/als/AlsTemplates.h"
#include "midi/MidiManager.h"
#include "plugins/PluginHost.h"

using namespace sonora;
using A = devices::Arpeggiator;

namespace
{
struct Event
{
    int64_t at;
    bool on;
    int pitch, velocity, channel;
};

// Drives an Arpeggiator block by block like the engine does (host info, then process) and records its output with
// absolute sample times. `input` maps absolute sample -> messages fed in that block.
struct Rig
{
    A arp;
    double rate = 48000, tempo = 120;
    bool playing = true;
    double beat = 0;
    int64_t clock = 0;
    std::vector<std::pair<int64_t, juce::MidiMessage>> input;
    std::vector<Event> out;
    std::vector<juce::MidiMessage> other;

    explicit Rig(int block = 512) : blockSize(block) { arp.prepare(rate, block); }
    int blockSize;

    void set(int index, float value) { arp.setParameter(index, value); }
    void press(int64_t at, int pitch, int velocity = 100, int channel = 1)
    { input.push_back({at, juce::MidiMessage::noteOn(channel, pitch, (juce::uint8) velocity)}); }
    void release(int64_t at, int pitch, int channel = 1) { input.push_back({at, juce::MidiMessage::noteOff(channel, pitch)}); }

    void run(int64_t until)
    {
        juce::AudioBuffer<float> audio(2, blockSize);
        juce::MidiBuffer midi;
        midi.ensureSize(65536);
        while (clock < until)
        {
            const int n = (int) std::min<int64_t>(blockSize, until - clock);
            juce::AudioBuffer<float> view(audio.getArrayOfWritePointers(), 2, n);
            midi.clear();
            for (const auto& [at, message] : input)
                if (at >= clock && at < clock + n) midi.addEvent(message, (int) (at - clock));
            arp.setHostInfo({tempo, beat, playing, rate});
            arp.process(view, midi);
            for (const auto meta : midi)
            {
                const auto m = meta.getMessage();
                if (m.isNoteOnOrOff())
                    out.push_back({clock + meta.samplePosition, m.isNoteOn(), m.getNoteNumber(), m.getVelocity(), m.getChannel()});
                else
                    other.push_back(m);
            }
            clock += n;
            if (playing) beat += n * tempo / 60.0 / rate;
        }
    }
    std::vector<Event> ons() const
    {
        std::vector<Event> r;
        for (const auto& e : out) if (e.on) r.push_back(e);
        return r;
    }
    std::vector<int> pitches() const
    {
        std::vector<int> r;
        for (const auto& e : ons()) r.push_back(e.pitch);
        return r;
    }
};

std::vector<int> pattern(int style, int n)
{
    std::array<int, A::kMaxPattern> out{};
    std::array<int, A::kMaxNotes> arrival{};
    for (int i = 0; i < n; ++i) arrival[(size_t) i] = n - 1 - i; // played high to low
    uint32_t seed = 1;
    const int len = A::buildPattern(style, n, arrival.data(), seed, out.data());
    return {out.begin(), out.begin() + len};
}
} // namespace

TEST_CASE("arpeggiator: pattern styles for four notes")
{
    CHECK(pattern(A::Up, 4) == std::vector<int>({0, 1, 2, 3}));
    CHECK(pattern(A::Down, 4) == std::vector<int>({3, 2, 1, 0}));
    CHECK(pattern(A::UpDown, 4) == std::vector<int>({0, 1, 2, 3, 2, 1}));
    CHECK(pattern(A::DownUp, 4) == std::vector<int>({3, 2, 1, 0, 1, 2}));
    CHECK(pattern(A::UpAndDown, 4) == std::vector<int>({0, 1, 2, 3, 3, 2, 1, 0}));
    CHECK(pattern(A::DownAndUp, 4) == std::vector<int>({3, 2, 1, 0, 0, 1, 2, 3}));
    CHECK(pattern(A::Converge, 4) == std::vector<int>({0, 3, 1, 2}));
    CHECK(pattern(A::Diverge, 4) == std::vector<int>({2, 1, 3, 0}));
    CHECK(pattern(A::ConAndDiverge, 4) == std::vector<int>({0, 3, 1, 2, 1, 3}));
    CHECK(pattern(A::PinkyUp, 4) == std::vector<int>({0, 3, 1, 3, 2, 3}));
    CHECK(pattern(A::PinkyUpDown, 4) == std::vector<int>({0, 3, 1, 3, 2, 3, 1, 3}));
    CHECK(pattern(A::ThumbUp, 4) == std::vector<int>({0, 1, 0, 2, 0, 3}));
    CHECK(pattern(A::ThumbUpDown, 4) == std::vector<int>({0, 1, 0, 2, 0, 3, 0, 2}));
    CHECK(pattern(A::PlayOrder, 4) == std::vector<int>({3, 2, 1, 0}));
    CHECK(pattern(A::ChordTrigger, 4) == std::vector<int>({0}));
    auto shuffled = pattern(A::RandomOther, 4);
    std::sort(shuffled.begin(), shuffled.end());
    CHECK(shuffled == std::vector<int>({0, 1, 2, 3})); // every note once per cycle
    for (int style = 0; style < A::NumStyles; ++style) // one note: every style just repeats it
        for (int i : pattern(style, 1)) CHECK(i == 0);
}

TEST_CASE("arpeggiator: synced steps sit on the song grid with the gate length")
{
    Rig rig;
    rig.press(0, 60); rig.press(0, 64); rig.press(0, 67);
    rig.run(48000); // 2 beats at 120 BPM: 1/16 steps every 6000 samples
    const auto ons = rig.ons();
    REQUIRE(ons.size() == 8);
    const std::vector<int> expected{60, 64, 67, 60, 64, 67, 60, 64};
    for (size_t i = 0; i < ons.size(); ++i)
    {
        CHECK(ons[i].at == (int64_t) i * 6000);
        CHECK(ons[i].pitch == expected[i]);
        CHECK(ons[i].velocity == 100);
    }
    // 50 % gate: each note ends half a step later.
    for (const auto& e : rig.out)
        if (! e.on) CHECK(e.at % 6000 == 3000);
}

TEST_CASE("arpeggiator: a note pressed between steps waits for the next grid step; output is block-size independent")
{
    std::vector<Event> reference;
    for (int block : {512, 64, 1000})
    {
        Rig rig(block);
        rig.press(1000, 60); rig.press(1000, 72);
        rig.release(30000, 60); rig.release(30000, 72);
        rig.run(48000);
        const auto ons = rig.ons();
        REQUIRE(! ons.empty());
        CHECK(ons.front().at == 6000);
        if (reference.empty()) reference = rig.out;
        else
        {
            REQUIRE(rig.out.size() == reference.size());
            for (size_t i = 0; i < reference.size(); ++i)
                CHECK(rig.out[i].at == reference[i].at && rig.out[i].pitch == reference[i].pitch && rig.out[i].on == reference[i].on);
        }
        CHECK(ons.back().at < 30000); // releasing the keys stops the pattern
        int sounding = 0;
        for (const auto& e : rig.out) sounding += e.on ? 1 : -1;
        CHECK(sounding == 0); // every note-on got its note-off
    }
}

TEST_CASE("arpeggiator: free rate and stopped transport run from the first note")
{
    Rig rig;
    rig.playing = false;
    rig.press(777, 50); rig.press(777, 53);
    rig.run(48000);
    auto ons = rig.ons();
    REQUIRE(ons.size() >= 4);
    CHECK(ons[0].at == 777);                // stopped + synced: free clock at the 1/16 length (6000)
    CHECK(ons[1].at == 777 + 6000);

    Rig free;
    free.set(A::Sync, 0);
    free.set(A::FreeRate, 100); // ms
    free.press(100, 50);
    free.run(48000);
    ons = free.ons();
    REQUIRE(ons.size() == 10);
    CHECK(ons[0].at == 100);
    CHECK(ons[3].at == 100 + 3 * 4800);
}

TEST_CASE("arpeggiator: Hold latches, a new gesture replaces, replaying a latched note removes it")
{
    Rig rig;
    rig.set(A::Hold, 1);
    rig.press(0, 60); rig.press(0, 64);
    rig.release(100, 60); rig.release(100, 64);
    rig.run(24000);
    CHECK(rig.pitches() == std::vector<int>({60, 64, 60, 64})); // still playing after the keys went up
    rig.press(24000, 70); rig.release(24100, 70);                 // new gesture: replaces the latch
    rig.run(48000);
    for (size_t i = 4; i < rig.ons().size(); ++i) CHECK(rig.ons()[i].pitch == 70);

    Rig toggle;
    toggle.set(A::Hold, 1);
    toggle.press(0, 60); toggle.press(0, 64); toggle.press(0, 67);
    toggle.release(10, 64); // 60 and 67 still down, 64 latched
    toggle.press(20, 64);   // played again while others are held: removed
    toggle.run(24000);
    CHECK(toggle.pitches() == std::vector<int>({60, 67, 60, 67}));
    toggle.set(A::Hold, 0); // turning Hold off drops what is only latched (nothing is: 60/67 are down)
    toggle.run(36000);
    CHECK(toggle.ons().size() == 6);
}

TEST_CASE("arpeggiator: transposition passes, in-scale transposition, offset and repeats")
{
    Rig octaves;
    octaves.set(A::TransposeSteps, 2);
    octaves.set(A::TransposeDistance, 12);
    octaves.press(0, 60); octaves.press(0, 64);
    octaves.run(48000);
    CHECK(octaves.pitches() == std::vector<int>({60, 64, 72, 76, 84, 88, 60, 64}));

    Rig scale;
    scale.set(A::TransposeMode, 1); // Major
    scale.set(A::TransposeKey, 0);  // C
    scale.set(A::TransposeSteps, 1);
    scale.set(A::TransposeDistance, 2); // two scale degrees: C->E, E->G, B->D
    scale.press(0, 60); scale.press(0, 64); scale.press(0, 71);
    scale.run(36000);
    CHECK(scale.pitches() == std::vector<int>({60, 64, 71, 64, 67, 74}));
    CHECK(model::transposeInScale(61, 1, 0, model::kScales[0]) == 63); // C# keeps its +1 above the moved step
    CHECK(model::transposeInScale(60, -1, 0, model::kScales[0]) == 59);

    Rig offset;
    offset.set(A::Offset, 1);
    offset.press(0, 60); offset.press(0, 64); offset.press(0, 67);
    offset.run(18000);
    CHECK(offset.pitches() == std::vector<int>({64, 67, 60}));

    Rig strum;
    strum.set(A::Repeats, 1);
    strum.press(0, 60); strum.press(0, 64); strum.press(0, 67);
    strum.run(48000);
    CHECK(strum.pitches() == std::vector<int>({60, 64, 67})); // one pass, then silence while the keys stay down
}

TEST_CASE("arpeggiator: Note and Beat retrigger, velocity ramp, chord trigger")
{
    Rig note;
    note.set(A::Retrigger, 1);
    note.press(0, 60); note.press(0, 64); note.press(0, 67);
    note.press(13000, 62); // adds D after two steps: the pattern restarts from the lowest note
    note.run(30000);
    CHECK(note.pitches() == std::vector<int>({60, 64, 67, 60, 62}));

    Rig beat;
    beat.set(A::Retrigger, 2);
    beat.set(A::RetriggerInterval, 3); // "1/4" = one beat = four 1/16 steps (as measured in the reference)
    for (int p : {60, 62, 64, 65, 67}) beat.press(0, p);
    beat.run(48000);
    CHECK(beat.pitches() == std::vector<int>({60, 62, 64, 65, 60, 62, 64, 65}));

    Rig ramp;
    ramp.set(A::VelocityOn, 1);
    ramp.set(A::VelocityDecay, 500);
    ramp.set(A::VelocityTarget, 0);
    ramp.press(0, 60, 120);
    ramp.run(36000);
    const auto ons = ramp.ons();
    REQUIRE(ons.size() == 6);
    CHECK(ons[0].velocity == 120);
    CHECK(ons[1].velocity == 90);  // 1/16 at 120 BPM = 125 ms: a quarter of the 500 ms ramp from 120 to 0
    CHECK(ons[2].velocity == 60);
    for (size_t i = 1; i < ons.size(); ++i) CHECK(ons[i].velocity <= ons[i - 1].velocity);
    CHECK(ons[4].velocity == 1); // reached the target (clamped to a valid note-on)

    Rig chord;
    chord.set(A::Style, A::ChordTrigger);
    chord.press(0, 60); chord.press(0, 64);
    chord.run(12000);
    CHECK(chord.pitches() == std::vector<int>({60, 64, 60, 64}));
    CHECK(chord.ons()[2].at == 6000);
}

TEST_CASE("arpeggiator: swing groove delays the off-beat sixteenths, monotonically")
{
    CHECK_NEAR(A::grooveWarp(0.25, 2), 0.25 + 0.25 / 3.0, 1e-12);
    CHECK_NEAR(A::grooveWarp(0.5, 2), 0.5, 1e-12);
    CHECK_NEAR(A::grooveWarp(0.75, 2), 0.75 + 0.25 / 3.0, 1e-12);
    double last = -1;
    for (double b = 0; b < 4; b += 1.0 / 128) { const double w = A::grooveWarp(b, 1); CHECK(w > last); last = w; }

    Rig rig;
    rig.set(A::Groove, 2);
    rig.press(0, 60);
    rig.run(24000);
    const auto ons = rig.ons();
    REQUIRE(ons.size() == 4);
    CHECK(ons[0].at == 0);
    CHECK(ons[1].at == 6000 + 2000);
    CHECK(ons[2].at == 12000);
}

TEST_CASE("arpeggiator: controllers pass through, All Notes Off releases, bypass passes notes unchanged")
{
    Rig rig;
    rig.press(0, 60);
    rig.input.push_back({100, juce::MidiMessage::controllerEvent(1, 1, 33)});
    rig.input.push_back({7000, juce::MidiMessage::allNotesOff(1)});
    rig.run(24000);
    CHECK(rig.pitches() == std::vector<int>({60, 60})); // steps at 0 and 6000, then the keys are gone
    bool modWheel = false, allOff = false;
    for (const auto& m : rig.other) { modWheel |= m.isControllerOfType(1); allOff |= m.isAllNotesOff(); }
    CHECK(modWheel);
    CHECK(allOff);

    Rig bypass;
    bypass.arp.setBypassed(true);
    bypass.press(10, 60); bypass.release(20000, 60);
    bypass.run(24000);
    REQUIRE(bypass.out.size() == 2);
    CHECK(bypass.out[0].at == 10);
    CHECK(bypass.out[1].at == 20000);
}

TEST_CASE("arpeggiator: imported MidiArpeggiator parameters map onto the native device")
{
    model::Device d;
    d.kind = model::DeviceKind::Native;
    d.type = "MidiArpeggiator";
    d.parameters.push_back({"Mode", 2, -1, {}});
    d.parameters.push_back({"SyncedRate", 8, -1, {}});
    d.parameters.push_back({"Gate", 84, -1, {}});
    d.parameters.push_back({"TransposeDistance", 12, -1, {}});
    std::vector<std::string> warnings;
    auto device = devices::createNative(d, &warnings);
    REQUIRE(device != nullptr);
    CHECK(device->name() == "Arpeggiator");
    CHECK(std::lround(device->getParameter(A::Style)) == A::UpDown);
    CHECK(std::lround(device->getParameter(A::SyncedRate)) == 8);
    CHECK_NEAR(A::syncedRateBeats(8), 0.5, 1e-12); // 1/8
    CHECK_NEAR(device->getParameter(A::Gate), 84.0f, 1e-4f);
    CHECK(warnings.empty());
    const auto fresh = devices::nativeDeviceModel("MidiArpeggiator");
    CHECK(fresh.kind == model::DeviceKind::Native);
    CHECK(fresh.parameters.size() == (size_t) A::NumParams);

    // A freshly inserted device is saved under the reference element names, booleans as true/false.
    auto xml = juce::XmlDocument::parse(juce::String(io::deviceTemplate(fresh)));
    REQUIRE(xml != nullptr);
    CHECK(xml->getTagName() == "MidiArpeggiator");
    for (const char* tag : {"Mode", "PatternOffset", "RepeatCount", "SyncedRate", "FreeRate", "Gate", "RetriggerInterval",
                            "TransposeMode", "TransposeKey", "TransposeSteps", "TransposeDistance", "VelocityDecay", "VelocityTarget"})
        CHECK(xml->getChildByName(tag) != nullptr && xml->getChildByName(tag)->getChildByName("Manual") != nullptr);
    CHECK(xml->getChildByName("SyncState")->getChildByName("Manual")->getStringAttribute("Value") == "true");
    CHECK(xml->getChildByName("Hold")->getChildByName("Manual")->getStringAttribute("Value") == "false");
    CHECK(xml->getChildByName("SyncedRate")->getChildByName("Manual")->getStringAttribute("Value") == "6");
}

namespace
{
class OnRecorder final : public devices::DeviceProcessor
{
public:
    std::string name() const override { return "recorder"; }
    void prepare(double, int) override {}
    void process(juce::AudioBuffer<float>& audio, juce::MidiBuffer& midi) override
    {
        for (const auto m : midi)
            if (m.getMessage().isNoteOn()) ons.push_back({counter + m.samplePosition, m.getMessage().getNoteNumber()});
        counter += audio.getNumSamples();
    }
    std::vector<std::pair<long long, int>> ons;
    long long counter = 0;
};
} // namespace

TEST_CASE("arpeggiator: in the engine a looping chord clip arpeggiates on the grid and restarts at the loop wrap")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    plugins::PluginHost host;
    midi::MidiManager midiManager;
    engine::AudioEngine eng(host, midiManager);
    model::Song song;
    song.tempo = 120.0; // 44.1 kHz: a 1/16 step is 5512.5 samples, so steps fall between samples
    model::Track t;
    t.id = 1;
    t.kind = model::TrackKind::Midi;
    model::Clip clip;
    clip.kind = model::Clip::Kind::Midi;
    clip.currentStart = 0.0;
    clip.currentEnd = 4.0;
    clip.loop = {0.0, 4.0, 0.0, false};
    for (int pitch : {60, 64, 67}) clip.notes.push_back({0.0, 4.0, pitch, 100.0f});
    t.arrangementClips = {clip};
    t.devices.push_back(devices::nativeDeviceModel("MidiArpeggiator"));
    song.tracks.push_back(t);

    engine::SampleCache samples;
    devices::BuildContext ctx;
    auto built = engine::buildGraph(song, ctx, samples);
    REQUIRE(built.graph->nodes[0].devices.size() == 1);
    CHECK(built.graph->nodes[0].devices[0]->name() == "Arpeggiator");
    auto recorder = std::make_unique<OnRecorder>();
    auto* rec = recorder.get();
    built.graph->nodes[0].devices.push_back(std::move(recorder));
    eng.installGraph(std::move(built.graph));
    eng.transport().setLoop(true, 0.0, 4.0);
    eng.transport().play();

    std::vector<float> l(512), r(512);
    float* out[2] = {l.data(), r.data()};
    for (int b = 0; b < 345; ++b) eng.renderBlock(out, 2, 512);

    REQUIRE(rec->ons.size() >= 32);
    for (int k = 0; k < 32; ++k)
    {
        const int step = k % 16;
        const long long expected = (k / 16) * 88200LL + (long long) std::floor(step * 5512.5 + 1e-6);
        CHECK(std::llabs(rec->ons[(size_t) k].first - expected) <= 1);
        constexpr int chord[]{60, 64, 67};
        CHECK(rec->ons[(size_t) k].second == chord[step % 3]); // the pattern starts over with each loop
    }
}
