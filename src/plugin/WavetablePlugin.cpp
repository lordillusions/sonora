#include "plugin/WavetablePlugin.h"

#include "devices/DeviceFactory.h"
#include "plugins/PrismPreset.h"
#include "plugin/WavetablePluginEditor.h"

#include <algorithm>
#include <cmath>
#include <thread>

namespace sonora::plugin
{
namespace
{
using W = devices::WavetableSynth;

// "Osc.1/Unison" + "Unison" -> "Osc 2 Unison": host lists show parameters without the editor's sections, so the id's
// sections (zero-based in the id, one-based on screen) tell the oscillators, envelopes, LFOs and slots apart.
juce::String hostName(const devices::ParameterInfo& info)
{
    auto sections = juce::StringArray::fromTokens(juce::String::fromUTF8(info.id.c_str()), "/", "");
    sections.remove(sections.size() - 1);
    juce::String out;
    for (const auto& section : sections)
    {
        const auto word = section.upToFirstOccurrenceOf(".", false, false), number = section.fromFirstOccurrenceOf(".", false, false);
        out << word << ' ';
        if (number.containsOnly("0123456789") && number.isNotEmpty()) out << (number.getIntValue() + 1) << ' ';
        else if (number.isNotEmpty()) out << number << ' ';
    }
    return out + juce::String::fromUTF8(info.name.c_str());
}

// A host parameter over one synth control. The value lives in the running WavetableSynth (atomics), so host automation,
// the editor and MIDI learn all write the same place.
class SynthParameter final : public juce::AudioProcessorParameterWithID
{
public:
    SynthParameter(WavetablePlugin& owner, int index, const devices::ParameterInfo& info)
        : AudioProcessorParameterWithID(juce::ParameterID(juce::String::fromUTF8(info.id.c_str()), 1), hostName(info)),
          owner_(owner), index_(index), info_(info)
    {
        for (const auto& choice : info.choices) choices_.add(juce::String::fromUTF8(choice.c_str()));
    }

    float getValue() const override { return normalise(owner_.parameterValue(index_)); }
    void setValue(float normalised) override { owner_.setParameterValue(index_, plain(normalised)); }
    float getDefaultValue() const override { return normalise(info_.defaultValue); }
    juce::String getLabel() const override { return info_.unit == "int" ? juce::String() : juce::String::fromUTF8(info_.unit.c_str()); }
    int getNumSteps() const override
    {
        if (info_.isToggle) return 2;
        if (! choices_.isEmpty()) return choices_.size();
        if (integral()) return (int) (info_.maxValue - info_.minValue) + 1;
        return juce::AudioProcessor::getDefaultNumParameterSteps();
    }
    bool isDiscrete() const override { return info_.isToggle || ! choices_.isEmpty() || integral(); }
    bool isBoolean() const override { return info_.isToggle; }
    juce::StringArray getAllValueStrings() const override { return choices_; }
    juce::String getText(float normalised, int maximumLength) const override
    {
        const float value = plain(normalised);
        juce::String text;
        if (info_.isToggle) text = value >= 0.5f ? "On" : "Off";
        else if (! choices_.isEmpty()) text = choices_[juce::jlimit(0, choices_.size() - 1, (int) std::lround(value - info_.minValue))];
        else if (integral()) text = juce::String((int) std::lround(value));
        else if (info_.unit == "%") text = juce::String(value * 100.0f, 1);
        else text = juce::String(value, std::abs(value) >= 100.0f ? 1 : 3);
        return maximumLength > 0 ? text.substring(0, maximumLength) : text;
    }
    float getValueForText(const juce::String& text) const override
    {
        if (info_.isToggle) return text.trim().equalsIgnoreCase("on") || text.getFloatValue() >= 0.5f ? 1.0f : 0.0f;
        if (const int choice = choices_.indexOf(text.trim(), true); choice >= 0) return normalise(info_.minValue + (float) choice);
        const float value = text.getFloatValue();
        return normalise(info_.unit == "%" ? value / 100.0f : value);
    }

private:
    bool integral() const { return info_.unit == "int" || info_.unit == "st"; }
    float normalise(float value) const
    {
        const float span = info_.maxValue - info_.minValue;
        return span > 0 ? juce::jlimit(0.0f, 1.0f, (value - info_.minValue) / span) : 0.0f;
    }
    float plain(float normalised) const
    {
        float value = info_.minValue + juce::jlimit(0.0f, 1.0f, normalised) * (info_.maxValue - info_.minValue);
        if (isDiscrete()) value = std::round(value);
        return value;
    }

    WavetablePlugin& owner_;
    const int index_;
    const devices::ParameterInfo info_;
    juce::StringArray choices_;
};

} // namespace

WavetablePlugin::WavetablePlugin()
    : AudioProcessor(BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true))
{
    model::Track track;
    track.kind = model::TrackKind::Midi;
    track.id = 1;
    track.name = "Sonora Prism";
    auto device = devices::nativeDeviceModel("SonoraWavetable");
    device.id = 1; // fixed: the editor keys its remembered pages by device id
    track.devices.push_back(std::move(device));
    song_.tracks.push_back(std::move(track));

    std::vector<std::string> warnings;
    owned_ = buildSynth(this->device(), warnings);
    active_.store(owned_.get());
    const auto& infos = owned_->parameters();
    const int count = std::min<int>(W::ParameterCount, (int) infos.size());
    reported_ = std::make_unique<std::atomic<float>[]>((size_t) count);
    for (int i = 0; i < count; ++i)
    {
        auto parameter = std::make_unique<SynthParameter>(*this, i, infos[(size_t) i]);
        hostParameters_.push_back(parameter.get());
        reported_[(size_t) i].store(owned_->getParameter(i));
        addParameter(parameter.release());
    }
    startTimerHz(20);
}

WavetablePlugin::~WavetablePlugin()
{
    stopTimer();
    active_.store(nullptr);
}

bool WavetablePlugin::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    return layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo() && layouts.getMainInputChannelSet().isDisabled();
}

std::unique_ptr<devices::WavetableSynth> WavetablePlugin::buildSynth(const model::Device& device, std::vector<std::string>& warnings) const
{
    auto built = devices::createNative(device, &warnings);
    std::unique_ptr<W> result;
    if (auto* synth = dynamic_cast<W*>(built.get())) { built.release(); result.reset(synth); }
    else result = std::make_unique<W>();
    result->setBypassed(false); // the host has its own bypass
    result->prepare(sampleRate_, blockSize_);
    return result;
}

void WavetablePlugin::installSynth(std::unique_ptr<devices::WavetableSynth> next)
{
    // The audio thread raises inProcess_ before it reads active_ (both sequentially consistent): once it is seen low
    // after the exchange, no block can still be running on the old instance.
    auto old = std::move(owned_);
    owned_ = std::move(next);
    active_.store(owned_.get());
    while (inProcess_.load())
        std::this_thread::yield();
    if (old) retired_.push_back(std::move(old));
    if (onSynthReplaced) onSynthReplaced();
    setLatencySamples(owned_->latencySamples());
    quietReport_ = true;
}

void WavetablePlugin::captureState()
{
    devices::captureDeviceState(song_.tracks.front().devices.front(), owned_.get());
}

bool WavetablePlugin::editDevice(const std::function<bool(model::Device&)>& edit)
{
    captureState();
    auto edited = device();
    if (! edit(edited)) return false;
    edited.id = device().id;
    std::vector<std::string> warnings;
    auto next = buildSynth(edited, warnings);
    buildLog.insert(buildLog.end(), warnings.begin(), warnings.end());
    song_.tracks.front().devices.front() = std::move(edited);
    installSynth(std::move(next));
    return true;
}

float WavetablePlugin::parameterValue(int index) const
{
    const auto* synth = active_.load(std::memory_order_acquire);
    return synth != nullptr ? synth->getParameter(index) : 0.0f;
}

void WavetablePlugin::setParameterValue(int index, float plainValue)
{
    auto* synth = active_.load(std::memory_order_acquire);
    if (synth == nullptr || index < 0 || (size_t) index >= hostParameters_.size()) return;
    synth->setParameter(index, plainValue);
    reported_[(size_t) index].store(synth->getParameter(index)); // the host knows this one already
}

void WavetablePlugin::timerCallback()
{
    if (getActiveEditor() == nullptr) releaseRetired();
    reportEditorChanges();
}

void WavetablePlugin::reportEditorChanges()
{
    // The host records automation and marks its project changed from these.
    const bool gestures = ! quietReport_;
    quietReport_ = false;
    for (size_t i = 0; i < hostParameters_.size(); ++i)
    {
        const float value = owned_->getParameter((int) i);
        if (std::abs(value - reported_[i].load()) <= 0.0f) continue; // exact: any change, however small
        reported_[i].store(value);
        auto* parameter = hostParameters_[i];
        if (gestures) parameter->beginChangeGesture();
        parameter->sendValueChangedMessageToListeners(parameter->getValue());
        if (gestures) parameter->endChangeGesture();
    }
}

void WavetablePlugin::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    const bool changed = std::abs(sampleRate - sampleRate_) > 1e-6 || samplesPerBlock != blockSize_;
    sampleRate_ = sampleRate > 0 ? sampleRate : 44100.0;
    blockSize_ = std::max(1, samplesPerBlock);
    chunkMidi_.ensureSize(64 * 1024);
    // prepareToPlay runs with processing stopped, so the running instance can be prepared in place.
    if (changed) owned_->prepare(sampleRate_, blockSize_);
    else owned_->reset();
    setLatencySamples(owned_->latencySamples());
}

void WavetablePlugin::releaseResources()
{
}

void WavetablePlugin::renderChunk(devices::WavetableSynth& synth, juce::AudioBuffer<float>& audio, juce::MidiBuffer& midi,
                                  int start, int length)
{
    juce::AudioBuffer<float> part(audio.getArrayOfWritePointers(), 2, start, length);
    chunkMidi_.clear();
    for (const auto event : midi)
        if (event.samplePosition >= start && event.samplePosition < start + length)
            chunkMidi_.addEvent(event.data, event.numBytes, event.samplePosition - start);
    synth.process(part, chunkMidi_);
}

void WavetablePlugin::processBlock(juce::AudioBuffer<float>& audio, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    audio.clear();
    inProcess_.store(true);
    auto* synth = active_.load();
    if (synth == nullptr || audio.getNumChannels() < 2)
    {
        inProcess_.store(false);
        return;
    }
    devices::HostInfo info;
    info.sampleRate = sampleRate_;
    if (auto* head = getPlayHead())
        if (const auto position = head->getPosition())
        {
            if (const auto bpm = position->getBpm()) info.tempo = *bpm;
            if (const auto beats = position->getPpqPosition()) info.beats = *beats;
            info.playing = position->getIsPlaying();
            if (const auto signature = position->getTimeSignature())
            {
                info.timeSigNumerator = signature->numerator;
                info.timeSigDenominator = signature->denominator;
            }
            info.looping = position->getIsLooping();
            if (const auto loop = position->getLoopPoints())
            {
                info.loopStart = loop->ppqStart;
                info.loopEnd = loop->ppqEnd;
            }
        }
    const int total = audio.getNumSamples();
    if (total <= blockSize_)
    {
        synth->setHostInfo(info);
        synth->process(audio, midi);
    }
    else // a host may send more than it announced: run the synth in blocks it was prepared for
        for (int start = 0; start < total; start += blockSize_)
        {
            const int length = std::min(blockSize_, total - start);
            synth->setHostInfo(info);
            renderChunk(*synth, audio, midi, start, length);
            info.beats += length / sampleRate_ * info.tempo / 60.0;
        }
    inProcess_.store(false);
    midi.clear();
}

juce::AudioProcessorEditor* WavetablePlugin::createEditor()
{
    return new WavetablePluginEditor(*this);
}

juce::MemoryBlock WavetablePlugin::encodeState(const model::Device& device)
{
    const auto bytes = plugins::encodePrismPreset(device); // a saved state is a .prism preset
    return {bytes.data(), bytes.size()};
}

bool WavetablePlugin::decodeState(const void* data, size_t size, model::Device& device, std::string& error)
{
    if (data == nullptr || size == 0) { error = "empty state"; return false; }
    return plugins::decodePrismPreset({static_cast<const uint8_t*>(data), size}, device, error);
}

void WavetablePlugin::getStateInformation(juce::MemoryBlock& destination)
{
    const juce::MessageManagerLock lock;
    captureState();
    destination = encodeState(device());
}

void WavetablePlugin::setStateInformation(const void* data, int sizeInBytes)
{
    const juce::MessageManagerLock lock;
    model::Device loaded;
    loaded.id = device().id;
    std::string error;
    if (sizeInBytes <= 0 || ! decodeState(data, (size_t) sizeInBytes, loaded, error))
    {
        buildLog.push_back("Could not restore the saved state: " + error);
        return; // never fail a load: keep the current sound
    }
    std::vector<std::string> warnings;
    auto next = buildSynth(loaded, warnings);
    buildLog.insert(buildLog.end(), warnings.begin(), warnings.end());
    song_.tracks.front().devices.front() = std::move(loaded);
    installSynth(std::move(next));
}

} // namespace sonora::plugin

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new sonora::plugin::WavetablePlugin();
}
