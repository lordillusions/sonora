#include "TestFramework.h"
#include "plugins/SerumState.h"
#include "plugins/SerumPatch.h"
#include "plugins/SerumEffectsImport.h"
#include "devices/DeviceFactory.h"
#include "devices/native/WavetableSynth.h"
#include "devices/native/WavetableData.h"
#include "devices/native/WavetableContent.h"

#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <zstd.h>

using namespace sonora;

namespace
{
// Default Shapes comes from the installed Serum 2 factory library. Without it (CI, Linux, Windows) the only
// acceptable preparation warnings are the missing-table ones.
bool onlyMissingFactoryTables(const std::vector<std::string>& warnings)
{
    if (juce::File("/Library/Audio/Presets/Xfer Records/Serum 2 Presets/Tables/S2 Tables/Default Shapes.wav").existsAsFile())
        return warnings.empty();
    return std::all_of(warnings.begin(), warnings.end(), [](const std::string& w) { return w.rfind("Wavetable not found: ", 0) == 0; });
}
// All fixtures are independently authored synthetic documents, not installed plugin state/assets.
using Bytes = std::vector<uint8_t>;
void append(Bytes& out, const Bytes& value) { out.insert(out.end(), value.begin(), value.end()); }
void argument(Bytes& out, uint8_t major, uint64_t value)
{
    if (value < 24) { out.push_back(static_cast<uint8_t>((static_cast<uint64_t>(major) << 5) | value)); return; }
    const unsigned count = value <= 0xff ? 1 : value <= 0xffff ? 2 : value <= 0xffffffff ? 4 : 8;
    const uint8_t extra = count == 1 ? 24 : count == 2 ? 25 : count == 4 ? 26 : 27;
    out.push_back(static_cast<uint8_t>((major << 5) | extra));
    for (unsigned i = count; i-- > 0;) out.push_back(static_cast<uint8_t>(value >> (i * 8)));
}
Bytes text(std::string_view s)
{
    Bytes out;
    argument(out, 3, s.size());
    out.insert(out.end(), s.begin(), s.end());
    return out;
}
Bytes integer(uint64_t n, bool negative = false)
{
    Bytes out;
    argument(out, negative ? 1 : 0, n);
    return out;
}
Bytes number(double n)
{
    Bytes out{0xfb};
    const uint64_t bits = std::bit_cast<uint64_t>(n);
    for (unsigned i = 8; i-- > 0;) out.push_back(static_cast<uint8_t>(bits >> (i * 8)));
    return out;
}
Bytes map(std::initializer_list<std::pair<std::string_view, Bytes>> members)
{
    Bytes out;
    argument(out, 5, members.size());
    for (const auto& [key, value] : members) { append(out, text(key)); append(out, value); }
    return out;
}
Bytes array(std::initializer_list<Bytes> values)
{
    Bytes out;
    argument(out, 4, values.size());
    for (const auto& value : values) append(out, value);
    return out;
}
Bytes syntheticFloatArray(size_t count, float value)
{
    Bytes out;
    out.reserve(5 + count * 5);
    argument(out, 4, count);
    const uint32_t bits = std::bit_cast<uint32_t>(value);
    for (size_t i = 0; i < count; ++i)
    {
        out.push_back(0xfa);
        for (unsigned byte = 4; byte-- > 0;) out.push_back(static_cast<uint8_t>(bits >> (byte * 8)));
    }
    return out;
}
Bytes document(const Bytes& additional = map({}), std::string_view product = "Serum2", std::string_view component = "processor")
{
    const auto prefix = map({{"product", text(product)}, {"component", text(component)}, {"version", number(9)},
                             {"productVersion", text("2.0.24")}});
    REQUIRE(! additional.empty());
    REQUIRE((additional[0] >> 5) == 5 && (additional[0] & 31) < 20);
    Bytes out = prefix;
    out[0] = static_cast<uint8_t>(out[0] + (additional[0] & 31));
    out.insert(out.end(), additional.begin() + 1, additional.end());
    return out;
}
std::string metadata(std::string_view product = "Serum2", std::string_view component = "processor")
{
    return "{\"component\":\"" + std::string(component) + "\",\"product\":\"" + std::string(product)
           + "\",\"productVersion\":\"2.0.24\",\"version\":9.0,\"hash\":\"unverified synthetic hash\",\"synthetic\":{\"note\":\"\\uD834\\uDD1E\"}}";
}
void little(Bytes& out, uint64_t value, unsigned count)
{
    for (unsigned i = 0; i < count; ++i) out.push_back(static_cast<uint8_t>(value >> (8 * i)));
}
Bytes container(const Bytes& cbor, std::string meta = metadata(), bool contentSize = true)
{
    Bytes compressed(ZSTD_compressBound(cbor.size()));
    const std::unique_ptr<ZSTD_CCtx, decltype(&ZSTD_freeCCtx)> context(ZSTD_createCCtx(), &ZSTD_freeCCtx);
    REQUIRE(context != nullptr);
    REQUIRE(! ZSTD_isError(ZSTD_CCtx_setParameter(context.get(), ZSTD_c_contentSizeFlag, contentSize ? 1 : 0)));
    REQUIRE(! ZSTD_isError(ZSTD_CCtx_setParameter(context.get(), ZSTD_c_checksumFlag, 1)));
    const auto count = ZSTD_compress2(context.get(), compressed.data(), compressed.size(), cbor.data(), cbor.size());
    REQUIRE(! ZSTD_isError(count));
    compressed.resize(count);
    Bytes out{'X', 'f', 'e', 'r', 'J', 's', 'o', 'n', 0};
    little(out, meta.size(), 8);
    out.insert(out.end(), meta.begin(), meta.end());
    little(out, cbor.size(), 4);
    little(out, 2, 4);
    append(out, compressed);
    return out;
}
void overwriteLittle(Bytes& out, size_t offset, uint64_t value, unsigned count)
{
    for (unsigned i = 0; i < count; ++i) out[offset + i] = static_cast<uint8_t>(value >> (8 * i));
}
void rejectedCbor(const Bytes& bytes, std::string_view message)
{
    const auto result = plugins::decodeSerumCbor(bytes);
    CHECK(! result);
    CHECK(result.error.find(message) != std::string::npos);
}
void rejectedContainer(const Bytes& bytes, std::string_view message)
{
    const auto result = plugins::decodeSerumState(bytes);
    CHECK(! result);
    CHECK(result.error.find(message) != std::string::npos);
}
} // namespace

TEST_CASE("Serum state: bounded container preserves metadata, unknown fields and sparse plain parameters")
{
    const Bytes cbor = document(map({
        {"Oscillator0", map({{"plainParams", text("default")}, {"WTOsc0", map({{"plainParams", map({{"SyntheticPosition", number(73.25)}})}})}})},
        {"Env0", map({{"plainParams", map({{"SyntheticAttack", number(0.125)}, {"SyntheticGate", {0xf5}}})}})},
        {"UnknownFutureGroup", map({{"opaque", {0x44, 0, 0xff, 0x80, 0x42}}, {"label", text("independent fixture")}, {"nullable", {0xf6}}})}
    }));
    auto meta = metadata();
    meta.pop_back();
    meta += ",\"exactId\":18446744073709551615,\"negativeId\":-9223372036854775808}";
    const auto encoded = container(cbor, meta);
    const auto result = plugins::decodeSerumState(encoded);
    REQUIRE(result);
    CHECK(result.state->hasContainer());
    CHECK(result.state->originalBytes() == encoded);
    CHECK(result.state->decodedBytes() == cbor);
    CHECK(result.state->metadata().find("hash")->text() != nullptr);
    CHECK(*result.state->metadata().find("hash")->text() == "unverified synthetic hash");
    CHECK(*result.state->metadata().find("synthetic")->find("note")->text() == "\xf0\x9d\x84\x9e");
    CHECK(result.state->metadata().find("exactId")->unsignedInteger() == std::numeric_limits<uint64_t>::max());
    CHECK(result.state->metadata().find("negativeId")->negativeIntegerMagnitude() == uint64_t{0x7fffffffffffffff});
    CHECK(! result.state->plainParameter("Oscillator0", "SyntheticVolume"));
    CHECK(! result.state->plainParameter("MissingGroup", "SyntheticAttack"));
    CHECK(! result.state->plainParameter("Env0", "MissingValue"));
    CHECK(! result.state->plainParameter("Env0", "SyntheticGate"));
    REQUIRE(result.state->plainParameter("Env0", "SyntheticAttack"));
    CHECK_NEAR(*result.state->plainParameter("Env0", "SyntheticAttack"), 0.125, 1e-12);
    const std::array<std::string_view, 2> path{"Oscillator0", "WTOsc0"};
    REQUIRE(result.state->plainParameter(path, "SyntheticPosition"));
    CHECK_NEAR(*result.state->plainParameter(path, "SyntheticPosition"), 73.25, 1e-12);
    const auto* unknown = result.state->root().find("UnknownFutureGroup");
    REQUIRE(unknown != nullptr);
    CHECK(*unknown->find("opaque")->bytes() == Bytes({0, 0xff, 0x80, 0x42}));
    CHECK(unknown->find("nullable")->type() == plugins::SerumValue::Type::Null);
}

TEST_CASE("Serum state: explicit raw CBOR and processor/controller identities")
{
    for (const auto* product : {"Serum2", "Serum2FX"})
        for (const auto* component : {"processor", "controller"})
        {
            const auto cbor = document(map({}), product, component);
            const auto raw = plugins::decodeSerumCbor(cbor);
            REQUIRE(raw);
            CHECK(! raw.state->hasContainer());
            CHECK(raw.state->metadata().type() == plugins::SerumValue::Type::Null);
            const auto framed = plugins::decodeSerumState(container(cbor, metadata(product, component)));
            CHECK(framed);
            rejectedContainer(cbor, "XferJson");
            rejectedCbor(container(cbor, metadata(product, component)), "CBOR");
        }
    rejectedCbor(document(map({}), "OtherProduct"), "product");
    rejectedCbor(document(map({}), "Serum2", "other"), "component");
    rejectedCbor(map({{"product", text("Serum2")}, {"component", text("processor")}, {"version", number(10)}, {"productVersion", text("2.0.24")}}), "version");
    rejectedCbor(map({{"product", text("Serum2")}, {"component", text("processor")}, {"version", number(9)}, {"productVersion", text("2..24")}}), "productVersion");
    rejectedContainer(container(document(), metadata("OtherProduct")), "product");
    rejectedContainer(container(document(), metadata("Serum2FX")), "mismatch");
    rejectedContainer(container(document(), metadata("Serum2", "controller")), "mismatch");
}

TEST_CASE("Serum state: exact uint64, negative boundary, floating widths, arrays and Unicode")
{
    const auto cbor = document(map({{"Synthetic", array({integer(std::numeric_limits<uint64_t>::max()),
        integer(std::numeric_limits<uint64_t>::max(), true), integer(42), integer(0, true),
        {0xf9, 0x3c, 0}, {0xf9, 0x80, 1}, {0xfa, 0x3f, 0xc0, 0, 0}, number(-3.25),
        {0xf4}, text("\xe6\x97\xa5\xe6\x9c\xac"), {0xf6}})}}));
    const auto result = plugins::decodeSerumCbor(cbor);
    REQUIRE(result);
    const auto* values = result.state->root().find("Synthetic")->array();
    REQUIRE(values != nullptr);
    REQUIRE(values->size() == 11);
    CHECK((*values)[0].unsignedInteger() == std::numeric_limits<uint64_t>::max());
    CHECK((*values)[1].negativeIntegerMagnitude() == std::numeric_limits<uint64_t>::max());
    CHECK_NEAR(*(*values)[2].number(), 42, 1e-12);
    CHECK_NEAR(*(*values)[3].number(), -1, 1e-12);
    CHECK_NEAR(*(*values)[4].number(), 1, 1e-12);
    CHECK_NEAR(*(*values)[5].number(), -std::ldexp(1.0, -24), 1e-15);
    CHECK_NEAR(*(*values)[6].number(), 1.5, 1e-12);
    CHECK_NEAR(*(*values)[7].number(), -3.25, 1e-12);
    CHECK((*values)[8].boolean() == false);
    CHECK(*(*values)[9].text() == "\xe6\x97\xa5\xe6\x9c\xac");
    CHECK((*values)[10].type() == plugins::SerumValue::Type::Null);
}

TEST_CASE("Serum state: rejects malformed CBOR lengths, keys, UTF8, depth and unsupported forms")
{
    rejectedCbor({0xa1, 0x61, 'x'}, "truncated");
    rejectedCbor({0xa1, 0, 0}, "key must be text");
    rejectedCbor(map({{"x", integer(0)}, {"x", integer(1)}}), "duplicate");
    rejectedCbor({0x78, 5, 'x'}, "truncated");
    rejectedCbor({0x5b, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff}, "truncated");
    rejectedCbor({0x9b, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff}, "length");
    rejectedCbor({0xbb, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff}, "length");
    rejectedCbor({0x1b, 0xff}, "truncated");
    rejectedCbor({0x1c}, "reserved");
    for (const auto& invalid : {Bytes{0xc0, 0x80}, Bytes{0xed, 0xa0, 0x80}, Bytes{0xf4, 0x90, 0x80, 0x80}, Bytes{0xe2, 0x82}, Bytes{0xff}})
    {
        Bytes cbor;
        argument(cbor, 3, invalid.size());
        append(cbor, invalid);
        rejectedCbor(cbor, "UTF-8");
    }
    rejectedCbor({0xbf, 0xff}, "indefinite");
    rejectedCbor({0xc0, 0xf6}, "tags");
    rejectedCbor({0xf7}, "simple");
    auto trailing = document();
    trailing.push_back(0);
    rejectedCbor(trailing, "trailing");
    Bytes deep(66, 0x81);
    deep.push_back(0);
    rejectedCbor(deep, "depth");
    // Count is within input bytes but still cannot force a huge vector reservation.
    Bytes many;
    argument(many, 4, plugins::SerumState::maxNodes + 1);
    many.resize(many.size() + plugins::SerumState::maxNodes + 1, 0);
    rejectedCbor(many, "node limit");
}

TEST_CASE("Serum state: all nonfinite floating representations fail before parameter access")
{
    for (const auto& invalid : {Bytes{0xf9, 0x7c, 0}, Bytes{0xf9, 0xfc, 0}, Bytes{0xf9, 0x7e, 0},
                                Bytes{0xfa, 0x7f, 0x80, 0, 0}, Bytes{0xfa, 0x7f, 0xc0, 0, 0},
                                number(std::numeric_limits<double>::infinity()), number(std::numeric_limits<double>::quiet_NaN())})
        rejectedCbor(document(map({{"Synthetic", map({{"plainParams", map({{"gain", invalid}})}})}})), "nonfinite");
}

TEST_CASE("Serum state: rejects truncated container, huge declarations, corruption and trailing frames")
{
    const auto good = container(document());
    const size_t jsonLength = metadata().size(), payloadHeader = 17 + jsonLength;
    for (const size_t length : {size_t{0}, size_t{8}, size_t{16}, size_t{24}, payloadHeader + 7, good.size() - 1})
        CHECK(! plugins::decodeSerumState(std::span<const uint8_t>(good).first(length)));
    auto bad = good;
    bad[8] = 1;
    rejectedContainer(bad, "XferJson");
    bad = good;
    overwriteLittle(bad, 9, std::numeric_limits<uint64_t>::max(), 8);
    rejectedContainer(bad, "metadata length");
    bad = good;
    overwriteLittle(bad, payloadHeader, plugins::SerumState::maxDecodedBytes + 1, 4);
    rejectedContainer(bad, "size limit");
    bad = good;
    overwriteLittle(bad, payloadHeader, 1, 4);
    rejectedContainer(bad, "disagrees");
    bad = good;
    overwriteLittle(bad, payloadHeader + 4, 99, 4);
    rejectedContainer(bad, "encoding");
    bad = good;
    bad.back() ^= 0x80;
    rejectedContainer(bad, "decompression failed");
    bad = good;
    bad.push_back(0);
    rejectedContainer(bad, "trailing");
    bad = good;
    bad.insert(bad.end(), good.begin() + static_cast<ptrdiff_t>(payloadHeader + 8), good.end());
    rejectedContainer(bad, "concatenated");
    // A tiny otherwise valid frame can still request a 128 MiB streaming window.
    bad.assign(good.begin(), good.begin() + static_cast<ptrdiff_t>(payloadHeader + 8));
    overwriteLittle(bad, payloadHeader, 1, 4);
    append(bad, {0x28, 0xb5, 0x2f, 0xfd, 0, 0x88, 1, 0, 0});
    rejectedContainer(bad, "decompression failed");
    auto unknownSize = container(document(), metadata(), false);
    CHECK(plugins::decodeSerumState(unknownSize));
    overwriteLittle(unknownSize, payloadHeader, 1000, 4);
    rejectedContainer(unknownSize, "decoded size disagrees");
}

TEST_CASE("Serum state: strict metadata JSON rejects duplicate/invalid keys, escapes, UTF8 and nonfinite numbers")
{
    for (const auto& json : {std::string("{\"product\":\"Serum2\",\"product\":\"Serum2\"}"),
                            std::string("{\"a\":\"\\uD800\"}"), std::string("{\"a\":\"\\uDC00\"}"),
                            std::string("{\"a\":01}"), std::string("{\"a\":1.}"), std::string("{\"a\":1e}"),
                            std::string("{\"a\":1e999}"), std::string("{\"a\":\"\\q\"}"),
                            std::string("{\"a\":18446744073709551616}"),
                            std::string("{\"a\":\"\xff\"}"), metadata() + "true"})
        rejectedContainer(container(document(), json), "JSON");
}

TEST_CASE("Serum state: two large independently authored embedded float arrays fit the bounded tree budget")
{
    // Counts reproduce a real component's shape. All values are synthetic constants; no vendor data is retained.
    constexpr size_t firstCount = 284672, secondCount = 256000;
    const auto cbor = document(map({
        {"Oscillator0", map({{"WTOsc0", map({{"embeddedWTData", syntheticFloatArray(firstCount, 0.125f)}})}})},
        {"Oscillator1", map({{"WTOsc1", map({{"embeddedWTData", syntheticFloatArray(secondCount, -0.5f)}})}})}
    }));
    const auto encoded = container(cbor);
    const auto result = plugins::decodeSerumState(encoded);
    REQUIRE(result);
    CHECK(result.state->originalBytes() == encoded);
    CHECK(result.state->decodedBytes() == cbor);
    const auto* first = result.state->root().find("Oscillator0")->find("WTOsc0")->find("embeddedWTData")->array();
    const auto* second = result.state->root().find("Oscillator1")->find("WTOsc1")->find("embeddedWTData")->array();
    REQUIRE(first != nullptr && second != nullptr);
    CHECK(first->size() == firstCount);
    CHECK(second->size() == secondCount);
    for (const auto& value : *first) { REQUIRE(value.number()); CHECK_NEAR(*value.number(), 0.125, 0); }
    for (const auto& value : *second) { REQUIRE(value.number()); CHECK_NEAR(*value.number(), -0.5, 0); }
}

TEST_CASE("Serum state: cumulative small-element arrays still respect the independent total node limit")
{
    const size_t count = plugins::SerumState::maxNodes / 2 + 1;
    Bytes values;
    argument(values, 4, count);
    values.resize(values.size() + count, 0);
    // Each array separately fits the count limit; the entire tree must still be rejected before unbounded growth.
    rejectedCbor(document(map({{"SyntheticFirst", values}, {"SyntheticSecond", values}})), "node limit");
}

TEST_CASE("Serum preset: explicit file identity preserves observed versions without accepting them as hosted state")
{
    for (int version = 3; version <= 7; ++version)
    {
        const auto cbor = map({{"product", text("Serum2")}, {"fileType", text("SerumPreset")},
                               {"version", number(version)}, {"productVersion", text("2.0.24")},
                               {"presetName", text("Authored synthetic preset")},
                               {"UnknownPresetField", array({number(0.125), text("preserved")})}});
        const auto meta = "{\"product\":\"Serum2\",\"fileType\":\"SerumPreset\",\"version\":"
                        + std::to_string(version) + ",\"productVersion\":\"2.0.24\"}";
        const auto encoded = container(cbor, meta);
        const auto decoded = plugins::decodeSerumPreset(encoded);
        REQUIRE(decoded);
        CHECK(decoded.state->originalBytes() == encoded);
        CHECK(decoded.state->root().find("UnknownPresetField")->array()->size() == 2);
        CHECK(! plugins::decodeSerumState(encoded));
        CHECK(! plugins::decodeSerumCbor(cbor));
    }
    CHECK(! plugins::decodeSerumPreset(container(document())));
}

TEST_CASE("Serum preset: metadata and payload versions must match and unknown file versions remain rejected")
{
    const auto fixture = [](double version) {
        return map({{"product", text("Serum2")}, {"fileType", text("SerumPreset")},
                     {"version", number(version)}, {"productVersion", text("2.0.24")}});
    };
    const auto meta = "{\"product\":\"Serum2\",\"fileType\":\"SerumPreset\",\"version\":7,\"productVersion\":\"2.0.24\"}";
    auto decoded = plugins::decodeSerumPreset(container(fixture(6), meta));
    CHECK(! decoded);
    CHECK(decoded.error.find("mismatch: version") != std::string::npos);
    for (double version : {2.0, 3.5, 8.0, 9.0})
        CHECK(! plugins::decodeSerumPreset(container(fixture(version), meta)));
}

TEST_CASE("Serum import: explicit native adapter preserves source and separates measured scalar domains")
{
    using W = devices::WavetableSynth;
    const auto source = container(document(map({
        {"presetName", text("Authored import")},
        {"Global0", map({{"plainParams", map({{"kParamMasterVolume", number(0.25)},
            {"kParamBendRangeUp", number(-7)}, {"kParamBendRangeDn", number(9)}})}})},
        {"Oscillator0", map({{"plainParams", map({{"kParamVolume", number(0.5625)}, {"kParamPan", number(-25)},
            {"kParamDetuneWid", number(65)}, {"kParamUnisonStereo", number(-80)}})},
            {"WTOsc0", map({{"embeddedWTData", syntheticFloatArray(2048, 0.125f)},
                {"plainParams", map({{"kParamTablePos", number(128.5)}, {"kParamInitialPhase", number(90)}, {"kParamRandomPhase", number(0)}})}})}})},
        {"Env0", map({{"plainParams", map({{"kParamSustain", number(0.5)}, {"kParamAttack", number(0.123)}})}})},
        {"UnknownNativeImportField", array({text("retained"), number(123)})}
    })));
    const auto controller = container(document(map({}), "Serum2", "controller"), metadata("Serum2", "controller"));
    const auto result = plugins::importSerumPatch(source, "/tmp/own-source", controller);
    REQUIRE(result);
    REQUIRE(result.device.nativeContent);
    CHECK(result.device.kind == model::DeviceKind::Native);
    CHECK(result.device.type == "SonoraWavetable"); CHECK(result.device.displayName == "Authored import");
    CHECK(result.device.nativeContent->data == source); CHECK(result.device.nativeContent->controllerData == controller);
    CHECK(result.device.nativeContent->sourceDirectory == "/tmp/own-source");
    CHECK(! result.completeFeatureSupport); CHECK(! result.audioVerified); CHECK(! result.warnings.empty());
    std::vector<std::string> warnings;
    auto processor = devices::createNative(result.device, &warnings);
    auto* synth = dynamic_cast<W*>(processor.get()); REQUIRE(synth);
    CHECK(onlyMissingFactoryTables(warnings)); REQUIRE(synth->customTable(0));
    CHECK(synth->customTable(0)->frames() == 1);
    CHECK_NEAR(synth->customTable(0)->originalSamples()[42], 0.125 * 1.00316, 1e-7); // measured reference table read gain
    CHECK(synth->customTable(0)->frameLayout() == devices::WavetableFrameLayout::Steps);
    CHECK_NEAR(synth->getParameter(W::Volume), -9.03089987, 1e-5);
    CHECK_NEAR(synth->getParameter(W::BendUp), -7, 0); CHECK_NEAR(synth->getParameter(W::BendDown), -9, 0);
    CHECK_NEAR(synth->getParameter(W::oscParam(0, W::Level)), 0.5625, 0);
    CHECK_NEAR(synth->getParameter(W::oscParam(0, W::OscPan)), -0.5, 0);
    CHECK_NEAR(synth->getParameter(W::oscParam(0, W::Blend)), 0.65, 1e-6);
    CHECK_NEAR(synth->getParameter(W::oscParam(0, W::Width)), -0.8, 1e-6);
    CHECK_NEAR(synth->getParameter(W::oscParam(0, W::Position)), 0.5, 1e-6);
    CHECK_NEAR(synth->getParameter(W::oscParam(0, W::Phase)), 0.25, 0);
    CHECK_NEAR(synth->getParameter(W::envParam(0, W::Sustain)), 0.5, 0);
    CHECK_NEAR(synth->getParameter(W::envParam(0, W::Attack)), 0.123, 1e-6);
    const auto wrongController = container(document());
    CHECK(! plugins::importSerumPatch(source, {}, wrongController));
    CHECK(! plugins::importSerumPatch(controller));
    CHECK(! plugins::importSerumPatch(container(document(map({}), "Serum2FX"), metadata("Serum2FX"))));
}

TEST_CASE("Serum import: unsupported oscillator types retain content and report missing native paths")
{
    const auto source = container(document(map({{"Oscillator0", map({{"plainParams", map({
        {"kParamType", text("UnknownFutureEngine")}, {"kParamEnable", number(1)}})}})}})));
    auto result = plugins::importSerumPatch(source); REQUIRE(result);
    REQUIRE(result.device.nativeContent); CHECK(result.device.nativeContent->data == source);
    std::vector<std::string> warnings;
    auto synth = devices::createNative(result.device, &warnings); REQUIRE(synth);
    CHECK(! warnings.empty());
    CHECK_NEAR(synth->getParameter(devices::WavetableSynth::oscParam(0, devices::WavetableSynth::On)), 0, 0);
}

TEST_CASE("Serum import: seven source rows defaults sends envelope placement and enabled filter levels retain measured domains")
{
    using W = devices::WavetableSynth;
    const auto source = container(document(map({
        {"Global0", map({{"plainParams", map({{"kParamDirectVol", number(.125)}})}})},
        {"RoutingSlot0", map({{"plainParams", map({{"kParamRoutingDest", text("kRoutingDestNone")}, {"kParamFilterBalance", number(50)},
            {"kParamFXBus1Level", number(50)}, {"kParamFXBus2Level", number(75)}, {"kParamViaEnv1", number(0)}})}})},
        {"RoutingSlot5", map({{"plainParams", map({{"kParamRoutingDest", text("kRoutingDestFilter")}, {"kParamFXBus1Level", number(25)}})}})},
        {"RoutingSlot6", map({{"plainParams", map({{"kParamRoutingDest", text("kRoutingDestDirect")}, {"kParamViaEnv1", number(0)}})}})},
        {"VoiceFilter0", map({{"plainParams", map({{"kParamEnable", number(1)}, {"kParamLevelOut", number(.75)}})}})},
        {"VoiceFilter1", map({{"plainParams", map({{"kParamEnable", number(0)}, {"kParamLevelOut", number(0)}})}})}
    })));
    const auto result = plugins::importSerumPatch(source); REQUIRE(result);
    CHECK(result.device.nativeContent->data == source); CHECK(! result.audioVerified && ! result.completeFeatureSupport);
    auto processor = devices::createNative(result.device); auto* synth = dynamic_cast<W*>(processor.get()); REQUIRE(synth);
    CHECK_NEAR(synth->getParameter(W::SourceRoutingOn), 1, 0); CHECK_NEAR(synth->getParameter(W::DirectGain), .25, 0);
    CHECK_NEAR(synth->getParameter(W::sourceRoutingParam(0, W::SourceDestination)), W::RouteToNone, 0);
    CHECK_NEAR(synth->getParameter(W::sourceRoutingParam(0, W::SourceFilterBalance)), .5, 0);
    CHECK_NEAR(synth->getParameter(W::sourceRoutingParam(0, W::SourceBus1)), .5, 0);
    CHECK_NEAR(synth->getParameter(W::sourceRoutingParam(0, W::SourceBus2)), .75, 0);
    CHECK_NEAR(synth->getParameter(W::sourceRoutingParam(0, W::SourceEnvelope)), 0, 0);
    for (int row = 1; row < 5; ++row)
    {
        CHECK_NEAR(synth->getParameter(W::sourceRoutingParam(row, W::SourceDestination)), W::RouteToMain, 0);
        CHECK_NEAR(synth->getParameter(W::sourceRoutingParam(row, W::SourceBus1)), 0, 0);
        CHECK_NEAR(synth->getParameter(W::sourceRoutingParam(row, W::SourceBus2)), 0, 0);
        CHECK_NEAR(synth->getParameter(W::sourceRoutingParam(row, W::SourceEnvelope)), 1, 0);
    }
    CHECK_NEAR(synth->getParameter(W::sourceRoutingParam(5, W::SourceDestination)), W::RouteToFilters, 0);
    CHECK_NEAR(synth->getParameter(W::sourceRoutingParam(5, W::SourceBus1)), .25, 0);
    CHECK_NEAR(synth->getParameter(W::sourceRoutingParam(6, W::SourceDestination)), W::RouteToDirect, 0);
    CHECK_NEAR(synth->getParameter(W::sourceRoutingParam(6, W::SourceEnvelope)), 0, 0);
    CHECK_NEAR(synth->getParameter(W::filterOutputParam(0)), 2.25, 0); CHECK_NEAR(synth->getParameter(W::filterOutputParam(1)), 0, 0);
    CHECK_NEAR(synth->getParameter(W::filterParam(1, W::FilterOn)), 0, 0);
    auto saved = result.device; devices::captureDeviceState(saved, synth); auto rebuilt = devices::createNative(saved); REQUIRE(rebuilt);
    for (int parameter = W::SourceRoutingStart; parameter < W::ParameterCount; ++parameter)
        CHECK_NEAR(rebuilt->getParameter(parameter), synth->getParameter(parameter), 0);
}

TEST_CASE("Serum import: symbolic routing choices numeric compatibility unknown defaults and cycles are explicit")
{
    using W = devices::WavetableSynth;
    for (const auto& entry : std::vector<std::pair<Bytes, int>>{
        {text("kRoutingDestFilter"), W::RouteToFilters}, {text("kRoutingDestMaster"), W::RouteToMain},
        {text("kRoutingDestDirect"), W::RouteToDirect}, {text("kRoutingDestNone"), W::RouteToNone}, {number(2), W::RouteToDirect}})
    {
        const auto result = plugins::importSerumPatch(container(document(map({
            {"RoutingSlot0", map({{"plainParams", map({{"kParamRoutingDest", entry.first}})}})}})))); REQUIRE(result);
        const auto synth = devices::createNative(result.device); REQUIRE(synth);
        CHECK_NEAR(synth->getParameter(W::sourceRoutingParam(0, W::SourceDestination)), entry.second, 0);
    }
    for (const auto& invalid : {text("kRoutingDestFuture"), number(.5), number(4)})
    {
        const auto result = plugins::importSerumPatch(container(document(map({
            {"RoutingSlot1", map({{"plainParams", map({{"kParamRoutingDest", invalid}})}})}})))); REQUIRE(result);
        const auto synth = devices::createNative(result.device); REQUIRE(synth);
        CHECK_NEAR(synth->getParameter(W::sourceRoutingParam(1, W::SourceDestination)), W::RouteToMain, 0);
        CHECK(std::any_of(result.warnings.begin(), result.warnings.end(), [](const auto& warning) { return warning.find("Unrecognized source routing") != std::string::npos; }));
    }
    const auto cyclic = plugins::importSerumPatch(container(document(map({
        {"RoutingSlot5", map({{"plainParams", map({{"kParamRoutingDest", text("kRoutingDestFilter")}})}})},
        {"RoutingSlot6", map({{"plainParams", map({{"kParamRoutingDest", text("kRoutingDestFilter")}})}})}
    })))); REQUIRE(cyclic);
    CHECK(std::any_of(cyclic.warnings.begin(), cyclic.warnings.end(), [](const auto& warning) { return warning.find("Cyclic filter routing") != std::string::npos; }));
}

TEST_CASE("Serum import: exact source bus matrix addresses restore all fourteen targets and retain the Sub sparse level")
{
    using W = devices::WavetableSynth;
    for (int source = 0; source < W::kRoutingSources; ++source) for (int bus = 0; bus < 2; ++bus)
    {
        const auto slot = map({{"source", array({integer(25), integer(0)})}, {"destModuleTypeString", text("RoutingSlot")},
            {"destModuleID", integer(static_cast<uint64_t>(source))}, {"destModuleParamName", text(bus == 0 ? "kParamFXBus1Level" : "kParamFXBus2Level")},
            {"destModuleParamID", integer(static_cast<uint64_t>(bus + 1))},
            {"plainParams", map({{"kParamAmount", number(50)}, {"kParamOut", number(100)}})}});
        const auto bytes = container(document(map({{"ModSlot0", slot}, {"Oscillator4", map({{"plainParams", map({{"kParamEnable", number(1)}})}})}})));
        const auto result = plugins::importSerumPatch(bytes); REQUIRE(result); CHECK(result.device.nativeContent->data == bytes);
        CHECK(std::none_of(result.warnings.begin(), result.warnings.end(), [](const auto& warning) { return warning.find("modulation routes require") != std::string::npos; }));
        auto synth = devices::createNative(result.device); REQUIRE(synth);
        CHECK_NEAR(synth->getParameter(W::modParam(0, W::ModDestination)), W::sourceSendDestination(source, bus), 0);
        CHECK_NEAR(synth->getParameter(W::modParam(0, W::ModAmount)), .5, 0);
        CHECK_NEAR(synth->getParameter(W::SubLevel), .75, 0);
        auto saved = result.device; devices::captureDeviceState(saved, synth.get()); auto restored = devices::createNative(saved); REQUIRE(restored);
        CHECK_NEAR(restored->getParameter(W::modParam(0, W::ModDestination)), W::sourceSendDestination(source, bus), 0);
    }
}

TEST_CASE("Serum import: Sub phase pan and full octave range survive capture with explicit continuity and shape limitations")
{
    using W = devices::WavetableSynth;
    const auto bytes = container(document(map({{"Oscillator4", map({
        {"plainParams", map({{"kParamEnable", number(1)}, {"kParamOctave", number(4)}, {"kParamPan", number(-25)}})},
        {"SubOsc4", map({{"plainParams", map({{"kParamInitialPhase", number(90)}, {"kParamContiguousPhase", number(1)}, {"kParamShape", text("kSquare")}})}})}
    })}})));
    auto result = plugins::importSerumPatch(bytes); REQUIRE(result); auto processor = devices::createNative(result.device); REQUIRE(processor);
    CHECK_NEAR(processor->getParameter(W::SubLevel), .75, 0); CHECK_NEAR(processor->getParameter(W::SubOctave), 4, 0);
    CHECK_NEAR(processor->getParameter(W::SubPan), -.5, 0); CHECK_NEAR(processor->getParameter(W::SubPhase), .25, 0);
    CHECK_NEAR(processor->getParameter(W::SubContinuous), 1, 0); CHECK_NEAR(processor->getParameter(W::SubWave), 3, 0);
    CHECK(std::any_of(result.warnings.begin(), result.warnings.end(), [](const auto& w) { return w.find("Sub waveform") != std::string::npos; }));
    CHECK(std::any_of(result.warnings.begin(), result.warnings.end(), [](const auto& w) { return w.find("Sub phase continuity") != std::string::npos; }));
    devices::captureDeviceState(result.device, processor.get()); CHECK(result.device.nativeContent);
    auto restored = devices::createNative(result.device); REQUIRE(restored);
    for (const int parameter : std::array<int, 5>{W::SubPhase, W::SubContinuous, W::SubWave, W::SubOctave, W::SubPan})
        CHECK_NEAR(restored->getParameter(parameter), processor->getParameter(parameter), 0);
}

TEST_CASE("Serum import: measured LFO real point count closure phase and envelope endpoint domains survive preparation")
{
    using W = devices::WavetableSynth;
    const auto source = container(document(map({
        {"Env1", map({{"plainParams", map({{"kParamStart", number(0.5)}, {"kParamEnd", number(0.75)}})}})},
        {"LFO0", map({{"plainParams", map({{"kParamMode", text("Envelope")}, {"kParamPhase", number(180)},
            {"kParamRate", number(0.001)}, {"kParamBeatSync", number(0)}})},
            {"curveData", map({{"numPoints", integer(2)}, {"xVals", array({number(0), number(0.5), number(1)})},
                {"yVals", array({number(0.1), number(0.7), number(0.9)})},
                {"curveVals", array({number(0.5), number(0.5), number(0.5)})}})}})},
        {"LFO1", map({{"curveData", map({{"numPoints", integer(1)},
            {"xVals", array({number(0), number(1)})}, {"yVals", array({number(0.25), number(0.75)})},
            {"curveVals", array({number(0.5), number(0.5)})}})}})},
        {"LFO2", map({{"curveData", map({{"xVals", array({number(0), number(0.33), number(0.67), number(1)})},
            {"yVals", array({number(0.1), number(0.4), number(0.8), number(0.9)})},
            {"curveVals", array({number(0.5), number(0.5), number(0.5), number(0.5)})}})}})}
    })));
    const auto result = plugins::importSerumPatch(source); REQUIRE(result);
    std::vector<std::string> warnings; auto processor = devices::createNative(result.device, &warnings);
    auto* synth = dynamic_cast<W*>(processor.get()); REQUIRE(synth); CHECK(onlyMissingFactoryTables(warnings));
    CHECK(result.device.nativeContent->data == source);
    CHECK(synth->usesImportedEnvelopeProfile()); CHECK(synth->usesImportedLfoProfile());
    CHECK_NEAR(synth->getParameter(W::envLevelParam(1, false)), 0.5, 0);
    CHECK_NEAR(synth->getParameter(W::envLevelParam(1, true)), 0.75, 0);
    CHECK_NEAR(synth->getParameter(W::lfoParam(0, W::Mode)), W::OneShot, 0);
    CHECK_NEAR(synth->getParameter(W::lfoParam(0, W::LfoPhase)), 0.5, 0);
    CHECK_NEAR(synth->getParameter(W::lfoParam(0, W::Rate)), 0.001, 1e-9);
    const auto curve = synth->lfoCurve(0); CHECK(curve.count == 3);
    CHECK_NEAR(curve.x[1], 0.5, 0); CHECK_NEAR(curve.y[0], 0.9, 1e-7); CHECK_NEAR(curve.y[1], 0.3, 1e-7);
    CHECK_NEAR(curve.y[2], curve.y[0], 0); // A distinct extra stored closing Y is ignored.
    const auto constant = synth->lfoCurve(1); CHECK(constant.count == 2);
    CHECK_NEAR(constant.y[0], 0.75, 0); CHECK_NEAR(constant.y[1], 0.75, 0);
    const auto sparseCount = synth->lfoCurve(2); CHECK(sparseCount.count == 3);
    CHECK_NEAR(sparseCount.x[1], 0.33, 1e-7); CHECK_NEAR(sparseCount.x[2], 1, 0); CHECK_NEAR(sparseCount.y[2], sparseCount.y[0], 0);
}


TEST_CASE("Serum import: flex segment count endpoints independent Out and LFO timing controls retain source")
{
    using W = devices::WavetableSynth;
    const auto curve = map({{"numPoints", integer(2)},
        {"xVals", array({number(.2), number(.4), number(.8)})},
        {"yVals", array({number(1), number(.2), number(.8)})},
        {"curveVals", array({number(.5), number(.25), number(.5)})}});
    const auto source = container(document(map({
        {"ModSlot63", map({{"source", array({integer(25), integer(26)})},
            {"destModuleTypeString", text("Oscillator")}, {"destModuleID", integer(0)},
            {"destModuleParamName", text("kParamCoarsePit")}, {"destModuleParamID", integer(6)},
            {"flex", array({curve, curve})}, {"plainParams", map({{"kParamAmount", number(25)}, {"kParamOut", number(50)},
                {"kParamMainCurveData", number(1)}, {"kParamAuxCurveData", number(1)}, {"kParamBipolar", number(1)}})}})},
        {"LFO0", map({{"plainParams", map({{"kParamRate", number(100)}, {"kParamDotted", number(1)},
            {"kParamTriplets", number(1)}, {"kParamAnchored", number(0)}, {"kParamRate10x", number(1)}})}})},
        {"Global0", map({{"plainParams", map({{"kParamMonoToggle", number(1)}, {"kParamLegato", number(1)},
            {"kParamPortaScaled", number(1)}, {"kParamPortamentoTime", number(.25)}, {"kParamPortamentoCurve", number(-50)}})}})}
    })));
    const auto result = plugins::importSerumPatch(source); REQUIRE(result);
    CHECK(result.device.nativeContent->data == source);
    std::vector<std::string> warnings; auto processor = devices::createNative(result.device, &warnings);
    auto* synth = dynamic_cast<W*>(processor.get()); REQUIRE(synth);
    CHECK_NEAR(synth->getParameter(W::modParam(63, W::ModAmount)), .25, 0);
    CHECK_NEAR(synth->getParameter(W::outParam(63)), .5, 0);
    for (const bool auxiliary : {false, true})
    {
        const auto actual = synth->modulationCurve(63, auxiliary); CHECK(actual.enabled);
        CHECK(actual.law == W::CurveLaw::ImportedExponential); CHECK(actual.graph.count == 3);
        CHECK_NEAR(actual.graph.x[0], 0, 0); CHECK_NEAR(actual.graph.x[1], .4, 1e-7); CHECK_NEAR(actual.graph.x[2], 1, 0);
        CHECK_NEAR(actual.graph.y[2], .2, 1e-7); // Real flex endpoint, not LFO closure.
        CHECK_NEAR(synth->modulationCurveAt(63, auxiliary, .25f), .5, 1e-6);
    }
    CHECK_NEAR(W::divisionBeats(static_cast<int>(synth->getParameter(W::lfoParam(0, W::Division)))), 1.0/128, 0);
    CHECK_NEAR(synth->getParameter(W::lfoAnchorParam(0)), 0, 0);
    CHECK_NEAR(synth->getParameter(W::lfoRateScaleParam(0)), 1, 0);
    CHECK_NEAR(synth->getParameter(W::voiceModeParam(W::MonoOn)), 1, 0);
    CHECK_NEAR(synth->getParameter(W::voiceModeParam(W::PortaTime)), .25, 0);
    CHECK_NEAR(synth->getParameter(W::voiceModeParam(W::PortaCurve)), -50, 0);
}


TEST_CASE("Serum import: Noise scalar defaults and piecewise pitch preserve original processor controller state")
{
    using W = devices::WavetableSynth;
    const auto controller = container(document(map({{"UnknownController", text("retain exact bytes")}}), "Serum2", "controller"), metadata("Serum2", "controller"));
    for (const double colour : {.125, .45, .5, .625, 1.0})
    {
        const auto encoded = container(document(map({
            {"Oscillator0", map({{"plainParams", map({{"kParamEnable", number(0)}})},
                {"WTOsc0", map({{"embeddedWTData", syntheticFloatArray(2048, .25f)}, {"numChannels", number(1)}, {"sampleRate", number(48000)}})}})},
            {"Oscillator3", map({{"plainParams", map({{"kParamEnable", number(1)}, {"kParamPan", number(-25)}, {"kParamPitchTrack", number(1)}})},
                {"NoiseOsc3", map({{"relativePathToNoiseSample", text("sonora-own-missing-noise.wav")},
                    {"plainParams", map({{"kParamColor", number(colour)}, {"kParamFine", number(-.5)}, {"kParamInitialPhase", number(75)}, {"kParamRandomPhase", number(50)}})}})}})},
            {"UnknownSourceSentinel", text("retained")}
        })));
        auto imported = plugins::importSerumPatch(encoded, "/tmp/sonora_tests/noise_import", controller); REQUIRE(imported);
        REQUIRE(imported.device.nativeContent); CHECK(imported.device.nativeContent->data == encoded);
        CHECK(imported.device.nativeContent->controllerData == controller);
        std::vector<std::string> warnings; auto processor = devices::createNative(imported.device, &warnings);
        auto* synth = dynamic_cast<W*>(processor.get()); REQUIRE(synth);
        CHECK_NEAR(synth->getParameter(W::NoiseLevel), .0625, 0); CHECK_NEAR(synth->getParameter(W::NoisePan), -.5, 0);
        CHECK_NEAR(synth->getParameter(W::NoisePitch), colour < .5 ? colour * 384 - 192 : colour * 96 - 48, 2e-5);
        CHECK_NEAR(synth->getParameter(W::NoiseFine), -48, 0); CHECK_NEAR(synth->getParameter(W::NoiseKeytrack), 1, 0);
        CHECK_NEAR(synth->getParameter(W::NoisePhase), .75, 0); CHECK_NEAR(synth->getParameter(W::NoiseRandom), .5, 0);
        CHECK(synth->requestedVoiceRateFactor() == 2); CHECK(synth->usesImportedNoiseProfile()); CHECK(synth->expectsNoiseSample());
        CHECK(synth->noiseSample() == nullptr); CHECK(synth->noiseSampleName() == "Missing Noise Sample");
        REQUIRE(synth->customTable(0)); CHECK(synth->customTable(0)->playbackProfile() == devices::WavetableData::PlaybackProfile::Octave16Approx);
        CHECK(std::any_of(warnings.begin(), warnings.end(), [](const auto& value) { return value.find("Noise sample not found") != std::string::npos; }));
        synth->prepare(48000, 512); CHECK(synth->voiceRateFactor() == 2);
        juce::AudioBuffer<float> audio(2, 1024); audio.clear(); juce::MidiBuffer midi;
        midi.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)), 17); synth->process(audio, midi);
        CHECK_NEAR(audio.getMagnitude(0, audio.getNumSamples()), 0, 0);
        std::string error; REQUIRE(devices::storeWavetableVoiceRate(imported.device, 1, error));
        auto overridden = devices::createNative(imported.device); auto* ownQuality = dynamic_cast<W*>(overridden.get()); REQUIRE(ownQuality);
        CHECK(ownQuality->requestedVoiceRateFactor() == 1); CHECK(imported.device.nativeContent->controllerData == controller);
    }
}

TEST_CASE("Serum import: own external Noise PCM uses the NoiseOsc3 path and sparse defaults without copying assets")
{
    using W = devices::WavetableSynth;
    const juce::File directory("/tmp/sonora_tests/noise_import"); REQUIRE(directory.createDirectory().wasOk());
    const auto file = directory.getChildFile("Own Noise.wav"); REQUIRE(file.deleteFile());
    {
        std::unique_ptr<juce::OutputStream> stream(file.createOutputStream()); REQUIRE(stream); juce::WavAudioFormat format;
        auto writer = format.createWriterFor(stream, juce::AudioFormatWriterOptions{}.withSampleRate(32000).withNumChannels(1).withBitsPerSample(24));
        REQUIRE(writer); juce::AudioBuffer<float> audio(1, 257);
        for (int sample = 0; sample < 257; ++sample) audio.setSample(0, sample, static_cast<float>(std::sin(sample * .1) * .25));
        REQUIRE(writer->writeFromAudioSampleBuffer(audio, 0, 257));
    }
    const auto expected = devices::OscillatorSample::fromFile(file); REQUIRE(expected);
    const auto encoded = container(document(map({
        {"Oscillator3", map({{"plainParams", map({{"kParamEnable", number(1)}})},
            {"NoiseOsc3", map({{"relativePathToNoiseSample", text("Own Noise.wav")}, {"plainParams", text("default")},
                {"numFrames", number(257)}, {"numChannels", number(1)}, {"sampleRate", number(32000)}, {"unknownNoiseField", text("retained")}})}})}
    })));
    auto imported = plugins::importSerumPatch(encoded, directory.getFullPathName().toStdString()); REQUIRE(imported);
    std::vector<std::string> warnings; auto processor = devices::createNative(imported.device, &warnings); auto* synth = dynamic_cast<W*>(processor.get());
    REQUIRE(synth); REQUIRE(synth->noiseSample()); CHECK(synth->noiseSample()->originalSamples() == expected.data->originalSamples());
    CHECK(synth->noiseSample()->pcmFingerprint() == expected.data->pcmFingerprint());
    CHECK_NEAR(synth->getParameter(W::NoisePitch), 0, 0); CHECK_NEAR(synth->getParameter(W::NoiseFine), 0, 0);
    CHECK_NEAR(synth->getParameter(W::NoisePhase), 0, 0); CHECK_NEAR(synth->getParameter(W::NoiseRandom), 0, 0);
    CHECK_NEAR(synth->getParameter(W::NoiseLevel), .0625, 0); CHECK(imported.device.nativeContent->data == encoded);
    std::string error; REQUIRE(devices::storeWavetableNoise(imported.device, nullptr, error));
    auto generated = devices::createNative(imported.device); auto* own = dynamic_cast<W*>(generated.get()); REQUIRE(own);
    CHECK(! own->expectsNoiseSample()); CHECK(! own->usesImportedNoiseProfile()); CHECK(own->noiseSample() == nullptr);
}

TEST_CASE("Serum import: MG Low filter types, the implicit default filter and the vendor drive law map to the native ladder")
{
    using W = devices::WavetableSynth;
    const auto mapOf = [](const std::vector<std::pair<std::string, Bytes>>& members) {
        Bytes out;
        argument(out, 5, members.size());
        for (const auto& [key, value] : members) { append(out, text(key)); append(out, value); }
        return out;
    };
    const auto build = [&](const std::vector<std::pair<std::string, Bytes>>& fields) {
        std::vector<std::pair<std::string, Bytes>> entries{{"kParamEnable", number(1)}, {"kParamFreq", number(.6)}, {"kParamReso", number(35)}};
        for (const auto& f : fields) entries.push_back(f);
        const auto result = plugins::importSerumPatch(container(document(map({{"VoiceFilter0", map({{"plainParams", mapOf(entries)}})}}))));
        REQUIRE(result);
        auto synth = devices::createNative(result.device); REQUIRE(synth);
        return std::make_pair(std::move(synth), result.warnings);
    };
    for (const auto& [name, mode] : std::vector<std::pair<std::string, int>>{{"MgL6", W::MgLow6}, {"MgL12", W::MgLow12}, {"MgL18", W::MgLow18}, {"MgL24", W::MgLow24}})
    {
        auto [synth, warnings] = build({{"kParamType", text(name)}});
        CHECK_NEAR(synth->getParameter(W::filterParam(0, W::FilterMode)), mode, 0);
        CHECK_NEAR(synth->getParameter(W::filterParam(0, W::Resonance)), .35, 1e-6);
        CHECK_NEAR(synth->getParameter(W::filterParam(0, W::Drive)), 0, 1e-6); // drive 0 -> 1x
        CHECK(std::none_of(warnings.begin(), warnings.end(), [](const auto& w) { return w.find("not implemented") != std::string::npos; }));
    }
    // vendor default (no kParamType) is MG Low 12
    auto [defaulted, defaultWarnings] = build({});
    CHECK_NEAR(defaulted->getParameter(W::filterParam(0, W::FilterMode)), W::MgLow12, 0);
    // drive percent -> input gain in dB through the measured law (50 % -> x5.061)
    auto [driven, drivenWarnings] = build({{"kParamType", text("MgL24")}, {"kParamDrive", number(50)}});
    CHECK_NEAR(driven->getParameter(W::filterParam(0, W::Drive)), 20 * std::log10(5.061), 0.01);
    // types without a native model are approximated by their nearest relative, and say so
    auto [ringMod, ringModWarnings] = build({{"kParamType", text("RM")}});
    CHECK(std::any_of(ringModWarnings.begin(), ringModWarnings.end(), [](const auto& w) { return w.find("RM is approximated") != std::string::npos; }));
    CHECK_NEAR(ringMod->getParameter(W::filterParam(0, W::FilterMode)), W::FilterThrough, 0);
    // measured comb / multi / diode types map exactly (dsp/MeasuredCombFilter.h, MeasuredMultiFilter.h, DiodeLadderFilter.h)
    auto [comb, combWarnings] = build({{"kParamType", text("CombHL6P")}});
    CHECK(std::none_of(combWarnings.begin(), combWarnings.end(), [](const auto& w) { return w.find("approximated") != std::string::npos; }));
    CHECK_NEAR(comb->getParameter(W::filterParam(0, W::FilterMode)), W::SerumCombHL6P, 0);
    auto [multi, multiWarnings] = build({{"kParamType", text("LH12")}, {"kParamVar", number(40)}});
    CHECK_NEAR(multi->getParameter(W::filterParam(0, W::FilterMode)), W::SerumLH12, 0);
    CHECK_NEAR(multi->getParameter(W::filterParam(0, W::FilterMorph)), .4, 1e-6); // Var: the second cutoff (MIDI note 128 x Var)
    auto [acid, acidWarnings] = build({{"kParamType", text("LadderAcid")}, {"kParamVar", number(40)}});
    CHECK_NEAR(acid->getParameter(W::filterParam(0, W::FilterMode)), W::SerumLadderAcid, 0);
    CHECK(std::none_of(acidWarnings.begin(), acidWarnings.end(), [](const auto& w) { return w.find("Var is not implemented") != std::string::npos; }));
    auto [phaser, phaserWarnings] = build({{"kParamType", text("Phase48HL6P")}});
    CHECK_NEAR(phaser->getParameter(W::filterParam(0, W::FilterMode)), W::SerumPhase48HL6P, 0);
    CHECK(std::any_of(phaserWarnings.begin(), phaserWarnings.end(), [](const auto& w) { return w.find("approximate loop filter") != std::string::npos; }));
    // the measured normal family and Ladder MG map exactly; Var feeds the gain compensation of the normal family
    for (const auto& [name, mode] : std::vector<std::pair<std::string, int>>{{"L6", W::SerumLow6}, {"H18", W::SerumHigh18}, {"B24", W::SerumBand24},
                                                                             {"N12", W::SerumNotch12}, {"LadderMg", W::SerumLadderMg}, {"DirtyMg", W::SerumDirtyMg}, {"LadderEMS", W::SerumLadderEms}})
    {
        auto [synth, warnings] = build({{"kParamType", text(name)}, {"kParamVar", number(30)}});
        CHECK_NEAR(synth->getParameter(W::filterParam(0, W::FilterMode)), mode, 0);
        CHECK(std::none_of(warnings.begin(), warnings.end(), [](const auto& w) { return w.find("approximated") != std::string::npos; }));
        if (W::isMeasuredSvfFilter(mode)) CHECK_NEAR(synth->getParameter(W::filterParam(0, W::FilterMorph)), .3, 1e-6);
    }
    // On the MG ladders Var restores that fraction of the resonance level loss (measured); comb types take it as their loop control,
    // unmeasured types still report it
    auto [vary, varyWarnings] = build({{"kParamType", text("MgL24")}, {"kParamVar", number(25)}});
    CHECK_NEAR(vary->getParameter(W::filterParam(0, W::FilterMorph)), .25, 1e-6);
    CHECK(std::none_of(varyWarnings.begin(), varyWarnings.end(), [](const auto& w) { return w.find("Var is not implemented") != std::string::npos; }));
    auto [combVar, combVarWarnings] = build({{"kParamType", text("CombP")}, {"kParamVar", number(25)}});
    CHECK(std::none_of(combVarWarnings.begin(), combVarWarnings.end(), [](const auto& w) { return w.find("Var is not implemented") != std::string::npos; }));
    CHECK_NEAR(combVar->getParameter(W::filterParam(0, W::FilterMorph)), .25, 1e-6);
    auto [diffuser, diffuserWarnings] = build({{"kParamType", text("Diffuser")}, {"kParamVar", number(25)}});
    CHECK(std::any_of(diffuserWarnings.begin(), diffuserWarnings.end(), [](const auto& w) { return w.find("Var is not implemented") != std::string::npos; }));
}

TEST_CASE("Serum import: table files follow the clm frame flag and are peak-normalised with the measured read gain")
{
    using W = devices::WavetableSynth;
    // Independently authored three-frame float WAV: frame k is a sine of amplitude 0.2 (k + 1).
    const auto writeTable = [](const juce::File& file, const char* clm) {
        std::vector<float> samples(3 * 2048);
        for (size_t i = 0; i < samples.size(); ++i)
            samples[i] = 0.2f * static_cast<float>(i / 2048 + 1) * static_cast<float>(std::sin(2 * M_PI * static_cast<double>(i % 2048) / 2048));
        Bytes riff{'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E'};
        const auto chunk = [&](const char* id, const void* data, size_t size) {
            riff.insert(riff.end(), id, id + 4); little(riff, size, 4);
            riff.insert(riff.end(), static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size);
            if (size & 1) riff.push_back(0);
        };
        const std::array<uint16_t, 8> format{3, 1, 0xac44, 0, 0xb110, 0x0002, 4, 32}; // float32 mono 44.1 kHz
        chunk("fmt ", format.data(), 16);
        if (clm != nullptr) chunk("clm ", clm, std::strlen(clm));
        chunk("data", samples.data(), samples.size() * sizeof(float));
        overwriteLittle(riff, 4, riff.size() - 8, 4);
        REQUIRE(file.replaceWithData(riff.data(), riff.size()));
    };
    const auto folder = juce::File::createTempFile("serum-clm"); REQUIRE(folder.createDirectory());
    const struct { const char* name; const char* clm; devices::WavetableFrameLayout layout; } cases[] = {
        {"steps.wav", "<!>2048 01000000 wavetable", devices::WavetableFrameLayout::Steps},
        {"spectral.wav", "<!>2048 21000000 wavetable", devices::WavetableFrameLayout::Interpolated256},
        {"plain.wav", nullptr, devices::WavetableFrameLayout::Interpolated256}};
    for (const auto& item : cases)
    {
        const auto file = folder.getChildFile(item.name); writeTable(file, item.clm);
        const auto result = plugins::importSerumPatch(container(document(map({{"Oscillator0", map({{"WTOsc0",
            map({{"relativePathToWT", text(file.getFullPathName().toStdString())}})}})}}))));
        REQUIRE(result);
        auto processor = devices::createNative(result.device); auto* synth = dynamic_cast<W*>(processor.get()); REQUIRE(synth);
        const auto* table = synth->customTable(0); REQUIRE(table);
        CHECK(table->frames() == 3); CHECK(table->frameLayout() == item.layout);
        float peak = 0;
        for (const float sample : table->originalSamples()) peak = std::max(peak, std::abs(sample));
        CHECK_NEAR(peak, 1.00316, 1e-6);
        // Quarter-cycle value per position: Steps snaps to floor(3 x); the 256-cell grid crossfades at cell * 2 / 255.
        const auto quarter = [&](float position) { return table->sample(position, 0.25, 0); };
        const double unit = 1.00316 / 3;
        if (item.layout == devices::WavetableFrameLayout::Steps)
        {
            CHECK_NEAR(quarter(0.3f), unit, 2e-3); CHECK_NEAR(quarter(0.34f), 2 * unit, 2e-3); CHECK_NEAR(quarter(0.99f), 3 * unit, 2e-3);
        }
        else
        {
            CHECK_NEAR(quarter(0.5f), unit * (1 + 2 * 128.0 / 255), 2e-3);
            CHECK_NEAR(quarter(0.5005f), quarter(0.5035f), 0); // same 1/256 cell
            CHECK(std::abs(quarter(0.5045f) - quarter(0.5035f)) > 1e-4);
        }
    }
    folder.deleteRecursively();
}

TEST_CASE("Serum import: Self PD and sourced warp menu names map to the native warp modes")
{
    using W = devices::WavetableSynth;
    const auto result = plugins::importSerumPatch(container(document(map({{"Oscillator0", map({{"WTOsc0", map({{"plainParams",
        map({{"kParamWarpMenu", text("kSelfPD")}, {"kParamWarp", number(0.4)}, {"kParamWarpMenu2", text("kFM_OSC")}, {"kParamWarp2", number(0.3)}})}})}})}}))));
    REQUIRE(result);
    auto processor = devices::createNative(result.device); auto* synth = dynamic_cast<W*>(processor.get()); REQUIRE(synth);
    CHECK_NEAR(synth->getParameter(W::oscParam(0, W::Warp1Mode)), W::SelfPd, 0);
    CHECK_NEAR(synth->getParameter(W::oscParam(0, W::Warp1Amount)), 0.4, 1e-6);
    CHECK_NEAR(synth->getParameter(W::oscParam(0, W::Warp2Mode)), W::FmOsc1, 0);
    for (const auto& warning : result.warnings) CHECK(warning.find("kSelfPD") == std::string::npos);
}

TEST_CASE("Serum import: NoteOn random, alternate, discrete and fixed matrix sources map to native sources")
{
    using W = devices::WavetableSynth;
    const auto slot = [](double source, double auxiliary) {
        return map({{"destModuleID", number(0)}, {"destModuleParamID", number(6)}, {"destModuleParamName", text("kParamCoarsePit")},
                    {"destModuleTypeString", text("Oscillator")}, {"plainParams", map({{"kParamAmount", number(12)}})},
                    {"source", array({number(source), number(auxiliary)})}});
    };
    const auto result = plugins::importSerumPatch(container(document(map({{"ModSlot0", slot(59, 22)}, {"ModSlot1", slot(23, 24)}, {"ModSlot2", slot(21, 38)}}))));
    REQUIRE(result);
    auto processor = devices::createNative(result.device); auto* synth = dynamic_cast<W*>(processor.get()); REQUIRE(synth);
    CHECK_NEAR(synth->getParameter(W::modParam(0, W::ModSource)), W::NoteRandomDiscrete, 0);
    CHECK_NEAR(synth->getParameter(W::modParam(0, W::AuxSource)), W::NoteRandom2, 0);
    CHECK_NEAR(synth->getParameter(W::modParam(1, W::ModSource)), W::NoteAlternate, 0);
    CHECK_NEAR(synth->getParameter(W::modParam(1, W::AuxSource)), W::NoteAlternate2, 0);
    CHECK_NEAR(synth->getParameter(W::modParam(2, W::ModSource)), W::NoteRandom, 0);
    CHECK_NEAR(synth->getParameter(W::modParam(2, W::AuxSource)), W::Fixed, 0);
}

TEST_CASE("Serum import (DEV-18i): kParamAuxInverted inverts the aux after its curve")
{
    using W = devices::WavetableSynth;
    const auto slot = [](double inverted) {
        return map({{"destModuleID", number(0)}, {"destModuleParamID", number(3)}, {"destModuleParamName", text("kParamFreq")},
                    {"destModuleTypeString", text("VoiceFilter")}, {"source", array({number(3), number(32)})},
                    {"plainParams", map({{"kParamAmount", number(78.5)}, {"kParamAuxInverted", number(inverted)}})}});
    };
    const auto result = plugins::importSerumPatch(container(document(map({{"ModSlot0", slot(1)}, {"ModSlot1", slot(0)}}))));
    REQUIRE(result);
    auto processor = devices::createNative(result.device); auto* synth = dynamic_cast<W*>(processor.get()); REQUIRE(synth);
    CHECK_NEAR(synth->getParameter(W::modParam(0, W::AuxSource)), W::Macro8, 0);
    CHECK(synth->auxiliaryInverted(0)); CHECK(! synth->auxiliaryInverted(1));
    CHECK_NEAR(synth->modulationCurveAt(0, true, 0.25f), 0.75, 1e-6);
    CHECK_NEAR(synth->modulationCurveAt(1, true, 0.25f), 0.25, 1e-6);
}

TEST_CASE("Serum import (DEV-18i): matrix CurveIn bends the main source; filter keytrack and transpose routes import")
{
    using W = devices::WavetableSynth;
    const auto slot = [](double bend, bool customCurve) {
        const auto linear = map({{"curveVals", array({number(0.5), number(0.5)})}, {"numPoints", number(1)},
                                 {"xVals", array({number(0), number(1)})}, {"yVals", array({number(1), number(0)})}});
        return map({{"destModuleID", number(0)}, {"destModuleParamID", number(2)}, {"destModuleParamName", text("kParamVoiceAmp")},
                    {"destModuleTypeString", text("Global")}, {"source", array({number(25), number(0)})}, {"flex", array({linear, linear})},
                    {"plainParams", map({{"kParamAmount", number(100)}, {"kParamCurveIn", number(bend)}, {"kParamCurveOut", number(100)},
                                         {"kParamMainCurveData", number(customCurve ? 1 : 0)}})}});
    };
    const auto transpose = map({{"destModuleID", number(0)}, {"destModuleParamID", number(16)}, {"destModuleParamName", text("kParamTranspose")},
                                {"destModuleTypeString", text("Global")}, {"source", array({number(38), number(0)})},
                                {"plainParams", map({{"kParamAmount", number(-23.77)}})}});
    const auto result = plugins::importSerumPatch(container(document(map({{"ModSlot0", slot(50, false)}, {"ModSlot1", slot(-50, false)},
        {"ModSlot2", slot(50, true)}, {"ModSlot3", transpose},
        {"VoiceFilter0", map({{"plainParams", map({{"kParamEnable", number(1)}, {"kParamKeyTrack", number(1)}})}})}}))));
    REQUIRE(result);
    auto processor = devices::createNative(result.device); auto* synth = dynamic_cast<W*>(processor.get()); REQUIRE(synth);
    // Hosted Serum 2 (Macro 1 -> Voice Amp, stored 0): CurveIn 50 -> 0.4262 / 0.7504 / 0.9399, -50 -> 0.0033 / 0.0131 / 0.0634.
    const float x[]{0.1f, 0.25f, 0.5f};
    const double up[]{0.4262, 0.7504, 0.9399}, down[]{0.0033, 0.0131, 0.0634};
    for (int i = 0; i < 3; ++i)
    {
        CHECK_NEAR(synth->modulationCurveAt(0, false, x[i]), up[i], 0.01);
        CHECK_NEAR(synth->modulationCurveAt(1, false, x[i]), down[i], 0.002);
        CHECK_NEAR(synth->modulationCurveAt(2, false, x[i]), x[i], 1e-6); // a custom main curve wins over the bend
        CHECK_NEAR(synth->modulationCurveAt(0, true, x[i]), x[i], 1e-6);  // CurveOut does nothing
    }
    CHECK_NEAR(synth->getParameter(W::modParam(3, W::ModDestination)), W::GlobalTranspose, 0);
    CHECK_NEAR(synth->getParameter(W::filterParam(0, W::FilterKeytrack)), 1, 0);
    CHECK_NEAR(synth->getParameter(W::filterParam(1, W::FilterKeytrack)), 0, 0);
}

TEST_CASE("Serum import (DEV-18i): LFO rate, macro, tuning, voice amp, filter level/drive and noise/sub routes map to measured targets")
{
    using W = devices::WavetableSynth;
    const auto slot = [](int source, std::string_view module, int index, std::string_view name, int id) {
        return map({{"source", array({integer(static_cast<uint64_t>(source)), integer(0)})}, {"destModuleTypeString", text(module)},
                    {"destModuleID", integer(static_cast<uint64_t>(index))}, {"destModuleParamName", text(name)},
                    {"destModuleParamID", integer(static_cast<uint64_t>(id))}, {"plainParams", map({{"kParamAmount", number(25)}})}});
    };
    const auto result = plugins::importSerumPatch(container(document(map({
        {"ModSlot0", slot(25, "LFO", 3, "kParamRate", 0)}, {"ModSlot1", slot(25, "Macro", 2, "kParamValue", 0)},
        {"ModSlot2", slot(25, "Global", 0, "kParamMasterTuning", 1)}, {"ModSlot3", slot(25, "Global", 0, "kParamVoiceAmp", 2)},
        {"ModSlot4", slot(25, "VoiceFilter", 1, "kParamLevelOut", 8)}, {"ModSlot5", slot(25, "VoiceFilter", 0, "kParamDrive", 5)},
        {"ModSlot6", slot(25, "Oscillator", 3, "kParamVolume", 1)}, {"ModSlot7", slot(25, "Oscillator", 4, "kParamVolume", 1)},
        {"ModSlot8", slot(25, "Oscillator", 3, "kParamPan", 2)}, {"ModSlot9", slot(25, "Oscillator", 4, "kParamPan", 2)},
        {"ModSlot10", slot(25, "LFOPointModBus", 2, "kParamValue", 0)},
        {"ModSlot11", slot(25, "RoutingSlot", 1, "kParamFilterBalance", 3)}, {"ModSlot12", slot(25, "Oscillator", 2, "kParamPitch", 4)},
        {"ModSlot13", slot(25, "Oscillator", 4, "kParamCoarsePit", 6)}, {"ModSlot14", slot(25, "Oscillator", 0, "kParamDetuneWid", 27)},
        {"ModSlot15", slot(25, "Oscillator", 1, "kParamUnisonStereo", 28)}, {"ModSlot16", slot(25, "MultiSampleOsc", 2, "kParamWarp", 0)},
        {"lfoPointModAssignments", array({map({{"busID", integer(2)}, {"lfoID", integer(0)}, {"lfoType", integer(0)}, {"pointID", integer(1)}, {"target", integer(1)}})})},
        {"Global0", map({{"plainParams", map({{"kParamModWheel", number(40)}})}})}}))));
    REQUIRE(result);
    auto synth = devices::createNative(result.device); REQUIRE(synth);
    const int expected[]{W::Lfo4Rate, W::Macro3Value, W::MasterTuning, W::VoiceAmp, W::Filter2Level, W::Filter1Drive,
                         W::NoiseAmplitude, W::SubAmplitude, W::NoisePanDest, W::SubPanDest, W::LfoPointBus1 + 2,
                         W::Source1Balance + 1, W::Osc3Semi, W::SubCoarse, W::Osc1Blend, W::Osc2Width, W::Osc3Warp1};
    for (int i = 0; i < 17; ++i)
    {
        CHECK_NEAR(synth->getParameter(W::modParam(i, W::ModDestination)), expected[i], 0);
        CHECK_NEAR(synth->getParameter(W::modParam(i, W::ModAmount)), 0.25, 1e-6);
    }
    const auto ySource = plugins::importSerumPatch(container(document(map({{"ModSlot0", slot(40, "Oscillator", 0, "kParamVolume", 1)}}))));
    REQUIRE(ySource);
    auto ySynth = devices::createNative(ySource.device); REQUIRE(ySynth);
    CHECK_NEAR(ySynth->getParameter(W::modParam(0, W::ModSource)), W::Lfo2Y, 0); // vendor source 40 = "LFO 2 Y"
    CHECK(std::none_of(result.warnings.begin(), result.warnings.end(), [](const auto& w) { return w.find("modulation routes require") != std::string::npos; }));
}

TEST_CASE("Serum import (DEV-18i): preset formats up to 6 migrate FX Distortion Freq to 160/171 like the hosted load")
{
    const auto distortion = [](double version) {
        const auto fx = map({{"type", number(0)}, {"FXDistortion", map({{"plainParams", map({{"kParamFreq", number(0.7)}})}})}});
        const auto cbor = map({{"product", text("Serum2")}, {"fileType", text("SerumPreset")}, {"version", number(version)},
                               {"productVersion", text("2.0.12")}, {"FXRack0", map({{"FX", array({fx})}})}});
        const auto meta = "{\"product\":\"Serum2\",\"fileType\":\"SerumPreset\",\"version\":" + std::to_string(static_cast<int>(version))
                        + ",\"productVersion\":\"2.0.12\"}";
        const auto decoded = plugins::decodeSerumPreset(container(cbor, meta));
        REQUIRE(decoded);
        const auto effects = plugins::parseSerumEffects(decoded.state->root(), std::string{}, 48000);
        const auto& modules = effects.configuration.buses[0]; REQUIRE(modules.size() == 1);
        const auto descriptors = devices::WavetableEffects::parameterDescriptors(modules[0].type);
        for (size_t i = 0; i < descriptors.size(); ++i)
            if (std::string_view(descriptors[i].id) == "FilterFreq") return static_cast<double>(modules[0].values[i]);
        return -1.0;
    };
    CHECK_NEAR(distortion(4), 0.7 * 160 / 171, 1e-6);
    CHECK_NEAR(distortion(6), 0.7 * 160 / 171, 1e-6);
    CHECK_NEAR(distortion(7), 0.7, 1e-6);
}
