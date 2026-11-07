#pragma once
// Sonora Sampler (.als tag "MultiSampler", ROADMAP DEV-14 / DEV-14b; parameter coverage in docs/SAMPLER.md).
//
// The reference's Sampler is its Simpler engine with every control exposed, so this device shares Simpler's measured
// laws (playback kernel, filter circuits, envelope slopes, LFO shapes, velocity / key / pan laws) and adds what only the
// Sampler has: zone crossfades (key / velocity / Sample Select, linear or constant power), round robin, sustain and
// release loops with their crossfades and detune, Reverse, Snap, Link, Tune Scale, the modulation oscillator (FM / AM
// with its own looping envelope), envelope Initial / Peak / End levels, Time < Vel, the global Time / Time < Key,
// the Shaper, the Aux envelope, LFO 2 / 3 (stereo Phase / Spin) and the modulation matrix (Aux, LFO 2, LFO 3, Key,
// Velocity, Off Velocity, Mod Wheel, Foot, Pitch Bend, Note PB, Slide, Pressure -> 31 destinations), per-note MPE.
//
// Parameters: indices 0 .. Simpler::NumParams - 1 are Simpler's (same XML paths, so a face can index both devices with
// Simpler::Param), then the Sampler-only ones below. Every id is the parameter's XML path below <MultiSampler>; plain
// values (the matrix connections, Pitch Bend ranges, round robin) are leaves without <Manual>.

#include "devices/native/Simpler.h"

#include <array>

namespace sonora::devices
{

class Sampler final : public NativeDevice
{
public:
    using Zone = Simpler::Zone;

    // Envelope parameter block layout (see envelopeBlock()): 14 consecutive parameters in XML order.
    // Envelope parameter block layout (see the envelope blocks below): 14 consecutive parameters in XML order.
    static constexpr int EnvAttack = 0, EnvInitial = 1, EnvAttackSlope = 2, EnvDecay = 3, EnvPeak = 4, EnvDecaySlope = 5,
                         EnvSustain = 6, EnvRelease = 7, EnvEnd = 8, EnvReleaseSlope = 9, EnvLoopMode = 10, EnvLoopTime = 11,
                         EnvRepeat = 12, EnvTimeVel = 13, EnvFieldCount = 14;
    // LFO 2 / 3 parameter block (AuxLfos.N): IsOn then 13 consecutive parameters, then the matrix (plain).
    static constexpr int LfoOnField = 0, LfoType = 1, LfoFrequency = 2, LfoRateType = 3, LfoBeatRate = 4, LfoStereoMode = 5,
                         LfoSpin = 6, LfoPhase = 7, LfoOffset = 8, LfoKeyScale = 9, LfoSmooth = 10, LfoAttackField = 11,
                         LfoRetrig = 12, LfoWidth = 13, LfoFieldCount = 14;
    // A modulation matrix row (SimplerModDst): two connections, each a destination and an amount (-100 .. 100).
    static constexpr int ModDst0 = 0, ModAmount0 = 1, ModDst1 = 2, ModAmount1 = 3, ModFieldCount = 4;

    enum Param
    {
        // Volume envelope levels and Time < Vel (the times / slopes / loop are Simpler's parameters).
        AmpInitial = static_cast<int>(Simpler::NumParams), AmpPeak, AmpEnd, AmpTimeVel,
        // Filter and pitch envelope levels, loop and Time < Vel.
        FilterInitial, FilterPeak, FilterEnd, FilterLoopMode, FilterLoopTime, FilterRepeat, FilterTimeVel,
        PitchInitial, PitchPeak, PitchEnd, PitchLoopMode, PitchLoopTime, PitchRepeat, PitchTimeVel,
        FilterLegacyType, FilterLegacyQ,
        ShaperOn, ShaperType, ShaperAmount, ShaperPost,
        OscOn, OscMode, OscType, OscVolume, OscVolVel, OscFixed, OscCoarse, OscFine, OscFixedFreq, OscFixedMul,
        OscEnv, // EnvFieldCount parameters
        AuxOn = OscEnv + EnvFieldCount,
        AuxEnv, // EnvFieldCount parameters
        AuxMod = AuxEnv + EnvFieldCount, // ModFieldCount plain parameters
        Lfo1StereoMode = AuxMod + ModFieldCount, Lfo1Spin, Lfo1Phase, Lfo1Smooth, Lfo1Width,
        Lfo2, // LfoFieldCount parameters, then ModFieldCount plain parameters
        Lfo3 = Lfo2 + LfoFieldCount + ModFieldCount,
        Reverse = Lfo3 + LfoFieldCount + ModFieldCount, SampleSelector,
        LoadInRam, LayerCrossfade, RoundRobin, RoundRobinMode, RoundRobinReset,
        KeyMod, // ModFieldCount plain parameters each: Key, Velocity, Off Velocity
        VelMod = KeyMod + ModFieldCount,
        RelVelMod = VelMod + ModFieldCount,
        MidiMod = RelVelMod + ModFieldCount, // 6 x ModFieldCount: Pressure, Pitch Bend, Mod Wheel, Foot, Slide, Note PB
        EnvTime = MidiMod + 6 * ModFieldCount, EnvTimeKey, EnvTimeAttack,
        PitchBendRangeParam, MpeBendRange,
        NumParams
    };

    // Matrix destinations (<Connection> values; docs/SAMPLER.md lists how each was identified).
    enum Destination
    {
        DstOff, DstSampleSelector, DstSampleOffset, DstLoopStart, DstLoopLength, DstReleaseLoop, DstPitch, DstPitchSample,
        DstPitchOsc, DstPitchEnv, DstOscVolume, DstShaper, DstFilterFreq, DstFilterQ, DstFilterRes, DstFilterMorph,
        DstFilterDrive, DstFilterEnv, DstVolume, DstPan, DstTime, DstLfo1Rate, DstLfo1Vol, DstLfo1Pan, DstLfo1Filter,
        DstLfo1Pitch, DstLfo2Rate, DstLfo2A, DstLfo2B, DstLfo3Rate, DstLfo3A, DstLfo3B, DstCount
    };
    static const char* destinationName(int destination);
    // MidiCtrl.N rows: 0 Pressure, 1 Pitch Bend, 2 Mod Wheel, 3 Foot Ctrl, 4 Slide, 5 Note PB.
    enum MidiRow { RowPressure, RowPitchBend, RowModWheel, RowFoot, RowSlide, RowNotePb, MidiRows };

    // `seed` = <RoundRobinRandomSeed> (kept out of the float parameters: it is a 32-bit integer).
    explicit Sampler(std::vector<Zone> zones, int randomSeed = 0);

    std::string name() const override { return "Sonora Sampler"; }
    bool isInstrument() const override { return true; }
    void prepare(double sampleRate, int maxBlockSize) override;
    void reset() override;
    void setHostInfo(const HostInfo& host) override { host_ = host; }
    void process(juce::AudioBuffer<float>& audio, juce::MidiBuffer& midi) override;
    std::vector<int> displayParameters() const override;

    int activeVoices() const { return activeVoices_.load(std::memory_order_relaxed); }
    const std::vector<Zone>& zones() const { return zones_; }
    double sampleRate() const { return sampleRate_; }
    int pitchBendRange() const { return (int) std::lround(value(PitchBendRangeParam)); }
    int filterStages(double freq, BiquadCoeffs* out) const; // unmodulated filter response (face display)
    int filterCircuit() const;

    // ---- Laws, public so tests can pin them (docs/SAMPLER.md) ----
    // Zone fade gain of `x` in [low, high] with full level in [fadeLow, fadeHigh]; 0 outside the zone.
    static float zoneFade(int x, int low, int high, int fadeLow, int fadeHigh, bool constantPower);
    // Envelope time factor of the global Time, Time < Key and an envelope's Time < Vel (all in %).
    static double timeScale(double envTime, double timeKey, int note, double timeVel, float velocity);
    // Modulation oscillator: waveform `type` (0 .. 20) at phase 0..1; frequency ratio of Coarse / Fine.
    static float oscWave(int type, double phase, unsigned& noise);
    static double oscRatio(double coarse, double fine);
    static double oscFixedMultiplier(int index);
    static float shape(int type, float x, float amount); // Shaper: Soft / Hard / Sine / 4 Bit
    static double lfoBeatQuarters(int index);            // Simpler's 17 measured rates, then 2 .. 8 bars

    // Zones that play `note` at `velocity` (and the selector value), with their crossfade gains; round robin applied.
    // Returns the count (at most kMaxLayers). Audio thread safe (no allocation).
    static constexpr int kMaxLayers = 8;
    int selectZones(int note, int velocity, int selector, int* zoneIndices, float* gains);

private:
    static constexpr int kMaxVoices = 32;
    static constexpr int kMaxGroups = 64;

    struct Env
    {
        enum class Stage { Idle, Attack, Decay, Sustain, Release, LoopFall };
        Stage stage = Stage::Idle;
        double age = 0, nextRepeat = 0;
        double floor = Simpler::kEnvSlopeFloor;
        float level = 0, from = 0, target = 0;
        double elapsed = 0, length = 1, term = 1, ratio = 1;
        float slope = 0;
        bool active = false;
    };
    struct EnvShape
    {
        float attack = 1, decay = 1, release = 1;   // samples
        float initial = 0, peak = 1, sustain = 1, end = 0;
        float attackSlope = 0, decaySlope = 1, releaseSlope = 1;
        int loopMode = 0;
        float loopTime = 1, repeat = 1; // samples
    };
    struct Layer
    {
        const Zone* zone = nullptr;
        const engine::SampleData* sample = nullptr;
        double pos[2] = {0, 0};
        int direction[2] = {1, 1};
        double start = 0, end = 0;             // played region (frames)
        double loopStart = 0, loopEnd = 0, fade = 0;
        int loopMode = 0;                      // 0 none, 1 forward, 2 back-and-forth (sustain loop)
        double relStart = 0, relFade = 0;      // release loop start (ends at `end`)
        int relMode = 3;
        bool releasing = false, inLoop = false, inRelease = false, done = false;
        float gainL = 1, gainR = 1;
        double rootSemis = 0;                  // pitch offset of this zone (key tracking, root, detune)
        double rate = 1;                       // source rate / output rate
    };
    struct Voice
    {
        bool active = false, released = false, keyDown = true;
        int note = -1, channel = 1;
        float velocity = 0, releaseVelocity = 0;
        long long age = 0;
        int layerCount = 0;
        std::array<Layer, kMaxLayers> layers{};
        double glideSemis = 0, glideStep = 0;
        float panBase = 0;              // pan without LFO / matrix modulation
        float levelGain = 1;            // volume / velocity / key law
        double timeScale = 1;
        Env amp, filter, pitch, aux, osc;
        EnvShape ampShape, filterShape, pitchShape, auxShape, oscShape;
        dsp::Lfo lfo1, lfo2[2], lfo3[2];
        double lfoAge = 0;
        float lfoSmooth[3][2][2] = {};
        double lfo1Scale = 1, lfo2Scale = 1, lfo3Scale = 1; // Rate < Key
        double oscPhase = 0;
        unsigned noise = 1;
        float notePb = 0, slide = 0.5f, pressure = 0; // per-note MPE values (member channel)
        float filterKeySemis = 0;
        SvfCoeffs coeffs[2][2];         // [channel][stage]
        SvfState f[2][2];
        int filterCountdown = 0, filterStageCount = 1;
    };
    struct Mods
    {
        float dst[2][DstCount] = {}; // [channel][destination], amounts summed (source x amount / 100)
    };

    void noteOn(int note, float velocity, int channel);
    void noteOff(int note, float releaseVelocity, int channel);
    void startVoice(Voice& v, int note, float velocity, int channel, int fromNote, bool legato);
    void startLayer(Voice& v, Layer& layer, const Zone& zone, float gain);
    void render(juce::AudioBuffer<float>& audio, int from, int to);
    void renderVoice(Voice& v, float* outL, float* outR, int from, int to);
    EnvShape shapeOf(int block, bool volumeEnvelope, double scale, float velocity, bool scaleAttack) const;
    EnvShape ampShape(double scale, float velocity, bool scaleAttack) const;
    static void startStage(Env& e, Env::Stage stage, float target, double length, float slope);
    static float stepEnv(Env& e, bool released, const EnvShape& shape);
    static void triggerEnv(Env& e, const EnvShape& shape, double floor);
    void computeMods(const Voice& v, Mods& m, const float lfo1, const float* lfo2, const float* lfo3) const;
    float readLayer(const Layer& layer, int channel, double step) const;
    bool advanceLayer(Layer& layer, int channel, double step) const;
    double snap(const engine::SampleData& sample, double frame, double low, double high) const;
    void stepRoundRobin();

    std::vector<Zone> zones_;
    double sampleRate_ = 44100.0;
    HostInfo host_;
    float bendSemis_ = 0, bend_ = 0, modWheel_ = 0, foot_ = 0, pressure_ = 0, slide_ = 0.5f;
    bool sustainPedal_ = false;
    std::array<float, 16> channelBend_{}, channelPressure_{}, channelSlide_{};
    long long counter_ = 0;
    int lastNote_ = -1;
    double lastPitch_ = -1; // portamento source (semitones of the last started voice)
    dsp::Lfo freeLfo1_, freeLfo2_[2], freeLfo3_[2];
    unsigned random_ = 22222u, rrRandom_ = 1u;
    struct Group { int keyMin = -1, keyMax = -1, counter = 0, last = -1; };
    std::array<Group, kMaxGroups> groups_{};
    double rrResetBeat_ = -1;
    long long samplesSeen_ = 0;
    std::array<Voice, kMaxVoices> voices_{};
    std::atomic<int> activeVoices_{0};
};

} // namespace sonora::devices
