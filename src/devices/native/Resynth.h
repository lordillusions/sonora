#pragma once
#include "core/ResynthModel.h"
#include "devices/NativeDevice.h"
#include "devices/native/WavetableData.h"
#include "dsp/Dsp.h"
#include <array>

namespace sonora::devices
{
// RESYN-1: immutable spectral model, bounded voices and sample-offset MIDI. Tables are built before publication.
class Resynth final : public NativeDevice
{
public:
    enum Param { Mode, Example, RootOffset, Attack, Release, Tone, Noise, Movement, Sustain, Glide,
                 Velocity, Volume, PitchMotion, HybridAmount,
                 Fundamental, OddLevel, EvenLevel, SpectralTilt, PartialCount, SubLevel,
                 AmpDecay, AmpSustain, FilterOn, FilterType, Resonance, FilterEnv,
                 FilterAttack, FilterDecay, FilterSustain, FilterRelease, Drive,
                 LfoRate, LfoDepth, LfoShape, LfoDestination, LfoSync, PlayMode, BendRange,
                 NoiseLow, NoiseMid, NoiseHigh, NoiseAir, TimbreStart, ScanMode,
                 Harmonic0, NumParams = Harmonic0 + model::ResynthFrame::harmonics };
    explicit Resynth(std::shared_ptr<const model::ResynthModel> model = {});
    std::string name() const override { return "Sonora Resynth"; }
    bool isInstrument() const override { return true; }
    void prepare(double sampleRate, int maxBlockSize) override;
    void reset() override;
    void setHostInfo(const HostInfo& info) override { tempo_ = info.tempo; }
    void process(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    const model::ResynthModel* model() const { return model_.get(); }
    bool ready() const { return model_ != nullptr && ! prepared_.empty(); }
    size_t preparedBytes() const { return preparedBytes_; }
    void audition(int note = 48); // atomic UI request; -1 releases audition
    int activeExample() const { return activeExample_.load(std::memory_order_relaxed); }
    float playhead() const { return playhead_.load(std::memory_order_relaxed); }
    float harmonicGain(int harmonic) const;

private:
    struct Prepared { std::shared_ptr<const WavetableData> table; };
    struct Voice
    {
        int note = -1, channel = 0, example = 0;
        bool held = false, releasing = false;
        float velocity = 1, envelope = 0, releaseStep = 0;
        int ampStage = 1, filterStage = 1, modulationClock = 0;
        float filterEnvelope = 0, filterReleaseStep = 0;
        double phase = 0, subPhase = 0, time = 0, sourcePosition = 0, frequency = 0;
        std::array<std::array<float, 3>, 4> noiseFilter{};
        dsp::Svf filter;
        uint32_t random = 1;
        uint64_t age = 0;
    };
    void event(const uint8_t*, int);
    void render(juce::AudioBuffer<float>&, int begin, int end);
    void releaseVoice(Voice&);
    std::shared_ptr<const model::ResynthModel> model_;
    std::vector<Prepared> prepared_;
    size_t preparedBytes_ = 0;
    std::array<Voice, 8> voices_{};
    std::array<float, 16> bends_{};
    std::array<bool, 16> pedals_{};
    std::array<int, 16> exampleOverride_{}; // CC119: 0 automatic, 1..64 source region; sample-accurate phrase replay
    std::array<std::array<bool, 128>, 16> held_{};
    std::array<std::array<float, 128>, 16> velocities_{};
    double rate_ = 48000;
    double tempo_ = 120;
    dsp::Lfo lfo_;
    std::array<float, 3> noiseAlpha_{};
    std::array<float, 4> noiseNormalization_{};
    uint64_t age_ = 0;
    std::atomic<int> audition_{-2}, activeExample_{-1};
    std::atomic<float> playhead_{0};
};
} // namespace sonora::devices
