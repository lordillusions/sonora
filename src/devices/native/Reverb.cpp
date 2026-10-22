#include "devices/native/Reverb.h"

namespace sonora::devices
{

namespace
{
// Base delay lengths (ms) of the 8 FDN lines at RoomSize 100: mutually prime-ish, 29..79 ms.
constexpr float kLineMs[8] = {29.7f, 37.1f, 41.1f, 43.7f, 53.3f, 59.9f, 67.3f, 79.1f};
constexpr float kTapMsL[8] = {4.3f, 9.1f, 15.7f, 22.9f, 31.7f, 41.3f, 53.9f, 67.1f};
constexpr float kTapMsR[8] = {5.7f, 11.3f, 17.9f, 26.3f, 35.9f, 47.3f, 59.3f, 73.9f};
constexpr float kAllpassMs[4] = {5.1f, 7.7f, 11.3f, 16.9f};
constexpr float kMaxScale = 5.0f;
constexpr float kMinScale = 0.05f;
constexpr float kSpinMsPerUnit = 0.05f;   // SpinAmount 100 == +-5 ms of tap-time modulation
constexpr float kFrozenGain = 0.99998f;   // per pass: "almost endlessly"
constexpr float kFrozenOpenGain = 0.9998f; // frozen but still taking input (Cut off): bounded, ~1 minute RT60

float sizeScale(float roomSize)
{
    return std::clamp(roomSize / 100.0f, kMinScale, kMaxScale);
}
} // namespace

Reverb::Reverb() : NativeDevice([] {
    using R = Reverb;
    std::vector<ParameterInfo> p(R::NumParams);
    p[R::PreDelay] = makeParam("PreDelay", "Predelay", 0.5f, 250.0f, 2.5f, "ms");
    p[R::BandHighOn] = makeParam("BandHighOn", "Low Cut", 0.0f, 1.0f, 0.0f, "", true);  // high-pass (measured)
    p[R::BandLowOn] = makeParam("BandLowOn", "High Cut", 0.0f, 1.0f, 1.0f, "", true);   // low-pass (measured)
    p[R::BandFreq] = makeParam("BandFreq", "In Freq", 50.0f, 18000.0f, 830.0f, "Hz");
    p[R::BandWidth] = makeParam("BandWidth", "In Width", 0.5f, 9.0f, 5.85f);
    p[R::SpinOn] = makeParam("SpinOn", "Spin", 0.0f, 1.0f, 1.0f, "", true);
    p[R::SpinRate] = makeParam("EarlyReflectModFreq", "Spin Rate", 0.01f, 5.0f, 0.2977f, "Hz");
    p[R::SpinAmount] = makeParam("EarlyReflectModDepth", "Spin Amt", 0.0f, 100.0f, 17.5f);
    p[R::Shape] = makeParam("DiffuseDelay", "Shape", 0.0f, 1.0f, 0.5f, "%");
    p[R::ShelfHighOn] = makeParam("ShelfHighOn", "Hi Shelf", 0.0f, 1.0f, 1.0f, "", true);
    p[R::HighFilterType] = makeParam("HighFilterType", "High Type", 0.0f, 1.0f, 0.0f, "", false, {"Shelf", "Cut"});
    p[R::ShelfHiFreq] = makeParam("ShelfHiFreq", "Hi Freq", 20.0f, 20000.0f, 4500.0f, "Hz");
    p[R::ShelfHiGain] = makeParam("ShelfHiGain", "Hi Gain", 0.2f, 1.0f, 0.7f);
    p[R::ShelfLowOn] = makeParam("ShelfLowOn", "Lo Shelf", 0.0f, 1.0f, 0.0f, "", true);
    p[R::ShelfLoFreq] = makeParam("ShelfLoFreq", "Lo Freq", 20.0f, 20000.0f, 90.0f, "Hz");
    p[R::ShelfLoGain] = makeParam("ShelfLoGain", "Lo Gain", 0.2f, 1.0f, 0.75f);
    p[R::ChorusOn] = makeParam("ChorusOn", "Chorus", 0.0f, 1.0f, 1.0f, "", true);
    p[R::SizeModFreq] = makeParam("SizeModFreq", "Ch. Rate", 0.01f, 10.0f, 0.02f, "Hz");
    p[R::SizeModDepth] = makeParam("SizeModDepth", "Ch. Amt", 0.0f, 5.0f, 0.02f);
    p[R::DecayTime] = makeParam("DecayTime", "Decay", 200.0f, 60000.0f, 1200.0f, "ms");
    p[R::Diffusion] = makeParam("AllPassGain", "Diffusion", 0.0f, 0.96f, 0.6f);
    p[R::Scale] = makeParam("AllPassSize", "Scale", 0.2f, 1.0f, 0.4f);
    p[R::FreezeOn] = makeParam("FreezeOn", "Freeze", 0.0f, 1.0f, 0.0f, "", true);
    p[R::FlatOn] = makeParam("FlatOn", "Flat", 0.0f, 1.0f, 1.0f, "", true);
    p[R::CutOn] = makeParam("CutOn", "Cut", 0.0f, 1.0f, 1.0f, "", true);
    p[R::RoomSize] = makeParam("RoomSize", "Size", 0.22f, 500.0f, 100.0f);
    p[R::SizeSmoothing] = makeParam("SizeSmoothing", "Smooth", 0.0f, 2.0f, 2.0f, "", false, {"None", "Slow", "Fast"});
    p[R::StereoSeparation] = makeParam("StereoSeparation", "Stereo", 0.0f, 120.0f, 100.0f, "pct");
    p[R::Density] = makeParam("RoomType", "Density", 0.0f, 3.0f, 1.0f, "", false, {"Sparse", "Low", "Mid", "High"});
    p[R::MixReflect] = makeParam("MixReflect", "Reflect", 0.0f, 2.0f, 1.0f, "lin");
    p[R::MixDiffuse] = makeParam("MixDiffuse", "Diffuse", 0.0f, 2.0f, 1.0f, "lin");
    p[R::MixDirect] = makeParam("MixDirect", "Dry/Wet", 0.0f, 1.0f, 0.55f, "%");
    p[R::StereoOnDry] = makeParam("StereoSeparationOnDrySignal", "Stereo Dry", 0.0f, 1.0f, 0.0f, "", true);
    return p;
}())
{
}

std::vector<DeviceProcessor::UiGroup> Reverb::uiGroups() const
{
    return {
        {"Input Processing", {BandLowOn, BandHighOn, BandFreq, BandWidth, PreDelay}},
        {"Early Reflections", {SpinOn, SpinRate, SpinAmount, Shape}},
        {"Diffusion Network", {ShelfHighOn, HighFilterType, ShelfHiFreq, ShelfHiGain, ShelfLowOn, ShelfLoFreq, ShelfLoGain,
                               ChorusOn, SizeModFreq, SizeModDepth, DecayTime, FreezeOn, FlatOn, CutOn, RoomSize,
                               SizeSmoothing, Diffusion, Scale, Density, StereoSeparation}},
        {"Output", {MixReflect, MixDiffuse, StereoOnDry, MixDirect}},
    };
}

void Reverb::prepare(double sampleRate, int)
{
    sampleRate_ = sampleRate;
    const int maxLine = (int) (0.080 * kMaxScale * sampleRate * 1.15) + 16;
    for (auto& l : line_)
        l.prepare(maxLine);
    for (auto& a : allpass_)
        a.prepare((int) (0.100 * sampleRate) + 16);
    for (auto& e : early_)
        e.prepare((int) (0.45 * sampleRate) + 16);
    diffuseOnset_.prepare((int) (0.12 * sampleRate) + 16);
    for (auto& p : preDelay_)
        p.prepare((int) (0.26 * sampleRate) + 16);
    reset();
    parametersChanged.store(true);
}

void Reverb::reset()
{
    for (auto& st : wetHighPassState_)
        st.reset();
    for (auto& l : line_)
        l.reset();
    for (auto& a : allpass_)
        a.reset();
    for (auto& e : early_)
        e.reset();
    for (auto& p : preDelay_)
        p.reset();
    diffuseOnset_.reset();
    lp1_.fill(0.0f);
    lp2_.fill(0.0f);
    lo_.fill(0.0f);
    for (auto& f : inLow_)
        f.reset();
    for (auto& f : inHigh_)
        f.reset();
    chorusLfo_.reset(0x1234567u);
    spinLfo_.reset(0x7654321u);
    tuned_ = false;
    parametersChanged.store(true);
}

// Derives everything that depends on size / decay / shelves / diffusion from the parameters.
void Reverb::updateTuning()
{
    const float sr = (float) sampleRate_;
    const float scale = sizeScale(value(RoomSize));
    // Measured (DEV-22b): the reference's T60 is 0.97 x Decay^0.889 (s) - 0.49 / 1.06 / 3.11 s at 0.5 / 1.2 / 4 s -
    // and this network decays in ~0.93 of its nominal time, hence the correction.
    const float decaySeconds = std::max(0.05f, value(DecayTime) * 0.001f);
    const float rt60 = 0.97f * std::pow(decaySeconds, 0.889f) / 0.93f;
    // Wet level grows with Decay in the reference (-33.4 / -28.8 / -20.4 dB tail onsets at 0.5 / 1.2 / 4 s for the same
    // input); this network's level is ~constant, so the wet signal is calibrated by -7.9 + 14.3 log10(Decay s) dB.
    // One gain for reflections and diffuse field: the law cases favour ~3 dB less reflections / 2.5 dB more diffuse
    // field, the factory presets the opposite, so the reflection/diffuse balance is left as the network makes it.
    wetCalibration_ = std::pow(10.0f, (-7.9f + 14.3f * std::log10(decaySeconds)) / 20.0f);
    reflectCalibration_ = wetCalibration_;
    // The wet signal's bass (measured, input high-pass off): -13 / -10 dB at 31 / 50 Hz relative to the mids.
    wetHighPass_ = BiquadCoeffs::make(BiquadCoeffs::Type::HighPass, sampleRate_, 90.0, M_SQRT1_2, 0.0);
    frozen_ = value(FreezeOn) > 0.5f;
    for (int i = 0; i < kLines; ++i)
    {
        lineTarget_[(size_t) i] = std::max(8.0f, kLineMs[i] * 0.001f * scale * sr);
        const float sec = lineTarget_[(size_t) i] / sr;
        lineGain_[(size_t) i] = frozen_ ? (value(CutOn) > 0.5f ? kFrozenGain : kFrozenOpenGain)
                                        : std::pow(10.0f, -3.0f * sec / rt60); // -60 dB after RT60
        if (! tuned_)
            lineNow_[(size_t) i] = lineTarget_[(size_t) i];
    }
    const int smoothing = std::clamp((int) std::lround(value(SizeSmoothing)), 0, 2);
    sizeCoef_ = smoothing == 0 ? 1.0f : dsp::timeToCoef(smoothing == 1 ? 400.0f : 60.0f, sampleRate_);

    // Early reflections: Shape shortens the reflection tail and delays the onset of the diffuse field.
    const float shape = std::clamp(value(Shape), 0.0f, 1.0f);
    const float tau = (0.12f + (0.025f - 0.12f) * shape) * std::sqrt(scale);
    for (int k = 0; k < kTaps; ++k)
    {
        tapTimeL_[(size_t) k] = kTapMsL[k] * 0.001f * scale * sr;
        tapTimeR_[(size_t) k] = kTapMsR[k] * 0.001f * scale * sr;
        const float t = 0.5f * (kTapMsL[k] + kTapMsR[k]) * 0.001f * scale;
        tapGain_[(size_t) k] = std::exp(-t / tau) * (k % 2 == 0 ? 1.0f : -0.8f);
    }
    diffuseOnsetSamples_ = shape * 0.04f * std::sqrt(scale) * sr;
    spinDepthSamples_ = value(SpinAmount) * kSpinMsPerUnit * 0.001f * sr;

    // Diffusion network: Density sets the number of all-pass stages, Scale their length.
    allpassStages_ = 1 + std::clamp((int) std::lround(value(Density)), 0, 3);
    const float apScale = value(Scale) / 0.4f * std::min(scale, 2.0f);
    for (int a = 0; a < kAllpass; ++a)
        allpassLen_[(size_t) a] = std::max(2.0f, kAllpassMs[a] * 0.001f * apScale * sr);

    hiCoef_ = dsp::timeToCoef(1000.0f / (2.0f * (float) dsp::kPi * std::clamp(value(ShelfHiFreq), 20.0f, 20000.0f)), sampleRate_);
    loCoef_ = dsp::timeToCoef(1000.0f / (2.0f * (float) dsp::kPi * std::clamp(value(ShelfLoFreq), 20.0f, 20000.0f)), sampleRate_);

    const float f = value(BandFreq), halfWidth = std::pow(2.0f, value(BandWidth) * 0.5f);
    for (int c = 0; c < 2; ++c)
    {
        inLow_[c].setup(sampleRate_, f / halfWidth, 0.7071f);   // low cut edge
        inHigh_[c].setup(sampleRate_, f * halfWidth, 0.7071f);  // high cut edge
    }
    if (! tuned_)
    {
        reflectNow_ = value(MixReflect);
        diffuseNow_ = value(MixDiffuse);
        wetNow_ = std::clamp(value(MixDirect), 0.0f, 1.0f);
        widthNow_ = value(StereoSeparation) * 0.01f;
        injectNow_ = 1.0f;
    }
    tuned_ = true;
}

void Reverb::process(juce::AudioBuffer<float>& audio, juce::MidiBuffer&)
{
    if (isBypassed())
        return;
    if (parametersChanged.exchange(false, std::memory_order_acquire) || ! tuned_)
        updateTuning();

    const int n = audio.getNumSamples();
    float* d[2] = {audio.getWritePointer(0), audio.getWritePointer(1)};

    const float preSamples = std::max(0.0f, value(PreDelay) * 0.001f * (float) sampleRate_ - 1.0f);
    // Measured (DEV-22b): "BandLowOn" switches the low-pass (high cut) and "BandHighOn" the high-pass (low cut).
    const bool lowCut = value(BandHighOn) > 0.5f, highCut = value(BandLowOn) > 0.5f;
    const bool flat = value(FlatOn) > 0.5f, cut = value(CutOn) > 0.5f;
    const bool hiShelf = value(ShelfHighOn) > 0.5f && ! (frozen_ && flat);
    const bool loShelf = value(ShelfLowOn) > 0.5f && ! (frozen_ && flat);
    const bool hiIsCut = value(HighFilterType) > 0.5f;
    const float hiGain = value(ShelfHiGain), loGain = value(ShelfLoGain);
    const bool stereoOnDry = value(StereoOnDry) > 0.5f;
    const bool spinOn = value(SpinOn) > 0.5f;
    const double spinInc = value(SpinRate) / sampleRate_;
    const double modInc = value(SizeModFreq) / sampleRate_;
    const float modDepth = value(ChorusOn) > 0.5f ? value(SizeModDepth) * 0.01f : 0.0f; // fraction of the line length
    const float reflectTarget = value(MixReflect), diffuseTarget = value(MixDiffuse);
    const float wetTarget = std::clamp(value(MixDirect), 0.0f, 1.0f);
    const float widthTarget = value(StereoSeparation) * 0.01f;
    // Frozen: Cut stops the input from adding to the tail; otherwise it keeps adding (the slightly lossy loop bounds it).
    const float injectTarget = frozen_ && cut ? 0.0f : 1.0f;
    const float smooth = dsp::timeToCoef(10.0f, sampleRate_);
    const float apGain = std::clamp(value(Diffusion), 0.0f, 0.9f);
    constexpr float h = 0.35355339f; // 1 / sqrt(8)

    for (int i = 0; i < n; ++i)
    {
        reflectNow_ += (reflectTarget - reflectNow_) * smooth;
        diffuseNow_ += (diffuseTarget - diffuseNow_) * smooth;
        wetNow_ += (wetTarget - wetNow_) * smooth;
        widthNow_ += (widthTarget - widthNow_) * smooth;
        injectNow_ += (injectTarget - injectNow_) * smooth;

        const float dryL = d[0][i], dryR = d[1][i];
        // 1) pre-delay, then the low cut / high cut band
        float in[2];
        for (int c = 0; c < 2; ++c)
        {
            preDelay_[c].write(d[c][i]);
            float x = preDelay_[c].read(preSamples);
            if (lowCut)
                x = inLow_[c].process(x, dsp::Svf::Mode::HighPass);
            if (highCut)
                x = inHigh_[c].process(x, dsp::Svf::Mode::LowPass);
            in[c] = x;
        }

        // 2) early reflections: taps of both channels, tap times modulated by Spin (opposite phase left / right)
        early_[0].write(in[0]);
        early_[1].write(in[1]);
        const float spin = spinOn ? spinLfo_.next(spinInc, dsp::ModLfo::Sine, 0.0f) * spinDepthSamples_ : 0.0f;
        float erL = 0.0f, erR = 0.0f;
        for (int k = 0; k < kTaps; ++k)
        {
            const float g = tapGain_[(size_t) k];
            const float jitter = (k % 2 == 0 ? spin : -spin);
            erL += early_[k % 2].read(std::max(0.0f, tapTimeL_[(size_t) k] + jitter)) * g;
            erR += early_[1 - k % 2].read(std::max(0.0f, tapTimeR_[(size_t) k] - jitter)) * g;
        }
        erL *= 0.35f;
        erR *= 0.35f;

        // 3) diffuse onset delay, then the all-pass diffusers feeding the network
        diffuseOnset_.write(0.5f * (in[0] + in[1]) * injectNow_);
        float diff = diffuseOnset_.read(diffuseOnsetSamples_);
        for (int a = 0; a < allpassStages_; ++a)
        {
            const float delayed = allpass_[a].read(allpassLen_[(size_t) a]);
            const float v = diff - apGain * delayed;
            allpass_[a].write(v);
            diff = delayed + apGain * v;
        }

        // 4) feedback delay network
        const float mod = chorusLfo_.next(modInc, dsp::ModLfo::Sine, 0.0f) * modDepth;
        float y[kLines];
        for (int l = 0; l < kLines; ++l)
        {
            lineNow_[(size_t) l] += (lineTarget_[(size_t) l] - lineNow_[(size_t) l]) * sizeCoef_;
            const float len = lineNow_[(size_t) l] * (1.0f + mod * (l % 2 == 0 ? 1.0f : -1.0f));
            float v = line_[l].read(std::max(1.0f, len - 1.0f));
            if (hiShelf)
            {
                lp1_[(size_t) l] += (v - lp1_[(size_t) l]) * hiCoef_;
                if (hiIsCut)
                {
                    lp2_[(size_t) l] += (lp1_[(size_t) l] - lp2_[(size_t) l]) * hiCoef_;
                    v = lp2_[(size_t) l] + (v - lp2_[(size_t) l]) * hiGain;
                }
                else
                    v = lp1_[(size_t) l] + (v - lp1_[(size_t) l]) * hiGain;
            }
            if (loShelf)
            {
                lo_[(size_t) l] += (v - lo_[(size_t) l]) * loCoef_;
                v += lo_[(size_t) l] * (loGain - 1.0f);
            }
            y[l] = v;
        }
        // Hadamard mixing (fast Walsh-Hadamard transform, 8 points)
        float z[kLines];
        for (int l = 0; l < kLines; ++l)
            z[l] = y[l];
        for (int len = 1; len < kLines; len <<= 1)
            for (int a = 0; a < kLines; a += len << 1)
                for (int b = a; b < a + len; ++b)
                {
                    const float u = z[b], w = z[b + len];
                    z[b] = u + w;
                    z[b + len] = u - w;
                }
        for (int l = 0; l < kLines; ++l)
        {
            const float inject = diff * (l % 2 == 0 ? 0.5f : -0.5f);
            float s = inject + z[l] * h * lineGain_[(size_t) l];
            if (std::fabs(s) < 1e-24f)
                s = 0.0f;
            line_[l].write(s);
        }

        // 5) outputs: alternating taps for left / right
        float lateL = 0.0f, lateR = 0.0f;
        for (int l = 0; l < kLines; ++l)
        {
            const float sign = ((l / 2) % 2 == 0) ? 1.0f : -1.0f;
            (l % 2 == 0 ? lateL : lateR) += y[l] * sign;
        }
        lateL *= 0.5f;
        lateR *= 0.5f;

        float wetL = wetHighPassState_[0].process(wetHighPass_, erL * reflectNow_ * reflectCalibration_ + lateL * diffuseNow_ * wetCalibration_);
        float wetR = wetHighPassState_[1].process(wetHighPass_, erR * reflectNow_ * reflectCalibration_ + lateR * diffuseNow_ * wetCalibration_);
        // Stereo: mid/side width, 100 % == as generated, 0 == mono, 120 % == wider than the source
        auto widen = [this](float& l, float& r) {
            const float mid = 0.5f * (l + r), side = 0.5f * (l - r) * widthNow_;
            l = mid + side;
            r = mid - side;
        };
        widen(wetL, wetR);
        float outDryL = dryL, outDryR = dryR;
        if (stereoOnDry)
            widen(outDryL, outDryR);
        d[0][i] = outDryL * (1.0f - wetNow_) + wetL * wetNow_;
        d[1][i] = outDryR * (1.0f - wetNow_) + wetR * wetNow_;
    }
}

} // namespace sonora::devices
