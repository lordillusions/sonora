#include "devices/native/Simpler.h"

#include <algorithm>
#include <cmath>
#include <span>

namespace sonora::devices
{

namespace
{
// Playback interpolator (DEV-22): at the original pitch the reference applies a symmetric 5-tap kernel (fit residual
// -149 dB) that is exactly a Kaiser-windowed sinc (half length 3, cutoff 0.9 x Nyquist, beta 8.612, gain 0.99).
// Pitched up, the kernel is stretched by the step (anti-aliasing); this reproduces the reference's spectra within
// ~0.2 dB from -12 to +24 st, although its fractional-position kernels are not identical (null ~ -10 dB).
constexpr int kKernelHalf = 3, kKernelResolution = 512;
constexpr double kKernelCutoff = 0.9, kKernelBeta = 8.612, kKernelGain = 0.99;
const std::array<float, 2 * kKernelHalf * kKernelResolution + 2>& kernelTable()
{
    static const auto table = [] {
        std::array<float, 2 * kKernelHalf * kKernelResolution + 2> t{};
        const auto bessel = [](double x) { double sum = 1, term = 1; for (int k = 1; k < 40; ++k) { term *= (x / (2 * k)) * (x / (2 * k)); sum += term; } return sum; };
        for (size_t i = 0; i < t.size(); ++i)
        {
            const double u = (double) i / kKernelResolution - kKernelHalf; // -3 .. 3
            const double r = u / kKernelHalf;
            const double window = std::abs(r) < 1 ? bessel(kKernelBeta * std::sqrt(1 - r * r)) / bessel(kKernelBeta) : 0.0;
            const double x = kKernelCutoff * u * juce::MathConstants<double>::pi;
            t[i] = (float) (kKernelGain * kKernelCutoff * (std::abs(x) < 1e-12 ? 1.0 : std::sin(x) / x) * window);
        }
        return t;
    }();
    return table;
}
// Envelope slope curves (DEV-22 measurements): |slope| < 1 blends linear with exp(-kSlopeRate x); slope +1 approaches
// the target dB-linearly down to kSlopeFloor; voices end once the release falls below -60 dB.
constexpr double kSlopeRate = 9.115, kSlopeFloor = 6.3e-5;
// The pitch envelope's slope +1 stages stop at a higher floor (measured: +12 st, 300 ms decay falls at 30 / s, not 32).
constexpr double kPitchSlopeFloor = 1.23e-4;
constexpr float kReleaseCut = 0.001f;
// Voices chooser entries (enum index -> voice count). Index 5 = 6 voices is the reference default.
constexpr int kVoiceCounts[] = {1, 2, 3, 4, 5, 6, 7, 8, 10, 12, 14, 16, 24, 32}; // indices 0-11 measured
constexpr int kVoiceChoices = (int) (sizeof kVoiceCounts / sizeof kVoiceCounts[0]);
// LFO "Beats" rates in quarter notes, by BeatRate enum index (all measured).
constexpr double kBeatLengths[] = {1.0 / 16, 1.0 / 12, 1.0 / 8, 1.0 / 6, 1.0 / 4, 1.0 / 3, 1.0 / 2, 2.0 / 3, 0.75, 1, 1.25, 4.0 / 3, 1.5, 2, 3, 4, 6};
constexpr int kBeatChoices = (int) (sizeof kBeatLengths / sizeof kBeatLengths[0]);
// Amplitude envelope Beat / Sync repeat (RepeatTime enum, quarter notes). Only index 3 is measured (125 ms at 120 BPM,
// i.e. a 1/16); the rest of the table is assumed. TODO(DEV-22a): measure the other entries.
constexpr double kRepeatBeats[] = {1.0 / 16, 1.0 / 12, 1.0 / 8, 1.0 / 4, 1.0 / 3, 1.0 / 2, 2.0 / 3, 3.0 / 4, 1, 4.0 / 3, 3.0 / 2, 2, 3, 4, 8};
constexpr int kRepeatChoices = (int) (sizeof kRepeatBeats / sizeof kRepeatBeats[0]);
// LFO Type enum -> shape, in the reference's order.
constexpr dsp::Lfo::Shape kLfoShapes[] = {dsp::Lfo::Shape::Sine, dsp::Lfo::Shape::Square, dsp::Lfo::Shape::Triangle,
                                          dsp::Lfo::Shape::SawDown, dsp::Lfo::Shape::SawUp, dsp::Lfo::Shape::Random};

std::vector<ParameterInfo> makeInfos()
{
    // Defaults follow the reference's default Simpler so a new device sounds like a new reference device.
    std::vector<ParameterInfo> p(Simpler::NumParams);
    using S = Simpler;
    const std::string f = "Filter/Slot/Value/SimplerFilter/", e = "VolumeAndPan/Envelope/", pe = "Pitch/Envelope/Slot/Value/SimplerPitchEnvelope/";
    const std::string loop = "Player/LoopModulators/", lfo = "Lfo/Slot/Value/SimplerLfo/";
    p[S::Transpose] = makeParam("Pitch/TransposeKey", "Transpose", -48.0f, 48.0f, 0.0f, "st");
    p[S::Detune] = makeParam("Pitch/TransposeFine", "Detune", -50.0f, 50.0f, 0.0f, "ct");
    p[S::Volume] = makeParam("VolumeAndPan/Volume", "Volume", -70.0f, 36.0f, -12.0f, "dB");
    p[S::Pan] = makeParam("VolumeAndPan/Panorama", "Pan", -1.0f, 1.0f, 0.0f);
    p[S::VelToVolume] = makeParam("VolumeAndPan/VolumeVelScale", "Vel>Vol", 0.0f, 1.0f, 0.35f, "%");
    p[S::Attack] = makeParam(e + "AttackTime", "Attack", 0.1f, 20000.0f, 0.1f, "ms");
    p[S::Decay] = makeParam(e + "DecayTime", "Decay", 1.0f, 60000.0f, 600.0f, "ms");
    p[S::Sustain] = makeParam(e + "SustainLevel", "Sustain", 0.0f, 1.0f, 1.0f, "lin");
    p[S::Release] = makeParam(e + "ReleaseTime", "Release", 1.0f, 60000.0f, 50.0f, "ms");
    p[S::FadeIn] = makeParam("VolumeAndPan/OneShotEnvelope/FadeInTime", "Fade In", 0.0f, 20000.0f, 0.1f, "ms");
    p[S::FadeOut] = makeParam("VolumeAndPan/OneShotEnvelope/FadeOutTime", "Fade Out", 0.0f, 20000.0f, 0.1f, "ms");
    p[S::FilterOn] = makeParam("Filter/IsOn", "Filter", 0.0f, 1.0f, 1.0f, "", true);
    p[S::FilterType] = makeParam(f + "Type", "Type", 0.0f, 4.0f, 0.0f, "", false, {"Lowpass", "Highpass", "Bandpass", "Notch", "Morph"});
    p[S::FilterFreq] = makeParam(f + "Freq", "Freq", 10.0f, 22000.0f, 22000.0f, "Hz");
    p[S::FilterRes] = makeParam(f + "Res", "Res", 0.0f, 1.25f, 0.0f);
    p[S::FilterSlope] = makeParam(f + "Slope", "Slope", 0.0f, 1.0f, 1.0f, "", true);
    p[S::FilterKeyTrack] = makeParam(f + "ModByPitch", "Key", 0.0f, 1.0f, 1.0f, "%");
    p[S::FilterEnvOn] = makeParam(f + "Envelope/IsOn", "Env", 0.0f, 1.0f, 1.0f, "", true);
    p[S::FilterEnvAmount] = makeParam(f + "Envelope/Amount", "Env Amt", -72.0f, 72.0f, 0.0f, "st");
    p[S::FilterAttack] = makeParam(f + "Envelope/AttackTime", "F Attack", 0.1f, 20000.0f, 0.1f, "ms");
    p[S::FilterDecay] = makeParam(f + "Envelope/DecayTime", "F Decay", 1.0f, 60000.0f, 600.0f, "ms");
    p[S::FilterSustain] = makeParam(f + "Envelope/SustainLevel", "F Sustain", 0.0f, 1.0f, 0.0f, "lin");
    p[S::FilterRelease] = makeParam(f + "Envelope/ReleaseTime", "F Release", 1.0f, 60000.0f, 50.0f, "ms");
    p[S::KeyZoneShift] = makeParam("Globals/KeyZoneShift", "Zone Shift", -48.0f, 48.0f, 0.0f, "st");
    std::vector<std::string> voices;
    for (int count : kVoiceCounts) voices.push_back(std::to_string(count));
    p[S::Voices] = makePlainParam(makeParam("Globals/NumVoices", "Voices", 0, (float) kVoiceChoices - 1, 5, "", false, voices));
    p[S::Retrigger] = makePlainParam(makeParam("Globals/RetriggerMode", "Retrig", 0, 1, 1, "", true));
    p[S::LfoOn] = makeParam("Lfo/IsOn", "LFO", 0, 1, 1, "", true);
    p[S::LfoRate] = makeParam(lfo + "Frequency", "Rate", 0.01f, 30, 1, "Hz");
    p[S::LfoShape] = makeParam(lfo + "Type", "Wave", 0, 5, 0, "", false, {"Sine", "Square", "Triangle", "Saw Down", "Saw Up", "Random"});
    p[S::LfoPitch] = makeParam("Pitch/PitchLfoAmount", "Pitch LFO", 0, 1, 0, "%");
    p[S::LfoVolume] = makeParam("VolumeAndPan/VolumeLfoAmount", "Vol LFO", 0, 1, 0, "%");
    p[S::LfoPan] = makeParam("VolumeAndPan/PanoramaLfoAmount", "Pan LFO", 0, 1, 0, "%");
    p[S::LfoSync] = makeParam(lfo + "RateType", "Sync", 0, 1, 0, "", true);
    p[S::LfoBeat] = makeParam(lfo + "BeatRate", "Beat", 0, (float) kBeatChoices - 1, 4, "", false,
                              {"1/64", "1/48", "1/32", "1/24", "1/16", "1/12", "1/8", "1/6", "3/16", "1/4", "5/16", "1/3", "3/8", "1/2", "3/4", "1", "1.5"});
    p[S::LoopOn] = makeParam(loop + "LoopOn", "Loop", 0, 1, 1, "", true);
    p[S::SampleStartMod] = makeParam(loop + "SampleStart", "Start", 0, 1, 0, "%");
    p[S::SampleLengthMod] = makeParam(loop + "SampleLength", "Length", 0, 1, 1, "%");
    p[S::LoopLengthMod] = makeParam(loop + "LoopLength", "Loop Length", 0, 1, 1, "%");
    p[S::LoopFadeMod] = makeParam(loop + "LoopFade", "Fade", 0, 1, 0, "%");
    p[S::AttackSlope] = makeParam(e + "AttackSlope", "A Slope", -1, 1, 0);
    p[S::DecaySlope] = makeParam(e + "DecaySlope", "D Slope", -1, 1, 1);
    p[S::ReleaseSlope] = makeParam(e + "ReleaseSlope", "R Slope", -1, 1, 1);
    p[S::FilterAttackSlope] = makeParam(f + "Envelope/AttackSlope", "F A Slope", -1, 1, 0);
    p[S::FilterDecaySlope] = makeParam(f + "Envelope/DecaySlope", "F D Slope", -1, 1, 1);
    p[S::FilterReleaseSlope] = makeParam(f + "Envelope/ReleaseSlope", "F R Slope", -1, 1, 1);
    p[S::FilterMorph] = makeParam(f + "X", "Morph", 0, 1, 0, "%");
    p[S::FilterVelMod] = makeParam(f + "ModByVelocity", "Vel", 0, 1, 0, "%");
    p[S::FilterLfoMod] = makeParam(f + "ModByLfo", "LFO", 0, 24, 0); // semitones of LFO depth (reference range 0-24, shown plain)
    p[S::FilterDrive] = makeParam(f + "Drive", "Drive", 0, 24, 0, "dB");
    p[S::GlideMode] = makeParam("Globals/PortamentoMode", "Glide", 0, 2, 0, "", false, {"Off", "Glide", "Portamento"});
    p[S::GlideTime] = makeParam("Globals/PortamentoTime", "Time", 0.1f, 10000, 50, "ms");
    p[S::VolKeyScale] = makeParam("VolumeAndPan/VolumeKeyScale", "Vol<Key", -1, 1, 0, "%");
    p[S::PanKeyScale] = makeParam("VolumeAndPan/PanoramaKeyScale", "Pan<Key", -1, 1, 0, "%");
    p[S::PanRandom] = makeParam("VolumeAndPan/PanoramaRnd", "Pan<Rnd", 0, 1, 0, "%");
    p[S::PitchEnvOn] = makeParam("Pitch/Envelope/IsOn", "Pitch Env", 0, 1, 1, "", true);
    p[S::PitchEnvAmount] = makeParam(pe + "Amount", "P Amount", -48, 48, 0, "st");
    p[S::PitchAttack] = makeParam(pe + "AttackTime", "P Attack", 0.1f, 20000.0f, 0.1f, "ms");
    p[S::PitchDecay] = makeParam(pe + "DecayTime", "P Decay", 1.0f, 60000.0f, 600.0f, "ms");
    p[S::PitchSustain] = makeParam(pe + "SustainLevel", "P Sustain", 0, 1, 0, "lin");
    p[S::PitchRelease] = makeParam(pe + "ReleaseTime", "P Release", 1.0f, 60000.0f, 50.0f, "ms");
    p[S::PitchAttackSlope] = makeParam(pe + "AttackSlope", "P A Slope", -1, 1, 0);
    p[S::PitchDecaySlope] = makeParam(pe + "DecaySlope", "P D Slope", -1, 1, 1);
    p[S::PitchReleaseSlope] = makeParam(pe + "ReleaseSlope", "P R Slope", -1, 1, 1);
    p[S::ConstPowerFade] = makePlainParam(makeParam("Player/UseConstPowCrossfade", "Const Power", 0, 1, 1, "", true));
    p[S::Interpolation] = makePlainParam(makeParam("Player/InterpolationMode", "Interpolation", 0, 3, 3, "", false, {"Mode 0", "Mode 1", "Mode 2", "Mode 3"}));
    p[S::LfoAttack] = makeParam(lfo + "Attack", "LFO Attack", 0.1f, 20000.0f, 0.1f, "ms");
    p[S::LfoRetrigger] = makeParam(lfo + "Retrigger", "LFO Retrig", 0, 1, 1, "", true);
    p[S::Spread] = makeParam("Globals/SpreadAmount", "Spread", 0, 100, 0, "%");
    p[S::OneShotGate] = makeParam("VolumeAndPan/OneShotEnvelope/SustainMode", "Gate", 0, 1, 0, "", false, {"Trigger", "Gate"});
    p[S::Snap] = makeParam("Player/Snap", "Snap", 0, 1, 1, "", true);
    p[S::FilterCircuit] = makeParam(f + "CircuitLpHp", "Circuit", 0, 4, 0, "", false, {"Clean", "OSR", "MS2", "SMP", "PRD"});
    p[S::FilterCircuitBp] = makeParam(f + "CircuitBpNoMo", "BP Circuit", 0, 1, 0, "", false, {"Clean", "OSR"});
    p[S::LfoOffset] = makeParam(lfo + "Offset", "Offset", 0, 360, 0, "deg");
    p[S::LfoKeyScale] = makeParam(lfo + "FrequencyKeyScale", "LFO Key", 0, 1, 0, "%");
    p[S::AmpLoopMode] = makeParam(e + "LoopMode", "Mode", 0, 4, 0, "", false, {"None", "Loop", "Beat", "Sync", "Trigger"});
    p[S::AmpLoopTime] = makeParam(e + "LoopTime", "Loop Time", 0.2f, 20000.0f, 100.0f, "ms");
    p[S::AmpRepeatTime] = makeParam(e + "RepeatTime", "Repeat", 0, (float) kRepeatChoices - 1, 3, "", false,
                                    {"1/64", "1/48", "1/32", "1/16", "1/12", "1/8", "1/6", "3/16", "1/4", "1/3", "3/8", "1/2", "3/4", "1", "2"});
    return p;
}

// Fraction (0..1) of a stage's distance covered at progress x (0..1) for |slope| < 1; see Simpler::stepEnv.
double slopeCurve(double x, double slope, double term)
{
    return slope >= 0 ? 1.0 - ((1.0 - slope) * (1.0 - x) + slope * term) : (1.0 + slope) * x + (-slope) * term;
}
} // namespace

Simpler::Simpler(std::vector<Zone> zones, Mode mode, int pitchBendRange, bool multisampler)
    : NativeDevice(makeInfos()), zones_(std::move(zones)), mode_(mode), multisampler_(multisampler), bendRange_(pitchBendRange)
{
}

void Simpler::prepare(double sampleRate, int)
{
    sampleRate_ = sampleRate;
    reset();
}

void Simpler::reset()
{
    for (auto& v : voices_)
        v = Voice{};
    bendSemis_ = 0.0f; sustainPedal_ = false; lastNote_ = -1;
    activeVoices_.store(0);
}

float Simpler::readSample(const float* d, int len, double pos, double step)
{
    const auto& table = kernelTable();
    const double stretch = std::clamp(step, 1.0, 8.0); // anti-aliasing when reading faster than the source rate
    const int i = (int) std::floor(pos);
    const double frac = pos - i;
    const int reach = (int) std::ceil(kKernelHalf * stretch);
    if (i + reach < 0 || i - reach >= len) return 0.0f;
    float out = 0.0f;
    for (int k = 1 - reach; k <= reach; ++k)
    {
        const int index = std::clamp(i + k, 0, len - 1); // measured: reads before the first frame repeat it
        const double u = (frac - k) / stretch; // kernel coordinate, -3 .. 3
        const double t = (u + kKernelHalf) * kKernelResolution;
        if (t < 0 || t >= (double) (table.size() - 1)) continue;
        const int j = (int) t;
        const float w = table[(size_t) j] + (float) (t - j) * (table[(size_t) j + 1] - table[(size_t) j]);
        out += w * d[index];
    }
    return stretch > 1.0 ? out / (float) stretch : out;
}

double Simpler::snappedLoopEnd(const engine::SampleData& sample, double loopStart, double loopEnd) const
{
    // TODO(DEV-22a): the reference's Snap rule is not fully known. Measured on ten samples: the end
    // moves back 15-26 ms to the last non-positive frame before a rising zero crossing (tones: whole periods).
    // Taking the latest such crossing at least 710 frames (16 ms at 44.1 kHz) before the end reproduces 7 of the 10
    // measured ends (tones at 50 / 100 / 440 Hz, 261.6 Hz at 1 and 2 s, saw); 1 kHz, 261.6 Hz at 0.5 s and noise differ.
    // Without such a crossing in the 16-26 ms window, the end whose following frames best match the start is used.
    const float* l = sample.audio.getReadPointer(0);
    const float* r = sample.audio.getReadPointer(sample.audio.getNumChannels() > 1 ? 1 : 0);
    const int len = sample.audio.getNumSamples();
    const auto mono = [&](int i) { return i >= 0 && i < len ? l[i] + r[i] : 0.0f; };
    const int start = (int) loopStart;
    const double frames = sample.sampleRate / 44100.0;
    const int latest = (int) loopEnd - (int) std::lround(710 * frames), earliest = (int) loopEnd - (int) std::lround(1150 * frames);
    for (int e = latest; e >= std::max(earliest, start + 64); --e)
        if (mono(e) <= 0.0f && mono(e + 1) > 0.0f) return (double) e;
    const int last = (int) loopEnd - (int) (0.015 * sample.sampleRate), first = (int) loopEnd - (int) (0.030 * sample.sampleRate);
    int best = (int) loopEnd;
    float bestCost = 1e30f;
    for (int e = last; e >= std::max(first, start + 64); --e)
    {
        float cost = 0.0f;
        for (int k = 0; k < 16 && cost < bestCost; ++k) cost += std::abs(mono(e + k) - mono(start + k));
        if (cost < bestCost * 0.999f) { bestCost = cost; best = e; }
    }
    return (double) best;
}

void Simpler::process(juce::AudioBuffer<float>& audio, juce::MidiBuffer& midi)
{
    if (isBypassed())
        return;
    int pos = 0;
    const int n = audio.getNumSamples();
    for (const auto meta : midi)
    {
        const int at = std::clamp(meta.samplePosition, 0, n);
        render(audio, pos, at);
        pos = at;
        if (meta.numBytes > 3) continue; // unsupported SysEx, no heap-backed MidiMessage on the audio thread
        const auto m = meta.getMessage();
        if (m.isNoteOn())
            noteOn(m.getNoteNumber(), m.getFloatVelocity());
        else if (m.isNoteOff())
            noteOff(m.getNoteNumber());
        else if (m.isAllSoundOff())
        {
            for (auto& v : voices_) v.active = false;
        }
        else if (m.isAllNotesOff())
        {
            for (auto& v : voices_)
                if (v.active)
                    v.released = true;
        }
        else if (m.isController() && m.getControllerNumber() == 64)
        {
            sustainPedal_ = m.getControllerValue() >= 64;
            if (! sustainPedal_) for (auto& v : voices_)
                if (v.active && ! v.keyDown) v.released = true;
        }
        else if (m.isResetAllControllers())
        {
            bendSemis_ = 0; sustainPedal_ = false;
            for (auto& v : voices_) if (v.active && ! v.keyDown) v.released = true;
        }
        else if (m.isPitchWheel())
            bendSemis_ = (float) (m.getPitchWheelValue() - 8192) / 8192.0f * (float) bendRange_;
    }
    render(audio, pos, n);

    int active = 0;
    for (const auto& v : voices_)
        active += v.active ? 1 : 0;
    activeVoices_.store(active, std::memory_order_relaxed);
}

void Simpler::noteOn(int note, float velocity)
{
    const int shifted = note - (int) std::lround(value(KeyZoneShift));
    const int vel127 = (int) std::lround(velocity * 127.0f);
    for (const auto& z : zones_)
    {
        if (shifted < z.part.keyMin || shifted > z.part.keyMax || vel127 < z.part.velMin || vel127 > z.part.velMax)
            continue;
        startVoice(z, note, velocity);
    }
    lastNote_ = note;
}

void Simpler::noteOff(int note)
{
    if (mode_ != Mode::Classic && ! (mode_ == Mode::OneShot && value(OneShotGate) > 0.5f))
        return; // One-Shot (Trigger) and Slice play to the end
    for (auto& v : voices_)
        if (v.active && v.note == note && v.keyDown)
        {
            v.keyDown = false;
            if (! sustainPedal_) v.released = true;
        }
}

void Simpler::startGlide(Voice& v, int fromNote, bool legato)
{
    // Glide is linear in semitones and takes GlideTime whatever the interval; only legato notes glide.
    if (value(GlideMode) < 0.5f || fromNote < 0 || ! legato || fromNote == v.note)
        return;
    v.glideSemis = (double) (fromNote - v.note);
    v.glideStep = -v.glideSemis / std::max(1.0, (double) value(GlideTime) * 0.001 * sampleRate_);
}

void Simpler::startVoice(const Zone& zone, int note, float velocity)
{
    const engine::SampleData* sample = zone.slot != nullptr ? zone.slot->get() : nullptr;
    if (sample == nullptr)
        return; // not loaded (yet)

    // One-Shot and glide (both modes, measured) play one voice.
    const int limit = mode_ == Mode::OneShot || value(GlideMode) > 0.5f ? 1 : kVoiceCounts[std::clamp((int) std::lround(value(Voices)), 0, kVoiceChoices - 1)];
    // A legato note in mono mode takes over the sounding voice (its pitch glides from the previous note).
    const int fromNote = lastNote_;
    bool legato = false;
    for (const auto& other : voices_)
        legato |= other.active && other.keyDown && ! other.released;
    for (int i = 0; i < kMaxVoices; ++i)
        if (i >= limit || (value(Retrigger) > 0.5f && voices_[(size_t) i].note == note)) voices_[(size_t) i].active = false;
    Voice* v = nullptr;
    for (auto& cand : std::span(voices_).first((size_t) limit))
        if (! cand.active)
        {
            v = &cand;
            break;
        }
    if (v == nullptr) // steal the oldest
        v = &*std::min_element(voices_.begin(), voices_.begin() + limit, [](const Voice& a, const Voice& b) { return a.age < b.age; });

    *v = Voice{};
    v->active = true;
    v->note = note;
    v->velocity = velocity;
    v->zone = &zone;
    v->sample = sample;
    v->age = ++counter_;
    const int len = sample->audio.getNumSamples();
    const double zoneStart = std::clamp<double>((double) zone.part.sampleStart, 0.0, (double) len - 1.0);
    const double zoneEnd = zone.part.sampleEnd > 0 ? std::min<double>((double) zone.part.sampleEnd, (double) len) : (double) len;
    v->startFrame = zoneStart;
    v->endFrame = zoneEnd;
    if (! multisampler_)
    {
        // Simpler's sample region and loop: Start / Length are fractions of the zone, the loop is the last
        // LoopLength of the region and the crossfade a fraction of the loop (measured, DEV-22).
        const double zoneLength = zoneEnd - zoneStart;
        v->startFrame = zoneStart + std::clamp((double) value(SampleStartMod), 0.0, 1.0) * zoneLength;
        v->endFrame = std::min(zoneEnd, v->startFrame + std::clamp((double) value(SampleLengthMod), 0.0, 1.0) * zoneLength);
        const double loopStart = v->endFrame - std::clamp((double) value(LoopLengthMod), 0.0, 1.0) * (v->endFrame - v->startFrame);
        // Snap moves the region end (loop or not) back to a point that continues the waveform at the start.
        if (value(Snap) > 0.5f && v->endFrame - v->startFrame > 0.06 * sample->sampleRate)
            v->endFrame = snappedLoopEnd(*sample, v->startFrame, v->endFrame);
        if (mode_ == Mode::Classic && value(LoopOn) > 0.5f && v->endFrame - v->startFrame > 4)
        {
            v->loopMode = 1;
            v->loopEnd = v->endFrame;
            v->loopStart = std::min(loopStart, v->loopEnd - 4);
            v->fadeLength = std::min(std::clamp((double) value(LoopFadeMod), 0.0, 1.0) * (v->loopEnd - v->loopStart),
                                     v->loopStart - v->startFrame);
        }
    }
    else if (mode_ == Mode::Classic && zone.part.loopMode != 0 && zone.part.loopEnd > zone.part.loopStart + 4)
    {
        v->loopMode = zone.part.loopMode == 2 ? 2 : 1;
        v->loopStart = (double) zone.part.loopStart;
        v->loopEnd = (double) zone.part.loopEnd;
        v->fadeLength = std::min((double) zone.part.loopCrossfade, (v->loopEnd - v->loopStart) * 0.5);
    }
    if (mode_ == Mode::Slice)
    {
        const int slice = note - 36;
        int index = 0;
        bool found = false;
        for (double seconds : sample->onsets)
        {
            const double frame = seconds * sample->sampleRate;
            if (frame < v->startFrame || frame >= v->endFrame) continue;
            if (found) { v->endFrame = frame; break; }
            if (index++ == slice) { v->startFrame = frame; found = true; }
        }
        if (! found) { v->active = false; return; }
    }
    v->pos = v->posR = v->startFrame;

    const double semis = (double) (mode_ == Mode::Slice ? 0 : note - zone.part.rootKey) + value(Transpose)
                         + (value(Detune) + zone.part.detuneCents) / 100.0;
    v->baseStep = std::pow(2.0, semis / 12.0) * sample->sampleRate / sampleRate_;
    startGlide(*v, fromNote, legato);

    // Velocity and key scale the level linearly in dB around velocity / note 70 (0.4 dB per velocity step and
    // 0.5 dB per semitone at 100 %); key -> pan is (note - 70) / 64. Pan is equal-power with +3 dB at the extremes.
    const float vel127 = velocity * 127.0f;
    const float levelDb = value(Volume) + value(VelToVolume) * 0.4f * (vel127 - 70.0f) + value(VolKeyScale) * 0.5f * (float) (note - 70);
    const float vol = (float) zone.part.volume * std::pow(10.0f, levelDb / 20.0f);
    random_ = random_ * 1664525u + 1013904223u;
    const float randomPan = value(PanRandom) * ((float) (random_ >> 8) / 8388608.0f - 1.0f);
    const float pan = std::clamp(value(Pan) + (float) zone.part.pan + value(PanKeyScale) * (float) (note - 70) / 64.0f + randomPan, -1.0f, 1.0f);
    const float angle = (pan + 1.0f) * 0.25f * juce::MathConstants<float>::pi;
    v->gainL = vol * std::sqrt(2.0f) * std::cos(angle);
    v->gainR = vol * std::sqrt(2.0f) * std::sin(angle);

    const auto& amp = envShape(Attack, Decay, Sustain, Release, AttackSlope, DecaySlope, ReleaseSlope);
    startStage(v->amp, Env::Stage::Attack, 1.0f, amp.attack, amp.attackSlope);
    v->amp.level = v->amp.from = 0.0f;
    const auto& filterShape = envShape(FilterAttack, FilterDecay, FilterSustain, FilterRelease, FilterAttackSlope, FilterDecaySlope, FilterReleaseSlope);
    startStage(v->filter, Env::Stage::Attack, 1.0f, filterShape.attack, filterShape.attackSlope);
    const auto& pitchShape = envShape(PitchAttack, PitchDecay, PitchSustain, PitchRelease, PitchAttackSlope, PitchDecaySlope, PitchReleaseSlope);
    v->amp.floor = v->filter.floor = kSlopeFloor;
    v->pitch.floor = kPitchSlopeFloor;
    startStage(v->pitch, Env::Stage::Attack, 1.0f, pitchShape.attack, pitchShape.attackSlope);
    v->fadeIn = value(FadeIn) > 0.0f ? 0.0f : 1.0f;
    // Velocity -> cutoff (measured, velocity sweep at 100 %): one semitone per velocity step, centred on 70.
    v->filterKeySemis = value(FilterKeyTrack) * (float) (note - 60) + value(FilterVelMod) * (vel127 - 70.0f);
    if (value(LfoRetrigger) < 0.5f) v->lfo = freeLfo_;
    else v->lfo.setPhase(value(LfoOffset) / 360.0); // measured: Offset is the start phase (lfo-offset-90/180 match)
    // LFO Key (measured, 2 Hz LFO at C1-C5): rate x (1 + key 2^((note - 60) / 12)), e.g. 4 Hz at C3 with key 100 %.
    v->lfoRateScale = 1.0 + value(LfoKeyScale) * std::exp2((note - 60) / 12.0);
    updateFilter(*v, 0.0f);
}

Simpler::EnvShape Simpler::envShape(int attack, int decay, int sustain, int release, int attackSlope, int decaySlope, int releaseSlope) const
{
    const float ms = (float) (sampleRate_ * 0.001);
    return {std::max(1.0f, value(attack) * ms), std::max(1.0f, value(decay) * ms), value(sustain), std::max(1.0f, value(release) * ms),
            value(attackSlope), value(decaySlope), value(releaseSlope)};
}

void Simpler::startStage(Env& e, Env::Stage stage, float target, double length, float slope)
{
    e.stage = stage;
    e.from = e.level;
    e.elapsed = 0;
    e.length = std::max(1.0, length);
    const double distance = std::abs((double) target - e.from);
    if (slope >= 0.999f)
    {
        // dB-linear approach to the floor: term = (floor / distance)^x
        e.term = 1.0;
        e.ratio = distance > e.floor ? std::pow(e.floor / distance, 1.0 / e.length) : 0.0;
    }
    else if (slope >= 0)
    {
        e.term = 1.0; // exp(-k x)
        e.ratio = std::exp(-kSlopeRate / e.length);
    }
    else
    {
        e.term = std::exp(-kSlopeRate); // exp(-k (1 - x))
        e.ratio = std::exp(kSlopeRate / e.length);
    }
}

float Simpler::stepEnv(Env& e, bool released, const EnvShape& shape)
{
    if (released && e.stage != Env::Stage::Release && e.stage != Env::Stage::Idle)
        startStage(e, Env::Stage::Release, 0.0f, shape.release, shape.releaseSlope);
    // Amplitude loop modes (measured): Beat / Sync restart the attack from the current level every repeat period while
    // the note is held; Loop falls linearly from the sustain level to 0 over Loop Time after the decay, then restarts
    // the attack; Trigger releases right after the decay. TODO(DEV-22a): Sync is not aligned to the song grid yet.
    e.age += 1.0;
    if ((shape.loopMode == 2 || shape.loopMode == 3) && ! released && e.stage != Env::Stage::Release && e.age >= e.nextRepeat)
    {
        if (e.nextRepeat > 0) startStage(e, Env::Stage::Attack, 1.0f, shape.attack, shape.attackSlope);
        e.nextRepeat += shape.repeat;
    }
    if (e.stage == Env::Stage::Idle || e.stage == Env::Stage::Sustain)
    {
        if (e.stage == Env::Stage::Sustain) e.level = shape.sustain;
        return e.level;
    }
    const float target = e.stage == Env::Stage::Attack ? 1.0f : e.stage == Env::Stage::Decay ? shape.sustain : 0.0f;
    const float slope = e.stage == Env::Stage::Attack ? shape.attackSlope : e.stage == Env::Stage::Decay ? shape.decaySlope
                      : e.stage == Env::Stage::LoopFall ? 0.0f : shape.releaseSlope;
    const double x = e.elapsed / e.length;
    if (slope >= 0.999f)
        e.level = target + (e.from - target) * (float) e.term;
    else
        e.level = e.from + (target - e.from) * (float) slopeCurve(x, slope, e.term);
    e.term *= e.ratio;
    e.elapsed += 1.0;
    if (e.elapsed >= e.length)
    {
        e.level = target;
        if (e.stage == Env::Stage::Attack) startStage(e, Env::Stage::Decay, shape.sustain, shape.decay, shape.decaySlope);
        else if (e.stage == Env::Stage::Decay && shape.loopMode == 1 && ! released) startStage(e, Env::Stage::LoopFall, 0.0f, shape.loopTime, 0.0f);
        else if (e.stage == Env::Stage::Decay && shape.loopMode == 4) startStage(e, Env::Stage::Release, 0.0f, shape.release, shape.releaseSlope);
        else if (e.stage == Env::Stage::Decay) e.stage = Env::Stage::Sustain;
        else if (e.stage == Env::Stage::LoopFall) startStage(e, Env::Stage::Attack, 1.0f, shape.attack, shape.attackSlope);
        else e.stage = Env::Stage::Idle;
    }
    else if (e.stage == Env::Stage::Release && e.level < kReleaseCut && e.from >= kReleaseCut)
        e.stage = Env::Stage::Idle;
    return e.level;
}

void Simpler::updateFilter(Voice& v, float lfo)
{
    if (value(FilterOn) < 0.5f)
        return;
    double semis = v.filterKeySemis + value(FilterLfoMod) * lfo; // measured: ModByLfo = 1 swings the cutoff +-1 st
    if (value(FilterEnvOn) > 0.5f)
        semis += (double) value(FilterEnvAmount) * v.filter.level;
    BiquadCoeffs stages[2];
    v.filterStageCount = filterStages((double) value(FilterFreq) * std::exp2(semis / 12.0), stages);
    for (int i = 0; i < v.filterStageCount; ++i)
        v.coeffs[i] = SvfCoeffs::fromBiquad(stages[i]);
}

int Simpler::filterCircuit() const
{
    const int type = std::clamp((int) std::lround(value(FilterType)), 0, 4);
    return type <= 1 ? std::clamp((int) std::lround(value(FilterCircuit)), 0, 4) : (value(FilterCircuitBp) > 0.5f ? 1 : 0);
}

float Simpler::saturate(float x, int circuit)
{
    // Measured with a -6 dBFS 220 Hz sine through Drive 0 / 12 / 24 dB (harmonics H1-H5 fitted): OSR is linear up to 1.54
    // and clips softly towards 1.75 with a +0.2 bias (even harmonics, H2 -27 dB at Drive 12); MS2 / SMP use the same
    // clipper (their saturation is not memoryless: Drive 24 on noise is ~3 dB louder in the reference, TODO(DEV-22a));
    // PRD saturates like tanh around 1.35 (H3 -40 dB even at Drive 0).
    if (circuit == 4) return 1.35f * std::tanh(x / 1.35f);
    constexpr float knee = 1.537f, room = 1.748f - knee, bias = 0.205f;
    const auto clip = [](float u) {
        const float a = std::abs(u);
        return a <= knee ? u : std::copysign(knee + room * std::tanh((a - knee) / room), u);
    };
    return clip(x + bias) - bias; // clip(bias) == bias: linear below the knee
}

int Simpler::filterStages(double freq, BiquadCoeffs* out) const
{
    // Clean circuit (measured): RBJ biquads, Q = 1 / (2 d) with resonance damping d = (1 - r) / (1 + 3 r) (Q <= 18.2);
    // 24 dB adds a second stage with damping (1 + d) / 2. Band-pass stages have a peak gain of Q.
    // OSR / MS2 / SMP (measured with noise at 1 kHz, r = 0 .. 1, 12 / 24 dB, LP / HP / BP): the same structure with their
    // own damping (fitted per resonance step within 0.4 dB); the drive saturation is applied before the filter (render).
    const double r = std::clamp((double) value(FilterRes), 0.0, 1.0);
    const double clean = (1.0 - r) / (1.0 + 3.0 * r);
    const int circuit = filterCircuit();
    // Modulated cutoff stays inside the Frequency range (measured: low velocities stop at ~30 Hz).
    freq = std::clamp(freq, 30.0, std::min(22000.0, sampleRate_ * 0.49));
    // OSR's extra damping grows with the cutoff (Q at r = 0.5: 2.44 / 2.12 / 1.68 at 200 Hz / 1 kHz / 5 kHz): scale
    // measured at 1 kHz, interpolated in octaves through 0.14 at 200 Hz and 2.72 at 5 kHz.
    const double octaves = std::log2(freq / 1000.0), span = std::log2(5.0);
    const double osrScale = octaves < 0 ? 1.0 + (1.0 - 0.14) * std::max(octaves / span, -1.0) : 1.0 + (2.72 - 1.0) * std::min(octaves / span, 1.0);
    const double d = circuit == 1 ? clean + osrScale * r * (0.057 + 0.027 * r) // OSR: much weaker resonance (Q 5.95 at r = 1)
                   : circuit == 2 ? std::max(clean - 0.006, 0.0205)       // MS2: slightly sharper than Clean
                   : circuit == 3 ? std::max(clean + 0.01 * r, 0.029)     // SMP: slightly softer
                                  : std::max(clean, 0.0274);
    const double d2 = circuit == 1 ? std::max((1.0 + d) * 0.5, 0.595) : (1.0 + d) * 0.5;
    const double q1 = 0.5 / d, q2 = 0.5 / d2;
    const int type = std::clamp((int) std::lround(value(FilterType)), 0, 4);
    const bool steep = value(FilterSlope) >= 0.5f;
    if (circuit == 4 && type <= 1)
    {
        // PRD (measured): a gain-compensated four-pole ladder, H = (1 + k) / ((1 + s)^4 + k) with s normalised to the
        // cutoff (single bilinear prewarp at the cutoff). k(r) fitted at r = 0 .. 1 (0.98 / 1.93 / 2.84 / 3.63 at .25
        // steps); 12 dB is the ladder's second tap, (1 + k)(1 + s)^2 / ((1 + s)^4 + k), with k scaled by 0.735; high-pass
        // mirrors s -> 1/s. Realised as its two pole pairs: s^2 + 2(1 -+ q)s + (1 -+ q)^2 + q^2, q = k^(1/4) / sqrt 2.
        static constexpr double kLadder[5] = {0.0, 0.975, 1.929, 2.843, 3.63};
        const double pos = r * 4.0;
        const int i = std::min((int) pos, 3);
        const double k = (kLadder[i] + (kLadder[i + 1] - kLadder[i]) * (pos - i)) * (steep ? 1.0 : 0.735);
        const double q = std::pow(k, 0.25) / std::sqrt(2.0);
        const double wa = (1 - q) * (1 - q) + q * q, wb = (1 + q) * (1 + q) + q * q;
        const double K = 1.0 / std::tan(juce::MathConstants<double>::pi * freq / sampleRate_);
        // Bilinear transform of (b2 s^2 + b1 s + b0) / (a2 s^2 + a1 s + a0).
        const auto section = [K](double b2, double b1, double b0, double a2, double a1, double a0) {
            const double kk = K * K, n = a2 * kk + a1 * K + a0;
            return BiquadCoeffs{(b2 * kk + b1 * K + b0) / n, (2 * b0 - 2 * b2 * kk) / n, (b2 * kk - b1 * K + b0) / n,
                                (2 * a0 - 2 * a2 * kk) / n, (a2 * kk - a1 * K + a0) / n};
        };
        if (type == 0)
        {
            out[0] = steep ? section(0, 0, wa, 1, 2 * (1 - q), wa) : section(wa, 2 * wa, wa, 1, 2 * (1 - q), wa);
            out[1] = section(0, 0, wb, 1, 2 * (1 + q), wb);
        }
        else
        {
            out[0] = steep ? section(wa, 0, 0, wa, 2 * (1 - q), 1) : section(wa, 2 * wa, wa, wa, 2 * (1 - q), 1);
            out[1] = section(wb, 0, 0, wb, 2 * (1 + q), 1);
        }
        return 2;
    }
    const auto stageCoeffs = [this, freq](int kind, double q) {
        constexpr BiquadCoeffs::Type kinds[4] = {BiquadCoeffs::Type::LowPass, BiquadCoeffs::Type::HighPass, BiquadCoeffs::Type::BandPass, BiquadCoeffs::Type::Notch};
        auto c = BiquadCoeffs::make(kinds[kind], sampleRate_, freq, q, 0.0);
        if (kind == 2) { c.b0 *= q; c.b2 *= q; } // constant-skirt band-pass: peak gain Q
        return c;
    };
    // Morph (measured at nine positions): each stage mixes its low-, band- and high-pass outputs (same poles) as
    // LP (0) > BP (.25) > HP (.5) > Notch = LP + HP (.75) > LP (1). The LP and HP weights move linearly; the BP weight
    // takes the remaining power, sqrt(1 - lp^2 - hp^2): 0.125 is 0.5 LP + 0.866 BP per stage.
    const double morph = std::clamp((double) value(FilterMorph), 0.0, 1.0);
    const double lpWeight = std::clamp(morph < 0.5 ? 1.0 - 4.0 * morph : 4.0 * morph - 2.0, 0.0, 1.0);
    const double hpWeight = std::clamp(morph < 0.75 ? 4.0 * morph - 1.0 : 4.0 - 4.0 * morph, 0.0, 1.0);
    const double bpWeight = std::sqrt(std::max(0.0, 1.0 - lpWeight * lpWeight - hpWeight * hpWeight));
    for (int stage = 0; stage < 2; ++stage)
    {
        const double q = stage == 0 ? q1 : q2;
        auto c = stageCoeffs(type == 4 ? 0 : type, q);
        if (type == 4)
        {
            const auto hp = stageCoeffs(1, q), bp = stageCoeffs(2, q);
            c.b0 = lpWeight * c.b0 + bpWeight * bp.b0 + hpWeight * hp.b0;
            c.b1 = lpWeight * c.b1 + bpWeight * bp.b1 + hpWeight * hp.b1;
            c.b2 = lpWeight * c.b2 + bpWeight * bp.b2 + hpWeight * hp.b2;
        }
        out[stage] = c;
        if (! steep) return 1;
    }
    return 2;
}

void Simpler::render(juce::AudioBuffer<float>& audio, int from, int to)
{
    if (to <= from)
        return;
    float* outL = audio.getWritePointer(0);
    float* outR = audio.getWritePointer(1);
    const float msToSamples = (float) (sampleRate_ * 0.001);
    const bool oneShot = mode_ != Mode::Classic;
    // One-Shot replaces the ADSR with the fades: Gate mode fades out over FadeOut on release.
    EnvShape amp = oneShot ? EnvShape{1, 1, 1, std::max(1.0f, value(FadeOut) * msToSamples), 0, 0, 0}
                           : envShape(Attack, Decay, Sustain, Release, AttackSlope, DecaySlope, ReleaseSlope);
    if (! oneShot)
    {
        amp.loopMode = std::clamp((int) std::lround(value(AmpLoopMode)), 0, 4);
        amp.loopTime = std::max(1.0f, value(AmpLoopTime) * msToSamples);
        amp.repeat = std::max(1.0f, (float) (kRepeatBeats[std::clamp((int) std::lround(value(AmpRepeatTime)), 0, kRepeatChoices - 1)] * 60.0 / tempo_ * sampleRate_));
    }
    const EnvShape filterShape = envShape(FilterAttack, FilterDecay, FilterSustain, FilterRelease, FilterAttackSlope, FilterDecaySlope, FilterReleaseSlope);
    const EnvShape pitchShape = envShape(PitchAttack, PitchDecay, PitchSustain, PitchRelease, PitchAttackSlope, PitchDecaySlope, PitchReleaseSlope);
    const float fadeInStep = value(FadeIn) > 0.0f ? 1.0f / std::max(1.0f, value(FadeIn) * msToSamples) : 1.0f;
    // Measured (One-Shot, Fade Out 0.1 ms): the sample end still fades linearly over ~1.03 ms (45 frames at 44.1 kHz).
    const float fadeOutSamples = std::max(1.03f * msToSamples, value(FadeOut) * msToSamples);
    const bool filterOn = value(FilterOn) >= 0.5f;
    const bool filterEnv = value(FilterEnvOn) >= 0.5f;
    // Non-Clean circuits: Drive (dB) is a gain into the circuit's saturation, before the filter (Clean ignores it).
    const int circuit = filterOn ? filterCircuit() : 0;
    const float drive = circuit != 0 ? dsp::dbToGain(value(FilterDrive)) : 1.0f;
    const bool lfoOn = value(LfoOn) > 0.5f;
    const bool pitchEnv = value(PitchEnvOn) > 0.5f && value(PitchEnvAmount) != 0.0f;
    const double lfoRate = value(LfoSync) > 0.5f ? tempo_ / 60 / kBeatLengths[std::clamp((int) std::lround(value(LfoBeat)), 0, kBeatChoices - 1)] : value(LfoRate);
    const double lfoIncrement = lfoRate / sampleRate_;
    const float lfoSmoothing = 1.0f - (float) std::exp(-1.0 / (0.0006 * sampleRate_));
    const auto lfoShape = kLfoShapes[std::clamp((int) std::lround(value(LfoShape)), 0, 5)];
    const double lfoAttack = std::max(1.0, (double) value(LfoAttack) * msToSamples);
    // Pitch LFO depth (measured): +-12 log2(1 + amount / 2) semitones, e.g. a fifth at 100 %.
    const float pitchAmount = 12.0f * std::log2(1.0f + 0.5f * value(LfoPitch)), volumeAmount = value(LfoVolume), panAmount = value(LfoPan);
    const float pitchEnvAmount = value(PitchEnvAmount);
    const double bendRatio = std::pow(2.0, (double) bendSemis_ / 12.0);
    const bool constPower = value(ConstPowerFade) > 0.5f;
    // Spread (measured): the left channel plays detuned down and the right one up by Spread / 10 cents.
    const double spreadRatio = std::exp2((double) value(Spread) / 10.0 / 1200.0);
    const bool spread = value(Spread) > 0.0f;
    for (int i = from; i < to; ++i) freeLfo_.next(lfoIncrement, lfoShape);

    for (auto& v : voices_)
    {
        if (! v.active)
            continue;
        const float* dl = v.sample->audio.getReadPointer(0);
        const float* dr = v.sample->audio.getReadPointer(1);
        const int len = v.sample->audio.getNumSamples();
        const bool loop = ! oneShot && v.loopMode != 0;
        const double loopLength = v.loopEnd - v.loopStart;
        // Advances one channel's read position, wrapping it in the loop; false once it runs past the end.
        const auto advance = [&](double& pos, int& direction, double step) {
            pos += direction * step;
            if (loop)
            {
                if (direction > 0 && pos >= v.loopEnd)
                {
                    if (v.loopMode == 2) { direction = -1; pos = v.loopEnd - 1.0; }
                    else if (multisampler_) pos = v.loopStart + v.fadeLength + std::fmod(pos - v.loopEnd, loopLength - v.fadeLength);
                    else pos = v.loopStart + std::fmod(pos - v.loopEnd, loopLength);
                }
                else if (direction < 0 && pos <= v.loopStart) { direction = 1; pos = v.loopStart; }
            }
            return pos < v.endFrame && pos >= 0.0;
        };
        const auto read = [&](const float* d, double pos, double step) {
            float x = readSample(d, len, pos, step);
            if (loop && v.loopMode == 1 && v.fadeLength > 0 && pos >= v.loopEnd - v.fadeLength)
            {
                // Simpler: pre-loop crossfade, fading in the audio just before the loop start. Sampler zones may
                // loop from their first frame, so they fade into the loop start instead (and wrap past the fade).
                const double t = std::clamp((pos - (v.loopEnd - v.fadeLength)) / v.fadeLength, 0.0, 1.0);
                const float in = constPower ? (float) std::sin(t * juce::MathConstants<double>::halfPi) : (float) t;
                const float out = constPower ? (float) std::cos(t * juce::MathConstants<double>::halfPi) : 1.0f - (float) t;
                const double other = multisampler_ ? v.loopStart + (pos - (v.loopEnd - v.fadeLength)) : pos - loopLength;
                x = x * out + readSample(d, len, other, step) * in;
            }
            return x;
        };

        for (int i = from; i < to; ++i)
        {
            const float env = stepEnv(v.amp, v.released, amp);
            if (v.amp.stage == Env::Stage::Idle)
            {
                v.active = false;
                break;
            }

            float modulation = 0;
            if (lfoOn)
            {
                // Random steps twice per cycle (measured: 4 steps per second at 2 Hz); the output is smoothed by two
                // one-pole stages of 0.6 ms (a square edge takes ~2 ms 10-90 %, S-shaped, in the reference).
                // Random (measured, volume LFO 100 %, 15 steps): the held values spread over -0.5 .. 0 only (gain 0.23-0.49).
                // TODO(DEV-22a): confirm with pitch / pan targets; the reference's random sequence itself is unknown.
                modulation = v.lfo.next(lfoIncrement * v.lfoRateScale * (lfoShape == dsp::Lfo::Shape::Random ? 2.0 : 1.0), lfoShape);
                if (lfoShape == dsp::Lfo::Shape::Random) modulation = 0.25f * (modulation - 1.0f);
                if (v.lfoAge < 0.5) for (float& stage : v.lfoSmooth) stage = modulation; // starts settled
                for (float& stage : v.lfoSmooth) modulation = stage += (modulation - stage) * lfoSmoothing;
            }
            if (lfoOn && v.lfoAge < lfoAttack) modulation *= (float) (v.lfoAge / lfoAttack);
            v.lfoAge += 1.0;
            double semis = modulation * pitchAmount + v.glideSemis;
            if (pitchEnv) semis += pitchEnvAmount * stepEnv(v.pitch, v.released, pitchShape);
            const double step = v.baseStep * bendRatio * (semis != 0.0 ? std::exp2(semis / 12.0) : 1.0);

            float l = read(dl, v.pos, spread ? step / spreadRatio : step);
            float r = read(dr, spread ? v.posR : v.pos, spread ? step * spreadRatio : step);

            if (filterOn)
            {
                if (filterEnv) stepEnv(v.filter, v.released, filterShape);
                // Every 16 frames, so Freq / Res moves (automation, knobs) also reach sounding notes.
                if (--v.filterCountdown <= 0)
                {
                    updateFilter(v, modulation);
                    v.filterCountdown = 16;
                }
                if (circuit != 0)
                {
                    l = saturate(l * drive, circuit);
                    r = saturate(r * drive, circuit);
                }
                l = v.f[0].process(v.coeffs[0], l);
                r = v.f[1].process(v.coeffs[0], r);
                if (v.filterStageCount > 1)
                {
                    l = v.f[2].process(v.coeffs[1], l);
                    r = v.f[3].process(v.coeffs[1], r);
                }
            }

            float g = env * (lfoOn ? 1 - volumeAmount * (1 - modulation) * 0.5f : 1);
            if (v.fadeIn < 1.0f)
            {
                v.fadeIn = std::min(1.0f, v.fadeIn + fadeInStep);
                g *= v.fadeIn;
            }
            if (oneShot) // fade out over the last FadeOut ms of the sample
            {
                const float remaining = (float) ((v.endFrame - v.pos) / std::max(1e-6, step));
                if (remaining < fadeOutSamples)
                    g *= std::max(0.0f, remaining / fadeOutSamples);
            }
            float gl = v.gainL, gr = v.gainR;
            if (lfoOn && panAmount > 0.0f)
            {
                // Equal-power pan offset; the voice's own pan is already in gainL / gainR.
                const float angle = (std::clamp(modulation * panAmount, -1.0f, 1.0f) + 1.0f) * 0.25f * juce::MathConstants<float>::pi;
                gl *= std::sqrt(2.0f) * std::cos(angle);
                gr *= std::sqrt(2.0f) * std::sin(angle);
            }
            outL[i] += l * g * gl;
            outR[i] += r * g * gr;

            if (v.glideStep != 0.0)
            {
                v.glideSemis += v.glideStep;
                if ((v.glideStep > 0) == (v.glideSemis > 0)) { v.glideSemis = 0; v.glideStep = 0; }
            }
            const bool leftPlaying = advance(v.pos, v.direction, spread ? step / spreadRatio : step);
            const bool rightPlaying = ! spread || advance(v.posR, v.directionR, step * spreadRatio);
            if (! leftPlaying || ! rightPlaying)
            {
                v.active = false;
                break;
            }
        }
    }
}

} // namespace sonora::devices
