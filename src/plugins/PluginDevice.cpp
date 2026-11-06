#include "plugins/PluginDevice.h"
#include "plugins/Vst2Bank.h"
#include "plugins/Vst3PresetBuilder.h"

#include <algorithm>
#include <cmath>
#include <limits>
#if defined(__APPLE__)
 #include <CoreFoundation/CoreFoundation.h>
#endif

namespace sonora::plugins
{
namespace
{
#if defined(__APPLE__)
// JUCE hands out the AU ClassInfo as a binary plist; Live writes (and reads) the XML form.
std::vector<uint8_t> xmlPlist(const juce::MemoryBlock& binary)
{
    std::vector<uint8_t> out;
    auto* data = CFDataCreate(kCFAllocatorDefault, static_cast<const UInt8*>(binary.getData()), (CFIndex) binary.getSize());
    if (data == nullptr) return out;
    if (auto* list = CFPropertyListCreateWithData(kCFAllocatorDefault, data, kCFPropertyListImmutable, nullptr, nullptr))
    {
        if (auto* xml = CFPropertyListCreateData(kCFAllocatorDefault, list, kCFPropertyListXMLFormat_v1_0, 0, nullptr))
        {
            const auto* bytes = CFDataGetBytePtr(xml);
            out.assign(bytes, bytes + CFDataGetLength(xml));
            CFRelease(xml);
        }
        CFRelease(list);
    }
    CFRelease(data);
    return out;
}
#endif
} // namespace

bool capturePluginInfo(juce::AudioPluginInstance& instance, model::DeviceKind kind, model::PluginInfo& info)
{
    const auto format = instance.getPluginDescription().pluginFormatName;
    if (kind == model::DeviceKind::PluginVst3 && format == "VST3")
    {
        const auto* client = instance.getVST3Client();
        if (! client) return false;
        const auto preset = client->getPreset();
        if (preset.isEmpty()) return false;
        const auto* bytes = static_cast<const uint8_t*>(preset.getData());
        return readVstPreset({bytes, bytes + preset.getSize()}, info);
    }
    if (kind == model::DeviceKind::PluginVst2 && format == "VST")
    {
        juce::MemoryBlock bank;
        instance.getStateInformation(bank);
        const auto* bytes = static_cast<const uint8_t*>(bank.getData());
        model::PluginInfo read = info;
        read.vst2ParameterCount = instance.getParameters().size();
        if (bank.isEmpty() || ! readVst2Bank({bytes, bytes + bank.getSize()}, read)) return false;
        read.vst2ProgramNumber = std::max(0, instance.getCurrentProgram());
        info = std::move(read);
        return true;
    }
#if defined(__APPLE__)
    if (kind == model::DeviceKind::PluginAu && format == "AudioUnit")
    {
        juce::MemoryBlock state;
        instance.getStateInformation(state);
        auto xml = xmlPlist(state);
        if (xml.empty()) return false;
        info.state = std::move(xml);
        return true;
    }
#endif
    return false;
}

bool PluginDevice::captureState(model::Device& device) const
{
    return device.plugin && capturePluginInfo(*instance_, device.kind, *device.plugin);
}

PluginDevice::PluginDevice(std::unique_ptr<juce::AudioPluginInstance> instance) : instance_(std::move(instance))
{
    instrument_ = instance_->getPluginDescription().isInstrument;
    // Parameters are exposed normalised 0..1 (plugins define their own display units).
    const auto& params = instance_->getParameters();
    hand_ = std::make_unique<HandSlot[]>((size_t) params.size());
    automationSeen_ = std::make_unique<uint32_t[]>((size_t) params.size());
    for (auto* p : params)
    {
        infos_.push_back(devices::makeParam(std::to_string(p->getParameterIndex()), p->getName(16).toStdString(),
                                            0.0f, 1.0f, p->getDefaultValue(), "", p->isBoolean()));
        int id = std::numeric_limits<int>::min();
        // The plugin's own bypass switch is left out like the reference does: the device's On switch is the bypass.
        if (auto* hosted = dynamic_cast<juce::AudioPluginInstance::HostedParameter*>(p);
            hosted && p->isAutomatable() && p != instance_->getBypassParameter())
        {
            // VST3 ParamIDs are unsigned; the set stores them as a signed int (bit pattern kept).
            const auto text = hosted->getParameterID();
            id = text.containsOnly("0123456789") && text.isNotEmpty() ? (int) (uint32_t) text.getLargeIntValue() : p->getParameterIndex();
        }
        hostIds_.push_back(id);
        names_.push_back(p->getName(64).toStdString());
        auto& slot = hand_[(size_t) p->getParameterIndex()];
        slot.value.store(p->getValue()); slot.before.store(p->getValue());
    }
    instance_->addListener(this);
}

PluginDevice::~PluginDevice()
{
    instance_->removeListener(this);
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
    if (id < -1) // a VST3 ParamID above INT_MAX, stored with its bit pattern
        if (const auto it = idToIndex_.find(std::to_string((uint32_t) id)); it != idToIndex_.end())
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

bool PluginDevice::hostParameter(int index, int& id, std::string& name) const
{
    if (index < 0 || (size_t) index >= hostIds_.size() || hostIds_[(size_t) index] == std::numeric_limits<int>::min())
        return false;
    id = hostIds_[(size_t) index];
    name = names_[(size_t) index];
    return true;
}

devices::DeviceProcessor::HandEdit PluginDevice::handEdit(int index) const
{
    if (index < 0 || (size_t) index >= infos_.size()) return {};
    const auto& slot = hand_[(size_t) index];
    return {slot.serial.load(std::memory_order_acquire), slot.value.load(std::memory_order_relaxed),
            slot.before.load(std::memory_order_relaxed), slot.gesture.load(std::memory_order_relaxed)};
}

bool PluginDevice::takeHandEdit(int index)
{
    if (index < 0 || (size_t) index >= infos_.size()) return false;
    const uint32_t serial = hand_[(size_t) index].serial.load(std::memory_order_acquire);
    const bool edited = serial != automationSeen_[(size_t) index];
    automationSeen_[(size_t) index] = serial;
    return edited;
}

void PluginDevice::audioProcessorParameterChanged(juce::AudioProcessor*, int index, float value)
{
    if (index < 0 || (size_t) index >= infos_.size()) return;
    auto& slot = hand_[(size_t) index];
    const float previous = slot.value.exchange(value, std::memory_order_relaxed);
    if (! slot.gesture.load(std::memory_order_relaxed)) slot.before.store(previous, std::memory_order_relaxed);
    slot.serial.fetch_add(1, std::memory_order_release);
    handTotal_.fetch_add(1, std::memory_order_release);
}

void PluginDevice::audioProcessorParameterChangeGestureBegin(juce::AudioProcessor*, int index)
{
    if (index < 0 || (size_t) index >= infos_.size()) return;
    auto& slot = hand_[(size_t) index];
    slot.before.store(slot.value.load(std::memory_order_relaxed), std::memory_order_relaxed);
    slot.gesture.store(true, std::memory_order_relaxed);
    handTotal_.fetch_add(1, std::memory_order_release);
}

void PluginDevice::audioProcessorParameterChangeGestureEnd(juce::AudioProcessor*, int index)
{
    if (index < 0 || (size_t) index >= infos_.size()) return;
    hand_[(size_t) index].gesture.store(false, std::memory_order_relaxed);
    handTotal_.fetch_add(1, std::memory_order_release);
}

bool PluginDevice::hasEditor() const
{
    // PLG-13: never ask the instance. JUCE's VST3 wrapper answers by creating and releasing the plugin's editor view: ~0.2 s
    // for Splice Sounds Listener, which also outputs silence from then on (its whole track went quiet). The editor window
    // falls back to a generic editor when the plugin has none.
    return true;
}

} // namespace sonora::plugins
