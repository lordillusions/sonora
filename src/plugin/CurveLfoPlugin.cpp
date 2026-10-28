#include "plugin/CurveLfoPlugin.h"

#include "plugin/CurveLfoPluginEditor.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace sonora::plugin
{
namespace
{
using L = devices::CurveLfo;
const juce::Identifier kStateTag{"SonoraCurveLfo"}, kParamTag{"Param"}, kId{"id"}, kValue{"value"}, kVersion{"version"};
constexpr int kStateVersion = 1;

bool isNote(const juce::MidiMessageMetadata& event)
{
    const int status = event.numBytes >= 3 ? event.data[0] & 0xF0 : 0;
    return status == 0x80 || status == 0x90;
}
} // namespace

CurveLfoPlugin::CurveLfoPlugin()
    : AudioProcessor(BusesProperties()
                         .withInput("Input", juce::AudioChannelSet::stereo(), true)
                         .withOutput("Output", juce::AudioChannelSet::stereo(), true))
{
    lfo_.setBypassed(false); // the host has its own bypass
    const auto& infos = lfo_.parameters();
    reported_ = std::make_unique<std::atomic<float>[]>((size_t) kHostParameters);
    for (int i = 0; i < kHostParameters; ++i)
    {
        const auto& info = infos[(size_t) i];
        // Numeric controls without a unit (CC Out, CC Depth) take whole numbers, as on the device face.
        const bool integral = info.unit.empty() || info.unit == "int";
        auto parameter = std::make_unique<DeviceParameter>(*this, i, info, integral);
        hostParameters_.push_back(parameter.get());
        reported_[(size_t) i].store(lfo_.getParameter(i));
        addParameter(parameter.release());
    }
    for (size_t i = kHostParameters; i < infos.size(); ++i) graphValues_.push_back(lfo_.getParameter((int) i));
    startTimerHz(20);
}

CurveLfoPlugin::~CurveLfoPlugin()
{
    stopTimer();
}

bool CurveLfoPlugin::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    const auto output = layouts.getMainOutputChannelSet();
    return (output == juce::AudioChannelSet::stereo() || output == juce::AudioChannelSet::mono())
        && layouts.getMainInputChannelSet() == output;
}

float CurveLfoPlugin::parameterValue(int index) const
{
    return lfo_.getParameter(index);
}

void CurveLfoPlugin::setParameterValue(int index, float plainValue)
{
    if (index < 0 || index >= kHostParameters) return;
    lfo_.setParameter(index, plainValue);
    reported_[(size_t) index].store(lfo_.getParameter(index)); // the host knows this one already
}

void CurveLfoPlugin::timerCallback()
{
    reportEditorChanges();
}

void CurveLfoPlugin::reportEditorChanges()
{
    // The host records automation and marks its project changed from these.
    for (size_t i = 0; i < hostParameters_.size(); ++i)
    {
        const float value = lfo_.getParameter((int) i);
        if (std::abs(value - reported_[i].load()) <= 0.0f) continue; // exact: any change, however small
        reported_[i].store(value);
        auto* parameter = hostParameters_[i];
        parameter->beginChangeGesture();
        parameter->sendValueChangedMessageToListeners(parameter->getValue());
        parameter->endChangeGesture();
    }
    bool graphsChanged = false;
    for (size_t i = 0; i < graphValues_.size(); ++i)
    {
        const float value = lfo_.getParameter((int) (kHostParameters + i));
        if (std::abs(value - graphValues_[i]) <= 0.0f) continue;
        graphValues_[i] = value;
        graphsChanged = true;
    }
    if (graphsChanged) updateHostDisplay(ChangeDetails().withNonParameterStateChanged(true));
}

void CurveLfoPlugin::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    sampleRate_ = sampleRate > 0 ? sampleRate : 44100.0;
    blockSize_ = std::max(1, samplesPerBlock);
    chunkMidi_.ensureSize(16 * 1024);
    outputMidi_.ensureSize(16 * 1024);
    lfo_.prepare(sampleRate_, blockSize_); // processing is stopped here
}

void CurveLfoPlugin::releaseResources()
{
}

void CurveLfoPlugin::renderChunk(juce::AudioBuffer<float>& audio, const juce::MidiBuffer& input, int start, int length,
                                 const devices::HostInfo& info)
{
    // The device reads note on/off and appends its CC; feeding it only notes means every controller it hands back is
    // its own output.
    chunkMidi_.clear();
    for (const auto event : input)
        if (event.samplePosition >= start && event.samplePosition < start + length && isNote(event))
            chunkMidi_.addEvent(event.data, event.numBytes, event.samplePosition - start);
    juce::AudioBuffer<float> part(audio.getArrayOfWritePointers(), audio.getNumChannels(), start, length);
    lfo_.setHostInfo(info);
    lfo_.process(part, chunkMidi_);
    for (const auto event : chunkMidi_)
        if (! isNote(event)) outputMidi_.addEvent(event.data, event.numBytes, event.samplePosition + start);
}

void CurveLfoPlugin::processBlock(juce::AudioBuffer<float>& audio, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    const int total = audio.getNumSamples();
    for (int channel = getTotalNumInputChannels(); channel < audio.getNumChannels(); ++channel) audio.clear(channel, 0, total);
    outputMidi_.clear();
    auto info = hostInfo(getPlayHead(), sampleRate_);
    // A host may send more than it announced: run the device in blocks it was prepared for.
    for (int start = 0; start < total; start += blockSize_)
    {
        const int length = std::min(blockSize_, total - start);
        renderChunk(audio, midi, start, length, info);
        info.beats += length / sampleRate_ * info.tempo / 60.0;
    }
    midi.clear();
    for (const auto event : outputMidi_) midi.addEvent(event.data, event.numBytes, event.samplePosition);
}

juce::AudioProcessorEditor* CurveLfoPlugin::createEditor()
{
    return new CurveLfoPluginEditor(*this);
}

juce::MemoryBlock CurveLfoPlugin::encodeState(const devices::CurveLfo& lfo)
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
    copyXmlToBinary(root, block);
    return block;
}

bool CurveLfoPlugin::decodeState(const void* data, size_t size, devices::CurveLfo& lfo, std::string& error)
{
    if (data == nullptr || size == 0) { error = "empty state"; return false; }
    const auto root = getXmlFromBinary(data, (int) size);
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

void CurveLfoPlugin::getStateInformation(juce::MemoryBlock& destination)
{
    destination = encodeState(lfo_);
}

void CurveLfoPlugin::setStateInformation(const void* data, int sizeInBytes)
{
    std::string error;
    if (sizeInBytes <= 0 || ! decodeState(data, (size_t) sizeInBytes, lfo_, error))
        buildLog.push_back("Could not restore the saved state: " + error); // never fail a load: keep the current curves
}

} // namespace sonora::plugin

#ifndef SONORA_PLUGIN_NO_ENTRY // sonora_tests links both plug-ins' processors: only one may define the entry point
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new sonora::plugin::CurveLfoPlugin();
}
#endif
