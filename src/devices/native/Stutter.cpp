#include "devices/native/Stutter.h"

#include <algorithm>
#include <cmath>

namespace sonora::devices
{
namespace
{
constexpr float kPi = 3.14159265f;

std::vector<ParameterInfo> stutterParameters()
{
    using S = Stutter;
    std::vector<std::string> modes(S::kModeNames, S::kModeNames + S::kModes);
    std::vector<std::string> captures(S::kCaptureNames, S::kCaptureNames + S::kCaptures);
    std::vector<ParameterInfo> p(S::NumParams);
    p[S::ModeParam] = makeParam("Mode", "Mode", 0, S::kModes - 1, 0, "", false, modes);
    p[S::Motion] = makeParam("Motion", "Motion", 0, 1, 0.35f, "%");
    p[S::Macro] = makeParam("Macro", "Macro", 0, 1, 1, "", true);
    p[S::Hold] = makeParam("Hold", "Hold", 0, 1, 0, "", true);
    p[S::Capture] = makeParam("Capture", "Capture", 0, S::kCaptures - 1, 1, "", false, captures);
    p[S::Sync] = makeParam("Sync", "Sync", 0, 1, 0, "", true);
    p[S::Grain] = makeParam("Grain", "Grain", 10, 1000, 120, "ms");
    p[S::Shape] = makeParam("Shape", "Shape", 0, 1, 0.6f, "%");
    p[S::Every] = makeParam("Every", "Every", 8, 1000, 60, "ms");
    p[S::Speed] = makeParam("Speed", "Speed", -4, 4, 0.25f, "x");
    p[S::Pitch] = makeParam("Pitch", "Pitch", -24, 24, 0, "st");
    p[S::Width] = makeParam("Width", "Width", 0, 1, 0.3f, "%");
    p[S::Jitter] = makeParam("Jitter", "Jitter", 0, 1, 0.1f, "%");
    p[S::Tone] = makeParam("Tone", "Tone", -1, 1, 0);
    p[S::Space] = makeParam("Space", "Space", 0, 1, 0.15f, "%");
    p[S::Mix] = makeParam("Mix", "Mix", 0, 1, 1, "%");
    p[S::Output] = makeParam("Output", "Level", -24, 12, 0, "dB");
    return p;
}

float logLerp(float a, float b, float t) { return a * std::pow(b / a, t); }
float lerp(float a, float b, float t) { return a + (b - a) * t; }

constexpr int kNotes = 11;
constexpr double kNoteBeats[kNotes] = {1.0 / 16, 1.0 / 12, 1.0 / 8, 1.0 / 6, 1.0 / 4, 1.0 / 3, 1.0 / 2, 2.0 / 3, 1.0, 2.0, 4.0};
constexpr const char* kNoteNames[kNotes] = {"1/64", "1/32T", "1/32", "1/16T", "1/16", "1/8T", "1/8", "1/4T", "1/4", "1/2", "1 Bar"};
} // namespace

Stutter::Stutter() : NativeDevice(stutterParameters()) {}

double Stutter::snapToNote(double ms, double tempo)
{
    tempo = std::clamp(tempo, 20.0, 999.0);
    const double beats = std::max(1.0e-4, ms * 0.001 * tempo / 60.0);
    double best = kNoteBeats[0], distance = 1e9;
    for (double note : kNoteBeats)
        if (const double d = std::abs(std::log(beats / note)); d < distance) { distance = d; best = note; }
    return best;
}

const char* Stutter::noteName(double beats)
{
    for (int i = 0; i < kNotes; ++i)
        if (std::abs(kNoteBeats[i] - beats) < 1e-6) return kNoteNames[i];
    return "";
}

Stutter::Settings Stutter::macroSettings(int mode, float m, double tempo, bool sync)
{
    m = std::clamp(m, 0.0f, 1.0f);
    Settings s;
    switch (mode)
    {
        case Swell:
            s.grainMs = logLerp(450, 55, m); s.everyMs = logLerp(120, 28, m); s.shape = lerp(1.0f, 0.55f, m);
            s.speed = logLerp(0.03f, 1.0f, m); s.width = lerp(0.25f, 0.15f, m); s.jitter = lerp(0.12f, 0.03f, m);
            s.space = lerp(0.35f, 0.08f, m);
            break;
        case Rewind:
            s.grainMs = logLerp(320, 45, m); s.everyMs = logLerp(110, 22, m); s.shape = lerp(0.9f, 0.35f, m);
            s.speed = -logLerp(0.08f, 2.2f, m); s.width = 0.2f; s.jitter = 0.05f; s.space = lerp(0.25f, 0.05f, m);
            s.reverseGrains = true;
            break;
        case Shatter:
            s.grainMs = logLerp(140, 18, m); s.everyMs = logLerp(190, 24, m); s.shape = lerp(0.35f, 0.08f, m);
            s.speed = logLerp(0.25f, 1.0f, m); s.width = lerp(0.7f, 1.0f, m); s.jitter = lerp(0.9f, 0.55f, m);
            s.scatter = lerp(0.45f, 0.25f, m); s.space = lerp(0.15f, 0.0f, m);
            break;
        case Freeze:
            s.grainMs = logLerp(600, 45, m); s.everyMs = logLerp(130, 95, m); s.shape = lerp(1.0f, 0.12f, m);
            s.speed = 0; s.width = lerp(0.45f, 0.6f, m); s.jitter = lerp(0.2f, 0.0f, m); s.space = lerp(0.55f, 0.0f, m);
            break;
        case Spread:
            s.grainMs = logLerp(700, 40, m); s.everyMs = logLerp(140, 20, m); s.shape = lerp(1.0f, 0.4f, m);
            s.speed = logLerp(0.06f, 1.0f, m); s.width = 1.0f; s.jitter = lerp(0.55f, 0.08f, m);
            s.space = lerp(0.4f, 0.05f, m); s.detune = lerp(0.18f, 0.1f, m);
            break;
        case Tunnel:
            s.grainMs = logLerp(380, 50, m); s.everyMs = logLerp(110, 25, m); s.shape = lerp(0.9f, 0.4f, m);
            s.speed = logLerp(0.12f, 1.0f, m); s.width = 0.3f; s.jitter = 0.06f; s.space = lerp(0.4f, 0.1f, m);
            s.tunnelMs = logLerp(32.0f, 3.2f, m); s.tunnelFeedback = lerp(0.84f, 0.72f, m); s.toneShift = lerp(-0.45f, 0.0f, m);
            break;
        case Retrigger:
        default:
        {
            static constexpr double kRolls[7] = {1.0, 0.5, 1.0 / 3, 0.25, 1.0 / 6, 0.125, 1.0 / 16};
            s.shape = 0.08f; s.speed = 0; s.width = 0; s.jitter = 0; s.space = 0; s.fixedSlice = true;
            if (sync)
            {
                s.everyBeats = kRolls[std::clamp((int) std::lround(m * 6.0f), 0, 6)];
                s.everyMs = (float) (s.everyBeats * 60000.0 / std::clamp(tempo, 20.0, 999.0));
            }
            else s.everyMs = logLerp(250, 15, m);
            s.grainMs = s.everyMs;
            return s;
        }
    }
    if (sync)
    {
        const float ratio = s.grainMs / s.everyMs;
        s.everyBeats = snapToNote(s.everyMs, tempo);
        s.everyMs = (float) (s.everyBeats * 60000.0 / std::clamp(tempo, 20.0, 999.0));
        s.grainMs = std::clamp(s.everyMs * ratio, 10.0f, 2000.0f);
    }
    return s;
}

Stutter::Settings Stutter::settings() const
{
    const int mode = engagedShown_.load(std::memory_order_relaxed) ? modeShown_.load(std::memory_order_relaxed)
                                                                   : std::clamp((int) std::lround(value(ModeParam)), 0, kModes - 1);
    const double tempo = host_.tempo > 0 ? host_.tempo : 120.0;
    const bool sync = value(Sync) > 0.5f;
    Settings s = macroSettings(mode, value(Motion), tempo, sync);
    if (value(Macro) < 0.5f && mode != Retrigger)
    {
        // The knobs play; the mode keeps its character (reverse, scatter, tube, detune).
        s.grainMs = value(Grain); s.shape = value(Shape); s.everyMs = value(Every); s.speed = value(Speed);
        s.width = value(Width); s.jitter = value(Jitter); s.space = value(Space);
        s.everyBeats = 0;
        if (sync)
        {
            s.everyBeats = snapToNote(s.everyMs, tempo);
            s.everyMs = (float) (s.everyBeats * 60000.0 / std::clamp(tempo, 20.0, 999.0));
        }
    }
    else if (value(Macro) < 0.5f) // Retrigger by hand: Every sets the roll, Shape the edges, Width the ping-pong
    {
        s.everyMs = s.grainMs = value(Every);
        s.shape = value(Shape); s.width = value(Width); s.space = value(Space);
        s.everyBeats = 0;
        if (sync)
        {
            s.everyBeats = snapToNote(s.everyMs, tempo);
            s.everyMs = s.grainMs = (float) (s.everyBeats * 60000.0 / std::clamp(tempo, 20.0, 999.0));
        }
    }
    s.pitch += value(Pitch);
    return s;
}

void Stutter::detachMacro()
{
    if (value(Macro) < 0.5f) return;
    const auto s = settings();
    setParameter(Grain, std::clamp(s.grainMs, 10.0f, 1000.0f));
    setParameter(Shape, s.shape);
    setParameter(Every, std::clamp(s.everyMs, 8.0f, 1000.0f));
    setParameter(Speed, s.speed);
    setParameter(Width, s.width);
    setParameter(Jitter, s.jitter);
    setParameter(Space, s.space);
    setParameter(Macro, 0.0f);
}

// ---------------------------------------------------------------------------------------------------------------------
void Stutter::Room::prepare(double sampleRate)
{
    static constexpr double kMs[4] = {31.7, 37.9, 43.3, 49.1};
    for (size_t i = 0; i < 4; ++i)
    {
        length[i] = std::max(8, (int) (kMs[i] * 0.001 * sampleRate));
        lines[i].assign((size_t) length[i], 0.0f);
    }
    clear();
}

void Stutter::Room::clear()
{
    for (auto& l : lines) std::fill(l.begin(), l.end(), 0.0f);
    index.fill(0);
    damp.fill(0.0f);
}

void Stutter::Room::process(float inL, float inR, float& outL, float& outR)
{
    float d[4];
    for (size_t i = 0; i < 4; ++i)
    {
        const float y = lines[i][(size_t) index[i]];
        damp[i] += 0.42f * (y - damp[i]);
        d[i] = damp[i];
    }
    const float sum = (d[0] + d[1] + d[2] + d[3]) * 0.5f;
    const float in[4] = {inL, inR, inL, inR};
    constexpr float kFeedback = 0.8f;
    for (size_t i = 0; i < 4; ++i)
    {
        lines[i][(size_t) index[i]] = in[i] * 0.5f + kFeedback * (d[i] - sum);
        if (++index[i] >= length[i]) index[i] = 0;
    }
    outL = (d[0] + d[2] * 0.7f) * 0.7f;
    outR = (d[1] + d[3] * 0.7f) * 0.7f;
}

void Stutter::Tube::prepare(double sampleRate)
{
    int size = 1;
    while (size < (int) (0.07 * sampleRate) + 4) size <<= 1;
    for (auto& l : line) l.assign((size_t) size, 0.0f);
    clear();
}

void Stutter::Tube::clear()
{
    for (auto& l : line) std::fill(l.begin(), l.end(), 0.0f);
    index = 0;
    damp[0] = damp[1] = 0;
}

void Stutter::Tube::process(float& l, float& r, float delaySamples, float feedback)
{
    const int mask = (int) line[0].size() - 1;
    float* io[2] = {&l, &r};
    for (int ch = 0; ch < 2; ++ch)
    {
        const float delay = std::clamp(delaySamples * (ch == 0 ? 1.0f : 1.07f), 2.0f, (float) mask - 2.0f);
        const float at = (float) index - delay;
        const int i0 = (int) std::floor(at);
        const float frac = at - (float) i0;
        const float a = line[ch][(size_t) (i0 & mask)], b = line[ch][(size_t) ((i0 + 1) & mask)];
        const float y = a + (b - a) * frac;
        damp[ch] += 0.55f * (y - damp[ch]);
        line[ch][(size_t) index] = *io[ch] + feedback * damp[ch];
        *io[ch] = (*io[ch] + y) * 0.55f;
    }
    index = (index + 1) & mask;
}

// ---------------------------------------------------------------------------------------------------------------------
void Stutter::prepare(double sampleRate, int)
{
    sampleRate_ = sampleRate > 0 ? sampleRate : 44100.0;
    int size = 1;
    while (size < (int) (kHistorySeconds * sampleRate_)) size <<= 1;
    for (auto& h : history_) h.assign((size_t) size, 0.0f);
    historyMask_ = size - 1;
    room_.prepare(sampleRate_);
    tube_.prepare(sampleRate_);
    inBinSamples_ = std::max(1, (int) (kInputSeconds * sampleRate_ / kBins));
    outBinSamples_ = std::max(1, (int) (kOutputSeconds * sampleRate_ / kBins));
    reset();
}

void Stutter::reset()
{
    for (auto& h : history_) std::fill(h.begin(), h.end(), 0.0f);
    written_ = 0;
    for (auto& g : grains_) g = {};
    engaged_ = false;
    engage_ = 0;
    head_ = 0;
    untilGrain_ = 0;
    lastGridIndex_ = -1;
    for (auto& ch : toneState_) ch[0] = ch[1] = 0;
    room_.clear();
    tube_.clear();
    midiNote_ = midiMode_ = -1;
    midiFresh_ = false;
    lastPressed_ = -1;
    inCount_ = outCount_ = 0;
    inLo_ = inHi_ = outLo_ = outHi_ = 0;
    inWrite_.store(0);
    outWrite_.store(0);
    inFilled_.store(0);
    outFilled_.store(0);
    engagedShown_.store(false);
    level_.store(0);
}

float Stutter::random01()
{
    noise_ = noise_ * 1664525u + 1013904223u;
    return (float) (noise_ >> 8) * (1.0f / 16777216.0f);
}

void Stutter::readRecording(double index, float& l, float& r) const
{
    const double newest = (double) (written_ - 1), oldest = (double) (written_ - historyMask_);
    index = std::clamp(index, std::max(0.0, oldest), std::max(0.0, newest));
    const auto i0 = (int64_t) std::floor(index);
    const float frac = (float) (index - (double) i0);
    const int64_t i1 = std::min<int64_t>(i0 + 1, written_ - 1);
    const auto a = (size_t) (i0 & historyMask_), b = (size_t) (std::max<int64_t>(i1, 0) & historyMask_);
    l = history_[0][a] + (history_[0][b] - history_[0][a]) * frac;
    r = history_[1][a] + (history_[1][b] - history_[1][a]) * frac;
}

int64_t Stutter::captureSamples() const
{
    const double beats = kCaptureBeats[std::clamp((int) std::lround(value(Capture)), 0, kCaptures - 1)];
    if (beats <= 0) return historyMask_;
    return (int64_t) (beats * 60.0 / std::clamp(host_.tempo, 20.0, 999.0) * sampleRate_);
}

void Stutter::engage(int64_t at)
{
    engaged_ = true;
    anchor_ = at;
    windowStart_ = at - captureSamples();
    untilGrain_ = 0; // the first grain starts at once
    lastGridIndex_ = -1;
    grainParity_ = 0;
    const auto s = settings();
    const double rate = std::pow(2.0, (double) s.pitch / 12.0);
    const double span = std::max(32.0, s.grainMs * 0.001 * sampleRate_) * rate;
    head_ = (double) at - span;
}

void Stutter::spawnGrain(const Settings& s, int64_t now)
{
    int slot = -1;
    float oldest = -1;
    for (int i = 0; i < kMaxGrains; ++i)
    {
        if (! grains_[(size_t) i].active) { slot = i; break; }
        const float age = (float) grains_[(size_t) i].pos / (float) std::max(1, grains_[(size_t) i].length);
        if (age > oldest) { oldest = age; slot = i; }
    }
    auto& v = grains_[(size_t) slot];
    const int length = std::max(32, (int) (s.grainMs * 0.001 * sampleRate_));
    const double everySamples = std::max(16.0, (double) s.everyMs * 0.001 * sampleRate_);
    float pitch = s.pitch;
    const bool odd = (grainParity_++ & 1) != 0;
    if (s.detune > 0) pitch += odd ? -s.detune : s.detune;
    bool reversed = s.reverseGrains;
    if (s.scatter > 0)
    {
        if (random01() < s.scatter * 0.5f) pitch += random01() < 0.5f ? -12.0f : 12.0f;
        if (random01() < s.scatter * 0.6f) reversed = ! reversed;
    }
    const double rate = std::clamp(std::pow(2.0, (double) pitch / 12.0), 0.125, 4.0);
    const double span = (double) length * rate;
    const double oldestValid = (double) (written_ - historyMask_ + 8);
    const double lo = std::max({(double) windowStart_, oldestValid, 0.0});
    const double hi = (double) now - 2.0 - span; // a grain never reads what is not recorded yet
    double left = head_;
    if (s.jitter > 0)
    {
        const double range = std::min(std::max(0.0, hi - lo), 0.75 * sampleRate_) * (double) s.jitter;
        left -= random01() * range;
    }
    left = std::clamp(left, std::min(lo, hi), std::max(lo, hi));
    v.active = true;
    v.pos = 0;
    v.length = length;
    v.fade = std::clamp((int) (s.shape * (float) length * 0.5f), std::max(1, (int) (0.002 * sampleRate_)), std::max(1, length / 2));
    v.step = reversed ? -rate : rate;
    v.read = reversed ? left + span - 1.0 : left;
    const float overlap = (float) ((double) length / everySamples);
    v.amp = 1.0f / std::pow(std::max(1.0f, overlap), 0.5f);
    const float pan = s.width * (odd ? -1.0f : 1.0f);
    v.gainL = pan > 0 ? 1.0f - pan : 1.0f;
    v.gainR = pan < 0 ? 1.0f + pan : 1.0f;

    auto& log = grainLog_[(size_t) (grainLogNext_.load(std::memory_order_relaxed) % kGrainLog)];
    log.at.store(now, std::memory_order_relaxed);
    log.read.store((int64_t) left, std::memory_order_relaxed);
    log.length.store(length, std::memory_order_relaxed);
    log.pan.store(pan, std::memory_order_relaxed);
    log.reversed.store(reversed, std::memory_order_relaxed);
    grainLogNext_.fetch_add(1, std::memory_order_release);
}

void Stutter::publishBins(float inL, float inR, float outL, float outR)
{
    const float in = 0.5f * (inL + inR), out = 0.5f * (outL + outR);
    if (inCount_ == 0) inLo_ = inHi_ = in;
    inLo_ = std::min(inLo_, in);
    inHi_ = std::max(inHi_, in);
    if (++inCount_ >= inBinSamples_)
    {
        const int w = inWrite_.load(std::memory_order_relaxed);
        inMin_[(size_t) w].store(inLo_, std::memory_order_relaxed);
        inMax_[(size_t) w].store(inHi_, std::memory_order_relaxed);
        inWrite_.store((w + 1) % kBins, std::memory_order_relaxed);
        inFilled_.store(std::min(kBins, inFilled_.load(std::memory_order_relaxed) + 1), std::memory_order_relaxed);
        inNewest_.store(written_, std::memory_order_release);
        inCount_ = 0;
    }
    if (outCount_ == 0) outLo_ = outHi_ = out;
    outLo_ = std::min(outLo_, out);
    outHi_ = std::max(outHi_, out);
    if (++outCount_ >= outBinSamples_)
    {
        const int w = outWrite_.load(std::memory_order_relaxed);
        outMin_[(size_t) w].store(outLo_, std::memory_order_relaxed);
        outMax_[(size_t) w].store(outHi_, std::memory_order_relaxed);
        outWrite_.store((w + 1) % kBins, std::memory_order_relaxed);
        outFilled_.store(std::min(kBins, outFilled_.load(std::memory_order_relaxed) + 1), std::memory_order_relaxed);
        outNewest_.store(written_, std::memory_order_release);
        outCount_ = 0;
    }
}

void Stutter::process(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    const int n = buffer.getNumSamples();
    const int channels = buffer.getNumChannels();
    if (n <= 0 || channels <= 0 || history_[0].empty()) return;
    float* left = buffer.getWritePointer(0);
    float* right = channels > 1 ? buffer.getWritePointer(1) : nullptr;

    const int paramMode = std::clamp((int) std::lround(value(ModeParam)), 0, kModes - 1);
    const bool hold = value(Hold) > 0.5f;
    const int pressed = pressed_.load(std::memory_order_relaxed);
    const bool pressedNew = pressed >= 0 && pressed != lastPressed_;
    lastPressed_ = pressed;
    const float mix = value(Mix);
    const float level = juce::Decibels::decibelsToGain(value(Output));
    const double beatsPerSample = std::clamp(host_.tempo, 20.0, 999.0) / 60.0 / sampleRate_;
    const float engageStep = (float) (1.0 / (0.012 * sampleRate_));

    Settings s = settings();
    int settingsMode = engaged_ ? playingMode_ : paramMode;
    auto refresh = [&](int mode) {
        modeShown_.store(mode, std::memory_order_relaxed);
        engagedShown_.store(true, std::memory_order_relaxed);
        s = settings();
        settingsMode = mode;
    };
    float toneA1 = 0, toneA2 = 0, toneA3 = 0;
    int toneKind = 0; // 0 none, 1 low-pass, 2 high-pass
    auto setTone = [&] {
        const float tone = std::clamp(value(Tone) + s.toneShift, -1.0f, 1.0f);
        toneKind = tone < -0.01f ? 1 : tone > 0.01f ? 2 : 0;
        const float cutoff = toneKind == 1 ? 18000.0f * std::pow(250.0f / 18000.0f, -tone) : 30.0f * std::pow(4000.0f / 30.0f, tone);
        const float g = std::tan(kPi * std::min(cutoff, (float) sampleRate_ * 0.45f) / (float) sampleRate_);
        const float k = 1.1f;
        toneA1 = 1.0f / (1.0f + g * (g + k));
        toneA2 = g * toneA1;
        toneA3 = g * toneA2;
    };
    setTone();
    float peak = 0;

    auto midiIt = midi.cbegin();
    for (int i = 0; i < n; ++i)
    {
        for (; midiIt != midi.cend() && (*midiIt).samplePosition <= i; ++midiIt)
        {
            const auto m = (*midiIt).getMessage();
            if (m.isNoteOn())
            {
                static constexpr int kWhite[12] = {Swell, -1, Rewind, -1, Shatter, Freeze, -1, Spread, -1, Tunnel, -1, Retrigger};
                midiNote_ = m.getNoteNumber();
                midiMode_ = kWhite[midiNote_ % 12];
                midiFresh_ = true;
            }
            else if (m.isNoteOff() && m.getNoteNumber() == midiNote_) midiNote_ = midiMode_ = -1;
            else if (m.isAllNotesOff() || m.isAllSoundOff()) midiNote_ = midiMode_ = -1;
        }

        const float inL = left[i], inR = right != nullptr ? right[i] : left[i];
        const auto slot = (size_t) (written_ & historyMask_);
        history_[0][slot] = inL;
        history_[1][slot] = inR;
        ++written_;

        // ---- engage / release (MIDI > face pad > Hold)
        const bool midiHeld = midiNote_ >= 0;
        const int wantMode = midiHeld && midiMode_ >= 0 ? midiMode_
                           : (pressed >= 0 && pressed < kModes) ? pressed
                                                                 : paramMode;
        const bool want = midiHeld || pressed >= 0 || hold;
        if (want && (! engaged_ || midiFresh_ || (pressedNew && i == 0)))
        {
            playingMode_ = wantMode;
            refresh(wantMode);
            setTone();
            engage(written_);
        }
        else if (want && wantMode != playingMode_)
        {
            playingMode_ = wantMode;
            refresh(wantMode);
            setTone();
        }
        else if (! want && engaged_)
        {
            engaged_ = false;
            engagedShown_.store(false, std::memory_order_relaxed);
        }
        midiFresh_ = false;
        if (! engaged_ && settingsMode != paramMode) // idle: the face shows the selected mode's settings
        {
            settingsMode = paramMode;
            s = settings();
            setTone();
        }

        engage_ = engaged_ ? std::min(1.0f, engage_ + engageStep) : std::max(0.0f, engage_ - engageStep);

        // ---- the read head and the grain clock
        if (engaged_)
        {
            const double rate = std::pow(2.0, (double) s.pitch / 12.0);
            const double span = std::max(32.0, s.grainMs * 0.001 * sampleRate_) * rate;
            const double oldestValid = (double) (written_ - historyMask_ + 8);
            const double lo = std::max({(double) windowStart_, oldestValid, 0.0}); // never before the first sample recorded
            const double liveEdge = (double) written_ - 2.0 - span;
            if (s.fixedSlice) head_ = std::max(lo, (double) anchor_ - std::max(32.0, s.everyMs * 0.001 * sampleRate_));
            else
            {
                head_ += s.speed;
                if (head_ > liveEdge) head_ = s.speed > 1.0f ? lo : liveEdge;
                if (head_ < lo) head_ = (double) anchor_ - span >= lo ? std::min((double) anchor_ - span, liveEdge) : liveEdge;
            }
            bool fire = false;
            if (s.everyBeats > 0 && host_.playing)
            {
                const double beat = host_.beats + (double) i * beatsPerSample;
                const double index = std::floor(beat / s.everyBeats + 1e-9);
                fire = lastGridIndex_ < 0 || std::abs(index - lastGridIndex_) > 0.5; // the first grain at once, then on the grid
                lastGridIndex_ = index;
            }
            else
            {
                untilGrain_ -= 1.0;
                fire = untilGrain_ <= 0;
                if (fire)
                {
                    const double every = std::max(16.0, (double) s.everyMs * 0.001 * sampleRate_);
                    untilGrain_ += every * (1.0 + (double) s.jitter * ((double) random01() - 0.5) * 0.8);
                    if (untilGrain_ < 1.0) untilGrain_ = every;
                }
            }
            if (fire) spawnGrain(s, written_);
        }

        // ---- the grains
        float wetL = 0, wetR = 0;
        if (engage_ > 0.0f)
            for (auto& v : grains_)
            {
                if (! v.active) continue;
                float env = 1.0f;
                if (v.pos < v.fade) env = 0.5f - 0.5f * std::cos(kPi * (float) v.pos / (float) v.fade);
                else if (v.length - v.pos < v.fade) env = 0.5f - 0.5f * std::cos(kPi * (float) (v.length - v.pos) / (float) v.fade);
                float l = 0, r = 0;
                readRecording(v.read, l, r);
                const float g = env * v.amp;
                wetL += l * g * v.gainL;
                wetR += r * g * v.gainR;
                v.read += v.step;
                if (++v.pos >= v.length) v.active = false;
            }
        else
            for (auto& v : grains_) v.active = false;

        if (toneKind != 0)
        {
            float* io[2] = {&wetL, &wetR};
            for (int ch = 0; ch < 2; ++ch)
            {
                auto& st = toneState_[ch];
                const float v3 = *io[ch] - st[1];
                const float v1 = toneA1 * st[0] + toneA2 * v3;
                const float v2 = st[1] + toneA2 * st[0] + toneA3 * v3;
                st[0] = 2.0f * v1 - st[0];
                st[1] = 2.0f * v2 - st[1];
                *io[ch] = toneKind == 1 ? v2 : *io[ch] - 1.1f * v1 - v2;
            }
        }
        if (s.tunnelMs > 0) tube_.process(wetL, wetR, s.tunnelMs * 0.001f * (float) sampleRate_, s.tunnelFeedback);

        float roomL = 0, roomR = 0;
        room_.process(wetL * engage_ * s.space, wetR * engage_ * s.space, roomL, roomR);

        const float dryGain = 1.0f - mix * engage_;
        const float outL = inL * dryGain + (wetL * engage_ + roomL) * level;
        const float outR = inR * dryGain + (wetR * engage_ + roomR) * level;
        left[i] = outL;
        if (right != nullptr) right[i] = outR;
        peak = std::max(peak, std::max(std::abs(outL), std::abs(outR)));
        publishBins(inL, inR, outL, outR);
    }

    now_.store(written_, std::memory_order_relaxed);
    anchorShown_.store(anchor_, std::memory_order_relaxed);
    headShown_.store((int64_t) head_, std::memory_order_relaxed);
    windowShown_.store(std::max(windowStart_, written_ - historyMask_), std::memory_order_relaxed);
    if (! engaged_) modeShown_.store(paramMode, std::memory_order_relaxed);
    level_.store(std::max(peak, level_.load(std::memory_order_relaxed) * 0.85f), std::memory_order_relaxed);
}

void Stutter::readView(View& v) const
{
    v.engaged = engagedShown_.load(std::memory_order_relaxed);
    v.mode = modeShown_.load(std::memory_order_relaxed);
    v.now = now_.load(std::memory_order_relaxed);
    v.anchor = anchorShown_.load(std::memory_order_relaxed);
    v.head = headShown_.load(std::memory_order_relaxed);
    v.windowStart = windowShown_.load(std::memory_order_relaxed);
    v.sampleRate = sampleRate_;
    v.playing = settings();
    v.level = level_.load(std::memory_order_relaxed);
    const auto copy = [](const std::array<std::atomic<float>, kBins>& lo, const std::array<std::atomic<float>, kBins>& hi, int write,
                         int filled, std::array<float, kBins>& outLo, std::array<float, kBins>& outHi) {
        for (int i = 0; i < filled; ++i)
        {
            const auto slot = (size_t) ((write - filled + i + kBins) % kBins);
            outLo[(size_t) i] = lo[slot].load(std::memory_order_relaxed);
            outHi[(size_t) i] = hi[slot].load(std::memory_order_relaxed);
        }
    };
    v.inputBins = inFilled_.load(std::memory_order_relaxed);
    v.outputBins = outFilled_.load(std::memory_order_relaxed);
    v.inputNewest = inNewest_.load(std::memory_order_acquire);
    v.outputNewest = outNewest_.load(std::memory_order_acquire);
    copy(inMin_, inMax_, inWrite_.load(std::memory_order_relaxed), v.inputBins, v.inMin, v.inMax);
    copy(outMin_, outMax_, outWrite_.load(std::memory_order_relaxed), v.outputBins, v.outMin, v.outMax);
    v.grains.clear();
    const uint32_t next = grainLogNext_.load(std::memory_order_acquire);
    const uint32_t count = std::min<uint32_t>(next, kGrainLog);
    for (uint32_t i = next - count; i != next; ++i)
    {
        const auto& e = grainLog_[(size_t) (i % kGrainLog)];
        GrainMark g;
        g.at = e.at.load(std::memory_order_relaxed);
        g.read = e.read.load(std::memory_order_relaxed);
        g.length = e.length.load(std::memory_order_relaxed);
        g.pan = e.pan.load(std::memory_order_relaxed);
        g.reversed = e.reversed.load(std::memory_order_relaxed);
        v.grains.push_back(g);
    }
}

std::vector<DeviceProcessor::UiGroup> Stutter::uiGroups() const
{
    return {{"Play", {ModeParam, Motion, Macro, Hold, Capture, Sync}, 3},
            {"Grain", {Grain, Shape, Every, Speed}, 2},
            {"Sound", {Pitch, Width, Jitter, Tone, Space, Mix, Output}, 4}};
}
} // namespace sonora::devices
