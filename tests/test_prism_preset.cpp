// UI-50: Sonora Prism's own .prism presets (plugins/PrismPreset).
#include "TestFramework.h"
#include "devices/DeviceFactory.h"
#include "devices/native/WavetableContent.h"
#include "devices/native/WavetableData.h"
#include "devices/native/WavetableSynth.h"
#include "plugins/PrismPreset.h"

#include <juce_core/juce_core.h>
#include <cmath>

using namespace sonora;
namespace
{
using W = devices::WavetableSynth;

std::shared_ptr<const devices::WavetableData> ownTable()
{
    std::vector<float> samples(256 * 2);
    for (int frame = 0; frame < 2; ++frame)
        for (int i = 0; i < 256; ++i)
            samples[(size_t) (frame * 256 + i)] = (float) std::sin(6.283185307179586 * i * (frame + 1) / 256);
    auto result = devices::WavetableData::fromSamples(samples, 1, 256, 48000, "Preset test table");
    REQUIRE(result);
    return result.data;
}

model::Device authored()
{
    auto device = devices::nativeDeviceModel("SonoraWavetable");
    device.id = 42;
    device.displayName = "Glass Lead";
    std::string error;
    REQUIRE(devices::storeWavetableTable(device, 1, *ownTable(), error));
    for (auto& parameter : device.parameters)
        if (parameter.path == "Volume") parameter.value = -7.5;
    device.plain["NoiseSeed"] = 3;
    return device;
}

double parameter(const model::Device& device, const std::string& path)
{
    for (const auto& p : device.parameters)
        if (p.path == path) return p.value;
    return std::nan("");
}
} // namespace

TEST_CASE("prism preset: encode and decode keep controls, plain values, content and name; the id stays the target's")
{
    const auto source = authored();
    const auto bytes = plugins::encodePrismPreset(source);
    CHECK(plugins::isPrismPreset(bytes));
    model::Device restored;
    restored.id = 7;
    std::string error;
    REQUIRE(plugins::decodePrismPreset(bytes, restored, error));
    CHECK(restored.id == 7);
    CHECK(restored.type == "SonoraWavetable");
    CHECK(restored.displayName == "Glass Lead");
    CHECK_NEAR(parameter(restored, "Volume"), -7.5, 1e-9);
    CHECK_NEAR(restored.plain["NoiseSeed"], 3, 1e-9);
    REQUIRE(restored.nativeContent != nullptr);
    CHECK(restored.nativeContent->data == source.nativeContent->data);
    auto synth = devices::createNative(restored);
    auto* prism = dynamic_cast<W*>(synth.get());
    REQUIRE(prism != nullptr);
    CHECK(prism->customTableName(1) == "Preset test table");
}

TEST_CASE("prism preset: files save and open under the file's name; damaged, foreign and newer data are refused")
{
    auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("sonora_prism_preset_test");
    dir.deleteRecursively();
    const auto file = dir.getChildFile("Bank/Lead/My Lead.prism");
    std::string error;
    REQUIRE(plugins::savePrismPreset(authored(), file, error)); // creates the folders
    const auto loaded = plugins::loadPrismPreset(file);
    REQUIRE(loaded);
    CHECK(loaded.device.displayName == "My Lead");
    CHECK_NEAR(parameter(loaded.device, "Volume"), -7.5, 1e-9);
    CHECK(loaded.warnings.empty());

    model::Device other;
    CHECK(! plugins::savePrismPreset(devices::nativeDeviceModel("Delay"), dir.getChildFile("x.prism"), error));
    const uint8_t junk[] = {'P', 'R', 'S', 'M', 1, 0, 0, 0, 9, 9, 9};
    CHECK(! plugins::decodePrismPreset(junk, other, error));
    const uint8_t newer[] = {'P', 'R', 'S', 'M', 99, 0, 0, 0, 0, 0, 0, 0};
    CHECK(! plugins::decodePrismPreset(newer, other, error));
    CHECK(error.find("newer") != std::string::npos);
    const auto text = dir.getChildFile("notes.prism");
    REQUIRE(text.replaceWithText("not a preset"));
    CHECK(! plugins::loadPrismPreset(text));
    CHECK(! plugins::loadPrismPreset(dir.getChildFile("missing.prism")));
    dir.deleteRecursively();
}
