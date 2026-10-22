#pragma once
// MIDI clock + transport out (ROADMAP MIDI-6): 24 ppqn clock, Start / Continue / Stop and Song
// Position Pointer, sent to one chosen output device.
//
// The audio thread only computes *when* events are due and pushes them into a lock-free FIFO
// (timestamped on the JUCE hi-res clock); a small sender thread transmits them on time.

#include <juce_audio_devices/juce_audio_devices.h>

#include <array>
#include <functional>

namespace sonora::midi
{

// Calls fn(sampleOffset) for every MIDI clock tick (24 per beat) that falls in [startBeats,
// startBeats + numSamples / samplesPerBeat). Pure, unit tested.
template <typename Fn>
void forEachClockTick(double startBeats, int numSamples, double samplesPerBeat, Fn&& fn)
{
    const double endBeats = startBeats + numSamples / samplesPerBeat;
    for (double k = std::ceil(startBeats * 24.0 - 1e-9); k / 24.0 < endBeats - 1e-12; k += 1.0)
    {
        const int offset = (int) std::lround((k / 24.0 - startBeats) * samplesPerBeat);
        fn(std::clamp(offset, 0, numSamples - 1));
    }
}

class MidiClockOut : private juce::Thread
{
public:
    MidiClockOut();
    ~MidiClockOut() override;

    // ---- message thread
    juce::Array<juce::MidiDeviceInfo> availableOutputs() const { return juce::MidiOutput::getAvailableDevices(); }
    // Empty identifier closes the output.
    void setOutput(const juce::String& identifier);
    juce::String outputIdentifier() const { return identifier_; }
    void setEnabled(bool on) { enabled_.store(on); }
    bool isEnabled() const { return enabled_.load(); }
    // Seconds added to every event time (compensates audio output latency).
    void setLatencySeconds(double s) { latency_.store(s); }

    // Replaces the transmit function (tests). Called on the sender thread with (bytes, size).
    std::function<void(const juce::uint8*, int)> sink;

    // ---- audio thread
    struct BlockInfo
    {
        bool playing = false;
        bool justStopped = false;
        double startBeats = 0.0; // block start on the timeline
    };
    // Called once per block (before the segments are rendered) for the transport messages...
    void transportBlock(const BlockInfo& block, double blockStartSeconds);
    // ...and once per rendered segment for the clock ticks.
    void clockSegment(double startBeats, int sampleOffset, int numSamples, double samplesPerBeat, double sampleRate,
                      double blockStartSeconds);

private:
    struct Event
    {
        double time = 0.0;
        std::array<juce::uint8, 3> bytes{};
        int size = 1;
    };
    void push(double time, juce::uint8 a, juce::uint8 b = 0, juce::uint8 c = 0, int size = 1);
    void run() override;

    std::unique_ptr<juce::MidiOutput> output_;
    juce::String identifier_;
    std::atomic<bool> enabled_{false};
    std::atomic<double> latency_{0.0};
    bool wasPlaying_ = false; // audio thread

    static constexpr int kFifoSize = 512;
    juce::AbstractFifo fifo_{kFifoSize};
    std::array<Event, kFifoSize> data_;
    std::mutex outputMutex_;
};

} // namespace sonora::midi
