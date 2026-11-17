#pragma once
// Shared pieces of the native-device VST3s (PLG-11 Sonora Prism, PLG-12 Sonora Curve LFO): a host parameter over one
// device control, and the host transport as the devices' HostInfo.

#include "devices/DeviceProcessor.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <algorithm>
#include <cmath>

namespace sonora::plugin
{

// Where a DeviceParameter reads and writes its plain value. The plug-in decides which instance that is (Prism swaps
// instances on content edits).
class ParameterOwner
{
public:
    virtual ~ParameterOwner() = default;
    virtual float parameterValue(int index) const = 0;
    virtual void setParameterValue(int index, float plainValue) = 0; // host automation: any thread
};

// "Osc.1/Unison" + "Unison" -> "Osc 2 Unison": host lists show parameters without the editor's sections, so the id's
// sections (zero-based in the id, one-based on screen) tell the oscillators, envelopes, LFOs and slots apart.
inline juce::String hostParameterName(const devices::ParameterInfo& info)
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

// A host parameter over one device control. The value lives in the running device (atomics), so host automation, the
// editor and MIDI learn all write the same place. `integral`: whole numbers only (counts, CC numbers). `logarithmic`: the
// host's 0..1 range follows a log scale (frequencies and times; min must be > 0).
class DeviceParameter final : public juce::AudioProcessorParameterWithID
{
public:
    DeviceParameter(ParameterOwner& owner, int index, const devices::ParameterInfo& info, bool integral, bool logarithmic = false)
        : AudioProcessorParameterWithID(juce::ParameterID(juce::String::fromUTF8(info.id.c_str()), 1), hostParameterName(info)),
          owner_(owner), index_(index), info_(info), integral_(integral), log_(logarithmic && info.minValue > 0)
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
        if (integral_) return (int) (info_.maxValue - info_.minValue) + 1;
        return juce::AudioProcessor::getDefaultNumParameterSteps();
    }
    bool isDiscrete() const override { return info_.isToggle || ! choices_.isEmpty() || integral_; }
    bool isBoolean() const override { return info_.isToggle; }
    juce::StringArray getAllValueStrings() const override { return choices_; }
    juce::String getText(float normalised, int maximumLength) const override
    {
        const float value = plain(normalised);
        juce::String text;
        if (info_.isToggle) text = value >= 0.5f ? "On" : "Off";
        else if (! choices_.isEmpty()) text = choices_[juce::jlimit(0, choices_.size() - 1, (int) std::lround(value - info_.minValue))];
        else if (integral_) text = juce::String((int) std::lround(value));
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
    float normalise(float value) const
    {
        if (log_)
            return juce::jlimit(0.0f, 1.0f, std::log(std::max(value, info_.minValue) / info_.minValue) / std::log(info_.maxValue / info_.minValue));
        const float span = info_.maxValue - info_.minValue;
        return span > 0 ? juce::jlimit(0.0f, 1.0f, (value - info_.minValue) / span) : 0.0f;
    }
    float plain(float normalised) const
    {
        if (log_) return info_.minValue * std::pow(info_.maxValue / info_.minValue, juce::jlimit(0.0f, 1.0f, normalised));
        float value = info_.minValue + juce::jlimit(0.0f, 1.0f, normalised) * (info_.maxValue - info_.minValue);
        if (isDiscrete()) value = std::round(value);
        return value;
    }

    ParameterOwner& owner_;
    const int index_;
    const devices::ParameterInfo info_;
    const bool integral_, log_;
    juce::StringArray choices_;
};

// The host's transport for one block (fields the host does not report keep HostInfo's defaults).
inline devices::HostInfo hostInfo(juce::AudioPlayHead* head, double sampleRate)
{
    devices::HostInfo info;
    info.sampleRate = sampleRate;
    if (head != nullptr)
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
    return info;
}

// PLG-25a: MIDI out of a pattern generator plug-in (Sonora Phrase, Sonora Pulse). These ship as VST3 instruments that send
// MIDI (TRANCE_SUITE.md "Decisions" 2; the juce_add_plugin flags are SONORA_GENERATOR_VST3_ARGS in src/CMakeLists.txt), so
// the processor's acceptsMidi() and producesMidi() return true and isMidiEffect() false. The bridge hands the host's MIDI
// to the device and puts the device's MIDI in its place, running the device in chunks of at most the block size it was
// prepared for (hosts may send more than they announced), with the transport advanced and wrapped at the loop per chunk.
// Audio thread; no allocation after prepare().
class GeneratorMidiBridge
{
public:
    void prepare(int maxChunk, int outputBytes = 72 * 1024)
    {
        chunk_ = std::max(1, maxChunk);
        in_.ensureSize(16 * 1024);
        out_.ensureSize((size_t) outputBytes);
    }

    void process(devices::DeviceProcessor& device, juce::AudioBuffer<float>& audio, juce::MidiBuffer& midi, devices::HostInfo info)
    {
        out_.clear();
        const int total = audio.getNumSamples();
        const double rate = info.sampleRate > 0 ? info.sampleRate : 44100.0;
        for (int start = 0; start < total; start += chunk_)
        {
            const int length = std::min(chunk_, total - start);
            in_.clear();
            for (const auto event : midi)
                if (event.samplePosition >= start && event.samplePosition < start + length)
                    in_.addEvent(event.data, event.numBytes, event.samplePosition - start);
            juce::AudioBuffer<float> part(audio.getArrayOfWritePointers(), audio.getNumChannels(), start, length);
            device.setHostInfo(info);
            device.process(part, in_);
            for (const auto event : in_) out_.addEvent(event.data, event.numBytes, event.samplePosition + start);
            if (info.playing)
            {
                info.beats += length / rate * info.tempo / 60.0;
                if (info.looping && info.loopEnd > info.loopStart && info.beats >= info.loopEnd)
                    info.beats = info.loopStart + (info.beats - info.loopEnd);
            }
        }
        midi.clear();
        midi.addEvents(out_, 0, -1, 0);
    }

private:
    int chunk_ = 512;
    juce::MidiBuffer in_, out_;
};

} // namespace sonora::plugin
