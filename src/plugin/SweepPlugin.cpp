#include "plugin/SweepPlugin.h"

#include "plugin/SweepPluginEditor.h"
#include "plugins/SonoraPluginState.h"

#include <algorithm>
#include <cmath>

namespace sonora::plugin
{
SweepPlugin::SweepPlugin()
    : AudioProcessor(BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true))
{
    device_.setBypassed(false); // the host has its own bypass
    const auto& infos = device_.parameters();
    reported_ = std::make_unique<std::atomic<float>[]>(infos.size());
    for (int i = 0; i < (int) infos.size(); ++i)
    {
        const auto& info = infos[(size_t) i];
        const bool logarithmic = info.unit == "Hz";
        const bool integral = info.unit == "st";
        auto parameter = std::make_unique<DeviceParameter>(*this, i, info, integral, logarithmic);
        hostParameters_.push_back(parameter.get());
        reported_[(size_t) i].store(device_.getParameter(i));
        addParameter(parameter.release());
    }
    startTimerHz(20);
}

SweepPlugin::~SweepPlugin()
{
    stopTimer();
}

bool SweepPlugin::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    const auto output = layouts.getMainOutputChannelSet();
    return output == juce::AudioChannelSet::stereo() || output == juce::AudioChannelSet::mono();
}

void SweepPlugin::setParameterValue(int index, float plainValue)
{
    if (index < 0 || index >= (int) hostParameters_.size()) return;
    device_.setParameter(index, plainValue);
    reported_[(size_t) index].store(device_.getParameter(index)); // the host knows this one already
}

void SweepPlugin::reportEditorChanges()
{
    for (size_t i = 0; i < hostParameters_.size(); ++i)
    {
        const float value = device_.getParameter((int) i);
        if (std::abs(value - reported_[i].load()) <= 0.0f) continue;
        reported_[i].store(value);
        auto* parameter = hostParameters_[i];
        parameter->beginChangeGesture();
        parameter->sendValueChangedMessageToListeners(parameter->getValue());
        parameter->endChangeGesture();
    }
}

void SweepPlugin::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    sampleRate_ = sampleRate;
    device_.prepare(sampleRate, samplesPerBlock);
    stereo_.setSize(2, std::max(1, samplesPerBlock));
    setLatencySamples(0);
}

void SweepPlugin::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples();
    if (n > stereo_.getNumSamples()) // larger than prepared: run in chunks
    {
        for (int start = 0; start < n; start += stereo_.getNumSamples())
        {
            const int len = std::min(stereo_.getNumSamples(), n - start);
            juce::AudioBuffer<float> view(buffer.getArrayOfWritePointers(), buffer.getNumChannels(), start, len);
            processBlock(view, midi);
        }
        return;
    }
    stereo_.clear(0, n);
    juce::AudioBuffer<float> io(stereo_.getArrayOfWritePointers(), 2, 0, n);
    device_.setHostInfo(hostInfo(getPlayHead(), sampleRate_));
    device_.process(io, midi);
    if (buffer.getNumChannels() == 1)
    {
        buffer.copyFrom(0, 0, io, 0, 0, n);
        buffer.addFrom(0, 0, io, 1, 0, n);
        buffer.applyGain(0, 0, n, 0.5f);
    }
    else
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch) buffer.copyFrom(ch, 0, io, std::min(ch, 1), 0, n);
}

juce::AudioProcessorEditor* SweepPlugin::createEditor()
{
    return new SweepPluginEditor(*this);
}

bool SweepPlugin::loadSample(const juce::File& file)
{
    std::string error;
    if (! device_.loadSample(file.getFullPathName().toStdString(), error))
    {
        buildLog.push_back("Could not load the sample: " + error);
        return false;
    }
    device_.setParameter(devices::Sweep::Source, (float) devices::Sweep::Sample);
    reportEditorChanges();
    updateHostDisplay(ChangeDetails().withNonParameterStateChanged(true));
    return true;
}

void SweepPlugin::getStateInformation(juce::MemoryBlock& destination)
{
    const auto bytes = plugins::encodeParameterState(device_, "SonoraSweep", {{"samplePath", device_.samplePath()}});
    destination.replaceAll(bytes.data(), bytes.size());
}

void SweepPlugin::setStateInformation(const void* data, int sizeInBytes)
{
    std::string error;
    std::map<std::string, std::string> attributes;
    if (sizeInBytes <= 0 || ! plugins::decodeParameterState(data, (size_t) sizeInBytes, device_, "SonoraSweep", "Sonora Sweep", error, &attributes))
        buildLog.push_back("Could not restore the saved state: " + error); // never fail a load
    if (const auto path = attributes.find("samplePath"); path != attributes.end() && ! path->second.empty())
        if (std::string loadError; ! device_.loadSample(path->second, loadError)) buildLog.push_back("Sample missing: " + loadError);
    for (size_t i = 0; i < hostParameters_.size(); ++i) reported_[i].store(device_.getParameter((int) i));
}
} // namespace sonora::plugin

#ifndef SONORA_PLUGIN_NO_ENTRY
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new sonora::plugin::SweepPlugin();
}
#endif
