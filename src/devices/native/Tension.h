#pragma once
// Tension (.als tag "StringStudio", ROADMAP DEV-28j): native import of the reference's physically modelled string
// instrument.
//
// Every voice is a bidirectional digital waveguide (velocity waves) with the reference's sections: an exciter (Bow,
// Hammer, Hammer (bouncing), Plectrum) acting as a physical contact at its position, a damper (mass/spring/felt
// contact, optionally gated by the key), a termination (finger and fret at the nut), a pickup or the bridge as the
// output, a body resonator (Piano / Guitar / Violin / Generic, five sizes), then a per-voice filter with its envelope
// and LFO, vibrato, unison, portamento and the keyboard section. Parameters are stored exactly as the set stores them
// (0..1, -1..1, enum indices); the value laws shown by the reference's face were read from captures of it and the
// sound laws fitted to its stems (docs/TENSION.md).
//
// Real-time: process() does not allocate, lock or log. The waveguide memory is allocated in prepare().

#include "devices/NativeDevice.h"
#include "devices/native/Biquad.h"

#include <array>
#include <atomic>
#include <string>
#include <vector>

namespace sonora::devices
{

class Tension final : public NativeDevice
{
public:
    // Parameter order = parameters(); ids are the reference's XML tags (schema order).
    enum Param
    {
        Polyphony, PitchBendRange, Octave, Transpose, FineTune, Priority, UnisonOn, UnisonVoices, UnisonDetune, UnisonDelay,
        Stretch, KeyError,
        VibratoOn, VibratoRate, VibratoFadeIn, VibratoAmount, VibratoError, VibratoDelay, VibratoModWheel,
        PortamentoOn, PortamentoTime, PortamentoProportional, PortamentoLegato,
        ExcPosition, ExcPositionAbs, ExcPositionKey, ExcPositionVel,
        DamperPosition, DamperPositionAbs, DamperPositionKey, DamperPositionVel,
        ExcOn, ExcType, ExcX, ExcXKey, ExcXVel, ExcStiffness, ExcStiffnessKey, ExcStiffnessVel,
        ExcVelocity, ExcVelocityKey, ExcVelocityVel, ExcDamping,
        PickupOn, PickupPosition,
        DamperOn, DamperMass, DamperMassKey, DamperStiffness, DamperStiffnessKey, DamperVelocity, DamperVelocityKey,
        DamperDamping, DamperGated,
        StringDamping, StringDampingKey, StringDecay, StringDecayKey, StringRatio, StringInharm,
        TermOn, TermFingerStiffness, TermFingerMass, TermFingerMassKey, TermFingerMassVel, TermFretStiffness,
        LfoOn, LfoShape, LfoSync, LfoSyncOn, LfoDelay, LfoRate, LfoFadeIn,
        FilterOn, FilterType, FilterFreq, FilterFreqKey, FilterFreqLfo, FilterFreqEnv,
        FilterRes, FilterResKey, FilterResLfo, FilterResEnv,
        FenvOn, FenvAttack, FenvDecay, FenvSustain, FenvRelease, FenvAttackVel, FenvAmountVel,
        BodyOn, BodyType, BodySize, BodyDecay, BodyLowCut, BodyHighCut, Volume, BodyMix,
        NotePbRange, PressureTarget1, PressureAmount1, PressureTarget2, PressureAmount2,
        SlideTarget1, SlideAmount1, SlideTarget2, SlideAmount2,
        CurrentTab, // plain: the face's String / Filter-Global tab
        NumParams
    };
    enum Exciter { Bow, Hammer, HammerBouncing, Plectrum };
    // Pressure / Slide targets (the reference's chooser order, read from captures).
    enum Target { TNone, TVolume, TVibratoAmount, TVibratoRate, TUnisonDetune, TInharm, TLfoRate, TCutoff, TCutoffLfo,
                  TCutoffEnv, TRes, TResLfo, TResEnv, kTargets };
    static constexpr int kFilterTypes = 10, kLfoShapes = 5, kSyncRates = 24, kPolyphonies = 10;

    Tension();
    std::string name() const override { return "Tension"; }
    bool isInstrument() const override { return true; }
    void prepare(double sampleRate, int maxBlockSize) override;
    void reset() override;
    void setHostInfo(const HostInfo& host) override { tempo_ = host.tempo > 0 ? host.tempo : 120.0; }
    void process(juce::AudioBuffer<float>& audio, juce::MidiBuffer& midi) override;
    std::vector<int> displayParameters() const override;
    std::vector<UiGroup> uiGroups() const override;

    int activeVoices() const { return activeVoices_.load(std::memory_order_relaxed); }

    // ---- value laws of the reference's face (stored value -> shown value), shared with the face and tests
    static float filterHz(float v);            // 30 Hz .. 22 kHz
    static float envelopeMs(float v);          // filter envelope A / D / R: 5 ms .. 15 s
    static float vibratoMs(float v);           // vibrato Delay / Attack: 0, 51 ms .. 5.1 s
    static float vibratoHz(float v);           // 0.2 .. 10 Hz
    static float lfoHz(float v);               // filter LFO rate
    static float lfoMs(float v);               // filter LFO Delay / Attack: 0, 200 ms .. 5 s
    static float unisonMs(float v);            // unison Delay: 0, 2 .. 100 ms
    static float keyAmount(float v);           // exciter / damper / string Key and Vel boxes: +-0.1 .. 16
    static float cutoffEnvAmount(float v);     // filter Freq < Env box: +-16
    static float modAmount(float v);           // the other filter modulation boxes: +-4
    static float envelopeVelAmount(float v) { return 8.0f * v; } // filter envelope A / S Vel boxes
    static float volumeDb(float v);            // (v / 0.705)^2 shown in dB (0 dB at the default, +6.07 at 1)
    static float volumeGain(float v) { return v * v / (0.705f * 0.705f); } // 0.705 (the default) = 0 dB
    static double syncBeats(int index);        // LFO Sync index -> cycle length in beats
    static const char* syncName(int index);
    static const char* filterTypeName(int type);
    static const char* targetName(int target);
    static int polyphonyVoices(int index);     // 1 (Mono) .. 32

private:
    static constexpr int kMaxStrings = 64, kControl = 16;
    struct Contact // a lumped mass touching the string at one point (hammer, plectrum, damper)
    {
        bool active = false, engaged = false, released = false;
        double y = 0, v = 0;    // position and velocity of the mass (string units / sample)
        double ys = 0;          // string displacement at the point
        double lastDelta = 0;
    };
    struct Env { int stage = 0; float level = 0; double t = 0; }; // 0 idle, 1 A, 2 D, 3 S, 4 R
    struct Str
    {
        bool active = false, gate = false, sustained = false, released = false, rising = false;
        int note = -1, channel = 0, member = 0, members = 1;
        float velocity = 0;
        uint64_t age = 0;
        int startDelay = 0;          // unison strum: samples before the excitation starts
        // pitch
        double semis = 60, targetSemis = 60, glideStep = 0;
        float detuneCents = 0, errorCents = 0, vibratoErrorRate = 1, vibratoErrorAmount = 1;
        // waveguide
        int mInt = 64;               // integer side (L line) length
        double mFrac = 64;           // fractional side (R line) length
        int write = 0;
        float apState[4]{}, apCoeff = 0; int apStages = 0;
        double lossState = 0, lossPole = 0, lossGain = 1;
        double nutState = 0, nutPole = 0, nutGain = 1;
        int excPos = 8, damperPos = 16, pickupPos = 8;
        // exciter
        int exciter = Plectrum;
        Contact exc, damper;
        double excMass = 1, excK = 1, excR = 0, excRelease = 1, excVelocity = 0;
        double plPeak = 0, plPush = 1, plTau = 1, plTime = 0;
        double bowForce = 0, bowVelocity = 0, bowSlope = 3, bowOffset = 0;
        double damperMass = 1, damperK = 1, damperR = 0, damperV = 0;
        bool damperDown = false;
        // tone
        float level = 1, decay = 1;
        float bodyState[2]{};
        // filter and modulators
        SvfCoeffs filter[2]{};
        SvfState fstate[2]{};
        int filterStages = 1;
        Env fenv;
        double lfoPhase = 0, lfoAge = 0, vibPhase = 0, age_s = 0;
        float lfoHeld = 0, lfoTarget = 0, lfoValue = 0;
        float fenvPeak = 1, fenvAttackScale = 1;
        int countdown = 0;
        double silence = 0;
        float lastOut = 0, dcIn = 0, dcOut = 0;
    };

    void handleEvent(const uint8_t* data, int size);
    void noteOn(int note, int channel, float velocity);
    void noteOff(int note, int channel);
    void releaseString(Str& s);
    void startString(Str& s, int note, int channel, float velocity, int member, int members, bool legato);
    void render(float* left, float* right, int from, int to);
    void control(Str& s);
    float tick(Str& s);
    void tuneString(Str& s, double frequency);
    float targetMod(const Str& s, int target) const;
    float stepEnv(Str& s, double samples);
    float stepLfo(Str& s, double samples);
    void updateBody();
    float bodyTick(float x);
    int voiceLimit() const;
    float k(Param p) const { return value(p); }

    std::vector<float> lineR_, lineL_; // kMaxStrings x lineLength_
    int lineLength_ = 4096, lineMask_ = 4095;
    std::array<Str, kMaxStrings> strings_{};
    std::array<float, 16> bend_{}, slide_{}, pressure_{};
    float modWheel_ = 0, channelPressure_ = 0;
    bool sustainPedal_ = false;
    double sampleRate_ = 44100, tempo_ = 120;
    uint64_t counter_ = 0;
    uint32_t random_ = 0x6C8E9CF5u;
    int lastNote_ = -1;
    std::array<int, 128> held_{}; // held keys (mono priority)
    uint64_t heldOrder_ = 0;
    std::array<uint64_t, 128> heldAt_{};
    // body: a bank of resonators and the cut filters (shared by all voices: the reference body is after the strings)
    static constexpr int kModes = 12;
    std::array<BiquadCoeffs, kModes> modeCoeffs_{};
    std::array<std::array<double, 2>, kModes> modeState_{};
    std::array<float, kModes> modeGain_{};
    BiquadCoeffs lowCut_{}, highCut_{};
    std::array<double, 2> lowState_{}, highState_{};
    float bodyKey_[4]{-1, -1, -1, -1};
    std::atomic<int> activeVoices_{0};
};

} // namespace sonora::devices
