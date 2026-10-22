#pragma once
// Independently authored native effect graph (DEV-18d). The public type codes are observed;
// algorithms and controls here are Sonora designs pending reference-audio calibration.
// Configure/IR loading allocates off audio. Processing uses frozen ownership and prepared storage.

#include <juce_audio_formats/juce_audio_formats.h>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sonora::devices
{

class ConvolutionIR;
struct ConvolutionIRResult
{
    std::shared_ptr<const ConvolutionIR> data;
    std::string error;
    explicit operator bool() const { return data != nullptr; }
};

class ConvolutionIR final
{
public:
    static constexpr int maxFrames = 1048576; // Bounded original PCM: at most 8 MiB for stereo float32.
    static ConvolutionIRResult fromSamples(std::span<const float> interleaved, int channels,
                                          double sampleRate, std::string name = {});
    static ConvolutionIRResult fromFile(const juce::File& file);
    int channels() const { return channels_; }
    int frames() const { return static_cast<int>(samples_.size() / static_cast<size_t>(channels_)); }
    double sampleRate() const { return sampleRate_; }
    const std::string& name() const { return name_; }
    const std::vector<float>& originalSamples() const { return samples_; }

private:
    ConvolutionIR() = default;
    ConvolutionIR(const ConvolutionIR&) = delete;
    ConvolutionIR& operator=(const ConvolutionIR&) = delete;
    std::vector<float> samples_;
    std::string name_;
    int channels_ = 1;
    double sampleRate_ = 44100;
};

class WavetableEffects final
{
public:
    using ModuleId = uint32_t;
    enum class Type : int
    {
        Distortion = 0, Flanger = 1, Phaser = 2, Chorus = 3, Delay = 4, Compressor = 5,
        Reverb = 6, Eq = 7, Filter = 8, HyperDimension = 9, FrequencyShift = 10,
        Convolution = 11, Utility = 12, Unknown = 13, Split3 = 14
    };
    static constexpr int busCount = 3, maxBusModules = 16, maxBandModules = 8;
    static constexpr int maxModules = 128, maxDepth = 3, maxParameters = 40;
    static constexpr int mixParameter = 0, outputParameter = 1, convolutionPartition = 128;
    // Independently authored nonuniform FFT convolution: a short low-latency head plus a larger tail.
    // Source IR and resampled prepared length are bounded independently; all stages count toward maxPreparedBytes.
    static constexpr int convolutionHeadFrames = 2048, convolutionTailPartition = 1024;
    static constexpr int maxResampledConvolutionFrames = 4 * 1024 * 1024;
    static constexpr size_t maxPreparedBytes = 256 * 1024 * 1024;
    struct ParameterDescriptor
    {
        std::string_view id, label, unit;
        float minimum = 0, maximum = 1, initial = 0;
        bool discrete = false;
        std::span<const std::string_view> choices{};
    };
    struct Module
    {
        ModuleId id = 0;
        Type type = Type::Utility;
        bool enabled = true;
        std::vector<float> values; // Plain values in descriptor order. Missing trailing values use native defaults.
        std::shared_ptr<const ConvolutionIR> impulse;
        std::array<std::vector<Module>, 3> bands; // Split3 only, independently ordered low/mid/high chains.
    };
    struct Routing
    {
        float mainToBus1 = 0, mainToBus2 = 0, bus1ToMain = 1, bus2ToMain = 1, mainGain = 1;
        float directGain = 1; // Scales post-Main returns; pre returns still pass through Main FX.
    };
    enum class MixLaw { EqualPower, Shaped };
    struct Configuration
    {
        std::array<std::vector<Module>, busCount> buses;
        Routing routing;
        MixLaw mixLaw = MixLaw::EqualPower; // Preserves initial own graphs; Shaped is the measured scalar reconstruction.
        bool alignLatency = true; // Prepared dry/band/bus delays. False exposes uncompensated paths for reference work.
        std::array<bool, 2> returnBeforeMain{false, false}; // Aux1/2 merge before Main FX; legacy graphs merge afterward.
    };
    struct StereoBuffer { float* left = nullptr; float* right = nullptr; };

    static std::optional<Type> typeFromCode(int code);
    static std::string_view typeName(Type type);
    static std::span<const ParameterDescriptor> parameterDescriptors(Type type);
    static int parameterIndex(Type type, std::string_view id);

    WavetableEffects();
    ~WavetableEffects();
    WavetableEffects(const WavetableEffects&) = delete;
    WavetableEffects& operator=(const WavetableEffects&) = delete;
    // Only before graph publication, or with processing explicitly suspended. Failure preserves the previous graph.
    bool prepare(const Configuration& configuration, double sampleRate, std::string& error);
    void reset();
    bool setParameter(ModuleId id, int parameter, float plainValue);
    float getParameter(ModuleId id, int parameter) const;
    bool setEnabled(ModuleId id, bool enabled);
    bool enabled(ModuleId id) const;
    std::optional<Type> moduleType(ModuleId id) const;
    void setTempo(double beatsPerMinute);
    bool setRouting(const Routing& routing);
    Routing routing() const;
    // Independent any-thread atomic writes; index order is Main->Bus1/2, Bus1/2->Main, MainGain, DirectGain.
    // Return/direct gains (indices2/3/5) span0..4; sends and MainGain retain the legacy0..2 range.
    bool setRoutingParameter(int parameter, float plainValue);
    float getRoutingParameter(int parameter) const;
    // Message/load thread only; snapshot copies structure/IR ownership and reads current atomic controls.
    Configuration snapshotConfiguration() const;
    std::span<const ModuleId> moduleIds() const;
    // Maximum intrinsic configured path latency, including serial pre-return Aux -> Main chains.
    // Convolution=128, FrequencyShift=31; shorter paths are aligned at each merge when enabled.
    // This query remains intrinsic when alignLatency=false; that mode deliberately mixes unaligned paths.
    int latencySamples() const;
    size_t preparedBytes() const;

    // All three stereo buffers must be non-null and disjoint, with at least `frames` samples.
    // Bus1/2 receive their existing source input plus sends from the original Main input. Configured
    // pre returns merge before Main FX; legacy post returns merge afterward. Their buffers retain
    // individual processed outputs, aligned to the respective merge point when alignment is enabled.
    // DirectGain scales only the post-return sum; MainGain scales the final combined output.
    // A zero Mix preserves that module's input, delayed by its fixed configured latency when alignment is on.
    // enabled=false freezes its wet DSP/tails while the prepared dry-alignment delay keeps advancing.
    // EqualPower uses cos/sin. Shaped uses wet=sin²(pi*Mix/2), dry=1-wet²; both can boost correlated input.
    void process(const std::array<StereoBuffer, busCount>& buses, int frames);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace sonora::devices
