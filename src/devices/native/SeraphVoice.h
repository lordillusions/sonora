#pragma once
// Sonora Seraph (PLG-27): one singer. A pitch model (tuning offset, drift, jitter, shimmer, vibrato with delay and
// fade-in, scoop, glide), the glottal source with pitch-synchronous breath noise, and five parallel formant resonators
// (TPT state-variable band-passes, alternating signs) updated every 16 samples. Audio thread only after prepare():
// no allocation, no locks. docs/SERAPH.md.

#include "devices/native/SeraphGlottis.h"
#include "devices/native/SeraphReal.h"

#include <cmath>
#include <cstdint>
#include <vector>

namespace sonora::devices::seraph
{
enum OnsetKind { OnsetSoft, OnsetH, OnsetM, OnsetN, OnsetL, OnsetY, OnsetW, kOnsetKinds };

// What makes this singer different: drawn from a hash of (seed, note, singer) when the note starts.
struct Traits
{
    float tuneCents = 0;     // fixed tuning offset
    float delayMs = 0;       // onset delay
    float throat = 1;        // all formants x this
    float vibRate = 1;       // x the vibrato rate
    float vibDepth = 1;      // x the vibrato depth
    float vibPhase = 0;      // 0..1
    float vowelOffset = 0;   // in vowel steps
    float pan = 0;           // -1..1
    float breath = 1;        // x the breath
    float brightDb = 0;      // added to Bright
    float attack = 1;        // x the attack time
    float scoopMs = 60;      // time to reach the pitch from the scoop
    float motionRate = 1;    // x the vowel motion rate
    float onsetMs = 0.5f;    // 0..1: where in its range this singer's consonant lasts
    uint32_t seed = 1;       // the singer's own random stream (drift, jitter, shimmer, breath, motion)
};

// What the device sets for every singer once per block.
struct Shared
{
    double sampleRate = 44100.0;
    const GlottalTables* tables = nullptr;
    float vowel = 0, hum = 0, size = 1, brightDb = 0, motion = 0, motionRate = 0.2f;
    float tension = 0.5f, breath = 0.2f, effort = 0.5f;
    float vibratoCents = 12, vibratoRate = 5.5f, vibratoDelayMs = 300, driftCents = 8, human = 0.5f, scoopCents = 0;
    float attackMs = 120, releaseMs = 600, swell = 0, dynamics = 0.6f;
    float bendSemitones = 0, modVibratoCents = 0;
};

struct StartInfo
{
    int type = Tenor;         // voice type (the formant table and its loudness)
    bool boys = false;        // soprano table, formants +12 %
    float pitch = 60;         // MIDI note (fractional)
    float velocity = 0.8f;    // 0..1
    float vowelStep = 0;      // offset of this note's vowel from the Vowel setting (Vowel Steps), mirrored inside 0..4
    int onset = OnsetSoft;
    float gain = 1;           // part level (organum, ison)
    bool drone = false;       // the ison: vowel towards U, half hummed
    const RealVoice* real = nullptr; // Real voices (PLG-27b): this recording replaces the source and the formants
    Traits traits;
};

class SeraphVoice
{
public:
    static constexpr int kControlInterval = 16;
    static constexpr float kLevel = 0.04f; // RMS of one singer at full velocity before the section's gain

    void prepare(double sampleRate)
    {
        sampleRate_ = sampleRate > 0 ? sampleRate : 44100.0;
        hpCoef_ = (float) std::exp(-2.0 * 3.14159265358979 * 1500.0 / sampleRate_);
        // Real: grains are laid this far ahead of the output (half the longest grain: 70 Hz read at 0.8 x)
        realDelay_ = (int) std::ceil(2.0 * sampleRate_ / 70.0 / 0.6) + 4; // grains span four periods
        int size = 1;
        while (size < realDelay_ * 4) size <<= 1;
        ring_.assign((size_t) size, 0.0f);
        ringMask_ = size - 1;
        reset();
    }
    // Starts a note (a sounding singer is faded out quickly first: no click when it is stolen).
    void start(const StartInfo&);
    // Moves to a new pitch over `glideMs` (legato, chant), without a new attack.
    void glideTo(float pitch, float glideMs);
    // A new syllable without a new attack (chant phrase notes): the onset plays again, the vowel step changes.
    void rearticulate(int onset, float vowelStep);
    void release();
    // Fades out over a few milliseconds and stops (stealing).
    void kill();
    void reset();

    bool idle() const { return stage_ == Stage::Idle; }
    bool released() const { return stage_ == Stage::Releasing || stage_ == Stage::Fading || stage_ == Stage::Idle; }
    bool fading() const { return stage_ == Stage::Fading; }

    // Adds n samples to left/right.
    void render(float* left, float* right, int n, const Shared&);

    // ---- face and tests
    float level() const { return idle() ? 0.0f : shown_; } // 0..1 (envelope x velocity x part gain)
    float pan() const { return info_.traits.pan; }
    int type() const { return info_.type; }
    bool boys() const { return info_.boys; }
    const RealVoice* real() const { return idle() ? nullptr : info_.real; }
    double realPosition() const { return srcPos_; } // Real: the read position in the recording (samples)
    // Real: the output latency of the grains (samples); the device compensates nothing, singers start this late.
    int realLatency() const { return realDelay_; }
    float targetPitch() const { return info_.pitch; }
    float currentHz() const { return (float) (increment_ * sampleRate_); }

private:
    enum class Stage { Idle, Delay, Sounding, Releasing, Fading };
    struct SmoothNoise
    {
        float a = 0, b = 0, phase = 0;
        float next(float increment, uint32_t& rng);
    };
    void begin(const StartInfo&);
    void emitGrain(const Shared&);
    void control(const Shared&);
    float uniform(); // -1..1
    float gauss();   // about unit variance

    Stage stage_ = Stage::Idle;
    StartInfo info_, pending_;
    bool hasPending_ = false;
    double sampleRate_ = 44100.0;
    int delaySamples_ = 0;
    int countdown_ = 0;
    int64_t age_ = 0;     // samples since voicing started
    int64_t onsetAge_ = 0; // samples since the onset started (re-articulations restart it)
    int onset_ = OnsetSoft;
    float vowelStep_ = 0, vowelStepFrom_ = 0;

    // pitch
    double phase_ = 0, increment_ = 0;
    float periodScale_ = 1, shimmer_ = 1;
    float glideFrom_ = 0, glideMs_ = 0, glideAge_ = 0;
    bool gliding_ = false;
    float vibratoPhase_ = 0;
    SmoothNoise driftSlow_, driftFast_, motion_;
    uint32_t rng_ = 1;

    // source
    const float* tableA_ = nullptr;
    const float* tableB_ = nullptr;
    float shapeMix_ = 0, openQuotient_ = 0.6f, noiseGain_ = 0, voiceGain_ = 1;
    float hp1_ = 0, hp2_ = 0, hpIn1_ = 0, hpIn2_ = 0, hpCoef_ = 0.8f;

    // resonators
    float a1_[kFormants]{}, a2_[kFormants]{}, a3_[kFormants]{}, out_[kFormants]{};
    float ic1_[kFormants]{}, ic2_[kFormants]{};

    // Real voices: the overlap-add ring, the read position and its wander
    std::vector<float> ring_;
    int ringMask_ = 0, realDelay_ = 0;
    int64_t outCount_ = 0;
    double nextGrain_ = 0, srcPos_ = 0;
    float realScale_ = 1, realGain_ = 1, walkDirection_ = 1;
    bool inSustain_ = false;
    SmoothNoise walk_;

    // level
    float attack_ = 0, releaseGain_ = 1, fade_ = 1;
    float envelope_ = 0, envelopeTarget_ = 0, envelopeStep_ = 0, shown_ = 0;
    float panLeft_ = 0.7071f, panRight_ = 0.7071f;
};
} // namespace sonora::devices::seraph
