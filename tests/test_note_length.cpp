#include "TestFramework.h"

#include "devices/DeviceFactory.h"
#include "devices/native/NoteLength.h"
#include "io/als/AlsTemplates.h"

using namespace sonora;
using N = devices::NoteLength;

namespace
{
struct Out
{
    int64_t at;
    bool on;
    int pitch, velocity;
};

// Feeds timed messages through a NoteLength in fixed blocks (48 kHz, 120 BPM) and records the output.
struct Rig
{
    N device;
    int block;
    int64_t clock = 0;
    std::vector<std::pair<int64_t, juce::MidiMessage>> input;
    std::vector<Out> out;
    std::vector<juce::MidiMessage> other;
    explicit Rig(int blockSize = 512) : block(blockSize) { device.prepare(48000, blockSize); }
    void on(int64_t at, int pitch, int velocity = 100) { input.push_back({at, juce::MidiMessage::noteOn(1, pitch, (juce::uint8) velocity)}); }
    void off(int64_t at, int pitch, int velocity = 64) { input.push_back({at, juce::MidiMessage::noteOff(1, pitch, (juce::uint8) velocity)}); }
    void run(int64_t until)
    {
        juce::AudioBuffer<float> audio(2, block);
        juce::MidiBuffer midi;
        while (clock < until)
        {
            const int n = (int) std::min<int64_t>(block, until - clock);
            juce::AudioBuffer<float> view(audio.getArrayOfWritePointers(), 2, n);
            midi.clear();
            for (const auto& [at, m] : input) if (at >= clock && at < clock + n) midi.addEvent(m, (int) (at - clock));
            device.setHostInfo({120.0, 0.0, true, 48000.0});
            device.process(view, midi);
            for (const auto meta : midi)
            {
                const auto m = meta.getMessage();
                if (m.isNoteOnOrOff()) out.push_back({clock + meta.samplePosition, m.isNoteOn(), m.getNoteNumber(), m.getVelocity()});
                else other.push_back(m);
            }
            clock += n;
        }
    }
};
} // namespace

TEST_CASE("note length: Note On trigger ends notes after Length x Gate, ignoring the played note-offs")
{
    for (int block : {512, 100})
    {
        Rig rig(block);
        rig.device.setParameter(N::TimeLength, 250); // ms = 12000 samples
        rig.on(1000, 60); rig.off(1100, 60);        // a short tap is stretched
        rig.on(20000, 62); rig.off(40000, 62);      // a long press is shortened
        rig.run(48000);
        REQUIRE(rig.out.size() == 4);
        CHECK(rig.out[0].on && rig.out[0].at == 1000);
        CHECK(! rig.out[1].on && rig.out[1].at == 13000);
        CHECK(rig.out[2].on && rig.out[2].at == 20000);
        CHECK(! rig.out[3].on && rig.out[3].at == 32000);
    }

    Rig synced;
    synced.device.setParameter(N::Sync, 1);
    synced.device.setParameter(N::SyncedLength, 5); // 1/16 note at 120 BPM = 6000 samples
    synced.device.setParameter(N::Gate, 200);
    synced.on(0, 60);
    synced.run(24000);
    REQUIRE(synced.out.size() == 2);
    CHECK(synced.out[1].at == 12000);
    // The reference's labels (Live 12.4.6): 0, 1/64, 1/48, 1/32, 1/24, 1/16, 1/12, 1/8, 1/6, 1/4 note.
    CHECK_NEAR(N::syncedLengthBeats(0), 0.0, 1e-12);
    CHECK_NEAR(N::syncedLengthBeats(1), 1.0 / 16, 1e-12);
    CHECK_NEAR(N::syncedLengthBeats(4), 1.0 / 6, 1e-12); // untouched default: 1/24
    CHECK_NEAR(N::syncedLengthBeats(7), 0.5, 1e-12);     // 1/8
    CHECK_NEAR(N::syncedLengthBeats(9), 1.0, 1e-12);     // 1/4 (measured: 0.5 s at 120 BPM)

    Rig zero; // "0" plays nothing and swallows the played note-off
    zero.device.setParameter(N::Sync, 1);
    zero.device.setParameter(N::SyncedLength, 0);
    zero.on(0, 60); zero.off(4800, 60);
    zero.run(24000);
    CHECK(zero.out.empty());

    Rig keyed; // Key Scale does not change the length (measured at five octaves in both length modes)
    keyed.device.setParameter(N::TimeLength, 100);
    keyed.device.setParameter(N::KeyScale, 1);
    keyed.on(0, 36); keyed.on(0, 84);
    keyed.run(24000);
    REQUIRE(keyed.out.size() == 4);
    CHECK(keyed.out[2].at == 4800 && keyed.out[3].at == 4800);
}

TEST_CASE("note length: Latch holds notes until the next note-on; a retriggered pitch ends first")
{
    Rig rig;
    rig.device.setParameter(N::Latch, 1);
    rig.on(0, 60); rig.off(100, 60);
    rig.on(30000, 64); rig.off(30100, 64);
    rig.run(48000);
    REQUIRE(rig.out.size() == 3);
    CHECK(rig.out[0].on && rig.out[0].pitch == 60);
    CHECK(! rig.out[1].on && rig.out[1].pitch == 60 && rig.out[1].at == 30000);
    CHECK(rig.out[2].on && rig.out[2].pitch == 64 && rig.out[2].at == 30000);

    Rig repeat;
    repeat.device.setParameter(N::TimeLength, 1000);
    repeat.on(0, 60); repeat.on(4800, 60);
    repeat.run(9600);
    REQUIRE(repeat.out.size() == 3);
    CHECK(! repeat.out[1].on && repeat.out[1].at == 4800); // ended before it starts again
    CHECK(repeat.out[2].on && repeat.out[2].at == 4800);
}

TEST_CASE("note length: Note Off trigger starts notes on release with decayed, blended velocity")
{
    Rig rig;
    rig.device.setParameter(N::Trigger, 1);
    rig.device.setParameter(N::TimeLength, 100);
    rig.device.setParameter(N::DecayTime, 1000);
    rig.device.setParameter(N::ReleaseVelocity, 0);
    rig.on(0, 64, 100); rig.off(6000, 64, 20); // held 125 ms of a 1000 ms decay: 100 e^(-9.7 / 8) = 30
    rig.on(30000, 62, 100); rig.off(80000, 62); // held past the decay: still plays, at velocity 1
    rig.run(96000);
    REQUIRE(rig.out.size() == 4);
    CHECK(rig.out[0].on && rig.out[0].at == 6000 && rig.out[0].velocity == 30);
    CHECK(! rig.out[1].on && rig.out[1].at == 6000 + 4800);
    CHECK(rig.out[2].on && rig.out[2].at == 80000 && rig.out[2].velocity == 1);

    // The decay law against the reference's exported levels (DEV-21c): exponential, Key Scale around E3 (64).
    CHECK_NEAR(N::decayedVelocity(127, 500, 60, 2000, 0), 11.25, 0.5); // measured velocities (0.1 dB = 0.25)
    CHECK_NEAR(N::decayedVelocity(100, 125, 48, 1000, 0), 29.5, 0.5);
    CHECK_NEAR(N::decayedVelocity(100, 500, 48, 4000, 1), 22.25, 0.5); // Key Scale 1: lower keys decay faster
    CHECK_NEAR(N::decayedVelocity(100, 500, 84, 4000, 1), 43.5, 0.5);
    CHECK_NEAR(N::decayedVelocity(100, 500, 36, 4000, -1), 50.5, 0.5);
    CHECK_NEAR(N::decayedVelocity(100, 500, 84, 4000, -1), 20.25, 0.5);
    CHECK_NEAR(N::decayedVelocity(100, 500, 64, 4000, 1), N::decayedVelocity(100, 500, 64, 4000, 0), 1e-9);

    Rig blend;
    blend.device.setParameter(N::Trigger, 1);
    blend.device.setParameter(N::ReleaseVelocity, 50);
    blend.on(0, 60, 100); blend.off(10, 60, 20);
    blend.run(1000);
    REQUIRE(! blend.out.empty());
    CHECK(blend.out[0].velocity == 60); // (100 + 20) / 2

    Rig latched; // Note Off + Latch: the chord starts once every key is up, and stays until the next release
    latched.device.setParameter(N::Trigger, 1);
    latched.device.setParameter(N::Latch, 1);
    latched.on(0, 60); latched.on(0, 64);
    latched.off(1000, 60); latched.off(2000, 64);
    latched.on(10000, 67); latched.off(11000, 67);
    latched.run(48000);
    REQUIRE(latched.out.size() == 5);
    CHECK(latched.out[0].on && latched.out[0].at == 2000 && latched.out[1].on && latched.out[1].at == 2000);
    CHECK(! latched.out[2].on && latched.out[2].at == 11000);
    CHECK(! latched.out[3].on && latched.out[3].at == 11000);
    CHECK(latched.out[4].on && latched.out[4].pitch == 67 && latched.out[4].at == 11000);
}

TEST_CASE("note length: All Notes Off, foreign note-offs, bypass and the saved element names")
{
    Rig rig;
    rig.device.setParameter(N::TimeLength, 1000);
    rig.on(0, 60);
    rig.input.push_back({100, juce::MidiMessage::allNotesOff(1)});
    rig.off(200, 70); // never started here: passes through
    rig.run(48000);
    REQUIRE(rig.out.size() == 3);
    CHECK(! rig.out[1].on && rig.out[1].pitch == 60 && rig.out[1].at == 100);
    CHECK(! rig.out[2].on && rig.out[2].pitch == 70);
    CHECK(rig.other.size() == 1);

    Rig bypass;
    bypass.device.setBypassed(true);
    bypass.on(0, 60); bypass.off(5, 60);
    bypass.run(512);
    REQUIRE(bypass.out.size() == 2);
    CHECK(bypass.out[1].at == 5);

    auto model = devices::nativeDeviceModel("MidiNoteLength");
    CHECK(model.parameters.size() == (size_t) N::NumParams);
    auto xml = juce::XmlDocument::parse(juce::String(io::deviceTemplate(model)));
    REQUIRE(xml != nullptr);
    CHECK(xml->getTagName() == "MidiNoteLength");
    CHECK(xml->getChildByName("Mode")->getChildByName("Manual")->getStringAttribute("Value") == "false");
    for (const char* tag : {"Latch", "SyncState", "SyncedLength", "TimeLength", "Gate", "Balance", "DecayTime", "DecayKeyScale"})
        CHECK(xml->getChildByName(tag) != nullptr);
    model.parameters[(size_t) N::Trigger].value = 1;
    auto device = devices::createNative(model);
    REQUIRE(device != nullptr);
    CHECK(device->name() == "Note Length");
    CHECK(device->getParameter(N::Trigger) > 0.5f);
}
