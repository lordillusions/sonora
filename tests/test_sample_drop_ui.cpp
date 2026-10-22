// UI-47: tempo detection on dropped loops, checked through the clip BPM the UI shows (ui/AudioClipEdits). Kept apart
// from test_sample_drop.cpp because headless builds have no sonora_ui.
#include "TestFramework.h"

#include "core/Edit.h"
#include "engine/SampleCache.h"
#include "engine/TempoAnalysis.h"
#include "io/als/AlsNewSet.h"
#include "io/als/AlsReader.h"
#include "io/als/SampleDrop.h"
#include "ui/AudioClipEdits.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>
#include <cmath>

using namespace sonora;

namespace
{
int audioTrackIndex(const model::Song& song)
{
    for (size_t i = 0; i < song.tracks.size(); ++i)
        if (song.tracks[i].kind == model::TrackKind::Audio) return (int) i;
    return -1;
}
} // namespace

TEST_CASE("sample drop: a loop's detected tempo sets its beats, and a set tempo change keeps the clip's BPM (UI-47)")
{
    // A 4-bar drum loop at 128 BPM: a short decaying click on every beat, 7.5 s.
    auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("sonora_tempo_drop_test");
    dir.deleteRecursively();
    REQUIRE(dir.createDirectory());
    const auto file = dir.getChildFile("loop128.wav");
    const double rate = 44100, bpm = 128;
    const int frames = (int) std::lround(16 * 60.0 / bpm * rate);
    {
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> stream(file.createOutputStream());
        REQUIRE(stream != nullptr);
        auto writer = wav.createWriterFor(stream, juce::AudioFormatWriterOptions{}.withSampleRate(rate).withNumChannels(1).withBitsPerSample(24));
        REQUIRE(writer != nullptr);
        juce::AudioBuffer<float> audio(1, frames);
        audio.clear();
        for (int beat = 0; beat < 16; ++beat)
        {
            const int at = (int) std::lround(beat * 60.0 / bpm * rate);
            for (int i = 0; i < 2000 && at + i < frames; ++i)
                audio.setSample(0, at + i, (float) (std::sin(i * 0.3) * std::exp(-i / 300.0) * (beat % 4 == 0 ? 0.9 : 0.6)));
        }
        REQUIRE(writer->writeFromAudioSampleBuffer(audio, 0, frames));
    }
    engine::SampleCache cache;
    const auto loaded = cache.load(file.getFullPathName().toStdString());
    REQUIRE(loaded != nullptr);
    const double detected = engine::estimateSampleTempo(loaded->onsets, 120).bpm;
    CHECK_NEAR(detected, 128, 0.5);

    auto song = io::parseAls(*io::createNewSetDom(), "/tmp/sonora_sample_drop.als").song;
    REQUIRE(song != nullptr);
    song->tempo = 120;
    const int audio = audioTrackIndex(*song);
    REQUIRE(audio >= 0);
    auto plan = io::placeSamples(*song, {{file.getFullPathName().toStdString(), rate, frames, detected}}, {audio, 0.0, -1, false});
    REQUIRE(plan.command.apply != nullptr);
    plan.command.apply(*song);
    REQUIRE(! song->tracks[(size_t) audio].arrangementClips.empty());
    const auto clip = song->tracks[(size_t) audio].arrangementClips.back();
    CHECK(clip.warped);
    CHECK(clip.tempoAnalyzed);
    CHECK_NEAR(clip.currentEnd - clip.currentStart, 16 * detected / 128, 1e-6); // 16 beats, not 15 at the set's 120
    CHECK_NEAR(ui::audioedit::clipBpm(clip, song->tempo), detected, 1e-6);

    // Changing the set tempo stretches playback but never the clip's own BPM or its length in beats (Live).
    model::setTempo(*song, 90).apply(*song);
    const auto after = song->tracks[(size_t) audio].arrangementClips.back();
    CHECK_NEAR(ui::audioedit::clipBpm(after, song->tempo), detected, 1e-6);
    CHECK_NEAR(after.currentEnd - after.currentStart, clip.currentEnd - clip.currentStart, 1e-9);

    // Without a detected tempo (one-shots) the set's tempo is assumed, as before.
    auto oneShot = io::placeSamples(*song, {{file.getFullPathName().toStdString(), rate, frames, 0.0}}, {audio, 64.0, -1, false});
    REQUIRE(oneShot.command.apply != nullptr);
    oneShot.command.apply(*song);
    CHECK(! song->tracks[(size_t) audio].arrangementClips.back().tempoAnalyzed);
    CHECK_NEAR(ui::audioedit::clipBpm(song->tracks[(size_t) audio].arrangementClips.back(), song->tempo), 90, 1e-6);
    dir.deleteRecursively();
}
