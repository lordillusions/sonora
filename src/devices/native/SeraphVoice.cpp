#include "devices/native/SeraphVoice.h"

#include <algorithm>
#include <cmath>

namespace sonora::devices::seraph
{
namespace
{
constexpr float kPi = 3.14159265358979f;
constexpr float kBreathScale = 3.0f;   // noise before the resonators, relative to the pulse (calibrated by the breath test)
constexpr float kJitterAtFull = 0.0080f;  // period SD at Human 100 % (local jitter about 0.9 %)
constexpr float kShimmerAtFull = 0.028f;  // amplitude SD at Human 100 % (local shimmer about 3 %)
constexpr float kStealMs = 6.0f;

float smoothstep(float x)
{
    x = std::clamp(x, 0.0f, 1.0f);
    return x * x * (3.0f - 2.0f * x);
}

// tan(x) for 0 <= x <= ~1 (formants are clamped below 0.3 fs, x <= 0.95).
float fastTan(float x)
{
    const float x2 = x * x;
    return x * (1.0f + x2 * (0.33333333f + x2 * (0.13333333f + x2 * (0.05396825f + x2 * 0.02186949f))));
}

uint32_t step(uint32_t& s)
{
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}
} // namespace

float SeraphVoice::SmoothNoise::next(float increment, uint32_t& rng)
{
    phase += increment;
    if (phase >= 1.0f)
    {
        phase -= std::floor(phase);
        a = b;
        b = (float) (step(rng) >> 8) * (2.0f / 16777216.0f) - 1.0f;
    }
    return a + (b - a) * smoothstep(phase);
}

float SeraphVoice::uniform() { return (float) (step(rng_) >> 8) * (2.0f / 16777216.0f) - 1.0f; }

float SeraphVoice::gauss()
{
    // Sum of four uniforms (variance 4/3), scaled to unit variance: close enough to normal and bounded (|x| < 3.5).
    return (uniform() + uniform() + uniform() + uniform()) * 0.8660254f;
}

void SeraphVoice::reset()
{
    stage_ = Stage::Idle;
    hasPending_ = false;
    envelope_ = envelopeTarget_ = 0;
    for (int f = 0; f < kFormants; ++f) ic1_[f] = ic2_[f] = 0;
    hp1_ = hp2_ = hpIn1_ = hpIn2_ = 0;
    std::fill(ring_.begin(), ring_.end(), 0.0f);
}

void SeraphVoice::start(const StartInfo& info)
{
    if (stage_ == Stage::Idle || stage_ == Stage::Delay || envelope_ < 1.0e-5f)
    {
        begin(info);
        return;
    }
    pending_ = info;
    hasPending_ = true;
    kill();
}

void SeraphVoice::begin(const StartInfo& info)
{
    info_ = info;
    hasPending_ = false;
    rng_ = info.traits.seed != 0 ? info.traits.seed : 0x9E3779B9u;
    for (int i = 0; i < 4; ++i) step(rng_);
    delaySamples_ = std::max(0, (int) std::lround(info.traits.delayMs * 0.001 * sampleRate_));
    stage_ = delaySamples_ > 0 ? Stage::Delay : Stage::Sounding;
    age_ = onsetAge_ = 0;
    countdown_ = 0;
    onset_ = info.onset;
    vowelStep_ = vowelStepFrom_ = info.vowelStep;
    phase_ = 0.5 * (1.0 + (double) uniform());
    increment_ = 0;
    periodScale_ = 1;
    shimmer_ = 1;
    gliding_ = false;
    vibratoPhase_ = info.traits.vibPhase;
    for (auto* n : {&driftSlow_, &driftFast_, &motion_})
    {
        n->a = uniform();
        n->b = uniform();
        n->phase = 0.5f * (1.0f + uniform());
    }
    attack_ = 0;
    releaseGain_ = 1;
    fade_ = 1;
    envelope_ = envelopeTarget_ = 0;
    envelopeStep_ = 0;
    for (int f = 0; f < kFormants; ++f) ic1_[f] = ic2_[f] = 0;
    hp1_ = hp2_ = hpIn1_ = hpIn2_ = 0;
    std::fill(ring_.begin(), ring_.end(), 0.0f);
    outCount_ = 0;
    nextGrain_ = 0;
    inSustain_ = false;
    walkDirection_ = 1;
    walk_.a = uniform();
    walk_.b = uniform();
    walk_.phase = 0;
    if (info.real != nullptr && ! info.real->marks.empty())
        srcPos_ = info.real->marks.front();
    const float angle = (std::clamp(info.traits.pan, -1.0f, 1.0f) + 1.0f) * 0.25f * kPi;
    panLeft_ = std::cos(angle);
    panRight_ = std::sin(angle);
}

void SeraphVoice::glideTo(float pitch, float glideMs)
{
    if (stage_ == Stage::Idle) return;
    const float now = gliding_ ? info_.pitch + (glideFrom_ - info_.pitch) * (1.0f - smoothstep(glideAge_ / std::max(1.0f, glideMs_)))
                               : info_.pitch;
    if (std::abs(pitch - info_.pitch) < 1.0e-4f) return;
    info_.pitch = pitch;
    if (glideMs < 1.0f || stage_ == Stage::Delay)
    {
        gliding_ = false;
        return;
    }
    glideFrom_ = now;
    glideMs_ = glideMs;
    glideAge_ = 0;
    gliding_ = true;
}

void SeraphVoice::rearticulate(int onset, float vowelStep)
{
    if (stage_ != Stage::Sounding && stage_ != Stage::Delay) return;
    const float t = std::clamp((float) onsetAge_ * 1000.0f / (float) sampleRate_ / 80.0f, 0.0f, 1.0f);
    vowelStepFrom_ = vowelStepFrom_ + (vowelStep_ - vowelStepFrom_) * smoothstep(t);
    vowelStep_ = vowelStep;
    onset_ = onset;
    onsetAge_ = 0;
}

void SeraphVoice::release()
{
    if (stage_ == Stage::Delay) { stage_ = Stage::Idle; return; }
    if (stage_ == Stage::Sounding) stage_ = Stage::Releasing;
}

void SeraphVoice::kill()
{
    if (stage_ == Stage::Idle) return;
    if (stage_ == Stage::Delay)
    {
        if (hasPending_) begin(pending_);
        else stage_ = Stage::Idle;
        return;
    }
    stage_ = Stage::Fading;
}

void SeraphVoice::control(const Shared& s)
{
    const float sr = (float) s.sampleRate;
    const float dt = (float) kControlInterval / sr;
    const float ms = (float) age_ * 1000.0f / sr;
    const float onsetMs = (float) onsetAge_ * 1000.0f / sr;
    const auto& tr = info_.traits;

    // ---- pitch
    float cents = tr.tuneCents;
    const float slow = driftSlow_.next(0.3f * (0.8f + 0.4f * tr.vibPhase) * dt, rng_);
    const float fast = driftFast_.next(1.5f * (0.8f + 0.4f * tr.onsetMs) * dt, rng_);
    cents += s.driftCents * (0.7f * slow + 0.3f * fast);
    const float fadeIn = smoothstep((ms - s.vibratoDelayMs) / 300.0f);
    const float depth = (s.vibratoCents * tr.vibDepth * (1.0f + 0.12f * s.human * fast) + s.modVibratoCents) * fadeIn;
    cents += depth * std::sin(2.0f * kPi * vibratoPhase_);
    vibratoPhase_ += s.vibratoRate * tr.vibRate * (1.0f + 0.03f * s.human * slow) * dt;
    vibratoPhase_ -= std::floor(vibratoPhase_);
    if (s.scoopCents > 0.0f)
    {
        const float x = ms / std::max(10.0f, tr.scoopMs);
        if (x < 1.0f) cents -= s.scoopCents * (1.0f - smoothstep(x));
        else if (x < 2.0f) cents += 5.0f * std::min(1.0f, s.scoopCents / 20.0f) * std::sin(kPi * (x - 1.0f));
    }
    float semis = info_.pitch;
    if (gliding_)
    {
        glideAge_ += dt * 1000.0f;
        const float u = glideAge_ / std::max(1.0f, glideMs_);
        if (u >= 1.0f) gliding_ = false;
        else semis += (glideFrom_ - info_.pitch) * (1.0f - smoothstep(u));
    }
    semis += s.bendSemitones;
    const float hz = 440.0f * std::exp2((semis - 69.0f) / 12.0f + cents / 1200.0f);
    increment_ = (double) std::min(hz, sr * 0.45f) / s.sampleRate;

    // ---- the singer's state
    const float velocity = std::clamp(info_.velocity, 0.01f, 1.0f);
    const float swelling = s.swell > 0.0f ? s.swell * (1.0f - smoothstep(ms / 2000.0f)) : 0.0f;
    if (info_.real != nullptr)
    {
        // Real: the recording is the source and the throat; Size and the throat resample its grains.
        realScale_ = std::clamp(s.size * tr.throat * (info_.boys ? kBoysFormantScale : 1.0f), 0.6f, 1.5f);
        realGain_ = kLevel / std::max(1.0e-4f, info_.real->level);
        attack_ += dt * 1000.0f / std::max(5.0f, s.attackMs * tr.attack);
        if (stage_ == Stage::Releasing) releaseGain_ *= std::exp(-6.9077553f * dt * 1000.0f / std::max(20.0f, s.releaseMs));
        if (stage_ == Stage::Fading) fade_ = std::max(0.0f, fade_ - dt * 1000.0f / kStealMs);
        float onsetLevel = 1.0f;
        if (onset_ == OnsetH || onset_ == OnsetM || onset_ == OnsetN || onset_ == OnsetL) // a softer, later start
            onsetLevel = smoothstep(onsetMs / 70.0f);
        shown_ = smoothstep(attack_) * releaseGain_ * fade_ * std::pow(velocity, 1.5f * s.dynamics) * info_.gain;
        envelopeTarget_ = shown_ * onsetLevel * (1.0f - 0.5f * swelling);
        return;
    }
    const float tension = std::clamp(s.tension + s.effort * (velocity - 0.6f) * 0.8f - 0.25f * swelling, 0.0f, 1.0f);
    const float shape = (1.0f - tension) * (float) (kPulseShapes - 1);
    const int shapeIndex = std::min((int) shape, kPulseShapes - 2);
    shapeMix_ = shape - (float) shapeIndex;
    const int octave = GlottalTables::octaveFor(hz * 1.03f);
    tableA_ = s.tables->table(shapeIndex, octave);
    tableB_ = s.tables->table(shapeIndex + 1, octave);
    openQuotient_ = openQuotient(shape);

    // ---- vowel
    const float stepNow = vowelStepFrom_ + (vowelStep_ - vowelStepFrom_) * smoothstep(onsetMs / 80.0f);
    float vowel = s.vowel + stepNow;
    if (vowel > (float) (kVowels - 1) + 0.001f) vowel = s.vowel - stepNow; // steps mirror inside the A..U line
    vowel += tr.vowelOffset;
    if (s.motion > 0.0f) vowel += 1.2f * s.motion * motion_.next(s.motionRate * tr.motionRate * dt, rng_);
    vowel = std::clamp(vowel, 0.0f, (float) (kVowels - 1));
    if (info_.drone) vowel += (4.0f - vowel) * 0.7f;

    // ---- onset: how the syllable begins
    FormantShape fs;
    float hum = info_.drone ? std::max(s.hum, 0.5f) : s.hum;
    float onsetLevel = 1.0f, onsetNoise = 0.0f;
    switch (onset_)
    {
        case OnsetH:
        {
            const float breathMs = 40.0f + 40.0f * tr.onsetMs;
            onsetLevel = onsetMs < breathMs ? 0.0f : smoothstep((onsetMs - breathMs) / 20.0f);
            onsetNoise = onsetMs < breathMs ? 1.0f : 1.0f - smoothstep((onsetMs - breathMs) / 30.0f);
            break;
        }
        case OnsetM:
        case OnsetN:
        {
            const float humMs = 60.0f + 30.0f * tr.onsetMs;
            const float closed = onsetMs < humMs ? 1.0f : 1.0f - smoothstep((onsetMs - humMs) / 50.0f);
            hum = std::max(hum, closed);
            fs.f2Target = onset_ == OnsetM ? 900.0f : 1500.0f;
            fs.f2Amount = closed;
            onsetLevel = 1.0f - 0.45f * closed;
            break;
        }
        case OnsetL:
        {
            const float dip = onsetMs < 40.0f ? 1.0f : 1.0f - smoothstep((onsetMs - 40.0f) / 50.0f);
            fs.f3Scale = 1.0f - 0.2f * dip;
            hum = std::max(hum, 0.35f * dip);
            onsetLevel = 1.0f - 0.35f * dip;
            break;
        }
        case OnsetY:
        case OnsetW:
        {
            const float from = onset_ == OnsetY ? 2.0f : 4.0f;
            vowel = from + (vowel - from) * smoothstep(onsetMs / 90.0f);
            break;
        }
        default: break;
    }

    // ---- formants
    fs.vowel = vowel;
    fs.hum = hum;
    fs.scale = s.size * tr.throat * (info_.boys ? kBoysFormantScale : 1.0f);
    fs.brightDb = s.brightDb + tr.brightDb + s.effort * (velocity - 0.6f) * 8.0f - 6.0f * swelling;
    fs.f0 = hz;
    const auto f = formants(info_.boys ? Soprano : info_.type, fs);
    for (int i = 0; i < kFormants; ++i)
    {
        const float centre = std::min(f.hz[i], 0.3f * sr);
        const float g = fastTan(kPi * centre / sr);
        const float k = std::clamp(f.bw[i] / centre, 0.005f, 2.0f);
        a1_[i] = 1.0f / (1.0f + g * (g + k));
        a2_[i] = g * a1_[i];
        a3_[i] = g * a2_[i];
        out_[i] = (i % 2 == 0 ? 1.0f : -1.0f) * f.gain[i] * k;
    }
    // Loudness from the note's own pitch (vibrato and drift keep the level changes they cause through the formants, as a
    // real voice does) over the throat scale: formants scaled by s at f0 put every harmonic where f0 / s puts it at scale 1.
    const float noteHz = 440.0f * std::exp2((semis - s.bendSemitones - 69.0f) / 12.0f);
    const float loudness = loudnessGain(info_.boys ? kVoiceTypes : info_.type, vowel, hum, shape, noteHz / (s.size * tr.throat)) * kLevel;

    // ---- breath
    const float breath = std::clamp(s.breath + (1.0f - tension) * 0.15f, 0.0f, 1.2f);
    noiseGain_ = (breath * breath * tr.breath + onsetNoise * 0.8f) * kBreathScale;
    voiceGain_ = onsetLevel;

    // ---- level
    attack_ += dt * 1000.0f / std::max(5.0f, s.attackMs * tr.attack);
    if (stage_ == Stage::Releasing) releaseGain_ *= std::exp(-6.9077553f * dt * 1000.0f / std::max(20.0f, s.releaseMs));
    if (stage_ == Stage::Fading) fade_ = std::max(0.0f, fade_ - dt * 1000.0f / kStealMs);
    const float velocityGain = std::pow(velocity, 1.5f * s.dynamics);
    shown_ = smoothstep(attack_) * releaseGain_ * fade_ * velocityGain * info_.gain;
    envelopeTarget_ = shown_ * (1.0f - 0.5f * swelling) * loudness;
}

void SeraphVoice::emitGrain(const Shared& s)
{
    const auto& r = *info_.real;
    const double outPeriod = increment_ > 0 ? (double) periodScale_ / increment_ : sampleRate_ / 220.0;
    // the mark nearest the read position
    const auto it = std::lower_bound(r.marks.begin(), r.marks.end(), srcPos_);
    size_t k = (size_t) (it - r.marks.begin());
    if (k >= r.marks.size()) k = r.marks.size() - 1;
    else if (k > 0 && srcPos_ - r.marks[k - 1] < r.marks[k] - srcPos_) --k;
    const double srcPeriod = r.periods[k];
    const double ratio = r.sampleRate / sampleRate_ * (double) realScale_; // source samples per output sample
    const float* grain = r.grainData.data() + r.grainStart[k];
    const int grainHalf = r.grainHalf[k];
    const int half = std::min(realDelay_ - 2, (int) (grainHalf / ratio));
    // a grain of two periods keeps the recording's level whatever the new period: gain sqrt(new / old period)
    const float gain = realGain_ * shimmer_ * (float) std::sqrt(outPeriod * r.sampleRate / sampleRate_ / srcPeriod);
    const double at = nextGrain_ + realDelay_;
    const int whole = (int) std::floor(at);
    const double frac = at - whole;
    for (int j = -half; j <= half; ++j)
    {
        const double t = ((double) j - frac) * ratio + grainHalf; // the grain's centre sits at `at` exactly
        if (t < 0 || t >= 2.0 * grainHalf) continue;
        const int i0 = (int) t;
        const float f = (float) (t - i0);
        const float sample = grain[i0] + (grain[i0 + 1] - grain[i0]) * f;
        ring_[(size_t) ((whole + j) & ringMask_)] += sample * gain;
    }
    nextGrain_ += outPeriod;
    // the read position: the attack once at the recording's own pace, then a slow walk inside the sustain
    const double step = outPeriod * r.sampleRate / sampleRate_;
    const double sustainBegin = r.marks[(size_t) r.sustainBegin], sustainEnd = r.marks[(size_t) r.sustainEnd - 1];
    if (! inSustain_)
    {
        srcPos_ += step;
        if (srcPos_ >= sustainBegin) inSustain_ = true;
    }
    else
    {
        const float speed = 0.3f + 0.35f * (1.0f + walk_.next((float) (0.35 * outPeriod / sampleRate_), rng_));
        if (uniform() > 1.0f - (float) (0.6 * outPeriod / sampleRate_)) walkDirection_ = -walkDirection_; // now and then, turn
        srcPos_ += walkDirection_ * speed * step;
        if (srcPos_ > sustainEnd) { srcPos_ = 2 * sustainEnd - srcPos_; walkDirection_ = -1; }
        if (srcPos_ < sustainBegin) { srcPos_ = 2 * sustainBegin - srcPos_; walkDirection_ = 1; }
        srcPos_ = std::clamp(srcPos_, sustainBegin, sustainEnd);
    }
    // the next period's jitter and shimmer (as at each glottal period of the synthesis)
    periodScale_ = std::clamp(1.0f + kJitterAtFull * s.human * gauss(), 0.95f, 1.05f);
    shimmer_ = std::clamp(1.0f + kShimmerAtFull * s.human * gauss(), 0.8f, 1.2f);
}

void SeraphVoice::render(float* left, float* right, int n, const Shared& s)
{
    sampleRate_ = s.sampleRate;
    if (s.tables == nullptr) return;
    int i = 0;
    while (i < n)
    {
        if (stage_ == Stage::Idle) return;
        if (stage_ == Stage::Delay)
        {
            const int wait = std::min(delaySamples_, n - i);
            delaySamples_ -= wait;
            i += wait;
            if (delaySamples_ > 0) return;
            stage_ = Stage::Sounding;
            countdown_ = 0;
            continue;
        }
        if (countdown_ <= 0)
        {
            // Finished fading (stolen) or releasing: stop, or start the note that waited for this singer.
            if ((stage_ == Stage::Fading && fade_ <= 0.0f) || (stage_ == Stage::Releasing && releaseGain_ < 1.0e-4f))
            {
                if (envelope_ <= 1.0e-6f)
                {
                    const bool pending = hasPending_;
                    reset();
                    if (pending) begin(pending_);
                    continue;
                }
            }
            control(s);
            countdown_ = kControlInterval;
            envelopeStep_ = (envelopeTarget_ - envelope_) / (float) kControlInterval;
        }
        const int len = std::min(countdown_, n - i);
        if (info_.real != nullptr && ! ring_.empty())
        {
            for (int j = 0; j < len; ++j)
            {
                while ((double) outCount_ >= nextGrain_) emitGrain(s);
                float& slot = ring_[(size_t) (outCount_ & ringMask_)];
                const float y = slot;
                slot = 0.0f;
                ++outCount_;
                envelope_ += envelopeStep_;
                left[i + j] += y * envelope_ * panLeft_;
                right[i + j] += y * envelope_ * panRight_;
            }
            countdown_ -= len;
            i += len;
            age_ += len;
            onsetAge_ += len;
            continue;
        }
        const float invOpen = 1.0f / openQuotient_;
        for (int j = 0; j < len; ++j)
        {
            // glottal pulse: two neighbouring shapes of the octave's table
            const float position = (float) phase_ * (float) kTableSize;
            const int index = std::min((int) position, kTableSize - 1);
            const float frac = position - (float) index;
            const float pa = tableA_[index] + (tableA_[index + 1] - tableA_[index]) * frac;
            const float pb = tableB_[index] + (tableB_[index + 1] - tableB_[index]) * frac;
            float source = (pa + (pb - pa) * shapeMix_) * shimmer_ * voiceGain_;
            // breath: high-passed noise, louder while the glottis is open
            const float white = (float) (step(rng_) >> 8) * (2.0f / 16777216.0f) - 1.0f;
            hp1_ = hpCoef_ * (hp1_ + white - hpIn1_);
            hpIn1_ = white;
            hp2_ = hpCoef_ * (hp2_ + hp1_ - hpIn2_);
            hpIn2_ = hp1_;
            const float x = (float) phase_ * invOpen;
            const float open = x < 1.0f ? 4.0f * x * (1.0f - x) : 0.0f;
            source += hp2_ * noiseGain_ * (0.3f + 0.7f * open);
            // five parallel resonators
            float y = 0;
            for (int f = 0; f < kFormants; ++f)
            {
                const float v3 = source - ic2_[f];
                const float v1 = a1_[f] * ic1_[f] + a2_[f] * v3;
                const float v2 = ic2_[f] + a2_[f] * ic1_[f] + a3_[f] * v3;
                ic1_[f] = 2.0f * v1 - ic1_[f];
                ic2_[f] = 2.0f * v2 - ic2_[f];
                y += out_[f] * v1;
            }
            envelope_ += envelopeStep_;
            const float out = y * envelope_;
            left[i + j] += out * panLeft_;
            right[i + j] += out * panRight_;
            // next sample; a new period draws its jitter and shimmer
            phase_ += increment_ / (double) periodScale_;
            if (phase_ >= 1.0)
            {
                phase_ -= 1.0;
                if (phase_ >= 1.0) phase_ -= std::floor(phase_);
                periodScale_ = std::clamp(1.0f + kJitterAtFull * s.human * gauss(), 0.95f, 1.05f);
                shimmer_ = std::clamp(1.0f + kShimmerAtFull * s.human * gauss(), 0.8f, 1.2f);
            }
        }
        countdown_ -= len;
        i += len;
        age_ += len;
        onsetAge_ += len;
    }
}
} // namespace sonora::devices::seraph
