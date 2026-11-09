#include "plugins/SonoraPluginState.h"

#include "devices/native/CurveLfo.h"
#include "devices/native/Kickstract.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <cmath>
#include <cstring>
#include <unordered_map>

namespace sonora::plugins
{
namespace
{
using L = devices::CurveLfo;
const juce::Identifier kStateTag{"SonoraCurveLfo"}, kParamTag{"Param"}, kId{"id"}, kValue{"value"}, kVersion{"version"};
constexpr int kStateVersion = 1;

constexpr uint32_t fourCC(const char (&code)[5])
{
    return (uint32_t) (uint8_t) code[0] << 24 | (uint32_t) (uint8_t) code[1] << 16 | (uint32_t) (uint8_t) code[2] << 8
           | (uint32_t) (uint8_t) code[3];
}
} // namespace

SonoraPlugin sonoraPluginFor(const model::PluginInfo& info, model::DeviceKind kind)
{
    // JUCE's component class id (juce_VST3Interface.h): two fixed words, then manufacturer and plug-in codes.
    if (kind != model::DeviceKind::PluginVst3 || info.vst3Uid[0] != 0xABCDEF01u || info.vst3Uid[1] != 0x9182FAEBu
        || info.vst3Uid[2] != fourCC("Snra"))
        return SonoraPlugin::None;
    switch (info.vst3Uid[3])
    {
        case fourCC("SnPr"):
        case fourCC("SnWt"): return SonoraPlugin::Prism; // the "Sonora Wavetable" build before UI-49
        case fourCC("SnCl"): return SonoraPlugin::CurveLfo;
        case fourCC("SnHs"): return SonoraPlugin::Hush;
        case fourCC("SnKk"): return SonoraPlugin::Kickstract;
        default: return SonoraPlugin::None;
    }
}

std::span<const uint8_t> juceVst3StateBody(std::span<const uint8_t> state)
{
    // JUCE's VST3 wrapper appends int64 0, the private data, its int64 size and "JUCEPrivateData" (getStateInformation).
    static constexpr char kMagic[] = "JUCEPrivateData";
    constexpr size_t magicSize = sizeof(kMagic) - 1;
    if (state.size() < magicSize + 2 * sizeof(uint64_t)
        || std::memcmp(state.data() + state.size() - magicSize, kMagic, magicSize) != 0)
        return state;
    uint64_t privateSize = 0;
    for (size_t i = 0; i < sizeof(uint64_t); ++i) // little-endian on every platform JUCE writes it from
        privateSize |= (uint64_t) state[state.size() - magicSize - sizeof(uint64_t) + i] << (8 * i);
    const uint64_t trailer = magicSize + 2 * sizeof(uint64_t);
    if (privateSize > state.size() - trailer) return state;
    return state.first((size_t) (state.size() - trailer - privateSize));
}

std::vector<uint8_t> encodeCurveLfoState(const devices::CurveLfo& lfo)
{
    juce::XmlElement root(kStateTag);
    root.setAttribute(kVersion, kStateVersion);
    const auto& infos = lfo.parameters();
    for (size_t i = 0; i < infos.size(); ++i)
    {
        auto* param = root.createNewChildElement(kParamTag);
        param->setAttribute(kId, juce::String::fromUTF8(infos[i].id.c_str()));
        param->setAttribute(kValue, (double) lfo.getParameter((int) i)); // shortest exact text of the float
    }
    juce::MemoryBlock block;
    juce::AudioProcessor::copyXmlToBinary(root, block);
    const auto* bytes = static_cast<const uint8_t*>(block.getData());
    return {bytes, bytes + block.getSize()};
}

bool decodeCurveLfoState(const void* data, size_t size, devices::CurveLfo& lfo, std::string& error)
{
    if (data == nullptr || size == 0) { error = "empty state"; return false; }
    const auto root = juce::AudioProcessor::getXmlFromBinary(data, (int) size);
    if (root == nullptr || ! root->hasTagName(kStateTag)) { error = "not a Sonora Curve LFO state"; return false; }
    if (root->getIntAttribute(kVersion) > kStateVersion)
    {
        error = "saved by a newer Sonora Curve LFO (state version " + root->getStringAttribute(kVersion).toStdString() + ")";
        return false;
    }
    const auto& infos = lfo.parameters();
    std::unordered_map<std::string, size_t> byId;
    for (size_t i = 0; i < infos.size(); ++i) byId.emplace(infos[i].id, i);
    std::vector<float> values(infos.size());
    for (size_t i = 0; i < infos.size(); ++i) values[i] = infos[i].defaultValue;
    for (const auto* param : root->getChildWithTagNameIterator(kParamTag))
    {
        const auto found = byId.find(param->getStringAttribute(kId).toStdString());
        if (found == byId.end()) continue;
        const double value = param->getDoubleAttribute(kValue, infos[found->second].defaultValue);
        if (std::isfinite(value)) values[found->second] = (float) value;
    }
    // Controls first, then each graph through setGraph, which publishes the point count after the points.
    for (int i = 0; i < L::GraphStart; ++i) lfo.setParameter(i, values[(size_t) i]);
    for (int g = 0; g < L::kGraphs; ++g)
    {
        dsp::CurveGraph graph;
        graph.count = (int) std::lround(values[(size_t) L::countParam(g)]);
        for (int p = 0; p < L::kMaxPoints; ++p)
        {
            graph.x[p] = values[(size_t) L::pointParam(g, p, L::X)];
            graph.y[p] = values[(size_t) L::pointParam(g, p, L::Y)];
            graph.curve[p] = values[(size_t) L::pointParam(g, p, L::Curve)];
        }
        lfo.setGraph(g, graph);
        lfo.setParameter(L::warpParam(g), values[(size_t) L::warpParam(g)]);
    }
    return true;
}

std::vector<uint8_t> encodeKickstractState(const devices::Kickstract& device)
{
    juce::XmlElement root("SonoraKickstract");
    root.setAttribute(kVersion, kStateVersion);
    const auto& infos = device.parameters();
    for (size_t i = 0; i < infos.size(); ++i)
    {
        auto* param = root.createNewChildElement(kParamTag);
        param->setAttribute(kId, juce::String::fromUTF8(infos[i].id.c_str()));
        param->setAttribute(kValue, (double) device.getParameter((int) i));
    }
    juce::MemoryBlock block;
    juce::AudioProcessor::copyXmlToBinary(root, block);
    const auto* bytes = static_cast<const uint8_t*>(block.getData());
    return {bytes, bytes + block.getSize()};
}

bool decodeKickstractState(const void* data, size_t size, devices::Kickstract& device, std::string& error)
{
    if (data == nullptr || size == 0) { error = "empty state"; return false; }
    const auto root = juce::AudioProcessor::getXmlFromBinary(data, (int) size);
    if (root == nullptr || ! root->hasTagName("SonoraKickstract")) { error = "not a Sonora Kickstract state"; return false; }
    if (root->getIntAttribute(kVersion) > kStateVersion)
    {
        error = "saved by a newer Sonora Kickstract (state version " + root->getStringAttribute(kVersion).toStdString() + ")";
        return false;
    }
    const auto& infos = device.parameters();
    std::unordered_map<std::string, size_t> byId;
    for (size_t i = 0; i < infos.size(); ++i) byId.emplace(infos[i].id, i);
    for (const auto* param : root->getChildWithTagNameIterator(kParamTag))
    {
        const auto found = byId.find(param->getStringAttribute(kId).toStdString());
        if (found == byId.end()) continue; // a newer version's control: skipped
        const double value = param->getDoubleAttribute(kValue, infos[found->second].defaultValue);
        if (std::isfinite(value)) device.setParameter((int) found->second, (float) value);
    }
    return true;
}
} // namespace sonora::plugins
