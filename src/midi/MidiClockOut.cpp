#include "midi/MidiClockOut.h"

#include <algorithm>
#include <vector>

namespace sonora::midi
{

MidiClockOut::MidiClockOut() : juce::Thread("MIDI clock out")
{
    startThread(juce::Thread::Priority::high);
}

MidiClockOut::~MidiClockOut()
{
    stopThread(500);
}

void MidiClockOut::setOutput(const juce::String& identifier)
{
    std::lock_guard lock(outputMutex_);
    identifier_ = identifier;
    output_ = identifier.isEmpty() ? nullptr : juce::MidiOutput::openDevice(identifier);
}

void MidiClockOut::push(double time, juce::uint8 a, juce::uint8 b, juce::uint8 c, int size)
{
    const auto scope = fifo_.write(1);
    if (scope.blockSize1 > 0)
        data_[(size_t) scope.startIndex1] = Event{time, {a, b, c}, size};
    // FIFO full: drop (the sender thread is stalled, a clock tick is not worth blocking the audio thread)
}

void MidiClockOut::transportBlock(const BlockInfo& block, double blockStartSeconds)
{
    if (! enabled_.load(std::memory_order_relaxed))
    {
        wasPlaying_ = block.playing;
        return;
    }
    const double t = blockStartSeconds + latency_.load(std::memory_order_relaxed);
    if (block.playing && ! wasPlaying_)
    {
        if (block.startBeats > 1e-6)
        {
            const int sixteenths = (int) std::lround(block.startBeats * 4.0) & 0x3fff; // SPP counts 16th notes
            push(t, 0xF2, (juce::uint8) (sixteenths & 0x7f), (juce::uint8) ((sixteenths >> 7) & 0x7f), 3);
            push(t, 0xFB); // Continue
        }
        else
        {
            push(t, 0xFA); // Start
        }
    }
    else if (block.justStopped || (! block.playing && wasPlaying_))
    {
        push(t, 0xFC); // Stop
    }
    wasPlaying_ = block.playing;
}

void MidiClockOut::clockSegment(double startBeats, int sampleOffset, int numSamples, double samplesPerBeat,
                                double sampleRate, double blockStartSeconds)
{
    if (! enabled_.load(std::memory_order_relaxed))
        return;
    const double base = blockStartSeconds + latency_.load(std::memory_order_relaxed);
    forEachClockTick(startBeats, numSamples, samplesPerBeat,
                     [&](int offset) { push(base + (sampleOffset + offset) / sampleRate, 0xF8); });
}

void MidiClockOut::run()
{
    std::vector<Event> pending;
    while (! threadShouldExit())
    {
        const int ready = fifo_.getNumReady();
        if (ready > 0)
        {
            const auto scope = fifo_.read(ready);
            for (int i = 0; i < scope.blockSize1; ++i)
                pending.push_back(data_[(size_t) (scope.startIndex1 + i)]);
            for (int i = 0; i < scope.blockSize2; ++i)
                pending.push_back(data_[(size_t) (scope.startIndex2 + i)]);
            std::stable_sort(pending.begin(), pending.end(), [](const Event& a, const Event& b) { return a.time < b.time; });
        }

        const double now = juce::Time::getMillisecondCounterHiRes() * 0.001;
        size_t sent = 0;
        while (sent < pending.size() && pending[sent].time <= now + 0.0005)
        {
            const auto& e = pending[sent++];
            if (now - e.time > 0.25)
                continue; // stale (device was blocked): do not burst old ticks
            if (sink)
            {
                sink(e.bytes.data(), e.size);
            }
            else
            {
                std::lock_guard lock(outputMutex_);
                if (output_ != nullptr)
                    output_->sendMessageNow(juce::MidiMessage(e.bytes.data(), e.size));
            }
        }
        pending.erase(pending.begin(), pending.begin() + (std::ptrdiff_t) sent);
        juce::Thread::sleep(1);
    }
}

} // namespace sonora::midi
