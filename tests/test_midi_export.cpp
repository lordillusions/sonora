#include "TestFramework.h"

#include "io/midi/MidiFileExport.h"

#include <juce_audio_basics/juce_audio_basics.h>

using namespace sonora;

TEST_CASE("export MIDI clip: start marker to end, cut at the end, tempo and meter, readable by a standard parser")
{
    model::Clip clip;
    clip.kind = model::Clip::Kind::Midi;
    clip.name = "Bass Line";
    clip.loop = {1.0, 5.0, 0.5, true}; // plays from beat 1.5 to the loop end at 5
    clip.timeSigNumerator = 7; clip.timeSigDenominator = 8;
    auto note = [](double start, double duration, int pitch, float velocity, bool enabled = true) {
        model::MidiNote n; n.start = start; n.duration = duration; n.pitch = pitch; n.velocity = velocity; n.enabled = enabled; return n;
    };
    clip.notes = {note(0.0, 1.0, 40, 90),      // before the start marker: left out
                  note(1.5, 0.5, 43, 100),     // first note, at tick 0
                  note(2.0, 0.25, 45, 127.6f), // velocity clamps to 127
                  note(3.0, 0.5, 47, 80, false), // muted: left out
                  note(4.5, 2.0, 48, 64)};     // crosses the end: cut at beat 5
    const auto bytes = io::midiClipToSmf(clip, 140.0, clip.timeSigNumerator, clip.timeSigDenominator, 960);

    juce::MemoryInputStream stream(bytes.data(), bytes.size(), false);
    juce::MidiFile file;
    REQUIRE(file.readFrom(stream));
    CHECK(file.getNumTracks() == 2);
    CHECK(file.getTimeFormat() == 960);
    juce::MidiMessageSequence tempos, signatures;
    file.findAllTempoEvents(tempos);
    file.findAllTimeSigEvents(signatures);
    REQUIRE(tempos.getNumEvents() == 1);
    CHECK_NEAR(tempos.getEventPointer(0)->message.getTempoSecondsPerQuarterNote(), 60.0 / 140.0, 1e-6);
    REQUIRE(signatures.getNumEvents() == 1);
    int num = 0, den = 0;
    signatures.getEventPointer(0)->message.getTimeSignatureInfo(num, den);
    CHECK(num == 7); CHECK(den == 8);

    const auto* notes = file.getTrack(1);
    REQUIRE(notes != nullptr);
    std::vector<std::tuple<double, int, int, bool>> seen; // tick, pitch, velocity, on
    for (int i = 0; i < notes->getNumEvents(); ++i)
    {
        const auto& m = notes->getEventPointer(i)->message;
        if (m.isNoteOnOrOff()) seen.emplace_back(m.getTimeStamp(), m.getNoteNumber(), m.getVelocity(), m.isNoteOn());
    }
    REQUIRE(seen.size() == 6);
    CHECK(seen[0] == std::make_tuple(0.0, 43, 100, true));
    CHECK(seen[1] == std::make_tuple(480.0, 43, 64, false));
    CHECK(seen[2] == std::make_tuple(480.0, 45, 127, true)); // the note-off at the same tick comes first
    CHECK(seen[3] == std::make_tuple(720.0, 45, 64, false));
    CHECK(seen[4] == std::make_tuple(2880.0, 48, 64, true));
    CHECK(seen[5] == std::make_tuple(3360.0, 48, 64, false)); // cut at beat 5 = 3.5 beats after the start marker

    // The writer refuses audio clips and writes a file for MIDI ones.
    model::Song song; song.tempo = 128;
    juce::TemporaryFile out(".mid");
    std::string error;
    REQUIRE(io::exportMidiClip(clip, song, out.getFile().getFullPathName().toStdString(), error));
    CHECK(out.getFile().getSize() == (juce::int64) io::midiClipToSmf(clip, 128, 7, 8).size());
    model::Clip audio; audio.kind = model::Clip::Kind::Audio;
    CHECK(! io::exportMidiClip(audio, song, out.getFile().getFullPathName().toStdString(), error));
    CHECK(! error.empty());
}
