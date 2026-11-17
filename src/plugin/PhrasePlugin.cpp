#include "plugin/PhrasePlugin.h"

#include "plugin/MidiDragOut.h"
#include "plugin/PhrasePluginEditor.h"
#include "plugins/SonoraPluginState.h"

#include <algorithm>
#include <cmath>

namespace sonora::plugin
{
PhrasePlugin::PhrasePlugin()
    : AudioProcessor(BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true))
{
    device_.setBypassed(false); // the host has its own bypass
    device_.setParameter(devices::Phrase::Audition, 1.0f); // alone in a host it should sound
    const auto& infos = device_.parameters();
    reported_ = std::make_unique<std::atomic<float>[]>(infos.size());
    for (int i = 0; i < (int) infos.size(); ++i)
    {
        const auto& info = infos[(size_t) i];
        const bool integral = info.unit == "int";
        auto parameter = std::make_unique<DeviceParameter>(*this, i, info, integral);
        hostParameters_.push_back(parameter.get());
        reported_[(size_t) i].store(device_.getParameter(i));
        addParameter(parameter.release());
    }
    startTimerHz(20);
}

PhrasePlugin::~PhrasePlugin()
{
    stopTimer();
}

bool PhrasePlugin::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    const auto output = layouts.getMainOutputChannelSet();
    return output == juce::AudioChannelSet::stereo() || output == juce::AudioChannelSet::mono();
}

void PhrasePlugin::setParameterValue(int index, float plainValue)
{
    if (index < 0 || index >= (int) hostParameters_.size()) return;
    device_.setParameter(index, plainValue);
    reported_[(size_t) index].store(device_.getParameter(index));
}

void PhrasePlugin::reportEditorChanges()
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

void PhrasePlugin::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    sampleRate_ = sampleRate;
    const int block = std::max(16, samplesPerBlock);
    device_.prepare(sampleRate, block);
    voice_.prepare(sampleRate, block);
    bridge_.prepare(block);
    stereo_.setSize(2, block);
    voiceMidi_.ensureSize(16 * 1024);
    setLatencySamples(0);
}

void PhrasePlugin::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples();
    buffer.clear();
    const auto info = hostInfo(getPlayHead(), sampleRate_);
    tempo_ = info.tempo;
    bridge_.process(device_, buffer, midi, info); // midi now holds the part
    if (device_.getParameter(devices::Phrase::Audition) < 0.5f) return;
    for (int start = 0; start < n; start += stereo_.getNumSamples())
    {
        const int len = std::min(stereo_.getNumSamples(), n - start);
        voiceMidi_.clear();
        for (const auto event : midi)
            if (event.samplePosition >= start && event.samplePosition < start + len) voiceMidi_.addEvent(event.data, event.numBytes, event.samplePosition - start);
        stereo_.clear(0, len);
        juce::AudioBuffer<float> io(stereo_.getArrayOfWritePointers(), 2, 0, len);
        voice_.process(io, voiceMidi_);
        if (buffer.getNumChannels() == 1)
        {
            buffer.addFrom(0, start, io, 0, 0, len, 0.5f);
            buffer.addFrom(0, start, io, 1, 0, len, 0.5f);
        }
        else
            for (int ch = 0; ch < std::min(2, buffer.getNumChannels()); ++ch) buffer.addFrom(ch, start, io, ch, 0, len);
    }
}

std::vector<model::MidiNote> PhrasePlugin::partNotes(double& lengthBeats) const
{
    lengthBeats = device_.clipLengthBeats();
    std::vector<model::MidiNote> input;
    if (device_.listensToInput())
    {
        const auto held = device_.heldChord();
        for (int i = 0; i < held.count; ++i)
        {
            model::MidiNote note;
            note.duration = lengthBeats;
            note.pitch = model::pitchOf(held.root, 4) + held.interval(i);
            input.push_back(note);
        }
    }
    return device_.renderNotes(input, 0, lengthBeats, tempo_);
}

bool PhrasePlugin::dragMidi(juce::Component* source)
{
    double length = 0;
    const auto notes = partNotes(length);
    std::string error;
    const std::string name = "Sonora Phrase " + std::string(model::phrasePartName(device_.part()));
    if (dragMidiOut(notes, length, tempo_, name, source, error)) return true;
    if (! error.empty()) buildLog.push_back(error);
    return false;
}

juce::AudioProcessorEditor* PhrasePlugin::createEditor()
{
    return new PhrasePluginEditor(*this);
}

void PhrasePlugin::getStateInformation(juce::MemoryBlock& destination)
{
    const auto bytes = plugins::encodeGeneratorState(device_, "SonoraPhrase");
    destination.replaceAll(bytes.data(), bytes.size());
}

void PhrasePlugin::setStateInformation(const void* data, int sizeInBytes)
{
    std::string error;
    if (sizeInBytes <= 0 || ! plugins::decodeGeneratorState(data, (size_t) sizeInBytes, device_, "SonoraPhrase", "Sonora Phrase", error))
        buildLog.push_back("Could not restore the saved state: " + error); // never fail a load
    for (size_t i = 0; i < hostParameters_.size(); ++i) reported_[i].store(device_.getParameter((int) i));
}
} // namespace sonora::plugin

#ifndef SONORA_PLUGIN_NO_ENTRY
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new sonora::plugin::PhrasePlugin();
}
#endif
