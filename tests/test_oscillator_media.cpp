#include "TestFramework.h"
#include "devices/native/OscillatorMedia.h"
#include "dsp/Dsp.h"

#include <algorithm>
#include <limits>

using namespace sonora;

namespace
{
using Sample = devices::OscillatorSample;
using Media = devices::OscillatorMedia;
using Zone = devices::OscillatorZone;
using Voice = devices::OscillatorMediaVoice;
using Loop = devices::MediaLoop;
constexpr double rate = 48000;

std::vector<float> ramp(int frames = 16)
{
    std::vector<float> samples(static_cast<size_t>(frames));
    for (int frame = 0; frame < frames; ++frame) samples[static_cast<size_t>(frame)] = static_cast<float>(frame) * 0.05f;
    return samples;
}

std::vector<float> sine(int frames = 32768, double frequency = 375, int channels = 1)
{
    std::vector<float> samples(static_cast<size_t>(frames) * static_cast<size_t>(channels));
    for (int frame = 0; frame < frames; ++frame)
        for (int channel = 0; channel < channels; ++channel)
            samples[static_cast<size_t>(frame) * static_cast<size_t>(channels) + static_cast<size_t>(channel)]
                = static_cast<float>(0.5 * std::sin(2 * dsp::kPi * frequency * frame / rate + channel * dsp::kPi * 0.5));
    return samples;
}

Zone zoneFor(const std::shared_ptr<const Sample>& sample, Loop loop = Loop::None)
{
    Zone zone;
    zone.sample = sample; zone.loop = loop; zone.velocityTracking = 0; zone.releaseSeconds = 0;
    return zone;
}

std::shared_ptr<const Media> checkedMedia(std::vector<Zone> zones)
{
    auto result = Media::fromZones(std::move(zones));
    REQUIRE(result);
    return result.data;
}

std::vector<Voice::Stereo> render(Voice& voice, const Voice::Controls& controls, int frames)
{
    std::vector<Voice::Stereo> output(static_cast<size_t>(frames));
    for (auto& frame : output) frame = voice.next(controls);
    return output;
}

double measuredFrequency(const std::vector<Voice::Stereo>& audio, int begin, int end)
{
    int crossings = 0;
    for (int frame = begin + 1; frame < end; ++frame)
        if (audio[static_cast<size_t>(frame - 1)].left <= 0 && audio[static_cast<size_t>(frame)].left > 0) ++crossings;
    return crossings * rate / (end - begin);
}

double power(const std::vector<Voice::Stereo>& audio)
{
    double sum = 0;
    for (const auto& frame : audio)
    {
        CHECK(std::isfinite(frame.left)); CHECK(std::isfinite(frame.right));
        sum += frame.left * frame.left + frame.right * frame.right;
    }
    return sum / static_cast<double>(audio.size());
}

void writeOwnAudio(const juce::File& file, std::span<const float> source, int channels, bool aiff = false)
{
    REQUIRE(file.getParentDirectory().createDirectory().wasOk());
    REQUIRE(file.deleteFile());
    std::unique_ptr<juce::OutputStream> stream(file.createOutputStream());
    REQUIRE(stream != nullptr);
    juce::WavAudioFormat wav;
    juce::AiffAudioFormat aif;
    juce::AudioFormat& format = aiff ? static_cast<juce::AudioFormat&>(aif) : static_cast<juce::AudioFormat&>(wav);
    auto writer = format.createWriterFor(stream, juce::AudioFormatWriterOptions{}.withSampleRate(rate).withNumChannels(channels).withBitsPerSample(24));
    REQUIRE(writer != nullptr);
    const int frames = static_cast<int>(source.size() / static_cast<size_t>(channels));
    juce::AudioBuffer<float> buffer(channels, frames);
    for (int frame = 0; frame < frames; ++frame)
        for (int channel = 0; channel < channels; ++channel)
            buffer.setSample(channel, frame, source[static_cast<size_t>(frame) * static_cast<size_t>(channels) + static_cast<size_t>(channel)]);
    REQUIRE(writer->writeFromAudioSampleBuffer(buffer, 0, frames));
}
}

TEST_CASE("oscillator media: immutable PCM preserves channel samples and validates bounded input")
{
    const std::vector<float> stereo{0.25f, -0.5f, 0.5f, -0.25f, 0.75f, 0};
    auto input = stereo;
    const auto sample = Sample::fromSamples(input, 2, rate, "Independent stereo");
    REQUIRE(sample);
    CHECK(sample.data->channels() == 2); CHECK(sample.data->sampleFrames() == 3);
    CHECK(sample.data->name() == "Independent stereo");
    CHECK(sample.data->originalSamples() == stereo);
    input[0] = 10;
    CHECK_NEAR(sample.data->sampleLinear(0.5, 0), 0.375, 0);
    CHECK_NEAR(sample.data->sampleLinear(0.5, 1), -0.375, 0);
    CHECK_NEAR(sample.data->sampleLinear(2.5, 1), 0, 0);
    CHECK_NEAR(sample.data->sampleLinear(-1, 0), 0, 0);
    CHECK_NEAR(sample.data->sampleLinear(std::numeric_limits<double>::quiet_NaN(), 0), 0, 0);
    CHECK(! Sample::fromSamples({}, 1, rate));
    CHECK(! Sample::fromSamples(stereo, 3, rate));
    CHECK(! Sample::fromSamples(std::span(stereo).first(3), 2, rate));
    CHECK(! Sample::fromSamples(stereo, 2, 0));
    CHECK(! Sample::fromSamples(stereo, 2, std::numeric_limits<double>::infinity()));
    input[0] = std::numeric_limits<float>::quiet_NaN();
    CHECK(! Sample::fromSamples(input, 2, rate));
    input[0] = 33;
    CHECK(! Sample::fromSamples(input, 2, rate));
    std::vector<float> oversized(Sample::maxPcmBytes / sizeof(float) + 1, 0);
    CHECK(! Sample::fromSamples(oversized, 1, rate));
}

TEST_CASE("oscillator media: spectral reanalysis shares original PCM and preserves deterministic float-bit identity")
{
    const std::vector<float> original{0.0f, -0.0f, 0.5f, -0.25f, 1.0f};
    const auto sample = Sample::fromSamples(original, 1, rate, "Own fingerprint"); REQUIRE(sample);
    CHECK(sample.data->pcmFingerprint() == 0xe98ebed8bf18e10bULL);
    CHECK(sample.data->sourceFile().empty());
    const auto analyzed = Sample::withSpectralAnalysis(sample.data, 2); REQUIRE(analyzed);
    CHECK(analyzed.data->originalSamples().data() == sample.data->originalSamples().data());
    CHECK(analyzed.data->originalSamples() == original);
    CHECK(analyzed.data->pcmFingerprint() == sample.data->pcmFingerprint());
    CHECK(analyzed.data->spectralFrames().size() == 2);
    CHECK(! Sample::withSpectralAnalysis({}, 2));
    CHECK(! Sample::withSpectralAnalysis(sample.data, 1));
    CHECK(! Sample::fromSamples(original, 1, rate, {}, true, Sample::maxSpectralFrames + 1));
    const juce::File file("/tmp/sonora_tests/sonora_oscillator_media/Spectral reference.wav");
    writeOwnAudio(file, sine(), 1);
    const auto loaded = Sample::fromFile(file); REQUIRE(loaded);
    const auto coarser = Sample::withSpectralAnalysis(loaded.data, 4); REQUIRE(coarser);
    CHECK(coarser.data->sourceFile() == loaded.data->sourceFile());
    CHECK(coarser.data->pcmFingerprint() == loaded.data->pcmFingerprint());
    CHECK(coarser.data->originalSamples().data() == loaded.data->originalSamples().data());
}

TEST_CASE("oscillator media: unique-sample spectral bank budget preserves complete PCM and reports time resolution")
{
    std::vector<Zone> zones;
    size_t pcmBytes = 0;
    for (int index = 0; index < 3; ++index)
    {
        const auto sample = Sample::fromSamples(sine(), 1, rate); REQUIRE(sample);
        pcmBytes += sample.data->originalSamples().size() * sizeof(float);
        zones.push_back(zoneFor(sample.data, Loop::Forward)); zones.push_back(zones.back());
    }
    const size_t budget = 12 * sizeof(Sample::SpectralFrame);
    const auto media = Media::fromZones(zones, "Own bounded bank", true, budget); REQUIRE(media);
    CHECK(media.data->zones().size() == zones.size()); CHECK(! media.warnings.empty());
    CHECK(media.data->pcmBytes() == pcmBytes); CHECK(media.data->spectralBytes() <= budget);
    CHECK(media.data->storageBytes() == pcmBytes + media.data->spectralBytes());
    CHECK(media.data->maximumSpectralHopSeconds() > 0.2);
    for (size_t zone = 0; zone < zones.size(); ++zone)
    {
        const auto& sample = media.data->zones()[zone].sample;
        CHECK(sample->originalSamples().data() == zones[zone].sample->originalSamples().data());
        CHECK(sample->pcmFingerprint() == zones[zone].sample->pcmFingerprint());
        CHECK(sample->spectralFrames().size() <= 4);
        CHECK_NEAR(sample->spectralHopSeconds(), sample->spectralHop() / rate, 0);
    }
    CHECK(media.data->zones()[0].sample == media.data->zones()[1].sample);
    Voice voice; Voice::Controls controls; controls.pitchSemitones = 12; controls.timeRate = 0;
    voice.start(media.data.get(), Voice::Mode::Spectral, 60, 100, rate); voice.seek(0.5);
    const auto output = render(voice, controls, 16384);
    CHECK(power(output) > 0.01); CHECK_NEAR(measuredFrequency(output, 4096, 16384), 750, 10);
    CHECK(! Media::fromZones(zones, {}, true, Media::maxSpectralBytes + 1));
}

TEST_CASE("oscillator media: insufficient analysis budget preserves audible sample fallback and shared PCM accounting")
{
    const auto source = Sample::fromSamples(sine(), 1, rate); REQUIRE(source);
    const auto analyzed = Sample::withSpectralAnalysis(source.data, 64); REQUIRE(analyzed);
    const auto sharing = Media::fromZones({zoneFor(source.data), zoneFor(analyzed.data)}); REQUIRE(sharing);
    CHECK(sharing.data->pcmBytes() == source.data->originalSamples().size() * sizeof(float));
    const auto media = Media::fromZones({zoneFor(analyzed.data, Loop::Forward)}, {}, true, 0); REQUIRE(media);
    CHECK(! media.warnings.empty()); CHECK(media.data->spectralBytes() == 0);
    CHECK_NEAR(media.data->maximumSpectralHopSeconds(), 0, 0);
    CHECK(media.data->zones()[0].sample->originalSamples().data() == source.data->originalSamples().data());
    Voice spectral, sample; Voice::Controls controls; controls.pitchSemitones = 12;
    spectral.start(media.data.get(), Voice::Mode::Spectral, 60, 100, rate);
    sample.start(media.data.get(), Voice::Mode::Sample, 60, 100, rate);
    const auto a = render(spectral, controls, 16384), b = render(sample, controls, 16384);
    CHECK(power(a) > 0.1); CHECK_NEAR(measuredFrequency(a, 4096, 16384), 750, 10);
    for (size_t frame = 0; frame < a.size(); ++frame)
    {
        CHECK_NEAR(a[frame].left, b[frame].left, 0); CHECK_NEAR(a[frame].right, b[frame].right, 0);
    }
}

TEST_CASE("oscillator media: SFZ prepares spectral analysis after deduplicating complete file PCM")
{
    const juce::File directory("/tmp/sonora_tests/sonora_sfz_analysis_budget");
    writeOwnAudio(directory.getChildFile("Own Tone.wav"), sine(), 1);
    const auto bank = Media::fromSfzText("<region> key=60 sample=Own Tone.wav\n<region> key=61 sample=Own Tone.wav\n",
                                       directory, "Own spectral SFZ", true);
    REQUIRE(bank); CHECK(bank.warnings.empty()); REQUIRE(bank.data->zones().size() == 2);
    const auto& first = bank.data->zones()[0].sample;
    CHECK(first == bank.data->zones()[1].sample);
    CHECK(first->hasSpectralAnalysis());
    CHECK(bank.data->pcmBytes() == first->originalSamples().size() * sizeof(float));
    CHECK(bank.data->spectralBytes() == first->spectralFrames().size() * sizeof(Sample::SpectralFrame));
    CHECK(first->sourceFile() == directory.getChildFile("Own Tone.wav").getFullPathName().toStdString());
}

TEST_CASE("oscillator media: own WAV and AIFF files load independently and corrupt files fail nonfatally")
{
    const juce::File directory("/tmp/sonora_tests/sonora_oscillator_media");
    const auto source = sine(4096, 375, 2);
    for (const bool aiff : {false, true})
    {
        const auto file = directory.getChildFile(aiff ? "Independent.aiff" : "Independent.wav");
        writeOwnAudio(file, source, 2, aiff);
        const auto loaded = Media::fromFile(file);
        REQUIRE(loaded);
        REQUIRE(loaded.data->zones().size() == 1);
        const auto sample = loaded.data->zones()[0].sample;
        CHECK(sample->channels() == 2); CHECK(sample->sampleFrames() == 4096);
        CHECK_NEAR(sample->sampleRate(), rate, 0);
        CHECK(sample->sourceFile() == file.getFullPathName().toStdString());
        for (size_t frame = 0; frame < source.size(); ++frame) CHECK_NEAR(sample->originalSamples()[frame], source[frame], 2e-6);
    }
    const auto invalid = directory.getChildFile("Invalid.wav");
    REQUIRE(invalid.replaceWithText("Own malformed audio fixture"));
    CHECK(! Media::fromFile(invalid)); CHECK(! Media::fromFile(directory.getChildFile("Missing.wav")));
}

TEST_CASE("oscillator media: key velocity root tune gain pan and layering select bounded zones")
{
    const auto sample = Sample::fromSamples(ramp(), 1, rate);
    REQUIRE(sample);
    auto a = zoneFor(sample.data), b = a;
    a.lowKey = 60; a.highKey = 71; a.lowVelocity = 1; a.highVelocity = 63;
    a.pan = -1; a.gainDb = -6.0206f; a.tuneCents = 1200;
    b.lowKey = 60; b.highKey = 71; b.lowVelocity = 64; b.highVelocity = 127; b.pan = 1;
    const auto media = checkedMedia({a, b});
    Voice voice;
    Voice::Controls controls;
    voice.start(media.get(), Voice::Mode::Multisample, 60, 63, rate);
    CHECK(voice.activeLayers() == 1);
    CHECK_NEAR(voice.next(controls).left, 0, 0);
    const auto left = voice.next(controls);
    CHECK_NEAR(left.left, 0.05, 2e-6); CHECK_NEAR(left.right, 0, 0);
    voice.start(media.get(), Voice::Mode::Multisample, 60, 64, rate);
    voice.next(controls);
    const auto right = voice.next(controls);
    CHECK_NEAR(right.left, 0, 0); CHECK_NEAR(right.right, 0.05, 1e-7);
    voice.start(media.get(), Voice::Mode::Multisample, 59, 63, rate);
    CHECK(! voice.active());
    auto overlap = zoneFor(sample.data);
    const auto layers = checkedMedia(std::vector<Zone>(12, overlap));
    voice.start(layers.get(), Voice::Mode::Multisample, 60, 127, rate);
    CHECK(voice.activeLayers() == Voice::maxLayers);
    voice.next(controls);
    CHECK_NEAR(voice.next(controls).left, 0.4, 2e-7);
    overlap.highKey = -1;
    CHECK(! Media::fromZones({overlap}));
    CHECK(! Media::fromZones(std::vector<Zone>(Media::maxZones + 1, a)));
    overlap = a; overlap.gainDb = std::numeric_limits<float>::infinity();
    CHECK(! Media::fromZones({overlap}));
}

TEST_CASE("oscillator media: sample resampling pitch reverse offsets and explicit seek are exact")
{
    const auto sample = Sample::fromSamples(ramp(), 1, rate);
    REQUIRE(sample);
    auto zone = zoneFor(sample.data); zone.offset = 2; zone.end = 8;
    const auto media = checkedMedia({zone});
    Voice voice;
    Voice::Controls controls;
    voice.start(media.get(), Voice::Mode::Sample, 60, 127, rate * 2);
    for (int frame = 0; frame < 12; ++frame) CHECK_NEAR(voice.next(controls).left, (2 + frame * 0.5) * 0.05, 1e-7);
    CHECK(! voice.active()); CHECK_NEAR(voice.next(controls).left, 0, 0);
    voice.start(media.get(), Voice::Mode::Sample, 72, 127, rate);
    for (int frame = 0; frame < 3; ++frame) CHECK_NEAR(voice.next(controls).left, (2 + frame * 2) * 0.05, 1e-7);
    CHECK(! voice.active());
    voice.start(media.get(), Voice::Mode::Sample, 60, 127, rate);
    controls.reverse = true;
    for (int frame = 0; frame < 6; ++frame) CHECK_NEAR(voice.next(controls).left, (7 - frame) * 0.05, 1e-7);
    CHECK(! voice.active());
    voice.start(media.get(), Voice::Mode::Sample, 60, 127, rate);
    voice.seek(0.4);
    CHECK_NEAR(voice.next(controls).left, 0.2, 1e-7);
    voice.reset(); CHECK(! voice.active());
}

TEST_CASE("oscillator media: forward pingpong sustain and one-shot have explicit release semantics")
{
    const auto sample = Sample::fromSamples(ramp(10), 1, rate);
    REQUIRE(sample);
    Voice::Controls controls;
    for (const auto loop : {Loop::Forward, Loop::PingPong, Loop::Sustain})
    {
        auto zone = zoneFor(sample.data, loop); zone.loopStart = 2; zone.loopEnd = 6; zone.releaseSeconds = 1;
        const auto media = checkedMedia({zone});
        Voice voice; voice.start(media.get(), Voice::Mode::Sample, 60, 127, rate);
        const std::array<int, 12> forward{0, 1, 2, 3, 4, 5, 2, 3, 4, 5, 2, 3};
        const std::array<int, 12> alternate{0, 1, 2, 3, 4, 5, 4, 3, 2, 3, 4, 5};
        const auto& expected = loop == Loop::PingPong ? alternate : forward;
        for (int frame : expected) CHECK_NEAR(voice.next(controls).left, frame * 0.05, 1e-7);
        CHECK(voice.active());
        voice.release();
        render(voice, controls, 16);
        CHECK(voice.active() == (loop != Loop::Sustain));
    }
    auto looped = zoneFor(sample.data, Loop::Forward); looped.loopStart = 2; looped.loopEnd = 6;
    const auto fractional = checkedMedia({looped});
    Voice halfStep; halfStep.start(fractional.get(), Voice::Mode::Sample, 60, 127, rate * 2); halfStep.seek(5.5 / 9);
    CHECK_NEAR(halfStep.next(controls).left, 0.175, 1e-7);
    looped.loop = Loop::PingPong; looped.loopWhileHeld = true; looped.releaseSeconds = 1;
    const auto held = checkedMedia({looped});
    Voice sustained; sustained.start(held.get(), Voice::Mode::Sample, 60, 127, rate);
    render(sustained, controls, 8); sustained.release(); render(sustained, controls, 16); CHECK(! sustained.active());
    auto oneShot = zoneFor(sample.data, Loop::OneShot);
    const auto media = checkedMedia({oneShot});
    Voice voice; voice.start(media.get(), Voice::Mode::Sample, 60, 127, rate); voice.release();
    for (int frame = 0; frame < 10; ++frame) CHECK_NEAR(voice.next(controls).left, frame * 0.05, 1e-7);
    CHECK(! voice.active());
    oneShot.loop = Loop::None;
    const auto gated = checkedMedia({oneShot});
    voice.start(gated.get(), Voice::Mode::Sample, 60, 127, rate); voice.release();
    CHECK_NEAR(voice.next(controls).left, 0, 0); CHECK(! voice.active());
}

TEST_CASE("oscillator media: SFZ scopes note aliases velocity layers and relative names preserve inherited mapping")
{
    const juce::File directory("/tmp/sonora_tests/sonora_oscillator_media");
    writeOwnAudio(directory.getChildFile("Own Samples/Own Tone.wav"), ramp(), 1);
    const std::string text = R"SFZ(
        // Only independent fixtures are loaded by this test.
        <control> default_path=Own Samples/
        <global> sample=Own Tone.wav key=C4 volume=-3 amp_veltrack=0 ampeg_release=.5
        <group> lokey=B3 hikey=C#4 tune=25 lovel=1 hivel=63
        <region> key=C#4 volume=-6 loop_mode=loop_sustain loop_start=2 loop_end=7 end=13
        <region> lovel=64 hivel=127 pitch_keycenter=C4 pan=50
        <group> key=D4
        <region> sample="Own Tone.wav" amp_veltrack=100
        <global> sample=Own Tone.wav key=E4
        <region> transpose=-1 pitch_keytrack=0 offset=2
    )SFZ";
    const auto result = Media::fromSfzText(text, directory, "Own scoped map");
    REQUIRE(result); REQUIRE(result.data->zones().size() == 4); CHECK(result.warnings.empty());
    const auto& z = result.data->zones();
    CHECK(z[0].lowKey == 61); CHECK(z[0].highKey == 61); CHECK(z[0].rootKey == 61);
    CHECK(z[0].lowVelocity == 1); CHECK(z[0].highVelocity == 63); CHECK_NEAR(z[0].tuneCents, 25, 0);
    CHECK(z[0].end == 14); CHECK(z[0].loopEnd == 8); CHECK(z[0].loop == Loop::Sustain);
    CHECK_NEAR(z[0].gainDb, -6, 0); CHECK_NEAR(z[0].releaseSeconds, 0.5, 0);
    CHECK(z[1].lowKey == 59); CHECK(z[1].highKey == 61); CHECK(z[1].rootKey == 60);
    CHECK(z[1].lowVelocity == 64); CHECK(z[1].highVelocity == 127); CHECK_NEAR(z[1].pan, 0.5, 0);
    CHECK(z[2].lowKey == 62); CHECK(z[2].highKey == 62); CHECK_NEAR(z[2].tuneCents, 0, 0);
    CHECK_NEAR(z[2].velocityTracking, 1, 0);
    CHECK(z[3].lowKey == 64); CHECK_NEAR(z[3].gainDb, 0, 0); CHECK_NEAR(z[3].tuneCents, -100, 0);
    CHECK_NEAR(z[3].keyTracking, 0, 0); CHECK(z[3].offset == 2);
    CHECK(z[0].sample.get() == z[3].sample.get());
    const auto alternate = Media::fromSfzText("<region> sample=Own Samples/Own Tone.wav loop_mode=loop_sustain loop_type=alternate", directory);
    REQUIRE(alternate); CHECK(alternate.data->zones()[0].loop == Loop::PingPong); CHECK(alternate.data->zones()[0].loopWhileHeld);
    const auto sfz = directory.getChildFile("Own map.sfz");
    REQUIRE(sfz.replaceWithText(juce::String::fromUTF8(text.c_str())));
    const auto loaded = Media::fromFile(sfz); REQUIRE(loaded); CHECK(loaded.data->zones().size() == 4);
}

TEST_CASE("oscillator media: unquoted double path separators are retained while trailing comments remain comments")
{
    const juce::File directory("/tmp/sonora_tests/sonora_media_double_separator");
    REQUIRE(directory.createDirectory());
    writeOwnAudio(directory.getChildFile("Own Samples/Own Tone.wav"), ramp(), 1);
    const auto loaded = Media::fromSfzText("// Own synthetic fixture\n<group> amp_veltrack=0\n"
        "<region> key=60 sample=Own Samples//Own Tone.wav // an actual trailing comment\n"
        "<region> key=61 sample=Own Samples/Own Tone.wav\n", directory);
    REQUIRE(loaded); CHECK(loaded.warnings.empty()); REQUIRE(loaded.data->zones().size() == 2);
    CHECK(loaded.data->zones()[0].rootKey == 60); CHECK(loaded.data->zones()[1].rootKey == 61);
    CHECK(loaded.data->zones()[0].sample->originalSamples() == loaded.data->zones()[1].sample->originalSamples());
}

TEST_CASE("oscillator media: SFZ unsupported missing corrupt and oversized regions report bounded partial results")
{
    const juce::File directory("/tmp/sonora_tests/sonora_oscillator_media");
    writeOwnAudio(directory.getChildFile("Own.wav"), ramp(), 1);
    const auto result = Media::fromSfzText("<region> sample=Own.wav key=60 unknown_opcode=4 <region> sample=Missing.wav <region> sample=Own.wav tune=nan", directory);
    REQUIRE(result); CHECK(result.data->zones().size() == 1); CHECK(result.warnings.size() == 3);
    CHECK(! Media::fromSfzText("<region> sample=Own.wav key=999", directory));
    CHECK(! Media::fromSfzText("<region> sample=Own.wav loop_start=8 loop_end=2", directory));
    CHECK(! Media::fromSfzText("<region> sample='unterminated", directory));
    CHECK(! Media::fromSfzText("/* unterminated", directory));
    CHECK(! Media::fromSfzText(std::string(Media::maxSfzBytes + 1, ' '), directory));
    CHECK(! Media::fromSfzText(std::string("\xc0\x80", 2), directory));
    const auto invalid = directory.getChildFile("Invalid.sfz");
    REQUIRE(invalid.replaceWithData("\xc0\x80", 2)); CHECK(! Media::fromSfz(invalid));
    CHECK(! Media::fromSfzText(std::string("<region>\0 sample=Own.wav", 23), directory));
    std::string many;
    for (int zone = 0; zone < Media::maxZones + 1; ++zone) many += "<region> sample=Own.wav key=60 ";
    const auto bounded = Media::fromSfzText(many, directory);
    REQUIRE(bounded); CHECK(bounded.data->zones().size() == Media::maxZones); CHECK(! bounded.warnings.empty());
}

TEST_CASE("oscillator media: granular pitch and scan speed are independent with bounded deterministic grains")
{
    const auto sample = Sample::fromSamples(sine(), 1, rate);
    REQUIRE(sample);
    auto zone = zoneFor(sample.data, Loop::Forward);
    const auto media = checkedMedia({zone});
    const auto owners = sample.data.use_count();
    Voice a, b;
    Voice::Controls controls;
    controls.timeRate = 0; controls.grainSeconds = 0.5; controls.density = 0.1;
    controls.densityMode = Voice::DensityMode::Hertz; controls.window = Voice::Window::Rectangle;
    a.start(media.get(), Voice::Mode::Granular, 60, 127, rate, 42); a.seek(0.5);
    const auto native = render(a, controls, 12000);
    CHECK(power(native) > 0.01); CHECK_NEAR(a.position(), 0.5 * 32767 / 32768, 1e-9);
    CHECK_NEAR(measuredFrequency(native, 3000, 11000), 375, 8);
    b.start(media.get(), Voice::Mode::Granular, 60, 127, rate, 42); b.seek(0.5);
    controls.pitchSemitones = 12;
    const auto pitched = render(b, controls, 12000);
    CHECK_NEAR(measuredFrequency(pitched, 3000, 11000), 750, 8);
    CHECK_NEAR(a.position(), b.position(), 0);
    controls.pitchSemitones = 0; controls.timeRate = 0.5; controls.positionJitter = 0.02f;
    controls.grainSeconds = 0.04; controls.density = 8; controls.jumpStart = true;
    controls.densityMode = Voice::DensityMode::GrainCount; controls.window = Voice::Window::Hann;
    controls.pitchJitter = 3; controls.panJitter = 0.7f; controls.lengthJitter = 0.2f;
    controls.directionJitter = 0.2f; controls.gainJitter = 0.3f; controls.grainLimit = 5;
    a.start(media.get(), Voice::Mode::Granular, 60, 127, rate, 99);
    b.start(media.get(), Voice::Mode::Granular, 60, 127, rate, 99);
    for (int frame = 0; frame < 17000; ++frame)
    {
        const auto first = a.next(controls), second = b.next(controls);
        CHECK_NEAR(first.left, second.left, 0); CHECK_NEAR(first.right, second.right, 0);
        CHECK(a.activeGrains() <= 5); CHECK(std::isfinite(first.left)); CHECK(std::isfinite(first.right));
    }
    CHECK(sample.data.use_count() == owners);
    a.release(); a.next(controls); CHECK(! a.active());
}

TEST_CASE("oscillator media: spectral analysis resynthesizes stereo partials with independent pitch and frozen scan")
{
    const auto source = sine(32768, 375, 2);
    const auto sample = Sample::fromSamples(source, 2, rate, "Own spectral sine", true);
    REQUIRE(sample); CHECK(sample.data->hasSpectralAnalysis());
    CHECK(sample.data->spectralFrames().size() <= Sample::maxSpectralFrames);
    const auto& frame = sample.data->spectralFrames()[16];
    CHECK_NEAR(frame.partial[0].frequency, 375, 0.1);
    CHECK_NEAR(frame.partial[0].amplitudeLeft, 0.5, 1e-4);
    CHECK_NEAR(frame.partial[0].amplitudeRight, 0.5, 1e-4);
    auto zone = zoneFor(sample.data, Loop::Forward);
    const auto media = checkedMedia({zone});
    Voice voice;
    Voice::Controls controls; controls.timeRate = 0;
    voice.start(media.get(), Voice::Mode::Spectral, 60, 127, rate); voice.seek(0.5);
    const auto audio = render(voice, controls, 16000);
    CHECK_NEAR(measuredFrequency(audio, 2000, 15000), 375, 4); CHECK(power(audio) > 0.1);
    CHECK_NEAR(voice.position(), 0.5 * 32767 / 32768, 1e-9);
    double stereoDifference = 0;
    for (const auto& value : audio) stereoDifference += std::abs(value.left - value.right);
    CHECK(stereoDifference > 100);
    controls.pitchSemitones = 12;
    voice.start(media.get(), Voice::Mode::Spectral, 60, 127, rate); voice.seek(0.5);
    const auto octave = render(voice, controls, 16000);
    CHECK_NEAR(measuredFrequency(octave, 2000, 15000), 750, 4); CHECK(power(octave) > 0.1);
    const auto plain = Media::fromSamples(source, 2, rate); REQUIRE(plain);
    voice.start(plain.data.get(), Voice::Mode::Spectral, 60, 127, rate);
    CHECK(voice.active()); CHECK_NEAR(voice.next(controls).left, 0, 0);
}

TEST_CASE("oscillator media: invalid controls remain finite and callback state is independent of caller partitioning")
{
    const auto media = Media::fromSamples(sine(), 1, rate, {}, true); REQUIRE(media);
    Voice::Controls controls;
    controls.pitchSemitones = std::numeric_limits<double>::infinity(); controls.timeRate = std::numeric_limits<double>::quiet_NaN();
    controls.grainSeconds = -10; controls.density = 100000; controls.grainLimit = 1000;
    controls.panJitter = std::numeric_limits<float>::quiet_NaN(); controls.windowSkew = 1;
    for (const auto mode : {Voice::Mode::Sample, Voice::Mode::Multisample, Voice::Mode::Granular, Voice::Mode::Spectral})
    {
        Voice first, second;
        first.start(media.data.get(), mode, 60, 127, rate, 7);
        second.start(media.data.get(), mode, 60, 127, rate, 7);
        const auto output = render(first, controls, 4096);
        CHECK(power(output) >= 0);
        int position = 0;
        while (position < 4096)
        {
            const int end = std::min(position + 97, 4096);
            for (; position < end; ++position)
            {
                const auto frame = second.next(controls);
                CHECK_NEAR(frame.left, output[static_cast<size_t>(position)].left, 0);
                CHECK_NEAR(frame.right, output[static_cast<size_t>(position)].right, 0);
            }
        }
        CHECK(first.activeGrains() <= Voice::maxGrains);
    }
}

TEST_CASE("oscillator media: Serum sampler profile (multisample gain, power velocity law, dB-linear release)")
{
    const auto source = Sample::fromSamples(sine(48000, 375), 1, rate); REQUIRE(source);
    auto zone = zoneFor(source.data, Loop::Forward);
    zone.velocityTracking = 0.35f; zone.releaseSeconds = 0.25;
    const auto media = checkedMedia({zone});
    const auto level = [&](bool serum, int velocity, Voice::Mode mode) {
        Voice voice; voice.start(media.get(), mode, 60, velocity, rate);
        Voice::Controls controls; controls.serumProfile = serum;
        const auto audio = render(voice, controls, 9600);
        double sum = 0; for (size_t i = 4800; i < audio.size(); ++i) sum += audio[i].left * audio[i].left;
        return std::sqrt(sum / 4800);
    };
    // Measured on the hosted reference: multisamples 2.84x louder than Sample mode, velocity t scales by (v / 127)^(2 t).
    CHECK_NEAR(level(true, 127, Voice::Mode::Multisample) / level(false, 127, Voice::Mode::Multisample), 2.84, 0.01);
    CHECK_NEAR(level(true, 127, Voice::Mode::Sample) / level(false, 127, Voice::Mode::Sample), 1, 1e-6);
    CHECK_NEAR(level(true, 64, Voice::Mode::Multisample) / level(true, 127, Voice::Mode::Multisample), std::pow(64.0 / 127, 0.7), 0.005);
    // Release: -80 dB at the release time (linear in dB), override seconds replace the zone's own.
    Voice voice; voice.start(media.get(), Voice::Mode::Multisample, 60, 127, rate);
    Voice::Controls controls; controls.serumProfile = true; controls.releaseOverride = 0.5;
    render(voice, controls, 4800);
    voice.release();
    const auto tail = render(voice, controls, 24000);
    const auto peak = [&](int from) { float p = 0; for (int i = from; i < from + 480; ++i) p = std::max(p, std::abs(tail[static_cast<size_t>(i)].left)); return p; };
    CHECK_NEAR(20 * std::log10(peak(12000) / peak(0)), -40, 1.5); // half the 0.5 s override
    CHECK(voice.active());
    render(voice, controls, 4000);
    CHECK(! voice.active());
}
