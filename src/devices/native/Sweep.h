#pragma once
// Sonora Sweep (PLG-19): risers, downlifters and reverse swells of any length, in time with the track. An instrument: a
// note starts one sweep that lasts Length (in beats at the host tempo) and ends exactly there; the note's pitch sets the
// Tone source's pitch. Over the sweep's progress p (0..1, shaped by Curve):
//
//   Up (riser):        the filter opens Low Hz -> High Hz, the pitch rises by Pitch semitones, the level swells in
//                      (Swell), the stereo width grows to Width, an optional gate speeds up (Rhythm).
//   Down (downlifter): the mirror: the filter closes, the pitch falls, the level fades out, the width narrows.
//
// Sources (PLG-23 adds the generators that need no sample): White Noise and Pink Noise (independent per side for width),
// Supersaw (five detuned saws per chord note), Sine (pure, for sub drops), Whoosh (noise through a moving resonant band),
// Metal (inharmonic square partials and bright noise: cymbal swells), and Sample (a loaded file, prepared on the message
// thread: + a reverb tail (Tail), reversed for Up). Chord stacks notes on Supersaw and Sine; Air adds a bright noise layer
// under any source (it follows the filter). The types() list sets everything but Length and Output in one click. A Sample sweep Up is the "reverse swell": the reversed sample
// and its tail rise in pitch to the original pitch and end on the sample's own attack at the end of the sweep (reverse
// chord going up, reverse crash, reverse vocal). Down plays the sample forwards while the pitch falls.
//
// `renderOffline` runs the same device over a sample into a buffer (the app's "Make Riser / Downlifter from Clip").

#include "devices/NativeDevice.h"

#include <atomic>
#include <cmath>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace sonora::devices
{
class Sweep final : public NativeDevice
{
public:
    enum Param
    {
        Source, Direction, LengthParam, Curve, LowHz, HighHz, Resonance, Pitch, Detune, Swell, Width, Rhythm, Reverse,
        Tail, Output, Chord, Air,
        // PLG-19d, the reverb swell (appended: older sets and VST3 states keep their indices)
        SwellMode, CharacterX, CharacterY, Fit, Damp, Hit, RegionStart, RegionEnd, NumParams
    };
    // How a Sample is prepared: the whole sample reversed with a reverb under it (0.1.32), or only its reverb tail,
    // reversed, rising into the sample (the reverse reverb swell: the dry sound is not in it; Hit plays it at the drop).
    enum SwellKind { ReversedSample, ReverbSwell };
    static constexpr const char* kSwellModeNames[2] = {"Reversed Sample", "Reverb Swell"};
    static constexpr double kMaxSwellSeconds = 12.0; // the part of a sample a reverb swell uses (Region)
    // The first three keep their indices from 0.1.32 (Noise = White Noise, Tone = Supersaw).
    enum SourceKind { Noise, Tone, Sample, Pink, Sine, Whoosh, Metal, kSources };
    static constexpr const char* kSourceNames[kSources] = {"White Noise", "Supersaw", "Sample", "Pink Noise", "Sine", "Whoosh", "Metal"};
    static constexpr int kChords = 5;
    static constexpr const char* kChordNames[kChords] = {"Single", "Octave", "Fifth", "Major", "Minor"};
    enum DirectionKind { Up, Down };
    static constexpr int kLengths = 8;
    static constexpr const char* kLengthNames[kLengths] = {"1 Beat", "2 Beats", "1 Bar", "2 Bars", "4 Bars", "8 Bars", "16 Bars", "32 Bars"};
    static constexpr double kLengthBeats[kLengths] = {1, 2, 4, 8, 16, 32, 64, 128};

    // A ready-made sweep: every parameter but Length and Output.
    struct Type
    {
        const char* name;
        const char* description;
        std::vector<std::pair<int, float>> values;
    };
    static const std::vector<Type>& types();
    // The index of the type the current settings match, or -1.
    int matchingType() const;
    void applyType(int index); // message thread

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
    std::shared_ptr<const juce::AudioBuffer<float>> sampleAudio() const { return sourceAudio_; }
    double sampleRateOfSample() const { return sourceRate_; }
    // Auditions one sweep from the face (or stops the one playing). Message thread; the audio thread starts it.
    void preview();
    static int previewNote(int source) { return source == Sine ? 48 : 60; }
    // The swell's reverb time in seconds now (Fit: 1.2 x the sweep's length; else Tail as 0.4 .. 12 s).
    double swellDecaySeconds() const;
    static double tailSeconds(float tail) { return 0.4 * std::pow(30.0, (double) tail); }
    bool reverbSwell() const;   // Source is Sample and SwellMode is Reverb Swell
    double hitSeconds() const;  // the length of the Region (what Hit plays at the drop), 0 without a sample
    // Renders one sweep at the current settings and tempo (48 kHz, plus the Hit after the drop) into a 24-bit WAV.
    bool renderToFile(const juce::File& file, std::string& error) const;
    // The source's waveform (min/max of `bins` bins over the whole file, -1..1) for the face's region strip.
    std::vector<std::pair<float, float>> sourceOverview(int bins) const;
    const std::string& sampleName() const { return sampleName_; }
    bool hasSample() const { return sourceAudio_ != nullptr; }

    double tempo() const { return host_.tempo > 0 ? host_.tempo : 120.0; }
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
    // `extraSeconds` renders past the sweep's end (the Hit after the drop).
    static juce::AudioBuffer<float> renderOffline(const std::vector<float>& parameterValues, const juce::AudioBuffer<float>* source,
                                                  double sampleRate, double tempo, int note = 60, double sourceRate = 0,
                                                  double extraSeconds = 0);

private:
    struct Prepared
    {
        juce::AudioBuffer<float> audio; // at the device sample rate, reversed for Up when Reverse is on
        juce::AudioBuffer<float> hit;   // the Region forwards at the device rate (Hit)
        bool reversed = false;
    };
    struct Owned
    {
        std::unique_ptr<const Prepared> prepared;
        double supersededMs = 0; // when a newer one replaced it (0: still the newest)
    };
    std::vector<float> preparationKey() const;
    void prepareSample();
    void collectGarbage();

    double sampleRate_ = 44100.0;
    HostInfo host_;
    // sweep state (audio thread)
    bool active_ = false;
    double elapsed_ = 0, duration_ = 1;
    int note_ = 60;
    static constexpr int kVoices = 20; // 5 saws x 4 chord notes
    double phases_[kVoices]{};
    double metal_[6]{};
    float pink_[2][7]{};
    float airState_[2]{};
    double lfo_ = 0;
    int previewSeen_ = 0;
    std::atomic<int> previewRequests_{0};
    double readPos_ = 0, gatePhase_ = 0;
    double hitPos_ = -1; // the Hit's read position (-1: not playing)
    float svf_[2][2]{};
    uint32_t noise_[2] = {0x1234567u, 0x89abcdefu};
    float release_ = 0; // short fade after the end / a retrigger
    float stopFade_ = 1.0f;
    bool stopping_ = false; // a preview click stops the sweep playing (short fade)
    float metalHp_[2]{};
    std::atomic<float> progress_{-1};

    // sample: source audio and its prepared version, swapped lock-free (the audio thread acknowledges what it uses)
    std::shared_ptr<const juce::AudioBuffer<float>> sourceAudio_;
    double sourceRate_ = 44100.0;
    std::string samplePath_, sampleName_;
    std::vector<float> preparedKey_;
    std::atomic<const Prepared*> pending_{nullptr};
    std::atomic<const Prepared*> inUse_{nullptr};
    const Prepared* current_ = nullptr; // audio thread
    std::vector<Owned> owned_;
};
} // namespace sonora::devices
