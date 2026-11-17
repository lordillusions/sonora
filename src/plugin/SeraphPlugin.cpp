#include "plugin/SeraphPlugin.h"

#include "plugin/SeraphPluginEditor.h"
#include "plugins/SonoraPluginState.h"

#include <algorithm>
#include <cmath>

namespace sonora::plugin
{
SeraphPlugin::SeraphPlugin()
    : AudioProcessor(BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true))
{
    device_.setBypassed(false); // the host has its own bypass
    const auto& infos = device_.parameters();
    reported_ = std::make_unique<std::atomic<float>[]>(infos.size());
    for (int i = 0; i < (int) infos.size(); ++i)
    {
        const auto& info = infos[(size_t) i];
        const bool logarithmic = info.unit == "Hz" || (info.unit == "ms" && info.minValue > 0);
        const bool integral = info.unit == "int";
        auto parameter = std::make_unique<DeviceParameter>(*this, i, info, integral, logarithmic);
        hostParameters_.push_back(parameter.get());
        reported_[(size_t) i].store(device_.getParameter(i));
        addParameter(parameter.release());
    }
    reportedContent_ = device_.encodeContent();
    startTimerHz(20);
}

SeraphPlugin::~SeraphPlugin()
{
    stopTimer();
}

bool SeraphPlugin::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    const auto output = layouts.getMainOutputChannelSet();
    return output == juce::AudioChannelSet::stereo() || output == juce::AudioChannelSet::mono();
}

void SeraphPlugin::setParameterValue(int index, float plainValue)
{
    if (index < 0 || index >= (int) hostParameters_.size()) return;
    device_.setParameter(index, plainValue);
    reported_[(size_t) index].store(device_.getParameter(index)); // the host knows this one already
}

void SeraphPlugin::reportEditorChanges()
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
    if (auto content = device_.encodeContent(); content != reportedContent_)
    {
        reportedContent_ = std::move(content);
        updateHostDisplay(ChangeDetails().withNonParameterStateChanged(true).withProgramChanged(true));
    }
}

void SeraphPlugin::setCurrentProgram(int index)
{
    if (index < 0 || index >= getNumPrograms()) return;
    device_.applyPreset(index);
    reportEditorChanges();
}

const juce::String SeraphPlugin::getProgramName(int index)
{
    const auto& list = devices::Seraph::presets();
    return index >= 0 && (size_t) index < list.size() ? juce::String::fromUTF8(list[(size_t) index].name) : juce::String();
}

void SeraphPlugin::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    sampleRate_ = sampleRate;
    device_.prepare(sampleRate, samplesPerBlock);
    stereo_.setSize(2, std::max(1, samplesPerBlock));
    setLatencySamples(0);
}

void SeraphPlugin::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples();
    if (n > stereo_.getNumSamples()) // larger than prepared: run in chunks (the MIDI goes with the first one)
    {
        juce::MidiBuffer none;
        for (int start = 0; start < n; start += stereo_.getNumSamples())
        {
            const int len = std::min(stereo_.getNumSamples(), n - start);
            juce::AudioBuffer<float> view(buffer.getArrayOfWritePointers(), buffer.getNumChannels(), start, len);
            juce::MidiBuffer part;
            for (const auto event : midi)
                if (event.samplePosition >= start && event.samplePosition < start + len)
                    part.addEvent(event.getMessage(), event.samplePosition - start);
            processBlock(view, part);
        }
        midi.clear();
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

juce::AudioProcessorEditor* SeraphPlugin::createEditor()
{
    return new SeraphPluginEditor(*this);
}

void SeraphPlugin::getStateInformation(juce::MemoryBlock& destination)
{
    const auto bytes = plugins::encodeParameterState(device_, "SonoraSeraph", device_.stateAttributes());
    destination.replaceAll(bytes.data(), bytes.size());
}

void SeraphPlugin::setStateInformation(const void* data, int sizeInBytes)
{
    std::string error;
    std::map<std::string, std::string> attributes;
    if (sizeInBytes <= 0 || ! plugins::decodeParameterState(data, (size_t) sizeInBytes, device_, "SonoraSeraph", "Sonora Seraph", error, &attributes))
        buildLog.push_back("Could not restore the saved state: " + error); // never fail a load
    device_.applyStateAttributes(attributes);
    for (const auto& problem : device_.realErrors) buildLog.push_back("Real voice: " + problem); // a missing file never fails the load
    for (size_t i = 0; i < hostParameters_.size(); ++i) reported_[i].store(device_.getParameter((int) i));
    reportedContent_ = device_.encodeContent();
}
} // namespace sonora::plugin

#ifndef SONORA_PLUGIN_NO_ENTRY
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new sonora::plugin::SeraphPlugin();
}
#endif
