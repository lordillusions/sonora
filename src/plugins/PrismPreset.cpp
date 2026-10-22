#include "plugins/PrismPreset.h"

#include "devices/DeviceFactory.h"
#include "plugins/SerumPatch.h"
#include "plugins/Vst3PresetBuilder.h"

#include <juce_data_structures/juce_data_structures.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace sonora::plugins
{
namespace
{
constexpr char kMagic[4] = {'P', 'R', 'S', 'M'};
constexpr uint32_t kVersion = 1;
const juce::Identifier kState{"SonoraPrism"}, kName{"name"}, kEnabled{"enabled"}, kParameters{"Parameters"}, kPlain{"Plain"},
    kValue{"V"}, kId{"id"}, kNumber{"v"}, kBoolean{"bool"}, kContent{"Content"}, kFormat{"format"}, kDirectory{"directory"},
    kData{"data"}, kController{"controller"};

juce::MemoryBlock block(const std::vector<uint8_t>& bytes) { return {bytes.data(), bytes.size()}; }
std::vector<uint8_t> bytesOf(const juce::var& value)
{
    const auto* data = value.getBinaryData();
    if (data == nullptr) return {};
    const auto* first = static_cast<const uint8_t*>(data->getData());
    return {first, first + data->getSize()};
}
juce::ValueTree valueNode(const std::string& id, double number)
{
    juce::ValueTree value(kValue);
    value.setProperty(kId, juce::String::fromUTF8(id.c_str()), nullptr);
    value.setProperty(kNumber, number, nullptr);
    return value;
}
} // namespace

bool isPrismPreset(std::span<const uint8_t> bytes)
{
    return bytes.size() >= 8 && std::memcmp(bytes.data(), kMagic, 4) == 0;
}

std::vector<uint8_t> encodePrismPreset(const model::Device& device)
{
    juce::ValueTree state(kState);
    state.setProperty(kName, juce::String::fromUTF8(device.displayName.c_str()), nullptr);
    state.setProperty(kEnabled, device.enabled, nullptr);
    juce::ValueTree parameters(kParameters), plain(kPlain);
    for (const auto& parameter : device.parameters)
    {
        auto value = valueNode(parameter.path, parameter.value);
        if (parameter.booleanValue) value.setProperty(kBoolean, true, nullptr);
        parameters.appendChild(value, nullptr);
    }
    for (const auto& [id, number] : device.plain) plain.appendChild(valueNode(id, number), nullptr);
    state.appendChild(parameters, nullptr);
    state.appendChild(plain, nullptr);
    if (device.nativeContent)
    {
        const auto& content = *device.nativeContent;
        juce::ValueTree node(kContent);
        node.setProperty(kFormat, juce::String::fromUTF8(content.format.c_str()), nullptr);
        node.setProperty(kDirectory, juce::String::fromUTF8(content.sourceDirectory.c_str()), nullptr);
        node.setProperty(kData, block(content.data), nullptr);
        node.setProperty(kController, block(content.controllerData), nullptr);
        state.appendChild(node, nullptr);
    }
    juce::MemoryOutputStream stream;
    stream.write(kMagic, 4);
    stream.writeInt((int) kVersion);
    state.writeToStream(stream);
    const auto* first = static_cast<const uint8_t*>(stream.getData());
    return {first, first + stream.getDataSize()};
}

bool decodePrismPreset(std::span<const uint8_t> bytes, model::Device& device, std::string& error)
{
    if (! isPrismPreset(bytes)) { error = "not a Prism preset"; return false; }
    if (bytes.size() > 2 * model::NativeContent::maxBytes) { error = "preset too large"; return false; }
    juce::MemoryInputStream stream(bytes.data(), bytes.size(), false);
    stream.skipNextBytes(4);
    if ((uint32_t) stream.readInt() > kVersion) { error = "saved by a newer Sonora Prism"; return false; }
    const auto state = juce::ValueTree::readFromStream(stream);
    if (! state.hasType(kState)) { error = "damaged Prism preset"; return false; }
    auto result = devices::nativeDeviceModel("SonoraWavetable");
    result.id = device.id;
    if (const auto name = state.getProperty(kName).toString(); name.isNotEmpty()) result.displayName = name.toStdString();
    result.enabled = state.getProperty(kEnabled, true);
    for (const auto value : state.getChildWithName(kParameters))
    {
        const auto id = value.getProperty(kId).toString().toStdString();
        const double number = value.getProperty(kNumber, 0.0);
        if (! std::isfinite(number)) continue;
        const auto found = std::find_if(result.parameters.begin(), result.parameters.end(), [&](const auto& p) { return p.path == id; });
        if (found != result.parameters.end()) { found->value = number; continue; }
        model::Parameter parameter;
        parameter.path = id;
        parameter.value = number;
        parameter.booleanValue = value.getProperty(kBoolean, false);
        result.parameters.push_back(std::move(parameter));
    }
    for (const auto value : state.getChildWithName(kPlain))
        if (const double number = value.getProperty(kNumber, 0.0); std::isfinite(number))
            result.plain[value.getProperty(kId).toString().toStdString()] = number;
    if (const auto node = state.getChildWithName(kContent); node.isValid())
    {
        auto content = std::make_shared<model::NativeContent>();
        content->format = node.getProperty(kFormat).toString().toStdString();
        content->sourceDirectory = node.getProperty(kDirectory).toString().toStdString();
        content->data = bytesOf(node.getProperty(kData));
        content->controllerData = bytesOf(node.getProperty(kController));
        if (content->data.size() > model::NativeContent::maxBytes || content->controllerData.size() > model::NativeContent::maxBytes)
        { error = "preset content over its size limit"; return false; }
        result.nativeContent = std::move(content);
    }
    device = std::move(result);
    return true;
}

PrismPresetLoad loadPrismPreset(const juce::File& file)
{
    PrismPresetLoad result;
    if (! file.existsAsFile() || file.getSize() <= 0 || file.getSize() > static_cast<int64_t>(model::NativeContent::maxBytes))
    { result.error = "invalid file or size"; return result; }
    juce::MemoryBlock data;
    if (! file.loadFileAsData(data) || data.getSize() > model::NativeContent::maxBytes) { result.error = "could not read the file"; return result; }
    const auto* first = static_cast<const uint8_t*>(data.getData());
    std::vector<uint8_t> bytes(first, first + data.getSize()), controller;
    if (isPrismPreset(bytes))
    {
        if (! decodePrismPreset(bytes, result.device, result.error)) return result;
    }
    else
    {
        if (bytes.size() >= 4 && std::memcmp(first, "VST3", 4) == 0)
        {
            model::PluginInfo state;
            if (! readVstPreset(bytes, state) || vst3ClassIdString(state.vst3Uid) != "56534558667350736572756D20320000")
            { result.error = "not a preset this instrument can open"; return result; }
            bytes = std::move(state.state);
            controller = std::move(state.controllerState);
        }
        auto imported = importSerumPatch(bytes, file.getParentDirectory().getFullPathName().toStdString(), controller);
        if (! imported) { result.error = imported.error; return result; }
        result.device = std::move(imported.device);
        result.warnings = std::move(imported.warnings);
    }
    result.device.displayName = file.getFileNameWithoutExtension().toStdString();
    return result;
}

bool savePrismPreset(const model::Device& device, const juce::File& file, std::string& error)
{
    if (device.type != "SonoraWavetable") { error = "not a Sonora Prism"; return false; }
    if (! file.getParentDirectory().createDirectory()) { error = "could not create " + file.getParentDirectory().getFullPathName().toStdString(); return false; }
    const auto bytes = encodePrismPreset(device);
    if (! file.replaceWithData(bytes.data(), bytes.size())) { error = "could not write " + file.getFullPathName().toStdString(); return false; }
    return true;
}

juce::File prismLibraryRoot()
{
    return juce::File::getSpecialLocation(juce::File::userMusicDirectory).getChildFile("Sonora").getChildFile("Prism");
}
} // namespace sonora::plugins
