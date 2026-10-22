#pragma once
// Immutable media and bounded native Sample/Multisample/Granular/Spectral primitives.
// Load/analysis allocates before graph publication. Runtime uses raw pointers and fixed state only.
// Granular windows and spectral resynthesis are independent Sonora DSP, not vendor algorithms.

#include <juce_audio_formats/juce_audio_formats.h>

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sonora::devices
{

class OscillatorSample;
struct OscillatorSampleResult
{
    std::shared_ptr<const OscillatorSample> data;
    std::string error;
    explicit operator bool() const { return data != nullptr; }
};

class OscillatorSample final
{
public:
    static constexpr size_t maxPcmBytes = 64 * 1024 * 1024;
    static constexpr int spectralSize = 2048, maxSpectralFrames = 8192, spectralPartials = 64;
    struct Partial { float frequency = 0, amplitudeLeft = 0, amplitudeRight = 0, phaseLeft = 0, phaseRight = 0; };
    struct SpectralFrame { std::array<Partial, spectralPartials> partial{}; };
    ~OscillatorSample() = default;

    static OscillatorSampleResult fromSamples(std::span<const float> interleaved, int channels,
                                              double sampleRate, std::string name = {}, bool spectral = false,
                                              int spectralFrameLimit = maxSpectralFrames);
    static OscillatorSampleResult fromFile(const juce::File& file, bool spectral = false,
                                           int spectralFrameLimit = maxSpectralFrames);
    // Shares immutable original PCM, preparing a new bounded analysis entirely off the audio thread.
    static OscillatorSampleResult withSpectralAnalysis(std::shared_ptr<const OscillatorSample> source,
                                                       int spectralFrameLimit = maxSpectralFrames);
    int channels() const { return channels_; }
    int sampleFrames() const { return static_cast<int>(samples_->size() / static_cast<size_t>(channels_)); }
    double sampleRate() const { return sampleRate_; }
    const std::string& name() const { return name_; }
    const std::vector<float>& originalSamples() const { return *samples_; }
    const std::string& sourceFile() const { return sourceFile_; } // Absolute origin, empty for embedded/generated PCM.
    uint64_t pcmFingerprint() const { return pcmFingerprint_; } // FNV1a64 over LE float32 bits; integrity, not cryptographic security.
    const std::vector<SpectralFrame>& spectralFrames() const { return spectral_; }
    int spectralHop() const { return spectralHop_; }
    double spectralHopSeconds() const { return spectral_.empty() ? 0 : spectralHop_ / sampleRate_; }
    bool hasSpectralAnalysis() const { return ! spectral_.empty(); }
    size_t storageBytes() const { return samples_->size() * sizeof(float) + spectral_.size() * sizeof(SpectralFrame); }
    float sampleLinear(double position, int channel) const;

private:
    friend class OscillatorMedia;
    OscillatorSample() = default;
    OscillatorSample(const OscillatorSample&) = delete;
    OscillatorSample& operator=(const OscillatorSample&) = delete;
    void analyzeSpectral(int frameLimit);
    std::shared_ptr<const std::vector<float>> samples_;
    std::vector<SpectralFrame> spectral_;
    std::string name_, sourceFile_;
    uint64_t pcmFingerprint_ = 0;
    int channels_ = 1, spectralHop_ = 512;
    double sampleRate_ = 44100;
};

enum class MediaLoop { None, OneShot, Forward, Sustain, PingPong };
struct OscillatorZone
{
    std::shared_ptr<const OscillatorSample> sample;
    int lowKey = 0, highKey = 127, lowVelocity = 0, highVelocity = 127, rootKey = 60;
    float tuneCents = 0, gainDb = 0, pan = 0, velocityTracking = 0, keyTracking = 1;
    double attackSeconds = 0, releaseSeconds = 0.1;
    // Frame indices: offset/start inclusive, end/loopEnd exclusive. Factories resolve -1 to the file end.
    int64_t offset = 0, end = -1, loopStart = 0, loopEnd = -1, loopCrossfade = 0;
    MediaLoop loop = MediaLoop::None;
    bool loopWhileHeld = false; // Allows a pingpong loop to follow SFZ loop_sustain release behavior.
};

class OscillatorMedia;
struct OscillatorMediaResult
{
    std::shared_ptr<const OscillatorMedia> data;
    std::string error;
    std::vector<std::string> warnings;
    explicit operator bool() const { return data != nullptr; }
};
class OscillatorMedia final
{
public:
    static constexpr int maxZones = 256;
    static constexpr size_t maxSfzBytes = 1024 * 1024, maxTotalPcmBytes = 768 * 1024 * 1024;
    static constexpr size_t maxSpectralBytes = 64 * 1024 * 1024, maxTotalBytes = maxTotalPcmBytes + maxSpectralBytes;
    ~OscillatorMedia() = default;
    static OscillatorMediaResult fromSamples(std::span<const float> interleaved, int channels,
                                             double sampleRate, std::string name = {}, bool spectral = false);
    static OscillatorMediaResult fromFile(const juce::File& file, bool spectral = false);
    static OscillatorMediaResult fromZones(std::vector<OscillatorZone> zones, std::string name = {},
                                           bool spectral = false, size_t spectralBudgetBytes = maxSpectralBytes);
    static OscillatorMediaResult fromSfz(const juce::File& file, bool spectral = false);
    static OscillatorMediaResult fromSfzText(std::string_view text, const juce::File& baseDirectory,
                                            std::string name = {}, bool spectral = false);
    const std::vector<OscillatorZone>& zones() const { return zones_; }
    const std::string& name() const { return name_; }
    size_t pcmBytes() const { return pcmBytes_; }
    size_t spectralBytes() const { return spectralBytes_; }
    size_t storageBytes() const { return pcmBytes_ + spectralBytes_; }
    double maximumSpectralHopSeconds() const { return maximumSpectralHopSeconds_; }

private:
    OscillatorMedia() = default;
    OscillatorMedia(const OscillatorMedia&) = delete;
    OscillatorMedia& operator=(const OscillatorMedia&) = delete;
    std::vector<OscillatorZone> zones_;
    std::string name_;
    size_t pcmBytes_ = 0, spectralBytes_ = 0;
    double maximumSpectralHopSeconds_ = 0;
};

class OscillatorMediaVoice
{
public:
    static constexpr int maxLayers = 8, maxGrains = 16;
    enum class Mode { Sample, Multisample, Granular, Spectral };
    enum class DensityMode { Hertz, GrainCount, Tempo };
    enum class Window { Hann, Triangle, Rectangle };
    enum class LoopOverride { Asset, None, Forward, Sustain, PingPong };
    struct Stereo { float left = 0, right = 0; };
    struct Controls
    {
        double pitchSemitones = 0, timeRate = 1;
        double grainSeconds = 0.05, density = 8, tempo = 120, densityBeats = 0.25;
        float positionJitter = 0, pitchJitter = 0, panJitter = 0, directionJitter = 0, lengthJitter = 0, gainJitter = 0;
        float windowAmount = 1, windowSkew = 0;
        int grainLimit = maxGrains;
        DensityMode densityMode = DensityMode::GrainCount;
        Window window = Window::Hann;
        LoopOverride loopOverride = LoopOverride::Asset;
        bool jumpStart = false, reverse = false, applyVelocity = true, oneShot = false;
        // Serum 2 sampler profile (measured on the hosted reference): multisamples play 2.84x (+9.07 dB) above Sample mode,
        // velocity tracking t scales by (velocity / 127)^(2 t), releases fall linearly in dB (-80 dB at the release time).
        // Attack/release overrides (seconds, < 0: the zone's own) carry the preset's kParamEnvOverride envelope.
        bool serumProfile = false;
        double attackOverride = -1, releaseOverride = -1;
    };

    // The immutable media owner must outlive this runtime. No shared ownership is changed by these methods.
    // Spectral mode without prepared analysis uses sample playback; independent scan/pitch is unavailable there.
    void start(const OscillatorMedia* media, Mode mode, int note, int velocity, double outputSampleRate, uint32_t seed = 1);
    void reset();
    void release();
    void seek(double normalizedPosition);
    Stereo next(const Controls& controls);
    bool active() const;
    int activeLayers() const;
    int activeGrains() const;
    double position() const;

private:
    struct SpectralOsc
    {
        double cosine = 1, sine = 0, stepCosine = 1, stepSine = 0;
        float left = 0, right = 0, rightCosine = 1, rightSine = 0;
    };
    struct Layer
    {
        const OscillatorZone* zone = nullptr;
        bool active = false, loopEngaged = false, spectral = false;
        MediaLoop loop = MediaLoop::None;
        bool loopWhileHeld = false;
        double position = 0, baseStep = 1, basePitch = 1, gain = 1, velocityGain = 1, envelope = 0, relativeVelocity = 1;
        int direction = 1, spectralCountdown = 0, spectralCount = 0;
        uint64_t played = 0;
        std::array<SpectralOsc, OscillatorSample::spectralPartials> partial{};
    };
    struct Grain
    {
        bool active = false, loopEngaged = false;
        int layer = 0, direction = 1;
        double position = 0, baseStep = 1, duration = 1, elapsed = 0;
        float pan = 0, gain = 1;
    };
    void advance(Layer& layer, double increment);
    void spawnGrain(const Controls& controls, int layer, double initialAge = 0);
    void updateSpectral(Layer& layer, double pitchScale);
    static float randomSigned(uint32_t& seed);
    const OscillatorMedia* media_ = nullptr;
    Mode mode_ = Mode::Sample;
    std::array<Layer, maxLayers> layers_{};
    std::array<Grain, maxGrains> grains_{};
    int layerCount_ = 0, nextGrainLayer_ = 0;
    double outputRate_ = 44100, spawnCountdown_ = 0;
    bool released_ = false, firstGrain_ = true;
    uint32_t seed_ = 1;
};

} // namespace sonora::devices
