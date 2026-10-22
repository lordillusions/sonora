#pragma once
// DEV-23: native stand-in for the third-party "Swiss Army Meter" Max for Live loudness meter (matched by its .amxd
// file name, see model::maxForLiveNativeName). Clean-room from watching the device run, no Max code or assets:
// short-term LUFS, integrated LUFS and true peak, per-channel momentary meters and a stereo correlation bar.
// Like the original it measures only while the transport plays (readings hold when it stops) and restarts its
// integrated loudness and true peak when playback starts. Audio passes through unchanged. The original stores no
// automatable parameters (only its UI state in a blob), so there is nothing to load or save besides On.

#include "devices/NativeDevice.h"
#include "dsp/Loudness.h"

namespace sonora::devices
{

class SwissArmyMeter final : public NativeDevice
{
public:
    struct Reading
    {
        float shortTerm = dsp::kSilenceDb, integrated = dsp::kSilenceDb, truePeak = dsp::kSilenceDb; // LUFS / dBTP
        float momentaryL = dsp::kSilenceDb, momentaryR = dsp::kSilenceDb; // LUFS per channel; silence while stopped
        float correlation = 0.0f;                                          // -1 .. +1, 0 while stopped or silent
        bool measuring = false;                                            // transport playing and device on
    };

    SwissArmyMeter();

    std::string name() const override { return "Swiss Army Meter"; }
    void prepare(double sampleRate, int maxBlockSize) override;
    void reset() override;
    void setHostInfo(const HostInfo& info) override { playing_ = info.playing; }
    void process(juce::AudioBuffer<float>& audio, juce::MidiBuffer& midi) override;

    Reading reading() const; // any thread
    void requestReset() { resetRequested_.store(true, std::memory_order_relaxed); } // any thread; applied next block

private:
    void restart();
    void publishSilentMeters();

    dsp::LoudnessMeter meter_;
    bool playing_ = false, wasPlaying_ = false;
    uint64_t lastHops_ = 0;
    double sumLL_ = 0, sumRR_ = 0, sumLR_ = 0, correlationDecay_ = 0.999;
    std::atomic<bool> resetRequested_{false};
    std::atomic<float> shortTerm_{dsp::kSilenceDb}, integrated_{dsp::kSilenceDb}, truePeak_{dsp::kSilenceDb};
    std::atomic<float> momentaryL_{dsp::kSilenceDb}, momentaryR_{dsp::kSilenceDb}, correlation_{0.0f};
    std::atomic<bool> measuring_{false};
};

} // namespace sonora::devices
