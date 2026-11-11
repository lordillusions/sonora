#include "plugin/PhaseAlignPlugin.h"

#include "plugin/PhaseAlignPluginEditor.h"
#include "plugins/SonoraPluginState.h"

#include <algorithm>
#include <cmath>

namespace sonora::plugin
{
PhaseAlignPlugin::PhaseAlignPlugin()
    : AudioProcessor(BusesProperties()
                         .withInput("Input", juce::AudioChannelSet::stereo(), true)
                         .withOutput("Output", juce::AudioChannelSet::stereo(), true)
                         .withInput("Sidechain", juce::AudioChannelSet::stereo(), true))
{
    device_.setBypassed(false); // the host has its own bypass
    const auto& infos = device_.parameters();
    reported_ = std::make_unique<std::atomic<float>[]>(infos.size());
    for (int i = 0; i < (int) infos.size(); ++i)
    {
        const auto& info = infos[(size_t) i];
        const bool logarithmic = info.unit == "ms" || info.unit == "Hz";
        const bool integral = false;
        auto parameter = std::make_unique<DeviceParameter>(*this, i, info, integral, logarithmic);
        hostParameters_.push_back(parameter.get());
        reported_[(size_t) i].store(device_.getParameter(i));
        addParameter(parameter.release());
    }
    startTimerHz(20);
}

PhaseAlignPlugin::~PhaseAlignPlugin()
{
    stopTimer();
}

bool PhaseAlignPlugin::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    const auto output = layouts.getMainOutputChannelSet();
    if (output != juce::AudioChannelSet::stereo() && output != juce::AudioChannelSet::mono()) return false;
    if (layouts.getMainInputChannelSet() != output) return false;
    if (layouts.inputBuses.size() > 1)
    {
        const auto sc = layouts.getChannelSet(true, 1);
        return sc.isDisabled() || sc == juce::AudioChannelSet::stereo() || sc == juce::AudioChannelSet::mono();
    }
    return true;
}

void PhaseAlignPlugin::setParameterValue(int index, float plainValue)
{
    if (index < 0 || index >= (int) hostParameters_.size()) return;
    device_.setParameter(index, plainValue);
    reported_[(size_t) index].store(device_.getParameter(index)); // the host knows this one already
}

void PhaseAlignPlugin::reportEditorChanges()
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

void PhaseAlignPlugin::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    sampleRate_ = sampleRate;
    device_.prepare(sampleRate, samplesPerBlock);
    stereo_.setSize(2, std::max(1, samplesPerBlock));
    sidechain_.setSize(2, std::max(1, samplesPerBlock));
    setLatencySamples(0);
}

void PhaseAlignPlugin::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
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
    for (int ch = 0; ch < 2; ++ch) stereo_.copyFrom(ch, 0, main, std::min(ch, channels - 1), 0, n);
    juce::AudioBuffer<float> io(stereo_.getArrayOfWritePointers(), 2, 0, n);
    const auto* bus = getBus(true, 1);
    if (bus != nullptr && bus->isEnabled() && bus->getNumberOfChannels() > 0)
    {
        auto sc = getBusBuffer(buffer, true, 1);
        for (int ch = 0; ch < 2; ++ch) sidechain_.copyFrom(ch, 0, sc, std::min(ch, sc.getNumChannels() - 1), 0, n);
        juce::AudioBuffer<float> key(sidechain_.getArrayOfWritePointers(), 2, 0, n);
        device_.setSidechain(&key, 1.0f);
    }
    else device_.setSidechain(nullptr, 1.0f);
    device_.process(io, midi);
    device_.setSidechain(nullptr, 1.0f);
    auto out = getBusBuffer(buffer, false, 0);
    if (out.getNumChannels() == 1)
    {
        out.copyFrom(0, 0, io, 0, 0, n);
        out.addFrom(0, 0, io, 1, 0, n);
        out.applyGain(0, 0, n, 0.5f);
    }
    else
        for (int ch = 0; ch < out.getNumChannels(); ++ch) out.copyFrom(ch, 0, io, std::min(ch, 1), 0, n);
}

juce::AudioProcessorEditor* PhaseAlignPlugin::createEditor()
{
    return new PhaseAlignPluginEditor(*this);
}

void PhaseAlignPlugin::getStateInformation(juce::MemoryBlock& destination)
{
    const auto bytes = plugins::encodeParameterState(device_, "SonoraPhaseAlign");
    destination.replaceAll(bytes.data(), bytes.size());
}

void PhaseAlignPlugin::setStateInformation(const void* data, int sizeInBytes)
{
    std::string error;
    if (sizeInBytes <= 0 || ! plugins::decodeParameterState(data, (size_t) sizeInBytes, device_, "SonoraPhaseAlign", "Sonora Phase Align", error))
        buildLog.push_back("Could not restore the saved state: " + error); // never fail a load
    for (size_t i = 0; i < hostParameters_.size(); ++i) reported_[i].store(device_.getParameter((int) i));
}
} // namespace sonora::plugin

#ifndef SONORA_PLUGIN_NO_ENTRY
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new sonora::plugin::PhaseAlignPlugin();
}
#endif
