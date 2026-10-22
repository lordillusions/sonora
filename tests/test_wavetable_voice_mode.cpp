#include "TestFramework.h"
#include "devices/native/WavetableVoiceMode.h"

#include <algorithm>
#include <array>
#include <limits>
#include <type_traits>
#include <vector>

using VoiceMode = sonora::devices::WavetableVoiceMode;

namespace
{
VoiceMode::Settings glidingSettings(bool legato = false, bool always = false, bool scaled = false)
{
    VoiceMode::Settings result;
    result.legato = legato;
    result.portaAlways = always;
    result.portaScaled = scaled;
    result.portaSeconds = 0.01;
    return result;
}

void finishGlide(VoiceMode& mode)
{
    while (mode.gliding()) mode.nextPitch();
}
}

TEST_CASE("wavetable voice mode last priority retriggers and falls back")
{
    VoiceMode mode;
    mode.prepare(48000);
    const auto first = mode.noteOn(0, 60, 92);
    CHECK(first.retrigger && first.identityChanged && ! first.legato && ! first.portamento);
    CHECK(first.after.note == 60 && first.after.velocity == 92 && first.after.channel == 0);
    CHECK_NEAR(mode.currentPitch(), 60, 0);
    const auto second = mode.noteOn(0, 67, 45);
    CHECK(second.retrigger && second.before.note == 60 && second.after.note == 67);
    const auto third = mode.noteOn(0, 64, 108);
    CHECK(third.after.note == 64 && third.after.velocity == 108);
    CHECK(mode.heldCount(0) == 3);
    const auto unselectedOff = mode.noteOff(0, 60);
    CHECK(! unselectedOff.changed && mode.selected().note == 64);
    const auto fallback = mode.noteOff(0, 64);
    CHECK(fallback.retrigger && fallback.after.note == 67 && ! fallback.release);
    const auto release = mode.noteOff(0, 67);
    CHECK(release.changed && release.release && ! release.after.active() && ! release.retrigger);
    CHECK(mode.heldCount(0) == 0);
    CHECK_NEAR(mode.nextPitch(), 67, 0);
}

TEST_CASE("wavetable voice mode legato controls retrigger independently from glide")
{
    VoiceMode mode;
    mode.prepare(48000);
    mode.configure(glidingSettings(true));
    CHECK(mode.noteOn(0, 60, 100).retrigger);
    const auto overlap = mode.noteOn(0, 72, 100);
    CHECK(overlap.legato && ! overlap.retrigger && overlap.portamento);
    CHECK(mode.glideRemainingSamples() == 480);
    finishGlide(mode);
    const auto fallback = mode.noteOff(0, 72);
    CHECK(fallback.legato && ! fallback.retrigger && fallback.portamento && fallback.after.note == 60);
    finishGlide(mode);
    CHECK(mode.noteOff(0, 60).release);
    const auto disconnected = mode.noteOn(0, 67, 100);
    CHECK(disconnected.retrigger && ! disconnected.legato && ! disconnected.portamento);
    mode.configure(glidingSettings(false));
    const auto nonLegato = mode.noteOn(0, 74, 100);
    CHECK(nonLegato.retrigger && ! nonLegato.legato && nonLegato.portamento);
}

TEST_CASE("wavetable voice mode duplicate releases are FIFO without losing velocity")
{
    VoiceMode mode;
    mode.prepare(48000);
    const auto a = mode.noteOn(0, 60, 23).after;
    const auto b = mode.noteOn(0, 60, 104).after;
    const auto c = mode.noteOn(0, 60, 67).after;
    CHECK(a.age < b.age && b.age < c.age);
    CHECK(mode.heldCount(0) == 3);
    CHECK(! mode.noteOff(0, 60).changed);
    CHECK(mode.heldCount(0) == 2 && mode.selected().age == c.age && mode.selected().velocity == 67);
    CHECK(! mode.noteOff(0, 60).changed);
    CHECK(mode.heldCount(0) == 1 && mode.selected().age == c.age);
    CHECK(mode.noteOff(0, 60).release);
    CHECK(! mode.noteOff(0, 60).changed);
    mode.noteOn(0, 60, 91);
    CHECK(mode.noteOn(0, 60, 0).release);
}

TEST_CASE("wavetable voice mode sustain retains notes and reset lifts released latches")
{
    VoiceMode mode;
    mode.prepare(48000);
    mode.configure(glidingSettings(true));
    mode.noteOn(0, 60, 81);
    mode.noteOn(0, 72, 90);
    mode.sustain(0, true);
    const auto latched = mode.noteOff(0, 72);
    CHECK(latched.changed && ! latched.identityChanged && ! latched.retrigger && ! latched.release);
    CHECK(mode.selected().note == 72 && ! mode.selected().keyDown);
    CHECK(mode.heldCount(0) == 2 && mode.physicalHeldCount(0) == 1);
    const auto fallback = mode.resetControllers(0);
    CHECK(fallback.identityChanged && fallback.legato && fallback.after.note == 60);
    CHECK(! mode.sustainDown(0) && mode.heldCount(0) == 1 && mode.physicalHeldCount(0) == 1);
    CHECK(mode.noteOff(0, 60).release);
    mode.sustain(0, true);
    mode.noteOn(0, 60, 20);
    mode.noteOn(0, 60, 100);
    mode.noteOff(0, 60);
    CHECK(mode.heldCount(0) == 2 && mode.physicalHeldCount(0) == 1);
    mode.noteOff(0, 60);
    CHECK(mode.heldCount(0) == 2 && mode.physicalHeldCount(0) == 0);
    CHECK(mode.sustain(0, false).release);
    CHECK(mode.heldCount(0) == 0);
}

TEST_CASE("wavetable voice mode all-off messages stay channel scoped")
{
    VoiceMode mode;
    mode.prepare(48000);
    mode.configure(glidingSettings(true));
    mode.noteOn(0, 60, 100);
    mode.noteOn(1, 72, 100);
    mode.sustain(1, true);
    const auto notesOff = mode.allNotesOff(1);
    CHECK(notesOff.changed && ! notesOff.identityChanged && ! notesOff.release);
    CHECK(mode.heldCount(1) == 1 && mode.physicalHeldCount(1) == 0 && mode.selected().channel == 1);
    const auto soundOff = mode.allSoundOff(1);
    CHECK(soundOff.identityChanged && soundOff.after.channel == 0 && soundOff.legato);
    CHECK(mode.heldCount(1) == 0 && mode.sustainDown(1));
    CHECK(mode.allNotesOff(0).release);
    CHECK(mode.heldCount(0) == 0);
    mode.noteOn(1, 74, 100);
    mode.noteOff(1, 74);
    CHECK(mode.selected().active());
    CHECK(mode.resetControllers(1).release);
    CHECK(! mode.sustainDown(1));
}

TEST_CASE("wavetable voice mode global and channel priorities are explicit")
{
    VoiceMode mode;
    mode.prepare(48000);
    mode.noteOn(0, 55, 10);
    mode.noteOn(1, 70, 20);
    mode.noteOn(0, 61, 30);
    CHECK(mode.selected().note == 61 && mode.selection(1).note == 70);
    auto settings = glidingSettings(true);
    settings.priority = VoiceMode::Priority::Low;
    const auto low = mode.configure(settings);
    CHECK(low.after.note == 55 && low.legato && low.portamento);
    CHECK(mode.selection(0).note == 55);
    mode.noteOn(2, 55, 90);
    CHECK(mode.selected().channel == 2 && mode.selected().velocity == 90);
    settings.priority = VoiceMode::Priority::High;
    CHECK(mode.configure(settings).after.note == 70);
    CHECK(mode.selection(0).note == 61);
    CHECK(mode.noteOff(1, 70).after.note == 61);
    settings.priority = VoiceMode::Priority::Last;
    CHECK(mode.configure(settings).after.channel == 2);
}

TEST_CASE("wavetable voice mode Always uses last audible pitch across idle")
{
    VoiceMode mode;
    mode.prepare(48000);
    mode.configure(glidingSettings(true, true));
    CHECK(! mode.noteOn(0, 60, 100).portamento);
    const auto overlap = mode.noteOn(0, 72, 100);
    CHECK(overlap.portamento);
    for (int i = 0; i < 120; ++i) mode.nextPitch();
    const double intermediate = mode.currentPitch();
    CHECK_NEAR(intermediate, 63, 1e-12);
    CHECK(mode.allSoundOff(0).release);
    CHECK(! mode.gliding());
    for (int i = 0; i < 1000; ++i) CHECK_NEAR(mode.nextPitch(), intermediate, 0);
    const auto afterIdle = mode.noteOn(0, 48, 100);
    CHECK(afterIdle.portamento && afterIdle.retrigger && ! afterIdle.legato);
    CHECK_NEAR(mode.currentPitch(), intermediate, 0);
    finishGlide(mode);
    CHECK_NEAR(mode.currentPitch(), 48, 0);
    mode.reset();
    CHECK(! mode.noteOn(0, 84, 100).portamento);
    CHECK_NEAR(mode.currentPitch(), 84, 0);
}

TEST_CASE("wavetable voice mode scaled time uses interval and retarget is continuous")
{
    VoiceMode mode;
    mode.prepare(48000);
    mode.configure(glidingSettings(true, false, true));
    mode.noteOn(0, 60, 100);
    CHECK(mode.noteOn(0, 66, 100).portamento);
    CHECK(mode.glideRemainingSamples() == 240);
    for (int i = 0; i < 120; ++i) mode.nextPitch();
    CHECK_NEAR(mode.currentPitch(), 63, 1e-12);
    CHECK(mode.noteOn(0, 87, 100).portamento);
    CHECK_NEAR(mode.currentPitch(), 63, 1e-12);
    CHECK(mode.glideRemainingSamples() == 960);
    finishGlide(mode);
    CHECK_NEAR(mode.currentPitch(), 87, 0);
    CHECK(mode.noteOff(0, 87).after.note == 66);
    CHECK(mode.glideRemainingSamples() == 840);
    finishGlide(mode);
    CHECK_NEAR(mode.currentPitch(), 66, 0);
}

TEST_CASE("wavetable voice mode cached glide settings survive block snapshots")
{
    VoiceMode mode;
    mode.prepare(48000);
    mode.configure(glidingSettings(true));
    mode.noteOn(0, 60, 100);
    mode.noteOn(0, 72, 100);
    for (int i = 0; i < 120; ++i) mode.nextPitch();
    auto settings = glidingSettings(true);
    settings.portaSeconds = 4;
    settings.portaCurve = 100;
    const auto unchanged = mode.configure(settings);
    CHECK(! unchanged.changed && mode.glideRemainingSamples() == 360);
    for (int i = 0; i < 120; ++i) mode.nextPitch();
    CHECK_NEAR(mode.currentPitch(), 66, 1e-12);
    finishGlide(mode);
    mode.noteOn(0, 84, 100);
    CHECK(mode.glideRemainingSamples() == 192000);
    for (int i = 0; i < 96000; ++i) mode.nextPitch();
    CHECK_NEAR(mode.currentPitch(), 72 + 12.0 / 17, 1e-12);
}

TEST_CASE("wavetable voice mode glide has sample exact finite bounded endpoints")
{
    VoiceMode::PitchGlide glide;
    glide.prepare(48000);
    CHECK(glide.start(60, 72, 0.001, 0));
    CHECK(glide.remainingSamples() == 48);
    CHECK_NEAR(glide.current(), 60, 0);
    CHECK_NEAR(glide.next(), 60.25, 1e-12);
    for (int i = 1; i < 47; ++i) glide.next();
    CHECK_NEAR(glide.current(), 71.75, 1e-12);
    CHECK(glide.active());
    CHECK_NEAR(glide.next(), 72, 0);
    CHECK(! glide.active() && glide.remainingSamples() == 0);
    CHECK_NEAR(glide.next(), 72, 0);
    CHECK(glide.start(72, 48, 0, 0));
    CHECK(! glide.active());
    CHECK_NEAR(glide.current(), 48, 0);
    CHECK(glide.start(48, 48, 8, 100));
    CHECK(! glide.active());
    CHECK(glide.start(48, 60, -1, 0));
    CHECK_NEAR(glide.current(), 60, 0);
    CHECK(glide.start(60, 72, 1, 0));
    const auto remaining = glide.remainingSamples();
    CHECK(! glide.start(std::numeric_limits<double>::infinity(), 72, 1, 0));
    CHECK(! glide.start(60, std::numeric_limits<double>::quiet_NaN(), 1, 0));
    CHECK(! glide.start(60, 72, std::numeric_limits<double>::infinity(), 0));
    CHECK(! glide.start(60, 72, 1, std::numeric_limits<float>::quiet_NaN()));
    CHECK(glide.remainingSamples() == remaining);
    CHECK_NEAR(glide.current(), 60, 0);
}

TEST_CASE("wavetable voice mode own rational curve is monotonic and reversible")
{
    for (const float curve : {-100.0f, -25.0f, 0.0f, 25.0f, 100.0f})
    {
        VoiceMode::PitchGlide rising, falling;
        rising.prepare(8000);
        falling.prepare(8000);
        rising.start(60, 72, 0.01, curve);
        falling.start(72, 60, 0.01, curve);
        double previous = 60;
        for (int i = 0; i < 80; ++i)
        {
            const double up = rising.next(), down = falling.next();
            CHECK(std::isfinite(up) && up >= previous && up <= 72);
            CHECK(std::isfinite(down) && down >= 60 && down <= 72);
            CHECK_NEAR(up + down, 132, 2e-14);
            previous = up;
            if (i == 39)
                CHECK_NEAR((up - 60) / 12, 1 / (1 + std::exp2(static_cast<double>(curve) / 25)), 1e-14);
        }
        CHECK_NEAR(rising.current(), 72, 0);
        CHECK_NEAR(falling.current(), 60, 0);
    }
}

TEST_CASE("wavetable voice mode rejects invalid MIDI and clamps invalid settings")
{
    VoiceMode mode;
    mode.prepare(48000);
    mode.noteOn(0, 60, 100);
    const auto selected = mode.selected();
    for (const auto& invalid : {mode.noteOn(-1, 60, 100), mode.noteOn(16, 60, 100),
                              mode.noteOn(0, -1, 100), mode.noteOn(0, 128, 100),
                              mode.noteOn(0, 61, -1), mode.noteOn(0, 61, 128),
                              mode.noteOff(-1, 60), mode.noteOff(0, 128), mode.sustain(16, true),
                              mode.allNotesOff(-1), mode.allSoundOff(16), mode.resetControllers(16)})
        CHECK(! invalid.changed && invalid.after.age == selected.age);
    CHECK(mode.heldCount(-1) == 0 && mode.physicalHeldCount(16) == 0 && ! mode.sustainDown(16));
    CHECK(! mode.selection(16).active());
    auto settings = glidingSettings(true);
    settings.portaSeconds = std::numeric_limits<double>::quiet_NaN();
    settings.portaCurve = std::numeric_limits<float>::infinity();
    settings.priority = static_cast<VoiceMode::Priority>(99);
    mode.configure(settings);
    CHECK_NEAR(mode.settings().portaSeconds, 0, 0);
    CHECK_NEAR(mode.settings().portaCurve, 0, 0);
    CHECK(mode.settings().priority == VoiceMode::Priority::Last);
    settings.portaSeconds = 100;
    settings.portaCurve = -200;
    mode.configure(settings);
    CHECK_NEAR(mode.settings().portaSeconds, 8, 0);
    CHECK_NEAR(mode.settings().portaCurve, -100, 0);
    mode.prepare(48000);
    CHECK(! mode.selected().active() && mode.heldCount(0) == 0);
}

TEST_CASE("wavetable voice mode rate and pitch clamps bound glide storage")
{
    VoiceMode::PitchGlide glide;
    glide.prepare(1);
    glide.start(-1000, 1000, 0.001, 0);
    CHECK(glide.remainingSamples() == 8);
    CHECK_NEAR(glide.current(), -256, 0);
    while (glide.active()) glide.next();
    CHECK_NEAR(glide.current(), 256, 0);
    glide.prepare(1e20);
    glide.start(60, 72, 1000, 0);
    CHECK(glide.remainingSamples() == 49152000);
    glide.prepare(std::numeric_limits<double>::quiet_NaN());
    glide.start(60, 72, 1, 0);
    CHECK(glide.remainingSamples() == 44100);
    glide.reset(std::numeric_limits<double>::quiet_NaN());
    CHECK_NEAR(glide.current(), 60, 0);
}

TEST_CASE("wavetable voice mode storage overflow never evicts another channel")
{
    static_assert(std::is_trivially_copyable_v<VoiceMode>);
    static_assert(sizeof(VoiceMode) <= 65536);
    VoiceMode mode;
    mode.prepare(48000);
    mode.noteOn(1, 47, 99);
    for (int i = 0; i < VoiceMode::notesPerChannel; ++i) mode.noteOn(0, 60, 100);
    CHECK(mode.heldCount(0) == VoiceMode::notesPerChannel);
    mode.noteOn(0, 72, 88);
    CHECK(mode.heldCount(0) == VoiceMode::notesPerChannel && mode.heldCount(1) == 1);
    CHECK(mode.selected().note == 72 && mode.selection(1).note == 47);
    for (int i = 0; i < VoiceMode::notesPerChannel - 1; ++i) mode.noteOff(0, 60);
    CHECK(mode.heldCount(0) == 1 && mode.selected().note == 72);
    CHECK(! mode.noteOff(0, 60).changed);
    CHECK(mode.noteOff(0, 72).after.channel == 1);
    CHECK(mode.allSoundOff(1).release);
    for (int channel = 0; channel < VoiceMode::channels; ++channel)
        for (int i = 0; i < VoiceMode::notesPerChannel + 3; ++i) mode.noteOn(channel, i % 128, 100);
    for (int channel = 0; channel < VoiceMode::channels; ++channel)
    {
        CHECK(mode.heldCount(channel) == VoiceMode::notesPerChannel);
        mode.allSoundOff(channel);
    }
    CHECK(! mode.selected().active());
}

TEST_CASE("wavetable voice mode observed portamento scalar preserves storage domain")
{
    CHECK_NEAR(VoiceMode::observedPortamentoSeconds(0), 0, 0);
    CHECK_NEAR(VoiceMode::observedPortamentoSeconds(0.375f), 0.059326171875, 1e-10);
    CHECK_NEAR(VoiceMode::observedPortamentoSeconds(0.5f), 0.25, 0);
    CHECK_NEAR(VoiceMode::observedPortamentoSeconds(1), 8, 0);
    CHECK_NEAR(VoiceMode::observedPortamentoSeconds(-1), 0, 0);
    CHECK_NEAR(VoiceMode::observedPortamentoSeconds(2), 8, 0);
    CHECK_NEAR(VoiceMode::observedPortamentoSeconds(std::numeric_limits<float>::quiet_NaN()), 0, 0);
}

TEST_CASE("wavetable voice mode sample event timeline ignores block partition")
{
    enum class Kind { On, Off, Pedal, NotesOff, SoundOff, Reset };
    struct Event { int sample; Kind kind; int channel, note, value; };
    const std::array events{
        Event{17, Kind::On, 0, 60, 80}, Event{38, Kind::On, 0, 72, 100},
        Event{38, Kind::On, 1, 67, 77}, Event{91, Kind::Pedal, 1, 0, 1},
        Event{98, Kind::Off, 1, 67, 0}, Event{103, Kind::On, 0, 72, 20},
        Event{170, Kind::Off, 0, 72, 0}, Event{259, Kind::Off, 0, 72, 0},
        Event{480, Kind::Reset, 1, 0, 0}, Event{679, Kind::Off, 0, 60, 0},
        Event{987, Kind::On, 2, 48, 99}, Event{1101, Kind::On, 2, 84, 99},
        Event{1200, Kind::Pedal, 2, 0, 1}, Event{1401, Kind::NotesOff, 2, 0, 0},
        Event{1537, Kind::SoundOff, 2, 0, 0}, Event{1700, Kind::On, 3, 60, 100},
        Event{1900, Kind::Off, 3, 60, 0}};
    const auto render = [&events](int blockSize)
    {
        VoiceMode mode;
        mode.prepare(48000);
        auto settings = glidingSettings(true, true, true);
        settings.portaCurve = 25;
        mode.configure(settings);
        std::vector<double> result(2200);
        size_t event = 0;
        for (int block = 0; block < static_cast<int>(result.size()); block += blockSize)
            for (int sample = block; sample < std::min(block + blockSize, static_cast<int>(result.size())); ++sample)
            {
                while (event < events.size() && events[event].sample == sample)
                {
                    const auto& e = events[event++];
                    switch (e.kind)
                    {
                        case Kind::On: mode.noteOn(e.channel, e.note, e.value); break;
                        case Kind::Off: mode.noteOff(e.channel, e.note); break;
                        case Kind::Pedal: mode.sustain(e.channel, e.value != 0); break;
                        case Kind::NotesOff: mode.allNotesOff(e.channel); break;
                        case Kind::SoundOff: mode.allSoundOff(e.channel); break;
                        case Kind::Reset: mode.resetControllers(e.channel); break;
                    }
                }
                result[static_cast<size_t>(sample)] = mode.nextPitch() + (mode.selected().active() ? 0 : 1000);
            }
        return result;
    };
    const auto reference = render(2200);
    for (const int block : {1, 7, 97, 512})
    {
        const auto actual = render(block);
        for (size_t sample = 0; sample < actual.size(); ++sample) CHECK_NEAR(actual[sample], reference[sample], 0);
    }
}


TEST_CASE("wavetable voice mode imported duplicates global pedal and interior exponential glide are explicit")
{
    sonora::devices::WavetableVoiceMode mode; mode.prepare(48000);
    sonora::devices::WavetableVoiceMode::Settings settings;
    settings.deduplicate = true; settings.globalSustain = true; settings.exponentialPortamento = true;
    settings.portaSeconds = .25; settings.portaCurve = -50;
    mode.configure(settings);
    const auto first = mode.noteOn(1, 60, 100); CHECK(first.retrigger);
    // A repeated pitch keeps one stack entry but re-selects it like a new note (measured reference retrigger).
    const auto repeated = mode.noteOn(1, 60, 60); CHECK(repeated.changed); CHECK(repeated.retrigger); CHECK(mode.heldCount(1) == 1);
    mode.noteOn(2, 60, 80); CHECK(mode.heldCount(2) == 1);
    CHECK(! mode.noteOff(3, 60).changed);
    mode.sustain(0, true); mode.noteOff(2, 60); CHECK(mode.selected().channel == 2);
    mode.noteOff(1, 60); CHECK(mode.selected().active());
    CHECK(mode.sustain(0, false).release); CHECK(! mode.selected().active());
    mode.reset(); mode.noteOn(0, 60, 100); mode.noteOn(0, 84, 100);
    double atMidpoint = 0;
    for (int sample = 0; sample < 6000; ++sample) atMidpoint = mode.nextPitch();
    CHECK_NEAR(atMidpoint, 61.5, .00001); // t=.5: measured interior slow law gives .0625 progress.
    for (int sample = 6000; sample < 12000; ++sample) mode.nextPitch();
    CHECK_NEAR(mode.currentPitch(), 84, 0);
}
