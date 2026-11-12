#include "devices/native/PhaseAlign.h"

#include <algorithm>
#include <cmath>

namespace sonora::devices
{
namespace
{
constexpr double kPi = 3.14159265358979323846;
// Olli Niemitalo's 90-degree allpass pair (4 second-order sections each): A one sample late and B are 90 degrees apart
// within about a degree from 20 Hz up at 44.1 / 48 kHz.
constexpr float kChainA[4] = {0.6923878f, 0.9360654322959f, 0.9882295226860f, 0.9987488452737f};
constexpr float kChainB[4] = {0.4021921162426f, 0.8561710882420f, 0.9722909545651f, 0.9952884791278f};

std::vector<ParameterInfo> phaseParameters()
{
    using P = PhaseAlign;
    std::vector<ParameterInfo> p(P::NumParams);
    p[P::Crossover] = makeParam("Crossover", "Crossover", 60, 400, 150, "Hz");
    p[P::ModeParam] = makeParam("Mode", "Mode", 0, 2, 0, "", false, {"Auto", "Hold", "Manual"});
    p[P::Angle] = makeParam("Angle", "Angle", -180, 180, 0, "deg");
    p[P::Speed] = makeParam("Speed", "Speed", 5, 2000, 120, "ms");
    p[P::Threshold] = makeParam("Threshold", "Threshold", -60, 0, -30, "dB");
    p[P::Window] = makeParam("Window", "Window", 20, 400, 150, "ms");
    p[P::MinCoherence] = makeParam("MinCoherence", "Min. Coherence", 0, 1, 0.25f, "%");
    p[P::Listen] = makeParam("Listen", "Listen", 0, 2, 0, "", false, {"Bass", "Sum", "Original Sum"});
    p[P::Output] = makeParam("Output", "Output", -24, 12, 0, "dB");
    return p;
}

float wrapDegrees(float d)
{
    d = std::fmod(d + 180.0f, 360.0f);
    if (d < 0) d += 360.0f;
    return d - 180.0f;
}
} // namespace

PhaseAlign::PhaseAlign() : NativeDevice(phaseParameters()) {}

void PhaseAlign::SvfCoeffs::set(double sampleRate, float hz)
{
    const double g = std::tan(kPi * std::min((double) hz, sampleRate * 0.45) / sampleRate);
    k = 1.41421356f;
    a1 = (float) (1.0 / (1.0 + g * (g + k)));
    a2 = (float) g * a1;
    a3 = (float) g * a2;
}

void PhaseAlign::split(Lr4& f, const SvfCoeffs& c, float x, float& low, float& high)
{
    const auto stage = [&c](Svf& s, float in, float& lp, float& hp) {
        const float v3 = in - s.ic2, v1 = c.a1 * s.ic1 + c.a2 * v3, v2 = s.ic2 + c.a2 * s.ic1 + c.a3 * v3;
        s.ic1 = 2 * v1 - s.ic1;
        s.ic2 = 2 * v2 - s.ic2;
        lp = v2;
        hp = in - c.k * v1 - v2;
    };
    float l1, h1, l2, h2, unused;
    stage(f.lp[0], x, l1, unused);
    stage(f.lp[1], l1, l2, unused);
    stage(f.hp[0], x, unused, h1);
    stage(f.hp[1], h1, unused, h2);
    low = l2;
    high = h2;
}

float PhaseAlign::hilbertStep(HilbertChain& c, const float (&coefficients)[4], float x)
{
    for (int s = 0; s < 4; ++s)
    {
        const float a2 = coefficients[s] * coefficients[s];
        const float y = a2 * (x + c.y2[(size_t) s]) - c.x2[(size_t) s];
        c.x2[(size_t) s] = c.x1[(size_t) s];
        c.x1[(size_t) s] = x;
        c.y2[(size_t) s] = c.y1[(size_t) s];
        c.y1[(size_t) s] = y;
        x = y;
    }
    return x;
}

void PhaseAlign::prepare(double sampleRate, int)
{
    sampleRate_ = sampleRate > 0 ? sampleRate : 44100.0;
    reset();
}

void PhaseAlign::reset()
{
    for (auto& f : bassSplit_) f = {};
    kickSplit_ = {};
    for (auto& c : chainA_) c = {};
    for (auto& c : chainB_) c = {};
    delayA_[0] = delayA_[1] = 0;
    fast_ = slow_ = 0;
    armed_ = true;
    inHit_ = false;
    sinceHit_ = 1 << 30;
    crossoverHz_ = 0;
    angle_ = learned_ = 0;
    cosA_ = 1;
    sinA_ = 0;
    rotationCounter_ = 0;
    mix_ = mixTarget_ = std::lround(value(ModeParam)) == Manual ? 1.0f : 0.0f;
    recent_ = 0;
    verdict_.store(NoKick);
}

void PhaseAlign::finishHit()
{
    inHit_ = false;
    const double bass = 0.5 * (aa_ + bb_);
    const double magnitude = std::sqrt(ca_ * ca_ + cb_ * cb_);
    const double coherence = kk_ > 1e-12 && bass > 1e-12 ? std::min(1.0, magnitude / std::sqrt(kk_ * bass)) : 0.0;
    const float best = (float) (std::atan2(-cb_, ca_) * 180.0 / kPi);
    // Low-end energy of kick + bass over the window: the bass as it came in vs rotated to the best angle (potential) and
    // vs rotated to the angle that plays now (applied).
    const double before = ll_, after = kl_ + bass + hh_ + 2.0 * (magnitude + kh_);
    const double applied = std::cos((double) angle_ * kPi / 180.0) * ca_ - std::sin((double) angle_ * kPi / 180.0) * cb_;
    const double afterApplied = kl_ + bass + hh_ + 2.0 * (applied + kh_);
    const auto db = [before](double e) { return before > 1e-12 && e > 1e-12 ? (float) (10.0 * std::log10(e / before)) : 0.0f; };
    const float potentialDb = db(after);
    const float appliedDb = mix_ > 0.5f ? db(afterApplied) : 0.0f;
    const float bassUnderKick = kk_ > 1e-12 ? (float) (10.0 * std::log10(std::max(bass, 1e-30) / kk_)) : -120.0f;
    const bool used = coherence >= (double) value(MinCoherence) && kk_ > 1e-10;
    const int mode = (int) std::lround(value(ModeParam));
    const bool worth = used && potentialDb >= kWorthDb;
    recent_ = ((recent_ << 1) | (worth ? 1u : 0u)) & 0xFFu;
    int worthCount = 0;
    for (uint32_t bits = recent_; bits != 0; bits >>= 1) worthCount += (int) (bits & 1u);
    if (mode == Auto)
    {
        if (worth) learned_ = best;
        if (worthCount >= 2) mixTarget_ = 1.0f;
        else if (worthCount == 0) mixTarget_ = 0.0f;
    }
    Verdict v = Aligning;
    if (mode == Manual) v = ManualAngle;
    else if (! used) v = bassUnderKick < -40.0f ? Silent : Unrelated;
    else if (potentialDb < kWorthDb) v = InPhase;
    verdict_.store(v, std::memory_order_relaxed);

    coherence_.store((float) coherence, std::memory_order_relaxed);
    gainDb_.store(appliedDb, std::memory_order_relaxed);
    potentialDb_.store(potentialDb, std::memory_order_relaxed);
    bassUnderKickDb_.store(bassUnderKick, std::memory_order_relaxed);
    const int slot = historyNext_.load(std::memory_order_relaxed);
    histAngle_[(size_t) slot].store(best, std::memory_order_relaxed);
    histCoherence_[(size_t) slot].store((float) coherence, std::memory_order_relaxed);
    histGain_[(size_t) slot].store(appliedDb, std::memory_order_relaxed);
    histPotential_[(size_t) slot].store(potentialDb, std::memory_order_relaxed);
    histBass_[(size_t) slot].store(bassUnderKick, std::memory_order_relaxed);
    histUsed_[(size_t) slot].store(used, std::memory_order_relaxed);
    historyNext_.store((slot + 1) % kHistory, std::memory_order_relaxed);
    hits_.fetch_add(1, std::memory_order_release);

    // The last hit's picture, the bass rotated to that hit's best angle.
    const float c = std::cos(best * (float) kPi / 180.0f), s = std::sin(best * (float) kPi / 180.0f);
    const int bins = std::min(shapeBin_, kShapeBins);
    for (int b = 0; b < bins; ++b)
    {
        shapeKick_[(size_t) b].store(capKick_[(size_t) b], std::memory_order_relaxed);
        shapeBefore_[(size_t) b].store(capLow_[(size_t) b], std::memory_order_relaxed);
        shapeAfter_[(size_t) b].store(c * capA_[(size_t) b] - s * capB_[(size_t) b], std::memory_order_relaxed);
    }
    shapeBins_.store(bins, std::memory_order_relaxed);
    shapeSerial_.fetch_add(1, std::memory_order_release);
}

void PhaseAlign::process(juce::AudioBuffer<float>& audio, juce::MidiBuffer&)
{
    const int n = audio.getNumSamples();
    if (n <= 0 || audio.getNumChannels() < 1) return;
    float* left = audio.getWritePointer(0);
    float* right = audio.getNumChannels() > 1 ? audio.getWritePointer(1) : left;
    const juce::AudioBuffer<float>* sc = sidechain_ != nullptr && sidechain_->getNumSamples() >= n && sidechain_->getNumChannels() > 0
                                             ? sidechain_ : nullptr;
    sidechainPresent_.store(sc != nullptr, std::memory_order_relaxed);
    const float* kickL = sc != nullptr ? sc->getReadPointer(0) : nullptr;
    const float* kickR = sc != nullptr ? sc->getReadPointer(std::min(1, sc->getNumChannels() - 1)) : nullptr;

    if (const float hz = value(Crossover); std::abs(hz - crossoverHz_) > 0.01f)
    {
        crossoverHz_ = hz;
        crossover_.set(sampleRate_, hz);
    }
    const int mode = (int) std::lround(value(ModeParam));
    const int listen = (int) std::lround(value(Listen));
    const float threshold = juce::Decibels::decibelsToGain(value(Threshold));
    const float output = juce::Decibels::decibelsToGain(value(Output));
    windowSamples_ = std::max(16, (int) (value(Window) * 0.001f * (float) sampleRate_));
    shapeSamplesPerBin_ = std::max(1, windowSamples_ / kShapeBins);
    const float target = mode == Manual ? value(Angle) : learned_;
    const float degreesPerSample = 180.0f / std::max(1.0f, value(Speed) * 0.001f * (float) sampleRate_);
    const auto coefficient = [&](double seconds) { return (float) std::exp(-1.0 / (seconds * sampleRate_)); };
    const float fastAttack = coefficient(0.0005), fastRelease = coefficient(0.03);
    const float slowAttack = coefficient(0.03), slowRelease = coefficient(0.3);
    const int refractory = (int) (0.06 * sampleRate_);
    const bool bypass = isBypassed();
    if (mode == Manual) mixTarget_ = 1.0f;
    if (kickL == nullptr && mode == Auto) mixTarget_ = 0.0f; // no kick: nothing to follow
    if (kickL == nullptr) verdict_.store(mode == Manual ? ManualAngle : NoKick, std::memory_order_relaxed);
    else if (hits_.load(std::memory_order_relaxed) == 0 && mode != Manual) verdict_.store(Waiting, std::memory_order_relaxed);
    const float mixStep = 1.0f - (float) std::exp(-1.0 / (0.15 * sampleRate_));

    for (int i = 0; i < n; ++i)
    {
        const float inL = left[i], inR = right[i];
        // ---- the bass: split, rotate the low band
        float lowL, highL, lowR, highR;
        split(bassSplit_[0], crossover_, inL, lowL, highL);
        split(bassSplit_[1], crossover_, inR, lowR, highR);
        float a[2], b[2];
        const float lows[2] = {lowL, lowR};
        for (int ch = 0; ch < 2; ++ch)
        {
            const float rawA = hilbertStep(chainA_[ch], kChainA, lows[ch]);
            a[ch] = delayA_[ch];
            delayA_[ch] = rawA;
            b[ch] = hilbertStep(chainB_[ch], kChainB, lows[ch]);
        }

        // ---- glide the angle (cos/sin every 16 samples)
        if (--rotationCounter_ <= 0)
        {
            rotationCounter_ = 16;
            const float delta = wrapDegrees(target - angle_);
            const float step = degreesPerSample * 16.0f;
            angle_ = std::abs(delta) <= step ? target : wrapDegrees(angle_ + std::copysign(step, delta));
            cosA_ = std::cos((double) angle_ * kPi / 180.0);
            sinA_ = std::sin((double) angle_ * kPi / 180.0);
        }
        const float rotL = (float) (cosA_ * a[0] - sinA_ * b[0]), rotR = (float) (cosA_ * a[1] - sinA_ * b[1]);

        // ---- the kick: low band, detector, correlation over the hit's window
        float kickFull = 0, kickLow = 0;
        if (kickL != nullptr)
        {
            kickFull = 0.5f * (kickL[i] + kickR[i]) * sidechainGain_;
            float unused;
            split(kickSplit_, crossover_, kickFull, kickLow, unused);
            const float level = std::abs(kickLow);
            fast_ = level > fast_ ? fastAttack * fast_ + (1 - fastAttack) * level : fastRelease * fast_ + (1 - fastRelease) * level;
            slow_ = level > slow_ ? slowAttack * slow_ + (1 - slowAttack) * level : slowRelease * slow_ + (1 - slowRelease) * level;
            ++sinceHit_;
            if (! armed_ && fast_ < 1.1f * slow_) armed_ = true;
            if (armed_ && ! inHit_ && sinceHit_ > refractory && fast_ > threshold && fast_ > 1.4f * slow_)
            {
                armed_ = false;
                inHit_ = true;
                sinceHit_ = hitSamples_ = 0;
                ca_ = cb_ = kk_ = aa_ = bb_ = kl_ = ll_ = hh_ = kh_ = 0;
                shapeBin_ = shapeCounter_ = 0;
                binKick_ = binLow_ = binA_ = binB_ = 0;
            }
        }
        if (inHit_)
        {
            const float monoA = 0.5f * (a[0] + a[1]), monoB = 0.5f * (b[0] + b[1]), monoLow = 0.5f * (lowL + lowR);
            // Against the kick as it sounds (unfiltered): the low-pass would shift its phase. The bass's A/B only hold its
            // low band, so the kick's click and highs barely count. Coherence is normalised by the kick's low-band energy.
            const float monoHigh = 0.5f * (highL + highR), monoIn = 0.5f * (inL + inR);
            ca_ += (double) kickFull * monoA;
            cb_ += (double) kickFull * monoB;
            kk_ += (double) kickLow * kickLow;
            aa_ += (double) monoA * monoA;
            bb_ += (double) monoB * monoB;
            kl_ += (double) kickFull * kickFull;                       // kick energy (as heard)
            ll_ += (double) (kickFull + monoIn) * (kickFull + monoIn); // kick + bass as it came in
            hh_ += (double) monoHigh * monoHigh;
            kh_ += (double) kickFull * monoHigh;
            binKick_ += kickFull;
            binLow_ += monoLow;
            binA_ += monoA;
            binB_ += monoB;
            if (++shapeCounter_ >= shapeSamplesPerBin_ && shapeBin_ < kShapeBins)
            {
                const float inv = 1.0f / (float) shapeCounter_;
                capKick_[(size_t) shapeBin_] = binKick_ * inv; // the kick as it sounds
                capLow_[(size_t) shapeBin_] = binLow_ * inv;
                capA_[(size_t) shapeBin_] = binA_ * inv;
                capB_[(size_t) shapeBin_] = binB_ * inv;
                ++shapeBin_;
                shapeCounter_ = 0;
                binKick_ = binLow_ = binA_ = binB_ = 0;
            }
            if (++hitSamples_ >= windowSamples_) finishHit();
        }

        mix_ += (mixTarget_ - mix_) * mixStep;
        if (bypass) continue;
        // Transparent until it helps: the untouched bass, or (fading in) the rotated low band with the matched high band.
        const float procL = rotL + highL, procR = rotR + highR;
        float outL = (inL + mix_ * (procL - inL)) * output, outR = (inR + mix_ * (procR - inR)) * output;
        if (listen == ListenOriginalSum) { outL = inL; outR = inR; }
        if (listen != ListenBass && kickL != nullptr)
        {
            outL += kickL[i] * sidechainGain_;
            outR += kickR[i] * sidechainGain_;
        }
        left[i] = outL;
        if (right != left) right[i] = outR;
    }
    angleShown_.store(angle_, std::memory_order_relaxed);
    target_.store(target, std::memory_order_relaxed);
    mixShown_.store(mix_, std::memory_order_relaxed);
}

PhaseAlign::HitRecord PhaseAlign::history(int newestFirst) const
{
    const uint32_t count = hits_.load(std::memory_order_acquire);
    if (newestFirst < 0 || newestFirst >= kHistory || (uint32_t) newestFirst >= count) return {};
    const int slot = ((historyNext_.load(std::memory_order_relaxed) - 1 - newestFirst) % kHistory + kHistory) % kHistory;
    return {histAngle_[(size_t) slot].load(std::memory_order_relaxed), histCoherence_[(size_t) slot].load(std::memory_order_relaxed),
            histGain_[(size_t) slot].load(std::memory_order_relaxed), histPotential_[(size_t) slot].load(std::memory_order_relaxed),
            histBass_[(size_t) slot].load(std::memory_order_relaxed), histUsed_[(size_t) slot].load(std::memory_order_relaxed)};
}

bool PhaseAlign::takeShape(Shape& out, uint32_t serial) const
{
    const uint32_t now = shapeSerial_.load(std::memory_order_acquire);
    if (now == serial) return false;
    out.bins = shapeBins_.load(std::memory_order_relaxed);
    for (int b = 0; b < out.bins; ++b)
    {
        out.kick[(size_t) b] = shapeKick_[(size_t) b].load(std::memory_order_relaxed);
        out.before[(size_t) b] = shapeBefore_[(size_t) b].load(std::memory_order_relaxed);
        out.after[(size_t) b] = shapeAfter_[(size_t) b].load(std::memory_order_relaxed);
    }
    out.serial = now;
    return true;
}

std::vector<DeviceProcessor::UiGroup> PhaseAlign::uiGroups() const
{
    return {{"Detect", {Threshold, Window, MinCoherence}, 2}, {"Align", {ModeParam, Angle, Speed, Crossover}, 2},
            {"Out", {Listen, Output}, 1}};
}
} // namespace sonora::devices
