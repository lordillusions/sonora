#pragma once
// DEV-28b: file-format facts of the reference's Wavetable (`InstrumentVector`), shared by the device (devices layer) and
// the set writer's templates (io layer). See docs/WAVETABLE.md "Format".

#include <array>

namespace sonora::model::wavetable
{
// `ModulationConnections` items, in the order every set and factory preset stores them (`TargetId` attributes).
inline constexpr std::array<const char*, 52> kTargetIds = {
    "Voice_Oscillator1_Pitch_PitchModulation", "Voice_Oscillator1_Wavetables_WavePosition", "Voice_Oscillator1_Effects_Effect1",
    "Voice_Oscillator1_Effects_Effect2", "Voice_Oscillator1_Pan", "Voice_Oscillator1_Gain", "Voice_Oscillator2_Pitch_PitchModulation",
    "Voice_Oscillator2_Wavetables_WavePosition", "Voice_Oscillator2_Effects_Effect1", "Voice_Oscillator2_Effects_Effect2",
    "Voice_Oscillator2_Pan", "Voice_Oscillator2_Gain", "Voice_SubOscillator_Tone", "Voice_SubOscillator_Gain",
    "Voice_Filter1_Frequency", "Voice_Filter1_Resonance", "Voice_Filter1_Drive", "Voice_Filter1_Morph", "Voice_Filter2_Frequency",
    "Voice_Filter2_Resonance", "Voice_Filter2_Drive", "Voice_Filter2_Morph", "Voice_Modulators_AmpEnvelope_Times_Attack",
    "Voice_Modulators_AmpEnvelope_Times_Decay", "Voice_Modulators_AmpEnvelope_Times_Release", "Voice_Modulators_AmpEnvelope_Sustain",
    "Voice_Modulators_Envelope2_Times_Attack", "Voice_Modulators_Envelope2_Times_Decay", "Voice_Modulators_Envelope2_Times_Release",
    "Voice_Modulators_Envelope2_Values_Initial", "Voice_Modulators_Envelope2_Values_Peak", "Voice_Modulators_Envelope2_Values_Sustain",
    "Voice_Modulators_Envelope2_Values_Final", "Voice_Modulators_Envelope3_Times_Attack", "Voice_Modulators_Envelope3_Times_Decay",
    "Voice_Modulators_Envelope3_Times_Release", "Voice_Modulators_Envelope3_Values_Initial", "Voice_Modulators_Envelope3_Values_Peak",
    "Voice_Modulators_Envelope3_Values_Sustain", "Voice_Modulators_Envelope3_Values_Final", "Voice_Modulators_Lfo1_Shape_Amount",
    "Voice_Modulators_Lfo1_Shape_Shaping", "Voice_Modulators_Lfo1_Time_UnifiedRateModulation", "Voice_Modulators_Lfo2_Shape_Amount",
    "Voice_Modulators_Lfo2_Shape_Shaping", "Voice_Modulators_Lfo2_Time_UnifiedRateModulation", "Voice_Modulators_TimeScale",
    "Voice_Modulators_Amount", "Voice_Unison_Amount", "Voice_Global_AmpModulation", "Voice_Global_PitchModulation", "Voice_Global_Glide"};

// The matrix rows' labels with no oscillator effect (the reference renames FX 1 / FX 2 per effect mode).
inline constexpr std::array<const char*, 52> kTargetNames = {
    "Osc 1 Pitch", "Osc 1 Pos", "Osc 1 FX 1", "Osc 1 FX 2", "Osc 1 Pan", "Osc 1 Gain", "Osc 2 Pitch", "Osc 2 Pos", "Osc 2 FX 1",
    "Osc 2 FX 2", "Osc 2 Pan", "Osc 2 Gain", "Sub Tone", "Sub Gain", "Flt 1 Freq", "Flt 1 Res", "Flt 1 Drive", "Flt 1 Morph",
    "Flt 2 Freq", "Flt 2 Res", "Flt 2 Drive", "Flt 2 Morph", "Amp Attack", "Amp Decay", "Amp Release", "Amp Sustain",
    "Env 2 Attack", "Env 2 Decay", "Env 2 Release", "Env 2 Initial", "Env 2 Peak", "Env 2 Sustain", "Env 2 Final", "Env 3 Attack",
    "Env 3 Decay", "Env 3 Release", "Env 3 Initial", "Env 3 Peak", "Env 3 Sustain", "Env 3 Final", "LFO 1 Amount", "LFO 1 Shaping",
    "LFO 1 Rate", "LFO 2 Amount", "LFO 2 Shaping", "LFO 2 Rate", "Time", "Global Mod Amount", "Unison Amount", "Amp", "Pitch",
    "Glide"};

inline constexpr int kSources = 13; // ModulationAmounts.0 .. 12
inline constexpr const char* kDefaultTable = "Basic Shapes"; // SpriteName of the reference's default preset
} // namespace sonora::model::wavetable
