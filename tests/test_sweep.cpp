// PLG-19: Sonora Sweep, risers / downlifters / reverse swells that end on time.
#include "TestFramework.h"
#include "core/DeviceCatalog.h"
#include "devices/DeviceFactory.h"
#include "devices/native/Sweep.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <cmath>

using namespace sonora;
namespace
{
using W = devices::Sweep;
constexpr double kRate = 48000.0;

juce::AudioBuffer<float> play(W& w, int samples, int note = 60, double tempo = 120.0)
{
    devices::HostInfo host;
    host.tempo = tempo;
    host.sampleRate = kRate;
    w.setHostInfo(host);
    juce::AudioBuffer<float> out(2, samples);
    out.clear();
    for (int start = 0; start < samples; start += 256)
    {
        const int len = std::min(256, samples - start);
        juce::AudioBuffer<float> view(out.getArrayOfWritePointers(), 2, start, len);
        juce::MidiBuffer midi;
        if (start == 0) midi.addEvent(juce::MidiMessage::noteOn(1, note, 1.0f), 0);
        w.process(view, midi);
    }
    return out;
}

double rms(const juce::AudioBuffer<float>& a, int from, int to)
{
    double sum = 0;
    for (int s = from; s < to; ++s) sum += (double) a.getSample(0, s) * a.getSample(0, s);
    return std::sqrt(sum / std::max(1, to - from));
}

// Energy of the first difference (a crude "brightness") over a span.
double brightness(const juce::AudioBuffer<float>& a, int from, int to)
{
    double diff = 0, level = 0;
    for (int s = from + 1; s < to; ++s)
    {
        const double d = a.getSample(0, s) - a.getSample(0, s - 1);
        diff += d * d;
        level += (double) a.getSample(0, s) * a.getSample(0, s);
    }
    return diff / std::max(1e-12, level);
}

double zeroCrossingHz(const juce::AudioBuffer<float>& a, int from, int to)
{
    int crossings = 0;
    for (int s = from + 1; s < to; ++s)
        if ((a.getSample(0, s - 1) < 0) != (a.getSample(0, s) < 0)) ++crossings;
    return crossings * 0.5 * kRate / (to - from);
}

void setup(W& w)
{
    w.prepare(kRate, 256);
    w.setParameter(W::LengthParam, 2.0f); // 1 Bar = 2 s at 120 BPM
    w.setParameter(W::Curve, 0.0f);       // linear
}
} // namespace

TEST_CASE("sweep: a note plays one sweep of exactly Length; Up opens the filter and swells, Down mirrors it")
{
    W up;
    setup(up);
    CHECK_NEAR(up.lengthSeconds(), 2.0, 1e-9);
    const auto riser = play(up, 120000);
    CHECK(rms(riser, 96000 - 2400, 96000 - 300) > 0.01);
    CHECK(rms(riser, 96000 + 10, 120000) < 1e-7); // silent after the end
    CHECK(rms(riser, 2000, 10000) < rms(riser, 80000, 88000) * 0.5); // swells in
    CHECK(brightness(riser, 2000, 10000) < brightness(riser, 80000, 88000) * 0.5); // the filter opens
    CHECK(up.progress() < 0);

    W down;
    setup(down);
    down.setParameter(W::Direction, (float) W::Down);
    const auto lifter = play(down, 96000);
    CHECK(rms(lifter, 2000, 10000) > rms(lifter, 80000, 88000) * 2.0);
    CHECK(brightness(lifter, 2000, 10000) > brightness(lifter, 80000, 88000) * 2.0);
    // The face's curves agree.
    CHECK(up.cutoffAt(0) < up.cutoffAt(1));
    CHECK(down.cutoffAt(0) > down.cutoffAt(1));
    CHECK_NEAR(up.levelAt(1), 1.0, 1e-6);
}

TEST_CASE("sweep: the Tone rises by Pitch semitones from the note; Rhythm gates faster and faster")
{
    W w;
    setup(w);
    w.setParameter(W::Source, (float) W::Tone);
    w.setParameter(W::Detune, 0.0f);
    w.setParameter(W::Swell, 0.0f);
    w.setParameter(W::HighHz, 20000.0f);
    w.setParameter(W::LowHz, 20000.0f); // filter open all the way
    w.setParameter(W::Pitch, 12.0f);
    const auto out = play(w, 96000, 45); // A1 = 110 Hz -> 220 Hz at the end
    CHECK_NEAR(zeroCrossingHz(out, 0, 4800), 110 * std::pow(2.0, 12.0 * 0.025 / 12.0), 6.0);
    CHECK_NEAR(zeroCrossingHz(out, 91200, 96000), 220 * std::pow(2.0, -12.0 * 0.025 / 12.0), 8.0);

    W g;
    setup(g);
    g.setParameter(W::Rhythm, 1.0f); // 1/4 -> 1/16
    g.setParameter(W::Swell, 0.0f);
    const auto gated = play(g, 96000);
    // Early: one gate per beat (0.5 s): open in its first half, closed in the second.
    CHECK(rms(gated, 2000, 9000) > 0.01);
    CHECK(rms(gated, 15000, 20500) < 1e-4);
    CHECK_NEAR(g.gateRateAt(0), 1.0, 1e-6);
    CHECK_NEAR(g.gateRateAt(1), 4.0, 1e-6);
}

TEST_CASE("sweep: a sample sweep Up plays it reversed with its tail and lands on its attack at the end")
{
    // The "sample": a click (the attack) then a 300 Hz tone decaying over 0.3 s.
    juce::AudioBuffer<float> sample(2, 24000);
    for (int s = 0; s < 24000; ++s)
    {
        const float v = (s < 24 ? 0.9f : 0.0f) + 0.5f * std::exp(-(float) s / 7000.0f) * std::sin(6.2831853f * 300.0f * (float) s / 48000.0f);
        sample.setSample(0, s, v);
        sample.setSample(1, s, v);
    }
    W w;
    setup(w);
    w.setParameter(W::Source, (float) W::Sample);
    w.setParameter(W::LowHz, 20000.0f);
    w.setParameter(W::HighHz, 20000.0f);
    w.setParameter(W::Swell, 0.0f);
    w.setParameter(W::Tail, 0.5f);
    w.setSampleAudio(sample, kRate, "hit");
    REQUIRE(w.hasSample());
    const auto out = play(w, 100000);
    // The loudest moment is the click, in the last few milliseconds of the 2 s sweep.
    int peakAt = 0;
    float peak = 0;
    for (int s = 0; s < 100000; ++s)
        if (std::abs(out.getSample(0, s)) > peak) { peak = std::abs(out.getSample(0, s)); peakAt = s; }
    CHECK(peakAt > 96000 - 480);
    CHECK(peakAt <= 96000);
    // The tail swells in before it (reverb, reversed): something sounds a second before the end, quieter than the end.
    CHECK(rms(out, 48000, 52000) > 1e-4);
    CHECK(rms(out, 48000, 52000) < rms(out, 90000, 95500));
    // Turning Reverse off (or Down) plays it forwards: the click comes first.
    w.setParameter(W::Reverse, 0.0f);
    w.refreshSample();
    const auto forwards = play(w, 100000);
    peak = 0;
    for (int s = 0; s < 100000; ++s)
        if (std::abs(forwards.getSample(0, s)) > peak) { peak = std::abs(forwards.getSample(0, s)); peakAt = s; }
    CHECK(peakAt < 2000);
}

TEST_CASE("sweep: offline render, catalog, factory with a saved sample path")
{
    std::vector<float> values(W::NumParams);
    W defaults;
    for (int i = 0; i < W::NumParams; ++i) values[(size_t) i] = defaults.getParameter(i);
    values[W::LengthParam] = 3.0f; // 2 bars = 4 s at 120
    const auto noise = W::renderOffline(values, nullptr, kRate, 120.0);
    CHECK(noise.getNumSamples() == 192000);
    CHECK(rms(noise, 180000, 191000) > 0.01);

    const auto* entry = model::findCatalogEntry("SonoraSweep");
    REQUIRE(entry != nullptr);
    CHECK(model::deviceCategory(*entry) == "Synthesis");
    // A sample path in native content loads with the device.
    const auto file = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("sonora_sweep_test.wav");
    {
        juce::AudioBuffer<float> tone(1, 4800);
        for (int s = 0; s < 4800; ++s) tone.setSample(0, s, 0.5f * std::sin((float) s * 0.05f));
        juce::WavAudioFormat wav;
        file.deleteFile();
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream>(file);
        auto writer = wav.createWriterFor(stream, juce::AudioFormatWriterOptions{}.withSampleRate(48000).withNumChannels(1).withBitsPerSample(16));
        REQUIRE(writer != nullptr);
        writer->writeFromAudioSampleBuffer(tone, 0, 4800);
    }
    auto d = devices::nativeDeviceModel("SonoraSweep");
    auto content = std::make_shared<model::NativeContent>();
    content->format = "SonoraSweep/Sample";
    const std::string path = file.getFullPathName().toStdString();
    content->data.assign(path.begin(), path.end());
    d.nativeContent = content;
    std::vector<std::string> warnings;
    auto dev = devices::createNative(d, &warnings);
    auto* sweep = dynamic_cast<W*>(dev.get());
    REQUIRE(sweep != nullptr);
    CHECK(sweep->hasSample());
    CHECK(sweep->sampleName() == "sonora_sweep_test");
    CHECK(warnings.empty());
    file.deleteFile();
}

#include "engine/SweepFromClip.h"

TEST_CASE("sweep from clip: a riser lands on a new track under the clip, ending where the clip starts")
{
    model::Song song;
    song.tempo = 120.0;
    song.tracks.resize(1);
    song.tracks[0].id = 3;
    song.tracks[0].name = "Chords";
    song.tracks[0].kind = model::TrackKind::Audio;
    model::Clip chord;
    chord.kind = model::Clip::Kind::Audio;
    chord.uid = 11;
    chord.time = chord.currentStart = 16.0;
    chord.currentEnd = 20.0;
    chord.sampleRate = kRate;
    chord.lengthSamples = 24000;
    song.tracks[0].arrangementClips = {chord};
    juce::AudioBuffer<float> audio(2, 24000);
    for (int s = 0; s < 24000; ++s)
        for (int ch = 0; ch < 2; ++ch) audio.setSample(ch, s, 0.4f * std::sin(6.2831853f * 220.0f * (float) s / 48000.0f) * std::exp(-(float) s / 9000.0f));
    const auto folder = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("sonora_sweep_from_clip");
    folder.deleteRecursively();
    const auto location = model::ClipLocation::arrangement(0, 11);

    auto made = engine::sweepFromClip(song, location, true, 8.0, audio, kRate, "chord", folder);
    REQUIRE(made.error.empty());
    CHECK(made.file.existsAsFile());
    CHECK(made.newTrack == 1);
    made.edit.apply(song);
    REQUIRE(song.tracks.size() == 2);
    CHECK(song.tracks[1].name == "Riser");
    CHECK(song.tracks[1].kind == model::TrackKind::Audio);
    REQUIRE(song.tracks[1].arrangementClips.size() == 1);
    const auto& riser = song.tracks[1].arrangementClips[0];
    CHECK_NEAR(riser.currentStart, 8.0, 1e-9);
    CHECK_NEAR(riser.currentEnd, 16.0, 1e-6); // ends exactly where the chord starts
    CHECK(riser.lengthSamples == 4 * 48000);   // 8 beats at 120 BPM
    CHECK(riser.name == "chord riser");
    CHECK(song.tracks[0].arrangementClips.size() == 1); // the original is untouched

    // A downlifter starts with the clip; a riser longer than the room before the clip is refused.
    auto down = engine::sweepFromClip(song, location, false, 4.0, audio, kRate, "chord", folder);
    REQUIRE(down.error.empty());
    down.edit.apply(song);
    CHECK_NEAR(song.tracks[1].arrangementClips[0].currentStart, 16.0, 1e-9);
    CHECK(song.tracks[1].name == "Downlifter");
    CHECK(! engine::sweepFromClip(song, location, true, 32.0, audio, kRate, "chord", folder).error.empty());
    folder.deleteRecursively();
}
