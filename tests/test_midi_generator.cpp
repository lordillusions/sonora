// PLG-25a: the generator device base (devices/MidiGenerator), its state hand-off (PatternSlot) and the clip commands
// (core/GeneratorClip), proven with a test-only generator (TestGenerator.h).

#include "TestFramework.h"
#include "TestGenerator.h"

#include "core/Edit.h"
#include "core/GeneratorClip.h"
#include "devices/DeviceFactory.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <map>
#include <set>
#include <random>
#include <thread>

using namespace sonora;
using testkit::TestGenerator;

namespace
{
struct Event
{
    int64_t at;
    bool on;
    int pitch, velocity, channel;
};

struct Note
{
    int64_t start, length;
    int pitch, velocity;
};

// Drives a generator like the engine: host info at each block start (beats from the sample clock, wrapping at the loop
// end inside a block like the transport), then process(). `blocks` picks each block's size.
struct Rig
{
    TestGenerator gen;
    double rate = 48000, tempo = 138;
    bool playing = true;
    bool looping = false;
    double loopStart = 0, loopEnd = 0;
    double beat = 0;
    int64_t clock = 0;
    int maxBlock = 4096;
    std::function<int()> blocks = [] { return 512; };
    std::vector<std::pair<int64_t, juce::MidiMessage>> input;
    std::vector<Event> out;
    std::vector<juce::MidiMessage> other;
    int maxBytes = 0;

    Rig(double sampleRate = 48000, int largest = 4096) : rate(sampleRate), maxBlock(largest) { gen.prepare(rate, maxBlock); }
    double perBeat() const { return rate * 60.0 / tempo; }
    void press(int64_t at, int pitch, int velocity = 100) { input.push_back({at, juce::MidiMessage::noteOn(1, pitch, (juce::uint8) velocity)}); }
    void release(int64_t at, int pitch) { input.push_back({at, juce::MidiMessage::noteOff(1, pitch)}); }

    void run(int64_t until)
    {
        juce::AudioBuffer<float> audio(2, maxBlock);
        juce::MidiBuffer midi;
        midi.ensureSize(65536);
        while (clock < until)
        {
            const int n = (int) std::min<int64_t>(std::clamp(blocks(), 1, maxBlock), until - clock);
            juce::AudioBuffer<float> view(audio.getArrayOfWritePointers(), 2, n);
            midi.clear();
            for (const auto& [at, message] : input)
                if (at >= clock && at < clock + n) midi.addEvent(message, (int) (at - clock));
            devices::HostInfo info;
            info.tempo = tempo;
            info.beats = beat;
            info.playing = playing;
            info.sampleRate = rate;
            info.looping = looping;
            info.loopStart = loopStart;
            info.loopEnd = loopEnd;
            gen.setHostInfo(info);
            gen.process(view, midi);
            int bytes = 0;
            for (const auto meta : midi)
            {
                bytes += meta.numBytes;
                const auto m = meta.getMessage();
                if (m.isNoteOnOrOff())
                    out.push_back({clock + meta.samplePosition, m.isNoteOn(), m.getNoteNumber(), m.getVelocity(), m.getChannel()});
                else
                    other.push_back(m);
            }
            maxBytes = std::max(maxBytes, bytes);
            clock += n;
            if (playing)
            {
                beat += n / perBeat();
                if (looping && beat >= loopEnd - 1e-9) beat = loopStart + (beat - loopEnd);
            }
        }
    }
    void locate(double to) { beat = to; }

    // Notes paired from the output (on -> the next off of the same pitch/channel).
    std::vector<Note> notes() const
    {
        std::vector<Note> result;
        std::map<std::pair<int, int>, std::vector<size_t>> open;
        for (const auto& e : out)
        {
            const auto key = std::make_pair(e.channel, e.pitch);
            if (e.on)
            {
                open[key].push_back(result.size());
                result.push_back({e.at, -1, e.pitch, e.velocity});
            }
            else if (! open[key].empty())
            {
                result[open[key].front()].length = e.at - result[open[key].front()].start;
                open[key].erase(open[key].begin());
            }
        }
        return result;
    }
    // True when no pitch is ever switched on twice without an off in between and nothing sounds at the end.
    bool balanced(bool allowOrphanOffs = false) const
    {
        std::map<std::pair<int, int>, int> depth;
        for (const auto& e : out)
        {
            auto& d = depth[{e.channel, e.pitch}];
            if (e.on) { if (++d > 1) return false; }
            else if (--d < 0) { if (! allowOrphanOffs) return false; d = 0; }
        }
        for (const auto& [key, d] : depth)
            if (d != 0) return false;
        return true;
    }
};

bool sameNotes(const std::vector<Note>& a, const std::vector<Note>& b, int64_t tolerance = 1)
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::abs(a[i].start - b[i].start) > tolerance || a[i].pitch != b[i].pitch || a[i].velocity != b[i].velocity
            || std::abs(a[i].length - b[i].length) > 2 * tolerance)
            return false;
    return true;
}

std::vector<Note> from(const std::vector<Note>& notes, int64_t start, int64_t end = INT64_MAX)
{
    std::vector<Note> r;
    for (auto n : notes)
        if (n.start >= start && n.start < end)
        {
            n.start -= start;
            r.push_back(n);
        }
    return r;
}

void sortNotes(std::vector<Note>& notes)
{
    std::stable_sort(notes.begin(), notes.end(), [](const Note& a, const Note& b) { return a.start < b.start || (a.start == b.start && a.pitch < b.pitch); });
}
} // namespace

TEST_CASE("midi generator: same notes in one block, in random block sizes at 44.1 / 48 / 96 kHz")
{
    for (const double rate : {44100.0, 48000.0, 96000.0})
    {
        const auto bars16 = (int64_t) std::ceil(64 * rate * 60.0 / 138);
        auto render = [&](std::function<int()> sizes, int largest) {
            Rig rig(rate, largest);
            rig.blocks = std::move(sizes);
            rig.gen.setParameter(TestGenerator::Gate, 80);
            rig.gen.setParameter(TestGenerator::Swing, 0.4f);
            rig.press(0, 60);
            rig.press(0, 64);
            rig.press(0, 67);
            rig.press(bars16 / 2 + 333, 70); // a fourth note joins in the middle, at an odd sample
            rig.release(bars16 * 3 / 4 + 17, 64);
            for (int pitch : {60, 67, 70}) rig.release(bars16 - 1, pitch);
            rig.run(bars16);
            rig.playing = false; // stop: everything ends
            rig.run(bars16 + 20000);
            CHECK(rig.balanced());
            auto n = rig.notes();
            sortNotes(n);
            return n;
        };
        const auto whole = render([] { return 4096; }, 4096);
        std::mt19937 random(1234);
        const auto pieces = render([&] { return (int) (random() % 4096) + 1; }, 4096);
        const auto single = render([] { return 1; }, 1);
        CHECK(whole.size() > 150);
        CHECK(sameNotes(whole, pieces));
        CHECK(sameNotes(whole, single));
    }
}

TEST_CASE("midi generator: a locate gives the notes a continuous run plays there; the walk is fast-forwarded")
{
    const double perBeat = 48000.0 * 60.0 / 138;
    Rig continuous;
    continuous.gen.setParameter(TestGenerator::Fixed, 1);
    continuous.run((int64_t) (64 * perBeat));
    auto all = continuous.notes();
    // Jump into the middle of a cycle (beat 37.5: not on a cycle start) after playing a while elsewhere.
    Rig jumped;
    jumped.gen.setParameter(TestGenerator::Fixed, 1);
    jumped.run((int64_t) (10 * perBeat));
    const int64_t jumpClock = jumped.clock;
    jumped.locate(37.5);
    jumped.run(jumpClock + (int64_t) (26.5 * perBeat) - 2000);
    CHECK(jumped.gen.locates >= 2);
    CHECK_NEAR(jumped.gen.lastLocate, 37.5, 1e-9);
    // Notes after the jump, shifted to song time, equal the continuous run's notes from beat 37.5.
    const auto offset = (int64_t) std::floor(37.5 * perBeat + 1e-8);
    auto after = from(jumped.notes(), jumpClock);
    for (auto& n : after) n.start += offset;
    auto expected = from(all, offset, offset + (int64_t) (26.5 * perBeat) - 4000);
    for (auto& n : expected) n.start += offset;
    after.resize(std::min(after.size(), expected.size()));
    CHECK(expected.size() > 40);
    CHECK(sameNotes(after, expected));
    // The walk: pitches after the jump follow the continuous run's 0-1-2 pattern (not restarted at 0).
    CHECK(after.front().pitch == expected.front().pitch);
}

TEST_CASE("midi generator: loop wrap inside a block repeats the loop exactly and ends notes at the wrap")
{
    Rig rig;
    rig.blocks = [] { return 1000; };
    rig.looping = true;
    rig.loopStart = 4;
    rig.loopEnd = 12; // two bars
    rig.beat = 4;
    rig.gen.setParameter(TestGenerator::Fixed, 1);
    rig.gen.setParameter(TestGenerator::Gate, 300); // long notes cross the loop end
    const double perBeat = rig.perBeat();
    const auto lap = (int64_t) std::llround(8 * perBeat);
    rig.run(lap * 5);
    rig.gen.setParameter(TestGenerator::Fixed, 0); // nothing to play on the free clock after the stop
    rig.playing = false;
    rig.run(lap * 5 + 100);
    CHECK(rig.balanced());
    const auto notes = rig.notes();
    const auto first = from(notes, 0, lap - 2);
    CHECK(first.size() > 20);
    for (int k = 1; k < 4; ++k)
    {
        // Lap k starts where the loop wrapped: the sample of beat 12 after k laps.
        const auto start = (int64_t) std::ceil(8 * perBeat * k - 1e-8);
        const auto lapNotes = from(notes, start, start + lap - 2);
        CHECK(sameNotes(first, lapNotes, 2));
    }
    // No note sounds across a wrap.
    for (const auto& n : notes)
    {
        const double lapPosition = std::fmod((double) n.start, 8 * perBeat);
        CHECK(lapPosition + (double) n.length <= 8 * perBeat + 2);
    }
    CHECK(rig.gen.locates == 6); // the start and five wraps (the run ends a few samples past the fifth)
}

TEST_CASE("midi generator: stop, bypass, locate and retrigger leave no hung note")
{
    Rig rig;
    rig.blocks = [] { return 777; };
    rig.gen.setParameter(TestGenerator::Gate, 400); // every note overlaps the next steps: retriggers
    rig.press(0, 50);
    rig.press(0, 57);
    const double perBeat = rig.perBeat();
    rig.run((int64_t) (6.3 * perBeat));
    CHECK(rig.balanced() == false); // notes are sounding now
    rig.playing = false;           // stop mid-note
    rig.run(rig.clock + 2000);
    // Stopped with keys held: the free clock plays on; end it by bypassing.
    rig.gen.setBypassed(true);
    rig.run(rig.clock + 3000);
    rig.gen.setBypassed(false);
    rig.playing = true;
    rig.locate(20.0);
    rig.run(rig.clock + (int64_t) (3.1 * perBeat));
    rig.locate(2.0); // locate while notes sound
    rig.run(rig.clock + (int64_t) (2.2 * perBeat));
    rig.playing = false;
    rig.release(rig.clock + 10, 50);
    rig.release(rig.clock + 10, 57);
    rig.run(rig.clock + 20000);
    CHECK(rig.balanced(true));
    // Retrigger: a pitch is never switched on while it sounds.
    std::map<int, int> depth;
    bool doubled = false;
    for (const auto& e : rig.out)
        if (e.on) doubled |= ++depth[e.pitch] > 1;
        else depth[e.pitch] = std::max(0, depth[e.pitch] - 1);
    CHECK(! doubled);
}

TEST_CASE("midi generator: keys pressed while bypassed get their note-off on re-enable; other MIDI passes")
{
    Rig rig;
    rig.gen.setBypassed(true);
    rig.press(100, 62);
    rig.input.push_back({200, juce::MidiMessage::controllerEvent(1, 74, 90)});
    rig.run(1024);
    CHECK(rig.out.size() == 1 && rig.out[0].on && rig.out[0].pitch == 62); // passed through unchanged
    rig.gen.setBypassed(false);
    rig.run(2048);
    CHECK(rig.out.size() >= 2 && ! rig.out[1].on && rig.out[1].pitch == 62 && rig.out[1].at == 1024);
    CHECK(rig.other.size() == 1 && rig.other[0].getControllerNumber() == 74);
    // Not bypassed: input notes are consumed (only generated notes come out), controllers pass.
    Rig consume;
    consume.gen.pattern.set([] { testkit::TestPattern p; p.on.fill(0); return p; }()); // plays nothing
    consume.press(10, 60);
    consume.input.push_back({20, juce::MidiMessage::controllerEvent(1, 1, 5)});
    consume.run(4096);
    CHECK(consume.out.empty());
    CHECK(consume.other.size() == 1);
}

TEST_CASE("midi generator: the MIDI cap drops a flooded block, ends its notes and recovers")
{
    Rig rig(48000, 32768);
    rig.tempo = 300;
    rig.blocks = [] { return 32768; };
    rig.gen.setParameter(TestGenerator::Rate, model::kRate1_32T);
    rig.gen.setParameter(TestGenerator::Burst, 100);
    rig.gen.setParameter(TestGenerator::Fixed, 1);
    rig.gen.setParameter(TestGenerator::Gate, 400);
    for (int s = 0; s < 32768 * 3; s += 256) rig.input.push_back({s, juce::MidiMessage::controllerEvent(1, 2, 1)});
    rig.run(32768);
    CHECK(rig.gen.droppedEvents() > 0);
    CHECK(rig.maxBytes <= devices::MidiGenerator::kByteLimit + 8192);
    for (const auto& e : rig.out) CHECK(! e.on); // the flooded block sent only note-offs
    // Back to sane settings: the next blocks play normally and everything ends at the stop.
    rig.gen.setParameter(TestGenerator::Burst, 0);
    rig.run(32768 * 2);
    size_t ons = 0;
    for (const auto& e : rig.out) ons += e.on;
    CHECK(ons > 10);
    rig.gen.setParameter(TestGenerator::Fixed, 0);
    rig.playing = false;
    rig.run(32768 * 3);
    std::map<int, int> depth;
    for (const auto& e : rig.out) depth[e.pitch] = e.on ? depth[e.pitch] + 1 : std::max(0, depth[e.pitch] - 1);
    for (const auto& [pitch, d] : depth) CHECK(d == 0);
}

TEST_CASE("midi generator: free clock when stopped starts at the first key and restarts after release")
{
    Rig rig;
    rig.playing = false;
    rig.gen.setParameter(TestGenerator::Gate, 50);
    rig.press(1000, 60);
    rig.release(30000, 60);
    rig.press(40000, 60);
    rig.release(52000, 60);
    rig.run(60000);
    CHECK(rig.balanced());
    const auto notes = rig.notes();
    REQUIRE(notes.size() >= 4);
    CHECK(notes[0].start == 1000);
    const double perBeat = rig.perBeat();
    CHECK(notes[1].start == 1000 + (int64_t) std::floor(0.25 * perBeat + 1e-8));
    bool restarted = false;
    for (const auto& n : notes) restarted |= n.start == 40000;
    CHECK(restarted);
    for (const auto& n : notes) CHECK(n.start < 30000 || n.start >= 40000);
    CHECK(rig.gen.locates == 2);
}

TEST_CASE("midi generator: latch keeps released keys; a new gesture replaces them")
{
    Rig rig;
    rig.gen.setParameter(TestGenerator::Latch, 1);
    rig.press(0, 60);
    rig.press(0, 64);
    rig.release(100, 60);
    rig.release(100, 64);
    rig.press(20000, 72); // new gesture: replaces both
    rig.release(20100, 72);
    const double perBeat = rig.perBeat();
    rig.run((int64_t) (4 * perBeat));
    std::set<int> early, late;
    for (const auto& n : rig.notes()) (n.start < 20000 ? early : late).insert(n.pitch);
    CHECK(early.count(60) + early.count(61) + early.count(62) > 0);
    CHECK(early.count(64) + early.count(65) + early.count(66) > 0);
    for (int p : late) CHECK(p >= 72);
    rig.gen.setParameter(TestGenerator::Latch, 0); // latch off drops the latched key
    const auto count = rig.out.size();
    rig.run(rig.clock + (int64_t) (2 * perBeat));
    size_t ons = 0;
    for (size_t i = count; i < rig.out.size(); ++i) ons += rig.out[i].on;
    CHECK(ons == 0);
}

TEST_CASE("midi generator: renderNotes equals what process() plays, in exact beats")
{
    TestGenerator gen;
    gen.setParameter(TestGenerator::Gate, 70);
    gen.setParameter(TestGenerator::Swing, 0.5f);
    auto p = gen.pattern.edited();
    p.transpose = 2;
    gen.pattern.set(p);
    std::vector<model::MidiNote> input(2);
    input[0].start = 8; input[0].duration = 16; input[0].pitch = 57;
    input[1].start = 10; input[1].duration = 4; input[1].pitch = 64;
    const auto rendered = gen.renderNotes(input, 8, 24, 138);
    CHECK(rendered.size() > 30);
    for (const auto& n : rendered)
    {
        // Exact grid beats (swung odd sixteenths), not sample positions.
        const double step = n.start / 0.25;
        const double frac = step - std::floor(step + 1e-9);
        CHECK(frac < 1e-9 || std::abs(frac - 0.5 / 3) < 1e-9);
        CHECK(n.start >= 8 && n.start + n.duration <= 24 + 1e-9);
    }
    // The same through process(), with the input as MIDI at its samples.
    Rig rig;
    rig.gen.setParameter(TestGenerator::Gate, 70);
    rig.gen.setParameter(TestGenerator::Swing, 0.5f);
    rig.gen.pattern.set(p);
    rig.beat = 8;
    const double perBeat = rig.perBeat();
    rig.press(0, 57);
    rig.press((int64_t) std::floor(2 * perBeat + 1e-8), 64);
    rig.release((int64_t) std::floor(6 * perBeat + 1e-8), 64);
    rig.run((int64_t) std::ceil(16 * perBeat));
    rig.playing = false; // the stop ends the last note
    rig.release(rig.clock, 57);
    rig.run(rig.clock + 4096);
    auto played = rig.notes();
    sortNotes(played);
    played.erase(std::remove_if(played.begin(), played.end(), [](const Note& n) { return n.length < 0; }), played.end());
    // The run's last block reaches a fraction of a sample past beat 24: a step exactly there is the next span's.
    played.erase(std::remove_if(played.begin(), played.end(), [&](const Note& n) { return (double) n.start >= 16 * perBeat - 1; }), played.end());
    REQUIRE(played.size() == rendered.size());
    for (size_t i = 0; i < played.size(); ++i)
    {
        CHECK(std::llabs(played[i].start - (int64_t) std::floor((rendered[i].start - 8) * perBeat + 1e-8)) <= 1);
        CHECK(played[i].pitch == rendered[i].pitch);
        CHECK(played[i].velocity == (int) rendered[i].velocity);
    }
    // The running instance was not touched (renderNotes works on a clone).
    CHECK(gen.locates == 0);
}

TEST_CASE("pattern slot: the audio side takes the newest published copy, wait-free and whole")
{
    struct Big
    {
        std::array<int, 512> values{};
    };
    devices::PatternSlot<Big> slot;
    CHECK(! slot.update());
    auto& e = slot.edit();
    e.values.fill(3);
    CHECK(slot.live().values[0] == 0); // not published yet
    slot.publish();
    CHECK(slot.update());
    CHECK(slot.live().values[511] == 3);
    CHECK(! slot.update());
    slot.edit().values.fill(4);
    slot.publish();
    slot.edit().values.fill(5);
    slot.publish();
    CHECK(slot.update());
    CHECK(slot.live().values[100] == 5); // several publishes: the last one
    // A writer thread publishing while the reader takes copies: every copy is whole (all values equal).
    std::atomic<bool> stop{false};
    std::thread writer([&] {
        for (int i = 6; ! stop.load(); ++i)
        {
            slot.edit().values.fill(i);
            slot.publish();
        }
    });
    bool whole = true;
    int seen = 0, last = 0;
    for (int i = 0; i < 200000; ++i)
        if (slot.update())
        {
            const auto& v = slot.live().values;
            for (int x : v) whole &= x == v[0];
            whole &= v[0] >= last;
            last = v[0];
            ++seen;
        }
    stop = true;
    writer.join();
    CHECK(whole);
    CHECK(seen > 0);
}

TEST_CASE("midi generator: state round trip, clone for render and the app's native content")
{
    TestGenerator gen;
    auto p = gen.pattern.edited();
    p.on.fill(0);
    p.on[3] = 1;
    p.transpose = -5;
    gen.pattern.set(p);
    gen.setParameter(TestGenerator::Seed, 99);
    const auto clone = gen.cloneForRender();
    auto* copy = dynamic_cast<TestGenerator*>(clone.get());
    REQUIRE(copy != nullptr);
    CHECK(copy->pattern.edited().transpose == -5);
    CHECK(copy->pattern.edited().on[3] == 1 && copy->pattern.edited().on[0] == 0);
    CHECK_NEAR(copy->getParameter(TestGenerator::Seed), 99.0f, 1e-6);
    std::string error;
    const std::vector<uint8_t> truncated{1, 2, 3};
    CHECK(! copy->loadState(truncated.data(), truncated.size(), error));
    CHECK(! error.empty());
    // The app saves the state as native content and restores it into a new instance.
    model::Device device;
    device.type = "TestGenerator";
    devices::captureDeviceState(device, &gen);
    REQUIRE(device.nativeContent != nullptr);
    CHECK(device.nativeContent->format == "sonora-test-generator-v1");
    TestGenerator restored;
    std::vector<std::string> warnings;
    devices::restoreGeneratorState(restored, device, &warnings);
    CHECK(warnings.empty());
    CHECK(restored.pattern.edited().transpose == -5);
    // Saving twice without a change keeps the same content object (no needless dirty set).
    const auto before = device.nativeContent;
    devices::captureDeviceState(device, &gen);
    CHECK(device.nativeContent == before);
    // A wrong format is reported, never fatal.
    auto other = std::make_shared<model::NativeContent>();
    other->format = "sonora-test-generator-v1";
    other->data = {9};
    device.nativeContent = other;
    devices::restoreGeneratorState(restored, device, &warnings);
    CHECK(warnings.size() == 1);
}

// ------------------------------------------------------------------------------------------------- clip commands
namespace
{
model::GeneratorClipSpec specFor(const TestGenerator& gen, bool usesInput, double tempo)
{
    model::GeneratorClipSpec spec;
    spec.deviceName = "Test Generator";
    spec.usesInput = usesInput;
    spec.render = [&gen, tempo](const std::vector<model::MidiNote>& input, double from, double to) {
        return gen.renderNotes(input, from, to, tempo);
    };
    return spec;
}

model::Song songWithClip()
{
    model::Song song;
    song.tempo = 138;
    song.midiClipTemplate = std::make_shared<std::string>("<MidiClip/>");
    song.tracks.resize(2);
    song.tracks[0].id = 1;
    song.tracks[0].kind = model::TrackKind::Midi;
    song.tracks[1].id = 2;
    song.tracks[1].kind = model::TrackKind::Audio;
    model::Clip clip;
    clip.uid = 5;
    clip.kind = model::Clip::Kind::Midi;
    clip.currentStart = clip.time = 16;
    clip.currentEnd = 24;
    clip.loop = {0, 8, 0, true};
    model::MidiNote chord;
    chord.duration = 8;
    for (int pitch : {57, 60, 64})
    {
        chord.pitch = pitch;
        clip.notes.push_back(chord);
    }
    model::MidiNote after; // outside the loop span: stays
    after.start = 9;
    after.pitch = 40;
    clip.notes.push_back(after);
    song.tracks[0].arrangementClips.push_back(clip);
    return song;
}
} // namespace

TEST_CASE("generator clip: Apply to Clip writes what the device plays over the clip, one undo step")
{
    auto song = songWithClip();
    TestGenerator gen;
    gen.setParameter(TestGenerator::Gate, 60);
    std::string message;
    const auto location = model::ClipLocation::arrangement(0, 5);
    auto command = model::applyGeneratorToClip(song, location, specFor(gen, true, song.tempo), message);
    REQUIRE(static_cast<bool>(command.apply));
    CHECK(command.label == "Apply Test Generator to Clip");
    model::EditHistory history;
    const auto original = model::findClip(song, location)->notes;
    history.perform(song, std::move(command));
    const auto* clip = model::findClip(song, location);
    REQUIRE(clip != nullptr);
    // Equal to the device's output over song beats 16..24 with the clip's chord held, moved into content time.
    std::vector<model::MidiNote> held;
    for (int pitch : {57, 60, 64})
    {
        model::MidiNote n;
        n.start = 16;
        n.duration = 8;
        n.pitch = pitch;
        held.push_back(n);
    }
    const auto expected = gen.renderNotes(held, 16, 24, song.tempo);
    std::vector<model::MidiNote> inside;
    bool kept = false;
    for (const auto& n : clip->notes)
    {
        if (n.pitch == 40 && std::abs(n.start - 9) < 1e-9) { kept = true; continue; }
        inside.push_back(n);
    }
    CHECK(kept);
    REQUIRE(inside.size() == expected.size());
    for (size_t i = 0; i < inside.size(); ++i)
    {
        CHECK_NEAR(inside[i].start, expected[i].start - 16, 1e-9);
        CHECK_NEAR(inside[i].duration, expected[i].duration, 1e-9);
        CHECK(inside[i].pitch == expected[i].pitch);
    }
    CHECK(message.find("wrote") != std::string::npos);
    history.undo(song);
    CHECK(model::findClip(song, location)->notes.size() == original.size());
    // A clip without notes for a generator that needs input: no notes, said so.
    auto empty = songWithClip();
    empty.tracks[0].arrangementClips[0].notes.clear();
    auto none = model::applyGeneratorToClip(empty, location, specFor(gen, true, song.tempo), message);
    CHECK(message.find("needs notes") != std::string::npos);
    // Audio clips are refused.
    auto audio = songWithClip();
    audio.tracks[0].arrangementClips[0].kind = model::Clip::Kind::Audio;
    CHECK(! model::applyGeneratorToClip(audio, location, specFor(gen, true, song.tempo), message).apply);
}

TEST_CASE("generator clip: a Session clip renders from song beat 0; a new clip at the loop or the playhead's bar")
{
    auto song = songWithClip();
    TestGenerator gen;
    gen.setParameter(TestGenerator::Fixed, 1);
    model::Clip clip = song.tracks[0].arrangementClips[0];
    clip.notes.clear();
    clip.loop = {4, 12, 0, true};
    song.tracks[0].slots.resize(2);
    song.tracks[0].slots[1].clip = clip;
    std::string message;
    const auto location = model::ClipLocation::session(0, 5, 1);
    CHECK_NEAR(model::generatorClipOrigin(clip, location), 0.0, 1e-12);
    auto command = model::applyGeneratorToClip(song, location, specFor(gen, false, song.tempo), message);
    REQUIRE(static_cast<bool>(command.apply));
    command.apply(song);
    const auto expected = gen.renderNotes({}, 0, 8, song.tempo);
    const auto* written = model::findClip(song, location);
    REQUIRE(written != nullptr);
    REQUIRE(written->notes.size() == expected.size());
    CHECK_NEAR(written->notes.front().start, 4.0, 1e-9); // content time starts at the loop start

    // Insert: a fresh clip, refused where a clip is in the way and on audio tracks.
    CHECK_NEAR(model::generatorClipStart(song, 13.5, false, 0), 12.0, 1e-12);
    CHECK_NEAR(model::generatorClipStart(song, 13.5, true, 32), 32.0, 1e-12);
    auto insert = model::insertGeneratorClip(song, 0, 32, 8, specFor(gen, false, song.tempo), message);
    REQUIRE(static_cast<bool>(insert.apply));
    model::EditHistory history;
    history.perform(song, std::move(insert));
    REQUIRE(song.tracks[0].arrangementClips.size() == 2);
    const auto& fresh = song.tracks[0].arrangementClips.back();
    CHECK(fresh.name == "Test Generator");
    const auto freshExpected = gen.renderNotes({}, 32, 40, song.tempo);
    REQUIRE(fresh.notes.size() == freshExpected.size());
    CHECK_NEAR(fresh.notes.front().start, freshExpected.front().start - 32, 1e-9);
    CHECK(! model::insertGeneratorClip(song, 0, 18, 8, specFor(gen, false, song.tempo), message).apply);
    CHECK(message.find("in the way") != std::string::npos);
    CHECK(! model::insertGeneratorClip(song, 1, 64, 8, specFor(gen, false, song.tempo), message).apply);
    history.undo(song);
    CHECK(song.tracks[0].arrangementClips.size() == 1);
}
