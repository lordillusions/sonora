#include "devices/native/Arpeggiator.h"

#include "core/Scales.h"

#include <algorithm>
#include <climits>
#include <cmath>

namespace sonora::devices
{

namespace
{
// Step lengths as fractions of a whole note, fastest first; the reference's dial offers all 14 (1/128 .. 1/1).
// RetriggerInterval, also in whole notes: the reference labels 1/16 .. 7/8, 1, 9/8, 5/4, 1.5, 2, 3, 4 (read in Live
// 12.4.6; "1/4" restarts the pattern every quarter note, ROADMAP DEV-21c).
constexpr std::array<double, 14> kRates{1.0 / 128, 1.0 / 96, 1.0 / 64, 1.0 / 48, 1.0 / 32, 1.0 / 24, 1.0 / 16,
                                        1.0 / 12, 1.0 / 8, 1.0 / 6, 1.0 / 4, 1.0 / 3, 1.0 / 2, 1.0};
constexpr std::array<double, 15> kRetriggers{1.0 / 16, 1.0 / 8, 3.0 / 16, 1.0 / 4, 3.0 / 8, 1.0 / 2, 3.0 / 4, 7.0 / 8,
                                             1, 9.0 / 8, 5.0 / 4, 1.5, 2, 3, 4};
constexpr double kSwing = 1.0 / 3.0; // off-beat moves a third of its grid later (measured: Swing 8/16 at the default groove amount)
constexpr int kByteLimit = 64000;

std::vector<std::string> rateNames(bool retrigger)
{
    std::vector<std::string> names;
    if (retrigger)
    {
        for (const char* n : {"1/16", "1/8", "3/16", "1/4", "3/8", "1/2", "3/4", "7/8", "1", "9/8", "5/4", "1.5", "2", "3", "4"})
            names.emplace_back(n);
        return names;
    }
    for (const char* n : {"1/128", "1/96", "1/64", "1/48", "1/32", "1/24", "1/16", "1/12", "1/8", "1/6", "1/4", "1/3", "1/2", "1/1"})
        names.emplace_back(n);
    return names;
}

std::vector<std::string> repeatNames()
{
    std::vector<std::string> names{"inf"};
    for (int i = 1; i <= 16; ++i) names.push_back(std::to_string(i));
    return names;
}

std::vector<std::string> transposeModeNames()
{
    std::vector<std::string> names{"Chromatic"};
    for (const auto& scale : model::kScales) names.emplace_back(scale.name);
    return names;
}

uint32_t nextRandom(uint32_t& seed)
{
    seed = seed * 1664525u + 1013904223u;
    return seed >> 8;
}
} // namespace

Arpeggiator::Arpeggiator() : NativeDevice([] {
    using A = Arpeggiator;
    std::vector<ParameterInfo> p(A::NumParams);
    p[A::Style] = makeParam("Mode", "Style", 0, NumStyles - 1, 0, "", false,
                            {"Up", "Down", "UpDown", "DownUp", "Up & Down", "Down & Up", "Converge", "Diverge", "Con & Diverge",
                             "Pinky Up", "Pinky UpDown", "Thumb Up", "Thumb UpDown", "Play Order", "Chord Trigger", "Random",
                             "Random Other", "Random Once"});
    p[A::Groove] = makeParam("Groove", "Groove", 0, 3, 0, "", false, {"None", "Swing 8", "Swing 16", "Swing 32"});
    p[A::Hold] = makeParam("Hold", "Hold", 0, 1, 0, "", true);
    p[A::Offset] = makeParam("PatternOffset", "Offset", -8, 8, 0, "int");
    p[A::Sync] = makeParam("SyncState", "Sync", 0, 1, 1, "", true);
    p[A::SyncedRate] = makeParam("SyncedRate", "Rate", 0, 13, 6, "", false, rateNames(false));
    p[A::FreeRate] = makeParam("FreeRate", "Rate", 10, 1000, 200, "ms");
    p[A::Gate] = makeParam("Gate", "Gate", 1, 200, 50, "pct");
    p[A::Retrigger] = makeParam("Retrigger", "Retrigger", 0, 2, 0, "", false, {"Off", "Note", "Beat"});
    p[A::RetriggerInterval] = makeParam("RetriggerInterval", "Interval", 0, 14, 8, "", false, rateNames(true));
    p[A::Repeats] = makeParam("RepeatCount", "Repeats", 0, 16, 0, "", false, repeatNames());
    p[A::TransposeMode] = makeParam("TransposeMode", "Scale", 0, (float) model::kScales.size(), 0, "", false, transposeModeNames());
    p[A::TransposeKey] = makeParam("TransposeKey", "Root", 0, 11, 0, "", false,
                                   {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"});
    p[A::TransposeSteps] = makeParam("TransposeSteps", "Steps", 0, 8, 1, "int");
    p[A::TransposeDistance] = makeParam("TransposeDistance", "Distance", -24, 24, 0, "int");
    p[A::VelocityOn] = makeParam("VelocitySwitch", "Velocity", 0, 1, 0, "", true);
    p[A::VelocityRetrigger] = makeParam("RetriggerVelocity", "Retrig", 0, 1, 1, "", true);
    p[A::VelocityDecay] = makeParam("VelocityDecay", "Decay", 10, 60000, 1000, "ms");
    p[A::VelocityTarget] = makeParam("VelocityTarget", "Target", 0, 127, 64, "int");
    return p;
}())
{
}

std::vector<DeviceProcessor::UiGroup> Arpeggiator::uiGroups() const
{
    return {
        {"Style", {Style, Groove, Hold, Offset}},
        {"Rate", {Sync, SyncedRate, FreeRate, Gate, Repeats}},
        {"Retrigger", {Retrigger, RetriggerInterval}},
        {"Transpose", {TransposeMode, TransposeKey, TransposeDistance, TransposeSteps}},
        {"Velocity", {VelocityOn, VelocityRetrigger, VelocityDecay, VelocityTarget}},
    };
}

bool Arpeggiator::parameterShown(int index) const
{
    switch (index)
    {
        case SyncedRate: return value(Sync) > 0.5f;
        case FreeRate: return value(Sync) <= 0.5f;
        case RetriggerInterval: return std::lround(value(Retrigger)) == 2;
        case TransposeKey: return std::lround(value(TransposeMode)) > 0;
        case VelocityRetrigger: case VelocityDecay: case VelocityTarget: return value(VelocityOn) > 0.5f;
        default: return true;
    }
}

double Arpeggiator::syncedRateBeats(int index)
{
    return 4.0 * kRates[(size_t) std::clamp(index, 0, (int) kRates.size() - 1)];
}

double Arpeggiator::retriggerIntervalBeats(int index)
{
    return 4.0 * kRetriggers[(size_t) std::clamp(index, 0, (int) kRetriggers.size() - 1)];
}

double Arpeggiator::grooveWarp(double beat, int groove)
{
    if (groove <= 0 || groove > 3) return beat;
    const double grid = groove == 1 ? 0.5 : groove == 2 ? 0.25 : 0.125; // Swing 8 / 16 / 32
    const double pair = 2.0 * grid;
    const double base = std::floor(beat / pair + 1e-9) * pair;
    const double f = std::max(0.0, beat - base);
    return base + (f < grid ? f * (1.0 + kSwing) : grid * (1.0 + kSwing) + (f - grid) * (1.0 - kSwing));
}

int Arpeggiator::buildPattern(int style, int n, const int* arrival, uint32_t& seed, int* out)
{
    if (n <= 0) return 0;
    int len = 0;
    auto push = [&](int i) { if (len < kMaxPattern) out[len++] = i; };
    auto converge = [&](bool reversed) {
        const int start = len;
        for (int lo = 0, hi = n - 1; lo <= hi;)
        {
            push(lo++);
            if (lo <= hi) push(hi--);
        }
        if (reversed) std::reverse(out + start, out + len);
    };
    auto shuffle = [&] {
        for (int i = 0; i < n; ++i) push(i);
        for (int i = n - 1; i > 0; --i)
            std::swap(out[i], out[(int) (nextRandom(seed) % (uint32_t) (i + 1))]);
    };
    switch (style)
    {
        case Down: for (int i = n - 1; i >= 0; --i) push(i); break;
        case UpDown:
            for (int i = 0; i < n; ++i) push(i);
            for (int i = n - 2; i >= 1; --i) push(i);
            break;
        case DownUp:
            for (int i = n - 1; i >= 0; --i) push(i);
            for (int i = 1; i <= n - 2; ++i) push(i);
            break;
        case UpAndDown:
            for (int i = 0; i < n; ++i) push(i);
            for (int i = n - 1; i >= 0; --i) push(i);
            break;
        case DownAndUp:
            for (int i = n - 1; i >= 0; --i) push(i);
            for (int i = 0; i < n; ++i) push(i);
            break;
        case Converge: converge(false); break;
        case Diverge: converge(true); break;
        case ConAndDiverge:
        {
            converge(false);
            const int half = len;
            for (int i = half - 2; i >= 1; --i) push(out[i]); // back out again without repeating either end
            break;
        }
        case PinkyUp:
        case PinkyUpDown:
            if (n == 1) { push(0); break; }
            for (int i = 0; i <= n - 2; ++i) { push(i); push(n - 1); }
            if (style == PinkyUpDown)
                for (int i = n - 3; i >= 1; --i) { push(i); push(n - 1); }
            break;
        case ThumbUp:
        case ThumbUpDown:
            if (n == 1) { push(0); break; }
            for (int i = 1; i <= n - 1; ++i) { push(0); push(i); }
            if (style == ThumbUpDown)
                for (int i = n - 2; i >= 2; --i) { push(0); push(i); }
            break;
        case PlayOrder:
            len = n;
            for (int i = 0; i < n; ++i) out[std::clamp(arrival[i], 0, n - 1)] = i;
            break;
        case ChordTrigger: push(0); break;
        case RandomOther:
        case RandomOnce: shuffle(); break;
        default: for (int i = 0; i < n; ++i) push(i); break; // Up; Random draws per step
    }
    return len;
}

void Arpeggiator::prepare(double sampleRate, int)
{
    sampleRate_ = sampleRate > 0 ? sampleRate : 44100.0;
    scratch_.ensureSize(kByteLimit + 4096);
    reset();
}

void Arpeggiator::reset()
{
    count_ = 0;
    numSounding_ = 0;
    patternLength_ = 0;
    patternDirty_ = true;
    nextFree_ = -1;
    lastStep_ = -1;
    stepCounter_ = 0;
    retriggerPending_ = false;
    expectedBeat_ = lastStepBeat_ = lastStepNominal_ = -1e30;
    passedThrough_ = {};
}

double Arpeggiator::stepSamples() const
{
    if (value(Sync) > 0.5f)
        return syncedRateBeats((int) std::lround(value(SyncedRate))) * samplesPerBeat_;
    return std::max(1.0, value(FreeRate) * 0.001 * sampleRate_);
}

void Arpeggiator::emit(const uint8_t* data, int size, int64_t at)
{
    if (bytes_ + size + 8 > kByteLimit) { overflow_ = true; return; }
    const int sample = (int) std::clamp<int64_t>(at - blockStart_, 0, std::max(0, blockLength_ - 1));
    scratch_.addEvent(data, size, sample);
    bytes_ += size + 8;
}

void Arpeggiator::noteOff(int index, int64_t at)
{
    const auto s = sounding_[(size_t) index];
    const uint8_t message[]{(uint8_t) (0x80 | s.channel), s.pitch, 64};
    emit(message, 3, at);
    sounding_[(size_t) index] = sounding_[(size_t) --numSounding_];
}

void Arpeggiator::rebuildPattern()
{
    std::array<int, kMaxNotes> arrival{};
    // Rank of each sorted note in playing order.
    for (int i = 0; i < count_; ++i)
    {
        int rank = 0;
        for (int j = 0; j < count_; ++j)
            if (notes_[(size_t) j].order < notes_[(size_t) i].order) ++rank;
        arrival[(size_t) i] = rank;
    }
    patternLength_ = buildPattern((int) std::lround(value(Style)), count_, arrival.data(), seed_, pattern_.data());
    patternDirty_ = false;
}

void Arpeggiator::removeNote(int index)
{
    for (int i = index; i + 1 < count_; ++i) notes_[(size_t) i] = notes_[(size_t) i + 1];
    --count_;
    notesChanged();
    if (count_ == 0) nextFree_ = -1;
}

void Arpeggiator::dropLatched()
{
    for (int i = count_ - 1; i >= 0; --i)
        if (! notes_[(size_t) i].physical) removeNote(i);
}

void Arpeggiator::addNote(int pitch, int velocity, int channel)
{
    const int64_t at = clock_;
    const bool hold = value(Hold) > 0.5f;
    int existing = -1;
    bool anyPhysical = false;
    for (int i = 0; i < count_; ++i)
    {
        anyPhysical |= notes_[(size_t) i].physical;
        if (notes_[(size_t) i].pitch == pitch && notes_[(size_t) i].channel == channel) existing = i;
    }
    bool fresh = count_ == 0;
    if (hold && ! anyPhysical && count_ > 0)
    {
        // Hold: the first key of a new gesture replaces the latched notes.
        count_ = 0; existing = -1; fresh = true; notesChanged();
    }
    else if (hold && existing >= 0 && ! notes_[(size_t) existing].physical)
    {
        removeNote(existing); // playing a latched note again removes it
        return;
    }
    if (existing >= 0)
    {
        notes_[(size_t) existing].physical = true;
        notes_[(size_t) existing].velocity = (uint8_t) velocity;
        return;
    }
    if (count_ >= kMaxNotes) return;
    int insert = 0;
    while (insert < count_ && (notes_[(size_t) insert].pitch < pitch
                               || (notes_[(size_t) insert].pitch == pitch && notes_[(size_t) insert].channel < channel)))
        ++insert;
    for (int i = count_; i > insert; --i) notes_[(size_t) i] = notes_[(size_t) i - 1];
    notes_[(size_t) insert] = {(uint8_t) pitch, (uint8_t) velocity, (uint8_t) channel, true, arrivals_++};
    ++count_;
    notesChanged();
    if (fresh)
    {
        stepCounter_ = 0;
        retriggerPending_ = false;
        velocityStart_ = at;
        if (! gridMode_ && nextFree_ < 0) nextFree_ = at; // a free clock starts with the first note
    }
    else if (std::lround(value(Retrigger)) == 1)
        retriggerPending_ = true;
}

void Arpeggiator::handleEvent(const juce::MidiMessageMetadata& meta)
{
    const auto* d = meta.data;
    const int status = d[0] & 0xf0, channel = d[0] & 15;
    const bool on = meta.numBytes == 3 && status == 0x90 && d[2] != 0;
    const bool off = meta.numBytes == 3 && (status == 0x80 || (status == 0x90 && d[2] == 0));
    if (on || off)
    {
        if (d[1] > 127 || d[2] > 127) return;
        if (on) { addNote(d[1], d[2], channel); return; }
        for (int i = 0; i < count_; ++i)
            if (notes_[(size_t) i].pitch == d[1] && notes_[(size_t) i].channel == channel)
            {
                if (value(Hold) > 0.5f) notes_[(size_t) i].physical = false;
                else removeNote(i);
                break;
            }
        return;
    }
    if (meta.numBytes == 3 && status == 0xb0 && (d[1] == 123 || d[1] == 120))
    {
        // All Notes Off releases the keys (Hold keeps its latch); All Sound Off also forgets the latch.
        for (int i = count_ - 1; i >= 0; --i)
        {
            notes_[(size_t) i].physical = false;
            if (d[1] == 120 || value(Hold) <= 0.5f) removeNote(i);
        }
    }
    emit(d, meta.numBytes, clock_);
}

int64_t Arpeggiator::nextGridStep()
{
    const double step = syncedRateBeats((int) std::lround(value(SyncedRate)));
    const int groove = (int) std::lround(value(Groove));
    const double end = blockBeat_ + blockLength_ / samplesPerBeat_;
    for (;; ++gridCursor_)
    {
        const double nominal = (double) gridCursor_ * step;
        const double beat = grooveWarp(nominal, groove);
        if (beat >= end - 1e-12) return INT64_MAX;
        if (beat < windowBeat_ - 1e-9 || beat <= lastStepBeat_ + 1e-9) continue;
        // Same rounding as the clip player, so a clip note on the grid arrives no later than its step.
        const double position = std::floor((beat - blockBeat_) * samplesPerBeat_ + 1e-8);
        const int64_t at = blockStart_ + (int64_t) std::clamp(position, 0.0, (double) std::max(0, blockLength_ - 1));
        if (at < clock_) continue; // before the note that started the pattern: that step had nothing to play
        pendingBeat_ = beat;
        pendingNominal_ = nominal;
        return at;
    }
}

void Arpeggiator::runUntil(int64_t limit)
{
    for (;;)
    {
        int offIndex = -1;
        int64_t off = INT64_MAX;
        for (int i = 0; i < numSounding_; ++i)
            if (sounding_[(size_t) i].offAt < off) { off = sounding_[(size_t) i].offAt; offIndex = i; }
        int64_t step = INT64_MAX;
        if (count_ > 0)
            step = gridMode_ ? nextGridStep() : (nextFree_ >= 0 ? nextFree_ : INT64_MAX);
        const int64_t t = std::min(off, step);
        if (t >= limit) return;
        if (off <= step)
        {
            noteOff(offIndex, off);
            continue;
        }
        if (gridMode_)
        {
            if (std::lround(value(Retrigger)) == 2)
            {
                const double interval = retriggerIntervalBeats((int) std::lround(value(RetriggerInterval)));
                if (std::floor(pendingNominal_ / interval + 1e-9) != std::floor(lastStepNominal_ / interval + 1e-9))
                    retriggerPending_ = true;
            }
            lastStepBeat_ = pendingBeat_;
            lastStepNominal_ = pendingNominal_;
            ++gridCursor_;
        }
        else
            nextFree_ = step + std::max<int64_t>(1, (int64_t) std::llround(stepSamples()));
        fireStep(step);
    }
}

void Arpeggiator::fireStep(int64_t at)
{
    lastStep_ = at;
    if (count_ <= 0) return;
    if (retriggerPending_)
    {
        retriggerPending_ = false;
        stepCounter_ = 0;
        if (value(VelocityRetrigger) > 0.5f) velocityStart_ = at;
    }
    const int style = (int) std::lround(value(Style));
    if (patternDirty_) rebuildPattern();
    const int len = patternLength_;
    if (len <= 0) return;
    const int distance = (int) std::lround(value(TransposeDistance));
    const int passes = distance == 0 ? 1 : 1 + (int) std::lround(value(TransposeSteps));
    const int64_t cycle = (int64_t) len * passes;
    const int repeats = (int) std::lround(value(Repeats));
    if (repeats > 0 && stepCounter_ >= cycle * repeats)
        return; // the pattern has played its repeats; the next retrigger or new chord starts it again
    const int64_t pos = stepCounter_ % len;
    const int pass = (int) ((stepCounter_ / len) % passes);
    if (style == RandomOther && pos == 0 && stepCounter_ > 0)
        rebuildPattern(); // a fresh order every cycle
    ++stepCounter_;

    const int mode = (int) std::lround(value(TransposeMode));
    const int key = (int) std::lround(value(TransposeKey));
    const double gate = std::max(1.0, value(Gate) * 0.01 * stepSamples());
    // Velocity ramp: from the played velocity to Target over Decay, counted from the start (or last retrigger).
    const bool velocityRamp = value(VelocityOn) > 0.5f;
    const float ramp = velocityRamp ? (float) std::clamp((double) (at - velocityStart_) * 1000.0 / sampleRate_
                                                             / std::max(1.0f, value(VelocityDecay)), 0.0, 1.0)
                                    : 0.0f;
    auto play = [&](int index) {
        const auto& note = notes_[(size_t) index];
        int pitch = note.pitch;
        if (pass > 0)
            pitch = mode <= 0 || mode > (int) model::kScales.size()
                        ? pitch + pass * distance
                        : model::transposeInScale(pitch, pass * distance, key, model::kScales[(size_t) mode - 1]);
        if (pitch < 0 || pitch > 127) return;
        int velocity = note.velocity;
        if (velocityRamp)
            velocity = (int) std::lround((float) velocity + ((float) value(VelocityTarget) - (float) velocity) * ramp);
        velocity = std::clamp(velocity, 1, 127);
        for (int i = 0; i < numSounding_; ++i) // a long gate still holds this pitch: end it first
            if (sounding_[(size_t) i].pitch == pitch && sounding_[(size_t) i].channel == note.channel)
            {
                noteOff(i, at);
                break;
            }
        if (numSounding_ >= kMaxPattern) noteOff(0, at);
        const uint8_t message[]{(uint8_t) (0x90 | note.channel), (uint8_t) pitch, (uint8_t) velocity};
        emit(message, 3, at);
        sounding_[(size_t) numSounding_++] = {(uint8_t) pitch, note.channel, at + std::max<int64_t>(1, (int64_t) std::llround(gate))};
    };
    if (style == ChordTrigger)
    {
        for (int i = 0; i < count_; ++i) play(i);
        return;
    }
    if (style == Random)
    {
        play((int) (nextRandom(seed_) % (uint32_t) count_));
        return;
    }
    const int offset = (int) std::lround(value(Offset));
    const int slot = (int) (((pos + offset) % len + len) % len);
    play(std::clamp(pattern_[(size_t) slot], 0, count_ - 1));
}

void Arpeggiator::process(juce::AudioBuffer<float>& audio, juce::MidiBuffer& midi)
{
    scratch_.clear();
    bytes_ = 0;
    overflow_ = false;
    blockStart_ = clock_;
    blockLength_ = audio.getNumSamples();
    const int64_t blockEnd = clock_ + blockLength_;
    if (blockLength_ <= 0) return;

    if (isBypassed())
    {
        if (! wasBypassed_)
        {
            while (numSounding_ > 0) noteOff(numSounding_ - 1, clock_);
            count_ = 0;
            nextFree_ = -1;
            notesChanged();
            wasBypassed_ = true;
        }
        for (const auto meta : midi)
        {
            const int status = meta.data[0] & 0xf0, channel = meta.data[0] & 15;
            if (meta.numBytes == 3 && (status == 0x80 || status == 0x90) && meta.data[1] < 128)
                passedThrough_[(size_t) channel][meta.data[1]] = status == 0x90 && meta.data[2] != 0;
            emit(meta.data, meta.numBytes, clock_ + meta.samplePosition);
        }
    }
    else
    {
        if (wasBypassed_)
        {
            // Keys that went through unchanged while bypassed: their note-offs will now be consumed.
            for (int ch = 0; ch < 16; ++ch)
                for (int note = 0; note < 128; ++note)
                    if (passedThrough_[(size_t) ch][(size_t) note])
                    {
                        const uint8_t message[]{(uint8_t) (0x80 | ch), (uint8_t) note, 64};
                        emit(message, 3, clock_);
                    }
            passedThrough_ = {};
            wasBypassed_ = false;
        }
        if (value(Hold) <= 0.5f) dropLatched();

        samplesPerBeat_ = sampleRate_ * 60.0 / std::max(1.0, host_.tempo);
        const bool grid = value(Sync) > 0.5f && host_.playing;
        if (grid)
        {
            blockBeat_ = host_.beats;
            const bool continuous = std::abs(blockBeat_ - expectedBeat_) < 1e-6;
            windowBeat_ = continuous ? std::min(blockBeat_, expectedBeat_) : blockBeat_;
            if (! continuous) lastStepBeat_ = lastStepNominal_ = -1e30; // located or looped: the grid starts over
            const double step = syncedRateBeats((int) std::lround(value(SyncedRate)));
            gridCursor_ = (int64_t) std::floor(windowBeat_ / step) - 2;
            expectedBeat_ = blockBeat_ + blockLength_ / samplesPerBeat_;
            nextFree_ = -1;
        }
        else
        {
            expectedBeat_ = -1e30;
            if (gridMode_ && count_ > 0) // transport stopped: keep the step rhythm going on the free clock
                nextFree_ = lastStep_ >= 0 ? std::max(clock_, lastStep_ + (int64_t) std::llround(stepSamples())) : clock_;
        }
        gridMode_ = grid;

        for (const auto meta : midi)
        {
            const int64_t at = blockStart_ + std::clamp(meta.samplePosition, 0, std::max(0, blockLength_ - 1));
            runUntil(at);
            clock_ = at;
            handleEvent(meta);
        }
        runUntil(blockEnd);
    }

    if (overflow_)
    {
        scratch_.clear();
        reset();
        for (int ch = 0; ch < 16; ++ch)
        {
            const uint8_t message[]{(uint8_t) (0xb0 | ch), 120, 0};
            scratch_.addEvent(message, 3, 0);
        }
    }
    clock_ = blockEnd;
    midi.clear();
    midi.addEvents(scratch_, 0, -1, 0);
}

} // namespace sonora::devices
