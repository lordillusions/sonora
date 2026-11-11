#include "plugin/StutterPlugin.h"

#include "plugin/StutterPluginEditor.h"
#include "plugins/SonoraPluginState.h"

#include <algorithm>
#include <cmath>

namespace sonora::plugin
{
StutterPlugin::StutterPlugin()
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
        const bool integral = info.unit == "st";
        auto parameter = std::make_unique<DeviceParameter>(*this, i, info, integral, logarithmic);
        hostParameters_.push_back(parameter.get());
        reported_[(size_t) i].store(device_.getParameter(i));
        addParameter(parameter.release());
    }
    startTimerHz(20);
}

StutterPlugin::~StutterPlugin()
{
    stopTimer();
}

bool StutterPlugin::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    const auto output = layouts.getMainOutputChannelSet();
    return (output == juce::AudioChannelSet::stereo() || output == juce::AudioChannelSet::mono())
        && layouts.getMainInputChannelSet() == output;
}

void StutterPlugin::setParameterValue(int index, float plainValue)
{
    if (index < 0 || index >= (int) hostParameters_.size()) return;
    device_.setParameter(index, plainValue);
    reported_[(size_t) index].store(device_.getParameter(index)); // the host knows this one already
}

void StutterPlugin::reportEditorChanges()
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

void StutterPlugin::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    sampleRate_ = sampleRate;
    device_.prepare(sampleRate, samplesPerBlock);
    stereo_.setSize(2, std::max(1, samplesPerBlock));
    setLatencySamples(0);
}

void StutterPlugin::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
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
    const int channels = std::max(1, std::min(buffer.getNumChannels(), getMainBusNumInputChannels()));
    for (int ch = 0; ch < 2; ++ch) stereo_.copyFrom(ch, 0, buffer, std::min(ch, channels - 1), 0, n);
    juce::AudioBuffer<float> io(stereo_.getArrayOfWritePointers(), 2, 0, n);
    device_.setHostInfo(hostInfo(getPlayHead(), sampleRate_));
    device_.process(io, midi);
    midi.clear(); // the notes only trigger the stutter
    if (buffer.getNumChannels() == 1)
    {
        buffer.copyFrom(0, 0, io, 0, 0, n);
        buffer.addFrom(0, 0, io, 1, 0, n);
        buffer.applyGain(0, 0, n, 0.5f);
    }
    else
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch) buffer.copyFrom(ch, 0, io, std::min(ch, 1), 0, n);
}

juce::AudioProcessorEditor* StutterPlugin::createEditor()
{
    return new StutterPluginEditor(*this);
}

void StutterPlugin::getStateInformation(juce::MemoryBlock& destination)
{
    const auto bytes = plugins::encodeStutterState(device_);
    destination.replaceAll(bytes.data(), bytes.size());
}

void StutterPlugin::setStateInformation(const void* data, int sizeInBytes)
{
    std::string error;
    if (sizeInBytes <= 0 || ! plugins::decodeStutterState(data, (size_t) sizeInBytes, device_, error))
        buildLog.push_back("Could not restore the saved state: " + error); // never fail a load
    for (size_t i = 0; i < hostParameters_.size(); ++i) reported_[i].store(device_.getParameter((int) i));
}
} // namespace sonora::plugin

#ifndef SONORA_PLUGIN_NO_ENTRY
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new sonora::plugin::StutterPlugin();
}
#endif
