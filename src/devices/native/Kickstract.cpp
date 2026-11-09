#include "devices/native/Kickstract.h"

#include <algorithm>
#include <cmath>

namespace sonora::devices
{
namespace
{
constexpr float kSqrt2 = 1.41421356f;
constexpr float kPiF = 3.14159265f;
constexpr const char* kBandNames[Kickstract::kBands] = {"Low", "Mid", "High"};
// Per band: Length (ms after the hit the band stays open) and Fade (ms to close). Lows ring longest, the click is short.
constexpr float kDefaultLength[Kickstract::kBands] = {180, 60, 12};
constexpr float kDefaultFade[Kickstract::kBands] = {140, 50, 20};
constexpr float kRise = 1.5f;        // onset: the fast follower must be this far above the slow one
constexpr float kRearm = 1.15f;      // ...and drop back under this ratio before the next hit counts

float dbToGain(float db) { return std::pow(10.0f, db / 20.0f); }
float coefficient(double sampleRate, double seconds) { return (float) (1.0 - std::exp(-1.0 / std::max(1.0, seconds * sampleRate))); }

std::vector<ParameterInfo> kickstractParameters()
{
    std::vector<ParameterInfo> p{
        makeParam("Input", "Input", -24, 24, 0, "dB"),
        makeParam("Split/Low", "Low|Mid", 40, 600, 150, "Hz"),
        makeParam("Split/High", "Mid|High", 400, 12000, 1500, "Hz"),
        makeParam("Detect/Threshold", "Threshold", -60, 0, -30, "dB"),
        makeParam("Detect/Retrigger", "Retrigger", 20, 500, 90, "ms"),
        makeParam("Chop", "Chop", 0, 1, 1, "", true),
    };
    for (int b = 0; b < Kickstract::kBands; ++b)
    {
        const std::string band = kBandNames[b];
        p.push_back(makeParam(band + "/On", "On", 0, 1, 1, "", true));
        p.push_back(makeParam(band + "/Solo", "Solo", 0, 1, 0, "", true));
        p.push_back(makeParam(band + "/Length", "Length", 1, 1000, kDefaultLength[b], "ms"));
        p.push_back(makeParam(band + "/Fade", "Fade", 1, 1000, kDefaultFade[b], "ms"));
        p.push_back(makeParam(band + "/Gain", "Gain", -24, 12, 0, "dB"));
    }
    p.push_back(makeParam("Punch", "Punch", 0, 1, 0.25f, "%"));
    p.push_back(makeParam("Drive", "Drive", 0, 1, 0, "%"));
    p.push_back(makeParam("Output", "Output", -24, 24, 0, "dB"));
    p.push_back(makeParam("Listen", "Listen", 0, 2, 0, "", false, {"Kick", "Removed", "Input"}));
    return p;
}

// One TPT state-variable stage; returns the band-pass (v1) and low-pass (v2) outputs.
inline void svfTick(float& ic1, float& ic2, float a1, float a2, float a3, float x, float& v1, float& v2)
{
    const float v3 = x - ic2;
    v1 = a1 * ic1 + a2 * v3;
    v2 = ic2 + a2 * ic1 + a3 * v3;
    ic1 = 2.0f * v1 - ic1;
    ic2 = 2.0f * v2 - ic2;
}
} // namespace

Kickstract::Kickstract() : NativeDevice(kickstractParameters()) {}

void Kickstract::SvfCoeffs::set(double sampleRate, float hz)
{
    const float fc = std::clamp(hz, 10.0f, (float) (sampleRate * 0.45));
    g = std::tan(kPiF * fc / (float) sampleRate);
    k = kSqrt2;
    a1 = 1.0f / (1.0f + g * (g + k));
    a2 = g * a1;
    a3 = g * a2;
}

void Kickstract::Lr4::split(const SvfCoeffs& c, float x, float& low, float& high)
{
    float v1 = 0, v2 = 0;
    svfTick(lp[0].ic1, lp[0].ic2, c.a1, c.a2, c.a3, x, v1, v2);
    svfTick(lp[1].ic1, lp[1].ic2, c.a1, c.a2, c.a3, v2, v1, v2);
    low = v2;
    svfTick(hp[0].ic1, hp[0].ic2, c.a1, c.a2, c.a3, x, v1, v2);
    const float h1 = x - c.k * v1 - v2;
    svfTick(hp[1].ic1, hp[1].ic2, c.a1, c.a2, c.a3, h1, v1, v2);
    high = h1 - c.k * v1 - v2;
}

float Kickstract::Allpass::process(const SvfCoeffs& c, float x)
{
    float v1 = 0, v2 = 0;
    svfTick(s.ic1, s.ic2, c.a1, c.a2, c.a3, x, v1, v2);
    return x - 2.0f * c.k * v1;
}

float Kickstract::lowSplitHz() const
{
    return std::clamp(getParameter(LowSplit), 20.0f, (float) (sampleRate_ * 0.2));
}

float Kickstract::highSplitHz() const
{
    return std::clamp(std::max(getParameter(HighSplit), lowSplitHz() * 1.25f), 30.0f, (float) (sampleRate_ * 0.42));
}

void Kickstract::prepare(double sampleRate, int)
{
    sampleRate_ = std::max(1000.0, sampleRate);
    lookahead_ = std::max(1, (int) std::lround(kLookaheadSeconds * sampleRate_));
    for (auto& d : delay_) d.assign((size_t) lookahead_, 0.0f);
    onsetEnv_.assign((size_t) lookahead_, 0.0f);
    inputTap.prepare(sampleRate_);
    outputTap.prepare(sampleRate_);
    timelineBinSamples_ = std::max(1, (int) std::lround(kTimelineBinSeconds * sampleRate_));
    reset();
}

void Kickstract::reset()
{
    for (auto& d : delay_) std::fill(d.begin(), d.end(), 0.0f);
    std::fill(onsetEnv_.begin(), onsetEnv_.end(), 0.0f);
    broadband_ = 0;
    pendingOpen_ = -1;
    delayPos_ = 0;
    for (int ch = 0; ch < 2; ++ch)
    {
        split1_[ch].reset(); split2_[ch].reset(); lowAllpass_[ch].s.reset();
    }
    detectSplit_.reset();
    for (auto& g : gates_) g = Gate{};
    fast_ = slow_ = compEnv_ = 0;
    sinceTrigger_ = 1 << 30;
    armed_ = true;
    captureBin_ = -1;
    updateCoefficients(true);
}

void Kickstract::updateCoefficients(bool immediate)
{
    const float lowTarget = lowSplitHz(), highTarget = highSplitHz();
    // Glide per block (about 20 ms at 512 samples / 48 kHz) so a dragged split does not click.
    const float amount = immediate ? 1.0f : 0.35f;
    lowHz_ = immediate ? lowTarget : lowHz_ * std::pow(lowTarget / lowHz_, amount);
    highHz_ = immediate ? highTarget : highHz_ * std::pow(highTarget / highHz_, amount);
    if (! std::isfinite(lowHz_) || lowHz_ <= 0) lowHz_ = lowTarget;
    if (! std::isfinite(highHz_) || highHz_ <= 0) highHz_ = highTarget;
    lowC_.set(sampleRate_, lowHz_);
    highC_.set(sampleRate_, highHz_);
}

void Kickstract::trigger(int newestPos)
{
    sinceTrigger_ = 0;
    armed_ = false;
    triggers_.fetch_add(1, std::memory_order_relaxed);
    // Where did the attack start? Walk back from the newest sample until the broadband envelope falls to the window's
    // floor (what played before the kick: hats, bass, a pad): the onset is the sample after that. A pitch-dropping kick's
    // beater click comes 15-20 ms before its low band rises (measured on a real drum stem), and its level is a tenth of
    // the body's, so the bar is relative to the floor, not to the body's peak. The onset leaves the delay line
    // `lookahead_ - back` samples from now; the gates finish opening half a millisecond before it.
    float peak = 0, floor = onsetEnv_[0];
    for (float v : onsetEnv_)
    {
        peak = std::max(peak, v);
        floor = std::min(floor, v);
    }
    const float onsetLevel = floor * 1.3f + 0.01f * (peak - floor);
    int back = lookahead_ - 1;
    for (int i = 0; i < lookahead_; ++i)
        if (onsetEnv_[(size_t) ((newestPos - i + lookahead_) % lookahead_)] <= onsetLevel)
        {
            back = std::max(0, i - 1);
            break;
        }
    const int ramp = (int) std::lround(kAttackSeconds * sampleRate_), margin = (int) std::lround(0.0005 * sampleRate_);
    pendingOpen_ = std::max(0, lookahead_ - back - ramp - margin);
}

void Kickstract::openGates()
{
    timelineHit_ = true;
    if (getParameter(Chop) > 0.5f)
        for (auto& g : gates_)
        {
            g.stage = Gate::Attack;
            g.counter = 0;
        }
    // Kick capture from the gates' opening: the window covers the longest band's Length + Fade.
    if (captureBin_ > 0)
    {
        auto& c = captures_[writing_];
        c.bins.store(captureBin_, std::memory_order_relaxed);
        published_.store(writing_, std::memory_order_release);
        captureSerial_.fetch_add(1, std::memory_order_release);
        writing_ ^= 1;
    }
    float longest = 0;
    for (int b = 0; b < kBands; ++b)
        longest = std::max(longest, getParameter(bandParam(b, BandLength)) + getParameter(bandParam(b, BandFade)));
    const float windowMs = std::clamp(longest * 1.15f + 10.0f, 60.0f, 1500.0f);
    binSamples_ = std::max(1, (int) std::lround(windowMs * 0.001 * sampleRate_ / kCaptureBins));
    captures_[writing_].windowMs.store((float) binSamples_ * kCaptureBins * 1000.0f / (float) sampleRate_, std::memory_order_relaxed);
    captureBin_ = 0;
    binCounter_ = 0;
    binOutMin_ = binOutMax_ = binIn_ = 0;
    for (auto& v : binBand_) v = 0;
}

float Kickstract::gateStep(Gate& g, int band)
{
    switch (g.stage)
    {
        case Gate::Idle: return g.level = 0.0f;
        case Gate::Attack:
        {
            g.level += 1.0f / std::max(1.0f, (float) (kAttackSeconds * sampleRate_));
            if (g.level >= 1.0f)
            {
                g.level = 1.0f;
                g.stage = Gate::Hold;
                g.counter = (int) std::lround(getParameter(bandParam(band, BandLength)) * 0.001 * sampleRate_); // from the onset
            }
            return g.level;
        }
        case Gate::Hold:
            if (--g.counter <= 0)
            {
                g.stage = Gate::Release;
                g.releaseFrom = g.level;
                g.total = g.counter = std::max(1, (int) std::lround(getParameter(bandParam(band, BandFade)) * 0.001 * sampleRate_));
            }
            return g.level;
        case Gate::Release:
        {
            const float r = std::clamp((float) --g.counter / (float) std::max(1, g.total), 0.0f, 1.0f);
            g.level = g.releaseFrom * r * r;
            if (g.counter <= 0)
            {
                g.stage = Gate::Idle;
                g.level = 0.0f;
            }
            return g.level;
        }
    }
    return g.level;
}

void Kickstract::timelineSample(float in, float kick, float removed)
{
    const float v[kTimelineLanes] = {in, kick, removed};
    for (int l = 0; l < kTimelineLanes; ++l)
    {
        timelineMin_[l] = timelineCounter_ == 0 ? v[l] : std::min(timelineMin_[l], v[l]);
        timelineMax_[l] = timelineCounter_ == 0 ? v[l] : std::max(timelineMax_[l], v[l]);
    }
    if (++timelineCounter_ < timelineBinSamples_) return;
    const auto written = timelineWritten_.load(std::memory_order_relaxed);
    const auto at = (size_t) (written % (uint64_t) kTimelineBins);
    for (size_t l = 0; l < (size_t) kTimelineLanes; ++l)
    {
        timeline_.min[l][at].store(timelineMin_[l], std::memory_order_relaxed);
        timeline_.max[l][at].store(timelineMax_[l], std::memory_order_relaxed);
    }
    timeline_.hit[at].store(timelineHit_, std::memory_order_relaxed);
    timelineWritten_.store(written + 1, std::memory_order_release);
    timelineCounter_ = 0;
    timelineHit_ = false;
}

Kickstract::TimelineBin Kickstract::timelineBin(uint64_t index) const
{
    TimelineBin bin;
    const auto at = (size_t) (index % (uint64_t) kTimelineBins);
    for (size_t l = 0; l < (size_t) kTimelineLanes; ++l)
    {
        bin.min[l] = timeline_.min[l][at].load(std::memory_order_relaxed);
        bin.max[l] = timeline_.max[l][at].load(std::memory_order_relaxed);
    }
    bin.hit = timeline_.hit[at].load(std::memory_order_relaxed);
    return bin;
}

void Kickstract::captureSample(float outL, float outR, float in, const float (&bands)[kBands])
{
    if (captureBin_ < 0) return;
    const float out = 0.5f * (outL + outR);
    binOutMin_ = std::min(binOutMin_, out);
    binOutMax_ = std::max(binOutMax_, out);
    binIn_ = std::max(binIn_, std::abs(in));
    for (int b = 0; b < kBands; ++b) binBand_[b] = std::max(binBand_[b], std::abs(bands[b]));
    if (++binCounter_ < binSamples_) return;
    auto& c = captures_[writing_];
    const auto i = (size_t) captureBin_;
    c.outMin[i].store(binOutMin_, std::memory_order_relaxed);
    c.outMax[i].store(binOutMax_, std::memory_order_relaxed);
    c.inPeak[i].store(binIn_, std::memory_order_relaxed);
    for (size_t b = 0; b < (size_t) kBands; ++b) c.band[b][i].store(binBand_[b], std::memory_order_relaxed);
    binCounter_ = 0;
    binOutMin_ = binOutMax_ = binIn_ = 0;
    for (auto& v : binBand_) v = 0;
    if (++captureBin_ >= kCaptureBins)
    {
        c.bins.store(kCaptureBins, std::memory_order_relaxed);
        published_.store(writing_, std::memory_order_release);
        captureSerial_.fetch_add(1, std::memory_order_release);
        writing_ ^= 1;
        captureBin_ = -1;
    }
}

bool Kickstract::takeKick(KickShape& out, uint32_t serial) const
{
    const uint32_t current = captureSerial_.load(std::memory_order_acquire);
    const int slot = published_.load(std::memory_order_acquire);
    if (slot < 0 || current == serial) return false;
    const auto& c = captures_[slot];
    out.bins = std::clamp(c.bins.load(std::memory_order_relaxed), 0, kCaptureBins);
    out.windowMs = c.windowMs.load(std::memory_order_relaxed);
    for (size_t i = 0; i < (size_t) kCaptureBins; ++i)
    {
        out.outMin[i] = c.outMin[i].load(std::memory_order_relaxed);
        out.outMax[i] = c.outMax[i].load(std::memory_order_relaxed);
        out.inPeak[i] = c.inPeak[i].load(std::memory_order_relaxed);
        for (size_t b = 0; b < (size_t) kBands; ++b) out.band[b][i] = c.band[b][i].load(std::memory_order_relaxed);
    }
    out.serial = current;
    return true;
}

void Kickstract::process(juce::AudioBuffer<float>& audio, juce::MidiBuffer&)
{
    const int n = audio.getNumSamples();
    if (n <= 0 || audio.getNumChannels() < 1 || lookahead_ <= 0 || delay_[0].size() != (size_t) lookahead_) return;
    float* left = audio.getWritePointer(0);
    float* right = audio.getNumChannels() > 1 ? audio.getWritePointer(1) : nullptr;

    if (isBypassed()) // keep the latency: the delayed input
    {
        for (int i = 0; i < n; ++i)
        {
            const auto pos = (size_t) delayPos_;
            const float l = delay_[0][pos], r = delay_[1][pos];
            delay_[0][pos] = left[i];
            delay_[1][pos] = right ? right[i] : left[i];
            left[i] = l;
            if (right) right[i] = r;
            delayPos_ = (delayPos_ + 1) % lookahead_;
        }
        return;
    }

    parametersChanged.store(false, std::memory_order_relaxed);
    updateCoefficients(false);
    inputTap.push(left, right, n);

    const float inGain = dbToGain(getParameter(Input));
    const float threshold = dbToGain(getParameter(Threshold));
    const int retrigger = (int) std::lround(getParameter(Retrigger) * 0.001 * sampleRate_);
    const bool chop = getParameter(Chop) > 0.5f;
    bool anySolo = false;
    for (int b = 0; b < kBands; ++b) anySolo |= getParameter(bandParam(b, BandSolo)) > 0.5f;
    float bandGain[kBands];
    for (int b = 0; b < kBands; ++b)
    {
        const bool on = getParameter(bandParam(b, BandOn)) > 0.5f && (! anySolo || getParameter(bandParam(b, BandSolo)) > 0.5f);
        bandGain[b] = on ? dbToGain(getParameter(bandParam(b, BandGain))) : 0.0f;
    }
    const float punch = getParameter(Punch), drive = getParameter(Drive);
    const float outGain = dbToGain(getParameter(Output));
    const int listen = std::clamp((int) std::lround(getParameter(Listen)), 0, 2);

    const float fastAttack = coefficient(sampleRate_, 0.0003), fastRelease = coefficient(sampleRate_, 0.02);
    const float slowAttack = coefficient(sampleRate_, 0.015), slowRelease = coefficient(sampleRate_, 0.25);
    const float broadbandRelease = std::exp(-1.0f / (float) (0.003 * sampleRate_));
    const float compAttack = coefficient(sampleRate_, 0.010), compRelease = coefficient(sampleRate_, 0.12);
    const float thresholdDb = -6.0f - 24.0f * punch, ratio = 1.0f + 5.0f * punch;
    const float makeupDb = -thresholdDb * (1.0f - 1.0f / ratio) * 0.5f;
    const float driveGain = 1.0f + 7.0f * drive, driveNorm = 1.0f / std::sqrt(driveGain);
    float detectorPeak = 0;

    for (int i = 0; i < n; ++i)
    {
        const float inL = left[i] * inGain, inR = (right ? right[i] : left[i]) * inGain;

        // Onset envelope (broadband peak, 3 ms release), stored where this sample enters the delay line.
        broadband_ = std::max(std::abs(0.5f * (inL + inR)), broadband_ * broadbandRelease);
        onsetEnv_[(size_t) delayPos_] = broadband_;

        // Detector: the undelayed low band.
        float detLow = 0, detHigh = 0;
        detectSplit_.split(lowC_, 0.5f * (inL + inR), detLow, detHigh);
        const float level = std::abs(detLow);
        fast_ += (level - fast_) * (level > fast_ ? fastAttack : fastRelease);
        slow_ += (level - slow_) * (level > slow_ ? slowAttack : slowRelease);
        detectorPeak = std::max(detectorPeak, fast_);
        if (sinceTrigger_ < (1 << 30)) ++sinceTrigger_;
        if (! armed_ && fast_ < slow_ * kRearm) armed_ = true;
        if (armed_ && fast_ > threshold && fast_ > slow_ * kRise && sinceTrigger_ >= retrigger) trigger(delayPos_);

        // Audio path: lookahead delay, then the 3-band split.
        const auto pos = (size_t) delayPos_;
        const float x[2] = {delay_[0][pos], delay_[1][pos]};
        delay_[0][pos] = inL;
        delay_[1][pos] = inR;
        delayPos_ = (delayPos_ + 1) % lookahead_;

        if (pendingOpen_ >= 0 && pendingOpen_-- == 0) openGates();
        float gate[kBands];
        for (int b = 0; b < kBands; ++b) gate[b] = chop ? gateStep(gates_[b], b) : 1.0f;

        float kick[2], removed[2], bandMono[kBands] = {};
        for (int ch = 0; ch < 2; ++ch)
        {
            float low = 0, rest = 0, mid = 0, high = 0;
            split1_[ch].split(lowC_, x[ch], low, rest);
            split2_[ch].split(highC_, rest, mid, high);
            low = lowAllpass_[ch].process(highC_, low); // the low band through the high split's phase: a flat sum
            const float band[kBands] = {low, mid, high};
            kick[ch] = 0;
            removed[ch] = 0;
            for (int b = 0; b < kBands; ++b)
            {
                const float kept = band[b] * gate[b] * bandGain[b];
                kick[ch] += kept;
                removed[ch] += band[b] * (1.0f - gate[b] * (bandGain[b] > 0 ? 1.0f : 0.0f));
                bandMono[b] += 0.5f * kept;
            }
        }

        // Finish: Punch (slow attack lets the hit through, then holds the body), Drive.
        if (punch > 0.001f)
        {
            const float peak = std::max(std::abs(kick[0]), std::abs(kick[1]));
            compEnv_ += (peak - compEnv_) * (peak > compEnv_ ? compAttack : compRelease);
            const float envDb = 20.0f * std::log10(std::max(compEnv_, 1e-6f));
            const float over = envDb - thresholdDb;
            const float gainDb = (over > 0 ? -over * (1.0f - 1.0f / ratio) : 0.0f) + makeupDb;
            const float g = dbToGain(gainDb);
            kick[0] *= g;
            kick[1] *= g;
        }
        if (drive > 0.001f)
            for (float& v : kick) v = std::tanh(driveGain * v) * driveNorm;

        float out[2];
        for (int ch = 0; ch < 2; ++ch)
            out[ch] = (listen == ListenKick ? kick[ch] : listen == ListenRemoved ? removed[ch] : x[ch]) * outGain;
        captureSample(kick[0] * outGain, kick[1] * outGain, 0.5f * (x[0] + x[1]), bandMono);
        timelineSample(0.5f * (x[0] + x[1]), 0.5f * (kick[0] + kick[1]) * outGain, 0.5f * (removed[0] + removed[1]) * outGain);
        left[i] = out[0];
        if (right) right[i] = out[1];
        if (i == 0 || i == n - 1)
            for (int b = 0; b < kBands; ++b) bandLevel_[(size_t) b].store(gate[b] * (bandGain[b] > 0 ? 1.0f : 0.0f), std::memory_order_relaxed);
    }
    if (detectorPeak > detectorPeak_.load(std::memory_order_relaxed)) detectorPeak_.store(detectorPeak, std::memory_order_relaxed);
    outputTap.push(left, right, n);
}

std::vector<DeviceProcessor::UiGroup> Kickstract::uiGroups() const
{
    std::vector<UiGroup> groups{{"Detect", {Input, Threshold, Retrigger, Chop}, 2}, {"Split", {LowSplit, HighSplit}, 1}};
    for (int b = 0; b < kBands; ++b)
        groups.push_back({kBandNames[b], {bandParam(b, BandOn), bandParam(b, BandSolo), bandParam(b, BandLength),
                                          bandParam(b, BandFade), bandParam(b, BandGain)}, 2});
    groups.push_back({"Finish", {Punch, Drive, Output, Listen}, 2});
    return groups;
}
} // namespace sonora::devices
