// PLG-19d: Sonora Sweep's reverb swell: the four characters (Hall, Shimmer, Bloom, Grain), their decay and damping, and the
// device playing only the sample's reverb, reversed, into the drop (Region, Fit, Hit, Down = a reverb throw).
#include "TestFramework.h"
#include "devices/native/Sweep.h"
#include "devices/native/SweepSwell.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_dsp/juce_dsp.h>

#include <chrono>
#include <cmath>
#include <cstdio>

using namespace sonora;
namespace
{
using W = devices::Sweep;
namespace sw = devices::swell;
constexpr double kRate = 48000.0;

// A drum-like hit: a click and a 220 Hz body decaying over ~0.15 s.
juce::AudioBuffer<float> hitSample(int frames = 24000, float hz = 220.0f)
{
    juce::AudioBuffer<float> b(2, frames);
    for (int s = 0; s < frames; ++s)
    {
        const float v = (s < 24 ? 0.9f : 0.0f) + 0.6f * std::exp(-(float) s / 7000.0f) * std::sin(6.2831853f * hz * (float) s / (float) kRate);
        b.setSample(0, s, v);
        b.setSample(1, s, v);
    }
    return b;
}

double rms(const juce::AudioBuffer<float>& a, int from, int to, int ch = 0)
{
    double sum = 0;
    from = std::max(0, from);
    to = std::min(to, a.getNumSamples());
    for (int s = from; s < to; ++s) sum += (double) a.getSample(ch, s) * a.getSample(ch, s);
    return std::sqrt(sum / std::max(1, to - from));
}

// Fraction of the energy above `hz` in a span (one FFT of 8192 points at the span's start).
double energyAbove(const juce::AudioBuffer<float>& a, int from, double hz)
{
    constexpr int order = 13, n = 1 << order;
    juce::dsp::FFT fft(order);
    std::vector<float> data((size_t) n * 2, 0.0f);
    for (int i = 0; i < n && from + i < a.getNumSamples(); ++i)
        data[(size_t) i] = a.getSample(0, from + i) * (0.5f - 0.5f * std::cos(6.2831853f * (float) i / n));
    fft.performFrequencyOnlyForwardTransform(data.data());
    double above = 0, total = 0;
    for (int k = 1; k < n / 2; ++k)
    {
        const double e = (double) data[(size_t) k] * data[(size_t) k];
        total += e;
        if (k * kRate / n > hz) above += e;
    }
    return above / std::max(1e-20, total);
}

// Energy within +-3 % of `hz` over the band 100 Hz..10 kHz.
double bandShare(const juce::AudioBuffer<float>& a, int from, double hz)
{
    constexpr int order = 14, n = 1 << order;
    juce::dsp::FFT fft(order);
    std::vector<float> data((size_t) n * 2, 0.0f);
    for (int i = 0; i < n && from + i < a.getNumSamples(); ++i)
        data[(size_t) i] = a.getSample(0, from + i) * (0.5f - 0.5f * std::cos(6.2831853f * (float) i / n));
    fft.performFrequencyOnlyForwardTransform(data.data());
    double in = 0, total = 0;
    for (int k = 1; k < n / 2; ++k)
    {
        const double f = k * kRate / n;
        if (f < 100 || f > 10000) continue;
        const double e = (double) data[(size_t) k] * data[(size_t) k];
        total += e;
        if (std::abs(f - hz) < hz * 0.03) in += e;
    }
    return in / std::max(1e-20, total);
}

juce::AudioBuffer<float> play(W& w, int samples, double tempo = 120.0, int note = 60)
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

bool finite(const juce::AudioBuffer<float>& a)
{
    for (int ch = 0; ch < a.getNumChannels(); ++ch)
        for (int s = 0; s < a.getNumSamples(); ++s)
            if (! std::isfinite(a.getSample(ch, s))) return false;
    return true;
}

// A swell-ready Sweep: Sample, Reverb Swell, the filter open, no level swell (the tail's own rise is what is measured).
void swellSetup(W& w, double tempo = 120.0)
{
    devices::HostInfo host;
    host.tempo = tempo;
    host.sampleRate = kRate;
    w.setHostInfo(host);
    w.prepare(kRate, 256);
    w.setParameter(W::Source, (float) W::Sample);
    w.setParameter(W::SwellMode, (float) W::ReverbSwell);
    w.setParameter(W::LengthParam, 3.0f); // 2 bars = 4 s at 120 BPM
    w.setParameter(W::LowHz, 20000.0f);
    w.setParameter(W::HighHz, 20000.0f);
    w.setParameter(W::Swell, 0.0f);
    w.setParameter(W::Pitch, 0.0f);
    w.setParameter(W::Output, 0.0f);
}
} // namespace

TEST_CASE("sweep swell: character weights are bilinear and sum to one")
{
    const auto corner = sw::weights(0, 0);
    CHECK(std::abs(corner[sw::Hall] - 1.0f) < 1e-6f);
    CHECK(std::abs(sw::weights(0, 1)[sw::Shimmer] - 1.0f) < 1e-6f);
    CHECK(std::abs(sw::weights(1, 1)[sw::Bloom] - 1.0f) < 1e-6f);
    CHECK(std::abs(sw::weights(1, 0)[sw::Grain] - 1.0f) < 1e-6f);
    for (float x : {0.0f, 0.3f, 0.5f, 1.0f})
        for (float y : {0.0f, 0.7f, 1.0f})
        {
            const auto w = sw::weights(x, y);
            CHECK(std::abs(w[0] + w[1] + w[2] + w[3] - 1.0f) < 1e-5f);
        }
    const auto middle = sw::weights(0.5f, 0.5f);
    for (float v : middle) CHECK(std::abs(v - 0.25f) < 1e-6f);
}

TEST_CASE("sweep swell: every character is finite, wet only and lasts about its decay; Damp darkens it")
{
    const auto dry = hitSample();
    for (int c = 0; c < sw::kCharacters; ++c)
    {
        sw::Settings s;
        s.decaySeconds = 2.0;
        s.damp = 0.2f;
        const auto t0 = std::chrono::steady_clock::now();
        const auto tail = sw::renderCharacter((sw::Character) c, dry, kRate, s);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::printf("    %s: %d frames, 60 dB decay %.2f s, %.0f ms to render\n", sw::kCharacterNames[c], tail.getNumSamples(),
                    sw::decayTimeOf(tail, kRate), ms);
        CHECK(finite(tail));
        CHECK(tail.getNumChannels() == 2);
        CHECK(tail.getNumSamples() >= dry.getNumSamples() + (int) (2.0 * kRate));
        // It rings on long after the dry hit has died away (~0.5 s) ...
        CHECK(rms(tail, (int) (1.0 * kRate), (int) (1.2 * kRate)) > 1e-5);
        // ... and decays: late is far quieter than early.
        CHECK(rms(tail, (int) (1.8 * kRate), (int) (2.0 * kRate)) < 0.3 * rms(tail, (int) (0.1 * kRate), (int) (0.3 * kRate)));
        const double decay = sw::decayTimeOf(tail, kRate, 40.0);
        CHECK(decay > 0.8);
        CHECK(decay < 2.6);
        // Stereo: the two sides are not the same signal.
        double diff = 0;
        for (int i = (int) (0.3 * kRate); i < (int) (0.8 * kRate); ++i) diff += std::abs(tail.getSample(0, i) - tail.getSample(1, i));
        CHECK(diff > 1.0);
        // Damp: less energy above 4 kHz.
        s.damp = 1.0f;
        const auto dark = sw::renderCharacter((sw::Character) c, dry, kRate, s);
        const int at = (int) (0.5 * kRate);
        std::printf("    %s energy above 4 kHz: damp 0.2 %.4f, damp 1 %.4f\n", sw::kCharacterNames[c], energyAbove(tail, at, 4000), energyAbove(dark, at, 4000));
        CHECK(energyAbove(dark, at, 4000) < 0.7 * energyAbove(tail, at, 4000));
    }
    // A longer decay rings longer (the hall at 1 s vs 4 s).
    sw::Settings shortTail, longTail;
    shortTail.decaySeconds = 1.0;
    longTail.decaySeconds = 4.0;
    const double a = sw::decayTimeOf(sw::renderCharacter(sw::Hall, dry, kRate, shortTail), kRate, 40.0);
    const double b = sw::decayTimeOf(sw::renderCharacter(sw::Hall, dry, kRate, longTail), kRate, 40.0);
    std::printf("    hall 40 dB decay: RT60 1 s -> %.2f s, RT60 4 s -> %.2f s\n", a, b);
    CHECK(b > 2.5 * a);
}

TEST_CASE("sweep swell: Shimmer adds the octave above; the blend is normalised to the dry peak")
{
    // A 440 Hz tone burst: shimmer's tail has far more 880 Hz than the hall's.
    juce::AudioBuffer<float> tone(2, 24000);
    for (int s = 0; s < 24000; ++s)
        for (int ch = 0; ch < 2; ++ch)
            tone.setSample(ch, s, 0.5f * std::sin(6.2831853f * 440.0f * (float) s / (float) kRate) * std::min(1.0f, (24000.0f - (float) s) / 2000.0f));
    sw::Settings s;
    s.decaySeconds = 3.0;
    s.damp = 0.1f;
    const auto hall = sw::renderCharacter(sw::Hall, tone, kRate, s);
    const auto shimmer = sw::renderCharacter(sw::Shimmer, tone, kRate, s);
    const int at = (int) (0.8 * kRate);
    std::printf("    880 Hz share: hall %.3f, shimmer %.3f\n", bandShare(hall, at, 880), bandShare(shimmer, at, 880));
    CHECK(bandShare(shimmer, at, 880) > 3.0 * bandShare(hall, at, 880) + 0.02);
    s.x = 0.5f;
    s.y = 0.5f;
    const auto blend = sw::renderTail(tone, kRate, s);
    CHECK(finite(blend));
    const float peak = std::max(blend.getMagnitude(0, 0, blend.getNumSamples()), blend.getMagnitude(1, 0, blend.getNumSamples()));
    CHECK(std::abs(peak - tone.getMagnitude(0, 0, tone.getNumSamples())) < 0.01f);
    // Nothing in, nothing out (and no crash).
    juce::AudioBuffer<float> empty(2, 0);
    CHECK(finite(sw::renderTail(empty, kRate, s)));
}

TEST_CASE("sweep swell: Up plays only the reverb, reversed, rising into the drop; Hit plays the sample at the drop")
{
    W w;
    swellSetup(w);
    const auto t0 = std::chrono::steady_clock::now();
    w.setSampleAudio(hitSample(), kRate, "hit");
    std::printf("    prepare (0.5 s hit, 4.8 s tail, Hall + Shimmer): %.0f ms\n",
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    CHECK(std::abs(w.swellDecaySeconds() - 4.8) < 1e-6); // Fit: 1.2 x 4 s
    const int drop = (int) (4.0 * kRate);
    auto out = play(w, drop + 48000);
    CHECK(finite(out));
    // It swells: each second louder than the one before, the last 100 ms the loudest.
    const double s1 = rms(out, 0, 48000), s2 = rms(out, 48000, 96000), s3 = rms(out, 96000, 144000), s4 = rms(out, 144000, drop - 4800);
    std::printf("    swell rms per second: %.5f %.5f %.5f %.5f, last 100 ms %.5f\n", s1, s2, s3, s4, rms(out, drop - 4800, drop));
    CHECK(s2 > s1);
    CHECK(s3 > s2);
    CHECK(s4 > 1.5 * s3);
    CHECK(rms(out, drop - 4800, drop) > s4);
    // Wet only: the dry click is not in it (no sample-sharp peak at the end), and after the drop, with Hit at 0, silence.
    CHECK(rms(out, drop + 480, drop + 48000) < 1e-6);
    // Hit 1: the sample itself right at the drop, its click within a millisecond.
    w.setParameter(W::Hit, 1.0f);
    out = play(w, drop + 48000);
    int peakAt = 0;
    float peak = 0;
    for (int s = 0; s < out.getNumSamples(); ++s)
        if (std::abs(out.getSample(0, s)) > peak) { peak = std::abs(out.getSample(0, s)); peakAt = s; }
    std::printf("    Hit: peak %.3f at %d (drop %d)\n", peak, peakAt, drop);
    CHECK(peakAt >= drop);
    CHECK(peakAt < drop + 48);
    CHECK(rms(out, drop + 2000, drop + 8000) > 0.05); // the 220 Hz body after the click
    // The face's helpers.
    CHECK(std::abs(w.hitSeconds() - 0.5) < 1e-3);
    CHECK(w.reverbSwell());
    CHECK(w.sourceOverview(64).size() == 64);
}

TEST_CASE("sweep swell: Fit follows Length and the tempo; without Fit, Tail sets the decay; edits prepare it again")
{
    W w;
    swellSetup(w);
    w.setSampleAudio(hitSample(), kRate, "hit");
    CHECK(std::abs(w.swellDecaySeconds() - 4.8) < 1e-6);
    w.setParameter(W::LengthParam, 4.0f); // 4 bars = 8 s
    CHECK(std::abs(w.swellDecaySeconds() - 9.6) < 1e-6);
    w.setParameter(W::Fit, 0.0f);
    w.setParameter(W::Tail, 0.0f);
    CHECK(std::abs(w.swellDecaySeconds() - 0.4) < 1e-6);
    w.setParameter(W::Tail, 1.0f);
    CHECK(std::abs(w.swellDecaySeconds() - 12.0) < 1e-6);
    w.setParameter(W::Tail, 0.5f);
    w.setParameter(W::LengthParam, 3.0f);
    w.refreshSample();
    const auto a = play(w, (int) (4.0 * kRate));
    // A shorter decay (Tail 0.5 = 2.2 s) inside the 4 s sweep: the first second stays quiet, unlike with Fit.
    const double quietA = rms(a, 0, 48000);
    w.setParameter(W::Fit, 1.0f);
    w.refreshSample(); // the face and the VST3's timer call it after edits
    const auto b = play(w, (int) (4.0 * kRate));
    const double quietB = rms(b, 0, 48000);
    std::printf("    first second: Tail 2.2 s %.6f, Fit 4.8 s %.6f\n", quietA, quietB);
    CHECK(quietB > 3.0 * quietA);
    // Character edits re-prepare too (the audio changes).
    w.setParameter(W::CharacterX, 1.0f);
    w.setParameter(W::CharacterY, 1.0f);
    w.refreshSample();
    const auto c = play(w, (int) (4.0 * kRate));
    double diff = 0;
    for (int s = 0; s < c.getNumSamples(); ++s) diff += std::abs(c.getSample(0, s) - b.getSample(0, s));
    CHECK(diff > 1.0);
    // The old preparation stays alive while the audio thread may hold it and is freed later; prepares never pile up.
    for (int i = 0; i < 6; ++i)
    {
        w.setParameter(W::Damp, 0.1f * (float) i);
        w.refreshSample();
    }
    CHECK(finite(play(w, 4096)));
}

TEST_CASE("sweep swell: Region picks the part of the sample; Down throws the reverb after the hit; the WAV render")
{
    // First half a 200 Hz tone, second half a 1 kHz tone.
    juce::AudioBuffer<float> two(2, 48000);
    for (int s = 0; s < 48000; ++s)
        for (int ch = 0; ch < 2; ++ch)
            two.setSample(ch, s, 0.5f * std::sin(6.2831853f * (s < 24000 ? 200.0f : 1000.0f) * (float) s / (float) kRate));
    W w;
    swellSetup(w);
    w.setParameter(W::Hit, 1.0f);
    w.setParameter(W::RegionStart, 0.5f);
    w.setSampleAudio(two, kRate, "two");
    CHECK(std::abs(w.hitSeconds() - 0.5) < 1e-3);
    const int drop = (int) (4.0 * kRate);
    const auto out = play(w, drop + 24000);
    int crossings = 0;
    for (int s = drop + 2000; s < drop + 12000; ++s)
        if ((out.getSample(0, s - 1) < 0) != (out.getSample(0, s) < 0)) ++crossings;
    const double hz = crossings * 0.5 * kRate / 10000.0;
    std::printf("    Hit of the second half: %.0f Hz\n", hz);
    CHECK(std::abs(hz - 1000.0) < 50.0);
    // Down: the hit with the note, then its reverb going on while the filter closes and the level falls.
    w.setParameter(W::Direction, (float) W::Down);
    w.setParameter(W::RegionStart, 0.0f);
    w.setParameter(W::Swell, 1.0f);
    w.refreshSample();
    const auto down = play(w, drop);
    CHECK(rms(down, 0, 4800) > 0.1); // the hit at once
    CHECK(rms(down, 30000, 40000) > 1e-4); // the reverb after the hit's end (0.5 s)
    CHECK(rms(down, drop - 24000, drop) < rms(down, 30000, 54000)); // fading out
    // The WAV render: Length + the Hit.
    w.setParameter(W::Direction, (float) W::Up);
    const auto file = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("sonora_sweep_swell_render.wav");
    std::string error;
    REQUIRE(w.renderToFile(file, error));
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
    REQUIRE(reader != nullptr);
    const double seconds = (double) reader->lengthInSamples / reader->sampleRate;
    std::printf("    render: %.2f s\n", seconds);
    CHECK(std::abs(seconds - (4.0 + 1.0 + 0.05)) < 0.01); // Region = the whole 1 s sample
    reader.reset();
    file.deleteFile();
    // Generators render too; a Sample source without a sample does not.
    W empty;
    swellSetup(empty);
    CHECK(! empty.renderToFile(file, error));
    CHECK(! error.empty());
}
