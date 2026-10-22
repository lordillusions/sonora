#include "devices/RackProcessor.h"
#include "midi/InputBlock.h"

#include <algorithm>
#include <cmath>

namespace sonora::devices
{

RackProcessor::RackProcessor(std::string displayName, bool instrument, bool isDrumRack, std::vector<Chain> chains)
    : name_(std::move(displayName)), instrument_(instrument), drumRack_(isDrumRack), chains_(std::move(chains))
{
    for (int i = 0; i < kNumMacros; ++i)
        infos_.push_back(makeParam("MacroControls." + std::to_string(i), "Macro " + std::to_string(i + 1), 0.0f, 127.0f, 0.0f));
    infos_.push_back(makeParam("ChainSelector", "Chain", 0.0f, 127.0f, 0.0f));
    for (auto& v : values_)
        v.store(0.0f);
    chainMidi_.resize(chains_.size());
}

void RackProcessor::setMacros(const std::vector<std::string>& names, const std::array<float, kNumMacros>& defaults,
                              std::vector<MacroLink> links, int numVisible)
{
    for (int i = 0; i < kNumMacros; ++i)
    {
        if ((size_t) i < names.size() && ! names[(size_t) i].empty())
            infos_[(size_t) i].name = names[(size_t) i];
        infos_[(size_t) i].defaultValue = defaults[(size_t) i];
        values_[(size_t) i].store(defaults[(size_t) i]);
    }
    links_ = std::move(links);
    numVisibleMacros_ = std::clamp(numVisible, 0, kNumMacros);
    for (int i = 0; i < kNumMacros; ++i)
        applyMacro(i);
}

std::vector<int> RackProcessor::displayParameters() const
{
    std::vector<int> out;
    for (int i = 0; i < numVisibleMacros_; ++i)
        out.push_back(i);
    if (! drumRack_) out.push_back(kChainSelector);
    return out;
}

float RackProcessor::getParameter(int index) const
{
    return index >= 0 && (size_t) index < values_.size() ? values_[(size_t) index].load(std::memory_order_relaxed) : 0.0f;
}

void RackProcessor::setParameter(int index, float value)
{
    if (index < 0 || (size_t) index >= values_.size())
        return;
    values_[(size_t) index].store(std::clamp(value, 0.0f, 127.0f), std::memory_order_relaxed);
    if (index < kNumMacros)
        applyMacro(index);
}

// A macro sweeps the target between its mapped range. Frequency-like parameters move on a log
// scale, like their knobs do.
void RackProcessor::applyMacro(int macro)
{
    const float norm = values_[(size_t) macro].load(std::memory_order_relaxed) / 127.0f;
    for (const auto& l : links_)
    {
        if (l.macro != macro || l.target == nullptr)
            continue;
        const auto& info = l.target->parameters()[(size_t) l.paramIndex];
        float v;
        if (info.unit == "Hz" && l.min > 0.0f && l.max > 0.0f)
            v = l.min * std::pow(l.max / l.min, norm);
        else
            v = l.min + (l.max - l.min) * norm;
        if (info.isToggle || ! info.choices.empty())
            v = std::round(v);
        l.target->setParameter(l.paramIndex, v);
    }
}

void RackProcessor::prepare(double sampleRate, int maxBlockSize)
{
    scratch_.setSize(2, maxBlockSize);
    sum_.setSize(2, maxBlockSize);
    chainMidi_.resize(chains_.size());
    for (auto& m : chainMidi_)
        m.ensureSize(sonora::midi::InputBlock::bufferCapacity * 2);
    for (auto& c : chains_)
        for (auto& d : c.devices)
            d->prepare(sampleRate, maxBlockSize);

    // Align the chains: every chain is delayed up to the slowest one.
    latency_ = 0;
    for (auto& c : chains_)
    {
        c.latency = 0;
        for (auto& d : c.devices)
            c.latency += std::max(0, d->latencySamples());
        latency_ = std::max(latency_, c.latency);
    }
    for (auto& c : chains_)
        c.alignDelay.prepare(latency_ - c.latency);

    for (int i = 0; i < kNumMacros; ++i) // devices were just prepared: make sure they carry the macro values
        applyMacro(i);
}

void RackProcessor::setHostInfo(const HostInfo& info)
{
    for (auto& c : chains_)
        for (auto& d : c.devices)
            d->setHostInfo(info);
}

void RackProcessor::reset()
{
    for (auto& c : chains_)
        for (auto& d : c.devices)
            d->reset();
}

bool RackProcessor::chainSelected(const Chain& c, float selector) const
{
    const int s = (int) std::lround(selector);
    return s >= c.selMin && s <= c.selMax;
}

void RackProcessor::process(juce::AudioBuffer<float>& audio, juce::MidiBuffer& midi)
{
    if (isBypassed())
        return;

    const int n = audio.getNumSamples();
    const float selector = values_[kChainSelector].load(std::memory_order_relaxed);

    // 1) Distribute the notes: zones, drum pad routing, choke groups.
    for (auto& m : chainMidi_)
        m.clear();
    for (auto& chain : chains_) chain.midiOverflow = false;
    auto add = [&](size_t ci, const void* bytes, int size, int sample) {
        auto& buffer = chainMidi_[ci];
        if (chains_[ci].midiOverflow) return;
        if (buffer.data.size() + size + 16 > sonora::midi::InputBlock::bufferCapacity * 2)
        {
            buffer.clear(); chains_[ci].midiOverflow = true;
            for (int ch = 1; ch <= 16; ++ch) buffer.addEvent(juce::MidiMessage::allSoundOff(ch), sample);
        }
        else buffer.addEvent(bytes, size, sample);
    };
    for (const auto meta : midi)
    {
        if (meta.numBytes > 3)
        {
            for (size_t ci = 0; ci < chainMidi_.size(); ++ci) add(ci, meta.data, meta.numBytes, meta.samplePosition);
            continue;
        }
        const auto msg = meta.getMessage();
        for (size_t ci = 0; ci < chains_.size(); ++ci)
        {
            const auto& chain = chains_[ci];
            auto out = msg;
            if (msg.isNoteOnOrOff())
            {
                if (drumRack_)
                {
                    if (chain.receivingNote >= 0 && msg.getNoteNumber() != chain.receivingNote)
                        continue;
                    out.setNoteNumber(chain.sendingNote);
                }
                else
                {
                    if (msg.getNoteNumber() < chain.keyMin || msg.getNoteNumber() > chain.keyMax)
                        continue;
                    if (msg.isNoteOn() && (msg.getVelocity() < chain.velMin || msg.getVelocity() > chain.velMax))
                        continue;
                }
                if (drumRack_ && msg.isNoteOn() && chain.chokeGroup > 0)
                    for (size_t other = 0; other < chains_.size(); ++other)
                        if (other != ci && chains_[other].chokeGroup == chain.chokeGroup)
                            for (int ch = 1; ch <= 16; ++ch)
                            {
                                const auto off = juce::MidiMessage::noteOff(ch, chains_[other].sendingNote);
                                add(other, off.getRawData(), off.getRawDataSize(), meta.samplePosition);
                                const auto stop = juce::MidiMessage::allSoundOff(ch);
                                add(other, stop.getRawData(), stop.getRawDataSize(), meta.samplePosition);
                            }
            }
            add(ci, out.getRawData(), out.getRawDataSize(), meta.samplePosition);
        }
    }

    // 2) Run the chains and sum them.
    sum_.clear(0, n);
    for (size_t ci = 0; ci < chains_.size(); ++ci)
    {
        auto& chain = chains_[ci];
        if (! chain.active || ! chainSelected(chain, selector))
        {
            chain.alignDelay.pushSilence(n);
            continue;
        }

        // Instruments start from silence; effects start from the rack input.
        for (int ch = 0; ch < 2; ++ch)
        {
            if (instrument_)
                scratch_.clear(ch, 0, n);
            else
                scratch_.copyFrom(ch, 0, audio, ch, 0, n);
        }

        juce::AudioBuffer<float> view(scratch_.getArrayOfWritePointers(), 2, n);
        for (auto& d : chain.devices)
            d->process(view, chainMidi_[ci]);
        chain.alignDelay.processInPlace(scratch_, n);

        const float gl = chain.volume * juce::jmin(1.0f, 1.0f - chain.pan);
        const float gr = chain.volume * juce::jmin(1.0f, 1.0f + chain.pan);
        sum_.addFrom(0, 0, scratch_, 0, 0, n, gl);
        sum_.addFrom(1, 0, scratch_, 1, 0, n, gr);
    }

    for (int ch = 0; ch < 2; ++ch)
    {
        if (instrument_)
            audio.addFrom(ch, 0, sum_, ch, 0, n);
        else
            audio.copyFrom(ch, 0, sum_, ch, 0, n);
    }
}

} // namespace sonora::devices
