#pragma once
// Sonora Simpler (.als tag "OriginalSimpler", ROADMAP DEV-4 / DEV-4c). The Sonora Sampler ("MultiSampler", DEV-14b) is
// devices::Sampler (Sampler.h), which shares this engine's laws (parameterInfos, simplerFilterStages, readSample...); the
// `multisampler` constructor flag only remains for older tests.
//
// Implemented: sample zones (key / velocity ranges, root key, detune, per-zone volume and pan),
// Classic, One-Shot and onset-Slice playback, the Simpler sample region and loop (Start / Length /
// Loop / Fade, constant-power or linear pre-loop crossfades; Sampler uses the zone's sustain loop),
// polyphony with oldest-voice stealing, glide, pitch (transpose, detune, pitch bend, pitch envelope),
// sloped ADSR envelopes, one-shot fades, velocity / key -> volume, equal-power pan, and the Clean
// filter circuit (LP / HP / BP / Notch / Morph, 12 or 24 dB, resonance, key / velocity / envelope / LFO
// modulation), Hz/beat LFO to pitch / volume / pan / filter.
// The DSP laws (velocity, envelope slopes, pan, filter Q and stage structure, loop region, playback
// kernel) were measured against the reference with tools/parity_probe (DEV-22).
// Not yet (ROADMAP DEV-4b / DEV-22): warping, aux envelope, shaper, filter circuit models (OSR / MS2 /
// SMP / PRD use Clean), loop-point snapping, exact interpolation away from the original pitch, spread.

#include "devices/NativeDevice.h"
#include "dsp/Dsp.h"
#include "devices/native/Biquad.h"
#include "engine/SampleCache.h"

#include <array>
#include <memory>

namespace sonora::devices
{

// The filter of the shared sampling engine (Simpler and Sampler, DEV-22 measurements): `type` 0 LP, 1 HP, 2 BP, 3 Notch,
// 4 Morph; `circuit` 0 Clean, 1 OSR, 2 MS2, 3 SMP, 4 PRD; `res` 0..1.25; `steep` = 24 dB; `morph` 0..1. Fills 1 or 2
// biquad stages for the cutoff `freq` and returns their count.
struct SimplerFilterSettings
{
    int type = 0, circuit = 0;
    double res = 0, morph = 0;
    bool steep = true;
};
int simplerFilterStages(const SimplerFilterSettings& settings, double sampleRate, double freq, BiquadCoeffs* out);

class Simpler final : public NativeDevice
{
public:
    enum Param
    {
        Transpose,       // semitones
        Detune,          // cents
        Volume,          // dB
        Pan,
        VelToVolume,     // 0..1
        Attack,          // ms
        Decay,           // ms
        Sustain,         // linear 0..1
        Release,         // ms
        FadeIn,          // ms (One-Shot)
        FadeOut,         // ms (One-Shot)
        FilterOn,
        FilterType,      // 0 LP, 1 HP, 2 BP, 3 Notch, 4 Morph (LP)
        FilterFreq,      // Hz
        FilterRes,       // 0..1.25
        FilterSlope,     // 0 = 12 dB, 1 = 24 dB
        FilterKeyTrack,  // 0..1
        FilterEnvOn,
        FilterEnvAmount, // semitones
        FilterAttack,    // ms
        FilterDecay,     // ms
        FilterSustain,   // 0..1
        FilterRelease,   // ms
        KeyZoneShift,    // semitones
        Voices, Retrigger, LfoOn, LfoRate, LfoShape, LfoPitch, LfoVolume, LfoPan, LfoSync, LfoBeat,
        // DEV-22 (appended so existing indices stay stable)
        LoopOn, SampleStartMod, SampleLengthMod, LoopLengthMod, LoopFadeMod, // fractions of the zone / region / loop
        AttackSlope, DecaySlope, ReleaseSlope, FilterAttackSlope, FilterDecaySlope, FilterReleaseSlope,
        FilterMorph, FilterVelMod, FilterLfoMod, FilterDrive,
        GlideMode, GlideTime, VolKeyScale, PanKeyScale, PanRandom,
        PitchEnvOn, PitchEnvAmount, PitchAttack, PitchDecay, PitchSustain, PitchRelease,
        PitchAttackSlope, PitchDecaySlope, PitchReleaseSlope,
        ConstPowerFade, Interpolation, LfoAttack, LfoRetrigger, Spread, OneShotGate, Snap,
        FilterCircuit, FilterCircuitBp, // circuit models (measured linear laws + drive saturation, DEV-22a)
        LfoOffset, LfoKeyScale, AmpLoopMode, AmpLoopTime, // reference Controls view (DEV-22a)
        AmpRepeatTime, // amplitude envelope Beat / Sync repeat (beat division enum)
        SlicePlayback, // Slicing mode Playback: 0 Mono, 1 Poly, 2 Thru (SimplerSlicing/PlaybackMode, DEV-4c)
        NumParams
    };

    enum class Mode
    {
        Classic = 0,
        OneShot = 1,
        Slice = 2
    };

    struct Zone
    {
        model::SamplePart part;
        std::shared_ptr<engine::SampleSlot> slot; // may still be loading
    };

    Simpler(std::vector<Zone> zones, Mode mode, int pitchBendRange, bool multisampler = false);

    std::string name() const override { return multisampler_ ? "Sonora Sampler" : "Sonora Simpler"; }
    bool isInstrument() const override { return true; }
    void prepare(double sampleRate, int maxBlockSize) override;
    void reset() override;
    void setHostInfo(const HostInfo& host) override { tempo_ = host.tempo; }
    void process(juce::AudioBuffer<float>& audio, juce::MidiBuffer& midi) override;

    std::vector<int> displayParameters() const override
    {
        if (multisampler_)
        {
            std::vector<int> result;
            for (int i = 0; i < NumParams; ++i) result.push_back(i);
            return result;
        }
        return {Transpose, Detune, Volume, Pan, Attack, Decay, Sustain, Release, FilterFreq, FilterRes, FilterEnvAmount, VelToVolume};
    }

    int activeVoices() const { return activeVoices_.load(std::memory_order_relaxed); }
    const std::vector<Zone>& zones() const { return zones_; }
    Mode playbackMode() const { return mode_; }
    int pitchBendRange() const { return bendRange_; }
    double sampleRate() const { return sampleRate_; }
    // The filter's biquad stages at `freq` (the unmodulated response when freq = Frequency); returns 1 or 2 stages.
    int filterStages(double freq, BiquadCoeffs* out) const;
    int filterCircuit() const; // circuit of the current filter type: 0 Clean, 1 OSR, 2 MS2, 3 SMP, 4 PRD

    // The part of a zone's sample a note plays (frames), from the zone's region and Start / Length / Loop / Fade, as
    // the voice computes it; the face draws the same region. Allocation free (audio and message thread).
    struct Region
    {
        double start = 0, end = 0;               // played region; end is exclusive (after Snap)
        bool loop = false;                       // Classic with Loop on
        double loopStart = 0, loopEnd = 0, fade = 0; // loop and the crossfade length before the loop end
    };
    Region region(const Zone& zone, const engine::SampleData& sample) const;
    // Slicing (DEV-4c): the start frames of the slices inside [start, end), in order, by the zone's Slice By setting
    // (Transient: the reference's saved transients filtered by Sensitivity, else Sonora's onsets; Beat: every Division
    // of the sample's beat grid; Region: Regions equal parts; Manual: the manual slice points). Returns the count.
    static constexpr int kMaxSlices = 128;
    int slices(const Zone& zone, const engine::SampleData& sample, double start, double end, double* out, int max) const;
    static double divisionBeats(int division); // SlicingBeatGrid enum -> beats

    // Shared with the Sonora Sampler (DEV-14b), which runs the same engine with more of the reference's controls.
    static std::vector<ParameterInfo> parameterInfos(); // the list above, by Param index (XML paths, ranges, defaults)
    // The measured playback kernel (Kaiser-windowed sinc, stretched by `step` above the source rate).
    static float readSample(const float* d, int len, double pos, double step);
    static float saturate(float x, int circuit);  // a non-Clean circuit's drive stage
    static int voiceCount(int index);             // Globals/NumVoices enum -> voices
    static double lfoBeatQuarters(int index);     // LFO BeatRate enum -> quarter notes
    static double repeatQuarters(int index);      // envelope RepeatTime enum -> quarter notes
    // Envelope slope law (DEV-22): |slope| < 1 blends linear with exp(-kEnvSlopeRate x); slope +1 approaches the target
    // dB-linearly down to kEnvSlopeFloor (kPitchSlopeFloor for the pitch envelope).
    static constexpr double kEnvSlopeRate = 9.115, kEnvSlopeFloor = 6.3e-5, kPitchSlopeFloor = 1.23e-4;

private:
    static constexpr int kMaxVoices = 32;

    // One ADSR envelope (amplitude, filter or pitch). Each stage runs from `from` to its target over `length`
    // samples along the measured slope curve: blend of linear and exp(-9.115 x) for |slope| < 1, and for
    // slope +1 a dB-linear approach to a fixed -84 dB floor (see stageValue()).
    struct Env
    {
        enum class Stage { Idle, Attack, Decay, Sustain, Release, LoopFall };
        Stage stage = Stage::Idle;
        double age = 0, nextRepeat = 0; // samples since note-on and the next Beat / Sync restart
        double floor = 6.3e-5;          // slope +1 stages approach the target dB-linearly down to this distance
        float level = 0.0f, from = 0.0f;
        double elapsed = 0, length = 1, term = 1, ratio = 1; // samples into the stage; running exponential term
    };
    struct EnvShape
    {
        float attack = 1, decay = 1, sustain = 1, release = 1; // samples, samples, level, samples
        float attackSlope = 0, decaySlope = 1, releaseSlope = 1;
        int loopMode = 0;                 // amplitude only: 0 None, 1 Loop, 2 Beat, 3 Sync, 4 Trigger
        float loopTime = 1, repeat = 1;   // samples: Loop fall time, Beat / Sync restart period
    };

    struct Voice
    {
        bool active = false;
        bool released = false, keyDown = true;
        int note = -1;
        float velocity = 0.0f; // 0..1
        const Zone* zone = nullptr;
        const engine::SampleData* sample = nullptr;
        double pos = 0.0;  // frames (left channel; the right one reads posR, which differs only with Spread)
        double posR = 0.0;
        double baseStep = 1.0; // frames per output sample before pitch bend / modulation
        double glideSemis = 0.0, glideStep = 0.0; // remaining glide offset (semitones) and its per-sample change
        int direction = 1, directionR = 1;
        double startFrame = 0, endFrame = 0;
        double loopStart = 0, loopEnd = 0, fadeLength = 0; // loopEnd <= loopStart: no loop
        int loopMode = 0;                                  // 1 forward, 2 back-and-forth (Sampler zones)
        float gainL = 1.0f, gainR = 1.0f;
        float fadeIn = 0.0f; // one-shot fade-in gain
        float filterKeySemis = 0.0f; // key / velocity offsets of the cutoff
        long long age = 0;
        double lfoAge = 0; // samples since note-on (LFO attack)
        double lfoRateScale = 1; // LFO Key: rate x (1 + key 2^((note - 60) / 12))
        float lfoSmooth[2] = {0, 0}; // two one-pole stages smoothing the LFO output (measured square edges)
        dsp::Lfo lfo;
        Env amp, filter, pitch;
        SvfCoeffs coeffs[2]; // state-variable form: cutoff automation / modulation steps must not click
        SvfState f[4];       // [L stage1, R stage1, L stage2, R stage2]
        int filterCountdown = 0, filterStageCount = 1;
    };

    void noteOn(int note, float velocity);
    void noteOff(int note);
    void startVoice(const Zone& zone, int note, float velocity);
    void render(juce::AudioBuffer<float>& audio, int from, int to);
    static void startStage(Env& e, Env::Stage stage, float target, double length, float slope);
    static float stepEnv(Env& e, bool released, const EnvShape& shape);
    EnvShape envShape(int attack, int decay, int sustain, int release, int attackSlope, int decaySlope, int releaseSlope) const;
    void updateFilter(Voice& v, float lfo);
    double snappedLoopEnd(const engine::SampleData& sample, double loopStart, double loopEnd) const;
    void startGlide(Voice& v, int fromNote, bool legato);

    std::vector<Zone> zones_;
    Mode mode_;
    bool multisampler_ = false;
    int bendRange_;
    double sampleRate_ = 44100.0, tempo_ = 120;
    float bendSemis_ = 0.0f;
    bool sustainPedal_ = false;
    long long counter_ = 0;
    int lastNote_ = -1;
    dsp::Lfo freeLfo_; // LFO with Retrigger off runs freely across notes
    unsigned random_ = 22222u;
    std::array<Voice, kMaxVoices> voices_{};
    std::atomic<int> activeVoices_{0};
};

} // namespace sonora::devices
