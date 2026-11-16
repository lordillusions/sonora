#pragma once
// Operator (.als tag "Operator", ROADMAP DEV-28a): native import of the reference's four-oscillator FM instrument.
//
// Architecture follows the reference device (docs/OPERATOR.md): four oscillators A-D (sine, 4/8-bit sine, additive saw /
// square with 3..64 harmonics, digital saw / square, triangle, looped / white noise and a 64-partial user waveform), each
// with Coarse / Fine ratio or a fixed frequency, level, Vel / Key level scaling, Osc<Vel, phase, retrigger, feedback and a
// three-rate / three-level envelope with Loop / Beat / Sync / Trigger modes; eleven algorithms; a filter (the sampling
// engine's measured LP / HP / BP / Notch / Morph and circuits) with its own envelope and a shaper; an LFO with an envelope
// and Dest A / Dest B; a pitch envelope; Spread, Transpose, Tone, Time, glide, pan and the MIDI modulation routing.
// Laws that are not yet measured against the reference are marked as approximations in docs/OPERATOR.md "Engine".
//
// Real-time: process() does not allocate, lock or log. User waveforms are rebuilt into fixed storage on the audio thread
// only when their plain parameters change (bounded work, no allocation).

#include "devices/NativeDevice.h"
#include "devices/native/Biquad.h"

#include <array>
#include <atomic>

namespace sonora::devices
{

class Operator final : public NativeDevice
{
public:
    static constexpr int kOperators = 4, kAlgorithms = 11, kWaveforms = 23, kHarmonics = 64;

    // ---- parameter layout (index = parameters() order; ids are the reference's XML paths)
    // Envelope fields, shared by the oscillator, filter, pitch and LFO envelopes ("<prefix>/AttackTime"...). Times in ms,
    // levels linear amplitude (oscillator / LFO), 0..1 (filter) or semitones (pitch); slopes -1..1.
    enum EnvField
    {
        AttackTime, AttackLevel, AttackSlope, DecayTime, DecayLevel, DecaySlope, SustainLevel, ReleaseTime, ReleaseLevel,
        ReleaseSlope, LoopMode, LoopTime, RepeatTime, TimeVelScale, kEnvFields
    };
    // Per-oscillator fields ("Operator.<n>/...").
    enum OpField
    {
        OpOn, OpCoarse, OpFine, OpFixed, OpFixedFreq, OpFixedMulti, OpVelCoarse, OpVelCoarseQuantized, OpVolume, OpRetrigger,
        OpPhase, OpFeedback, OpWave, OpVelScale, OpKeyScale, OpLfoOn, OpPitchEnvOn,
        OpEnv, // OpEnv + EnvField: the oscillator's envelope
        kOpFields = OpEnv + (int) kEnvFields
    };
    enum EnvIndex { EnvFilter, EnvPitch, EnvLfo, kOtherEnvs };
    enum Global
    {
        GlobalFirst = kOperators * (int) kOpFields,
        OtherEnvFirst = GlobalFirst, // kOtherEnvs envelope blocks (filter, pitch, LFO), then the rest
        Algorithm = OtherEnvFirst + (int) kOtherEnvs * (int) kEnvFields,
        Transpose, PitchBendRange, Volume, Pan, PanKey, PanRandom, Tone, Spread, GlideOn, GlideTime,
        EnvTime, EnvTimeKey, EnvTimeIncludeAttack,
        LfoOn, LfoType, LfoRange, LfoRate, LfoRateSynced, LfoRateKey, LfoRetrigger, LfoAmount, LfoAmountA, LfoAmountVel,
        LfoPitchEnvOn, LfoDestBAmount, LfoDestB,
        PitchEnvOn, PitchEnvAmount, PitchAmountA, PitchDestBAmount, PitchDestB,
        FilterOn, FilterType, FilterLegacyType, FilterCircuitLpHp, FilterCircuitBpNoMo, FilterSlope, FilterFreq, FilterLegacyQ,
        FilterRes, FilterMorph, FilterDrive, FilterFreqVel, FilterFreqKey, FilterEnvAmount, FilterLfoOn,
        ShaperType, ShaperDryWet, ShaperDrive,
        // plain (Remoteable / user) leaves, not automatable
        NumVoices, RetriggerMode, Interpolation, AntiAlias,
        MidiModFirst, // 5 sources (Key, Velocity, Aftertouch? / MidiCtrl.0..2) x 2 slots x (Amount, Connection)
        UserFirst = MidiModFirst + 5 * 2 * 2, // per oscillator: kHarmonics amplitudes, NumHarmonics, ExpansionMode, Normalize
        NumParams = UserFirst + kOperators * (kHarmonics + 3)
    };
    static constexpr int opParam(int op, int field) { return op * kOpFields + field; }
    static constexpr int envParam(int op, int field) { return op * kOpFields + OpEnv + field; }    // an oscillator's envelope
    static constexpr int otherEnvParam(int env, int field) { return OtherEnvFirst + env * kEnvFields + field; }
    static constexpr int harmonicParam(int op, int harmonic) { return UserFirst + op * (kHarmonics + 3) + harmonic; }
    static constexpr int numHarmonicsParam(int op) { return UserFirst + op * (kHarmonics + 3) + kHarmonics; }
    static constexpr int expansionParam(int op) { return numHarmonicsParam(op) + 1; }
    static constexpr int normalizeParam(int op) { return numHarmonicsParam(op) + 2; }
    // MIDI routing: source 0 Key, 1 Velocity, 2..4 MidiCtrl.0..2; slot 0 / 1; Amount (-100..100) or Connection (enum).
    static constexpr int midiModParam(int source, int slot, bool connection) { return MidiModFirst + (source * 2 + slot) * 2 + (connection ? 1 : 0); }

    // ---- value names and laws shared with the face
    static const char* waveName(int wave);        // "Sin", "Sin 4 Bit", ... "User" (WaveForm enum order)
    static const char* waveShortName(int wave);   // chooser text in the oscillator display ("Sin", "Saw 3", "Sq D"...)
    static const char* lfoWaveName(int type);     // LfoType
    static const char* loopModeName(int mode);    // None / Loop / Beat / Sync / Trigger
    static const char* repeatName(int index);     // RepeatTime enum ("1/64" ... "8")
    static const char* fixedMultiName(int index); // FixedFrequencyMulti enum
    static double fixedMultiplier(int index);
    static const char* syncedRateName(int index); // LfoRateSynced enum
    static double syncedRateBeats(int index);
    static const char* destinationName(int connection); // Dest B / MIDI routing Connection enum
    static constexpr int kDestinations = 24;
    // Algorithm structure: for algorithm `a`, the oscillators that modulate oscillator `op` (bit mask of 1 << m) and the
    // carriers (bit mask). Oscillator index 0 = A ... 3 = D. Feedback applies to oscillators no other one modulates.
    static int modulators(int algorithm, int op);
    static int carriers(int algorithm);
    // Oscillator frequency ratio (of the note) from Coarse / Fine, and the fixed frequency in Hz from Freq / Multi.
    static double ratio(float coarse, float fine);
    static double fixedFrequency(float freq, int multi);
    // LFO frequency in Hz for Rate 0..127 in Range 0 Low / 1 High.
    static double lfoHz(float rate, int range);
    // One sample of `wave` (not User / noise) at phase 0..1, band-limited to harmonics below `maxHarmonic`.
    static float waveSample(int wave, double phase);
    // Envelope stage curve shared with the face: fraction covered at x in 0..1 for slope -1..1 (Simpler's measured law).
    static float slopeCurve(float x, float slope);

    Operator();
    std::string name() const override { return "Operator"; }
    bool isInstrument() const override { return true; }
    void prepare(double sampleRate, int maxBlockSize) override;
    void reset() override;
    void setHostInfo(const HostInfo& host) override;
    void process(juce::AudioBuffer<float>& audio, juce::MidiBuffer& midi) override;
    std::vector<int> displayParameters() const override;

    int activeVoices() const { return activeVoices_.load(std::memory_order_relaxed); }
    int voiceLimit() const; // NumVoices enum -> voices
    static int voiceCount(int index);
    // Filter as biquad stages at `freq` with the current settings; returns the number of stages (1 or 2). Face display.
    int filterStages(double freq, BiquadCoeffs* out) const;
    int filterStages(double freq, BiquadCoeffs* out, float resOffset, float morphOffset) const;
    double sampleRate() const { return sampleRate_; }

private:
    static constexpr int kMaxVoices = 32, kTableSize = 2048, kNoiseSize = 1 << 15, kControl = 16;

    struct Env
    {
        enum Stage : uint8_t { Idle, Attack, Decay, Sustain, Release, LoopFall, Done };
        Stage stage = Idle;
        float level = 0, from = 0;
        double elapsed = 0, length = 1, age = 0, nextRepeat = 0;
    };
    struct EnvShape
    {
        float attack = 1, decay = 1, release = 1;              // samples
        float initial = 0, peak = 1, sustain = 1, end = 0;      // levels
        float attackSlope = 0, decaySlope = 1, releaseSlope = 1;
        int loopMode = 0;
        float loopTime = 1, repeat = 1;                         // samples
        bool amplitude = true;                                 // oscillator / LFO envelopes: dB-linear decay and release
    };
    struct Osc
    {
        double phase = 0;
        float last = 0, prev = 0; // feedback memory (the last two outputs)
        Env env;
        float level = 0;          // control-rate oscillator level (volume x vel / key scaling)
        double freq = 0;          // Hz without pitch modulation
        uint32_t noisePos = 0;
    };
    // Destination values of the modulation routing (Dest B of the LFO and pitch envelope, MIDI Key / Vel / controllers).
    struct Mods
    {
        float gain[4] = {1, 1, 1, 1};
        float oscSemis = 0, filterSemis = 0, res = 0, morph = 0, envAmount = 1, lfoRateSemis = 0, lfoAmount = 1, volume = 1, pan = 0;
    };
    // Adds connection `dest` at `amount` (-1..1) for a source at `s` (0..1): centred (0.5 neutral) or, `fromZero`, neutral at 0.
    static void applyRoute(Mods& m, int dest, float amount, float s, bool fromZero);

    struct Voice
    {
        bool active = false, gate = false, sustained = false;
        int note = -1, channel = 0;
        float velocity = 0, panL = 1, panR = 1, spreadSign = 0;
        uint64_t age = 0;
        double glideSemis = 0, glideStep = 0;
        std::array<Osc, kOperators> osc{};
        std::array<Osc, kOperators> spreadOsc{}; // second, detuned voice for Spread
        Env filterEnv, pitchEnv, lfoEnv;
        double lfoPhase = 0;
        float lfoHeld = 0, lfoValue = 0, lfoNoise = 0;
        SvfCoeffs coeffs[2];
        SvfState svf[2][2];
        int filterStages = 1, countdown = 0;
        float timeScale = 1;
        std::array<EnvShape, kOperators> opShape{};
        EnvShape filterShape, pitchShape, lfoShape;
        Mods mods;
        float pan = 0; // Panorama + Pan<Key + Pan<Rnd of this note (before routing)
    };

    void handleEvent(const uint8_t* data, int size);
    void noteOn(int note, int channel, float velocity);
    void noteOff(int note, int channel);
    void releaseVoice(Voice& v);
    void startVoice(Voice& v, int note, int channel, float velocity, bool legato);
    void render(float* left, float* right, int from, int to);
    void control(Voice& v);
    EnvShape envShape(int firstParam, bool amplitude, float timeScale, float velocity) const;
    static void startStage(Env& e, Env::Stage stage, double length);
    float stepEnv(Env& e, bool released, const EnvShape& s) const;
    float oscSample(Osc& o, int op, int wave, double phase);
    float lfoStep(Voice& v, double inc);
    void refreshDerived();
    void rebuildUserTable(int op);

    std::array<Voice, kMaxVoices> voices_{};
    std::array<float, 16> bend_{};
    float modWheel_ = 0, aftertouch_ = 0;
    bool sustainPedal_ = false;
    double sampleRate_ = 44100, tempo_ = 120, beats_ = 0;
    bool playing_ = false;
    uint64_t counter_ = 0;
    int lastNote_ = -1;
    uint32_t random_ = 0x6d2b79f5u;
    double freeLfoPhase_ = 0;
    // User waveforms: one band-limited table per oscillator, rebuilt when its harmonics change.
    std::array<std::array<float, kTableSize + 1>, kOperators> userTables_{};
    std::array<uint32_t, kOperators> userHash_{};
    std::array<float, kNoiseSize> noise_{};
    std::atomic<int> activeVoices_{0};
};

} // namespace sonora::devices
