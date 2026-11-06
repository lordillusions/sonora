#include "plugin/HushPlugin.h"

#include "plugin/HushPluginEditor.h"

#include <algorithm>
#include <cmath>

namespace sonora::plugin
{
namespace
{
using H = devices::Hush;
using S = devices::HushSession;

// Host parameter over one Hush control: log mapping for frequencies, tilts and Q, the reference's text formats. The host
// id is the name with underscores ("Band_1_frequency", "Sidechain"), as since the first release: hosts key automation on it.
class HushParameter final : public juce::AudioProcessorParameterWithID
{
public:
    HushParameter(ParameterOwner& owner, int index, const devices::ParameterInfo& info)
        : AudioProcessorParameterWithID(juce::ParameterID(juce::String(info.name).replaceCharacter(' ', '_'), 1),
                                        juce::String(info.name)),
          owner_(owner), index_(index), info_(info), log_(S::isLogarithmic(info))
    {
        for (const auto& c : info.choices) choices_.add(juce::String(c));
    }

    float getValue() const override { return normalise(owner_.parameterValue(index_)); }
    void setValue(float normalised) override { owner_.setParameterValue(index_, plain(normalised)); }
    float getDefaultValue() const override { return normalise(info_.defaultValue); }
    int getNumSteps() const override
    {
        if (info_.isToggle) return 2;
        if (! choices_.isEmpty()) return choices_.size();
        if (info_.unit == "int") return (int) (info_.maxValue - info_.minValue) + 1;
        return juce::AudioProcessor::getDefaultNumParameterSteps();
    }
    bool isDiscrete() const override { return info_.isToggle || ! choices_.isEmpty() || info_.unit == "int"; }
    bool isBoolean() const override { return info_.isToggle; }
    juce::StringArray getAllValueStrings() const override { return choices_; }
    juce::String getText(float normalised, int maximumLength) const override
    {
        const auto text = S::formatValue(info_, plain(normalised));
        return maximumLength > 0 ? text.substring(0, maximumLength) : text;
    }
    float getValueForText(const juce::String& text) const override
    {
        return normalise(S::parseValue(info_, text, owner_.parameterValue(index_)));
    }

private:
    float normalise(float v) const
    {
        if (log_) return juce::jlimit(0.0f, 1.0f, std::log(std::max(v, info_.minValue) / info_.minValue) / std::log(info_.maxValue / info_.minValue));
        const float span = info_.maxValue - info_.minValue;
        return span > 0 ? juce::jlimit(0.0f, 1.0f, (v - info_.minValue) / span) : 0.0f;
    }
    float plain(float n) const
    {
        n = juce::jlimit(0.0f, 1.0f, n);
        if (log_) return info_.minValue * std::pow(info_.maxValue / info_.minValue, n);
        float v = info_.minValue + n * (info_.maxValue - info_.minValue);
        return isDiscrete() ? std::round(v) : v;
    }

    ParameterOwner& owner_;
    const int index_;
    const devices::ParameterInfo info_;
    const bool log_;
    juce::StringArray choices_;
};

} // namespace

HushPlugin::HushPlugin()
    : AudioProcessor(BusesProperties()
                         .withInput("Input", juce::AudioChannelSet::stereo(), true)
                         .withOutput("Output", juce::AudioChannelSet::stereo(), true)
                         .withInput("Sidechain", juce::AudioChannelSet::stereo(), false))
{
    hush_.setBypassed(false);
    const auto& infos = hush_.parameters();
    for (int i = 0; i < (int) infos.size(); ++i)
    {
        auto parameter = std::make_unique<HushParameter>(*this, i, infos[(size_t) i]);
        hostParameters_.push_back(parameter.get());
        addParameter(parameter.release());
    }
    // Face edits tell the host: the value, and gestures around drags.
    session_.onEdited = [this](int index) {
        auto* p = hostParameters_[(size_t) index];
        p->sendValueChangedMessageToListeners(p->getValue());
    };
    session_.onGesture = [this](int index, bool begin) {
        if (begin) hostParameters_[(size_t) index]->beginChangeGesture();
        else hostParameters_[(size_t) index]->endChangeGesture();
    };
    session_.hostName = juce::PluginHostType().getHostDescription();
    session_.offline = isNonRealtime();
    session_.applySettings();
    startTimerHz(20);
}

HushPlugin::~HushPlugin()
{
    stopTimer();
}

bool HushPlugin::isBusesLayoutSupported(const BusesLayout& layouts) const
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

void HushPlugin::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    session_.offline = isNonRealtime();
    session_.applySettings(); // offline bounces start with "always render on ultra" already applied
    hush_.prepare(sampleRate, samplesPerBlock);
    stereo_.setSize(2, samplesPerBlock);
    sidechain_.setSize(2, samplesPerBlock);
    setLatencySamples(hush_.currentLatency());
}

void HushPlugin::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
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
    const int mainChannels = std::max(1, getMainBusNumInputChannels());
    for (int ch = 0; ch < 2; ++ch) stereo_.copyFrom(ch, 0, main, std::min(ch, mainChannels - 1), 0, n);
    juce::AudioBuffer<float> io(stereo_.getArrayOfWritePointers(), 2, 0, n);

    const auto* scBus = getBus(true, 1);
    if (scBus != nullptr && scBus->isEnabled() && scBus->getNumberOfChannels() > 0)
    {
        auto sc = getBusBuffer(buffer, true, 1);
        for (int ch = 0; ch < 2; ++ch) sidechain_.copyFrom(ch, 0, sc, std::min(ch, sc.getNumChannels() - 1), 0, n);
        juce::AudioBuffer<float> key(sidechain_.getArrayOfWritePointers(), 2, 0, n);
        hush_.setSidechain(&key, 1.0f);
        hush_.process(io, midi);
    }
    else
    {
        hush_.setSidechain(nullptr, 1.0f);
        hush_.process(io, midi);
    }
    hush_.setSidechain(nullptr, 1.0f);
    auto out = getBusBuffer(buffer, false, 0);
    if (out.getNumChannels() == 1) // mono: the average of both processed channels
    {
        out.copyFrom(0, 0, io, 0, 0, n);
        out.addFrom(0, 0, io, 1, 0, n);
        out.applyGain(0, 0, n, 0.5f);
    }
    else
        for (int ch = 0; ch < out.getNumChannels(); ++ch) out.copyFrom(ch, 0, io, std::min(ch, 1), 0, n);
}

juce::AudioProcessorEditor* HushPlugin::createEditor()
{
    return new HushPluginEditor(*this);
}

void HushPlugin::timerCallback()
{
    session_.offline = isNonRealtime();
    session_.applySettings();
    const int latency = hush_.currentLatency();
    if (latency != getLatencySamples()) setLatencySamples(latency);
}

void HushPlugin::setCurrentProgram(int index)
{
    const auto& list = S::factoryPresets();
    if (index < 0 || index >= (int) list.size()) return;
    currentProgram_ = index;
    session_.loadPreset(list[(size_t) index]);
}

const juce::String HushPlugin::getProgramName(int index)
{
    const auto& list = S::factoryPresets();
    return index >= 0 && index < (int) list.size() ? juce::String(list[(size_t) index].name) : juce::String();
}

// The session's bytes; JUCE's VST3 wrapper appends its private-data trailer (plugins/SonoraPluginState).
void HushPlugin::getStateInformation(juce::MemoryBlock& destination)
{
    const auto bytes = session_.encodeState();
    destination.replaceAll(bytes.data(), bytes.size());
}

void HushPlugin::setStateInformation(const void* data, int sizeInBytes)
{
    std::string error;
    if (sizeInBytes <= 0 || ! session_.decodeState(data, (size_t) sizeInBytes, error))
        buildLog.push_back("Could not restore the saved state: " + error); // never fail a load
}

} // namespace sonora::plugin

#ifndef SONORA_PLUGIN_NO_ENTRY
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new sonora::plugin::HushPlugin();
}
#endif
