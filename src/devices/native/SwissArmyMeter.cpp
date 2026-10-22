#include "devices/native/SwissArmyMeter.h"

#include <cmath>

namespace sonora::devices
{

SwissArmyMeter::SwissArmyMeter() : NativeDevice({})
{
    meter_.setRealtime(true);
    meter_.prepare(48000.0);
}

void SwissArmyMeter::prepare(double sampleRate, int)
{
    meter_.prepare(sampleRate);
    // Correlation: exponential averages with a 300 ms time constant.
    correlationDecay_ = std::exp(-1.0 / (0.3 * std::max(8000.0, sampleRate)));
    reset();
}

void SwissArmyMeter::reset()
{
    restart();
    shortTerm_.store(dsp::kSilenceDb);
    integrated_.store(dsp::kSilenceDb);
    truePeak_.store(dsp::kSilenceDb);
    publishSilentMeters();
    playing_ = wasPlaying_ = false;
}

void SwissArmyMeter::restart()
{
    meter_.reset();
    lastHops_ = 0;
    sumLL_ = sumRR_ = sumLR_ = 0.0;
}

void SwissArmyMeter::publishSilentMeters()
{
    momentaryL_.store(dsp::kSilenceDb, std::memory_order_relaxed);
    momentaryR_.store(dsp::kSilenceDb, std::memory_order_relaxed);
    correlation_.store(0.0f, std::memory_order_relaxed);
    measuring_.store(false, std::memory_order_relaxed);
}

void SwissArmyMeter::process(juce::AudioBuffer<float>& audio, juce::MidiBuffer&)
{
    const bool start = playing_ && ! wasPlaying_;
    wasPlaying_ = playing_;
    if (resetRequested_.exchange(false, std::memory_order_relaxed) || start)
    {
        restart();
        shortTerm_.store(dsp::kSilenceDb, std::memory_order_relaxed);
        integrated_.store(dsp::kSilenceDb, std::memory_order_relaxed);
        truePeak_.store(dsp::kSilenceDb, std::memory_order_relaxed);
    }
    if (isBypassed() || ! playing_ || audio.getNumChannels() == 0)
    {
        publishSilentMeters(); // the readouts keep their last values
        return;
    }

    const int n = audio.getNumSamples();
    const float* left = audio.getReadPointer(0);
    const float* right = audio.getReadPointer(std::min(1, audio.getNumChannels() - 1));
    meter_.process(left, right, n);
    const double decay = correlationDecay_;
    for (int i = 0; i < n; ++i)
    {
        const double l = left[i], r = right[i];
        sumLL_ = sumLL_ * decay + l * l;
        sumRR_ = sumRR_ * decay + r * r;
        sumLR_ = sumLR_ * decay + l * r;
    }
    const double energy = std::sqrt(sumLL_ * sumRR_);
    correlation_.store(energy > 1e-9 ? (float) std::clamp(sumLR_ / energy, -1.0, 1.0) : 0.0f, std::memory_order_relaxed);
    truePeak_.store(meter_.truePeakDb(), std::memory_order_relaxed);
    if (meter_.hops() != lastHops_)
    {
        lastHops_ = meter_.hops();
        shortTerm_.store(meter_.shortTerm(), std::memory_order_relaxed);
        integrated_.store(meter_.integrated(), std::memory_order_relaxed);
        momentaryL_.store(meter_.momentaryChannel(0), std::memory_order_relaxed);
        momentaryR_.store(meter_.momentaryChannel(1), std::memory_order_relaxed);
    }
    measuring_.store(true, std::memory_order_relaxed);
}

SwissArmyMeter::Reading SwissArmyMeter::reading() const
{
    Reading r;
    r.shortTerm = shortTerm_.load(std::memory_order_relaxed);
    r.integrated = integrated_.load(std::memory_order_relaxed);
    r.truePeak = truePeak_.load(std::memory_order_relaxed);
    r.momentaryL = momentaryL_.load(std::memory_order_relaxed);
    r.momentaryR = momentaryR_.load(std::memory_order_relaxed);
    r.correlation = correlation_.load(std::memory_order_relaxed);
    r.measuring = measuring_.load(std::memory_order_relaxed);
    return r;
}

} // namespace sonora::devices
