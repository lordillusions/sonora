#pragma once
// Sonora Prism (DEV-18; named Sonora Wavetable until 2026-10-21, still the "SonoraWavetable" device tag): native synthesis guided by Serum 2's observable features and parameter scaling.
// Oscillator/filter/warp audio and generated tables are Sonora's own designs, not vendor DSP.
// No factory waveforms, presets or third-party source code are incorporated; no automatic VST3 replacement.

#include "devices/NativeDevice.h"
#include "devices/native/WavetableData.h"
#include "devices/native/WavetableDownsampler.h"
#include "devices/native/NoiseSampleVoice.h"
#include "devices/native/OscillatorMedia.h"
#include "devices/native/WavetableEffects.h"
#include "devices/native/WavetableVoiceMode.h"
#include "dsp/CurveGraph.h"
#include "dsp/ExponentialEnvelopeCurve.h"
#include "dsp/Dsp.h"
#include "dsp/ExponentialCurveGraph.h"
#include "dsp/LadderFilter.h"
#include "dsp/DiodeLadderFilter.h"
#include "dsp/MeasuredCombFilter.h"
#include "dsp/MeasuredMultiFilter.h"
#include "dsp/MeasuredSvfFilter.h"
#include "dsp/LfoTempoDivisions.h"
#include "dsp/WavetableMipTransition.h"

#include <array>
#include <atomic>
#include <random>
#include <vector>

namespace sonora::devices
{

class WavetableSynth final : public NativeDevice
{
public:
    static constexpr int kOscillators = 3, kVoices = 32, kUnison = 16, kFilters = 2;
    static constexpr int kEnvelopes = 4, kLfos = 10, kModSlots = 64, kMacros = 8, kRoutingSources = 7;
    static constexpr int kGeneratedTableFamilies = 4, kTableFamilies = kGeneratedTableFamilies + 1;
    static constexpr int kPreviewSamples = 256;
    enum TableFamily { Basic, Harmonic, Hollow, Formant, Custom };
    // FM / PD / AM / RM warps modulate an oscillator from another source (Osc1 / Osc2 = the other two oscillators in index order, Sub): measured Serum 2 laws.
    // DistTube..DistSoftSat (measured): the table sample through the FX Distortion shaper of that mode at drive = amount,
    // input x / 2, not renormalised (devices/native/SerumDistortion.h).
    enum Warp { WarpOff, Bend, Sync, Mirror, Pulse, PhaseMod, RingMod, FmSub, FmOsc1, FmOsc2, PdOsc1, PdOsc2, PdSub, AmOsc1, AmOsc2, AmSub, RmOsc1, RmOsc2, RmSub, SelfPd,
                DistTube, DistSoftClip, DistHardClip, DistDiode1, DistDiode2, DistZeroSquare, DistAsym, DistRectify, DistStompBox, DistTapeSat, DistSoftSat,
                // Measured: one-pole low/high pass on the oscillator at the voice rate, cutoff 41.5 * 2^(10 amount) Hz (LPF at 0 mutes).
                FilterLpfWarp, FilterHpfWarp,
                NumWarps };
    static constexpr bool isDistortionWarp(int warp) { return warp >= DistTube && warp <= DistSoftSat; }
    static float distortionWarp(int warp, float amount, float sample, float scale);
    static float distortionWarpScale(int warp, float amount);
    // MgLow6..24 is the Serum 2 "MG Low" ladder family (own implementation of measured behaviour, dsp/LadderFilter.h).
    // SerumLow6..SerumNotch24 are the Serum 2 "normal" family (measured, dsp/MeasuredSvfFilter.h; Morph is their Var).
    enum FilterType { Low12, Low24, High12, High24, BandPass, Notch, Morph, MgLow6, MgLow12, MgLow18, MgLow24,
                      SerumLow6, SerumLow12, SerumLow18, SerumLow24, SerumHigh6, SerumHigh12, SerumHigh18, SerumHigh24,
                      SerumBand12, SerumBand24, SerumNotch12, SerumNotch24,
                      // Ladder MG (measured: MG Low 24 without the ladder's resonance level loss, x0.80) and a level-only
                      // pass-through used for imported allpass/comb/phaser types that have no native model yet.
                      SerumLadderMg, FilterThrough,
                      // Dirty MG (measured: the 4-pole ladder at 0.841 x cutoff, x0.893, feedback rising steeply with resonance).
                      SerumDirtyMg,
                      // Ladder EMS (measured staggered ladder, dsp::StaggeredLadder).
                      SerumLadderEms,
                      // Measured peak / dual / morph types (dsp::MeasuredMultiFilter, same order as its Shape).
                      SerumPeak12, SerumPeak24, SerumLH6, SerumLH12, SerumLB12, SerumLP12, SerumLN12, SerumHB12, SerumHP12, SerumHN12,
                      SerumBP12, SerumBN12, SerumPP12, SerumPN12, SerumNN12, SerumLBH12, SerumLBH24, SerumLPH12, SerumLPH24,
                      SerumLNH12, SerumLNH24, SerumBPN12, SerumBPN24,
                      // Ladder Acid (measured diode ladder, dsp::DiodeLadderFilter).
                      SerumLadderAcid,
                      // Measured comb / flanger / phaser types (dsp::MeasuredCombFilter, same order as its Shape).
                      SerumCombP, SerumCombN, SerumCombL6P, SerumCombL6N, SerumCombH6P, SerumCombH6N, SerumCombHL6P, SerumCombHL6N,
                      SerumFlangeP, SerumFlangeN, SerumFlangeL6P, SerumFlangeL6N, SerumFlangeH6P, SerumFlangeH6N, SerumFlangeHL6P, SerumFlangeHL6N,
                      SerumPhase12P, SerumPhase12N, SerumPhase24P, SerumPhase24N, SerumPhase36P, SerumPhase36N, SerumPhase48P, SerumPhase48N,
                      SerumPhase48L6P, SerumPhase48L6N, SerumPhase48H6P, SerumPhase48H6N, SerumPhase48HL6P, SerumPhase48HL6N,
                      SerumFlangePhase12HL6P, SerumFlangePhase12HL6N, NumFilterTypes };
    static constexpr bool isLadderFilter(int type) { return (type >= MgLow6 && type <= MgLow24) || type == SerumLadderMg || type == SerumDirtyMg; }
    static float dirtyMgFeedback(float resonance); // ladder feedback / 4 for Dirty MG
    static constexpr bool isMeasuredSvfFilter(int type) { return type >= SerumLow6 && type <= SerumNotch24; }
    static constexpr bool isMeasuredMultiFilter(int type) { return type >= SerumPeak12 && type <= SerumBPN24; }
    static constexpr bool isMeasuredCombFilter(int type) { return type >= SerumCombP && type <= SerumFlangePhase12HL6N; }
    enum LfoMode { Global, Retrigger, OneShot };
    enum OscillatorMode { WavetableMode, SampleMode, MultisampleMode, GranularMode, SpectralMode, NumOscillatorModes };
    static constexpr int CustomLfoShape = dsp::CurveGraph::kShapeCount;
    static constexpr int kLfoDivisions = dsp::LfoTempoDivisions::count;
    enum Source
    {
        SourceOff, Env1, Env2, Env3, Env4, Lfo1, Lfo2, Lfo3, Lfo4, Lfo5, Lfo6, Lfo7, Lfo8, Lfo9, Lfo10,
        Velocity, Keytrack, ModWheel, Pressure, NoteRandom, Macro1, Macro2, Macro3, Macro4, Macro5, Macro6, Macro7, Macro8,
        NoteRandom2, NoteAlternate, NoteAlternate2, NoteRandomDiscrete, Fixed,
        // The second (Y) output of each LFO: the attractor's y coordinate for the chaos types, 0 otherwise (reference "LFO n Y").
        Lfo1Y, Lfo2Y, Lfo3Y, Lfo4Y, Lfo5Y, Lfo6Y, Lfo7Y, Lfo8Y, Lfo9Y, Lfo10Y, NumSources
    };
    enum Destination
    {
        DestOff, Pitch, Amplitude, StereoPan, Osc1Position, Osc2Position, Osc3Position,
        Osc1Level, Osc2Level, Osc3Level, Osc1Pitch, Osc2Pitch, Osc3Pitch, Osc1Pan, Osc2Pan, Osc3Pan,
        Osc1Warp1, Osc2Warp1, Osc3Warp1, Osc1Warp2, Osc2Warp2, Osc3Warp2,
        Filter1Cutoff, Filter2Cutoff, Filter1Resonance, Filter2Resonance, SubAmplitude, NoiseAmplitude,
        Osc1MediaScan, Osc2MediaScan, Osc3MediaScan, Osc1MediaPosition, Osc2MediaPosition, Osc3MediaPosition,
        Osc1GrainSize, Osc2GrainSize, Osc3GrainSize, Osc1GrainDensity, Osc2GrainDensity, Osc3GrainDensity,
        Osc1Bus1Send, Osc2Bus1Send, Osc3Bus1Send, NoiseBus1Send, SubBus1Send, Filter1Bus1Send, Filter2Bus1Send,
        Osc1Bus2Send, Osc2Bus2Send, Osc3Bus2Send, NoiseBus2Send, SubBus2Send, Filter1Bus2Send, Filter2Bus2Send,
        // Envelope stage targets, four per envelope (attack, decay, sustain, release).
        Env1Attack, Env1Decay, Env1Sustain, Env1Release, Env2Attack, Env2Decay, Env2Sustain, Env2Release,
        Env3Attack, Env3Decay, Env3Sustain, Env3Release, Env4Attack, Env4Decay, Env4Sustain, Env4Release,
        // An imported effect control; the slot's prepared FX target (setImportedFxSlot) says which one.
        FxParameter,
        // Appended (UI-28f): imported oscillator fine/detune/octave and filter mix targets (normalized add + clamp, unmeasured).
        Osc1Fine, Osc2Fine, Osc3Fine, Osc1Detune, Osc2Detune, Osc3Detune, Osc1Octave, Osc2Octave, Osc3Octave, Filter1Mix, Filter2Mix,
        // Appended (DEV-18i, measured against the hosted reference): LFO rate (quartic rate taper, sync divisions snap),
        // macro values (macro -> macro), master tuning (128 semitones per unit), filter output level (4 u^2) and drive,
        // and the global voice amplitude (linear in u, stored default 0.5). All add in the normalized domain and clamp.
        Lfo1Rate, Lfo2Rate, Lfo3Rate, Lfo4Rate, Lfo5Rate, Lfo6Rate, Lfo7Rate, Lfo8Rate, Lfo9Rate, Lfo10Rate,
        Macro1Value, Macro2Value, Macro3Value, Macro4Value, Macro5Value, Macro6Value, Macro7Value, Macro8Value,
        MasterTuning, Filter1Level, Filter2Level, Filter1Drive, Filter2Drive, VoiceAmp, NoisePanDest, SubPanDest, Filter1Var, Filter2Var,
        // LFO point mod buses 1..16 (measured): each bus moves the LFO curve points assigned to it (setImportedLfoPointAssignments).
        LfoPointBus1, LfoPointBus16 = LfoPointBus1 + 15,
        // Oscillator semitones (measured: a 25-step discrete control, floor(25 u) - 12) and the Sub's coarse pitch (128 st/unit).
        Osc1Semi, Osc2Semi, Osc3Semi, SubCoarse,
        // Per-source filter balance (measured: the filter 2 share (b + 100) / 200 plus the routes, clamped), A B C Noise Sub.
        Source1Balance, Source5Balance = Source1Balance + 4,
        // Unison blend (measured: stored 0..1 plus the routes) and stereo width (the -1..1 width moves 2 per unit), clamped.
        Osc1Blend, Osc2Blend, Osc3Blend, Osc1Width, Osc2Width, Osc3Width,
        // Global transpose (measured: a 49-step control over +-24 semitones, floor(49 u) - 24 with u = (stored + 24) / 48 + routes).
        GlobalTranspose,
        NumDestinations
    };
    enum Param
    {
        Volume, Transpose, BendUp, BendDown, Pan, FilterRouting,
        SubOn, SubWave, SubOctave, SubLevel, SubPan, SubDirect,
        NoiseOn, NoiseLevel, NoisePan, NoiseColor,
        OscStart
    };
    enum OscParam
    {
        On, Table, Position, Level, Octave, Semi, Fine, Unison, Detune, Blend, Width, OscPan, Phase, Random,
        Warp1Mode, Warp1Amount, Warp2Mode, Warp2Amount, RouteFilter1, RouteFilter2, OscParams
    };
    static constexpr int FilterStart = OscStart + kOscillators * OscParams;
    enum FilterParam { FilterOn, FilterMode, Cutoff, Resonance, Drive, FilterKeytrack, EnvAmount, EnvSource, FilterMix, FilterMorph, FilterParams };
    static constexpr int EnvStart = FilterStart + kFilters * FilterParams;
    enum EnvParam { Delay, Attack, Hold, Decay, Sustain, Release, AttackCurve, DecayCurve, ReleaseCurve, EnvParams };
    static constexpr int LfoStart = EnvStart + kEnvelopes * EnvParams;
    enum LfoParam { Shape, Rate, SyncOn, Division, Mode, LfoPhase, Bipolar, Fade, LfoParams };
    static constexpr int ModStart = LfoStart + kLfos * LfoParams;
    enum ModParam { ModSource, ModDestination, ModAmount, AuxSource, ModBipolar, ModParams };
    static constexpr int MacroStart = ModStart + kModSlots * ModParams;
    static constexpr int LegacyParameterCount = MacroStart + kMacros;
    static constexpr int MediaStart = LegacyParameterCount;
    enum MediaParam
    {
        MediaMode, MediaStartPosition, MediaLoopMode, MediaReverse, MediaOneShot, MediaScanRate,
        MediaGrainSize, MediaDensity, MediaDensityMode, MediaDensityBeats, MediaGrainLimit,
        MediaPositionSpread, MediaPitchSpread, MediaPanSpread, MediaDirectionSpread, MediaLengthSpread, MediaGainSpread,
        MediaWindow, MediaWindowAmount, MediaWindowSkew, MediaJumpStart, MediaVelocityTracking, MediaParams
    };
    static constexpr int CoarseStart = MediaStart + kOscillators * MediaParams;
    static constexpr int EnvLevelStart = CoarseStart + kOscillators;
    static constexpr int ModOutStart = EnvLevelStart + (kEnvelopes - 1) * 2;
    static constexpr int LfoAnchorStart = ModOutStart + kModSlots;
    static constexpr int LfoRateScaleStart = LfoAnchorStart + kLfos;
    static constexpr int VoiceModeStart = LfoRateScaleStart + kLfos;
    enum VoiceModeParam { MonoOn, LegatoOn, PortaAlways, PortaScaled, PortaTime, PortaCurve, NotePriority, VoiceModeParams };
    static constexpr int SourceRoutingStart = VoiceModeStart + VoiceModeParams;
    enum SourceRoutingParam { SourceDestination, SourceFilterBalance, SourceBus1, SourceBus2, SourceEnvelope, SourceRoutingParams };
    enum SourceRouteDestination { RouteToFilters, RouteToMain, RouteToDirect, RouteToNone };
    static constexpr int DirectGain = SourceRoutingStart + kRoutingSources * SourceRoutingParams;
    static constexpr int SourceRoutingOn = DirectGain + 1;
    static constexpr int FilterOutputStart = SourceRoutingOn + 1;
    static constexpr int SubPhase = FilterOutputStart + kFilters;
    static constexpr int SubContinuous = SubPhase + 1;
    static constexpr int NoisePitch = SubContinuous + 1;
    static constexpr int NoiseFine = NoisePitch + 1;
    static constexpr int NoiseKeytrack = NoiseFine + 1;
    static constexpr int NoisePhase = NoiseKeytrack + 1;
    static constexpr int NoiseRandom = NoisePhase + 1;
    // Sonora's own arpeggiator (appended; not a Serum 2 arp/clip import): held keys replay as a tempo-synced pattern.
    static constexpr int ArpStart = NoiseRandom + 1;
    enum ArpParam { ArpOn, ArpMode, ArpRate, ArpOctaves, ArpGate, ArpParams };
    enum ArpPattern { ArpUp, ArpDown, ArpUpDown, ArpDownUp, ArpAsPlayed, ArpRandom, NumArpPatterns };
    static constexpr int kArpRates = 6;
    static constexpr int arpParam(ArpParam p) { return ArpStart + p; }
    // Velocity and Note source curves (appended): input 0..1 -> output through start, a movable middle point and end
    // (piecewise linear, Serum's VELO/NOTE panels). Defaults are the identity, so existing patches are unchanged.
    static constexpr int CurveSourceStart = ArpStart + ArpParams;
    enum CurveSource { VelocityCurve, NoteCurve, CurveSources };
    enum CurveSourceParam { CurveStart, CurveMidX, CurveMidY, CurveEnd, CurveSourceParams };
    static constexpr int curveSourceParam(int curve, CurveSourceParam p) { return CurveSourceStart + curve * CurveSourceParams + p; }
    // Voice limit (appended, UI-28f): Serum's POLY. Imported presets set it from Global kParamPolyCount; own patches default to all voices.
    static constexpr int PolyphonyParam = CurveSourceStart + static_cast<int>(CurveSources) * static_cast<int>(CurveSourceParams);
    // Generated noise colours and LFO types (appended, UI-28g). Chaos LFOs integrate a Lorenz / Rossler attractor at the LFO
    // rate (own scaling, unmeasured); Random S&H holds a new random value each cycle.
    static constexpr int NoiseTypeParam = PolyphonyParam + 1;
    enum NoiseType { NoiseSampleType, NoiseWhite, NoisePink, NoiseBrown, NoiseGeiger, NumNoiseTypes };
    static constexpr int LfoTypeStart = NoiseTypeParam + 1;
    enum LfoType { LfoNormal, LfoLorenz, LfoRossler, LfoRandomHold, NumLfoTypes };
    static constexpr int lfoTypeParam(int lfo) { return LfoTypeStart + lfo; }
    static constexpr int ParameterCount = LfoTypeStart + kLfos;
    // A normalized attractor point (0..1 each) for drawing: the trajectory a chaos LFO follows.
    static const std::vector<std::array<float, 2>>& chaosTrajectory(int type);
    float curveSourceAt(int curve, float input) const; // UI: the current curve (atomic controls)
    static double arpRateBeats(int rate);
    static constexpr int filterOutputParam(int filter) { return FilterOutputStart + filter; }
    static constexpr int sourceRoutingParam(int source, SourceRoutingParam p) { return SourceRoutingStart + source * SourceRoutingParams + p; }
    static constexpr int voiceModeParam(VoiceModeParam p) { return VoiceModeStart + p; }
    static constexpr int outParam(int slot) { return ModOutStart + slot; }
    static constexpr int lfoAnchorParam(int lfo) { return LfoAnchorStart + lfo; }
    static constexpr int lfoRateScaleParam(int lfo) { return LfoRateScaleStart + lfo; }
    enum FxRoutingParam { MainToBus1, MainToBus2, Bus1ToMain, Bus2ToMain, MainGain, FxRoutingParams };
    static constexpr int maxFxBlockSize = 32768;
    static constexpr int coarseParam(int oscillator) { return CoarseStart + oscillator; }
    static constexpr int envLevelParam(int envelope, bool end)
    {
        return envelope > 0 && envelope < kEnvelopes ? EnvLevelStart + (envelope - 1) * 2 + (end ? 1 : 0) : -1;
    }
    enum class ImportedModulationDomain { Native, CoarsePitch128, SquaredLevel, Pan2, FilterFrequency, SourceSend, Unit, EnvelopeTime,
                                          FineCents200, DetuneCurve, OctaveSteps };
    static constexpr int sourceSendDestination(int source, int bus) { return Osc1Bus1Send + bus * kRoutingSources + source; }
    static constexpr int oscParam(int oscillator, OscParam p) { return OscStart + oscillator * OscParams + p; }
    static constexpr int filterParam(int filter, FilterParam p) { return FilterStart + filter * FilterParams + p; }
    static constexpr int envParam(int envelope, EnvParam p) { return EnvStart + envelope * EnvParams + p; }
    static constexpr int lfoParam(int lfo, LfoParam p) { return LfoStart + lfo * LfoParams + p; }
    static constexpr int modParam(int slot, ModParam p) { return ModStart + slot * ModParams + p; }
    static constexpr int macroParam(int macro) { return MacroStart + macro; }
    static constexpr int mediaParam(int oscillator, MediaParam p) { return MediaStart + oscillator * MediaParams + p; }

    // Observed host normalized -> stored plain scalar relationships. These are not patch defaults.
    static float observedLevel(float normalized);
    static float observedEnvelopeSeconds(float normalized);
    static float observedLfoHertz(float normalized);
    static float observedTableIndex(float normalized);
    static float observedPanGain(float normalized, bool right);
    static float observedFilterHertz(float normalized, double sampleRate = 44100);
    static double divisionBeats(int division);
    static const char* tableName(int table);
    // Table family 0: the classic stepped shape set (saw, sine, triangle, split ramp, square, 1/4..1/32 pulses), generated from
    // elementary formulas here (no vendor file). The oscillator selects whole frames, like a stepped imported table.
    static std::shared_ptr<const WavetableData> defaultShapesTable();
    static const char* sourceName(int source);
    static const char* destinationName(int destination);
    static const char* filterTypeName(int type);

    WavetableSynth();
    std::string name() const override { return "Sonora Prism"; }
    bool isInstrument() const override { return true; }
    void prepare(double sampleRate, int maxBlockSize) override;
    void reset() override;
    void setHostInfo(const HostInfo& info) override { host_ = info; }
    void process(juce::AudioBuffer<float>& audio, juce::MidiBuffer& midi) override;
    int latencySamples() const override;
    const std::vector<ParameterInfo>& parameters() const override;
    float getParameter(int index) const override;
    void setParameter(int index, float plainValue) override;
    std::vector<int> displayParameters() const override;
    std::vector<UiGroup> uiGroups() const override;
    bool parameterShown(int index) const override;

    // Safe on the UI/message thread: own immutable bank plus current atomic parameter values.
    float waveformAt(int oscillator, float phase) const;
    float waveformAt(int oscillator, float phase, int channel) const;
    float subWaveformAt(float phase) const;
    std::array<float, kPreviewSamples> wavePreview(int oscillator) const;
    // Preparation/message thread, only before graph publication (or while audio is explicitly suspended).
    // These immutable owners are never changed/copied/released by process(). Null clears a custom table.
    bool setCustomTable(int oscillator, std::shared_ptr<const WavetableData> table);
    const WavetableData* customTable(int oscillator) const;
    std::string customTableName(int oscillator) const;
    // Immutable asset preparation before publication or while processing is suspended.
    bool setNoiseSample(std::shared_ptr<const OscillatorSample>);
    const OscillatorSample* noiseSample() const { return noiseSample_.get(); }
    std::string noiseSampleName() const;
    float noiseWaveformAt(float normalizedPosition, int channel = -1) const;
    bool setOscillatorMedia(int oscillator, std::shared_ptr<const OscillatorMedia> media);
    const OscillatorMedia* oscillatorMedia(int oscillator) const;
    std::string oscillatorMediaName(int oscillator) const;
    // Preparation/message thread only, before publication or with audio explicitly suspended.
    // Previous indices 0..608 stay fixed; endpoint levels append before dynamic FX controls.
    bool setEffectsConfiguration(const WavetableEffects::Configuration&, std::string& error);
    WavetableEffects::Configuration effectsConfiguration() const;
    bool hasEffectsConfiguration() const { return effects_ != nullptr; }
    const WavetableEffects* effects() const { return effects_.get(); }
    const std::string& effectsPreparationError() const { return effectsPreparationError_; }
    int fxRoutingParameterIndex(FxRoutingParam) const;
    int fxEnabledParameterIndex(WavetableEffects::ModuleId) const;
    int fxParameterIndex(WavetableEffects::ModuleId, int descriptorIndex) const;
    // Prepared import configuration; ordinary own patches retain their original matrix units.
    bool setImportedModulationDomain(int destination, ImportedModulationDomain);
    // Imported FX targets (preparation thread, before publication). Each curve maps the route sum position u (0..1, sampled
    // evenly) to a native control of `module`; routes whose slot points at the target add amount x source to `base`.
    struct FxModulationCurve { int parameter = -1; std::vector<float> values; };
    int addImportedFxTarget(WavetableEffects::ModuleId module, float base, std::vector<FxModulationCurve> curves);
    bool setImportedFxSlot(int slot, int target);
    void clearImportedFxTargets() { fxTargets_.clear(); fxSlotTargets_ = filledSlots(); }
    int importedFxSlot(int slot) const { return slot >= 0 && slot < kModSlots ? fxSlotTargets_[static_cast<size_t>(slot)] : -1; }
    void setImportedEnvelopeProfile(bool enabled); // preparation/message thread, before publication
    bool usesImportedEnvelopeProfile() const { return importedEnvelopeProfile_; }
    float envelopeCurveAt(int envelope, EnvParam curve, float phase) const; // UI: reads current atomic controls
    void setImportedLfoProfile(bool enabled); // prepared timing/curve adapter; own curves keep their law
    bool usesImportedLfoProfile() const { return importedLfoProfile_; }
    void setImportedVoiceProfile(bool enabled) { importedVoiceProfile_ = enabled; }
    static constexpr int kImportedKeytrackCenter = 32; // filter keytrack reference note of the imported voice profile
    // Reference Global kParamLimitSameNotePolyphony: on, a new note fades out the still-sounding voices of its pitch over 20 ms (measured).
    void setImportedSameNoteLimit(bool enabled) { importedSameNoteLimit_ = enabled; }
    void setImportedPolyphony(int voices) { setParameter(PolyphonyParam, static_cast<float>(std::clamp(voices, 1, kVoices))); }
    // Reference Global kParamModWheel (0..100): the wheel position a preset is stored with; MIDI CC 1 overrides it.
    void setImportedModWheel(float value);
    // Reference LFO kParamDotted / kParamTriplets: which sync divisions a modulated synced rate may snap to (measured).
    void setImportedLfoSyncDivisions(int lfo, bool dotted, bool triplets);
    // Reference kParamFreq (0..1, 8.2 Hz .. 22 kHz): kept exactly because the Cutoff control stops at 20 Hz; imported
    // cutoff routes add to it while the control still shows the imported value.
    void setImportedFilterCutoffPosition(int filter, float normalized);
    // Stored Global Voice Amp (0..1, default 0.5): gain 2 u with u the stored value plus the Voice Amp routes (measured).
    void setImportedVoiceAmp(float stored) { importedVoiceAmp_ = std::isfinite(stored) ? std::clamp(stored, 0.0f, 1.0f) : 0.5f; }
    // Per-voice detune cents for the stored Detune (cents at range 2), the Detune route sum and the Unison Range scale (range / 2).
    static float importedDetuneCents(float detune, float modulation, float scale);
    // Unison Range (measured: per-voice cents = 100 range d^1.25; the Detune parameter holds the range-2 value 200 d^1.25).
    void setImportedDetuneScale(int oscillator, float scale)
    {
        if (oscillator >= 0 && oscillator < kOscillators)
            importedDetuneScale_[static_cast<size_t>(oscillator)] = std::isfinite(scale) ? std::max(0.0f, scale) : 1.0f;
    }
    // Reference sampler envelope (MultiSampleOsc kParamEnvOverride on): attack / release seconds replacing the SFZ's own.
    void setImportedMediaEnvelope(int oscillator, double attackSeconds, double releaseSeconds);
    // Reference lfoPointModAssignments: bus (0..15) -> LFO curve point; target 0 moves x, 1 moves y, 2 bends the curve
    // of the segment starting at the point (bus value added to the stored 0..1 coordinate, measured).
    struct LfoPointAssignment { int bus = -1, lfo = -1, point = -1, target = 0; };
    static constexpr int kLfoPointAssignments = 32;
    void setImportedLfoPointAssignments(const std::vector<LfoPointAssignment>& assignments); // preparation thread
    void setImportedSubProfile(bool enabled);
    // Imported warp menu names for display (preparation thread): the native mode the importer chose (WarpOff when the
    // reference warp has no native counterpart yet) and the reference's own label, e.g. {Bend, "BEND+"} or {WarpOff, "DIST TUBE"}.
    struct WarpLabel { int mode = WarpOff; std::string label; bool approximate = false; };
    void setImportedWarpLabel(int oscillator, int warp, WarpLabel label);
    WarpLabel importedWarpLabel(int oscillator, int warp) const;
    void setImportedNoiseProfile(bool enabled) { importedNoiseProfile_ = enabled; if (enabled) noiseSampleExpected_ = true; }
    bool usesImportedNoiseProfile() const { return importedNoiseProfile_; }
    void setNoiseSampleExpected(bool expected) { noiseSampleExpected_ = expected; } // preparation only
    bool expectsNoiseSample() const { return noiseSampleExpected_; }
    // Immutable voice preparation; effects remain at host rate. Reprepare before publication.
    bool setVoiceRateFactor(int factor);
    int requestedVoiceRateFactor() const { return requestedVoiceRateFactor_; }
    int voiceRateFactor() const { return voiceRateFactor_; }
    const std::string& voiceRatePreparationError() const { return voiceRatePreparationError_; }
    bool setImportedMasterLevel(double storedLevel); // preparation only; preserves measured values below the dB knob floor
    bool setLfoCurve(int lfo, const dsp::CurveGraph& curve);
    void clearLfoCurve(int lfo);
    bool hasCustomLfoCurve(int lfo) const;
    dsp::CurveGraph lfoCurve(int lfo) const;
    enum class CurveLaw { NativePower, ImportedExponential };
    struct ModulationCurve
    {
        dsp::CurveGraph graph = dsp::CurveGraph::preset(3);
        bool enabled = false;
        CurveLaw law = CurveLaw::NativePower;
    };
    // Immutable curve preparation; never edit published curves from a parameter callback.
    bool setModulationCurve(int slot, bool auxiliary, const dsp::CurveGraph&, bool enabled, CurveLaw);
    ModulationCurve modulationCurve(int slot, bool auxiliary) const;
    float modulationCurveAt(int slot, bool auxiliary, float source) const;
    // Imported aux "Inverted" (measured, DEV-18i): the via value is 1 - curve(aux source), curve first. Kept apart from the
    // curve so curve edits keep it.
    void setAuxiliaryInverted(int slot, bool inverted);
    bool auxiliaryInverted(int slot) const;
    unsigned activeVoiceCount() const { return activeVoiceCount_.load(std::memory_order_relaxed); }
    // Editor keyboard, wheels and arp view: message thread -> audio thread, lock-free single producer; consumed at the next block start
    // (before the arpeggiator), so the device plays whether or not its track is armed. False when the queue is full.
    bool queueUiMidi(uint8_t status, uint8_t data1, uint8_t data2 = 0) noexcept;
    // UI display state published by process(): keys held by voices or the arpeggiator, the last wheel/bend values.
    bool noteLit(int note) const noexcept;
    float modWheelDisplay() const noexcept { return wheelDisplay_.load(std::memory_order_relaxed); }
    float pitchBendDisplay() const noexcept { return bendDisplay_.load(std::memory_order_relaxed); }
    int arpNoteDisplay() const noexcept { return arpNoteDisplay_.load(std::memory_order_relaxed); } // the arpeggiator's sounding note, -1: none
    // Live display (UI-28c): the newest sounding voice as of the last block, so the editor can animate knobs, filter curves,
    // the wavetable frame and ENV/LFO playheads like Serum. Published with relaxed atomics; readers get a consistent-enough
    // snapshot for drawing (values may straddle two blocks).
    struct LiveState
    {
        bool active = false;      // a voice is sounding
        int note = -1;
        float velocity = 0;                                    // the voice's input velocity 0..1 (before the VELO curve)
        std::array<float, kModSlots> route{};                 // each slot's current source value after curves/via/out (0 when idle)
        std::array<int, kEnvelopes> envelopeStage{};          // 0 idle, 1 delay, 2 attack, 3 hold, 4 decay, 5 sustain, 6 release, 7 end
        std::array<float, kEnvelopes> envelopeElapsed{}, envelopeLevel{}; // seconds into the stage; level before the native square law
        std::array<float, kLfos> lfoPhase{}, lfoValue{};
        std::array<std::array<float, 2>, kLfos> lfoPoint{};    // chaos LFOs: the attractor position (normalized, as in chaosTrajectory)
        std::array<float, 2> outputPeak{};                     // block peak per channel
    };
    LiveState liveState() const noexcept;
    // Unwarped-table preview at an explicit (modulated) position and warp amounts (UI thread).
    float waveformAt(int oscillator, float phase, float position, float warp1, float warp2) const;
    float tableFramePosition(int oscillator, float position) const;
    // Table display (UI thread): frames of the oscillator's current table, one unwarped frame, and the frame position the
    // Position control selects (whole frames for stepped tables).
    int tableFrameCount(int oscillator) const;
    float tableFrameAt(int oscillator, int frame, float phase) const;
    float tableFramePosition(int oscillator) const;

private:
    struct TableBank;
    // releaseTime < 0: the Release parameter; a forced release continues from releaseOffset (release progress) at 1 / releaseTime.
    struct EnvelopeState { int stage = 0; double elapsed = 0; float level = 0, start = 0, attackStart = -1, releaseTime = -1, releaseOffset = 0; };
    struct OscControl
    {
        int table = 0, count = 1, warp1 = 0, warp2 = 0, mip = 0;
        int mediaMode = -1;
        int mediaRootNote = 60;
        bool on = false, route1 = true, route2 = false;
        bool mediaReleased = false;
        float mediaSeekPosition = 0, mediaLeft = 1, mediaRight = 1;
        OscillatorMediaVoice::Controls mediaControls{};
        float position = 0, level = 0, targetPosition = 0, targetLevel = 0, warpAmount1 = 0, warpAmount2 = 0, distScale1 = 1, distScale2 = 1;
        std::array<float, 2> warpFilterG{}; // one-pole coefficient per warp slot (FilterLpfWarp / FilterHpfWarp)
        std::array<std::array<float, 2>, 2> warpFilterState{}; // [slot][channel]
        std::array<double, kUnison> increment{};
        std::array<dsp::WavetableMipTransition, kUnison> mipTransition{};
        double bandwidthScale = 1;
        std::array<float, kUnison> left{}, right{};
    };
    struct Chaos { double x = 0, y = 0, z = 0; bool started = false; };
    static void advanceChaos(Chaos&, int type, double cycles, uint32_t& seed, float& held);
    static std::array<float, 2> chaosPoint(const Chaos&, int type);
    struct Voice
    {
        int note = -1, channel = 0, controlCountdown = 0, midiVelocity = 0;
        bool keyDown = false, released = false;
        uint64_t age = 0;
        uint64_t noteSamples = 0;
        double pitchNote = 60;
        float velocity = 0, pressure = 0, random = 0, gain = 0, targetGain = 0, panLeft = 1, panRight = 1;
        float random2 = 0, alternate = 0; // NoteOn Rand2 and NoteOn Alt. (Alt.2 = 1 - alternate), latched per voice
        bool counted = true; // false once cut by the same-note limit: the reference's bookkeeping no longer sees it
        std::array<std::array<float, 4>, kEnvelopes> envelopeStages{}; // modulated attack/decay/sustain/release per envelope (imported routes)
        bool envelopeStagesModulated = false;
        bool cutHeld = false; int64_t cutReleasedAt = -1; // cut while held: still a held voice of its pitch until its fade ends, then a ghost
        float subLevel = 0, subLeft = 1, subRight = 1, noiseLevel = 0, noiseLow = 0;
        float noiseLeft = 1, noiseRight = 1;
        double noiseRatio = 1;
        NoiseSampleVoice noiseSample;
        double subIncrement = 0, subPhase = 0;
        std::array<EnvelopeState, kEnvelopes> envelope{};
        std::array<double, kLfos> lfoPhase{};
        std::array<double, kLfos> lfoIncrement{}; // per-voice phase increment (the block rate unless an LFO Rate route moves it)
        std::array<float, kLfos> lfoValue{}, lfoY{};
        std::array<float, kMacros> macroValue{}; // macro values after macro -> macro routes (read only while macroModulated)
        bool macroModulated = false;
        std::array<float, kFilters> filterLevel{1, 1}, filterVar{};
        std::array<float, 5> filterShare{}; // filter 2 share per filter-routed source (A, B, C, Noise, Sub)
        std::array<Chaos, kLfos> chaos{};
        std::array<float, kLfos> heldRandom{};
        std::array<float, 3> pinkNoise{};
        float brownNoise = 0, geiger = 0;
        std::array<std::array<double, kUnison>, kOscillators> phase{};
        std::array<float, kOscillators> lastOscillator{};
        std::array<std::array<float, kUnison>, kOscillators> selfFeedback{}; // previous raw table value per unison voice (Self PD)
        std::array<OscControl, kOscillators> oscillator{};
        std::array<std::array<dsp::Svf, 2>, kFilters> filter{}, filterSecond{};
        std::array<std::array<dsp::LadderFilter, 2>, kFilters> ladder{};
        std::array<std::array<dsp::MeasuredSvfFilter, 2>, kFilters> measured{};
        std::array<std::array<dsp::StaggeredLadder, 2>, kFilters> staggered{};
        std::array<std::array<dsp::MeasuredMultiFilter, 2>, kFilters> multi{};
        std::array<std::array<dsp::DiodeLadderFilter, 2>, kFilters> diode{};
        std::array<std::array<dsp::MeasuredCombFilter, 2>, kFilters> comb{};
        std::array<float, kFilters> filterDrive{}, filterMix{}, filterResonance{};
        std::array<std::array<float, 2>, kRoutingSources> sourceSends{};
        uint32_t noiseSeed = 1;
        double serumNoiseRandom = -1; // imported profile: the note's Noise draw from the shared stream (< 0: none)
        // FX route modulation state, advanced only while this voice is the selected FX voice (see applyFxModulation).
        bool fxStarted = false;
        std::array<EnvelopeState, kEnvelopes> fxEnvelope{};
        std::array<double, kLfos> fxLfoPhase{};
        uint64_t fxNoteSamples = 0;
    };
    struct ModRoute { int slot = 0, source = 0, destination = 0, auxiliary = 0; float amount = 0, output = 1; bool bipolar = false; };
    struct FxBinding { WavetableEffects::ModuleId module = 0; int parameter = -1, routing = -1; };
    struct FxTarget { WavetableEffects::ModuleId module = 0; float base = 0, applied = -1; std::vector<FxModulationCurve> curves; };
    struct SourceRoute
    {
        int destination = RouteToMain;
        float filter1 = 1, filter2 = 0;
        bool envelope = true;
    };
    void snapshot();
    void startVoice(Voice&, int note, int channel, int velocity, bool monoRetrigger = false);
    void applyMonoTransition(const WavetableVoiceMode::Transition&);
    void event(const uint8_t* data, int size);
    void render(juce::AudioBuffer<float>& audio, int from, int to);
    void renderVoices(float* left, float* right, int from, int to, int outputOffset,
                      float* directSubLeft = nullptr, float* directSubRight = nullptr,
                      float* bus1Left = nullptr, float* bus1Right = nullptr,
                      float* bus2Left = nullptr, float* bus2Right = nullptr);
    void renderPreparedVoices(float* left, float* right, int from, int to, int outputOffset,
                              float* directLeft = nullptr, float* directRight = nullptr,
                              float* bus1Left = nullptr, float* bus1Right = nullptr,
                              float* bus2Left = nullptr, float* bus2Right = nullptr);
    void resetEffects();
    void updateControl(Voice& voice, int sample);
    void release(Voice& voice);
    float nextEnvelope(EnvelopeState& state, int envelope, bool released, const std::array<float, 4>* stages = nullptr) const;
    float sourceValue(const Voice& voice, int source, int slot = 0) const;
    float routeValue(const Voice& voice, const ModRoute& route) const; // after curves, polarity, aux and output scaling
    void selectFxVoice();
    void applyFxModulation(int frames);
    void updateLfoValues(Voice& voice, const std::array<float, 16>* pointBuses = nullptr);
    float filterSample(Voice& voice, int filter, int channel, float input) const;
    static float warpedPhase(float phase, int mode, float amount, float modulator, bool measured = false);
    static float randomSigned(uint32_t& seed);
    const WavetableData* oscillatorTable(int oscillator, int table) const;
    // Arpeggiator (audio thread): held keys, countdowns in host samples to the next step / the sounding note's end.
    static constexpr int kArpHeld = 32;
    struct ArpState
    {
        std::array<uint8_t, kArpHeld> notes{}, velocities{};
        int count = 0, step = 0, sounding = -1;
        double untilStep = 0, untilOff = -1;
        uint32_t seed = 0x2545f491u;
        bool active = false;
    };
    bool arpInput(const uint8_t* data, int size); // true: consumed by the arpeggiator
    void arpAdvance(double samples);
    void arpStep();
    void arpStop();
    void renderSpan(juce::AudioBuffer<float>& audio, int& position, int to, bool bypass);
    void input(const uint8_t* data, int size) { if (! arp_.active || ! arpInput(data, size)) event(data, size); }
    ArpState arp_;
    static constexpr uint32_t kUiMidiCapacity = 256;
    std::array<std::atomic<uint32_t>, kUiMidiCapacity> uiMidi_{};
    std::atomic<uint32_t> uiMidiWrite_{0}, uiMidiRead_{0};
    std::array<std::atomic<uint32_t>, 4> litNotes_{};
    std::atomic<float> wheelDisplay_{0}, bendDisplay_{0};
    std::atomic<int> arpNoteDisplay_{-1};
    std::array<std::array<WarpLabel, 2>, kOscillators> warpLabels_{};
    std::atomic<bool> liveActive_{false};
    std::atomic<int> liveNote_{-1};
    std::atomic<float> liveVelocity_{0};
    float curveSource(int curve, float input) const noexcept; // audio thread: block_ snapshot
    std::array<std::atomic<float>, kModSlots> liveRoute_{};
    std::array<std::atomic<int>, kEnvelopes> liveEnvelopeStage_{};
    std::array<std::atomic<float>, kEnvelopes> liveEnvelopeElapsed_{}, liveEnvelopeLevel_{};
    std::array<std::atomic<float>, kLfos> liveLfoPhase_{}, liveLfoValue_{}, liveLfoPointX_{}, liveLfoPointY_{};
    std::array<std::atomic<float>, 2> liveOutputPeak_{};
    void publishLiveState(const juce::AudioBuffer<float>& audio);
    std::shared_ptr<const WavetableData> defaultShapes_ = defaultShapesTable();
    const TableBank* tables_ = nullptr;
    std::array<std::shared_ptr<const WavetableData>, kOscillators> customTables_{};
    std::array<std::shared_ptr<const OscillatorMedia>, kOscillators> oscillatorMedia_{};
    std::shared_ptr<const OscillatorSample> noiseSample_;
    bool noiseSampleExpected_ = false;
    using MediaVoices = std::array<std::array<OscillatorMediaVoice, kOscillators>, kVoices>;
    std::unique_ptr<MediaVoices> mediaVoices_;
    std::unique_ptr<WavetableEffects> effects_;
    std::vector<ParameterInfo> fxInfos_;
    std::vector<FxBinding> fxBindings_;
    std::vector<FxTarget> fxTargets_;
    std::array<int, kModSlots> fxSlotTargets_ = filledSlots();
    static constexpr std::array<int, kModSlots> filledSlots() { std::array<int, kModSlots> slots{}; for (auto& s : slots) s = -1; return slots; }
    Voice fxVoice_{}; // monophonic modulation voice for FX routes (note < 0: no note yet, sources read zero)
    static constexpr int kFxModulationPeriod = 16; // WavetableEffects re-derives module controls every 16 samples
    uint64_t fxModulationClock_ = 0; // samples processed by the graph since its reset (aligns the sub-blocks)
    int fxVoiceSlot_ = -1, fxDiscreteSlot_ = -1; // the selected voice; the last selected one (its Discrete draws feed FX routes)
    std::array<std::vector<float>, WavetableEffects::busCount * 2> fxBuffers_;
    std::array<std::vector<float>, 2> directSubBuffers_;
    std::array<dsp::FractionalDelay, 2> directSubAlignment_;
    std::string effectsPreparationError_;
    int fxBlockCapacity_ = 512, directSubLatency_ = 0;
    bool effectsReady_ = false, masterGainInitialized_ = false;
    std::array<bool, 2> fxReturnBeforeMain_{};
    float masterGain_ = 1;
    std::array<dsp::CurveGraph, kLfos> customCurves_{};
    std::array<bool, kLfos> hasCustomCurves_{};
    std::array<Voice, kVoices> voices_{};
    // Comb / flanger delay lines: (kVoices + 1 for the FX voice) x kFilters x 2 channels, allocated in prepare().
    // Mutable: filterSample() is const but writes the line of the voice it renders.
    mutable std::vector<float> combLines_;
    int combLineLength_ = 0;
    float* combLine(const Voice& voice, int filter, int channel) const;
    WavetableVoiceMode monoVoice_;
    bool monoActive_ = false, importedVoiceProfile_ = false, importedSameNoteLimit_ = false, importedSubProfile_ = false, importedNoiseProfile_ = false;
    double retainedSubPhase_ = 0;
    float importedSubSineScale_ = 1;
    std::array<float, ParameterCount> block_{};
    std::array<SourceRoute, kRoutingSources> sourceRoutes_{};
    std::array<dsp::CurveGraph, kLfos> curves_{};
    bool importedLfoProfile_ = false;
    std::array<dsp::ExponentialCurveGraph, kLfos> importedLfoCurves_{};
    std::array<ModRoute, kModSlots> routes_{};
    std::array<std::array<ModulationCurve, 2>, kModSlots> modulationCurves_{};
    std::array<std::array<dsp::ExponentialCurveGraph, 2>, kModSlots> exponentialModulationCurves_{};
    std::array<std::atomic<bool>, kModSlots> auxiliaryInverted_{};
    std::array<ImportedModulationDomain, NumDestinations> importedDomains_{};
    bool importedEnvelopeProfile_ = false;
    float importedMasterDb_ = -60, importedMasterCorrection_ = 1;
    float masterGainFor(float db) const;
    std::array<std::array<dsp::ExponentialEnvelopeCurve, 3>, kEnvelopes> importedEnvelopeCurves_{};
    int routeCount_ = 0;
    std::array<float, 16> bends_{}, wheels_{}, pressures_{};
    float importedModWheel_ = 0;
    std::array<std::array<bool, 2>, kLfos> importedLfoSync_{}; // dotted, triplets
    std::array<float, kFilters> importedCutoffPosition_{-1, -1};
    float importedVoiceAmp_ = 0.5f;
    std::array<float, kOscillators> importedDetuneScale_{1, 1, 1};
    std::array<std::array<double, 2>, kOscillators> importedMediaEnvelope_{{{-1, -1}, {-1, -1}, {-1, -1}}};
    std::array<LfoPointAssignment, kLfoPointAssignments> lfoPointAssignments_{};
    int lfoPointAssignmentCount_ = 0;
    std::array<bool, 16> pedals_{};
    std::array<double, kLfos> globalLfoPhase_{}, lfoIncrement_{};
    std::array<double, kLfos> processingEndLfoPhase_{};
    bool hasHostClock_ = false;
    double expectedHostBeat_ = 0;
    std::array<float, kLfos> globalLfoValue_{};
    std::array<Chaos, kLfos> globalChaos_{};
    std::array<float, kLfos> globalHeld_{};
    uint32_t chaosSeed_ = 0x9e3779b9u;
    HostInfo host_;
    double sampleRate_ = 44100, hostSampleRate_ = 44100;
    int requestedVoiceRateFactor_ = 1, voiceRateFactor_ = 1;
    WavetableDownsampler voiceDownsampler_;
    std::array<std::vector<float>, WavetableDownsampler::laneCount> internalVoiceBuffers_;
    std::string voiceRatePreparationError_;
    float masterSmoothCoefficient_ = 1;
    float smoothCoefficient_ = 1;
    uint64_t age_ = 0, sampleClock_ = 0;
    uint32_t randomSeed_ = 0x13579bdu;
    // Imported profile: the reference's single engine-wide std::mt19937_64 (default seed 5489), consumed in the measured order.
    // Advanced only on the message-free audio path by note starts.
    static constexpr int kSerumInitialDraws = 320, kSerumOscResetDraws = 16, kSerumOscMediaDraws = 80, kSerumNoteTailDraws = 3, kSerumReleaseDraws = 3,
        kSerumMonoRetriggerTailDraws = 51, kSerumLegatoDraws = 4;
    static constexpr float kSameNoteFadeSeconds = 0.02f;
    std::mt19937_64 serumRandom_{5489};
    float serumUniform() noexcept { return static_cast<float>(serumRandom_()) * 5.421011e-20f; }
    // NoteOn Rand (Discrete), one value per matrix slot (measured): the draws of a note start are handed to the voice started before it only
    // when the next note starts; a new voice keeps what its slot last held (zero at first). Kept per voice slot, outside Voice resets.
    std::array<std::array<float, kModSlots>, kVoices> discreteRandom_{};
    std::array<float, kModSlots> pendingDiscreteRandom_{};
    int lastStartedVoice_ = -1;
    bool alternateState_ = true; // NoteOn Alt. of the next note (the first note reads 1)
    int polyphony_ = kVoices; // PolyphonyParam, snapshotted per block
    // Voices cut (or pedal-reused) while held stay alive, silent, in the reference's note-off bookkeeping until a note-off of their pitch
    // releases them; a released ghost no longer counts.
    struct SameNoteGhost { int note = -1, channel = 0; };
    std::array<SameNoteGhost, kVoices> ghosts_{};
    void importedNoteOff(int note, int channel, bool pedalUp = false);
    void addSameNoteGhost(int note, int channel);
    bool wasBypassed_ = false;
    std::atomic<unsigned> activeVoiceCount_{0};
};

} // namespace sonora::devices
