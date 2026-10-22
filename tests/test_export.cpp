// Export / bounce: offline render through the real engine into WAV / AIFF / FLAC, stems, normalize, mono, tails, cancel.

#include "TestFramework.h"
#include "engine/AudioEngine.h"
#include "engine/Exporter.h"
#include "io/als/AlsReader.h"
#include "midi/MidiManager.h"
#include "plugins/PluginHost.h"

#include <juce_audio_formats/juce_audio_formats.h>

using namespace sonora;

namespace
{
struct ExportRig
{
    juce::ScopedJuceInitialiser_GUI gui;
    plugins::PluginHost host;
    midi::MidiManager midi;
    engine::AudioEngine engine{host, midi};
    io::AlsLoadResult loaded;
    juce::File dir;

    ExportRig()
    {
        loaded = io::loadAls(juce::File(SONORA_TEST_FIXTURES).getChildFile("two_tracks.xml")); // a MIDI clip plays at beats 4..12
        REQUIRE(loaded);
        engine.loadSong(*loaded.song);
        dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("sonora_export_test");
        dir.deleteRecursively();
        REQUIRE(dir.createDirectory());
    }
    ~ExportRig() { dir.deleteRecursively(); }

    engine::ExportResult run(const engine::ExportOptions& options, bool cancelled = false)
    {
        engine::Exporter exporter(engine, *loaded.song);
        std::atomic<float> progress{0.0f};
        std::atomic<bool> cancel{cancelled};
        REQUIRE(engine.beginOffline());
        auto result = exporter.run(options, progress, cancel);
        engine.endOffline();
        return result;
    }
};

struct FileInfo
{
    int channels = 0;
    juce::int64 frames = 0;
    double sampleRate = 0;
    float peak = 0;
    float peakInFirstSeconds = 0; // before the first note at beat 4
};

FileInfo inspect(const juce::File& file, double silentUntilSeconds = 1.5)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
    FileInfo info;
    if (reader == nullptr)
        return info;
    info.channels = (int) reader->numChannels;
    info.frames = reader->lengthInSamples;
    info.sampleRate = reader->sampleRate;
    juce::AudioBuffer<float> all(info.channels, (int) info.frames);
    reader->read(&all, 0, (int) info.frames, 0, true, true);
    info.peak = all.getMagnitude(0, (int) info.frames);
    info.peakInFirstSeconds = all.getMagnitude(0, 0, std::min((int) info.frames, (int) (silentUntilSeconds * info.sampleRate)));
    return info;
}
} // namespace

TEST_CASE("export: the default range ends on the bar after the last clip")
{
    ExportRig rig;
    CHECK_NEAR(engine::Exporter::arrangementEndBeat(*rig.loaded.song), 12.0, 1e-9);
    model::Song empty;
    CHECK_NEAR(engine::Exporter::arrangementEndBeat(empty), 4.0, 1e-9);
}

TEST_CASE("export: Main mix renders the exact range length with silence before the clip and sound after it")
{
    ExportRig rig;
    engine::ExportOptions o;
    o.startBeat = 0.0;
    o.endBeat = 16.0; // 7.5 s at 128 BPM; the MIDI clip starts at beat 4 = 1.875 s
    o.destination = rig.dir.getChildFile("mix.wav");
    o.bitDepth = 24;
    const auto r = rig.run(o);
    REQUIRE(r.ok);
    REQUIRE(r.files.size() == 1);
    const auto info = inspect(o.destination);
    CHECK(info.channels == 2);
    const double expectedFrames = 16.0 * 60.0 / 128.0 * rig.engine.sampleRate();
    CHECK(std::abs((double) info.frames - expectedFrames) <= 2.0);
    CHECK(info.peakInFirstSeconds < 1.0e-4f);
    CHECK(info.peak > 0.01f);
    CHECK_NEAR(r.seconds, 7.5, 0.01);
    CHECK(r.truePeakDb > -60.0f);
    CHECK(r.samplePeakDb <= r.truePeakDb + 0.01f);
    CHECK_NEAR(rig.engine.transport().positionBeats(), 0.0, 1e-9); // the transport is back where it was
}

TEST_CASE("export: start offset, tail, normalize, mono, 16-bit FLAC and AIFF")
{
    ExportRig rig;
    engine::ExportOptions o;
    o.startBeat = 4.0;
    o.endBeat = 8.0;
    o.tailSeconds = 1.0;
    o.normalize = true;
    o.mono = true;
    o.bitDepth = 16;
    o.format = engine::ExportFormat::Flac;
    o.destination = rig.dir.getChildFile("loud");
    auto r = rig.run(o);
    REQUIRE(r.ok);
    REQUIRE(r.files.size() == 1);
    CHECK(r.files[0].file.getFileExtension() == ".flac");
    auto info = inspect(r.files[0].file, 0.0);
    CHECK(info.channels == 1);
    const double expected = (4.0 * 60.0 / 128.0 + 1.0) * rig.engine.sampleRate();
    CHECK(std::abs((double) info.frames - expected) <= 2.0);
    CHECK(info.peak > 0.97f); // normalised to full scale (dither may nudge the last bit)
    CHECK(info.peak <= 1.0f);

    o.format = engine::ExportFormat::Aiff;
    o.normalize = false;
    o.mono = false;
    o.bitDepth = 24;
    o.destination = rig.dir.getChildFile("aiff");
    r = rig.run(o);
    REQUIRE(r.ok);
    info = inspect(r.files[0].file, 0.0);
    CHECK(r.files[0].file.getFileExtension() == ".aif");
    CHECK(info.channels == 2);
    CHECK(info.peak > 0.001f);
}

TEST_CASE("export: stems write one file per track and return, only the playing track has sound, the folder is named safely")
{
    ExportRig rig;
    engine::ExportOptions o;
    o.target = engine::ExportTarget::AllTracks;
    o.startBeat = 0.0;
    o.endBeat = 12.0;
    o.destination = rig.dir.getChildFile("stems");
    o.baseName = "My: Song";
    const auto r = rig.run(o);
    REQUIRE(r.ok);
    REQUIRE(r.files.size() == 3); // 1-Synth, 2-Midi, A-Reverb (not Main)
    CHECK(r.files[0].name == "1-Synth");
    CHECK(r.files[0].file.getFileName() == "My- Song - 01 1-Synth.wav");
    CHECK(inspect(r.files[0].file).peak > 0.01f);
    CHECK(inspect(r.files[1].file).peak < 1.0e-5f);
    CHECK(inspect(r.files[2].file).frames == inspect(r.files[0].file).frames); // all stems have the same length
}

TEST_CASE("export: one track, an empty range and a cancelled run fail cleanly without leaving files")
{
    ExportRig rig;
    engine::ExportOptions o;
    o.target = engine::ExportTarget::SingleTrack;
    o.trackIndex = 0;
    o.endBeat = 8.0;
    o.destination = rig.dir.getChildFile("one.wav");
    auto r = rig.run(o);
    REQUIRE(r.ok);
    CHECK(r.files[0].name == "1-Synth");

    o.trackIndex = 99;
    CHECK(! rig.run(o).ok);

    o.trackIndex = 0;
    o.endBeat = 0.0;
    r = rig.run(o);
    CHECK(! r.ok);
    CHECK(r.error.find("empty") != std::string::npos);

    o.endBeat = 8.0;
    o.destination = rig.dir.getChildFile("cancelled.wav");
    r = rig.run(o, true);
    CHECK(! r.ok);
    CHECK(r.cancelled);
    CHECK(! o.destination.existsAsFile());
}

TEST_CASE("export: auto tail stops after the sound has died away")
{
    ExportRig rig;
    engine::ExportOptions o;
    o.endBeat = 12.0; // the note ends inside the range; the reverb return rings on
    o.autoTail = true;
    o.destination = rig.dir.getChildFile("auto.wav");
    const auto r = rig.run(o);
    REQUIRE(r.ok);
    const double rangeSeconds = 12.0 * 60.0 / 128.0;
    CHECK(r.seconds >= rangeSeconds - 0.01);
    CHECK(r.seconds < rangeSeconds + 30.0 - 0.01); // gave up before the 30 s limit
}
