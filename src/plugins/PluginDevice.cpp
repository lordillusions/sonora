#include "plugins/PluginDevice.h"
#include "plugins/Vst3PresetBuilder.h"

#include <cmath>

namespace sonora::plugins
{
bool PluginDevice::captureState(model::Device& device) const
{
    if (device.kind != model::DeviceKind::PluginVst3 || ! device.plugin) return false;
    const auto* client = instance_->getVST3Client();
    if (! client) return false;
    const auto preset = client->getPreset();
    if (preset.isEmpty()) return false;
    const auto* bytes = static_cast<const uint8_t*>(preset.getData());
    return readVstPreset({bytes, bytes + preset.getSize()}, *device.plugin);
}

PluginDevice::PluginDevice(std::unique_ptr<juce::AudioPluginInstance> instance) : instance_(std::move(instance))
{
    instrument_ = instance_->getPluginDescription().isInstrument;
    // Parameters are exposed normalised 0..1 (plugins define their own display units).
    for (auto* p : instance_->getParameters())
        infos_.push_back(devices::makeParam(std::to_string(p->getParameterIndex()), p->getName(16).toStdString(),
                                            0.0f, 1.0f, p->getDefaultValue(), "", p->isBoolean()));
}

PluginDevice::~PluginDevice()
{
    instance_->setPlayHead(nullptr);
    instance_->releaseResources();
}

juce::Optional<juce::AudioPlayHead::PositionInfo> PluginDevice::PlayHead::getPosition() const
{
    PositionInfo position;
    const double tempo = info.tempo > 0 ? info.tempo : 120.0;
    const double seconds = info.beats * 60.0 / tempo;
    const int numerator = std::max(1, info.timeSigNumerator), denominator = std::max(1, info.timeSigDenominator);
    const double beatsPerBar = numerator * 4.0 / denominator;
    position.setBpm(tempo);
    position.setTimeSignature(TimeSignature{numerator, denominator});
    position.setPpqPosition(info.beats);
    position.setPpqPositionOfLastBarStart(std::floor(info.beats / beatsPerBar + 1e-9) * beatsPerBar);
    position.setBarCount((juce::int64) std::floor(info.beats / beatsPerBar + 1e-9));
    position.setTimeInSeconds(seconds);
    position.setTimeInSamples((juce::int64) std::llround(seconds * info.sampleRate));
    position.setIsPlaying(info.playing);
    position.setIsRecording(false); // TODO(PLG-10): an audio-thread-safe recording flag
    position.setIsLooping(info.looping);
    if (info.looping) position.setLoopPoints(LoopPoints{info.loopStart, info.loopEnd});
    return position;
}

std::string PluginDevice::name() const
{
    return instance_->getName().toStdString();
}

bool PluginDevice::isInstrument() const
{
    return instrument_;
}

void PluginDevice::prepare(double sampleRate, int maxBlockSize)
{
    // Ask for stereo in/out; instruments get no input.
    auto layout = instance_->getBusesLayout();
    if (! layout.inputBuses.isEmpty())
        layout.inputBuses.getReference(0) = juce::AudioChannelSet::stereo();
    if (! layout.outputBuses.isEmpty())
        layout.outputBuses.getReference(0) = juce::AudioChannelSet::stereo();
    instance_->setBusesLayout(layout); // best effort, plugins may refuse

    instance_->setRateAndBufferSizeDetails(sampleRate, maxBlockSize);
    playHead_.info.sampleRate = sampleRate;
    instance_->setPlayHead(&playHead_); // before prepareToPlay: some plugins read the transport while preparing
    instance_->prepareToPlay(sampleRate, maxBlockSize);
    const int channels = juce::jmax(2, instance_->getTotalNumInputChannels(), instance_->getTotalNumOutputChannels());
    io_.setSize(channels, maxBlockSize);
    releases_.prepare();
}

void PluginDevice::process(juce::AudioBuffer<float>& audio, juce::MidiBuffer& midi)
{
    const int n = audio.getNumSamples();
    if (isBypassed())
        return;
    releases_.process(midi, playing_);

    const bool instrument = instrument_;
    io_.clear(0, n);
    if (! instrument)
        for (int ch = 0; ch < 2; ++ch)
            io_.copyFrom(ch, 0, audio, ch, 0, n);

    juce::AudioBuffer<float> view(io_.getArrayOfWritePointers(), io_.getNumChannels(), n);
    instance_->processBlock(view, midi);

    for (int ch = 0; ch < 2; ++ch)
    {
        const int src = juce::jmin(ch, io_.getNumChannels() - 1);
        if (instrument)
            audio.addFrom(ch, 0, io_, src, 0, n);
        else
            audio.copyFrom(ch, 0, io_, src, 0, n);
    }
}

void PluginDevice::reset()
{
    instance_->reset();
}

int PluginDevice::latencySamples() const
{
    return instance_->getLatencySamples();
}

int PluginDevice::findParameterByHostId(int id, const std::string& name) const
{
    if (! lookupBuilt_)
    {
        const auto& params = instance_->getParameters();
        idToIndex_.reserve((size_t) params.size());
        nameToIndex_.reserve((size_t) params.size());
        for (auto* p : params)
        {
            const int index = p->getParameterIndex();
            if (auto* hosted = dynamic_cast<juce::AudioPluginInstance::HostedParameter*>(p))
                idToIndex_.emplace(hosted->getParameterID().toStdString(), index);
            nameToIndex_.emplace(p->getName(64).toStdString(), index); // first parameter with a name wins
        }
        lookupBuilt_ = true;
    }
    if (const auto it = idToIndex_.find(std::to_string(id)); it != idToIndex_.end())
        return it->second;
    if (! name.empty())
        if (const auto it = nameToIndex_.find(name); it != nameToIndex_.end())
            return it->second;
    return -1;
}

float PluginDevice::getParameter(int index) const
{
    const auto& params = instance_->getParameters();
    return index >= 0 && index < params.size() ? params[index]->getValue() : 0.0f;
}

void PluginDevice::setParameter(int index, float value)
{
    const auto& params = instance_->getParameters();
    if (index >= 0 && index < params.size())
        params[index]->setValue(value); // automation / MIDI: no host round trip
}

bool PluginDevice::hasEditor() const
{
    return instance_->hasEditor();
}

} // namespace sonora::plugins
