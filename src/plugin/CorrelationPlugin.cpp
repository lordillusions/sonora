#include "plugin/CorrelationPlugin.h"

#include "plugin/CorrelationPluginEditor.h"
#include "plugins/SonoraPluginState.h"

#include <algorithm>
#include <cmath>

namespace sonora::plugin
{
CorrelationPlugin::CorrelationPlugin()
    : AudioProcessor(BusesProperties()
                         .withInput("Input", juce::AudioChannelSet::stereo(), true)
                         .withOutput("Output", juce::AudioChannelSet::stereo(), true))
{
    device_.setBypassed(false); // the host has its own bypass
    const auto& infos = device_.parameters();
    reported_ = std::make_unique<std::atomic<float>[]>(infos.size());
    for (int i = 0; i < (int) infos.size(); ++i)
    {
        const auto& info = infos[(size_t) i];
        const bool logarithmic = info.unit == "ms";
        auto parameter = std::make_unique<DeviceParameter>(*this, i, info, false, logarithmic);
        hostParameters_.push_back(parameter.get());
        reported_[(size_t) i].store(device_.getParameter(i));
        addParameter(parameter.release());
    }
    startTimerHz(20);
}

CorrelationPlugin::~CorrelationPlugin()
{
    stopTimer();
}

bool CorrelationPlugin::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    const auto output = layouts.getMainOutputChannelSet();
    if (output != juce::AudioChannelSet::stereo() && output != juce::AudioChannelSet::mono()) return false;
    return layouts.getMainInputChannelSet() == output;
}

void CorrelationPlugin::setParameterValue(int index, float plainValue)
{
    if (index < 0 || index >= (int) hostParameters_.size()) return;
    device_.setParameter(index, plainValue);
    reported_[(size_t) index].store(device_.getParameter(index)); // the host knows this one already
}

void CorrelationPlugin::reportEditorChanges()
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

void CorrelationPlugin::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    device_.prepare(sampleRate, samplesPerBlock);
    stereo_.setSize(2, std::max(1, samplesPerBlock));
    setLatencySamples(0);
}

void CorrelationPlugin::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
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
    auto main = getBusBuffer(buffer, true, 0);
    const int channels = std::max(1, std::min(main.getNumChannels(), getMainBusNumInputChannels()));
    if (channels >= 2)
    {
        // Stereo: measured and (Listen = Stereo) passed in place, untouched.
        juce::AudioBuffer<float> io(main.getArrayOfWritePointers(), 2, 0, n);
        device_.process(io, midi);
        return;
    }
    for (int ch = 0; ch < 2; ++ch) stereo_.copyFrom(ch, 0, main, 0, 0, n);
    juce::AudioBuffer<float> io(stereo_.getArrayOfWritePointers(), 2, 0, n);
    device_.process(io, midi);
    auto out = getBusBuffer(buffer, false, 0);
    out.copyFrom(0, 0, io, 0, 0, n);
    out.addFrom(0, 0, io, 1, 0, n);
    out.applyGain(0, 0, n, 0.5f);
}

juce::AudioProcessorEditor* CorrelationPlugin::createEditor()
{
    return new CorrelationPluginEditor(*this);
}

void CorrelationPlugin::getStateInformation(juce::MemoryBlock& destination)
{
    const auto bytes = plugins::encodeParameterState(device_, "SonoraCorrelation");
    destination.replaceAll(bytes.data(), bytes.size());
}

void CorrelationPlugin::setStateInformation(const void* data, int sizeInBytes)
{
    std::string error;
    if (sizeInBytes <= 0 || ! plugins::decodeParameterState(data, (size_t) sizeInBytes, device_, "SonoraCorrelation", "Sonora Correlation", error))
        buildLog.push_back("Could not restore the saved state: " + error); // never fail a load
    for (size_t i = 0; i < hostParameters_.size(); ++i) reported_[i].store(device_.getParameter((int) i));
}
} // namespace sonora::plugin

#ifndef SONORA_PLUGIN_NO_ENTRY
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new sonora::plugin::CorrelationPlugin();
}
#endif
