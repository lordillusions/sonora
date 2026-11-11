// PLG-20: drum fills written into a MIDI clip, fitting its kit and groove.
#include "TestFramework.h"
#include "core/DrumFill.h"

#include <algorithm>
#include <cmath>
#include <set>

using namespace sonora;
using R = model::DrumRole;
namespace
{
model::MidiNote note(double start, int pitch, float velocity)
{
    model::MidiNote n;
    n.start = start;
    n.duration = 0.25;
    n.pitch = pitch;
    n.velocity = velocity;
    return n;
}

// A 2-bar house groove on a Drum Rack whose pads are named (kick C1, clap D1, closed hat F#1 swung, toms on 45/47/50).
model::Track drumTrack()
{
    model::Track track;
    track.kind = model::TrackKind::Midi;
    model::Device rack;
    rack.type = "DrumGroupDevice";
    const std::pair<const char*, int> pads[] = {{"Kick 909", 36}, {"Clap", 38}, {"Closed Hat", 42}, {"Open Hat", 46},
                                                {"Tom", 45}, {"Tom", 50}, {"Tom", 47}, {"Crash", 49}, {"Shaker", 70}};
    for (const auto& [name, pitch] : pads)
    {
        model::Device::Chain chain;
        chain.name = name;
        chain.receivingNote = pitch;
        rack.chains.push_back(chain);
    }
    track.devices = {rack};
    return track;
}

std::vector<model::MidiNote> houseGroove()
{
    std::vector<model::MidiNote> notes;
    for (int beat = 0; beat < 8; ++beat)
    {
        notes.push_back(note(beat, 36, 110));
        if (beat % 2 == 1) notes.push_back(note(beat, 38, 96));
        notes.push_back(note(beat + 0.5, 42, 70));
        notes.push_back(note(beat + 0.25 + 0.03, 42, 50)); // swung off 16ths
        notes.push_back(note(beat + 0.75 + 0.03, 42, 50));
    }
    return notes;
}
} // namespace

TEST_CASE("drum fill: the kit comes from the Drum Rack's pad names, else General MIDI")
{
    const auto track = drumTrack();
    const auto kit = model::drumKitFor(track, houseGroove());
    CHECK(kit[R::Kick] == 36);
    CHECK(kit[R::Clap] == 38);
    CHECK(kit[R::ClosedHat] == 42);
    CHECK(kit[R::OpenHat] == 46);
    CHECK(kit[R::TomLow] == 45); // three "Tom" pads: low / mid / high by pitch
    CHECK(kit[R::TomMid] == 47);
    CHECK(kit[R::TomHigh] == 50);
    CHECK(kit[R::Crash] == 49);
    CHECK(! kit.has(R::Snare)); // no snare pad: fills use the clap
    bool known = true;
    model::roleForName("Shaker", known);
    CHECK(! known);
    model::roleForName("Hi Tom 2", known);
    CHECK(known);

    model::Track plain; // no Drum Rack: General MIDI
    const auto gm = model::drumKitFor(plain, {});
    CHECK(gm[R::Kick] == 36);
    CHECK(gm[R::Snare] == 38);
    CHECK(gm.toms() == 3);
}

TEST_CASE("drum fill: the groove reads swing and velocities")
{
    const auto kit = model::drumKitFor(drumTrack(), houseGroove());
    const auto groove = model::analyseGroove(houseGroove(), kit, 0, 8);
    CHECK_NEAR(groove.swing, 0.03, 1e-6);
    CHECK(! groove.triplet);
    CHECK_NEAR(groove.kickVelocity, 110, 1e-3);
    CHECK_NEAR(groove.snareVelocity, 96, 1e-3);
}

TEST_CASE("drum fill: every style stays inside the region, uses the kit, leaves the rest alone, ends on a hit")
{
    const auto notes = houseGroove();
    const auto kit = model::drumKitFor(drumTrack(), notes);
    const auto groove = model::analyseGroove(notes, kit, 0, 4);
    std::set<int> pitches;
    for (const int p : kit.pitch) if (p >= 0) pitches.insert(p);
    for (int style = 0; style < (int) model::FillStyle::Count; ++style)
        for (uint32_t seed : {1u, 7u, 99u})
        {
            const auto result = model::generateFill(notes, 4.0, 8.0, kit, groove, 124.0, seed, style);
            CHECK((int) result.style == style);
            int before = 0, inside = 0;
            double lastStart = 0;
            for (const auto& n : result.notes)
            {
                if (n.start < 4.0) ++before;
                else
                {
                    ++inside;
                    CHECK(n.start < 8.0);
                    CHECK(pitches.count(n.pitch) == 1);
                    CHECK(n.velocity >= 1 && n.velocity <= 127);
                    lastStart = std::max(lastStart, n.start);
                }
            }
            CHECK(before == (int) std::count_if(notes.begin(), notes.end(), [](const auto& n) { return n.start < 4.0; }));
            CHECK(inside >= 3);
            CHECK(lastStart >= 7.0); // something happens in the last beat, leading into the next bar
            CHECK(std::is_sorted(result.notes.begin(), result.notes.end(), [](const auto& a, const auto& b) { return a.start < b.start; }));
        }
    // Same seed, same fill; another seed, (almost always) another.
    const auto a = model::generateFill(notes, 4, 8, kit, groove, 124, 5);
    const auto b = model::generateFill(notes, 4, 8, kit, groove, 124, 5);
    CHECK(a.notes.size() == b.notes.size());
    int different = 0;
    for (uint32_t seed = 10; seed < 20; ++seed)
        different += model::generateFill(notes, 4, 8, kit, groove, 124, seed).notes.size() != a.notes.size() ? 1 : 0;
    CHECK(different >= 5);
    // The tom run walks down: high tom notes come before low tom notes.
    const auto run = model::generateFill(notes, 4, 8, kit, groove, 124, 3, (int) model::FillStyle::TomRun);
    double highLast = 0, lowFirst = 99;
    for (const auto& n : run.notes)
    {
        if (n.pitch == kit[R::TomHigh]) highLast = std::max(highLast, n.start);
        if (n.pitch == kit[R::TomLow]) lowFirst = std::min(lowFirst, n.start);
    }
    CHECK(highLast < lowFirst);
    // The snare roll speeds up: more hits in its last beat than in its first.
    const auto roll = model::generateFill(notes, 4, 8, kit, groove, 124, 4, (int) model::FillStyle::SnareRoll);
    int first = 0, lastBeat = 0;
    for (const auto& n : roll.notes)
        if (n.pitch == kit[R::Clap]) { if (n.start >= 4 && n.start < 5) ++first; if (n.start >= 7) ++lastBeat; }
    CHECK(lastBeat > first);
}

TEST_CASE("drum fill: the clip command replaces only the last bar and is one undo step")
{
    model::Song song;
    song.tempo = 126;
    song.tracks = {drumTrack()};
    song.tracks[0].id = 1;
    model::Clip clip;
    clip.kind = model::Clip::Kind::Midi;
    clip.uid = 4;
    clip.loop = {0, 8, 0, true};
    clip.currentStart = 0;
    clip.currentEnd = 8;
    clip.notes = houseGroove();
    song.tracks[0].arrangementClips = {clip};
    const auto location = model::ClipLocation::arrangement(0, 4);
    std::string message;
    auto command = model::drumFill(song, location, 4.0, 1234, message);
    REQUIRE(command.apply != nullptr);
    CHECK(message.find("Drum fill: ") == 0);
    const auto before = song.tracks[0].arrangementClips[0].notes;
    command.apply(song);
    const auto& after = song.tracks[0].arrangementClips[0].notes;
    int kept = 0;
    for (const auto& n : after) if (n.start < 4.0) ++kept;
    CHECK(kept == (int) std::count_if(before.begin(), before.end(), [](const auto& n) { return n.start < 4.0; }));
    command.revert(song);
    CHECK(song.tracks[0].arrangementClips[0].notes.size() == before.size());

    // An audio clip is refused with a message.
    model::Clip audio;
    audio.kind = model::Clip::Kind::Audio;
    audio.uid = 5;
    song.tracks[0].arrangementClips.push_back(audio);
    auto refused = model::drumFill(song, model::ClipLocation::arrangement(0, 5), 4.0, 1, message);
    CHECK(refused.apply == nullptr);
    CHECK(message == "Drum fills go into MIDI clips");
}
