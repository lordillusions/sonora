#pragma once
// Immutable wavetable media and band-limited banks (DEV-18d).
// Loading/generation allocates and belongs on the message/load thread before graph publication.

#include <juce_audio_formats/juce_audio_formats.h>

#include <memory>
#include <span>
#include <string>
#include <vector>

namespace sonora::devices
{

class WavetableData;
// How a table position selects frames. Continuous crossfades at position * (frames - 1) (own tables). The imported
// Serum grid always snaps to a cell (measured): Steps has one cell per frame,
// Interpolated256 has 256 cells crossfaded at cell * (frames - 1) / 255.
enum class WavetableFrameLayout { Continuous = 0, Steps = 1, Interpolated256 = 2 };
// Load-time shaping: the samples are scaled by gain (divided by the global peak first when normalizePeak is set).
struct WavetableShaping
{
    WavetableFrameLayout layout = WavetableFrameLayout::Continuous;
    float gain = 1;
    bool normalizePeak = false;
};
struct WavetableLoadResult
{
    std::shared_ptr<const WavetableData> data;
    std::string error;
    explicit operator bool() const { return data != nullptr; }
};

class WavetableData final
{
public:
    // Octave16Approx is an independently generated research profile (DEV-18d),
    // not an assertion of matching another instrument's interpolation kernel.
    enum class PlaybackProfile { Dense12 = 0, Octave16Approx = 1, Octave16Minimax = 2 };

    ~WavetableData() = default;
    static constexpr int nativeFrameLength = 2048, maxFrames = 256, maxChannels = 2;
    static constexpr int minFrameLength = 64, maxFrameLength = 2048;
    static constexpr size_t maxBankBytes = 64 * 1024 * 1024;

    // Samples are interleaved: [L0,R0,L1,R1,...]. Each channel must contain a whole number of frames.
    // The observed native table has 2048 samples/frame. Smaller power-of-two own tables are also accepted.
    static WavetableLoadResult fromSamples(std::span<const float> samples, int numChannels,
                                           int frameLength = nativeFrameLength, double sampleRate = 44100,
                                           std::string name = {}, PlaybackProfile profile = PlaybackProfile::Dense12,
                                           WavetableShaping shaping = {});
    static WavetableLoadResult fromFile(const juce::File& file, int frameLength = nativeFrameLength,
                                       PlaybackProfile profile = PlaybackProfile::Dense12, WavetableShaping shaping = {});

    PlaybackProfile playbackProfile() const { return profile_; }
    WavetableFrameLayout frameLayout() const { return layout_; }
    WavetableLoadResult withPlaybackProfile(PlaybackProfile profile) const;
    WavetableLoadResult withProfile(PlaybackProfile profile) const { return withPlaybackProfile(profile); }
    // Advisory only: callers prepare and render the oversampling path themselves.
    int recommendedRateFactor() const { return profile_ == PlaybackProfile::Dense12 ? 1 : 2; }

    int channels() const { return channels_; }
    int frames() const { return frames_; }
    int frameLength() const { return frameLength_; }
    int mipLevels() const { return mipLevels_; }
    int harmonicLimit(int mip) const;
    int mipLength(int mip) const;
    size_t storageBytes() const;
    double sampleRate() const { return sampleRate_; }
    const std::string& name() const { return name_; }
    const std::vector<float>& originalSamples() const { return original_; }

    // Audio-thread safe. Increment is cycles per INTERNAL sample, including the
    // caller's warp bandwidth estimate. This class never silently divides by a rate factor.
    int mipForIncrement(double increment) const;
    float sample(float position, double phase, int mip, int channel = 0) const;

private:
    WavetableData() = default;
    WavetableData(const WavetableData&) = delete;
    WavetableData& operator=(const WavetableData&) = delete;
    void generate();
    void generateOctaveBands();
    void configureBands(int upperBandsPerOctave);
    void configureOctaveBands();
    size_t offset(int channel, int frame, int mip) const;
    struct Band { int harmonics = 1, length = minFrameLength; size_t offset = 0; };
    std::vector<float> original_, bank_;
    std::vector<Band> bands_;
    std::string name_;
    size_t frameStride_ = 0;
    int channels_ = 1, frames_ = 1, frameLength_ = nativeFrameLength, mipLevels_ = 0;
    double sampleRate_ = 44100;
    PlaybackProfile profile_ = PlaybackProfile::Dense12;
    WavetableFrameLayout layout_ = WavetableFrameLayout::Continuous;
    int frameCells_ = 0; // 0: continuous position
    float frameStep_ = 1;
    const float* minimaxKernel_ = nullptr;
};

} // namespace sonora::devices
