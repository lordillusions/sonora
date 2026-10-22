#include "TestFramework.h"
#include "dsp/LadderFilter.h"
#include "plugins/SerumEffectsImport.h"

#include <algorithm>
#include <bit>
#include <set>

using namespace sonora;

namespace
{
// Independently authored CBOR and WAV fixtures. No captured plugin state or media in this file.
using Bytes = std::vector<uint8_t>;
using Members = std::vector<std::pair<std::string, Bytes>>;
using E = devices::WavetableEffects;
using R = plugins::SerumEffectsImportResult;

void argument(Bytes& out, uint8_t major, uint64_t number)
{
    if (number < 24) { out.push_back(static_cast<uint8_t>((static_cast<uint64_t>(major) << 5) | number)); return; }
    const int count = number <= 0xff ? 1 : number <= 0xffff ? 2 : number <= 0xffffffff ? 4 : 8;
    const uint8_t code = count == 1 ? 24 : count == 2 ? 25 : count == 4 ? 26 : 27;
    out.push_back(static_cast<uint8_t>((major << 5) | code));
    for (int byte = count - 1; byte >= 0; --byte) out.push_back(static_cast<uint8_t>(number >> (byte * 8)));
}
void append(Bytes& a, const Bytes& b) { a.insert(a.end(), b.begin(), b.end()); }
Bytes text(std::string_view value)
{
    Bytes out; argument(out, 3, value.size()); out.insert(out.end(), value.begin(), value.end()); return out;
}
Bytes number(double value)
{
    Bytes out{0xfb}; const uint64_t bits = std::bit_cast<uint64_t>(value);
    for (int byte = 7; byte >= 0; --byte) out.push_back(static_cast<uint8_t>(bits >> (byte * 8)));
    return out;
}
Bytes object(const Members& members)
{
    Bytes out; argument(out, 5, members.size());
    for (const auto& [key, value] : members) { append(out, text(key)); append(out, value); }
    return out;
}
Bytes array(const std::vector<Bytes>& entries)
{
    Bytes out; argument(out, 4, entries.size()); for (const auto& entry : entries) append(out, entry); return out;
}
Bytes effect(int code, std::string_view key, Bytes params = text("default"), Members extras = {})
{
    extras.emplace_back("plainParams", std::move(params));
    return object({{"type", number(code)}, {std::string(key), object(extras)}});
}
Bytes utility(Bytes params = text("default")) { return effect(12, "FXUtils", std::move(params)); }
Bytes split(int a, int b, int c)
{
    return effect(14, "FXSplit3", object({{"kParamModuleCount1", number(a)}, {"kParamModuleCount2", number(b)}, {"kParamModuleCount3", number(c)}}));
}
Bytes rack(const std::vector<Bytes>& entries, Bytes params = text("default"))
{
    return object({{"FX", array(entries)}, {"plainParams", std::move(params)}});
}
std::shared_ptr<const plugins::SerumState> state(Members groups)
{
    groups.emplace_back("product", text("Serum2")); groups.emplace_back("productVersion", text("2.0.24"));
    groups.emplace_back("version", number(9)); groups.emplace_back("component", text("processor"));
    const auto decoded = plugins::decodeSerumCbor(object(groups)); REQUIRE(decoded); return decoded.state;
}
R parse(Members groups)
{
    const auto source = state(std::move(groups)); return plugins::parseSerumEffects(source->root(), std::string{}, 48000);
}
bool warning(const R& result, std::string_view token)
{
    return std::any_of(result.warnings.begin(), result.warnings.end(), [token](const auto& w) { return w.find(token) != std::string::npos; });
}
float value(const E::Module& module, std::string_view parameter)
{
    const int index = E::parameterIndex(module.type, parameter); REQUIRE(index >= 0);
    REQUIRE(static_cast<size_t>(index) < module.values.size()); return module.values[static_cast<size_t>(index)];
}
void validGraph(const R& result)
{
    E effects; std::string error; REQUIRE(effects.prepare(result.configuration, 48000, error)); CHECK(error.empty());
    CHECK(effects.moduleIds().size() == result.addresses.size());
    std::set<E::ModuleId> ids;
    for (const auto& address : result.addresses) CHECK(address.nativeId != 0 && ids.insert(address.nativeId).second);
}
const juce::File directory("/tmp/sonora_tests/serum_effects_import");
juce::File writeImpulse(std::string_view name)
{
    REQUIRE(directory.createDirectory().wasOk());
    const auto file = directory.getChildFile(juce::String(std::string(name)) + ".wav"); REQUIRE(file.deleteFile());
    std::unique_ptr<juce::OutputStream> stream(file.createOutputStream()); REQUIRE(stream);
    juce::WavAudioFormat format;
    auto writer = format.createWriterFor(stream, juce::AudioFormatWriterOptions{}.withSampleRate(48000).withNumChannels(2).withBitsPerSample(24)); REQUIRE(writer);
    juce::AudioBuffer<float> audio(2, 64); audio.clear(); audio.setSample(0, 0, 0.5f); audio.setSample(1, 1, 0.25f);
    REQUIRE(writer->writeFromAudioSampleBuffer(audio, 0, 64)); return file;
}
Bytes convolution(const std::string& reference, int frames = 64)
{
    return effect(11, "FXConv", text("default"), {{"relativePathToIR", text(reference)}, {"numChannels", number(2)}, {"numFrames", number(frames)}, {"sampleRate", number(48000)}});
}
}

TEST_CASE("Serum effects import: missing and empty racks never invent module defaults")
{
    const auto missing = parse({}); CHECK(! missing.hasRacks && missing.addresses.empty());
    CHECK(missing.configuration.buses[0].empty()); CHECK(! missing.completeFeatureSupport);
    const auto empty = parse({{"FXRack0", rack({})}, {"FXRack1", rack({})}, {"FXRack2", rack({})}});
    CHECK(empty.hasRacks && empty.addresses.empty() && empty.warnings.empty());
    CHECK(empty.configuration.mixLaw == E::MixLaw::Shaped);
    CHECK(! empty.nativeModuleId(0)); validGraph(empty);
    plugins::SerumValue null;
    CHECK(warning(plugins::parseSerumEffects(null), "root is not a map"));
    CHECK(warning(parse({{"FXRack0", text("unexpected")}}), "FX list missing/malformed"));
}

TEST_CASE("Serum effects import: stored return and Direct levels map to measured gains with explicit Main or Direct destinations")
{
    const auto result = parse({{"Global0", object({{"plainParams", object({
        {"kParamDirectVol", number(1.125)}, {"kParamFXBus1Vol", number(2)}, {"kParamFXBus2Vol", number(.125)},
        {"kParamFXBus1Dest", number(0)}, {"kParamFXBus2Dest", number(1)}})}})},
        {"FXRack0", rack({})}, {"FXRack1", rack({})}, {"FXRack2", rack({})}});
    CHECK((result.configuration.returnBeforeMain == std::array<bool, 2>{true, false}));
    CHECK_NEAR(result.configuration.routing.directGain, 2.25, 0);
    CHECK_NEAR(result.configuration.routing.bus1ToMain, 4, 0); CHECK_NEAR(result.configuration.routing.bus2ToMain, .25, 0);
    CHECK(result.warnings.empty()); validGraph(result);
    const auto sparse = parse({}); CHECK((sparse.configuration.returnBeforeMain == std::array<bool, 2>{true, true}));
    CHECK_NEAR(sparse.configuration.routing.directGain, 1, 0);
    CHECK_NEAR(sparse.configuration.routing.bus1ToMain, 1, 0); CHECK_NEAR(sparse.configuration.routing.bus2ToMain, 1, 0);
    for (const double destination : {.5, 2.0, -1.0})
    {
        const auto unknown = parse({{"Global0", object({{"plainParams", object({
            {"kParamFXBus1Dest", number(destination)}, {"kParamDirectVol", number(2.01)}})}})}});
        CHECK(warning(unknown, "uncalibrated destination")); CHECK(warning(unknown, "measured stored range"));
        CHECK(unknown.configuration.returnBeforeMain[0]); CHECK_NEAR(unknown.configuration.routing.directGain, 1, 0);
    }
}

TEST_CASE("Serum effects import: flat Split3 counts retain three racks order and exact stable address mapping")
{
    const auto result = parse({{"FXRack0", rack({utility(), split(1, 1, 1), effect(5, "FXComp"), effect(7, "FXEQ"), effect(8, "FXFilter"), utility()})},
                               {"FXRack1", rack({utility(), effect(2, "FXPhaser")})}, {"FXRack2", rack({effect(3, "FXChorus")})}});
    REQUIRE(result.configuration.buses[0].size() == 3);
    const auto& main = result.configuration.buses[0];
    CHECK(main[0].id == 1 && main[1].id == 2 && main[2].id == 6);
    CHECK(main[1].type == E::Type::Split3);
    for (size_t band = 0; band < 3; ++band)
    {
        REQUIRE(main[1].bands[band].size() == 1);
        CHECK(main[1].bands[band][0].id == band + 3);
        CHECK(result.nativeModuleId(static_cast<int>(band) + 2) == band + 3);
    }
    CHECK(result.configuration.buses[1][0].id == 257 && result.configuration.buses[1][1].id == 258);
    CHECK(result.configuration.buses[2][0].id == 513);
    CHECK(result.nativeModuleId(0) == 1 && result.nativeModuleId(5) == 6);
    CHECK(result.nativeModuleId(100) == 257 && result.nativeModuleId(101) == 258 && result.nativeModuleId(200) == 513);
    CHECK(warning(result, "independently authored"));
    CHECK_NEAR(result.configuration.routing.mainToBus1, 0, 0); validGraph(result);
}

TEST_CASE("Serum effects import: unsupported malformed entries keep ordered valid remainder with index gaps")
{
    const auto unknown = effect(13, "FutureEffect", object({{"FutureScalar", number(42)}}));
    const auto result = parse({{"FXRack0", rack({utility(), unknown, effect(3, "WrongKey"), object({{"type", number(3.5)}}), utility()})}});
    REQUIRE(result.configuration.buses[0].size() == 2);
    CHECK(result.configuration.buses[0][0].id == 1 && result.configuration.buses[0][1].id == 5);
    CHECK(! result.nativeModuleId(1) && ! result.nativeModuleId(2) && ! result.nativeModuleId(3));
    CHECK(result.nativeModuleId(4) == 5 && warning(result, "unsupported type13") && warning(result, "type/module key mismatch")); validGraph(result);
}

TEST_CASE("Serum effects import: sparse defaults come from scoped controlled omission evidence")
{
    const auto result = parse({{"FXRack0", rack({utility(), effect(7, "FXEQ"), effect(8, "FXFilter"), effect(4, "FXDelay"), effect(5, "FXComp")})}});
    REQUIRE(result.configuration.buses[0].size() == 5);
    const auto& modules = result.configuration.buses[0];
    CHECK(modules[0].enabled);
    CHECK_NEAR(value(modules[0], "Mix"), 1, 0); CHECK_NEAR(value(modules[0], "Width"), 1, 0);
    CHECK_NEAR(value(modules[0], "Pan"), 0, 0); CHECK_NEAR(value(modules[0], "OutputGain"), 1, 0);
    CHECK_NEAR(value(modules[1], "OutputGain"), 1, 0); CHECK_NEAR(value(modules[1], "Band1Gain"), 0, 0);
    CHECK_NEAR(value(modules[2], "Cutoff"), 440 * std::exp2(11.4 * 0.5 - 5.75), 2e-5);
    CHECK_NEAR(value(modules[2], "Resonance"), 0, 0); CHECK_NEAR(value(modules[2], "Drive"), 0, 0);
    CHECK_NEAR(value(modules[3], "Style"), 1, 0); CHECK_NEAR(value(modules[3], "Sync"), 1, 0); // sparse FXDelay is tempo-synced by default
    CHECK_NEAR(value(modules[3], "Time"), 0.0773125, 1e-7);
    CHECK_NEAR(value(modules[3], "Mix"), 0.30, 1e-6); // measured default wet 30 %
    // Sparse compressor = measured defaults: threshold param 0.5 -> (0.5)^3 amplitude, ratio 4, attack/release 90 ms (attack tau = half), unity.
    CHECK_NEAR(value(modules[4], "Style"), 1, 0); CHECK_NEAR(value(modules[4], "Threshold"), 60 * std::log10(0.5), 1e-4);
    CHECK_NEAR(value(modules[4], "Ratio"), 4, 0); CHECK_NEAR(value(modules[4], "Attack"), 0.045045, 1e-6); CHECK_NEAR(value(modules[4], "Release"), 0.089929, 1e-6);
    CHECK_NEAR(value(modules[4], "Mix"), 1, 0); CHECK_NEAR(value(modules[4], "OutputGain"), 1, 0); CHECK_NEAR(value(modules[4], "Makeup"), 0, 0);
    // Sparse EQ bands are the measured factory bands: low shelf 209.6 Hz / high shelf 2040.5 Hz, both at 60 % resonance.
    CHECK_NEAR(value(modules[1], "Band1Type"), 0, 0); CHECK_NEAR(value(modules[1], "Band1Freq"), 209.61696906559536, 1e-3);
    CHECK_NEAR(value(modules[1], "Band2Type"), 2, 0); CHECK_NEAR(value(modules[1], "Band2Freq"), 2040.5360638259378, 1e-3);
    CHECK_NEAR(value(modules[1], "Band1Q"), 0.4 + 1.7416 * 0.216, 1e-5); validGraph(result);
}

TEST_CASE("Serum effects import: FXComp maps the measured threshold ratio time and makeup laws")
{
    const auto comp = effect(5, "FXComp", object({{"kParamThresh", number(0.574)}, {"kParamRatio", number(3.931)}, {"kParamAttack", number(0.61)},
                                                  {"kParamRelease", number(393)}, {"kParamMakeup", number(3.0)}, {"kParamWet", number(40)},
                                                  {"kParamLevelOut", number(0.7)}, {"kParamRatioBelow", number(0.75)}, {"kParamGain0", number(4.6)}}));
    const auto limiter = effect(5, "FXComp", object({{"kParamThresh", number(0.2089)}, {"kParamRatio", number(1000000)}}));
    const auto result = parse({{"FXRack0", rack({comp, limiter})}});
    const auto& modules = result.configuration.buses[0]; REQUIRE(modules.size() == 2);
    CHECK_NEAR(value(modules[0], "Style"), 1, 0);
    CHECK_NEAR(value(modules[0], "Threshold"), 60 * std::log10(1 - 0.574), 1e-4);
    CHECK_NEAR(value(modules[0], "Ratio"), 3.931, 1e-5); CHECK_NEAR(value(modules[0], "Attack"), 0.61 / 2000, 1e-7);
    CHECK_NEAR(value(modules[0], "Release"), 0.393, 1e-6);
    CHECK_NEAR(value(modules[0], "Makeup"), 9.7587, 0.02); // measured: makeup 3 -> +9.74 dB
    CHECK_NEAR(value(modules[0], "Mix"), 0.4, 1e-6); CHECK_NEAR(value(modules[0], "OutputGain"), 4 * 0.7 * 0.7, 1e-6);
    CHECK(! warning(result, "RatioBelow") && ! warning(result, "kParamGain0"));
    CHECK_NEAR(value(modules[1], "Ratio"), 100, 0); CHECK(warning(result, "lookahead brickwall limiter")); validGraph(result);
}

TEST_CASE("Serum effects import: measured Utility gains width balance and shaped mix are actual runtime controls")
{
    for (const double output : {0.0, 0.25, 0.5, 0.75, 1.0})
    {
        const auto result = parse({{"FXRack0", rack({utility(object({{"kParamLevelOut", number(output)}, {"kParamWidth", number(200)}, {"kParamBalance", number(-50)}}))})}});
        const auto& module = result.configuration.buses[0][0];
        CHECK_NEAR(value(module, "OutputGain"), 4 * output * output, 0);
        CHECK_NEAR(value(module, "Output"), 0, 0); CHECK_NEAR(value(module, "Width"), 2, 0); CHECK_NEAR(value(module, "Pan"), -0.5, 0);
        E effects; std::string error; REQUIRE(effects.prepare(result.configuration, 48000, error));
        std::array<std::array<float, 32>, 6> audio{}; audio[0].fill(0.1f);
        std::array<E::StereoBuffer, 3> buses{};
        for (size_t bus = 0; bus < buses.size(); ++bus) buses[bus] = {audio[bus * 2].data(), audio[bus * 2 + 1].data()};
        effects.process(buses, 32);
        CHECK_NEAR(audio[0][31], 0.15 * 4 * output * output, 0.015 * 0.15 * 4 * output * output + 3e-6); // 2.85 Hz DC blocker droops a held step by 1.2 % after 32 samples
        CHECK_NEAR(audio[1][31], -0.05 * 4 * output * output, 0.015 * 0.05 * 4 * output * output + 3e-6); // balance acts before the width matrix
    }
    const auto mixed = parse({{"FXRack0", rack({utility(object({{"kParamWet", number(50)}, {"kParamBalance", number(150)}}))})}});
    CHECK_NEAR(value(mixed.configuration.buses[0][0], "Mix"), 0.5, 0);
    CHECK_NEAR(value(mixed.configuration.buses[0][0], "Pan"), 1, 0);
    E mixedRuntime; std::string error; REQUIRE(mixedRuntime.prepare(mixed.configuration, 48000, error));
    std::array<std::array<float, 32>, 6> mixedAudio{}; mixedAudio[0].fill(0.1f); mixedAudio[1].fill(0.1f);
    std::array<E::StereoBuffer, 3> mixedBuses{};
    for (size_t bus = 0; bus < mixedBuses.size(); ++bus) mixedBuses[bus] = {mixedAudio[bus * 2].data(), mixedAudio[bus * 2 + 1].data()};
    mixedRuntime.process(mixedBuses, 32);
    CHECK_NEAR(mixedAudio[0][31], 0.075, 0.015 * 0.075 + 3e-6); CHECK_NEAR(mixedAudio[1][31], 0.125, 0.015 * 0.125 + 3e-6);
    const auto disabled = parse({{"FXRack0", rack({utility(object({{"kParamEnable", number(0)}}))})}});
    CHECK(! disabled.configuration.buses[0][0].enabled);
}

TEST_CASE("Serum effects import: calibrated EQ filter and delay fields never interpret unrelated raw controls")
{
    const auto eq = effect(7, "FXEQ", object({{"kParamFreq1", number(1000)}, {"kParamType1", number(1)}, {"kParamGain1", number(6)},
                                              {"kParamLevelOut", number(0)}, {"kParamReso1", number(50)}}));
    const auto filter = effect(8, "FXFilter", object({{"kParamType", text("MgL12")}, {"kParamFreq", number(0.75)}, {"kParamDrive", number(25)}}));
    const auto delay = effect(4, "FXDelay", object({{"kParamBeatSync", number(0)}, {"kParamTimeL", number(0.025)}, {"kParamTimeR", number(0.05)},
                                                   {"kParamOffsetL", number(1.25)}, {"kParamOffsetR", number(2)}, {"kParamFeedback", number(90)}, {"kParamWet", number(25)}}));
    const auto result = parse({{"FXRack0", rack({eq, filter, delay})}});
    const auto& modules = result.configuration.buses[0]; REQUIRE(modules.size() == 3);
    CHECK_NEAR(value(modules[0], "Band1Freq"), 1000, 0); CHECK_NEAR(value(modules[0], "Band1Gain"), 6, 0);
    CHECK_NEAR(value(modules[0], "Band1Type"), 1, 0); CHECK_NEAR(value(modules[0], "OutputGain"), 0, 0);
    CHECK_NEAR(value(modules[0], "Band1Q"), 0.1 + 18.889 / 16, 1e-6); // measured bell Q law at 50 %
    CHECK_NEAR(value(modules[0], "Band2Type"), 2, 0); CHECK_NEAR(value(modules[0], "Band2Freq"), 2040.5360638259378, 1e-3); // sparse band 2: high shelf
    CHECK_NEAR(value(modules[0], "Band2Q"), 0.4 + 1.7416 * 0.216, 1e-5);
    CHECK_NEAR(value(modules[1], "Cutoff"), 440 * std::exp2(11.4 * 0.75 - 5.75), 2e-4);
    CHECK_NEAR(value(modules[1], "Mode"), 5, 0); // MgL12: second tap of the ladder
    CHECK_NEAR(value(modules[1], "Slope"), 0, 0);
    CHECK_NEAR(value(modules[1], "Drive"), 20 * std::log10(dsp::ladderDriveGain(0.25f)), 1e-4); // measured drive law, 25 %
    CHECK_NEAR(value(modules[1], "Resonance"), 0, 0);
    CHECK_NEAR(value(modules[2], "Style"), 1, 0); CHECK_NEAR(value(modules[2], "Time"), 0.025, 1e-8); CHECK_NEAR(value(modules[2], "RightTime"), 0.05, 1e-8);
    CHECK_NEAR(value(modules[2], "OffsetLeft"), 1.25, 0); CHECK_NEAR(value(modules[2], "OffsetRight"), 1.5, 0); // offset 2 is bounded to the measured 0.5..1.5 // offsets multiply (time - 50 samples) inside the module
    CHECK_NEAR(value(modules[2], "Feedback"), 0.9, 3e-8); CHECK_NEAR(value(modules[2], "Mix"), 0.25, 0);
    validGraph(result);
}

TEST_CASE("Serum effects import: numeric errors unsupported fields and native bounds produce explicit warnings")
{
    const auto source = state({{"FXRack0", rack({utility(object({{"kParamWidth", number(1e300)}, {"kParamBalance", text("unknown")},
                                                                {"kParamHPF", number(120)}, {"kParamLFMono", number(1)}, {"FutureControl", number(42)}})),
                                                 effect(0, "FXDistortion", object({{"kParamDrive", number(100)}, {"kParamMode", text("kLinFold")}}))},
                                               object({{"FutureRackControl", number(0.25)}}))}});
    const auto original = source->decodedBytes();
    const auto result = plugins::parseSerumEffects(source->root());
    CHECK(source->decodedBytes() == original);
    CHECK_NEAR(value(result.configuration.buses[0][0], "Width"), 2, 0);
    CHECK(warning(result, "exceeds the native range") && warning(result, "not a finite numeric"));
    CHECK_NEAR(value(result.configuration.buses[0][0], "HighPass"), 120, 0); CHECK_NEAR(value(result.configuration.buses[0][0], "LowMono"), 1, 0);
    CHECK_NEAR(value(result.configuration.buses[0][0], "LowPass"), 20000, 0); CHECK_NEAR(value(result.configuration.buses[0][0], "LowMonoFreq"), 90, 0);
    CHECK(warning(result, "FutureControl") && warning(result, "rack scalar/routing controls"));
    CHECK(warning(result, "kLinFold has no measured native shaper")); validGraph(result);
}

TEST_CASE("Serum effects import: FXDistortion maps measured mode, drive and filter section")
{
    const auto tube = effect(0, "FXDistortion", object({{"kParamDrive", number(52)}, {"kParamPrePost", number(2)}, {"kParamFreq", number(0.7)},
                                                        {"kParamBW", number(0.1)}, {"kParamLPHP", number(30)}, {"kParamLevelOut", number(0.6)}}));
    const auto diode = effect(0, "FXDistortion", object({{"kParamMode", text("kDiode2")}}));
    const auto result = parse({{"FXRack0", rack({tube, diode})}});
    const auto& modules = result.configuration.buses[0]; REQUIRE(modules.size() == 2);
    CHECK_NEAR(value(modules[0], "Style"), 1, 0); CHECK_NEAR(value(modules[0], "Mode"), 0, 0); CHECK_NEAR(value(modules[0], "Amount"), 52, 0);
    CHECK_NEAR(value(modules[0], "Filter"), 2, 0); CHECK_NEAR(value(modules[0], "FilterFreq"), 0.7, 1e-6);
    CHECK_NEAR(value(modules[0], "FilterWidth"), 0.1, 1e-6); CHECK_NEAR(value(modules[0], "FilterMorph"), 30, 1e-4);
    CHECK_NEAR(value(modules[0], "OutputGain"), 4 * 0.6 * 0.6, 1e-6);
    CHECK_NEAR(value(modules[1], "Mode"), 4, 0); CHECK_NEAR(value(modules[1], "Amount"), 24.7, 1e-4); CHECK_NEAR(value(modules[1], "Filter"), 0, 0);
    validGraph(result);
}

TEST_CASE("Serum effects import: malformed plainParams never receives inferred sparse defaults")
{
    const auto result = parse({{"FXRack0", rack({effect(8, "FXFilter", text("unrecognized-token"))})}});
    REQUIRE(result.configuration.buses[0].size() == 1);
    const auto& filter = result.configuration.buses[0][0];
    CHECK_NEAR(value(filter, "Cutoff"), 10000, 0);
    CHECK_NEAR(value(filter, "Resonance"), 0.1f, 0);
    CHECK(warning(result, "plainParams is neither a map") && warning(result, "Enable sparse default is unverified")); validGraph(result);
}

TEST_CASE("Serum effects import: malformed Split3 counts cannot cross containing spans or discard remainder")
{
    for (const auto& malformed : {split(100, 0, 0), split(-1, 0, 0),
                                 effect(14, "FXSplit3", text("default")),
                                 effect(14, "FXSplit3", object({{"kParamModuleCount1", number(0.5)}, {"kParamModuleCount2", number(0)}, {"kParamModuleCount3", number(0)}}))})
    {
        const auto result = parse({{"FXRack0", rack({malformed, utility()})}});
        REQUIRE(result.configuration.buses[0].size() == 1);
        CHECK(result.configuration.buses[0][0].id == 2); CHECK(! result.nativeModuleId(0));
        CHECK(warning(result, "Split3 count")); validGraph(result);
    }
    const auto nested = parse({{"FXRack0", rack({split(2, 0, 0), split(2, 0, 0), utility(), utility()})}});
    REQUIRE(nested.configuration.buses[0].size() == 2);
    CHECK(nested.configuration.buses[0][0].bands[0].size() == 1 && nested.configuration.buses[0][1].id == 4);
    CHECK(warning(nested, "containing flat span")); validGraph(nested);
}

TEST_CASE("Serum effects import: root band depth inspection and ambiguous address limits remain bounded")
{
    std::vector<Bytes> roots(20, utility());
    const auto rootLimit = parse({{"FXRack0", rack(roots)}});
    CHECK(rootLimit.configuration.buses[0].size() == E::maxBusModules && rootLimit.addresses.size() == E::maxBusModules);
    CHECK(warning(rootLimit, "root limit16")); validGraph(rootLimit);
    std::vector<Bytes> band{split(9, 0, 0)}; band.insert(band.end(), 10, utility());
    const auto bandLimit = parse({{"FXRack0", rack(band)}});
    REQUIRE(bandLimit.configuration.buses[0].size() == 2);
    CHECK(bandLimit.configuration.buses[0][0].bands[0].size() == E::maxBandModules);
    CHECK(bandLimit.configuration.buses[0][1].id == 11 && ! bandLimit.nativeModuleId(9)); validGraph(bandLimit);
    std::vector<Bytes> deep{utility()};
    for (int i = 0; i < E::maxDepth + 1; ++i) deep.insert(deep.begin(), split(static_cast<int>(deep.size()), 0, 0));
    const auto depthLimit = parse({{"FXRack0", rack(deep)}});
    CHECK(depthLimit.addresses.size() == E::maxDepth + 1 && warning(depthLimit, "count/depth limit")); validGraph(depthLimit);
    std::vector<Bytes> longRack{split(99, 0, 0)}; longRack.insert(longRack.end(), 129, utility());
    const auto addressLimit = parse({{"FXRack0", rack(longRack)}, {"FXRack1", rack({utility()})}});
    CHECK(warning(addressLimit, "flat list exceeds128") && warning(addressLimit, "flat index>=100"));
    CHECK(! addressLimit.nativeModuleId(100)); CHECK(addressLimit.nativeModuleId(0) == 1); validGraph(addressLimit);
}

TEST_CASE("Serum effects import: guarded nested Split3 spans retain flatten identities")
{
    const auto result = parse({{"FXRack0", rack({split(3, 1, 0), split(1, 0, 0), utility(), utility(), utility(), utility()})}});
    REQUIRE(result.configuration.buses[0].size() == 2);
    const auto& parent = result.configuration.buses[0][0]; REQUIRE(parent.bands[0].size() == 2);
    REQUIRE(parent.bands[0][0].bands[0].size() == 1); CHECK(parent.bands[0][0].bands[0][0].id == 3);
    CHECK(parent.bands[0][1].id == 4 && parent.bands[1][0].id == 5 && result.configuration.buses[0][1].id == 6);
    CHECK(warning(result, "nested Split3 count semantics")); validGraph(result);
}

TEST_CASE("Serum effects import: immutable origins resolve deduplicated own IRs with validated PCM metadata")
{
    const auto file = writeImpulse("SyntheticIR"); const auto modified = file.getLastModificationTime();
    const auto source = state({{"FXRack0", rack({convolution(file.getFileName().toStdString()), convolution(file.getFullPathName().toStdString())})}});
    model::NativeContent origin;
    origin.sourceDirectory = directory.getFullPathName().toStdString(); origin.data = {1, 2, 3}; origin.controllerData = {4, 5};
    const auto result = plugins::parseSerumEffects(source->root(), origin, 48000);
    REQUIRE(result.configuration.buses[0].size() == 2);
    const auto& a = result.configuration.buses[0][0]; const auto& b = result.configuration.buses[0][1];
    REQUIRE(a.impulse && b.impulse); CHECK(a.impulse == b.impulse); CHECK(a.enabled && b.enabled);
    CHECK(a.impulse->channels() == 2 && a.impulse->frames() == 64); CHECK_NEAR(a.impulse->sampleRate(), 48000, 0);
    CHECK_NEAR(a.impulse->originalSamples()[0], 0.5, 1e-7); CHECK_NEAR(a.impulse->originalSamples()[3], 0.25, 1e-7);
    CHECK(origin.data == Bytes({1, 2, 3}) && origin.controllerData == Bytes({4, 5})); CHECK(file.getLastModificationTime() == modified);
    CHECK(warning(result, "IR PCM loaded unchanged")); validGraph(result);
}

TEST_CASE("Serum effects import: missing malformed mismatched IRs bypass only their ordered module")
{
    const auto file = writeImpulse("MismatchIR");
    const auto source = state({{"FXRack0", rack({convolution(file.getFullPathName().toStdString(), 65),
                                                convolution(directory.getChildFile("Absent.wav").getFullPathName().toStdString()),
                                                convolution(std::string(4097, 'a')), utility()})}});
    const auto result = plugins::parseSerumEffects(source->root(), std::string{}, 48000);
    REQUIRE(result.configuration.buses[0].size() == 4);
    for (size_t i = 0; i < 3; ++i) CHECK(! result.configuration.buses[0][i].enabled && ! result.configuration.buses[0][i].impulse);
    CHECK(result.configuration.buses[0][3].enabled);
    CHECK(warning(result, "numFrames IR metadata mismatch") && warning(result, "IR file unavailable") && warning(result, "IR reference missing/malformed")); validGraph(result);
}

TEST_CASE("Serum effects import: Hall and Plate reverbs map the measured controls and a sparse reverb is the Plate type")
{
    const auto hall = effect(6, "FXReverb", object({{"kParamType", text("kHall")}, {"kParamSize", number(50)}, {"kParamDelay", number(62.5)},
                                                   {"kParamFreq", number(20)}, {"kParamFreqB", number(80)}, {"kParamFeedback", number(10)},
                                                   {"kParamWidth", number(40)}, {"kParamPreDelay", number(0.01)}, {"kParamWet", number(60)}, {"kParamLevelOut", number(1)}}));
    const auto sparseHall = effect(6, "FXReverb", object({{"kParamType", text("kHall")}}));
    const auto plate = effect(6, "FXReverb");
    const auto vintage = effect(6, "FXReverb", object({{"kParamType", text("kVintage")}}));
    const auto result = parse({{"FXRack0", rack({hall, sparseHall, plate, vintage})}});
    const auto& modules = result.configuration.buses[0]; REQUIRE(modules.size() == 4);
    CHECK_NEAR(value(modules[0], "Style"), 1, 0); CHECK_NEAR(value(modules[0], "RvSize"), 0.5, 1e-9);
    CHECK_NEAR(value(modules[0], "RvDelay"), 0.5, 1e-7); // plain = 250 x^2
    CHECK_NEAR(value(modules[0], "RvFreq"), 0.2, 1e-7); CHECK_NEAR(value(modules[0], "RvFreqB"), 0.8, 1e-7);
    CHECK_NEAR(value(modules[0], "RvFeedback"), 0.1, 1e-7); CHECK_NEAR(value(modules[0], "RvWidth"), 0.4, 1e-7);
    CHECK_NEAR(value(modules[0], "Predelay"), 0.01, 1e-8); CHECK_NEAR(value(modules[0], "Mix"), 0.6, 1e-7); CHECK_NEAR(value(modules[0], "OutputGain"), 4, 1e-7);
    // measured defaults: Size 35 %, Delay 30.625 (x = 0.35), Freq 0, FreqB 35 %, Feedback 25 %, Width 20 %, Wet 20 %, Level 0.5
    CHECK_NEAR(value(modules[1], "Style"), 1, 0); CHECK_NEAR(value(modules[1], "RvSize"), 0.35, 1e-7); CHECK_NEAR(value(modules[1], "RvDelay"), 0.35, 1e-7);
    CHECK_NEAR(value(modules[1], "RvFreq"), 0, 0); CHECK_NEAR(value(modules[1], "RvFreqB"), 0.35, 1e-7);
    CHECK_NEAR(value(modules[1], "RvFeedback"), 0.25, 1e-7); CHECK_NEAR(value(modules[1], "RvWidth"), 0.2, 1e-7);
    CHECK_NEAR(value(modules[1], "Mix"), 0.2, 1e-7); CHECK_NEAR(value(modules[1], "OutputGain"), 1, 1e-7); CHECK_NEAR(value(modules[1], "Predelay"), 0, 0);
    CHECK_NEAR(value(modules[2], "Style"), 2, 0); CHECK_NEAR(value(modules[2], "RvSize"), 0.35, 1e-7); // a sparse type is kPlate
    CHECK_NEAR(value(modules[3], "Style"), 0, 0); // kVintage: no native engine yet
    bool warnedVintage = false;
    for (const auto& warning : result.warnings) warnedVintage = warnedVintage || warning.find("reverb type kVintage") != std::string::npos;
    CHECK(warnedVintage);
    validGraph(result);
}

TEST_CASE("Serum effects import: modulated FX controls sample the measured normalized law into native tables")
{
    CHECK(plugins::serumEffectFieldModulatable("FXDelay", "kParamWet"));
    CHECK(plugins::serumEffectFieldModulatable("FXComp", "kParamMakeup"));
    CHECK(! plugins::serumEffectFieldModulatable("FXChorus", "kParamWet")); // native chorus mix is not mapped from Wet
    CHECK(! plugins::serumEffectFieldModulatable("FXDelay", "kParamHQ"));
    const auto source = state({{"FXRack0", rack({effect(5, "FXComp", object({{"kParamMakeup", number(2)}})),
                                                 effect(4, "FXDelay", object({{"kParamWet", number(30)}, {"kParamBeatSync", number(0)}})),
                                                 effect(7, "FXEQ", object({{"kParamFreq1", number(200)}}))})},
                               {"FXRack1", rack({effect(12, "FXUtils", object({{"kParamLPF", number(2000)}}))})}});
    const auto& root = source->root();
    // Makeup: plain = 1 + 30 u^2; the native control is the adapter's dB curve of that plain value.
    const auto makeup = plugins::serumEffectModulation(root, 0, "FXComp", "kParamMakeup", 5);
    REQUIRE(makeup); CHECK(makeup->module == 1); CHECK_NEAR(makeup->base, std::sqrt(1.0f / 30), 1e-6);
    REQUIRE(makeup->curves.size() == 1);
    CHECK(makeup->curves[0].parameter == E::parameterIndex(E::Type::Compressor, "Makeup"));
    CHECK_NEAR(makeup->curves[0].values.front(), 0.0f, 1e-6);
    const double half = 1 + 30 * 0.25;
    CHECK_NEAR(makeup->curves[0].values[2], 20 * std::log10(half * (1 + 0.0335 * (1 - std::pow(half, -1.15)))), 1e-4);
    // Wet: linear 0..100 -> native Mix 0..1; the stored 30 is position 0.3.
    const auto wet = plugins::serumEffectModulation(root, 1, "FXDelay", "kParamWet", 11);
    REQUIRE(wet); CHECK(wet->module == 2); CHECK_NEAR(wet->base, 0.3f, 1e-6);
    REQUIRE(wet->curves.size() == 1); CHECK_NEAR(wet->curves[0].values[7], 0.7f, 1e-6);
    // Free-running delay time: plain seconds = 0.001 + 0.5 u^4 (stored default 0.0773125 here).
    const auto time = plugins::serumEffectModulation(root, 1, "FXDelay", "kParamTimeL", 3);
    REQUIRE(time); CHECK_NEAR(time->curves[0].values[1], 0.001f + 0.5f / 16, 1e-6);
    // Frequencies are logarithmic: EQ 22050/1024 .. 20000 Hz, Utility low-pass 50 .. 20000 Hz.
    const auto eq = plugins::serumEffectModulation(root, 2, "FXEQ", "kParamFreq1", 3);
    REQUIRE(eq); CHECK_NEAR(eq->base, std::log(200 / (22050.0 / 1024)) / std::log(20000 / (22050.0 / 1024)), 1e-5);
    CHECK_NEAR(eq->curves[0].values.front(), 22050.0f / 1024, 1e-3); CHECK_NEAR(eq->curves[0].values.back(), 20000.0f, 0.05);
    const auto lowPass = plugins::serumEffectModulation(root, 100, "FXUtils", "kParamLPF", 3);
    REQUIRE(lowPass); CHECK(lowPass->module == 257); CHECK_NEAR(lowPass->curves[0].values[1], 1000.0f, 0.01);
    // Mismatched key, missing entry and unmeasured field yield no table.
    CHECK(! plugins::serumEffectModulation(root, 0, "FXDelay", "kParamWet"));
    CHECK(! plugins::serumEffectModulation(root, 5, "FXDelay", "kParamWet"));
    CHECK(! plugins::serumEffectModulation(root, 1, "FXDelay", "kParamHQ"));
}
