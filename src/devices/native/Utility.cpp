#include "devices/native/Utility.h"

namespace sonora::devices
{

namespace
{
std::vector<ParameterInfo> makeInfos()
{
    std::vector<ParameterInfo> p(Utility::NumParams);
    p[Utility::Gain] = makeParam("Gain", "Gain", 0.0f, 56.23f, 1.0f, "lin");
    p[Utility::Balance] = makeParam("Balance", "Balance", -1.0f, 1.0f, 0.0f, "");
    p[Utility::StereoWidth] = makeParam("StereoWidth", "Width", 0.0f, 4.0f, 1.0f, "%");
    p[Utility::ChannelMode] = makeParam("ChannelMode", "Channel", 0.0f, 3.0f, 1.0f, "", false, {"Left", "Stereo", "Right", "Swap"});
    p[Utility::Mono] = makeParam("Mono", "Mono", 0.0f, 1.0f, 0.0f, "", true);
    p[Utility::Mute] = makeParam("Mute", "Mute", 0.0f, 1.0f, 0.0f, "", true);
    p[Utility::PhaseInvertL] = makeParam("PhaseInvertL", "Phz-L", 0.0f, 1.0f, 0.0f, "", true);
    p[Utility::PhaseInvertR] = makeParam("PhaseInvertR", "Phz-R", 0.0f, 1.0f, 0.0f, "", true);
    p[Utility::MidSideBalance] = makeParam("MidSideBalance", "Mid/Side", 0.0f, 2.0f, 1.0f);
    p[Utility::BassMono] = makeParam("BassMono", "Bass Mono", 0.0f, 1.0f, 0.0f, "", true);
    p[Utility::BassMonoFrequency] = makeParam("BassMonoFrequency", "Bass Freq", 50.0f, 500.0f, 120.0f, "Hz");
    p[Utility::DcFilter] = makeParam("DcFilter", "DC", 0.0f, 1.0f, 0.0f, "", true);
    p[Utility::MidSideOn] = makePlainParam(makeParam("MidSideBalanceOn", "Mid/Side Mode", 0.0f, 1.0f, 0.0f, "", true));
    p[Utility::BassMonoAudition] = makePlainParam(makeParam("BassMonoAudition", "Audition", 0.0f, 1.0f, 0.0f, "", true));
    return p;
}
} // namespace

Utility::Utility() : NativeDevice(makeInfos()) {}

void Utility::prepare(double sampleRate, int)
{
    sampleRate_ = sampleRate;
    lastBassFreq_ = -1.0f;
    gainL_.reset(sampleRate, 0.02);
    gainR_.reset(sampleRate, 0.02);
}

void Utility::process(juce::AudioBuffer<float>& audio, juce::MidiBuffer&)
{
    if (isBypassed())
        return;

    const int n = audio.getNumSamples();
    float* l = audio.getWritePointer(0);
    float* r = audio.getWritePointer(1);

    const bool mute = value(Mute) > 0.5f;
    const float gain = mute ? 0.0f : value(Gain);
    const float bal = value(Balance);
    gainL_.setTargetValue(gain * juce::jmin(1.0f, 1.0f - bal));
    gainR_.setTargetValue(gain * juce::jmin(1.0f, 1.0f + bal));

    const bool midSide = value(MidSideOn) > 0.5f;
    const float width = value(Mono) > 0.5f ? 0.0f : midSide ? 1.0f : value(StereoWidth);
    const int mode = (int) std::lround(value(ChannelMode));
    const float phL = value(PhaseInvertL) > 0.5f ? -1.0f : 1.0f;
    const float phR = value(PhaseInvertR) > 0.5f ? -1.0f : 1.0f;
    const float msb = midSide ? value(MidSideBalance) : 1.0f;
    const float midGain = msb <= 1.0f ? 1.0f : 2.0f - msb;
    const float sideGain = msb >= 1.0f ? 1.0f : msb;
    const bool bassMono = value(BassMono) > 0.5f;
    const bool audition = bassMono && value(BassMonoAudition) > 0.5f;
    const bool dc = value(DcFilter) > 0.5f;
    if (bassMono && std::fabs(value(BassMonoFrequency) - lastBassFreq_) > 1e-3f)
    {
        lastBassFreq_ = value(BassMonoFrequency);
        lp_ = SvfCoeffs::fromBiquad(BiquadCoeffs::make(BiquadCoeffs::Type::LowPass, sampleRate_, lastBassFreq_, 0.7071, 0.0));
        hp_ = SvfCoeffs::fromBiquad(BiquadCoeffs::make(BiquadCoeffs::Type::HighPass, sampleRate_, lastBassFreq_, 0.7071, 0.0));
    }

    for (int i = 0; i < n; ++i)
    {
        float inL = l[i], inR = r[i];
        switch (mode)
        {
            case 0: inR = inL; break;
            case 2: inL = inR; break;
            case 3: std::swap(inL, inR); break;
            default: break;
        }
        inL *= phL;
        inR *= phR;

        if (bassMono) // everything below the crossover is summed to mono
        {
            const float lowL = lowL_[1].process(lp_, lowL_[0].process(lp_, inL));
            const float lowR = lowR_[1].process(lp_, lowR_[0].process(lp_, inR));
            const float highL = highL_[1].process(hp_, highL_[0].process(hp_, inL));
            const float highR = highR_[1].process(hp_, highR_[0].process(hp_, inR));
            const float lowMono = 0.5f * (lowL + lowR);
            inL = (audition ? 0.0f : highL) + lowMono;
            inR = (audition ? 0.0f : highR) + lowMono;
        }
        if (dc)
        {
            const float yl = inL - dcXL_ + 0.9995f * dcYL_;
            const float yr = inR - dcXR_ + 0.9995f * dcYR_;
            dcXL_ = inL;
            dcXR_ = inR;
            dcYL_ = yl;
            dcYR_ = yr;
            inL = yl;
            inR = yr;
        }

        const float mid = 0.5f * (inL + inR) * midGain;
        const float side = 0.5f * (inL - inR) * width * sideGain;
        l[i] = (mid + side) * gainL_.getNextValue();
        r[i] = (mid - side) * gainR_.getNextValue();
    }
}

} // namespace sonora::devices
