#include "devices/native/Delay.h"

namespace sonora::devices
{

namespace
{
constexpr float kMaxTimeSec = 5.0f;     // longest time Live's Delay offers in Time mode
constexpr float kBufferSec = 6.0f;      // + room for offset (+33 %) and modulation
constexpr float kMaxOffset = 0.33f;
constexpr float kFadeSeconds = 0.06f;   // Fade mode cross-fade length (Sonora's choice)
constexpr float kMaxGlideStep = 0.5f;   // Repitch: delay may change by at most 0.5 sample per sample (+-50 % pitch)
constexpr float kModDepthSeconds = 0.02f;

std::vector<std::string> sixteenthNames()
{
    std::vector<std::string> n;
    for (int i = 1; i <= 16; ++i)
        n.push_back(std::to_string(i));
    return n;
}
} // namespace

Delay::Delay() : NativeDevice([] {
    using D = Delay;
    std::vector<ParameterInfo> p(D::NumParams);
    p[D::SmoothingMode] = makeParam("DelayLine_SmoothingMode", "Mode", 0.0f, 2.0f, 0.0f, "", false, {"Repitch", "Fade", "Jump"});
    p[D::Link] = makeParam("DelayLine_Link", "Link", 0.0f, 1.0f, 1.0f, "", true);
    p[D::PingPong] = makeParam("DelayLine_PingPong", "Ping Pong", 0.0f, 1.0f, 0.0f, "", true);
    p[D::SyncL] = makeParam("DelayLine_SyncL", "L Sync", 0.0f, 1.0f, 1.0f, "", true);
    p[D::SyncR] = makeParam("DelayLine_SyncR", "R Sync", 0.0f, 1.0f, 1.0f, "", true);
    p[D::TimeL] = makeParam("DelayLine_TimeL", "L Time", 0.001f, kMaxTimeSec, 0.3749999404f, "s");
    p[D::TimeR] = makeParam("DelayLine_TimeR", "R Time", 0.001f, kMaxTimeSec, 0.3749999404f, "s");
    p[D::SyncedL] = makeParam("DelayLine_SyncedSixteenthL", "L 16th", 0.0f, 15.0f, 2.0f, "", false, sixteenthNames());
    p[D::SyncedR] = makeParam("DelayLine_SyncedSixteenthR", "R 16th", 0.0f, 15.0f, 3.0f, "", false, sixteenthNames());
    p[D::OffsetL] = makeParam("DelayLine_OffsetL", "L Offset", -kMaxOffset, kMaxOffset, 0.0f, "%");
    p[D::OffsetR] = makeParam("DelayLine_OffsetR", "R Offset", -kMaxOffset, kMaxOffset, 0.0f, "%");
    p[D::Feedback] = makeParam("Feedback", "Feedback", 0.0f, 1.0f, 0.5f, "%");
    p[D::Freeze] = makeParam("Freeze", "Freeze", 0.0f, 1.0f, 0.0f, "", true);
    p[D::FilterOn] = makeParam("Filter_On", "Filter", 0.0f, 1.0f, 1.0f, "", true);
    p[D::FilterFrequency] = makeParam("Filter_Frequency", "Freq", 20.0f, 18000.0f, 1000.0f, "Hz");
    p[D::FilterBandwidth] = makeParam("Filter_Bandwidth", "Width", 0.5f, 9.0f, 8.0f);
    p[D::ModTimeMode] = makeParam("Modulation_TimeMode", "LFO Mode", 0.0f, 5.0f, 0.0f, "", false,
                                  {"Rate", "Time", "Synced", "Triplet", "Dotted", "16th"});
    p[D::ModFrequency] = makeParam("Modulation_Frequency", "Rate", 0.01f, 40.0f, 0.5f, "Hz");
    p[D::ModTime] = makeParam("Modulation_Time", "Time", 0.025f, 100.0f, 1.0f, "s");
    p[D::ModSyncedRate] = makeParam("Modulation_SyncedRate", "Note", 0.25f, 64.0f, 4.0f, "note");
    p[D::ModSixteenth] = makeParam("Modulation_Sixteenth", "16ths", 1.0f, 64.0f, 16.0f, "int");
    p[D::ModWaveform] = makeParam("Modulation_Waveform", "Wave", 0.0f, 6.0f, 1.0f, "", false,
                                  {"Sine", "Triangle", "Ramp Up", "Ramp Down", "Square", "S&H", "Wander"});
    p[D::ModMorph] = makeParam("Modulation_Morph", "Morph", 0.0f, 1.0f, 0.0f, "%");
    p[D::ModAmountTime] = makeParam("Modulation_AmountTime", "Delay", 0.0f, 1.0f, 0.0f, "%");
    p[D::ModAmountFilter] = makeParam("Modulation_AmountFilter", "Filter", 0.0f, 1.0f, 0.0f, "%");
    p[D::DryWet] = makeParam("DryWet", "Dry/Wet", 0.0f, 1.0f, 0.5f, "%");
    p[D::DryWetMode] = makeParam("DryWetMode", "Mix Law", 0.0f, 1.0f, 1.0f, "", false, {"Classic", "Equal Loud"});
    p[D::EcoProcessing] = makeParam("EcoProcessing", "Eco", 0.0f, 1.0f, 0.0f, "", true);
    // Plain <X Value=""/> leaves of the device element (no Manual): measured, the reference plays the stored mix law.
    p[D::DryWetMode].plainValue = p[D::EcoProcessing].plainValue = true;
    p[D::LegacyTimeL] = makeParam("DelayLine_SimpleDelayTimeL", "Simple L", 1.0f, kMaxTimeSec * 1000.0f, 100.0f, "ms");
    p[D::LegacyTimeR] = makeParam("DelayLine_SimpleDelayTimeR", "Simple R", 1.0f, kMaxTimeSec * 1000.0f, 100.0f, "ms");
    p[D::PingPongTimeL] = makeParam("DelayLine_PingPongDelayTimeL", "PP L", 1.0f, kMaxTimeSec * 1000.0f, 1.0f, "ms");
    p[D::PingPongTimeR] = makeParam("DelayLine_PingPongDelayTimeR", "PP R", 1.0f, kMaxTimeSec * 1000.0f, 1.0f, "ms");
    p[D::CompatibilityMode] = makeParam("DelayLine_CompatibilityMode", "Compat", 0.0f, 2.0f, 0.0f);
    p[D::CompatibilityMode].plainValue = true;
    return p;
}())
{
}

std::vector<DeviceProcessor::UiGroup> Delay::uiGroups() const
{
    return {
        {"Delay Time", {SyncL, SyncedL, TimeL, OffsetL, SyncR, SyncedR, TimeR, OffsetR, Link, PingPong, SmoothingMode}, 2},
        {"Filter", {FilterOn, FilterFrequency, FilterBandwidth}},
        {"LFO", {ModWaveform, ModMorph, ModTimeMode, ModFrequency, ModTime, ModSyncedRate, ModSixteenth, ModAmountTime,
                 ModAmountFilter}},
        {"Output", {Feedback, Freeze, DryWet, DryWetMode, EcoProcessing}},
    };
}

bool Delay::parameterShown(int index) const
{
    const int lfoMode = (int) std::lround(value(ModTimeMode));
    switch (index)
    {
        case SyncedL: return value(SyncL) > 0.5f;
        case TimeL: return value(SyncL) <= 0.5f;
        case SyncedR: return value(SyncR) > 0.5f;
        case TimeR: return value(SyncR) <= 0.5f;
        case ModFrequency: return lfoMode == 0;
        case ModTime: return lfoMode == 1;
        case ModSyncedRate: return lfoMode >= 2 && lfoMode <= 4;
        case ModSixteenth: return lfoMode == 5;
        default: return true;
    }
}

void Delay::setParameterLinked(int index, float newValue)
{
    const float before = getParameter(index);
    setParameter(index, newValue);
    if (value(Link) < 0.5f)
        return;
    const float after = getParameter(index);
    auto partnerOf = [](int i) {
        switch (i)
        {
            case SyncL: return (int) SyncR;
            case SyncR: return (int) SyncL;
            case TimeL: return (int) TimeR;
            case TimeR: return (int) TimeL;
            case SyncedL: return (int) SyncedR;
            case SyncedR: return (int) SyncedL;
            case OffsetL: return (int) OffsetR;
            case OffsetR: return (int) OffsetL;
            default: return -1;
        }
    };
    const int partner = partnerOf(index);
    if (partner < 0)
        return;
    if (index == SyncL || index == SyncR)
        setParameter(partner, after);
    else if (index == TimeL || index == TimeR)
    {
        if (before > 0.0f)
            setParameter(partner, getParameter(partner) * after / before);
    }
    else
        setParameter(partner, getParameter(partner) + (after - before));
}

void Delay::prepare(double sampleRate, int)
{
    sampleRate_ = sampleRate;
    for (auto& l : line_)
        l.prepare((int) (kBufferSec * sampleRate) + 8);
    reset();
}

void Delay::reset()
{
    for (auto& l : line_)
        l.reset();
    for (auto& line : filterState_)
        for (auto& f : line)
            f.reset();
    lfo_.reset();
    initialised_ = false;
    lastMode_ = -1;
}

// Delay length in samples requested by the controls (before modulation). With Link on, the right line plays the
// left line's settings (measured: a linked right side with its own synced index still repeats at the left time).
float Delay::targetSamples(int channel, double sixteenthSamples) const
{
    const bool left = channel == 0 || value(Link) > 0.5f;
    const bool sync = value(left ? SyncL : SyncR) > 0.5f;
    double samples = sync ? (std::round(value(left ? SyncedL : SyncedR)) + 1.0) * sixteenthSamples
                          : (double) value(left ? TimeL : TimeR) * sampleRate_;
    samples *= 1.0 + (double) value(left ? OffsetL : OffsetR);
    return (float) std::clamp(samples, std::max(1.0, 0.0005 * sampleRate_), (double) kMaxTimeSec * 1.1 * sampleRate_);
}

// Length of one LFO cycle in beats for the synced modes, 0 for the free-running ones (Rate / Time).
double Delay::modulationPeriodBeats() const
{
    switch ((int) std::lround(value(ModTimeMode)))
    {
        case 2: return 4.0 / std::max(0.25f, value(ModSyncedRate));
        case 3: return 4.0 / std::max(0.25f, value(ModSyncedRate)) * 2.0 / 3.0;
        case 4: return 4.0 / std::max(0.25f, value(ModSyncedRate)) * 1.5;
        case 5: return std::max(1.0f, value(ModSixteenth)) * 0.25;
        default: return 0.0;
    }
}

void Delay::process(juce::AudioBuffer<float>& audio, juce::MidiBuffer&)
{
    if (isBypassed())
        return;
    const int n = audio.getNumSamples();
    float* d[2] = {audio.getWritePointer(0), audio.getWritePointer(1)};

    const double sixteenth = sampleRate_ * 60.0 / tempo_ * 0.25;
    const float target[2] = {targetSamples(0, sixteenth), targetSamples(1, sixteenth)};
    const int mode = std::clamp((int) std::lround(value(SmoothingMode)), 0, 2);
    const bool pingPong = value(PingPong) > 0.5f;
    const bool freeze = value(Freeze) > 0.5f;
    const bool filterOn = value(FilterOn) > 0.5f;
    const float feedbackTarget = freeze ? 1.0f : value(Feedback);
    const float inputTarget = freeze ? 0.0f : 1.0f;
    const float wetTarget = std::clamp(value(DryWet), 0.0f, 1.0f);
    const bool equalLoudness = value(DryWetMode) > 0.5f;
    const float smooth = dsp::timeToCoef(8.0f, sampleRate_);
    if (! initialised_ || mode != lastMode_)
    {
        for (int c = 0; c < 2; ++c)
        {
            current_[c] = timeA_[c] = timeB_[c] = target[c];
            fadeMix_[c] = 1.0f;
        }
        if (! initialised_)
        {
            feedbackNow_ = feedbackTarget;
            inputNow_ = inputTarget;
            wetNow_ = wetTarget;
        }
        initialised_ = true;
        lastMode_ = mode;
    }

    // Filter (measured): 2nd-order Butterworth high-pass at Freq / 2^(Width / 2) and low-pass at Freq * 2^(Width / 2),
    // so Width is the distance in octaves between the two -3 dB points.
    const float bandwidth = std::clamp(value(FilterBandwidth), 0.5f, 9.0f);
    const float halfWidth = std::exp2(0.5f * bandwidth);
    const float baseFreq = value(FilterFrequency);
    const float filterModOctaves = value(ModAmountFilter) * 2.0f;
    const int controlStep = value(EcoProcessing) > 0.5f ? 32 : 8;

    // LFO.
    const int wave = std::clamp((int) std::lround(value(ModWaveform)), 0, (int) dsp::ModLfo::NumWaves - 1);
    const float morph = value(ModMorph);
    const double periodBeats = modulationPeriodBeats();
    double lfoInc;
    if (periodBeats > 0.0)
    {
        lfoInc = tempo_ / 60.0 / sampleRate_ / periodBeats;
        if (playing_) // lock to the song position while playing
            lfo_.setPhase(beats_ / periodBeats);
    }
    else if ((int) std::lround(value(ModTimeMode)) == 1)
        lfoInc = 1.0 / (std::max(0.025, (double) value(ModTime)) * sampleRate_);
    else
        lfoInc = (double) value(ModFrequency) / sampleRate_;
    const float timeModAmount = value(ModAmountTime);
    const float timeModSamples = timeModAmount * 0.5f * (float) (kModDepthSeconds * sampleRate_);

    // Fade mode: start a cross-fade when the requested time moved.
    if (mode == 1)
        for (int c = 0; c < 2; ++c)
            if (std::fabs(target[c] - timeB_[c]) > 0.5f)
            {
                timeA_[c] = fadeMix_[c] >= 0.5f ? timeB_[c] : timeA_[c];
                timeB_[c] = target[c];
                fadeMix_[c] = 0.0f;
            }
    const float fadeStep = 1.0f / (kFadeSeconds * (float) sampleRate_);
    const float glideCoef = dsp::timeToCoef(70.0f, sampleRate_);

    float lfoValue = 0.0f;
    for (int i = 0; i < n; ++i)
    {
        lfoValue = lfo_.next(lfoInc, wave, morph);
        if (i % controlStep == 0)
        {
            const float f = std::clamp(baseFreq * std::exp2(filterModOctaves * lfoValue), 20.0f, 18000.0f);
            // Corners are clamped to 20 Hz .. 18 kHz (measured: Width 9 at 5 kHz gives -0.70 dB at 16 kHz = LP at 18 kHz).
            highPass_ = SvfCoeffs::fromBiquad(BiquadCoeffs::make(BiquadCoeffs::Type::HighPass, sampleRate_, std::clamp(f / halfWidth, 20.0f, 18000.0f), M_SQRT1_2, 0.0));
            lowPass_ = SvfCoeffs::fromBiquad(BiquadCoeffs::make(BiquadCoeffs::Type::LowPass, sampleRate_, std::clamp(f * halfWidth, 20.0f, 18000.0f), M_SQRT1_2, 0.0));
        }
        feedbackNow_ += (feedbackTarget - feedbackNow_) * smooth;
        inputNow_ += (inputTarget - inputNow_) * smooth;
        wetNow_ += (wetTarget - wetNow_) * smooth;

        float out[2];
        for (int c = 0; c < 2; ++c)
        {
            const float mod = lfoValue * timeModSamples;
            if (mode == 0)
            {
                const float step = std::clamp((target[c] - current_[c]) * glideCoef, -kMaxGlideStep, kMaxGlideStep);
                current_[c] += step;
                out[c] = line_[c].readCubic(std::max(1.0f, current_[c] + mod - 1.0f));
            }
            else if (mode == 1)
            {
                float a = line_[c].readCubic(std::max(1.0f, timeA_[c] + mod - 1.0f));
                if (fadeMix_[c] < 1.0f)
                {
                    fadeMix_[c] = std::min(1.0f, fadeMix_[c] + fadeStep);
                    const float b = line_[c].readCubic(std::max(1.0f, timeB_[c] + mod - 1.0f));
                    a += (b - a) * fadeMix_[c];
                    if (fadeMix_[c] >= 1.0f)
                        timeA_[c] = timeB_[c];
                }
                out[c] = a;
            }
            else
            {
                current_[c] = target[c];
                out[c] = line_[c].readCubic(std::max(1.0f, current_[c] + mod - 1.0f));
            }
        }

        for (int c = 0; c < 2; ++c)
        {
            // Ping pong (measured): the equal-power mono sum feeds the left line, the left line's output feeds the right
            // line unattenuated, and the right line's output returns to the left line through Feedback.
            float sum;
            if (pingPong)
                sum = c == 0 ? 0.70710678f * (d[0][i] + d[1][i]) * inputNow_ + out[1] * feedbackNow_ : out[0];
            else
                sum = d[c][i] * inputNow_ + out[c] * feedbackNow_;
            if (filterOn && ! freeze && ! (pingPong && c == 1)) // ping pong: filtered once per round trip (left input)
                sum = filterState_[c][1].process(lowPass_, filterState_[c][0].process(highPass_, sum));
            if (std::fabs(sum) < 1e-24f)
                sum = 0.0f;
            line_[c].write(sum);
        }

        const float wet = equalLoudness ? std::sin(wetNow_ * 1.57079633f) : wetNow_;
        const float dry = equalLoudness ? std::cos(wetNow_ * 1.57079633f) : 1.0f - wetNow_;
        for (int c = 0; c < 2; ++c)
            d[c][i] = d[c][i] * dry + out[c] * wet;
    }
}

} // namespace sonora::devices
