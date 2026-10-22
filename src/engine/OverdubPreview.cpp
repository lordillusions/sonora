#include "engine/OverdubPreview.h"
#include "core/TimeUtils.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace sonora::engine
{
namespace
{
constexpr double infinity = std::numeric_limits<double>::infinity();
}
OverdubPreview::OverdubPreview(int track, int scene, const model::Clip& clip)
    : clip_(prepareSessionClip(clip, std::shared_ptr<SampleSlot>{})), track_(track), scene_(scene), uid_(clip.uid),
      originalNotes_(clip_.notes.size())
{
    const size_t capacity = originalNotes_ + maxRecordedNotes;
    clip_.notes.reserve(capacity);
    earliestOn_.reserve(capacity); disabledFrom_.reserve(capacity); inputStarts_.reserve(capacity);
    earliestOn_.assign(originalNotes_, -infinity); disabledFrom_.assign(originalNotes_, infinity);
    inputStarts_.assign(originalNotes_, -infinity);
    held_.fill(-1);
    originalControllers_ = std::move(clip_.midiControllers);
    clip_.midiControllers.clear();
    controllerNodes_.resize(maxControllerNodes);
    for (int i = 0; i < maxControllerNodes; ++i) controllerNodes_[(size_t) i].next = i + 1 < maxControllerNodes ? i + 1 : -1;
    for (size_t i = 0; i < originalControllers_.size(); ++i)
    {
        const int index = originalControllers_[i].index;
        controllers_[(size_t) index].originalLane = (int) i;
        if (! controllerPresent_[(size_t) index])
        { controllerPresent_[(size_t) index] = true; controllerIndices_[(size_t) numControllerIndices_++] = index; }
    }
}

int OverdubPreview::appendNote(int key, int velocity, double elapsed)
{
    if (clip_.notes.size() == originalNotes_ + maxRecordedNotes) { full_ = true; return -1; }
    const auto& loop = clip_.loop;
    const double span = loop.loopEnd - loop.loopStart;
    model::MidiNote note;
    note.start = model::clipContentPosition(elapsed, 0, loop.loopStart, loop.loopEnd, loop.startRelative, loop.enabled);
    note.pitch = key % 128; note.velocity = (float) velocity;
    note.duration = 0; // unpublished until closed: original/live held voices stay independent
    const int index = (int) clip_.notes.size();
    clip_.notes.push_back(note);
    earliestOn_.push_back(loop.enabled && span > 0 && note.start >= loop.loopStart ? elapsed + span : infinity);
    disabledFrom_.push_back(infinity); inputStarts_.push_back(elapsed);
    held_[(size_t) key] = index; changed_ = true;
    return index;
}

void OverdubPreview::closeNote(int index, double elapsed, int velocity)
{
    if (index < 0) return;
    auto& note = clip_.notes[(size_t) index];
    const auto& loop = clip_.loop; const double span = loop.loopEnd - loop.loopStart;
    note.duration = std::max(1e-9, elapsed - inputStarts_[(size_t) index]);
    if (loop.enabled && span > 0) note.duration = std::min(note.duration, span);
    note.releaseVelocity = (float) velocity;
    // Do not replay a note before its captured endpoint is known, including multiple wraps in one block.
    earliestOn_[(size_t) index] = std::max(earliestOn_[(size_t) index], elapsed);
    for (size_t old = 0; old < (size_t) index; ++old)
    {
        const auto& candidate = clip_.notes[old];
        if (candidate.pitch != note.pitch) continue;
        const bool overlap = candidate.start < note.start + note.duration && candidate.start + candidate.duration > note.start;
        const bool wrap = loop.enabled && note.start + note.duration > loop.loopEnd
                       && candidate.start < loop.loopStart + note.start + note.duration - loop.loopEnd;
        if (overlap || wrap) disabledFrom_[old] = std::min(disabledFrom_[old], inputStarts_[(size_t) index]);
    }
}

void OverdubPreview::stop(double elapsed, int sample, juce::MidiBuffer& out)
{
    if (stopped_) return;
    stopped_ = true;
    for (size_t key = 0; key < held_.size(); ++key)
        if (held_[key] >= 0)
        {
            closeNote(held_[key], elapsed, 0);
            if (out.data.size() + 9 <= midiControllerByteLimit)
                out.addEvent(juce::MidiMessage::noteOff((int) key / 128 + 1, (int) key % 128), sample);
            else full_ = true;
            held_[key] = -1;
        }
}

int OverdubPreview::newControllerNode(double phase, int value, bool original)
{
    if (freeController_ < 0) { full_ = true; return -1; }
    const int index = freeController_;
    freeController_ = controllerNodes_[(size_t) index].next;
    controllerNodes_[(size_t) index] = {phase, -1, value, original};
    return index;
}

void OverdubPreview::captureController(int index, int value, double phase)
{
    auto& state = controllers_[(size_t) index];
    if (! controllerPresent_[(size_t) index])
    { controllerPresent_[(size_t) index] = true; controllerIndices_[(size_t) numControllerIndices_++] = index; }
    if (state.tail >= 0 && std::abs(controllerNodes_[(size_t) state.tail].phase - phase) < 1e-10)
    { controllerNodes_[(size_t) state.tail].value = value; return; }
    const int node = newControllerNode(phase, value, false);
    if (node < 0) return;
    if (state.pending < 0) { state.pending = node; state.first = phase; }
    else controllerNodes_[(size_t) state.tail].next = node;
    state.tail = node; changed_ = true;
}

void OverdubPreview::commitControllers(double end)
{
    for (auto& state : controllers_)
    {
        if (state.pending < 0) continue;
        // Find the old state at the right edge before editing; original markers restore Float lanes.
        int previous = -1, node = state.head;
        bool restoreOriginal = true; int restoreValue = 0;
        while (node >= 0 && controllerNodes_[(size_t) node].phase <= end + 1e-10)
        {
            restoreOriginal = controllerNodes_[(size_t) node].original;
            restoreValue = controllerNodes_[(size_t) node].value;
            node = controllerNodes_[(size_t) node].next;
        }
        node = state.head;
        while (node >= 0 && controllerNodes_[(size_t) node].phase < state.first - 1e-10)
        { previous = node; node = controllerNodes_[(size_t) node].next; }
        while (node >= 0 && controllerNodes_[(size_t) node].phase <= end + 1e-10)
        {
            const int next = controllerNodes_[(size_t) node].next;
            controllerNodes_[(size_t) node].next = freeController_; freeController_ = node; node = next;
        }
        const int restore = newControllerNode(end, restoreValue, restoreOriginal);
        if (restore >= 0)
        {
            controllerNodes_[(size_t) restore].next = node;
            controllerNodes_[(size_t) state.tail].next = restore;
        }
        else controllerNodes_[(size_t) state.tail].next = node;
        if (previous < 0) state.head = state.pending;
        else controllerNodes_[(size_t) previous].next = state.pending;
        state.pending = state.tail = -1; state.cursor = -1; state.reading = false;
    }
}

int OverdubPreview::controllerValue(int index, double phase)
{
    auto& state = controllers_[(size_t) index];
    if (state.pending >= 0 && phase + 1e-10 >= state.first)
        return controllerNodes_[(size_t) state.tail].value; // live writing holds until this pass ends
    if (! state.reading)
    {
        state.cursor = -1; state.reading = true;
        for (int node = state.head; node >= 0 && controllerNodes_[(size_t) node].phase <= phase + 1e-10;
             node = controllerNodes_[(size_t) node].next) state.cursor = node;
    }
    else
    {
        int node = state.cursor >= 0 ? controllerNodes_[(size_t) state.cursor].next : state.head;
        while (node >= 0 && controllerNodes_[(size_t) node].phase <= phase + 1e-10)
        { state.cursor = node; node = controllerNodes_[(size_t) node].next; }
    }
    if (state.cursor >= 0 && ! controllerNodes_[(size_t) state.cursor].original)
        return controllerNodes_[(size_t) state.cursor].value;
    if (state.originalLane >= 0)
        return model::quantiseMidiController(index, originalControllers_[(size_t) state.originalLane].lane.valueAt(phase + 1e-10));
    return model::midiControllerDefault(index);
}

void OverdubPreview::finish(double elapsed, int sample, juce::MidiBuffer& out)
{
    stop(elapsed, sample, out);
    if (! controllersStopped_ && controllerStarted_)
    {
        const auto& loop = clip_.loop;
        const double phase = model::clipContentPosition(elapsed, 0, loop.loopStart, loop.loopEnd, loop.startRelative, loop.enabled);
        // A deferred stop exactly at a wrap closes the previous full pass, not an empty new one.
        commitControllers(phase < previousPhase_ ? loop.loopEnd : phase);
        controllersStopped_ = true;
    }
}

void OverdubPreview::renderControllers(const BlockSegment& seg, const juce::MidiBuffer& input, int channel,
                                      const BlockSegment* capture, bool stopAfterCapture, bool stopNow,
                                      juce::MidiBuffer& out, bool refresh, bool* overflow)
{
    const auto& loop = clip_.loop;
    auto event = input.begin(); const auto inputEnd = input.end();
    while (event != inputEnd && (*event).samplePosition < seg.sampleOffset) ++event;
    for (int sample = 0; sample < seg.numSamples; ++sample)
    {
        const int offset = seg.sampleOffset + sample;
        const double elapsed = seg.startBeats + sample / seg.samplesPerBeat;
        const double phase = model::clipContentPosition(elapsed, 0, loop.loopStart, loop.loopEnd, loop.startRelative, loop.enabled);
        const bool wrapped = sample == 0 ? phase + 1e-10 < previousPhase_ : phase < previousPhase_;
        if (wrapped)
        {
            commitControllers(loop.loopEnd);
            for (auto& state : controllers_) state.reading = false;
        }
        if (stopNow || (stopAfterCapture && capture && offset >= capture->sampleOffset + capture->numSamples))
        {
            if (! controllersStopped_ && controllerStarted_) commitControllers(phase);
            if (controllerStarted_) controllersStopped_ = true;
        }
        const bool recording = capture && ! controllersStopped_ && offset >= capture->sampleOffset && offset < capture->sampleOffset + capture->numSamples;
        if (recording && ! controllerStarted_)
        {
            controllerStarted_ = true;
            for (int index = 0; index < 130; ++index)
                if (controllers_[(size_t) index].input != -32768) captureController(index, controllers_[(size_t) index].input, phase);
        }
        while (event != inputEnd && (*event).samplePosition <= offset)
        {
            const auto message = *event;
            const int index = model::midiControllerIndex(message.data, message.numBytes);
            if (index >= 0 && (channel == 0 || (message.data[0] & 15) + 1 == channel))
            {
                const int value = model::midiControllerValue(index, message.data);
                controllers_[(size_t) index].input = value;
                if (recording) captureController(index, value, phase);
            }
            ++event;
        }
        if (elapsed >= clip_.currentEnd)
        {
            if (! controllerEnded_) { releaseMidiControllers(out, offset); controllerEnded_ = true; }
            continue;
        }
        for (int position = 0; position < numControllerIndices_; ++position)
        {
            const int index = controllerIndices_[(size_t) position];
            auto& state = controllers_[(size_t) index];
            if (state.originalLane < 0 && state.head < 0 && state.pending < 0) continue;
            const int value = controllerValue(index, phase);
            if ((refresh && sample == 0) || wrapped || value != state.last)
            {
                if (! addMidiControllerEvent(index, value, offset, out)) { if (overflow) *overflow = true; }
                state.last = value;
            }
        }
        previousPhase_ = phase;
    }
}

bool OverdubPreview::render(const BlockSegment& seg, const juce::MidiBuffer& input, int channel,
                            const BlockSegment* capture, bool stopAfterCapture, bool stopNow,
                            juce::MidiBuffer& out, bool refresh, bool* controllerOverflow, float velocityScale)
{
    if (seg.numSamples <= 0) return ! full_;
    if (stopNow && started_) stop(seg.startBeats, seg.sampleOffset, out);
    const int end = seg.sampleOffset + seg.numSamples;
    const int first = capture ? std::max(seg.sampleOffset, capture->sampleOffset) : end;
    const int last = capture ? std::min(end, capture->sampleOffset + capture->numSamples) : end;
    bool entered = started_ || ! capture || first >= last || stopped_;
    auto enter = [&] {
        if (entered) return;
        entered = started_ = true;
        const double elapsed = seg.startBeats + (first - seg.sampleOffset) / seg.samplesPerBeat;
        for (size_t key = 0; key < inputHeld_.size(); ++key)
            if (inputHeld_[key] > 0) appendNote((int) key, inputHeld_[key], elapsed);
    };
    for (const auto event : input)
    {
        if (event.samplePosition < seg.sampleOffset || event.samplePosition >= end || event.numBytes < 3) continue;
        const auto* bytes = event.data;
        if (channel != 0 && (bytes[0] & 15) + 1 != channel) continue;
        const int status = bytes[0] & 0xf0;
        if (status != 0x80 && status != 0x90) continue;
        if (event.samplePosition >= first) enter();
        const int key = (bytes[0] & 15) * 128 + (bytes[1] & 127);
        const int velocity = status == 0x90 ? bytes[2] & 127 : 0;
        inputHeld_[(size_t) key] = (uint8_t) velocity;
        if (! capture || stopped_ || event.samplePosition < first || event.samplePosition >= last) continue;
        const double elapsed = seg.startBeats + (event.samplePosition - seg.sampleOffset) / seg.samplesPerBeat;
        if (held_[(size_t) key] >= 0) closeNote(held_[(size_t) key], elapsed, bytes[2] & 127);
        held_[(size_t) key] = -1;
        if (velocity > 0) appendNote(key, velocity, elapsed);
    }
    enter();
    if (stopAfterCapture && last < end && started_)
        stop(seg.startBeats + (last - seg.sampleOffset) / seg.samplesPerBeat, last, out);
    renderControllers(seg, input, channel, capture, stopAfterCapture, stopNow, out, refresh, controllerOverflow);
    renderMidiClip(clip_, seg, out, false, controllerOverflow, earliestOn_.data(), disabledFrom_.data(), velocityScale);
    if (full_ && controllerOverflow) *controllerOverflow = true;
    return ! full_;
}
} // namespace sonora::engine
