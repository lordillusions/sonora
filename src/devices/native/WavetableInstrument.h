#pragma once
// Wavetable (.als tag "InstrumentVector", ROADMAP DEV-28b): native import of the reference's wavetable instrument.
//
// Architecture follows the reference device (docs/WAVETABLE.md): two wavetable oscillators with an effect mode each
// (None / FM / Classic / Modern), a sub oscillator, two filters (LP / HP / BP / Notch / Morph, circuits Clean / OSR / MS2 /
// SMP / PRD, 12 / 24 dB) routed Serial / Parallel / Split, the amplitude envelope and two free envelopes (Time / Slope /
// Value, None / Trigger / Loop), two LFOs, the 52 x 13 modulation matrix (5 modulators, MIDI and MPE sources), unison
// (Classic, Shimmer, Noise, Phase Sync, Position Spread, Random Note), Poly / Mono with a voice count and glide.
// The filter circuits reuse the laws measured for Simpler (docs/PARITY.md). Oscillator effects, unison and modulation
// scaling are Sonora approximations until measured (DEV-28b2, docs/WAVETABLE.md "Parity").
//
// Real-time: process() does not allocate, lock or log. Tables are immutable WavetableData shared from the load thread.

#include "devices/NativeDevice.h"
#include "devices/native/Biquad.h"
#include "devices/native/WavetableData.h"

#include <array>
#include <memory>

namespace sonora::devices
{

class WavetableInstrument final : public NativeDevice
{
public:
    // Parameter order = parameters(). Ids are the reference's XML tags; Manual (automatable) leaves first.
    enum Param
    {
        // per oscillator block: On, Transpose (st), Detune (st), Position, Effect1, Effect2, Pan, Gain (linear)
        Osc1On, Osc1Transpose, Osc1Detune, Osc1Position, Osc1Effect1, Osc1Effect2, Osc1Pan, Osc1Gain,
        Osc2On, Osc2Transpose, Osc2Detune, Osc2Position, Osc2Effect1, Osc2Effect2, Osc2Pan, Osc2Gain,
        SubOn, SubTone, SubGain, SubTranspose, // SubTranspose: 0 / -1 / -2 octaves
        // per filter: On, Type, CircuitLpHp, CircuitBpNoMo, Slope, Frequency (Hz), Resonance, Drive (dB), Morph
        F1On, F1Type, F1CircuitLpHp, F1CircuitBpNoMo, F1Slope, F1Frequency, F1Resonance, F1Drive, F1Morph,
        F2On, F2Type, F2CircuitLpHp, F2CircuitBpNoMo, F2Slope, F2Frequency, F2Resonance, F2Drive, F2Morph,
        AmpAttack, AmpDecay, AmpRelease, AmpSlopeAttack, AmpSlopeDecay, AmpSlopeRelease, AmpSustain, AmpLoop,
        // Env 2 / Env 3: times (s), slopes, values Initial / Peak / Sustain / Final, loop mode
        E2Attack, E2Decay, E2Release, E2SlopeAttack, E2SlopeDecay, E2SlopeRelease, E2Initial, E2Peak, E2Sustain, E2Final, E2Loop,
        E3Attack, E3Decay, E3Release, E3SlopeAttack, E3SlopeDecay, E3SlopeRelease, E3Initial, E3Peak, E3Sustain, E3Final, E3Loop,
        // LFO 1 / 2: Retrigger, Type, Amount, Shaping, PhaseOffset (deg), Sync (0 Hz / 1 synced), Rate (Hz), SyncedRate, Attack (s)
        L1Retrigger, L1Type, L1Amount, L1Shaping, L1Offset, L1Sync, L1Rate, L1SyncedRate, L1Attack,
        L2Retrigger, L2Type, L2Amount, L2Shaping, L2Offset, L2Sync, L2Rate, L2SyncedRate, L2Attack,
        TimeScale, ModAmount, UnisonAmount, GlobalTranspose, Glide, Volume,
        // plain (Remoteable / user) leaves, not automatable
        Osc1EffectMode, Osc2EffectMode, UnisonMode, UnisonVoices, FilterRouting, MonoPoly, PolyVoices, HiQ,
        MatrixFirst, // MatrixFirst + target * kSources + source: ModulationConnections amounts
        NumParams = MatrixFirst + 52 * 13
    };
    static constexpr int kTargets = 52, kSources = 13;
    // Matrix sources (ModulationAmounts.n): the five modulators, then MIDI and MPE.
    enum Source { SrcAmp, SrcEnv2, SrcEnv3, SrcLfo1, SrcLfo2, SrcVelocity, SrcNote, SrcPitchBend, SrcPressure, SrcModWheel,
                  SrcRandom, SrcNotePb, SrcSlide };
    // Matrix targets (TargetId order of every reference set and preset).
    enum Target { TOsc1Pitch, TOsc1Pos, TOsc1Fx1, TOsc1Fx2, TOsc1Pan, TOsc1Gain, TOsc2Pitch, TOsc2Pos, TOsc2Fx1, TOsc2Fx2,
                  TOsc2Pan, TOsc2Gain, TSubTone, TSubGain, TF1Freq, TF1Res, TF1Drive, TF1Morph, TF2Freq, TF2Res, TF2Drive,
                  TF2Morph, TAmpAttack, TAmpDecay, TAmpRelease, TAmpSustain, TE2Attack, TE2Decay, TE2Release, TE2Initial,
                  TE2Peak, TE2Sustain, TE2Final, TE3Attack, TE3Decay, TE3Release, TE3Initial, TE3Peak, TE3Sustain, TE3Final,
                  TL1Amount, TL1Shaping, TL1Rate, TL2Amount, TL2Shaping, TL2Rate, TTime, TModAmount, TUnisonAmount, TAmp,
                  TPitch, TGlide };
    static int matrixParam(int target, int source) { return MatrixFirst + target * kSources + source; }
    static const char* targetName(int target, int effectMode1 = 0, int effectMode2 = 0); // the reference's row label
    static const char* sourceName(int source);
    static const char* targetId(int target); // XML TargetId

    WavetableInstrument();
    std::string name() const override { return "Sonora Wavetable"; }
    bool isInstrument() const override { return true; }
    void prepare(double sampleRate, int maxBlockSize) override;
    void reset() override;
    void setHostInfo(const HostInfo& host) override { tempo_ = host.tempo > 0 ? host.tempo : 120.0; }
    void process(juce::AudioBuffer<float>& audio, juce::MidiBuffer& midi) override;
    std::vector<int> displayParameters() const override;

    // Message / load thread, before the device is published to the audio graph.
    void setTable(int oscillator, std::shared_ptr<const WavetableData> table, std::string category, std::string name);
    std::shared_ptr<const WavetableData> table(int oscillator) const { return tables_[(size_t) std::clamp(oscillator, 0, 1)]; }
    const std::string& tableCategory(int oscillator) const { return categories_[(size_t) std::clamp(oscillator, 0, 1)]; }
    const std::string& tableName(int oscillator) const { return names_[(size_t) std::clamp(oscillator, 0, 1)]; }
    int activeVoices() const { return activeVoices_.load(std::memory_order_relaxed); }
    // Modulated wavetable position of the most recent voice (face display), -1 when silent.
    float displayPosition(int oscillator) const { return displayPosition_[(size_t) std::clamp(oscillator, 0, 1)].load(std::memory_order_relaxed); }

    // Filter `index` (0 / 1) as biquad stages at `freq` with the current settings; returns the number of stages (1 or 2).
    int filterStages(int index, double freq, double resonance, double morph, BiquadCoeffs* out) const;
    int voiceLimit() const; // 1 in Mono, else the Poly voice count
    static int polyVoiceCount(int index) { return 2 + std::clamp(index, 0, 6); }
    static double syncedRateBeats(int index); // LFO SyncedRate enum -> cycle length in beats
    static const char* syncedRateName(int index);
    static constexpr int kSyncedRates = 22;

    // Envelope stage curve shared with the face: fraction of a stage covered at x in 0..1 for slope -1..1.
    static float slopeCurve(float x, float slope);
    // One sample of the sub oscillator (before its Gain) at `phase` (cycles), `inc` cycles per sample, Tone 0..1;
    // hpState is the pulse's high-pass memory, hpCoeff its one-pole coefficient.
    static float subOscillator(double phase, double inc, float tone, float& hpState, float hpCoeff);

private:
    static constexpr int kMaxVoices = 8, kMaxUnison = 8, kControl = 16;
    struct Env
    {
        int stage = 0; // 0 idle, 1 attack, 2 decay, 3 sustain, 4 release, 5 done
        float level = 0, from = 0;
        double elapsed = 0, length = 1;
    };
    struct EnvSettings { float attack, decay, release, slopeA, slopeD, slopeR, initial, peak, sustain, final; int loop; };
    struct Lfo { double phase = 0; float held = 0, value = 0; double age = 0; };
    struct Filter { SvfCoeffs c[2]; SvfState s[2][2]; int stages = 1; }; // s[stage][channel]
    struct Voice
    {
        bool active = false, gate = false, sustained = false;
        int note = -1, channel = 0;
        float velocity = 0, random = 0, pressure = 0;
        uint64_t age = 0;
        double glideSemis = 0, glideStep = 0;
        std::array<std::array<double, kMaxUnison>, 2> phase{};
        std::array<std::array<double, kMaxUnison>, 2> modPhase{}; // FM modulator phases
        std::array<float, kMaxUnison> jitter{}, jitterTarget{}, spread{};
        double subPhase = 0;
        float subHp = 0;
        Env env[3];
        Lfo lfo[2];
        Filter filter[2];
        int countdown = 0;
        // control-rate results
        float pitch[2]{}, position[2]{}, fx1[2]{}, fx2[2]{}, panL[2]{}, panR[2]{}, gain[2]{};
        float subTone = 0, subGain = 0, drive[2]{1, 1}, ampMod = 1, unisonAmount = 0, glideTime = 0;
        float subPitch = 0;
        EnvSettings amp{};       // amplitude envelope settings of this control block (stepped per sample)
        float mods[kSources]{};  // source values at the last control block (face display)
    };

    void handleEvent(const uint8_t* data, int size);
    void noteOn(int note, int channel, float velocity);
    void noteOff(int note, int channel);
    void releaseVoice(Voice& v);
    void render(float* left, float* right, int from, int to);
    void control(Voice& v);
    float renderOscillator(Voice& v, int osc, int unison, int voices, float& left, float& right);
    EnvSettings envSettings(int env, const float* targetMods) const;
    void stepEnv(Env& e, const EnvSettings& s, bool gate, double samples) const;
    float stepLfo(Lfo& l, int index, float rateMod, float amountMod, float shapingMod, double samples);
    float sourceValue(const Voice& v, int source) const;
    void refreshMatrix();

    std::array<std::shared_ptr<const WavetableData>, 2> tables_;
    std::array<std::string, 2> categories_, names_;
    std::array<Voice, kMaxVoices> voices_{};
    std::array<float, 16> bend_{}, slide_{}, channelPressure_{};
    float modWheel_ = 0;
    bool sustainPedal_ = false;
    double sampleRate_ = 44100, tempo_ = 120, subHpCoeff_ = 0.0007;
    uint64_t counter_ = 0;
    int lastNote_ = -1;
    uint32_t random_ = 0x2545F491u;
    Lfo freeLfo_[2];
    // Non-zero matrix cells, rebuilt on the audio thread when parameters change (no allocation: fixed storage).
    struct Route { int16_t target, source; float amount; };
    std::array<Route, kTargets * kSources> routes_{};
    int routeCount_ = 0;
    std::atomic<int> activeVoices_{0};
    std::array<std::atomic<float>, 2> displayPosition_{};
};

} // namespace sonora::devices
