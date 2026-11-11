#include "devices/native/Stutter.h"

#include <algorithm>
#include <cmath>

namespace sonora::devices
{
namespace
{
constexpr float kPi = 3.14159265f;
constexpr double kGrainSeconds = 0.09;     // Freeze grain length
constexpr double kMinRepeatSeconds = 0.004; // Shrink floor

std::vector<ParameterInfo> stutterParameters()
{
    using S = Stutter;
    std::vector<std::string> modes(S::kModeNames, S::kModeNames + S::kModes);
    std::vector<std::string> rates(S::kRateNames, S::kRateNames + S::kRates);
    std::vector<std::string> steps{"Off"};
    steps.insert(steps.end(), modes.begin(), modes.end());
    std::vector<ParameterInfo> p(S::NumParams);
    p[S::ModeParam] = makeParam("Mode", "Mode", 0, S::kModes - 1, 0, "", false, modes);
    p[S::Hold] = makeParam("Hold", "Hold", 0, 1, 0, "", true);
    p[S::Rate] = makeParam("Rate", "Rate", 0, S::kRates - 1, 6, "", false, rates);
    p[S::Sync] = makeParam("Sync", "Sync", 0, 1, 1, "", true);
    p[S::LengthMs] = makeParam("Length", "Length", 10, 2000, 120, "ms");
    p[S::Quantize] = makeParam("Quantize", "Quantize", 0, S::kQuantizes - 1, 1, "", false, {"Off", "1/16", "1/8", "1/4", "1 Bar"});
    p[S::Shrink] = makeParam("Shrink", "Shrink", 0, 1, 0, "%");
    p[S::Pitch] = makeParam("Pitch", "Pitch", -24, 24, 0, "st");
    p[S::Ramp] = makeParam("Ramp", "Ramp", -12, 12, 0, "st");
    p[S::Decay] = makeParam("Decay", "Decay", 0, 1, 0, "%");
    p[S::Spacing] = makeParam("Spacing", "Spacing", 0.05f, 1, 1, "%");
    p[S::Smooth] = makeParam("Smooth", "Smooth", 0.5f, 30, 3, "ms");
    p[S::Wide] = makeParam("Wide", "Wide", 0, 1, 0, "%");
    p[S::Tone] = makeParam("Tone", "Tone", -1, 1, 0);
    p[S::Mix] = makeParam("Mix", "Mix", 0, 1, 1, "%");
    p[S::Output] = makeParam("Output", "Output", -24, 12, 0, "dB");
    p[S::PatternOn] = makeParam("Pattern", "Pattern", 0, 1, 0, "", true);
    p[S::Chance] = makeParam("Chance", "Chance", 0, 1, 1, "%");
    for (int s = 0; s < S::kSteps; ++s)
        p[(size_t) S::stepParam(s)] = makeParam("Step" + std::to_string(s), "Step " + std::to_string(s + 1), 0, S::kModes,
                                                s >= 12 ? 1.0f : 0.0f, "", false, steps); // last beat: Repeat
    return p;
}
} // namespace

Stutter::Stutter() : NativeDevice(stutterParameters()) {}

void Stutter::prepare(double sampleRate, int)
{
    sampleRate_ = sampleRate > 0 ? sampleRate : 44100.0;
    int size = 1;
    while (size < (int) (kHistorySeconds * sampleRate_)) size <<= 1;
    for (auto& h : history_) h.assign((size_t) size, 0.0f);
    historyMask_ = size - 1;
    reset();
}

void Stutter::reset()
{
    for (auto& h : history_) std::fill(h.begin(), h.end(), 0.0f);
    written_ = 0;
    event_ = {};
    for (auto& g : grains_) g = {};
    engage_ = 0;
    for (auto& ch : toneState_) ch[0] = ch[1] = 0;
    midiMode_ = midiNote_ = -1;
    midiPitch_ = 0;
    manualSeen_ = manualActive_ = pendingMode_ = -1;
    manualSeenPitch_ = manualPitch_ = pendingPitch_ = 0;
    pendingBeat_ = -1;
    midiFresh_ = midiPitched_ = manualPast_ = pendingPast_ = false;
    lastStep_ = stepMode_ = -1;
    stepFires_ = true;
    viewEngaged_.store(false);
    viewStep_.store(-1);
}

double Stutter::sliceSamples() const
{
    if (value(Sync) > 0.5f)
    {
        const int rate = std::clamp((int) std::lround(value(Rate)), 0, kRates - 1);
        const double tempo = std::clamp(host_.tempo, 20.0, 999.0);
        return std::max(16.0, kRateBeats[rate] * 60.0 / tempo * sampleRate_);
    }
    return std::max(16.0, (double) value(LengthMs) * 0.001 * sampleRate_);
}

void Stutter::readHistory(double index, float& l, float& r) const
{
    // Never read what is not written yet or what the ring already overwrote.
    const double newest = (double) (written_ - 1), oldest = (double) (written_ - historyMask_);
    index = std::clamp(index, std::max(0.0, oldest), std::max(0.0, newest));
    const auto i0 = (int64_t) std::floor(index);
    const float frac = (float) (index - (double) i0);
    const int64_t i1 = std::min<int64_t>(i0 + 1, written_ - 1);
    const auto a = (size_t) (i0 & historyMask_), b = (size_t) (std::max<int64_t>(i1, 0) & historyMask_);
    l = history_[0][a] + (history_[0][b] - history_[0][a]) * frac;
    r = history_[1][a] + (history_[1][b] - history_[1][a]) * frac;
}

float Stutter::fadeAt(double pos, double length, double smooth) const
{
    if (pos < 0 || pos >= length) return 0.0f;
    smooth = std::max(1.0, std::min(smooth, length * 0.5));
    return (float) std::min({1.0, (pos + 1.0) / smooth, (length - pos) / smooth});
}

void Stutter::begin(int mode, float pitch, bool fromPast, int64_t at)
{
    event_ = {};
    event_.active = true;
    event_.mode = mode;
    event_.start = at;
    event_.slice = sliceSamples();
    event_.pitch = pitch;
    event_.fromPast = fromPast; // a pitched note repeats what came just before it, so it sounds pitched at once
    event_.read = (double) at;
    event_.speed = 1;
    startRepeat();
    grainNext_ = 0;
    for (auto& g : grains_) g = {};
    publishSlice();
}

void Stutter::startRepeat()
{
    const double shrink = 1.0 - 0.5 * (double) value(Shrink);
    const double floor = kMinRepeatSeconds * sampleRate_;
    event_.current = std::max(std::min(floor, event_.slice), event_.slice * std::pow(shrink, (double) event_.repeat));
    event_.gain = std::pow(1.0f - value(Decay), (float) event_.repeat);
    event_.pan = event_.repeat == 0 ? 0.0f : value(Wide) * (event_.repeat % 2 == 1 ? -1.0f : 1.0f);
}

void Stutter::publishSlice()
{
    // The audio shown for the event: what Repeat loops, what Reverse plays backwards, what Freeze holds.
    double from = (double) event_.start, length = event_.slice;
    if (event_.mode == Reverse || event_.fromPast || event_.mode == Stop || event_.mode == Gate) from -= length;
    if (event_.mode == Freeze)
    {
        length = kGrainSeconds * sampleRate_ * 1.5;
        from = (double) event_.start - length;
    }
    if (event_.mode == Repeat && ! event_.fromPast && event_.repeat == 0) return; // not captured yet: at the first repeat
    const double step = length / kViewBins;
    for (int b = 0; b < kViewBins; ++b)
    {
        float lo = 0, hi = 0;
        const int count = std::max(1, std::min(64, (int) step)); // sparse reads for long slices
        for (int k = 0; k < count; ++k)
        {
            float l = 0, r = 0;
            readHistory(from + step * ((double) b + (double) k / count), l, r);
            const float m = 0.5f * (l + r);
            lo = std::min(lo, m);
            hi = std::max(hi, m);
        }
        viewMin_[(size_t) b].store(lo, std::memory_order_relaxed);
        viewMax_[(size_t) b].store(hi, std::memory_order_relaxed);
    }
    viewBins_.store(kViewBins, std::memory_order_relaxed);
    viewSerial_.fetch_add(1, std::memory_order_release);
}

void Stutter::wetSample(float inL, float inR, float& outL, float& outR)
{
    auto& e = event_;
    outL = outR = 0;
    const double smooth = (double) value(Smooth) * 0.001 * sampleRate_;
    const float spacing = value(Spacing);
    const auto speedFor = [&](int repeat) {
        const float semis = value(Pitch) + value(Ramp) * (float) repeat + e.pitch;
        return std::clamp(std::pow(2.0, (double) semis / 12.0), 0.25, 4.0);
    };
    switch (e.mode)
    {
        case Repeat:
        case Reverse:
        {
            if (e.pos >= e.current)
            {
                e.pos -= e.current;
                ++e.repeat;
                startRepeat();
                if (e.repeat == 1 && e.mode == Repeat && ! e.fromPast) publishSlice();
            }
            const double sounding = e.current * spacing;
            const float env = fadeAt(e.pos, sounding, smooth) * e.gain;
            if (env > 0)
            {
                float l = 0, r = 0;
                if (e.mode == Repeat && e.repeat == 0 && ! e.fromPast) { l = inL; r = inR; } // capturing: the live input
                else
                {
                    // Pitched repeats read the slice faster or slower inside the repeat's length.
                    const double offset = std::fmod(e.pos * speedFor(e.repeat), e.current);
                    const double base = e.mode == Reverse ? (double) e.start - 1.0 - offset
                                                          : (double) e.start - (e.fromPast ? e.slice : 0.0) + offset;
                    readHistory(base, l, r);
                }
                outL = l * env * std::min(1.0f, 1.0f - e.pan);
                outR = r * env * std::min(1.0f, 1.0f + e.pan);
            }
            break;
        }
        case Freeze:
        {
            // Two Hann grains half a grain apart (they sum to one), each reading a little before the engage point.
            const double grain = kGrainSeconds * sampleRate_;
            if (e.pos >= e.read) // time for the next grain
            {
                auto& g = grains_[grainNext_];
                grainNext_ ^= 1;
                noise_ = noise_ * 1664525u + 1013904223u;
                const double jitter = (double) (noise_ >> 8) / 16777216.0 * grain * 0.4;
                g = {(double) e.start - grain * 1.2 - jitter, 0.0, grain, true};
                e.read = e.pos + grain * 0.5;
            }
            const double speed = speedFor(0);
            for (auto& g : grains_)
            {
                if (! g.active) continue;
                const float w = std::sin(kPi * (float) (g.pos / g.length));
                float l = 0, r = 0;
                readHistory(g.read + g.pos * speed, l, r);
                outL += l * w * w;
                outR += r * w * w;
                g.pos += 1.0;
                if (g.pos >= g.length) g.active = false;
            }
            const float fade = (float) std::min(1.0, (e.pos + 1.0) / std::max(1.0, smooth));
            outL *= fade;
            outR *= fade;
            break;
        }
        case Stop:
        {
            // Tape stop over one slice: the speed falls from 1 to 0 (a little faster at the end, like a platter).
            const double t = std::min(1.0, e.pos / e.slice);
            e.speed = std::pow(1.0 - t, 1.6);
            if (t < 1.0)
            {
                float l = 0, r = 0;
                readHistory(e.read, l, r);
                const float tail = (float) std::min(1.0, (e.slice - e.pos) / std::max(1.0, smooth));
                outL = l * tail;
                outR = r * tail;
                e.read += e.speed;
            }
            break;
        }
        case Gate:
        default:
        {
            const double phase = std::fmod(e.pos, e.slice);
            const float env = fadeAt(phase, e.slice * std::min(0.95f, spacing < 1.0f ? spacing : 0.5f), smooth);
            outL = inL * env;
            outR = inR * env;
            break;
        }
    }
    e.pos += 1.0;
}

int Stutter::patternMode(double beat, bool& fresh)
{
    if (value(PatternOn) < 0.5f) { lastStep_ = stepMode_ = -1; return -1; }
    const int step = (int) (std::fmod(std::floor(beat * 4.0 + 1e-6), (double) kSteps) + kSteps) % kSteps;
    if (step != lastStep_)
    {
        const int v = (int) std::lround(value(stepParam(step))) - 1;
        const bool continues = v >= 0 && v == stepMode_ && lastStep_ >= 0
                               && (int) std::lround(value(stepParam(lastStep_))) - 1 == v && (lastStep_ + 1) % kSteps == step;
        if (v >= 0 && ! continues)
        {
            noise_ = noise_ * 1664525u + 1013904223u;
            stepFires_ = (float) (noise_ >> 8) / 16777216.0f < value(Chance) || value(Chance) >= 1.0f;
            fresh = stepFires_;
        }
        stepMode_ = v;
        lastStep_ = step;
    }
    return stepMode_ >= 0 && stepFires_ ? stepMode_ : -1;
}

void Stutter::process(juce::AudioBuffer<float>& audio, juce::MidiBuffer& midi)
{
    const int n = audio.getNumSamples();
    if (n <= 0 || audio.getNumChannels() < 1 || history_[0].empty()) return;
    float* left = audio.getWritePointer(0);
    float* right = audio.getNumChannels() > 1 ? audio.getWritePointer(1) : left;
    const bool bypass = isBypassed();

    // Tone: one state-variable filter per channel, low-pass to the left of centre, high-pass to the right.
    const float tone = value(Tone);
    const bool toneOn = std::abs(tone) > 0.01f;
    const float cutoff = tone < 0 ? 20000.0f * std::pow(200.0f / 20000.0f, -tone) : 20.0f * std::pow(4000.0f / 20.0f, tone);
    const float g = std::tan(kPi * std::min(cutoff, (float) sampleRate_ * 0.45f) / (float) sampleRate_);
    const float k = 1.2f, a1 = 1.0f / (1.0f + g * (g + k)), a2 = g * a1, a3 = g * a2;
    const float wetGain = juce::Decibels::decibelsToGain(value(Output)), mix = value(Mix);
    const double smoothSamples = std::max(1.0, (double) value(Smooth) * 0.001 * sampleRate_);
    const float engageStep = (float) (1.0 / smoothSamples);
    const int mode = std::clamp((int) std::lround(value(ModeParam)), 0, kModes - 1);
    const bool hold = value(Hold) > 0.5f;
    const double q = kQuantizeBeats[std::clamp((int) std::lround(value(Quantize)), 0, kQuantizes - 1)];
    const double beatsPerSample = std::clamp(host_.tempo, 20.0, 999.0) / 60.0 / sampleRate_;
    const bool playing = host_.playing;

    auto midiIt = midi.cbegin();
    int stepShown = -1;
    for (int i = 0; i < n; ++i)
    {
        // ---- MIDI at this sample: C1..B1 pick a mode while held, C2 and up play Repeat pitched from C3.
        for (; midiIt != midi.cend() && (*midiIt).samplePosition <= i; ++midiIt)
        {
            const auto m = (*midiIt).getMessage();
            if (m.isNoteOn())
            {
                const int note = m.getNoteNumber();
                int noteMode = -1;
                float notePitch = 0;
                if (note >= 36 && note < 48)
                {
                    static constexpr int kWhite[12] = {Repeat, -1, Reverse, -1, Freeze, Stop, -1, Gate, -1, -1, -1, -1};
                    noteMode = kWhite[note - 36];
                }
                else if (note >= 48) { noteMode = Repeat; notePitch = (float) (note - 60); }
                if (noteMode >= 0) { midiMode_ = noteMode; midiPitch_ = notePitch; midiNote_ = note; midiFresh_ = true; midiPitched_ = note >= 48; }
            }
            else if (m.isNoteOff() && m.getNoteNumber() == midiNote_) { midiMode_ = midiNote_ = -1; midiPitch_ = 0; }
            else if (m.isAllNotesOff() || m.isAllSoundOff()) { midiMode_ = midiNote_ = -1; midiPitch_ = 0; }
        }

        const double beat = host_.beats + (double) i * beatsPerSample;
        bool fresh = false;

        // ---- manual sources (face pads > MIDI > Hold), waiting for the quantize grid while the transport plays
        const int pressed = pressed_.load(std::memory_order_relaxed);
        const int manual = pressed >= 0 ? std::min(pressed, kModes - 1) : midiMode_ >= 0 ? midiMode_ : hold ? mode : -1;
        const float manualPitch = pressed < 0 && midiMode_ >= 0 ? midiPitch_ : 0.0f;
        const bool manualPast = pressed < 0 && midiMode_ >= 0 && midiPitched_;
        if (manual < 0) { manualActive_ = -1; pendingBeat_ = -1; }
        else if (manual != manualSeen_ || std::abs(manualPitch - manualSeenPitch_) > 1e-3f || midiFresh_)
        {
            if (playing && q > 0)
            {
                pendingBeat_ = std::ceil(beat / q - 1e-6) * q;
                pendingMode_ = manual;
                pendingPitch_ = manualPitch;
                pendingPast_ = manualPast;
                if (manualActive_ >= 0 && manualActive_ != manual) manualActive_ = -1;
            }
            else { manualActive_ = manual; manualPitch_ = manualPitch; manualPast_ = manualPast; pendingBeat_ = -1; fresh = true; }
        }
        midiFresh_ = false;
        manualSeen_ = manual;
        manualSeenPitch_ = manualPitch;
        if (pendingBeat_ >= 0 && (! playing || beat >= pendingBeat_ - 1e-9))
        {
            manualActive_ = pendingMode_;
            manualPitch_ = pendingPitch_;
            manualPast_ = pendingPast_;
            pendingBeat_ = -1;
            fresh = true;
        }

        // ---- the pattern, while the transport plays
        int want = -1;
        float wantPitch = 0;
        bool wantPast = false;
        bool patternFresh = false;
        const int fromPattern = playing ? patternMode(beat, patternFresh) : (lastStep_ = stepMode_ = -1);
        if (playing && value(PatternOn) > 0.5f) stepShown = lastStep_;
        if (manualActive_ >= 0) { want = manualActive_; wantPitch = manualPitch_; wantPast = manualPast_; }
        else if (fromPattern >= 0) { want = fromPattern; fresh = fresh || patternFresh; }

        if (want >= 0 && (fresh || ! event_.active || event_.mode != want || std::abs(event_.pitch - wantPitch) > 1e-3f))
            begin(want, wantPitch, wantPast, written_);

        // ---- record, play
        const float inL = left[i], inR = right[i];
        const auto w = (size_t) (written_ & historyMask_);
        history_[0][w] = inL;
        history_[1][w] = inR;
        ++written_;

        engage_ = want >= 0 ? std::min(1.0f, engage_ + engageStep) : std::max(0.0f, engage_ - engageStep);
        if (engage_ <= 0.0f && want < 0) event_.active = false;
        if (bypass || ! event_.active) continue; // the input passes untouched

        float wl = 0, wr = 0;
        wetSample(inL, inR, wl, wr);
        if (toneOn)
        {
            float* outs[2] = {&wl, &wr};
            for (int ch = 0; ch < 2; ++ch)
            {
                auto& s = toneState_[ch];
                const float x = *outs[ch];
                const float v3 = x - s[1], v1 = a1 * s[0] + a2 * v3, v2 = s[1] + a2 * s[0] + a3 * v3;
                s[0] = 2 * v1 - s[0];
                s[1] = 2 * v2 - s[1];
                *outs[ch] = tone < 0 ? v2 : x - k * v1 - v2;
            }
        }
        const float wet = engage_ * mix;
        left[i] = inL * (1.0f - wet) + wl * wetGain * wet;
        if (right != left) right[i] = inR * (1.0f - wet) + wr * wetGain * wet;
    }
    if (! event_.active) for (auto& ch : toneState_) ch[0] = ch[1] = 0;

    // ---- face
    viewEngaged_.store(event_.active && ! bypass, std::memory_order_relaxed);
    viewMode_.store(event_.mode, std::memory_order_relaxed);
    viewRepeat_.store(event_.repeat, std::memory_order_relaxed);
    viewStep_.store(stepShown, std::memory_order_relaxed);
    float position = 0;
    if (event_.active)
    {
        const auto& e = event_;
        if (e.mode == Repeat || e.mode == Reverse)
        {
            const double offset = std::fmod(e.pos * std::pow(2.0, (double) (value(Pitch) + value(Ramp) * (float) e.repeat + e.pitch) / 12.0), e.current);
            position = (float) (offset / e.slice);
            if (e.mode == Reverse) position = 1.0f - position;
        }
        else if (e.mode == Stop) position = (float) std::clamp((e.read - (double) e.start) / e.slice, 0.0, 1.0);
        else if (e.mode == Gate) position = (float) (std::fmod(e.pos, e.slice) / e.slice);
        else position = 0.8f;
    }
    viewPos_.store(position, std::memory_order_relaxed);
}

void Stutter::readView(View& out) const
{
    out.engaged = viewEngaged_.load(std::memory_order_relaxed);
    out.mode = viewMode_.load(std::memory_order_relaxed);
    out.repeat = viewRepeat_.load(std::memory_order_relaxed);
    out.step = viewStep_.load(std::memory_order_relaxed);
    out.position = viewPos_.load(std::memory_order_relaxed);
    const uint32_t serial = viewSerial_.load(std::memory_order_acquire);
    if (serial == out.serial) return;
    out.bins = viewBins_.load(std::memory_order_relaxed);
    for (int b = 0; b < kViewBins; ++b)
    {
        out.min[(size_t) b] = viewMin_[(size_t) b].load(std::memory_order_relaxed);
        out.max[(size_t) b] = viewMax_[(size_t) b].load(std::memory_order_relaxed);
    }
    out.serial = serial;
}

std::vector<DeviceProcessor::UiGroup> Stutter::uiGroups() const
{
    std::vector<UiGroup> groups{{"Trigger", {ModeParam, Hold, Quantize, PatternOn, Chance}, 2},
                                {"Slice", {Rate, Sync, LengthMs, Shrink, Spacing, Smooth}, 3},
                                {"Pitch", {Pitch, Ramp, Decay, Wide}, 2},
                                {"Out", {Tone, Mix, Output}, 2}};
    UiGroup pattern{"Pattern", {}, 8};
    for (int s = 0; s < kSteps; ++s) pattern.params.push_back(stepParam(s));
    groups.push_back(pattern);
    return groups;
}

bool Stutter::parameterShown(int index) const
{
    if (index == LengthMs) return value(Sync) < 0.5f;
    if (index == Rate) return value(Sync) >= 0.5f;
    return true;
}
} // namespace sonora::devices
