#include "devices/native/AutoFilter.h"

namespace sonora::devices
{

double AutoFilter::beatsPerCycle(int index)
{
    // 16 bars, 12, 8, 6, 4, 3, 2 bars, 1.5, 1, 3/4, 1/2 ... down to 1/32 (4 beats per bar).
    static constexpr double v[] = {64, 48, 32, 24, 16, 12, 8, 6, 4, 3, 2, 1.5, 1, 0.75, 0.5, 0.25, 0.125};
    return v[std::clamp(index, 0, 16)];
}

AutoFilter::AutoFilter() : NativeDevice([] {
    using A = AutoFilter;
    std::vector<ParameterInfo> p(A::NumParams);
    p[A::FilterType] = makeParam("FilterType", "Type", 0.0f, 4.0f, 0.0f, "", false, {"Lowpass", "Highpass", "Bandpass", "Notch", "Morph"});
    p[A::Slope] = makeParam("Slope", "24 dB", 0.0f, 1.0f, 0.0f, "", true);
    p[A::Cutoff] = makeParam("Cutoff", "Cutoff", 0.0f, 135.0f, 135.0f);
    p[A::Resonance] = makeParam("Resonance", "Res", 0.0f, 1.25f, 0.0f);
    p[A::Morph] = makeParam("Morph", "Morph", 0.0f, 1.0f, 0.0f);
    p[A::LfoAmount] = makeParam("LfoAmount", "LFO Amt", 0.0f, 1.0f, 0.0f, "%");
    p[A::LfoShape] = makeParam("Lfo/Type", "LFO Shape", 0.0f, 5.0f, 0.0f, "", false, {"Sine", "Triangle", "Saw Down", "Saw Up", "Square", "Random"});
    p[A::LfoFrequency] = makeParam("Lfo/Frequency", "LFO Rate", 0.01f, 40.0f, 1.0f, "Hz");
    p[A::LfoRateType] = makeParam("Lfo/RateType", "Sync", 0.0f, 1.0f, 0.0f, "", true);
    p[A::LfoBeatRate] = makeParam("Lfo/BeatRate", "LFO Beat", 0.0f, 16.0f, 8.0f);
    p[A::LfoPhase] = makeParam("Lfo/Phase", "Phase", 0.0f, 360.0f, 0.0f);
    p[A::LfoOn] = makeParam("Lfo/IsOn", "LFO", 0.0f, 1.0f, 1.0f, "", true);
    return p;
}())
{
}

void AutoFilter::prepare(double sampleRate, int)
{
    sampleRate_ = sampleRate;
    reset();
}

void AutoFilter::reset()
{
    for (auto& ch : filter_)
        for (auto& f : ch)
            f.reset();
    updateCounter_ = 0;
}

void AutoFilter::process(juce::AudioBuffer<float>& audio, juce::MidiBuffer&)
{
    if (isBypassed())
        return;
    const int n = audio.getNumSamples();
    float* d[2] = {audio.getWritePointer(0), audio.getWritePointer(1)};

    const int type = (int) std::lround(value(FilterType));
    const bool steep = value(Slope) > 0.5f;
    const float res = value(Resonance);
    const float q = 0.5f + res * res * 8.0f;
    const float morph = value(Morph);
    const float lfoAmount = value(LfoOn) > 0.5f ? value(LfoAmount) : 0.0f;
    const auto shape = static_cast<dsp::Lfo::Shape>(std::clamp((int) std::lround(value(LfoShape)), 0, 5));
    const bool sync = value(LfoRateType) > 0.5f;
    const double hz = sync ? (double) tempo_.load(std::memory_order_relaxed) / 60.0 / beatsPerCycle((int) std::lround(value(LfoBeatRate)))
                           : (double) value(LfoFrequency);
    const double inc = hz / sampleRate_;
    const float phaseOffset = value(LfoPhase) / 360.0f;
    const float baseHz = pitchToHz(value(Cutoff));

    // Tempo-synced LFO follows the song position so it stays locked after a locate.
    if (sync && playing_.load(std::memory_order_relaxed))
    {
        const double cycles = beats_.load(std::memory_order_relaxed) / beatsPerCycle((int) std::lround(value(LfoBeatRate)));
        for (auto& l : lfo_)
            l.setPhase(cycles + phaseOffset);
    }

    for (int i = 0; i < n; ++i)
    {
        // Recompute the coefficients every 16 samples: cheap and smooth enough for sweeps.
        if (updateCounter_-- <= 0)
        {
            updateCounter_ = 15;
            float mod = 0.0f;
            if (lfoAmount > 0.0f)
                mod = lfo_[0].next(inc * 16.0, shape) * lfoAmount * 36.0f; // +-3 octaves at 100 %
            else
                lfo_[0].next(inc * 16.0, shape);
            lfo_[1].next(inc * 16.0, shape);
            const float fc = baseHz * std::pow(2.0f, mod / 12.0f);
            for (auto& ch : filter_)
                for (auto& f : ch)
                    f.setup(sampleRate_, fc, q);
        }
        for (int c = 0; c < 2; ++c)
        {
            float x = d[c][i];
            auto run = [&](dsp::Svf::Mode m) {
                float y = filter_[c][0].process(x, m);
                if (steep)
                    y = filter_[c][1].process(y, m);
                return y;
            };
            switch (type)
            {
                case 1: x = run(dsp::Svf::Mode::HighPass); break;
                case 2: x = run(dsp::Svf::Mode::BandPass); break;
                case 3: x = run(dsp::Svf::Mode::Notch); break;
                case 4: // morph: low-pass -> band-pass -> high-pass
                {
                    const float lp = filter_[c][0].process(x, dsp::Svf::Mode::LowPass);
                    const float bp = filter_[c][1].process(x, dsp::Svf::Mode::BandPass);
                    const float hpIn = x - lp;
                    x = morph < 0.5f ? lp + (bp - lp) * (morph * 2.0f) : bp + (hpIn - bp) * ((morph - 0.5f) * 2.0f);
                    break;
                }
                default: x = run(dsp::Svf::Mode::LowPass); break;
            }
            d[c][i] = x;
        }
    }
}

// ----------------------------------------------------------------------------- Auto Filter 2

AutoFilter2::AutoFilter2() : NativeDevice([] {
    using A = AutoFilter2;
    std::vector<ParameterInfo> p(A::NumParams);
    p[A::Frequency] = makeParam("Filter_Frequency", "Frequency", 20.0f, 20000.0f, 10000.0f, "Hz");
    p[A::Resonance] = makeParam("Filter_Resonance", "Res", 0.0f, 1.0f, 0.0f, "%");
    p[A::Morph] = makeParam("Filter_Morph", "Morph", 0.0f, 1.0f, 0.0f, "%");
    p[A::FilterType] = makeParam("Filter_Type", "Type", 0.0f, 7.0f, 0.0f, "", false,
                                 {"Lowpass", "Highpass", "Bandpass", "Notch", "Morph", "DJ", "Comb", "Vowel"});
    p[A::Slope] = makeParam("Filter_Slope", "Slope", 0.0f, 1.0f, 1.0f, "", false, {"12 dB", "24 dB"});
    p[A::Circuit] = makeParam("Filter_Circuit", "Circuit", 0.0f, 4.0f, 0.0f, "", false, {"Clean", "OSR", "MS2", "SMP", "PRD"});
    p[A::Drive] = makeParam("Filter_Drive", "Drive", 0.0f, 1.0f, 0.0f, "%");
    p[A::DjControl] = makeParam("Filter_DjControl", "DJ", -1.0f, 1.0f, 0.0f);
    p[A::LfoAmount] = makeParam("Lfo_Amount", "LFO Amt", 0.0f, 1.0f, 0.0f, "%");
    p[A::LfoWaveform] = makeParam("Lfo_Waveform", "LFO Wave", 0.0f, 7.0f, 0.0f);
    p[A::LfoTimeMode] = makeParam("Lfo_TimeMode", "LFO Mode", 0.0f, 5.0f, 0.0f);
    p[A::LfoFrequency] = makeParam("Lfo_Frequency", "LFO Rate", 0.01f, 40.0f, 1.0f, "Hz");
    p[A::LfoTime] = makeParam("Lfo_Time", "LFO Time", 0.025f, 100.0f, 1.0f, "s");
    p[A::LfoSyncedRate] = makeParam("Lfo_SyncedRate", "LFO Sync", 0.25f, 64.0f, 4.0f);
    p[A::EnvAmount] = makeParam("Envelope_Amount", "Env Amt", -1.0f, 1.0f, 0.0f, "%");
    p[A::EnvAttack] = makeParam("Envelope_Attack", "Attack", 0.0001f, 1.0f, 0.001f, "s");
    p[A::EnvRelease] = makeParam("Envelope_Release", "Release", 0.001f, 10.0f, 0.25f, "s");
    p[A::Output] = makeParam("Output", "Output", 0.0f, 4.0f, 1.0f, "lin");
    p[A::SoftClip] = makeParam("SoftClipOn", "Soft Clip", 0.0f, 1.0f, 0.0f, "", true);
    p[A::DryWet] = makeParam("DryWet", "Dry/Wet", 0.0f, 1.0f, 1.0f, "%");
    return p;
}())
{
}

void AutoFilter2::prepare(double sampleRate, int)
{
    sampleRate_ = sampleRate;
    for (auto& c : comb_)
        c.prepare((int) (0.06 * sampleRate) + 8);
    reset();
}

void AutoFilter2::reset()
{
    for (auto& ch : state_)
        for (auto& kind : ch)
            for (auto& st : kind)
                st.reset();
    for (auto& c : comb_)
        c.reset();
    combOut_[0] = combOut_[1] = 0.0f;
    lfo_.reset();
    env_ = 0.0f;
    updateCounter_ = 0;
}

void AutoFilter2::setupFilters(float hz, int)
{
    const float d = damping(value(Resonance));
    const double q1 = 1.0 / (2.0 * d), q2 = 1.0 / (2.0 * (1.0 + d) * 0.5);
    const double f = std::clamp((double) hz, 20.0, 0.49 * sampleRate_);
    using T = BiquadCoeffs::Type;
    for (int k = 0; k < 2; ++k)
    {
        const double q = k == 0 ? q1 : q2;
        lp_[(size_t) k] = SvfCoeffs::fromBiquad(BiquadCoeffs::make(T::LowPass, sampleRate_, f, q, 0.0));
        hp_[(size_t) k] = SvfCoeffs::fromBiquad(BiquadCoeffs::make(T::HighPass, sampleRate_, f, q, 0.0));
        auto bp = BiquadCoeffs::make(T::BandPass, sampleRate_, f, q, 0.0); // 0 dB peak: scale to the measured peak gain Q
        bp.b0 *= q; bp.b1 *= q; bp.b2 *= q;
        bp_[(size_t) k] = SvfCoeffs::fromBiquad(bp);
        notch_[(size_t) k] = SvfCoeffs::fromBiquad(BiquadCoeffs::make(T::Notch, sampleRate_, f, q, 0.0));
    }
}

void AutoFilter2::process(juce::AudioBuffer<float>& audio, juce::MidiBuffer&)
{
    if (isBypassed())
        return;
    const int n = audio.getNumSamples();
    float* d[2] = {audio.getWritePointer(0), audio.getWritePointer(1)};
    const int type = std::clamp((int) std::lround(value(FilterType)), 0, 7);
    const int stages = value(Slope) > 0.5f ? 2 : 1;
    const float morph = value(Morph), res = value(Resonance), dj = value(DjControl);
    const float lfoAmount = value(LfoAmount), envAmount = value(EnvAmount);
    const int wave = std::clamp((int) std::lround(value(LfoWaveform)), 0, (int) dsp::ModLfo::NumWaves - 1);
    const int timeMode = (int) std::lround(value(LfoTimeMode));
    const double tempo = (double) tempo_.load(std::memory_order_relaxed);
    double lfoHz = (double) value(LfoFrequency);
    if (timeMode == 1)
        lfoHz = 1.0 / std::max(0.025, (double) value(LfoTime));
    else if (timeMode >= 2) // synced (assumed: SyncedRate 4 == 8 beats per cycle, measured period ~4 s at 120 BPM)
    {
        const double periodBeats = 32.0 / std::max(0.25, (double) value(LfoSyncedRate));
        lfoHz = tempo / 60.0 / periodBeats;
        if (playing_.load(std::memory_order_relaxed))
            lfo_.setPhase(beats_.load(std::memory_order_relaxed) / periodBeats);
    }
    const double inc = lfoHz / sampleRate_;
    const float envAttack = dsp::timeToCoef(value(EnvAttack) * 1000.0f, sampleRate_);
    const float envRelease = dsp::timeToCoef(value(EnvRelease) * 1000.0f, sampleRate_);
    const float output = value(Output), wet = value(DryWet);
    const bool clip = value(SoftClip) > 0.5f;
    const float baseHz = value(Frequency);
    const float combFeedback = std::min(0.995f, 0.5f * (1.0f + res));
    // Drive (non-Clean circuits): measured as ~26 dB x Drive of input gain (MS2, Drive 0.5: +12.9 dB RMS); the
    // circuits' own nonlinearity (intermodulation) is not modelled yet.
    const float drive = value(Circuit) > 0.5f ? dsp::dbToGain(26.0f * value(Drive)) : 1.0f;

    for (int i = 0; i < n; ++i)
    {
        lfoValue_ = lfo_.next(inc, wave, 0.0f);
        const float level = 0.5f * (std::fabs(d[0][i]) + std::fabs(d[1][i]));
        env_ += (level - env_) * (level > env_ ? envAttack : envRelease);
        float hz = baseHz * std::exp2(kOctavesPerUnit * (lfoAmount * lfoValue_ + envAmount * env_));
        if (type == 5) // DJ: negative = low-pass closing from 20 kHz, positive = high-pass opening from 20 Hz
            hz = dj >= 0.0f ? 20.0f * std::exp2(9.2f * dj) : 20000.0f * std::exp2(9.2f * dj);
        if (updateCounter_-- <= 0)
        {
            updateCounter_ = 15;
            setupFilters(hz, type);
        }
        for (int c = 0; c < 2; ++c)
        {
            const float x = d[c][i];
            auto run = [&](int kind, const std::array<SvfCoeffs, 2>& co) {
                float y = x * drive;
                for (int k = 0; k < stages; ++k)
                    y = state_[(size_t) c][(size_t) kind][(size_t) k].process(co[(size_t) k], y);
                return y;
            };
            float y;
            switch (type)
            {
                case 1: y = run(1, hp_); break;
                case 2: case 7: y = run(2, bp_); break;
                case 3: y = run(3, notch_); break;
                case 4: // Morph: LP (0) -> BP (0.5) -> HP (1)
                {
                    const float lp = run(0, lp_), bp = run(2, bp_), hp = run(1, hp_);
                    y = morph < 0.5f ? lp + (bp - lp) * morph * 2.0f : bp + (hp - bp) * (morph - 0.5f) * 2.0f;
                    break;
                }
                case 5: y = dj >= 0.0f ? run(1, hp_) : run(0, lp_); break;
                case 6: // Comb: normalised feedback comb, delay 1 / (2 f)
                {
                    const float delay = (float) std::clamp(sampleRate_ / (2.0 * std::max(20.0f, hz)), 2.0, 0.05 * sampleRate_);
                    const float fb = comb_[c].readCubic(delay - 1.0f);
                    const float v = x * drive + combFeedback * fb;
                    comb_[c].write(v);
                    y = v * (1.0f - combFeedback);
                    break;
                }
                default: y = run(0, lp_); break;
            }
            y *= output;
            if (clip)
                y = std::tanh(y);
            d[c][i] = x + (y - x) * wet;
        }
    }
}

} // namespace sonora::devices
