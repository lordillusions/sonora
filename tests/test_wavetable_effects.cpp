#include "devices/native/SerumDistortion.h"
#include "TestFramework.h"
#include "devices/native/WavetableEffects.h"
#include "dsp/Dsp.h"

#include <algorithm>
#include <complex>
#include <cstring>
#include <limits>
#include <set>

using namespace sonora;

namespace
{
using E = devices::WavetableEffects;
using Type = E::Type;
using Audio = std::array<std::vector<float>, 6>;
constexpr double rate = 48000;

E::Module module(E::ModuleId id, Type type)
{
    E::Module result;
    result.id = id; result.type = type;
    for (const auto& parameter : E::parameterDescriptors(type)) result.values.push_back(parameter.initial);
    return result;
}
void value(E::Module& effect, std::string_view parameter, float plain)
{
    const int index = E::parameterIndex(effect.type, parameter);
    REQUIRE(index >= 0);
    effect.values[static_cast<size_t>(index)] = plain;
}
Audio blank(int frames)
{
    Audio audio;
    for (auto& channel : audio) channel.resize(static_cast<size_t>(frames), 0);
    return audio;
}
Audio tone(int frames, double frequency = 375, float amplitude = 0.1f)
{
    auto audio = blank(frames);
    for (int frame = 0; frame < frames; ++frame)
    {
        const float sample = amplitude * static_cast<float>(std::sin(2 * dsp::kPi * frequency * frame / rate));
        audio[0][static_cast<size_t>(frame)] = sample;
        audio[1][static_cast<size_t>(frame)] = sample * 0.75f;
    }
    return audio;
}
Audio process(E& effects, Audio input, int block = 512)
{
    const int count = static_cast<int>(input[0].size());
    for (int start = 0; start < count; start += block)
    {
        std::array<E::StereoBuffer, E::busCount> buses{};
        for (int bus = 0; bus < E::busCount; ++bus)
            buses[static_cast<size_t>(bus)] = {input[static_cast<size_t>(bus * 2)].data() + start, input[static_cast<size_t>(bus * 2 + 1)].data() + start};
        effects.process(buses, std::min(block, count - start));
    }
    return input;
}
void prepare(E& effects, const E::Configuration& configuration)
{
    std::string error;
    REQUIRE(effects.prepare(configuration, rate, error)); CHECK(error.empty());
}
E::Configuration single(E::Module effect)
{
    E::Configuration result;
    result.buses[0].push_back(std::move(effect));
    return result;
}
double difference(const Audio& a, const Audio& b)
{
    double maximum = 0;
    for (size_t channel = 0; channel < a.size(); ++channel)
        for (size_t sample = 0; sample < a[channel].size(); ++sample) maximum = std::max(maximum, static_cast<double>(std::abs(a[channel][sample] - b[channel][sample])));
    return maximum;
}
double energy(const std::vector<float>& data, int start = 0, int count = -1)
{
    if (count < 0) count = static_cast<int>(data.size()) - start;
    double sum = 0;
    for (int sample = start; sample < start + count; ++sample) sum += static_cast<double>(data[static_cast<size_t>(sample)]) * data[static_cast<size_t>(sample)];
    return sum / count;
}
double magnitude(const std::vector<float>& data, double frequency, int start, int count)
{
    double real = 0, imaginary = 0;
    for (int sample = start; sample < start + count; ++sample)
    {
        const double phase = 2 * dsp::kPi * frequency * sample / rate;
        real += data[static_cast<size_t>(sample)] * std::cos(phase);
        imaginary -= data[static_cast<size_t>(sample)] * std::sin(phase);
    }
    return 2 * std::hypot(real, imaginary) / count;
}
void finite(const Audio& audio)
{
    for (const auto& channel : audio) for (float sample : channel)
    { CHECK(std::isfinite(sample)); CHECK(std::abs(sample) <= 128); }
}
}

TEST_CASE("wavetable effects: observed type codes stable identities and plain control snapshots remain explicit")
{
    CHECK(! E::typeFromCode(13)); CHECK(! E::typeFromCode(15)); CHECK(! E::typeFromCode(-1));
    for (int code = 0; code <= 14; ++code)
    {
        if (code == 13) continue;
        REQUIRE(E::typeFromCode(code));
        const auto type = *E::typeFromCode(code);
        CHECK(! E::typeName(type).empty());
        const auto parameters = E::parameterDescriptors(type);
        REQUIRE(parameters.size() >= 2); CHECK(parameters.size() <= E::maxParameters);
        std::set<std::string_view> ids;
        for (const auto& parameter : parameters)
        {
            CHECK(ids.insert(parameter.id).second); CHECK(parameter.minimum <= parameter.initial); CHECK(parameter.initial <= parameter.maximum);
        }
    }
    E effects;
    auto configuration = single(module(111, Type::Utility));
    prepare(effects, configuration);
    CHECK(effects.moduleType(111) == Type::Utility); CHECK(! effects.moduleType(999));
    REQUIRE(effects.setParameter(111, E::outputParameter, 6));
    CHECK_NEAR(effects.getParameter(111, E::outputParameter), 6, 0);
    CHECK(! effects.setParameter(111, 999, 1)); CHECK(! effects.setParameter(999, 0, 1));
    CHECK(! effects.setParameter(111, 1, std::numeric_limits<float>::quiet_NaN()));
    CHECK(effects.setEnabled(111, false)); CHECK(! effects.enabled(111));
    const auto snapshot = effects.snapshotConfiguration();
    CHECK(snapshot.buses[0][0].id == 111); CHECK(! snapshot.buses[0][0].enabled); CHECK_NEAR(snapshot.buses[0][0].values[1], 6, 0);
    CHECK(effects.moduleIds().size() == 1); CHECK(effects.moduleIds()[0] == 111);
    REQUIRE(effects.setRoutingParameter(0, 0.25f)); REQUIRE(effects.setRoutingParameter(1, 0.75f));
    CHECK_NEAR(effects.getRoutingParameter(0), 0.25f, 0); CHECK_NEAR(effects.getRoutingParameter(1), 0.75f, 0);
    CHECK_NEAR(effects.routing().mainToBus1, 0.25f, 0); CHECK_NEAR(effects.routing().mainToBus2, 0.75f, 0);
    CHECK(! effects.setRoutingParameter(-1, 1)); CHECK(! effects.setRoutingParameter(6, 1));
    CHECK(! effects.setRoutingParameter(0, std::numeric_limits<float>::quiet_NaN()));
    REQUIRE(effects.setRoutingParameter(4, 99)); CHECK_NEAR(effects.getRoutingParameter(4), 2, 0);
}

TEST_CASE("wavetable effects: distortion bias shapes preserve true silence and reset clears their tone state")
{
    for (int shape = 0; shape < 6; ++shape)
        for (float bias : {-1.0f, -0.3f, 0.0f, 0.7f, 1.0f})
        {
            auto distortion = module(17, Type::Distortion);
            value(distortion, "Shape", static_cast<float>(shape)); value(distortion, "Bias", bias); value(distortion, "Drive", 36);
            E effects; prepare(effects, single(distortion));
            const auto silence = process(effects, blank(1024), 73);
            CHECK(difference(silence, blank(1024)) < 1e-20);
            const auto sound = process(effects, tone(512));
            CHECK(energy(sound[0]) > 1e-7);
            effects.reset();
            CHECK(difference(process(effects, blank(1024)), blank(1024)) < 1e-20);
        }
}

TEST_CASE("wavetable effects: reset invalidates wrapped delay and convolution history without leaking previous audio")
{
    std::vector<float> samples(4096, 0);
    samples[0] = 1; samples[127] = 0.25f; samples[2000] = -0.13f;
    const auto impulse = devices::ConvolutionIR::fromSamples(samples, 1, rate); REQUIRE(impulse);
    for (auto type : {Type::Delay, Type::Flanger, Type::Chorus, Type::HyperDimension, Type::Reverb, Type::Convolution})
    {
        auto effect = module(12, type);
        if (type == Type::Convolution) effect.impulse = impulse.data;
        if (type == Type::Delay) { value(effect, "Time", 0.00217f); value(effect, "RightTime", 0.00319f); }
        const auto graph = single(effect);
        E warmed, fresh; prepare(warmed, graph); prepare(fresh, graph);
        process(warmed, tone(32768));
        warmed.reset();
        const auto input = tone(8192, 515);
        CHECK(difference(process(warmed, input, 97), process(fresh, input, 512)) < 2e-6);
    }
}

TEST_CASE("wavetable effects: every type has exact zero-mix and disabled passthrough with equal-power intermediate mixing")
{
    const auto ir = devices::ConvolutionIR::fromSamples(std::vector<float>{1, 0.5f}, 1, rate); REQUIRE(ir);
    const auto input = tone(2048);
    for (int code = 0; code <= 14; ++code)
    {
        if (code == 13) continue;
        auto effect = module(1, *E::typeFromCode(code));
        value(effect, "Mix", 0); value(effect, "Output", 12);
        if (effect.type == Type::Convolution) effect.impulse = ir.data;
        E graph; auto configuration = single(effect); configuration.alignLatency = false; prepare(graph, configuration);
        CHECK_NEAR(difference(process(graph, input), input), 0, 0);
        REQUIRE(graph.setParameter(1, E::mixParameter, 1)); REQUIRE(graph.setEnabled(1, false));
        CHECK_NEAR(difference(process(graph, input), input), 0, 0);
    }
    auto identity = module(77, Type::Utility); value(identity, "Mix", 0.5f);
    E graph; prepare(graph, single(identity));
    const auto mixed = process(graph, input);
    for (size_t frame = 0; frame < input[0].size(); ++frame) CHECK_NEAR(mixed[0][frame], input[0][frame] * std::sqrt(2.0), 2e-7);
}

TEST_CASE("wavetable effects: shaped mix matches measured steady reference coefficients while legacy graphs retain equal power")
{
    const auto input = tone(2048);
    for (const auto& [mix, measuredScale] : std::array<std::pair<float, double>, 3>{{{0.25f, 1.2714198}, {0.5f, 1.749971}, {0.75f, 1.9785089}}})
    {
        auto effect = module(1, Type::Utility); value(effect, "Mix", mix); value(effect, "Output", 6.0206f);
        auto configuration = single(effect); configuration.mixLaw = E::MixLaw::Shaped;
        E graph; prepare(graph, configuration); const auto output = process(graph, input);
        CHECK(graph.snapshotConfiguration().mixLaw == E::MixLaw::Shaped);
        for (int channel = 0; channel < 2; ++channel)
            for (size_t frame = 0; frame < input[static_cast<size_t>(channel)].size(); ++frame)
                CHECK_NEAR(output[static_cast<size_t>(channel)][frame], input[static_cast<size_t>(channel)][frame] * measuredScale, 6e-6);
    }
    auto invalid = single(module(1, Type::Utility)); invalid.mixLaw = static_cast<E::MixLaw>(99);
    E graph; std::string error; CHECK(! graph.prepare(invalid, rate, error)); CHECK(! error.empty());
}

TEST_CASE("wavetable effects: effect ordering changes nonlinear audio while module identities stay stable")
{
    auto gain = module(41, Type::Utility); value(gain, "Output", 12);
    auto drive = module(83, Type::Distortion); value(drive, "Drive", 12);
    E::Configuration first, second;
    first.buses[0] = {gain, drive}; second.buses[0] = {drive, gain};
    E a, b; prepare(a, first); prepare(b, second);
    const auto source = tone(8192);
    CHECK(difference(process(a, source), process(b, source)) > 0.1);
    const auto snapshot = b.snapshotConfiguration();
    CHECK(snapshot.buses[0][0].id == 83); CHECK(snapshot.buses[0][1].id == 41);
}

TEST_CASE("wavetable effects: three bus sends returns and individual outputs follow explicit routing gains")
{
    E::Configuration configuration;
    auto auxiliary = module(17, Type::Utility); value(auxiliary, "Output", 6.0206f);
    configuration.buses[1] = {auxiliary}; configuration.routing = {0.5f, 0.25f, 0.25f, 0.5f, 0.75f};
    E effects; prepare(effects, configuration);
    auto input = blank(128);
    for (size_t frame = 0; frame < input[0].size(); ++frame)
        for (int bus = 0; bus < 3; ++bus) input[static_cast<size_t>(bus * 2)][frame] = input[static_cast<size_t>(bus * 2 + 1)][frame] = static_cast<float>(bus + 1) * 0.1f;
    const auto output = process(effects, input);
    CHECK_NEAR(output[2][64], 0.5, 2e-6); CHECK_NEAR(output[4][64], 0.325, 2e-6);
    CHECK_NEAR(output[0][64], (0.1 + 0.5 * 0.25 + 0.325 * 0.5) * 0.75, 2e-6);
    CHECK(effects.setRouting({0, 0, 0, 0, 1}));
    const auto dry = process(effects, input); CHECK_NEAR(dry[0][64], 0.1, 1e-7);
    CHECK(! effects.setRouting({0, 0, 0, 0, std::numeric_limits<float>::infinity()}));
}

TEST_CASE("wavetable effects: delay echoes sample times tempo divisions and reset clears all tails")
{
    auto effect = module(1, Type::Delay);
    value(effect, "Time", 0.01f); value(effect, "RightTime", 0.015f); value(effect, "Feedback", 0.5f);
    E graph; prepare(graph, single(effect));
    auto source = blank(4096); source[0][0] = 1; source[1][0] = 1;
    const auto output = process(graph, source);
    CHECK_NEAR(energy(output[0], 0, 479), 0, 0); CHECK(output[0][480] > 0.5f);
    CHECK_NEAR(energy(output[1], 0, 719), 0, 0); CHECK(output[1][720] > 0.5f);
    CHECK(energy(output[0], 960, 500) > 1e-6);
    graph.reset(); CHECK_NEAR(energy(process(graph, blank(4096))[0]), 0, 0);
    value(effect, "Sync", 1); value(effect, "Beats", 0.25f); value(effect, "RightBeats", 0.5f);
    prepare(graph, single(effect)); graph.setTempo(120);
    source = blank(15000); source[0][0] = 1; source[1][0] = 1;
    const auto synced = process(graph, source, 97);
    CHECK_NEAR(energy(synced[0], 0, 6000), 0, 0); CHECK(synced[0][6000] > 0.5f);
    CHECK_NEAR(energy(synced[1], 0, 12000), 0, 0); CHECK(synced[1][12000] > 0.5f);
}

TEST_CASE("wavetable effects: prepared partitioned stereo convolution reproduces the independent impulse across partitions")
{
    std::vector<float> pcm(600 * 2, 0);
    pcm[0] = 1; pcm[2] = 0.5f; pcm[258] = -0.25f; pcm[1198] = 0.1f;
    pcm[1] = 0.75f; pcm[515] = 0.2f;
    const auto ir = devices::ConvolutionIR::fromSamples(pcm, 2, rate, "Own stereo impulse"); REQUIRE(ir);
    CHECK(ir.data->originalSamples() == pcm); CHECK(ir.data->frames() == 600); CHECK(ir.data->channels() == 2);
    auto effect = module(16, Type::Convolution); effect.impulse = ir.data;
    E graph; prepare(graph, single(effect)); CHECK(graph.latencySamples() == E::convolutionPartition);
    const auto owners = ir.data.use_count();
    auto source = blank(1024); source[0][0] = 1; source[1][0] = 1;
    const auto output = process(graph, source, 97);
    CHECK(ir.data.use_count() == owners);
    for (int frame = 0; frame < 1024; ++frame)
        for (int channel = 0; channel < 2; ++channel)
        {
            const int delayed = frame - E::convolutionPartition;
            const float expected = delayed >= 0 && delayed < 600 ? pcm[static_cast<size_t>(delayed * 2 + channel)] : 0;
            CHECK_NEAR(output[static_cast<size_t>(channel)][static_cast<size_t>(frame)], expected, 3e-6);
        }
    graph.reset(); CHECK_NEAR(energy(process(graph, blank(1024))[0]), 0, 0);
}

TEST_CASE("wavetable effects: nonuniform convolution preserves stereo taps at head tail and long impulse boundaries")
{
    constexpr int frames = 300137;
    std::vector<float> pcm(static_cast<size_t>(frames * 2), 0);
    for (int frame : {0, 127, 128, 2047, 2048, 2049, 3071, 3072, 70003, frames - 1})
    {
        pcm[static_cast<size_t>(frame * 2)] = frame % 2 == 0 ? 0.3f : -0.2f;
        pcm[static_cast<size_t>(frame * 2 + 1)] = frame % 3 == 0 ? -0.15f : 0.25f;
    }
    const auto impulse = devices::ConvolutionIR::fromSamples(pcm, 2, rate, "Own long stereo impulse"); REQUIRE(impulse);
    auto convolution = module(92, Type::Convolution); convolution.impulse = impulse.data;
    E graph; prepare(graph, single(convolution)); CHECK(graph.latencySamples() == 128);
    CHECK(graph.preparedBytes() < 12 * 1024 * 1024);
    auto source = blank(frames + 1024); source[0][0] = 1; source[1][0] = 1;
    const auto output = process(graph, source, 97);
    double error = 0;
    for (int frame = 0; frame < frames + 1024; ++frame)
        for (int channel = 0; channel < 2; ++channel)
        {
            const int delayed = frame - E::convolutionPartition;
            const float expected = delayed >= 0 && delayed < frames ? pcm[static_cast<size_t>(delayed * 2 + channel)] : 0;
            error = std::max(error, static_cast<double>(std::abs(output[static_cast<size_t>(channel)][static_cast<size_t>(frame)] - expected)));
        }
    CHECK(error < 3e-6);
    graph.reset(); CHECK_NEAR(energy(process(graph, blank(8192))[0]), 0, 0);
}

TEST_CASE("wavetable effects: nonuniform convolution matches direct convolution across the partition transition")
{
    constexpr int frames = 4099, inputFrames = 31, outputFrames = frames + inputFrames + 256;
    std::vector<float> pcm(frames);
    for (int frame = 0; frame < frames; ++frame) pcm[static_cast<size_t>(frame)] = static_cast<float>(0.015 * std::exp(-frame / 1400.0) * std::cos(frame * 0.37));
    const auto impulse = devices::ConvolutionIR::fromSamples(pcm, 1, rate); REQUIRE(impulse);
    auto convolution = module(7, Type::Convolution); convolution.impulse = impulse.data;
    E graph; prepare(graph, single(convolution));
    auto source = blank(outputFrames);
    for (int frame = 0; frame < inputFrames; ++frame)
    {
        source[0][static_cast<size_t>(frame)] = static_cast<float>(0.1 * std::sin(frame * 0.7));
        source[1][static_cast<size_t>(frame)] = static_cast<float>(0.2 * std::cos(frame * 0.3));
    }
    const auto output = process(graph, source, 17);
    double error = 0;
    for (int frame = 0; frame < outputFrames; ++frame) for (int channel = 0; channel < 2; ++channel)
    {
        double expected = 0;
        for (int tap = 0; tap < inputFrames; ++tap)
        {
            const int irFrame = frame - 128 - tap;
            if (irFrame >= 0 && irFrame < frames) expected += static_cast<double>(source[static_cast<size_t>(channel)][static_cast<size_t>(tap)]) * pcm[static_cast<size_t>(irFrame)];
        }
        error = std::max(error, std::abs(output[static_cast<size_t>(channel)][static_cast<size_t>(frame)] - expected));
    }
    CHECK(error < 3e-6);
    graph.reset(); E fresh; prepare(fresh, single(convolution));
    CHECK(difference(process(graph, source, 97), process(fresh, source, 1024)) < 3e-6);
}

TEST_CASE("wavetable effects: long source-rate impulses resample into bounded prepared stages and preserve endpoints")
{
    constexpr int frames = 275713;
    std::vector<float> pcm(frames, 0); pcm[0] = 1; pcm[2048] = 0.25f; pcm.back() = -0.1f;
    const auto impulse = devices::ConvolutionIR::fromSamples(pcm, 1, 44100); REQUIRE(impulse);
    auto convolution = module(6, Type::Convolution); convolution.impulse = impulse.data;
    E graph; prepare(graph, single(convolution));
    const int renderedFrames = static_cast<int>(std::ceil(frames * rate / 44100)) + 256;
    auto source = blank(renderedFrames); source[0][0] = 1; source[1][0] = 1;
    const auto output = process(graph, source, 512);
    double error = 0;
    const double ratio = 44100 / rate;
    for (int frame = 0; frame < renderedFrames; ++frame)
    {
        const double position = (frame - 128) * ratio;
        double expected = 0;
        if (position >= 0 && position < frames)
        {
            const auto first = static_cast<size_t>(position), second = std::min(first + 1, pcm.size() - 1);
            expected = (pcm[first] + (pcm[second] - pcm[first]) * (position - static_cast<double>(first))) * ratio;
        }
        error = std::max(error, std::abs(output[0][static_cast<size_t>(frame)] - expected));
    }
    CHECK(error < 3e-6);
    CHECK(graph.preparedBytes() < 12 * 1024 * 1024);
}

TEST_CASE("wavetable effects: filter attenuation EQ gain and frequency shifting have measured spectral behavior")
{
    auto filtered = module(1, Type::Filter); value(filtered, "Cutoff", 750); value(filtered, "Resonance", 0); value(filtered, "Slope", 1);
    E graph; prepare(graph, single(filtered));
    auto source = tone(12288, 375);
    for (size_t frame = 0; frame < source[0].size(); ++frame) source[0][frame] += 0.1f * static_cast<float>(std::sin(2 * dsp::kPi * 6000 * static_cast<double>(frame) / rate));
    const auto output = process(graph, source);
    CHECK(magnitude(output[0], 375, 8192, 4096) > 0.05); CHECK(magnitude(output[0], 6000, 8192, 4096) < 0.002);
    auto equalizer = module(2, Type::Eq); value(equalizer, "Band1Type", 1); value(equalizer, "Band1Freq", 3000); value(equalizer, "Band1Gain", 12); value(equalizer, "Band1Q", 4);
    prepare(graph, single(equalizer));
    const auto boosted = process(graph, tone(12288, 3000));
    CHECK_NEAR(magnitude(boosted[0], 3000, 8192, 4096), 0.1 * std::pow(10.0, 12.0 / 20), 2e-4);
    auto cut = module(4, Type::Eq); value(cut, "Band1Type", 3); value(cut, "Band1Freq", 1000); value(cut, "Band1Q", 0.1f + 18.889f / 16);
    value(cut, "Band2Type", 4); value(cut, "Band2Freq", 8000); value(cut, "Band2Q", 0.7071f);
    prepare(graph, single(cut)); // measured: band 1 high pass / band 2 low pass are plain RBJ biquads (gain ignored)
    const auto lowTone = process(graph, tone(12288, 100)), midTone = process(graph, tone(12288, 3000));
    CHECK_NEAR(magnitude(lowTone[0], 100, 8192, 4096) / 0.1, std::pow(10.0, -40.03 / 20), 3e-3);
    CHECK_NEAR(magnitude(midTone[0], 3000, 8192, 4096) / 0.1, 1, 0.1);
    auto shifted = module(3, Type::FrequencyShift); value(shifted, "Shift", 375);
    prepare(graph, single(shifted)); CHECK(graph.latencySamples() == 31);
    const auto shiftedAudio = process(graph, tone(24576, 3000));
    CHECK(magnitude(shiftedAudio[0], 3375, 16384, 8192) > 0.08);
    CHECK(magnitude(shiftedAudio[0], 2625, 16384, 8192) < 0.01);
    CHECK(magnitude(shiftedAudio[0], 3000, 16384, 8192) < 0.01);
}

TEST_CASE("wavetable effects: single linked compressor follows threshold ratio and multiband unity remains spectrally flat")
{
    auto effect = module(1, Type::Compressor);
    value(effect, "Threshold", -12.0412f); value(effect, "Ratio", 4); value(effect, "Attack", 0.0001f); value(effect, "Knee", 0);
    E graph; prepare(graph, single(effect));
    auto source = blank(4096); std::fill(source[0].begin(), source[0].end(), 0.5f); std::fill(source[1].begin(), source[1].end(), 0.25f);
    const auto compressed = process(graph, source);
    CHECK_NEAR(compressed[0][4000], 0.25 * std::pow(2.0, 0.25), 2e-5);
    CHECK_NEAR(compressed[1][4000], compressed[0][4000] * 0.5, 2e-6);
    value(effect, "Multiband", 1); value(effect, "Ratio", 1);
    for (double frequency : {187.5, 1500.0, 9000.0})
    {
        prepare(graph, single(effect));
        const auto banded = process(graph, tone(16384, frequency));
        CHECK_NEAR(magnitude(banded[0], frequency, 8192, 8192), 0.1, 0.002);
    }
}

TEST_CASE("wavetable effects: Instant Peak compressor reproduces the measured steady gain, limiter peak and linked stereo")
{
    // Reference measurements: threshold (0.5)^3 amplitude (-18.06 dB), ratio 4, attack tau 45 ms / release 90 ms, 0.8 sine at 1 kHz settles at -9.28 dB.
    auto effect = module(1, Type::Compressor);
    value(effect, "Style", 1); value(effect, "Threshold", 60 * std::log10(0.5f)); value(effect, "Ratio", 4);
    value(effect, "Attack", 0.045045f); value(effect, "Release", 0.089929f);
    E graph; prepare(graph, single(effect));
    auto source = tone(96000, 1000, 0.8f);
    const auto compressed = process(graph, source);
    double signal = 0, reference = 0;
    for (size_t i = 72000; i < 96000; ++i) { signal += compressed[0][i] * compressed[0][i]; reference += source[0][i] * source[0][i]; }
    CHECK_NEAR(10 * std::log10(signal / reference), -9.28, 0.06);
    double rightPower = 0; for (size_t i = 72000; i < 96000; ++i) rightPower += compressed[1][i] * compressed[1][i];
    CHECK_NEAR(std::sqrt(rightPower / signal), 0.75, 0.01); // linked detection: both channels receive the same gain
    // Ratios from ~53:1 are a brickwall: the settled sine peak equals the threshold amplitude.
    value(effect, "Ratio", 100);
    prepare(graph, single(effect));
    const auto limited = process(graph, tone(96000, 1000, 0.8f));
    float peak = 0; for (size_t i = 72000; i < 96000; ++i) peak = std::max(peak, std::abs(limited[0][i]));
    CHECK_NEAR(peak, 0.125, 2e-3);
    // Below the threshold only the 12 Hz DC blocker acts: a small sine passes at unity.
    value(effect, "Ratio", 4);
    prepare(graph, single(effect));
    const auto quiet = process(graph, tone(24576, 1000, 0.01f));
    CHECK_NEAR(magnitude(quiet[0], 1000, 8192, 8192), 0.01, 2e-5);
    // Linear mix: half wet sits halfway between dry and compressed amplitudes.
    value(effect, "Mix", 0.5f);
    prepare(graph, single(effect));
    const auto mixed = process(graph, tone(96000, 1000, 0.8f));
    double mixedPower = 0; for (size_t i = 72000; i < 96000; ++i) mixedPower += mixed[0][i] * mixed[0][i];
    CHECK_NEAR(std::sqrt(mixedPower), 0.5 * (std::sqrt(reference) + std::sqrt(signal)), 0.01 * std::sqrt(reference));
}

TEST_CASE("wavetable effects: tabulated Serum distortion reproduces the measured tube law, unity half-band, latency and level out")
{
    using namespace sonora::devices::serum_distortion;
    // Tube: y = 0.25 * u / ((1 - d) + d |u|), u = 2 clip(x, +-0.5)
    CHECK_NEAR(shaper(0, 0.0f, 0.1f), 0.2, 1e-6); CHECK_NEAR(shaper(0, 0.0f, 0.9f), 1.0, 1e-6);
    CHECK_NEAR(shaper(0, 0.5f, 0.2f), 0.4 / (1 - 0.4943 + 0.4943 * 0.4), 1e-5); CHECK_NEAR(shaper(0, 1.0f, -0.01f), -0.02 / (1 - 0.9886 + 0.9886 * 0.02), 1e-4);
    CHECK(isSupported(0) && isSupported(4) && isSupported(8) && isSupported(16) && ! isSupported(5) && ! isSupported(11)); // folds and the X / Sine shapers are not measured yet
    CHECK_NEAR(shaper(2, 0.0f, 0.25f), 0.5, 2e-3); // hard clip at drive 0 is linear (slope 2)
    {
        // Morph filter at Freq 0.5, BW 0, low-pass: hosted measurements (small signal) are 0.984 at 200 Hz, 0.776 at 400 Hz, 0.182 at 1 kHz, 0.0459 at 2 kHz.
        const auto gainAt = [&](double frequency, float bandwidth, float morph) {
            MorphFilter filter; filter.setup(rate, 0.5f, bandwidth, morph); filter.reset();
            double sine = 0, cosine = 0; constexpr int frames = 48000;
            for (int i = 0; i < frames; ++i)
            {
                const double phase = 2 * dsp::kPi * frequency * i / rate;
                const float out = filter.process(0.05f * static_cast<float>(std::sin(phase)));
                if (i >= 24000) { sine += out * std::sin(phase); cosine += out * std::cos(phase); }
            }
            return 2 * std::hypot(sine, cosine) / 24000 / 0.05;
        };
        CHECK_NEAR(gainAt(200, 0, 0), 0.984, 0.004); CHECK_NEAR(gainAt(400, 0, 0), 0.776, 0.004);
        CHECK_NEAR(gainAt(1000, 0, 0), 0.1815, 0.003); CHECK_NEAR(gainAt(2000, 0, 0), 0.0459, 0.002);
        CHECK(gainAt(425, 7.0f, 0) > 3 * gainAt(425, 0, 0)); // BW opens the resonance
        CHECK(gainAt(200, 0, 1.0f) < 0.9 * gainAt(200, 0, 0) && gainAt(5000, 0, 1.0f) > 0.9); // morph 100 % is the high-pass
    }

    auto effect = module(1, Type::Distortion);
    value(effect, "Style", 1); value(effect, "Mode", 0); value(effect, "Amount", 0); value(effect, "OutputGain", 1);
    E graph; prepare(graph, single(effect));
    CHECK(graph.latencySamples() == 7);
    for (double frequency : {200.0, 5000.0, 15000.0}) // linear region: gain 0.5, flat through the half-band
    {
        prepare(graph, single(effect));
        const auto sound = process(graph, tone(16384, frequency, 0.2f));
        CHECK_NEAR(magnitude(sound[0], frequency, 8192, 8192), 0.1, 0.002);
    }
    // Level Out scales the whole output; Mix 0 leaves the dry path delayed by the reported latency
    value(effect, "OutputGain", 2); value(effect, "Mix", 0);
    prepare(graph, single(effect));
    auto impulse = blank(256); impulse[0][0] = 1; impulse[1][0] = 1;
    const auto delayed = process(graph, impulse);
    CHECK_NEAR(delayed[0][7], 2.0, 1e-6); CHECK_NEAR(delayed[0][6], 0.0, 1e-6);
}

TEST_CASE("wavetable effects: Serum delay style reproduces the measured timing, offsets, sync grid, ping-pong and feedback law")
{
    const auto peakIndex = [](const std::vector<float>& data, size_t from = 0) {
        size_t best = from; for (size_t i = from; i < data.size(); ++i) if (std::abs(data[i]) > std::abs(data[best])) best = i; return best; };
    auto effect = module(1, Type::Delay);
    value(effect, "Style", 1); value(effect, "Mix", 1); value(effect, "Feedback", 0);
    value(effect, "Time", 0.01f); value(effect, "RightTime", 0.02f); value(effect, "Sync", 0);
    value(effect, "FilterFreq", 849); value(effect, "FilterWidth", 6.75f);
    E graph; prepare(graph, single(effect));
    auto impulse = blank(4096); impulse[0][0] = 1; impulse[1][0] = 1;
    auto result = process(graph, impulse);
    CHECK(peakIndex(result[0]) == 430); CHECK(peakIndex(result[1]) == 910); // 0.01 s * 48000 - 50, 0.02 s * 48000 - 50
    CHECK(result[0][430] > 0.7f); // unity-peak band-pass keeps most of an impulse at the default wide setting
    // offsets multiply (time - 50 samples) and round up: 1.25 * 430 = 537.5 -> 538
    value(effect, "OffsetLeft", 1.25f); prepare(graph, single(effect));
    result = process(graph, impulse); CHECK(peakIndex(result[0]) == 538);
    // synced: stored time 0.0773 selects one beat (0.5 s at 120 bpm) minus two samples
    value(effect, "OffsetLeft", 1); value(effect, "Sync", 1); value(effect, "Time", 0.0773125f); value(effect, "RightTime", 0.15f);
    prepare(graph, single(effect));
    auto longImpulse = blank(60000); longImpulse[0][0] = 1; longImpulse[1][0] = 1;
    result = process(graph, longImpulse);
    CHECK(peakIndex(result[0]) == 23998); CHECK(peakIndex(result[1]) == 47998);
    // feedback law: with Time 0.01 / 0.02 the right (longer) channel feeds back with feedback^2
    value(effect, "Sync", 0); value(effect, "Time", 0.01f); value(effect, "RightTime", 0.02f); value(effect, "Feedback", 0.5f);
    prepare(graph, single(effect));
    result = process(graph, impulse);
    const float firstRight = result[1][910];
    size_t secondRight = 1820; for (size_t i = 1815; i < 1830; ++i) if (std::abs(result[1][i]) > std::abs(result[1][secondRight])) secondRight = i;
    CHECK_NEAR(std::abs(result[1][secondRight]) / std::abs(firstRight), 0.183, 0.02); // 0.5^2 times the 0.73 echo-to-echo band-pass loss (hosted: 0.183)
    // ping-pong: the right tap lags the left tap by the right delay
    value(effect, "Mode", 1); value(effect, "Feedback", 0); prepare(graph, single(effect));
    result = process(graph, impulse);
    CHECK(peakIndex(result[0]) == 430); CHECK(peakIndex(result[1]) == 430 + 910);
}

TEST_CASE("wavetable effects: Serum phaser style matches the measured all-pass cascade, mix law and LFO depth law")
{
    const double frequency = 1000.0;
    const auto response = [&](double sectionCoefficient, int sections, double q) {
        // 2 * sections first-order all-passes with g = coefficient, mixed (1 - q^2) dry + q wet
        const std::complex<double> z = std::polar(1.0, 2 * dsp::kPi * frequency / rate), inverse = 1.0 / z;
        const double c = (1 - sectionCoefficient) / (1 + sectionCoefficient);
        std::complex<double> ap = 1.0;
        for (int i = 0; i < 2 * sections; ++i) ap *= (-c + inverse) / (1.0 - c * inverse);
        return std::abs((1 - q * q) + q * ap);
    };
    auto effect = module(1, Type::Phaser);
    value(effect, "Style", 1); value(effect, "Mix", 1); value(effect, "Freq", 600); value(effect, "Spread", 0); value(effect, "Mod", 0);
    value(effect, "Feedback", 0); value(effect, "Poles", 4); value(effect, "Rate", 0); value(effect, "Width", 0);
    E graph; prepare(graph, single(effect));
    const double base = 2 * std::sin(dsp::kPi * 20.0 * std::pow(600.0 / 20.0, 6.6846 / std::log(900.0)) / rate);
    const auto settled = process(graph, tone(32768, frequency, 0.1f));
    CHECK_NEAR(magnitude(settled[0], frequency, 16384, 16384) / 0.1, response(base, 4, 0.5), 2e-3); // q = 0.5 at full wet
    // LFO depth law: coefficient * (1 + 16 pi D^3 triangle(phase)); D = 0.5, phase 0.25 -> factor 1 + 16 pi / 8 * 0.25
    value(effect, "Mod", 0.5f); value(effect, "LfoPhase", 0.25f); prepare(graph, single(effect));
    const auto modulated = process(graph, tone(32768, frequency, 0.1f));
    CHECK_NEAR(magnitude(modulated[0], frequency, 16384, 16384) / 0.1, response(base * (1 + 16 * dsp::kPi * 0.125 * 0.25), 4, 0.5), 2e-3);
    // mix law: q = sin^2(pi w / 4) -> half wet gives q = 0.1464
    value(effect, "Mod", 0); value(effect, "Mix", 0.5f); prepare(graph, single(effect));
    const auto half = process(graph, tone(32768, frequency, 0.1f));
    CHECK_NEAR(magnitude(half[0], frequency, 16384, 16384) / 0.1, response(base, 4, 0.14644660940672627), 2e-3);
}

TEST_CASE("wavetable effects: Serum Hyper style keeps the measured dry gain, voice limit and static comb structure")
{
    auto effect = module(1, Type::HyperDimension);
    value(effect, "Style", 1); value(effect, "Unison", 4); value(effect, "HyperRate", 0); value(effect, "HyperDetune", 0.25f); value(effect, "DimeWet", 0);
    value(effect, "Mix", 0);
    E graph; prepare(graph, single(effect));
    const auto dry = process(graph, tone(32768, 1000, 0.1f));
    CHECK_NEAR(magnitude(dry[0], 1000, 16384, 16384) / 0.1, 0.99785, 1e-3); // no wet: fixed dry gain 0.99785
    value(effect, "Mix", 1); value(effect, "Unison", 0); prepare(graph, single(effect));
    const auto none = process(graph, tone(32768, 1000, 0.1f));
    CHECK_NEAR(magnitude(none[0], 1000, 16384, 16384) / 0.1, 0.99785, 1e-3); // zero voices ignore Mix
    // static comb: Rate 0, one voice, full wet -> dry 0.444 plus a delayed copy through a 120 Hz high-pass
    value(effect, "Unison", 1); value(effect, "PhaseL0", 0.5f); prepare(graph, single(effect));
    const auto one = process(graph, tone(32768, 1000, 0.1f));
    // phase 0.5 means zero extra delay: wet is the one-sample-delayed high-passed input, so the 1 kHz amplitude is dry 0.444 + ~1.0 * cos(2 pi f/fs)-ish combination
    const double expected = std::abs(0.44419 + std::polar(1.0, -2 * dsp::kPi * 1000 / rate));
    CHECK_NEAR(magnitude(one[0], 1000, 16384, 16384) / 0.1, expected, 0.01);
    // the reference caps the voice count at 7
    value(effect, "Unison", 7); value(effect, "HyperRate", 0.4f);
    prepare(graph, single(effect));
    CHECK_NEAR(E::parameterDescriptors(Type::HyperDimension)[static_cast<size_t>(E::parameterIndex(Type::HyperDimension, "Unison"))].maximum, 7, 0);
}

TEST_CASE("wavetable effects: reverberation has a decaying stereo tail and resets deterministically")
{
    auto effect = module(5, Type::Reverb); value(effect, "Predelay", 0); value(effect, "Decay", 0.5f);
    E graph; prepare(graph, single(effect));
    auto source = blank(96000); source[0][0] = 1; source[1][0] = 1;
    const auto output = process(graph, source);
    CHECK(energy(output[0], 2000, 12000) > 1e-8); CHECK(energy(output[0], 70000, 12000) < energy(output[0], 2000, 12000));
    double stereo = 0; for (size_t i = 0; i < output[0].size(); ++i) stereo += std::abs(output[0][i] - output[1][i]);
    CHECK(stereo > 0.01); graph.reset();
    CHECK_NEAR(difference(output, process(graph, source)), 0, 0); finite(output);
}

TEST_CASE("wavetable effects: nested Split3 band chains change targeted bands and preserve stable child controls")
{
    auto bands = module(100, Type::Split3);
    auto low = module(102, Type::Utility); value(low, "Output", 6.0206f);
    auto high = module(104, Type::Utility); value(high, "Output", -6.0206f);
    bands.bands[0] = {low}; bands.bands[2] = {high};
    E graph; prepare(graph, single(bands));
    REQUIRE(graph.setParameter(102, 1, 6.0206f));
    auto source = tone(24576, 93.75);
    for (size_t frame = 0; frame < source[0].size(); ++frame) source[0][frame] += 0.1f * static_cast<float>(std::sin(2 * dsp::kPi * 9000 * static_cast<double>(frame) / rate));
    const auto output = process(graph, source);
    CHECK_NEAR(magnitude(output[0], 93.75, 16384, 8192), 0.2, 0.01);
    CHECK_NEAR(magnitude(output[0], 9000, 16384, 8192), 0.05, 0.005);
    const auto snapshot = graph.snapshotConfiguration(); CHECK(snapshot.buses[0][0].bands[2][0].id == 104);
    CHECK(graph.moduleIds().size() == 3);
}

TEST_CASE("wavetable effects: all types and boundary controls remain finite and combined graphs ignore caller block partition")
{
    const auto ir = devices::ConvolutionIR::fromSamples(std::vector<float>{1, 0.5f, -0.1f}, 1, rate); REQUIRE(ir);
    E::Configuration chain;
    for (int code = 0; code <= 14; ++code)
    {
        if (code == 13) continue;
        auto effect = module(static_cast<E::ModuleId>(code + 1), *E::typeFromCode(code));
        for (bool maximum : {false, true})
        {
            auto edge = effect;
            const auto parameters = E::parameterDescriptors(edge.type);
            for (size_t p = 0; p < parameters.size(); ++p) edge.values[p] = maximum ? parameters[p].maximum : parameters[p].minimum;
            value(edge, "Mix", 1); value(edge, "Output", 0);
            if (edge.type == Type::Convolution) edge.impulse = ir.data;
            E graph; prepare(graph, single(edge)); finite(process(graph, tone(16384)));
        }
        value(effect, "Mix", 0.4f);
        if (effect.type == Type::Convolution) effect.impulse = ir.data;
        if (effect.type == Type::Split3) effect.bands[1] = {module(100, Type::Utility)};
        chain.buses[0].push_back(effect);
    }
    auto aux = module(101, Type::Delay); value(aux, "Time", 0.01f); value(aux, "RightTime", 0.015f); value(aux, "Mix", 0.4f);
    chain.buses[1] = {aux}; chain.routing.mainToBus1 = 0.2f;
    E whole, split; prepare(whole, chain); prepare(split, chain); whole.setTempo(140); split.setTempo(140);
    auto input = tone(8192); input[0][97] = std::numeric_limits<float>::quiet_NaN(); input[1][350] = std::numeric_limits<float>::infinity();
    const auto a = process(whole, input, 8192), b = process(split, input, 97);
    CHECK_NEAR(difference(a, b), 0, 2e-6); finite(a); CHECK(energy(a[0]) > 1e-6);
    CHECK(whole.preparedBytes() <= E::maxPreparedBytes);
}

TEST_CASE("wavetable effects: unsupported malformed duplicate and excessive graph descriptions preserve the previous graph")
{
    E graph; const auto good = single(module(1, Type::Utility)); prepare(graph, good);
    std::string error;
    auto duplicate = good; duplicate.buses[1] = good.buses[0]; CHECK(! graph.prepare(duplicate, rate, error)); CHECK(! error.empty());
    auto unknown = single(module(2, Type::Unknown)); CHECK(! graph.prepare(unknown, rate, error));
    auto tooMany = good; tooMany.buses[0].clear();
    for (int i = 0; i < E::maxBusModules + 1; ++i) tooMany.buses[0].push_back(module(static_cast<E::ModuleId>(i + 1), Type::Utility));
    CHECK(! graph.prepare(tooMany, rate, error));
    auto invalid = good; invalid.buses[0][0].values[0] = std::numeric_limits<float>::infinity(); CHECK(! graph.prepare(invalid, rate, error));
    invalid = good; invalid.buses[0][0].bands[0] = {module(2, Type::Utility)}; CHECK(! graph.prepare(invalid, rate, error));
    CHECK(! graph.prepare(good, 0, error));
    auto deep = module(100, Type::Utility);
    for (int depth = 0; depth < E::maxDepth + 1; ++depth) { auto parent = module(static_cast<E::ModuleId>(200 + depth), Type::Split3); parent.bands[0] = {deep}; deep = parent; }
    CHECK(! graph.prepare(single(deep), rate, error));
    CHECK(graph.moduleType(1) == Type::Utility);
    const auto input = tone(512); CHECK_NEAR(difference(process(graph, input), input), 0, 1e-7);
}

TEST_CASE("wavetable effects: impulse validation rejects malformed and resampled allocation bombs")
{
    CHECK(! devices::ConvolutionIR::fromSamples({}, 1, rate));
    CHECK(! devices::ConvolutionIR::fromSamples(std::vector<float>{1, 2, 3}, 2, rate));
    CHECK(! devices::ConvolutionIR::fromSamples(std::vector<float>{1}, 3, rate));
    CHECK(! devices::ConvolutionIR::fromSamples(std::vector<float>{std::numeric_limits<float>::quiet_NaN()}, 1, rate));
    CHECK(! devices::ConvolutionIR::fromSamples(std::vector<float>(devices::ConvolutionIR::maxFrames + 1, 0), 1, rate));
    const auto bomb = devices::ConvolutionIR::fromSamples(std::vector<float>(100, 1), 1, 1); REQUIRE(bomb);
    auto effect = module(1, Type::Convolution); effect.impulse = bomb.data;
    E graph; std::string error; CHECK(! graph.prepare(single(effect), rate, error)); CHECK(! error.empty());
}

TEST_CASE("wavetable effects: every named module processes audible audio beyond its dry control path")
{
    const auto impulse = devices::ConvolutionIR::fromSamples(std::vector<float>{0.5f, 0.25f}, 1, rate); REQUIRE(impulse);
    const auto source = tone(24576);
    for (int code = 0; code <= 14; ++code)
    {
        if (code == 13) continue;
        auto effect = module(1, *E::typeFromCode(code));
        if (effect.type == Type::Eq) value(effect, "Band1Gain", 12);
        if (effect.type == Type::Utility) value(effect, "Mono", 1);
        if (effect.type == Type::Compressor) value(effect, "Threshold", -30);
        if (effect.type == Type::FrequencyShift) value(effect, "Shift", 125);
        if (effect.type == Type::Convolution) effect.impulse = impulse.data;
        E graph; prepare(graph, single(effect));
        const auto output = process(graph, source);
        CHECK(energy(output[0], 16000, 8192) > 1e-8);
        CHECK(difference(output, source) > 1e-5); finite(output);
    }
}

TEST_CASE("wavetable effects: own external WAV impulses preserve file rate channel order and bounded errors")
{
    const juce::File directory("/tmp/sonora_tests/sonora_wavetable_effects"); REQUIRE(directory.createDirectory().wasOk());
    const auto file = directory.getChildFile("Independent IR.wav"); REQUIRE(file.deleteFile());
    {
        std::unique_ptr<juce::OutputStream> stream(file.createOutputStream()); REQUIRE(stream != nullptr);
        juce::WavAudioFormat format;
        auto writer = format.createWriterFor(stream, juce::AudioFormatWriterOptions{}.withSampleRate(32000).withNumChannels(2).withBitsPerSample(24)); REQUIRE(writer != nullptr);
        juce::AudioBuffer<float> pcm(2, 512); pcm.clear(); pcm.setSample(0, 0, 0.75f); pcm.setSample(1, 5, -0.5f);
        REQUIRE(writer->writeFromAudioSampleBuffer(pcm, 0, 512));
    }
    const auto loaded = devices::ConvolutionIR::fromFile(file); REQUIRE(loaded);
    CHECK(loaded.data->channels() == 2); CHECK(loaded.data->frames() == 512); CHECK_NEAR(loaded.data->sampleRate(), 32000, 0);
    CHECK_NEAR(loaded.data->originalSamples()[0], 0.75, 2e-6); CHECK_NEAR(loaded.data->originalSamples()[11], -0.5, 2e-6);
    auto effect = module(1, Type::Convolution); effect.impulse = loaded.data;
    E graph; prepare(graph, single(effect)); finite(process(graph, tone(1024)));
    const auto bad = directory.getChildFile("Invalid.wav"); REQUIRE(bad.replaceWithText("Independent malformed impulse fixture"));
    CHECK(! devices::ConvolutionIR::fromFile(bad)); CHECK(! devices::ConvolutionIR::fromFile(directory.getChildFile("Missing.wav")));
}

TEST_CASE("wavetable effects: identity convolution dry wet and bypass share exact fixed latency")
{
    const auto ir = devices::ConvolutionIR::fromSamples(std::vector<float>{1}, 1, rate); REQUIRE(ir);
    auto effect = module(1, Type::Convolution); effect.impulse = ir.data;
    auto input = blank(512); input[0][0] = 1; input[1][0] = 0.75f;
    for (float amount : {0.0f, 0.5f, 1.0f})
    {
        value(effect, "Mix", amount);
        E graph; prepare(graph, single(effect));
        CHECK(graph.latencySamples() == 128);
        const auto output = process(graph, input, 97);
        const double scale = amount > 0 && amount < 1 ? std::sqrt(2.0) : 1;
        for (int frame = 0; frame < 512; ++frame)
        {
            CHECK_NEAR(output[0][static_cast<size_t>(frame)], frame == 128 ? scale : 0, 2e-6);
            CHECK_NEAR(output[1][static_cast<size_t>(frame)], frame == 128 ? scale * 0.75 : 0, 2e-6);
        }
        REQUIRE(graph.setEnabled(1, false)); graph.reset();
        const auto bypass = process(graph, input, 97); CHECK_NEAR(bypass[0][128], 1, 2e-6); CHECK_NEAR(energy(bypass[0], 0, 128), 0, 0);
    }
    value(effect, "Mix", 0.5f);
    auto reference = single(effect); reference.alignLatency = false;
    E unaligned; prepare(unaligned, reference);
    const auto exposed = process(unaligned, input); CHECK_NEAR(exposed[0][0], std::sqrt(0.5), 2e-6); CHECK_NEAR(exposed[0][128], std::sqrt(0.5), 2e-6);
}

TEST_CASE("wavetable effects: three parallel bus paths align convolution Hilbert and dry impulses before returns")
{
    const auto ir = devices::ConvolutionIR::fromSamples(std::vector<float>{1}, 1, rate); REQUIRE(ir);
    auto convolution = module(1, Type::Convolution); convolution.impulse = ir.data; value(convolution, "Mix", 0.5f);
    E::Configuration configuration;
    configuration.buses[0] = {convolution}; configuration.buses[1] = {module(2, Type::Utility)};
    configuration.buses[2] = {module(3, Type::FrequencyShift)};
    configuration.routing = {0.5f, 0, 1, 0.25f, 1};
    E graph; prepare(graph, configuration); CHECK(graph.latencySamples() == 128);
    auto input = blank(512); input[0][0] = input[1][0] = input[4][0] = input[5][0] = 1;
    const auto output = process(graph, input, 97);
    CHECK_NEAR(output[0][128], std::sqrt(2.0) + 0.5 + 0.25, 3e-6);
    CHECK_NEAR(output[2][128], 0.5, 2e-6); CHECK_NEAR(output[4][128], 1, 2e-6);
    for (const auto& channel : output) CHECK_NEAR(energy(channel, 0, 128), 0, 0);
}

TEST_CASE("wavetable effects: nested band paths align identity convolution shifter and zero-latency chains")
{
    const auto ir = devices::ConvolutionIR::fromSamples(std::vector<float>{1}, 1, rate); REQUIRE(ir);
    auto convolution = module(2, Type::Convolution); convolution.impulse = ir.data;
    auto bands = module(1, Type::Split3);
    bands.bands[0] = {convolution}; bands.bands[1] = {module(3, Type::Utility)}; bands.bands[2] = {module(4, Type::FrequencyShift)};
    E nested, reference; prepare(nested, single(bands)); prepare(reference, single(module(5, Type::Split3)));
    CHECK(nested.latencySamples() == 128); CHECK(reference.latencySamples() == 0);
    auto input = blank(2048); input[0][0] = 1; input[1][0] = 0.5f;
    const auto delayed = process(nested, input, 97), original = process(reference, input, 2048);
    for (int frame = 0; frame < 2048; ++frame)
        for (int channel = 0; channel < 2; ++channel)
            CHECK_NEAR(delayed[static_cast<size_t>(channel)][static_cast<size_t>(frame)], frame < 128 ? 0 : original[static_cast<size_t>(channel)][static_cast<size_t>(frame - 128)], 3e-6);
}


TEST_CASE("wavetable effects: appended linear OutputGain preserves legacy descriptors and exact wet mute")
{
    const auto input = tone(1024);
    for (const Type type : {Type::Eq, Type::Utility})
    {
        CHECK(E::parameterIndex(type, "Output") == 1);
        const int index = E::parameterIndex(type, "OutputGain"); REQUIRE(index > 1);
        auto effect = module(1, type); effect.values.pop_back(); // Historical content has no trailing scale.
        E historical; prepare(historical, single(effect));
        const auto unchanged = process(historical, input);
        CHECK_NEAR(difference(unchanged, input), 0, 2e-6);
        for (const float gain : {0.0f, .00001f, .25f, 1.0f, 2.25f, 4.0f})
        {
            E scaled; prepare(scaled, single(effect)); REQUIRE(scaled.setParameter(1, index, gain));
            const auto actual = process(scaled, input);
            for (size_t channel = 0; channel < 2; ++channel)
                for (size_t sample = 0; sample < input[0].size(); ++sample)
                    CHECK_NEAR(actual[channel][sample], input[channel][sample] * gain, 2e-6);
            const auto captured = scaled.snapshotConfiguration();
            CHECK_NEAR(captured.buses[0][0].values[static_cast<size_t>(index)], gain, 0);
        }
        E dry; auto mixed = module(1, type); value(mixed, "Mix", 0); value(mixed, "OutputGain", 0);
        prepare(dry, single(mixed)); CHECK_NEAR(difference(process(dry, input), input), 0, 0);
    }
}

TEST_CASE("wavetable effects: pre Main returns pass through Main mute while legacy post returns bypass Main effects")
{
    auto mute = module(1, Type::Utility); value(mute, "OutputGain", 0);
    E::Configuration configuration; configuration.buses[0] = {mute}; configuration.buses[1] = {module(2, Type::Utility)};
    configuration.routing = {0, 0, 0.75f, 0, 1};
    auto input = blank(1024);
    for (size_t sample = 0; sample < input[2].size(); ++sample) { input[2][sample] = 0.1f; input[3][sample] = -0.2f; }
    E post; prepare(post, configuration); const auto legacy = process(post, input, 73);
    CHECK(! post.snapshotConfiguration().returnBeforeMain[0]);
    CHECK_NEAR(legacy[0][100], 0.075, 1e-7); CHECK_NEAR(legacy[1][100], -0.15, 1e-7);
    configuration.returnBeforeMain[0] = true;
    E pre; prepare(pre, configuration); const auto routed = process(pre, input, 73);
    CHECK(pre.snapshotConfiguration().returnBeforeMain[0]); CHECK_NEAR(energy(routed[0]), 0, 0); CHECK_NEAR(energy(routed[1]), 0, 0);
    CHECK_NEAR(routed[2][100], 0.1, 1e-7); CHECK_NEAR(routed[3][100], -0.2, 1e-7);
    REQUIRE(pre.setEnabled(1, false)); pre.reset(); const auto bypass = process(pre, input);
    CHECK_NEAR(bypass[0][100], 0.075, 1e-7); CHECK_NEAR(bypass[1][100], -0.15, 1e-7);
    REQUIRE(pre.setEnabled(1, true)); REQUIRE(pre.setParameter(1, E::mixParameter, 0)); pre.reset();
    CHECK_NEAR(difference(process(pre, input), bypass), 0, 0);
}

TEST_CASE("wavetable effects: mixed pre and post sends use the original Main input and apply each return exactly once")
{
    auto main = module(1, Type::Utility); value(main, "OutputGain", 2);
    auto auxiliary1 = module(2, Type::Utility); value(auxiliary1, "OutputGain", 3);
    auto auxiliary2 = module(3, Type::Utility); value(auxiliary2, "OutputGain", 0.5f);
    E::Configuration configuration; configuration.buses = {{{main}, {auxiliary1}, {auxiliary2}}};
    configuration.returnBeforeMain = {true, false}; configuration.routing = {0.5f, 0.25f, 0.75f, 1.5f, 0.8f};
    E effects; prepare(effects, configuration); auto input = tone(1024);
    for (size_t sample = 0; sample < input[0].size(); ++sample)
    { input[2][sample] = 0.04f; input[3][sample] = -0.03f; input[4][sample] = -0.02f; input[5][sample] = 0.01f; }
    const auto output = process(effects, input, 91);
    for (size_t sample = 0; sample < input[0].size(); ++sample)
        for (size_t channel = 0; channel < 2; ++channel)
        {
            const float pre = (input[2 + channel][sample] + input[channel][sample] * 0.5f) * 3;
            const float post = (input[4 + channel][sample] + input[channel][sample] * 0.25f) * 0.5f;
            CHECK_NEAR(output[2 + channel][sample], pre, 2e-7); CHECK_NEAR(output[4 + channel][sample], post, 2e-7);
            CHECK_NEAR(output[channel][sample], ((input[channel][sample] + pre * 0.75f) * 2 + post * 1.5f) * 0.8f, 3e-7);
        }
    configuration.returnBeforeMain = {true, true}; prepare(effects, configuration);
    const auto both = process(effects, input);
    CHECK(std::abs(both[0][100] - output[0][100]) > 0.001f);
    CHECK(effects.snapshotConfiguration().returnBeforeMain == configuration.returnBeforeMain);
}

TEST_CASE("wavetable effects: serial convolution and Hilbert returns align Main input and post outputs to the total topology latency")
{
    const auto ir = devices::ConvolutionIR::fromSamples(std::vector<float>{1}, 1, rate); REQUIRE(ir);
    auto main = module(1, Type::Convolution); main.impulse = ir.data;
    E::Configuration configuration; configuration.buses[0] = {main}; configuration.buses[1] = {module(2, Type::FrequencyShift)};
    auto longPost = module(3, Type::Convolution); longPost.impulse = ir.data;
    auto longPost2 = module(4, Type::Convolution); longPost2.impulse = ir.data;
    configuration.buses[2] = {longPost, longPost2}; configuration.returnBeforeMain = {true, false};
    configuration.routing = {0, 0, 1, 1, 1};
    auto input = blank(1024); for (auto& channel : input) channel[0] = 0.25f;
    E effects; prepare(effects, configuration); CHECK(effects.latencySamples() == 256);
    const auto output = process(effects, input, 37);
    CHECK_NEAR(output[0][256], 0.75, 4e-6); CHECK_NEAR(output[1][256], 0.75, 4e-6);
    CHECK_NEAR(output[2][31], 0.25, 2e-6); CHECK_NEAR(output[3][31], 0.25, 2e-6);
    CHECK_NEAR(output[4][256], 0.25, 2e-6);
    CHECK_NEAR(energy(output[0], 0, 256), 0, 1e-12);
    configuration.buses[2].clear(); configuration.routing.bus2ToMain = 0; prepare(effects, configuration); CHECK(effects.latencySamples() == 159);
    const auto serial = process(effects, input, 101); CHECK_NEAR(serial[0][159], 0.5, 3e-6); CHECK_NEAR(energy(serial[0], 0, 159), 0, 1e-12);
    configuration.alignLatency = false; prepare(effects, configuration); CHECK(effects.latencySamples() == 159);
    const auto exposed = process(effects, input); CHECK_NEAR(exposed[0][128], 0.25, 2e-6); CHECK_NEAR(exposed[0][159], 0.25, 2e-6);
}

TEST_CASE("wavetable effects: two pre return latencies dry zeroMix and bypass retain exact serial impulse timing")
{
    const auto ir = devices::ConvolutionIR::fromSamples(std::vector<float>{1}, 1, rate); REQUIRE(ir);
    auto main = module(1, Type::Convolution); main.impulse = ir.data;
    auto pre = module(2, Type::Convolution); pre.impulse = ir.data;
    E::Configuration configuration; configuration.buses = {{{main}, {pre}, {module(3, Type::FrequencyShift)}}};
    configuration.returnBeforeMain = {true, true}; configuration.routing = {0.5f, 0.25f, 1, 1, 1};
    auto input = blank(1024); input[0][0] = 1; input[1][0] = 0.5f;
    for (int variant = 0; variant < 3; ++variant)
    {
        E effects; prepare(effects, configuration); CHECK(effects.latencySamples() == 256);
        if (variant == 1) for (auto id : {1u, 2u, 3u}) REQUIRE(effects.setParameter(id, E::mixParameter, 0));
        if (variant == 2) for (auto id : {1u, 2u, 3u}) REQUIRE(effects.setEnabled(id, false));
        const auto output = process(effects, input, 67);
        CHECK_NEAR(output[0][256], 1.75, 4e-6); CHECK_NEAR(output[1][256], 0.875, 4e-6);
        CHECK_NEAR(output[2][128], 0.5, 2e-6); CHECK_NEAR(output[4][128], 0.25, 2e-6);
        CHECK_NEAR(energy(output[0], 0, 256), 0, 1e-12);
        effects.reset(); CHECK_NEAR(energy(process(effects, blank(2048))[0]), 0, 0);
    }
}

TEST_CASE("wavetable effects: nested split pre returns keep aligned serial tails independent of caller partition and reset")
{
    const auto ir = devices::ConvolutionIR::fromSamples(std::vector<float>{1, 0.3f, -0.1f}, 1, rate); REQUIRE(ir);
    auto convolution = module(3, Type::Convolution); convolution.impulse = ir.data;
    auto nested = module(2, Type::Split3); nested.bands[0] = {convolution}; nested.bands[1] = {module(4, Type::FrequencyShift)};
    auto main = module(1, Type::Convolution); main.impulse = ir.data;
    E::Configuration configuration; configuration.buses[0] = {main}; configuration.buses[1] = {nested};
    configuration.returnBeforeMain = {true, false}; configuration.routing = {0.5f, 0.25f, 0.75f, 0.5f, 0.8f};
    E first, second; prepare(first, configuration); prepare(second, configuration); CHECK(first.latencySamples() == 256);
    const auto input = tone(8192, 751); const auto expected = process(first, input, 512), actual = process(second, input, 73);
    CHECK_NEAR(difference(actual, expected), 0, 2e-6); finite(actual);
    second.reset(); E fresh; prepare(fresh, configuration); CHECK_NEAR(difference(process(second, input, 29), process(fresh, input, 1024)), 0, 2e-6);
    const auto bytes = second.preparedBytes(); const auto* identities = second.moduleIds().data();
    auto scratch = blank(1024); std::array<E::StereoBuffer, E::busCount> buses{};
    for (int bus = 0; bus < E::busCount; ++bus) buses[static_cast<size_t>(bus)] = {scratch[static_cast<size_t>(bus * 2)].data(), scratch[static_cast<size_t>(bus * 2 + 1)].data()};
    for (int iteration = 0; iteration < 128; ++iteration) { second.reset(); second.process(buses, 1024); }
    CHECK(second.preparedBytes() == bytes); CHECK(second.preparedBytes() <= E::maxPreparedBytes); CHECK(second.moduleIds().data() == identities);
}

TEST_CASE("wavetable effects: legacy post return samples remain byte identical to independently processed and delayed parallel paths")
{
    const auto ir = devices::ConvolutionIR::fromSamples(std::vector<float>{1, 0.3f, -0.1f}, 1, rate); REQUIRE(ir);
    auto convolution = module(2, Type::Convolution); convolution.impulse = ir.data;
    auto distortion = module(1, Type::Distortion); value(distortion, "Drive", 8);
    E::Configuration configuration; configuration.buses = {{{distortion}, {convolution}, {module(3, Type::FrequencyShift)}}};
    configuration.routing = {0.5f, 0.25f, 0.75f, 0.5f, 0.8f};
    E legacy; prepare(legacy, configuration); CHECK(legacy.latencySamples() == 128); const auto input = tone(4096, 511);
    const auto actual = process(legacy, input, 73); auto expected = blank(4096);
    for (size_t bus = 0; bus < E::busCount; ++bus)
    {
        E branch; prepare(branch, single(configuration.buses[bus][0])); auto source = blank(4096);
        for (size_t channel = 0; channel < 2; ++channel) for (size_t frame = 0; frame < input[0].size(); ++frame)
            source[channel][frame] = bus == 0 ? input[channel][frame] : input[bus * 2 + channel][frame] + input[channel][frame] * (bus == 1 ? 0.5f : 0.25f);
        const auto output = process(branch, source, 73); const int delay = legacy.latencySamples() - branch.latencySamples();
        for (size_t channel = 0; channel < 2; ++channel) for (size_t frame = static_cast<size_t>(delay); frame < input[0].size(); ++frame)
            expected[bus * 2 + channel][frame] = output[channel][frame - static_cast<size_t>(delay)];
    }
    for (size_t channel = 0; channel < 2; ++channel) for (size_t frame = 0; frame < input[0].size(); ++frame)
        expected[channel][frame] = (expected[channel][frame] + expected[2 + channel][frame] * 0.75f + expected[4 + channel][frame] * 0.5f) * 0.8f;
    for (size_t channel = 0; channel < actual.size(); ++channel)
        CHECK(std::memcmp(actual[channel].data(), expected[channel].data(), actual[channel].size() * sizeof(float)) == 0);
    const auto snapshot = legacy.snapshotConfiguration(); const std::array<bool, 2> post{false, false}; CHECK(snapshot.returnBeforeMain == post);
    CHECK_NEAR(snapshot.routing.mainToBus1, configuration.routing.mainToBus1, 0); CHECK(snapshot.buses[1][0].id == 2);
}

TEST_CASE("wavetable effects: prepared and atomic bus return gains span four while sends and Main gain preserve two")
{
    E::Configuration configuration; configuration.routing = {2, 2, 4, 4, 2}; configuration.returnBeforeMain = {true, false};
    E effects; prepare(effects, configuration);
    for (int index = 0; index < 6; ++index)
    {
        REQUIRE(effects.setRoutingParameter(index, 9)); CHECK_NEAR(effects.getRoutingParameter(index), index == 2 || index == 3 || index == 5 ? 4 : 2, 0);
    }
    REQUIRE(effects.setRouting({0, 0, 3.5f, 4, 1})); CHECK_NEAR(effects.routing().bus1ToMain, 3.5, 0);
    CHECK(! effects.setRouting({0, 0, 4.1f, 1, 1})); CHECK_NEAR(effects.routing().bus1ToMain, 3.5, 0);
    std::string error;
    auto invalid = configuration; invalid.routing.mainToBus1 = 2.1f; CHECK(! effects.prepare(invalid, rate, error));
    invalid = configuration; invalid.routing.mainGain = 2.1f; CHECK(! effects.prepare(invalid, rate, error));
    invalid = configuration; invalid.routing.bus2ToMain = 4.1f; CHECK(! effects.prepare(invalid, rate, error));
    invalid = configuration; invalid.routing.directGain = 4.1f; CHECK(! effects.prepare(invalid, rate, error));
    CHECK(effects.snapshotConfiguration().returnBeforeMain == configuration.returnBeforeMain);
    CHECK_NEAR(effects.routing().bus1ToMain, 3.5, 0);
    auto input = blank(128); input[2][0] = 0.25f; input[4][0] = -0.125f;
    const auto output = process(effects, input); CHECK_NEAR(output[0][0], 0.375, 1e-7);
    CHECK_NEAR(effects.routing().directGain, 1, 0); REQUIRE(effects.setRoutingParameter(5, -1)); CHECK_NEAR(effects.routing().directGain, 0, 0);
}

TEST_CASE("wavetable effects: appended DirectGain mutes or boosts only the post Main return sum for every pre post topology")
{
    auto main = module(1, Type::Utility); value(main, "OutputGain", 2);
    auto input = blank(128); input[0][0] = 0.125f; input[1][0] = -0.1f;
    input[2][0] = 0.25f; input[3][0] = 0.15f; input[4][0] = -0.05f; input[5][0] = -0.03f;
    for (const std::array<bool, 2> topology : {std::array<bool, 2>{false, false}, {true, false}, {false, true}, {true, true}})
        for (const float directGain : {0.0f, 0.00001f, 0.25f, 1.0f, 2.25f, 4.0f})
        {
            E::Configuration configuration; configuration.buses[0] = {main}; configuration.returnBeforeMain = topology;
            configuration.routing = {0, 0, 0.5f, 1.25f, 0.75f, directGain};
            E effects; prepare(effects, configuration); const auto output = process(effects, input, 19);
            for (size_t channel = 0; channel < 2; ++channel)
            {
                const float auxiliary1 = input[2 + channel][0] * 0.5f, auxiliary2 = input[4 + channel][0] * 1.25f;
                const float pre = (topology[0] ? auxiliary1 : 0) + (topology[1] ? auxiliary2 : 0);
                const float post = (topology[0] ? 0 : auxiliary1) + (topology[1] ? 0 : auxiliary2);
                CHECK_NEAR(output[channel][0], ((input[channel][0] + pre) * 2 + post * directGain) * 0.75f, 2e-7);
                CHECK_NEAR(output[2 + channel][0], input[2 + channel][0], 1e-7);
            }
            CHECK_NEAR(effects.snapshotConfiguration().routing.directGain, directGain, 0);
        }
}

TEST_CASE("wavetable effects: DirectGain edits retain aligned post latency and never mute a pre Main serial path")
{
    const auto ir = devices::ConvolutionIR::fromSamples(std::vector<float>{1}, 1, rate); REQUIRE(ir);
    auto main = module(1, Type::Convolution); main.impulse = ir.data;
    E::Configuration configuration; configuration.buses[0] = {main}; configuration.buses[1] = {module(2, Type::FrequencyShift)};
    configuration.routing = {0, 0, 1, 0, 1, 2};
    auto input = blank(512); input[0][0] = 0.25f; input[2][0] = 0.125f;
    E effects; prepare(effects, configuration); CHECK(effects.latencySamples() == 128);
    CHECK_NEAR(process(effects, input, 23)[0][128], 0.5, 2e-6);
    REQUIRE(effects.setRoutingParameter(5, 0)); effects.reset(); CHECK(effects.latencySamples() == 128);
    CHECK_NEAR(process(effects, input, 23)[0][128], 0.25, 2e-6);
    configuration.returnBeforeMain[0] = true; configuration.routing.directGain = 0;
    prepare(effects, configuration); CHECK(effects.latencySamples() == 159);
    CHECK_NEAR(process(effects, input, 23)[0][159], 0.375, 2e-6);
    REQUIRE(effects.setRoutingParameter(5, 4)); effects.reset(); CHECK_NEAR(process(effects, input, 23)[0][159], 0.375, 2e-6);
}

TEST_CASE("wavetable effects: Serum Hall reverb reproduces the measured all-pass timing, direct gain, pre-delay and mix law")
{
    auto effect = module(1, Type::Reverb);
    value(effect, "Style", 1); value(effect, "Mix", 1); value(effect, "OutputGain", 1);
    value(effect, "RvSize", 0.35f); value(effect, "RvDelay", 0); value(effect, "RvFreq", 0); value(effect, "RvFreqB", 0);
    value(effect, "RvWidth", 0); value(effect, "RvFeedback", 0); value(effect, "Predelay", 0);
    E graph; prepare(graph, single(effect));
    constexpr int at = 60000; // the output gains fade in and the delay lengths glide from the reference's start-up values for about a second
    auto input = blank(at + 9000);
    input[0][at] = 1; input[1][at] = 1;
    const auto impulse = process(graph, input);
    // three all-passes with g = 0.625 in series: direct term -0.625^3 one sample late (the pre-delay write leads by one sample)
    CHECK_NEAR(impulse[0][at + 1], -0.244140625, 2e-4); CHECK_NEAR(impulse[1][at + 1], -0.244140625, 2e-4);
    for (int i = 2; i < 2180; ++i) CHECK(std::abs(impulse[0][static_cast<size_t>(at + i)]) < 1e-3f); // quiet until the first all-pass echo (about 45.8 ms)
    double echo = 0;
    for (int i = 2195; i < 2210; ++i) echo += std::abs(impulse[0][static_cast<size_t>(at + i)]);
    CHECK(echo > 0.2); // (1 - g^2) g^2 = 0.238 spread by the fractional-delay all-pass
    double right = 0;
    for (int i = 1955; i < 1970; ++i) right += std::abs(impulse[1][static_cast<size_t>(at + i)]); // the right chain is 40.8 ms
    CHECK(right > 0.2);
    // pre-delay in seconds moves the direct tap
    value(effect, "Predelay", 0.01f); prepare(graph, single(effect));
    const auto late = process(graph, input);
    CHECK_NEAR(late[0][at + 481], -0.244140625, 2e-4); CHECK(std::abs(late[0][at + 1]) < 1e-6f);
    // Mix 0 is the dry signal times Level; the shaped law gives wet 0.5 and dry 0.75 at half
    value(effect, "Predelay", 0); value(effect, "Mix", 0); prepare(graph, single(effect));
    const auto dry = process(graph, input);
    CHECK_NEAR(dry[0][at], 1, 5e-4); // the float smoothers stall about 1.5e-4 below their target, as in the reference CHECK(std::abs(dry[0][at + 1]) < 1e-6f);
    value(effect, "Mix", 0.5f); value(effect, "OutputGain", 2); prepare(graph, single(effect));
    const auto half = process(graph, input);
    CHECK_NEAR(half[0][at], 0.75 * 2, 1e-3); CHECK_NEAR(half[0][at + 1], 0.5 * 2 * -0.244140625, 4e-4);
    // the output gains start from silence: a signal at the very first sample is nearly muted
    auto early = blank(2048);
    early[0][0] = 1; early[1][0] = 1;
    value(effect, "Mix", 0); value(effect, "OutputGain", 1); prepare(graph, single(effect));
    CHECK(std::abs(process(graph, early)[0][0]) < 0.01f);
}

TEST_CASE("wavetable effects: Serum Hall decay control lengthens the tail and Size scales the all-pass delays")
{
    auto effect = module(1, Type::Reverb);
    value(effect, "Style", 1); value(effect, "Mix", 1); value(effect, "OutputGain", 1); value(effect, "RvWidth", 0); value(effect, "Predelay", 0);
    auto input = blank(96000 + 4000);
    input[0][4000] = 1; input[1][4000] = 1; // after the fade-in
    double previous = -1;
    for (const float decay : {0.1f, 0.25f, 0.35f, 0.5f})
    {
        value(effect, "RvDelay", decay);
        E graph; prepare(graph, single(effect));
        const auto out = process(graph, input);
        finite(out);
        const double tail = energy(out[0], 52000, 48000);
        CHECK(tail > previous); // more decay, more energy left after one second
        previous = tail;
    }
    // Size 0.5 scales the delays by 32^0.15 = 1.68: the first left echo moves from ~2199 to ~3699 samples
    value(effect, "RvDelay", 0); value(effect, "RvSize", 0.5f);
    E graph; prepare(graph, single(effect));
    auto later = blank(60000 + 5000);
    later[0][60000] = 1; later[1][60000] = 1; // let the delay glide from the default Size settle first
    const auto big = process(graph, later);
    double echo = 0;
    for (int i = 3690; i < 3705; ++i) echo += std::abs(big[0][static_cast<size_t>(60000 + i)]);
    CHECK(echo > 0.2);
}

TEST_CASE("wavetable effects: Serum Plate reverb sums to mono, honours Width, and its tail grows with Size while Freq high-passes")
{
    auto effect = module(1, Type::Reverb);
    value(effect, "Style", 2); value(effect, "Mix", 1); value(effect, "OutputGain", 1); value(effect, "Predelay", 0);
    constexpr int at = 4000, frames = 96000;
    auto input = blank(frames);
    input[0][at] = 1; // only the left input: the reference sums the channels before the network
    const auto run = [&](float size, float freq, float width) {
        value(effect, "RvSize", size); value(effect, "RvFreq", freq); value(effect, "RvWidth", width);
        E graph; prepare(graph, single(effect));
        auto out = process(graph, input);
        finite(out);
        return out;
    };
    const auto mono = run(0.35f, 0.0f, 0.0f);
    double asymmetry = 0;
    for (int i = 0; i < frames; ++i) asymmetry = std::max(asymmetry, static_cast<double>(std::abs(mono[0][static_cast<size_t>(i)] - mono[1][static_cast<size_t>(i)])));
    CHECK(asymmetry < 1e-6); // Width 0 is a mono output
    const auto wide = run(0.35f, 0.0f, 1.0f);
    double difference = 0;
    for (int i = 0; i < frames; ++i) difference = std::max(difference, static_cast<double>(std::abs(wide[0][static_cast<size_t>(i)] - wide[1][static_cast<size_t>(i)])));
    CHECK(difference > 1e-3); // Width 1 keeps the two comb banks apart
    const auto small = run(0.1f, 0.0f, 0.2f), large = run(0.8f, 0.0f, 0.2f);
    CHECK(energy(large[0], 40000, 40000) > energy(small[0], 40000, 40000) * 4); // larger Size: far longer tail
    // Freq is a two-pole high-pass: raising it removes low-frequency energy from a low tone burst
    auto tone = blank(frames);
    for (int i = 0; i < frames; ++i) tone[0][static_cast<size_t>(i)] = 0.1f * static_cast<float>(std::sin(2 * dsp::kPi * 60.0 * i / rate));
    value(effect, "RvSize", 0.5f); value(effect, "RvWidth", 0.2f);
    value(effect, "RvFreq", 0); E open; prepare(open, single(effect));
    const auto lowOpen = process(open, tone);
    value(effect, "RvFreq", 0.9f); E closed; prepare(closed, single(effect));
    const auto lowClosed = process(closed, tone);
    CHECK(energy(lowClosed[0], 48000, 40000) < energy(lowOpen[0], 48000, 40000) * 0.25);
}

TEST_CASE("wavetable effects: Serum distortion Downsample holds samples and Overdrive compresses and saturates")
{
    auto effect = module(1, Type::Distortion);
    value(effect, "Style", 1); value(effect, "Mix", 1); value(effect, "OutputGain", 1);
    // Downsample at full drive holds each input for int(1 * 96 + 0.5) + 1 samples at 48 kHz
    value(effect, "Mode", 8); value(effect, "Amount", 100);
    E graph; prepare(graph, single(effect));
    constexpr int frames = 1024;
    auto ramp = blank(frames);
    for (int i = 0; i < frames; ++i) { ramp[0][static_cast<size_t>(i)] = 0.001f * static_cast<float>(i); ramp[1][static_cast<size_t>(i)] = ramp[0][static_cast<size_t>(i)]; }
    const auto held = process(graph, ramp);
    int runLength = 0, longest = 0;
    for (int i = 40; i < frames - 1; ++i)
    {
        if (std::abs(held[0][static_cast<size_t>(i)] - held[0][static_cast<size_t>(i - 1)]) < 1e-9f) ++runLength; else { longest = std::max(longest, runLength); runLength = 0; }
    }
    CHECK(longest >= 94 && longest <= 98);
    // Overdrive: output is bounded by the saturator and grows less than linearly with the input
    value(effect, "Mode", 16); value(effect, "Amount", 80); value(effect, "Stages", 2);
    prepare(graph, single(effect));
    const auto quiet = process(graph, tone(48000, 440, 0.05f)), loud = process(graph, tone(48000, 440, 0.8f));
    const double gainQuiet = magnitude(quiet[0], 440, 24000, 24000) / 0.05, gainLoud = magnitude(loud[0], 440, 24000, 24000) / 0.8;
    CHECK(gainLoud < gainQuiet * 0.6);
    for (float sample : loud[0]) CHECK(std::abs(sample) < 0.5f);
}
