#pragma once
// Sonora Sweep (PLG-19): risers, downlifters and reverse swells of any length, in time with the track. An instrument: a
// note starts one sweep that lasts Length (in beats at the host tempo) and ends exactly there; the note's pitch sets the
// Tone source's pitch. Over the sweep's progress p (0..1, shaped by Curve):
//
//   Up (riser):        the filter opens Low Hz -> High Hz, the pitch rises by Pitch semitones, the level swells in
//                      (Swell), the stereo width grows to Width, an optional gate speeds up (Rhythm).
//   Down (downlifter): the mirror: the filter closes, the pitch falls, the level fades out, the width narrows.
//
// Sources: Noise (white, independent per side for width), Tone (five detuned saws), Sample (a loaded file, prepared on the
// message thread: + a reverb tail (Tail), reversed for Up). A Sample sweep Up is the "reverse swell": the reversed sample
// and its tail rise in pitch to the original pitch and end on the sample's own attack at the end of the sweep (reverse
// chord going up, reverse crash, reverse vocal). Down plays the sample forwards while the pitch falls.
//
// `renderOffline` runs the same device over a sample into a buffer (the app's "Make Riser / Downlifter from Clip").

#include "devices/NativeDevice.h"

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace sonora::devices
{
class Sweep final : public NativeDevice
{
public:
    enum Param
    {
        Source, Direction, LengthParam, Curve, LowHz, HighHz, Resonance, Pitch, Detune, Swell, Width, Rhythm, Reverse,
        Tail, Output, NumParams
    };
    enum SourceKind { Noise, Tone, Sample };
    enum DirectionKind { Up, Down };
    static constexpr int kLengths = 8;
    static constexpr const char* kLengthNames[kLengths] = {"1 Beat", "2 Beats", "1 Bar", "2 Bars", "4 Bars", "8 Bars", "16 Bars", "32 Bars"};
    static constexpr double kLengthBeats[kLengths] = {1, 2, 4, 8, 16, 32, 64, 128};

    Sweep();
    ~Sweep() override;
    std::string name() const override { return "Sonora Sweep"; }
    bool isInstrument() const override { return true; }
    void prepare(double sampleRate, int maxBlockSize) override;
    void reset() override;
    void process(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void setHostInfo(const HostInfo& info) override { host_ = info; }
    std::vector<UiGroup> uiGroups() const override;

    // ---- the Sample source (message thread). Loads `path` (any format JUCE reads, up to 60 s) and prepares it with the
    // current Tail and Reverse; false (and `error`) when it cannot be read. setSampleAudio takes audio directly.
    bool loadSample(const std::string& path, std::string& error);
    void setSampleAudio(const juce::AudioBuffer<float>& audio, double sampleRate, const std::string& name);
    void clearSample();
    // Prepares again when Tail or Reverse changed since the last preparation (the face calls it after edits).
    void refreshSample();
    const std::string& samplePath() const { return samplePath_; }
    const std::string& sampleName() const { return sampleName_; }
    bool hasSample() const { return sourceAudio_ != nullptr; }

    // Sweep length in seconds for the current Length and tempo.
    double lengthSeconds() const;
    // ---- face
    float progress() const { return progress_.load(std::memory_order_relaxed); } // 0..1, -1 idle
    // Progress p (0..1) -> shaped progress (Curve), and the values the sweep has there (for the face's curves).
    float shaped(float p) const;
    float cutoffAt(float p) const;   // Hz
    float pitchAt(float p) const;    // semitones from the note / the sample's pitch
    float levelAt(float p) const;    // 0..1
    float gateRateAt(float p) const; // gates per beat (0: off)

    // Renders one sweep of `seconds` over `source` (or the device's own Noise/Tone when null) at `tempo`.
    static juce::AudioBuffer<float> renderOffline(const std::vector<float>& parameterValues, const juce::AudioBuffer<float>* source,
                                                  double sampleRate, double tempo, int note = 60);

private:
    struct Prepared
    {
        juce::AudioBuffer<float> audio; // at the device sample rate, reversed for Up when Reverse is on
        bool reversed = false;
    };
    void prepareSample();
    void collectGarbage();

    double sampleRate_ = 44100.0;
    HostInfo host_;
    // sweep state (audio thread)
    bool active_ = false;
    double elapsed_ = 0, duration_ = 1;
    int note_ = 60;
    double phases_[5]{};
    double readPos_ = 0, gatePhase_ = 0;
    float svf_[2][2]{};
    uint32_t noise_[2] = {0x1234567u, 0x89abcdefu};
    float release_ = 0; // short fade after the end / a retrigger
    std::atomic<float> progress_{-1};

    // sample: source audio and its prepared version, swapped lock-free (the audio thread acknowledges what it uses)
    std::shared_ptr<const juce::AudioBuffer<float>> sourceAudio_;
    double sourceRate_ = 44100.0;
    std::string samplePath_, sampleName_;
    float preparedTail_ = -1, preparedReverse_ = -1;
    std::atomic<const Prepared*> pending_{nullptr};
    std::atomic<const Prepared*> inUse_{nullptr};
    const Prepared* current_ = nullptr; // audio thread
    std::vector<std::unique_ptr<const Prepared>> owned_;
};
} // namespace sonora::devices
