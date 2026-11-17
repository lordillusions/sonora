#include "devices/MidiGenerator.h"

#include <algorithm>
#include <cmath>

namespace sonora::devices
{
namespace
{
enum EventKind : uint8_t { NoteEvent, CcEvent, BendEvent };
constexpr int kForceReserve = 8192; // room past kByteLimit for the note-offs that end a block
} // namespace

// ---------------------------------------------------------------------------------------------------------------- Sink
void MidiGenerator::Sink::push(const Event& e)
{
    if (! (e.beat >= from_ && e.beat < to_)) { ++dropped_; return; } // also rejects NaN
    if (count_ >= kMaxEvents) { ++dropped_; return; }
    events_[(size_t) count_] = e;
    events_[(size_t) count_].order = count_;
    ++count_;
}

void MidiGenerator::Sink::note(double beat, double lengthBeats, int pitch, int velocity, int channel)
{
    if (pitch < 0 || pitch > 127 || ! std::isfinite(lengthBeats)) { ++dropped_; return; }
    Event e;
    e.kind = NoteEvent;
    e.beat = beat;
    e.length = std::max(0.0, lengthBeats);
    e.data1 = (uint8_t) pitch;
    e.data2 = (uint8_t) std::clamp(velocity, 1, 127);
    e.channel = (uint8_t) std::clamp(channel, 0, 15);
    push(e);
}

void MidiGenerator::Sink::cc(double beat, int number, int value, int channel)
{
    if (number < 0 || number > 127) { ++dropped_; return; }
    Event e;
    e.kind = CcEvent;
    e.beat = beat;
    e.data1 = (uint8_t) number;
    e.data2 = (uint8_t) std::clamp(value, 0, 127);
    e.channel = (uint8_t) std::clamp(channel, 0, 15);
    push(e);
}

void MidiGenerator::Sink::bend(double beat, int value14, int channel)
{
    Event e;
    e.kind = BendEvent;
    e.beat = beat;
    e.value14 = (uint16_t) std::clamp(value14, 0, 16383);
    e.channel = (uint8_t) std::clamp(channel, 0, 15);
    push(e);
}

// ------------------------------------------------------------------------------------------------------------ device
MidiGenerator::MidiGenerator(std::vector<ParameterInfo> parameters) : NativeDevice(std::move(parameters))
{
}

bool MidiGenerator::loadState(const uint8_t*, size_t, std::string&)
{
    return true;
}

std::unique_ptr<MidiGenerator> MidiGenerator::cloneForRender() const
{
    auto copy = createEmpty();
    if (! copy) return nullptr;
    for (int i = 0; i < (int) parameters().size(); ++i) copy->setParameter(i, getParameter(i));
    if (! stateFormat().empty())
    {
        const auto bytes = saveState();
        std::string error;
        copy->loadState(bytes.data(), bytes.size(), error);
    }
    return copy;
}

void MidiGenerator::prepare(double sampleRate, int)
{
    sampleRate_ = sampleRate > 0 ? sampleRate : 44100.0;
    scratch_.ensureSize((size_t) (kByteLimit + kForceReserve));
    reset();
}

void MidiGenerator::reset()
{
    heldCount_ = 0;
    numSounding_ = 0;
    bent_ = {};
    passedThrough_ = {};
    wasPlaying_ = freeRunning_ = wrapped_ = false;
    wrapSample_ = -1;
    cursorBeat_ = expectedBeat_ = freeBeat_ = segmentBeat_ = 0;
}

// ------------------------------------------------------------------------------------------------------------ output
void MidiGenerator::emit(const uint8_t* data, int size, int sample, bool force)
{
    if (! force && bytes_ + size + 8 > kByteLimit)
    {
        overflow_ = true;
        return;
    }
    scratch_.addEvent(data, size, std::clamp(sample, 0, std::max(0, blockLength_ - 1)));
    bytes_ += size + 8;
}

int MidiGenerator::sampleOf(double beat) const
{
    // Same rounding as the clip player: a note on the grid arrives on the sample at or just after its beat.
    const double position = std::floor((beat - segmentBeat_) * samplesPerBeat_ + 1e-8);
    return segmentSample_ + (int) std::clamp(position, 0.0, (double) std::max(0, blockLength_ - 1 - segmentSample_));
}

void MidiGenerator::endNote(int index, int sample, double beat)
{
    auto& s = sounding_[(size_t) index];
    const uint8_t message[]{(uint8_t) (0x80 | s.channel), s.pitch, 64};
    emit(message, 3, sample, true);
    if (record_ != nullptr && s.record >= 0 && (size_t) s.record < record_->size())
    {
        auto& note = (*record_)[(size_t) s.record];
        note.duration = std::max(1e-6, std::min(note.duration, beat - note.start));
    }
    s = sounding_[(size_t) --numSounding_];
}

void MidiGenerator::allNotesOff(int sample, double beat)
{
    while (numSounding_ > 0) endNote(numSounding_ - 1, sample, beat);
    for (int ch = 0; ch < 16; ++ch)
        if (bent_[(size_t) ch])
        {
            const uint8_t message[]{(uint8_t) (0xE0 | ch), 0, 64};
            emit(message, 3, sample, true);
            bent_[(size_t) ch] = false;
        }
}

void MidiGenerator::releaseSounding()
{
    allNotesOff(std::clamp(cursorSample_, 0, std::max(0, blockLength_ - 1)), cursorBeat_);
}

void MidiGenerator::flushOffs(double limit, bool inclusive)
{
    for (;;)
    {
        int index = -1;
        for (int i = 0; i < numSounding_; ++i)
            if (index < 0 || sounding_[(size_t) i].offBeat < sounding_[(size_t) index].offBeat) index = i;
        if (index < 0) return;
        const double off = sounding_[(size_t) index].offBeat;
        if (inclusive ? off > limit : off >= limit) return;
        endNote(index, sampleOf(off), off);
    }
}

void MidiGenerator::emitNote(const Sink::Event& e)
{
    const int sample = sampleOf(e.beat);
    for (int i = 0; i < numSounding_; ++i) // retrigger: the pitch is still sounding on this channel
        if (sounding_[(size_t) i].pitch == e.data1 && sounding_[(size_t) i].channel == e.channel)
        {
            endNote(i, sample, e.beat);
            break;
        }
    if (numSounding_ >= kMaxSounding)
    {
        int earliest = 0;
        for (int i = 1; i < numSounding_; ++i)
            if (sounding_[(size_t) i].offBeat < sounding_[(size_t) earliest].offBeat) earliest = i;
        endNote(earliest, sample, e.beat);
    }
    const uint8_t message[]{(uint8_t) (0x90 | e.channel), e.data1, e.data2};
    emit(message, 3, sample);
    auto& s = sounding_[(size_t) numSounding_++];
    s.pitch = e.data1;
    s.channel = e.channel;
    s.startBeat = e.beat;
    s.offBeat = e.beat + std::max(e.length, 1.0 / samplesPerBeat_);
    s.record = -1;
    if (record_ != nullptr)
    {
        model::MidiNote note;
        note.start = e.beat;
        note.duration = s.offBeat - e.beat;
        note.pitch = e.data1;
        note.velocity = (float) e.data2;
        s.record = (int) record_->size();
        record_->push_back(note);
    }
}

void MidiGenerator::window(double from, double to)
{
    if (! (to > from)) return;
    const bool generating = host_.playing || runsWhenStopped();
    sink_.count_ = 0;
    sink_.from_ = from;
    sink_.to_ = to;
    const int droppedBefore = sink_.dropped_;
    if (generating) generate(from, to, sink_);
    if (sink_.dropped_ != droppedBefore) dropped_.fetch_add(sink_.dropped_ - droppedBefore, std::memory_order_relaxed);
    auto* first = sink_.events_.data();
    std::sort(first, first + sink_.count_, [](const Sink::Event& a, const Sink::Event& b) {
        return a.beat < b.beat || (! (b.beat < a.beat) && a.order < b.order);
    });
    for (int i = 0; i < sink_.count_; ++i)
    {
        const auto& e = sink_.events_[(size_t) i];
        flushOffs(e.beat, true);
        if (e.kind == NoteEvent) emitNote(e);
        else if (e.kind == CcEvent)
        {
            const uint8_t message[]{(uint8_t) (0xB0 | e.channel), e.data1, e.data2};
            emit(message, 3, sampleOf(e.beat));
        }
        else
        {
            const uint8_t message[]{(uint8_t) (0xE0 | e.channel), (uint8_t) (e.value14 & 0x7F), (uint8_t) (e.value14 >> 7)};
            emit(message, 3, sampleOf(e.beat));
            bent_[e.channel] = e.value14 != 8192;
        }
    }
    flushOffs(to, false);
}

void MidiGenerator::runTo(int target)
{
    while (cursorSample_ < target)
    {
        int end = target;
        bool wrapHere = false;
        if (wrapSample_ > 0 && ! wrapped_ && cursorSample_ < wrapSample_ && target >= wrapSample_)
        {
            end = wrapSample_;
            wrapHere = true;
        }
        if (host_.playing || freeRunning_)
        {
            const double toBeat = wrapHere ? host_.loopEnd : segmentBeat_ + (end - segmentSample_) / samplesPerBeat_;
            window(cursorBeat_, toBeat);
            cursorBeat_ = std::max(cursorBeat_, toBeat);
        }
        cursorSample_ = end;
        if (wrapHere)
        {
            allNotesOff(wrapSample_, host_.loopEnd);
            wrapped_ = true;
            segmentSample_ = wrapSample_;
            segmentBeat_ = cursorBeat_ = host_.loopStart;
            located(host_.loopStart);
        }
    }
    if (! host_.playing && freeRunning_ && ! runsWhenStopped() && numSounding_ == 0) freeRunning_ = false;
}

// ------------------------------------------------------------------------------------------------------------- input
void MidiGenerator::removeHeld(int index)
{
    for (int i = index; i + 1 < heldCount_; ++i) held_[(size_t) i] = held_[(size_t) i + 1];
    --heldCount_;
}

void MidiGenerator::dropLatched()
{
    bool changed = false;
    for (int i = heldCount_ - 1; i >= 0; --i)
        if (! held_[(size_t) i].physical)
        {
            removeHeld(i);
            changed = true;
        }
    if (changed) heldChanged();
}

void MidiGenerator::addHeld(int pitch, int velocity, int channel)
{
    const bool latch = latchOn();
    bool anyPhysical = false;
    int existing = -1;
    for (int i = 0; i < heldCount_; ++i)
    {
        anyPhysical |= held_[(size_t) i].physical;
        if (held_[(size_t) i].pitch == pitch && held_[(size_t) i].channel == channel) existing = i;
    }
    if (latch && ! anyPhysical && heldCount_ > 0)
    {
        heldCount_ = 0; // a new gesture replaces the latched notes
        existing = -1;
    }
    if (existing >= 0)
    {
        held_[(size_t) existing].physical = true;
        held_[(size_t) existing].velocity = (uint8_t) velocity;
        heldChanged();
        return;
    }
    if (heldCount_ >= kMaxHeld) return;
    int insert = 0;
    while (insert < heldCount_ && (held_[(size_t) insert].pitch < pitch
                                   || (held_[(size_t) insert].pitch == pitch && held_[(size_t) insert].channel < channel)))
        ++insert;
    for (int i = heldCount_; i > insert; --i) held_[(size_t) i] = held_[(size_t) i - 1];
    held_[(size_t) insert] = {(uint8_t) pitch, (uint8_t) velocity, (uint8_t) channel, true, arrivals_++};
    ++heldCount_;
    heldChanged();
}

void MidiGenerator::startFreeClock(int sample)
{
    freeRunning_ = true;
    segmentSample_ = cursorSample_ = sample;
    segmentBeat_ = cursorBeat_ = 0.0;
    located(0.0);
}

void MidiGenerator::handleInput(const juce::MidiMessageMetadata& meta, int sample)
{
    const auto* d = meta.data;
    const int status = d[0] & 0xF0, channel = d[0] & 15;
    const bool on = meta.numBytes == 3 && status == 0x90 && d[2] != 0;
    const bool off = meta.numBytes == 3 && (status == 0x80 || (status == 0x90 && d[2] == 0));
    if (on || off)
    {
        if (d[1] > 127 || d[2] > 127) return;
        if (on)
        {
            addHeld(d[1], d[2], channel);
            if (! host_.playing && ! freeRunning_ && runsWhenStopped()) startFreeClock(sample);
            return;
        }
        for (int i = 0; i < heldCount_; ++i)
            if (held_[(size_t) i].pitch == d[1] && held_[(size_t) i].channel == channel)
            {
                if (latchOn()) held_[(size_t) i].physical = false;
                else removeHeld(i);
                heldChanged();
                break;
            }
        return;
    }
    if (meta.numBytes == 3 && status == 0xB0 && (d[1] == 123 || d[1] == 120))
    {
        // All Notes Off releases the keys (latch keeps them); All Sound Off also forgets the latch.
        for (int i = heldCount_ - 1; i >= 0; --i)
        {
            held_[(size_t) i].physical = false;
            if (d[1] == 120 || ! latchOn()) removeHeld(i);
        }
        heldChanged();
    }
    emit(d, meta.numBytes, sample);
}

// ----------------------------------------------------------------------------------------------------------- process
void MidiGenerator::process(juce::AudioBuffer<float>& audio, juce::MidiBuffer& midi)
{
    scratch_.clear();
    bytes_ = 0;
    overflow_ = false;
    blockLength_ = audio.getNumSamples();
    blockStarted();
    if (blockLength_ <= 0) return;
    samplesPerBeat_ = sampleRate_ * 60.0 / std::max(1.0, host_.tempo);

    if (isBypassed())
    {
        if (! wasBypassed_)
        {
            segmentSample_ = 0;
            allNotesOff(0, cursorBeat_);
            heldCount_ = 0;
            heldChanged();
            wasBypassed_ = true;
            wasPlaying_ = freeRunning_ = false;
        }
        for (const auto meta : midi)
        {
            const int status = meta.data[0] & 0xF0, channel = meta.data[0] & 15;
            if (meta.numBytes == 3 && (status == 0x80 || status == 0x90) && meta.data[1] < 128)
                passedThrough_[(size_t) channel][meta.data[1]] = status == 0x90 && meta.data[2] != 0;
            emit(meta.data, meta.numBytes, meta.samplePosition);
        }
    }
    else
    {
        if (wasBypassed_)
        {
            // Keys that went through unchanged while bypassed: their note-offs would now be consumed.
            for (int ch = 0; ch < 16; ++ch)
                for (int note = 0; note < 128; ++note)
                    if (passedThrough_[(size_t) ch][(size_t) note])
                    {
                        const uint8_t message[]{(uint8_t) (0x80 | ch), (uint8_t) note, 64};
                        emit(message, 3, 0, true);
                    }
            passedThrough_ = {};
            wasBypassed_ = false;
        }
        if (! latchOn()) dropLatched();

        cursorSample_ = segmentSample_ = 0;
        wrapSample_ = -1;
        wrapped_ = false;
        if (host_.playing)
        {
            const double start = host_.beats;
            const bool continuous = wasPlaying_ && std::abs(start - expectedBeat_) <= 1.0 / samplesPerBeat_;
            segmentBeat_ = start;
            if (! continuous)
            {
                allNotesOff(0, cursorBeat_);
                freeRunning_ = false;
                cursorBeat_ = start;
                located(start);
            }
            const double end = start + blockLength_ / samplesPerBeat_;
            if (host_.looping && host_.loopEnd > host_.loopStart + 1e-9 && start < host_.loopEnd - 1e-9 && end > host_.loopEnd + 1e-9)
            {
                const int at = (int) std::ceil((host_.loopEnd - start) * samplesPerBeat_ - 1e-8);
                if (at >= 1 && at < blockLength_) wrapSample_ = at;
            }
        }
        else
        {
            if (wasPlaying_)
            {
                allNotesOff(0, cursorBeat_);
                freeRunning_ = false;
            }
            segmentBeat_ = freeBeat_;
            if (! freeRunning_ && runsWhenStopped()) startFreeClock(0); // latched notes from before the stop
        }
        wasPlaying_ = host_.playing;

        for (const auto meta : midi)
        {
            const int at = std::clamp(meta.samplePosition, 0, blockLength_ - 1);
            runTo(at);
            handleInput(meta, at);
        }
        runTo(blockLength_);
        const double endBeat = segmentBeat_ + (blockLength_ - segmentSample_) / samplesPerBeat_;
        if (host_.playing) expectedBeat_ = endBeat;
        else freeBeat_ = freeRunning_ ? endBeat : 0.0;
        playBeat_.store(host_.playing || freeRunning_ ? endBeat : host_.beats, std::memory_order_relaxed);
    }
    playing_.store(host_.playing, std::memory_order_relaxed);

    if (overflow_)
    {
        // Over the cap: drop this block's output, end whatever sounds, start over at the next block.
        scratch_.clear();
        bytes_ = 0;
        segmentSample_ = 0;
        allNotesOff(0, cursorBeat_);
        wasPlaying_ = freeRunning_ = false;
        dropped_.fetch_add(1, std::memory_order_relaxed);
    }
    midi.clear();
    midi.addEvents(scratch_, 0, -1, 0);
}

// ----------------------------------------------------------------------------------------------------- offline render
std::vector<model::MidiNote> MidiGenerator::renderNotes(const std::vector<model::MidiNote>& input, double fromBeat,
                                                       double toBeat, double tempo, double rate, int blockSize) const
{
    std::vector<model::MidiNote> out;
    if (! (toBeat > fromBeat)) return out;
    auto clone = cloneForRender();
    if (! clone) return out;
    rate = rate > 0 ? rate : 48000.0;
    blockSize = std::max(16, blockSize);
    tempo = std::max(1.0, tempo);
    clone->prepare(rate, blockSize);
    clone->record_ = &out;
    const double perBeat = rate * 60.0 / tempo;
    const auto total = (int64_t) std::ceil((toBeat - fromBeat) * perBeat - 1e-8);

    struct In { int64_t at; bool on; int pitch, velocity; };
    std::vector<In> events;
    auto sampleAt = [&](double beat) { return (int64_t) std::floor((beat - fromBeat) * perBeat + 1e-8); };
    for (const auto& n : input)
    {
        if (! n.enabled || n.start + n.duration <= fromBeat || n.start >= toBeat) continue;
        const int velocity = std::clamp((int) std::lround(n.velocity), 1, 127);
        events.push_back({std::max<int64_t>(0, sampleAt(n.start)), true, std::clamp(n.pitch, 0, 127), velocity});
        if (n.start + n.duration < toBeat) events.push_back({sampleAt(n.start + n.duration), false, std::clamp(n.pitch, 0, 127), 0});
    }
    std::stable_sort(events.begin(), events.end(), [](const In& a, const In& b) { return a.at < b.at || (a.at == b.at && ! a.on && b.on); });

    juce::AudioBuffer<float> audio(2, blockSize);
    juce::MidiBuffer midi;
    midi.ensureSize(4096);
    HostInfo info;
    info.tempo = tempo;
    info.playing = true;
    info.sampleRate = rate;
    size_t next = 0;
    for (int64_t clock = 0; clock < total; clock += blockSize)
    {
        const int n = (int) std::min<int64_t>(blockSize, total - clock);
        juce::AudioBuffer<float> view(audio.getArrayOfWritePointers(), 2, n);
        view.clear();
        midi.clear();
        for (; next < events.size() && events[next].at < clock + n; ++next)
        {
            const auto& e = events[next];
            const auto message = e.on ? juce::MidiMessage::noteOn(1, e.pitch, (juce::uint8) e.velocity) : juce::MidiMessage::noteOff(1, e.pitch);
            midi.addEvent(message, (int) std::max<int64_t>(0, e.at - clock));
        }
        info.beats = fromBeat + (double) clock / perBeat;
        clone->setHostInfo(info);
        clone->process(view, midi);
    }
    clone->record_ = nullptr;
    // The last block may reach a fraction of a sample past toBeat: a step exactly there belongs to the next span.
    out.erase(std::remove_if(out.begin(), out.end(), [toBeat](const model::MidiNote& n) { return n.start >= toBeat - 1e-9; }), out.end());
    for (auto& note : out) note.duration = std::max(1e-6, std::min(note.duration, toBeat - note.start));
    std::stable_sort(out.begin(), out.end(), [](const model::MidiNote& a, const model::MidiNote& b) {
        return a.start < b.start || (! (b.start < a.start) && a.pitch < b.pitch);
    });
    return out;
}
} // namespace sonora::devices
