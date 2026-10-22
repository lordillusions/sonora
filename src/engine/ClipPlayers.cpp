#include "engine/ClipPlayers.h"

#include "core/TimeUtils.h"

#include <algorithm>
#include <bit>
#include <array>
#include <cmath>

namespace sonora::engine
{

PreparedClip prepareClip(const model::Clip& c, std::shared_ptr<const SampleData> sample)
{
    return prepareClip(c, sample ? SampleSlot::makeReady(std::move(sample)) : nullptr);
}

PreparedClip prepareClip(const model::Clip& c, std::shared_ptr<SampleSlot> sample)
{
    PreparedClip p;
    p.kind = c.kind;
    p.currentStart = c.currentStart;
    p.currentEnd = c.currentEnd;
    p.loop = c.loop;
    p.midiControllers = prepareMidiControllers(c.envelopes);
    p.probabilitySeed = (uint32_t) std::max(0, c.uid);
    p.gain = (float) c.gain;
    p.fadeInBeats = c.fadeIn;
    p.fadeOutBeats = c.fadeOut;
    p.fadeInCurve = c.fadeInCurve; p.fadeOutCurve = c.fadeOutCurve;
    for (const auto& n : c.notes)
        if (n.enabled)
            p.notes.push_back(n);
    p.noteExpressions = c.noteExpressions;
    std::array<double, 15> ends{}; ends.fill(-1.0);
    for (const auto& note : p.notes)
    {
        int channel = 1;
        if (note.noteId >= 0 && std::any_of(p.noteExpressions.begin(), p.noteExpressions.end(), [&](const auto& expression) { return expression.noteId == note.noteId && (expression.cc == -2 || expression.cc == -1 || expression.cc == 74) && ! expression.points.empty(); }))
        {
            for (size_t member = 0; member < ends.size(); ++member)
                if (ends[member] <= note.start + 1e-9)
                { channel = (int) member + 2; ends[member] = note.start + note.duration; break; }
            if (channel == 1) p.expressionOverflow = true;
        }
        p.expressionChannels.push_back(channel);
    }
    p.sample = std::move(sample);
    p.warped = c.warped;
    p.warpMode = c.warpMode;
    p.warpMarkers = c.warpMarkers;
    p.pitchRatio = (float) std::pow(2.0, ((double) c.pitchCoarse + c.pitchFine / 100.0) / 12.0);
    p.transientResolution = c.transientResolution;
    p.transientLoopMode = c.transientLoopMode;
    p.granularityTones = (float) c.granularityTones;
    p.granularityTexture = (float) c.granularityTexture;
    p.fluctuationTexture = (float) c.fluctuationTexture;

    if (c.kind == model::Clip::Kind::Audio && c.warped && c.warpMode == model::WarpMode::Beats
        && c.transientResolution >= 0 && c.transientResolution <= 5 && ! c.warpMarkers.empty())
    {
        static constexpr double kStepBeats[6] = {4.0, 2.0, 1.0, 0.5, 0.25, 0.125};
        const double step = kStepBeats[c.transientResolution];
        const double lo = c.loop.loopStart;
        const double hi = c.loop.enabled ? c.loop.loopEnd : c.loop.loopStart + c.loop.startRelative + (c.currentEnd - c.currentStart);
        for (double b = std::floor(lo / step) * step; b < hi + step && p.gridSliceSeconds.size() < 100000; b += step)
            p.gridSliceSeconds.push_back(warpBeatsToSeconds(c.warpMarkers, b, 0.5));
        std::sort(p.gridSliceSeconds.begin(), p.gridSliceSeconds.end());
    }
    return p;
}

PreparedClip prepareSessionClip(const model::Clip& c, std::shared_ptr<SampleSlot> sample)
{
    auto p = prepareClip(c, std::move(sample));
    p.currentStart = 0.0;
    p.legato = c.legato;
    // Looping clips play until stopped; others play from the start marker to the clip end.
    const double length = std::max(0.25, std::max(c.loop.loopEnd, c.currentEnd) - (c.loop.loopStart + c.loop.startRelative));
    p.currentEnd = c.loop.enabled ? 1.0e9 : length;
    return p;
}

// --------------------------------------------------------------------------- MIDI

std::vector<PreparedMidiController> prepareMidiControllers(const std::vector<model::AutomationEnvelope>& envelopes)
{
    std::vector<PreparedMidiController> out;
    for (const auto& env : envelopes)
        if (model::isMidiController(env.midiController) && ! env.points.empty())
            out.push_back({env.midiController, AutomationLane(env)});
    return out;
}

bool addMidiControllerEvent(int index, int value, int sample, juce::MidiBuffer& out)
{
    if (out.data.size() + 9 > midiControllerByteLimit) return false;
    uint8_t data[3]{};
    int size = 3;
    if (index == 0) { const int bend = value + 8192; data[0] = 0xe0; data[1] = (uint8_t) (bend & 127); data[2] = (uint8_t) (bend >> 7); }
    else if (index == 1) { data[0] = 0xd0; data[1] = (uint8_t) value; size = 2; }
    else { data[0] = 0xb0; data[1] = (uint8_t) (index - 2); data[2] = (uint8_t) value; }
    out.addEvent(data, size, sample);
    return true;
}

bool renderMidiControllers(const std::vector<PreparedMidiController>& controllers, const BlockSegment& seg, juce::MidiBuffer& out, bool refresh)
{
    for (const auto& controller : controllers)
        for (int sample = 0; sample < seg.numSamples; ++sample)
        {
            const double beat = seg.startBeats + sample / seg.samplesPerBeat;
            const int value = model::quantiseMidiController(controller.index, controller.lane.valueAt(beat + 1e-10));
            const int previous = model::quantiseMidiController(controller.index, controller.lane.valueAt(beat - 1.0 / seg.samplesPerBeat + 1e-10));
            if ((refresh && sample == 0) || value != previous)
                if (! addMidiControllerEvent(controller.index, value, seg.sampleOffset + sample, out)) return false;
        }
    return true;
}

void releaseMidiControllers(juce::MidiBuffer& out, int sample)
{
    for (int channel = 1; channel <= 16; ++channel)
    {
        for (int pedal : {64, 66, 69}) out.addEvent(juce::MidiMessage::controllerEvent(channel, pedal, 0), sample);
        out.addEvent(juce::MidiMessage::pitchWheel(channel, 8192), sample);
    }
}

void renderMidiClip(const PreparedClip& clip, const BlockSegment& seg, juce::MidiBuffer& out, bool refreshControllers,
                    bool* controllerOverflow, const double* earliestOn, const double* disabledFrom, float velocityScale)
{
    // Note-offs may land exactly on currentEnd, so a segment starting at currentEnd still counts.
    if (seg.endBeats() <= clip.currentStart || seg.startBeats > clip.currentEnd)
        return;

    const auto& loop = clip.loop;
    const double playStart = loop.loopStart + loop.startRelative; // content position at currentStart
    const double loopLen = loop.loopEnd - loop.loopStart;
    const bool looping = loop.enabled && loopLen > 0.0;

    auto toSample = [&](double beat) {
        return juce::jlimit(0, seg.numSamples - 1, (int) std::floor((beat - seg.startBeats) * seg.samplesPerBeat + 1e-8))
               + seg.sampleOffset;
    };
    auto inSeg = [&](double beat) { return beat >= seg.startBeats && beat < seg.endBeats(); };

    // A sub-sample epsilon avoids delayed jumps from cancellation in saved content offsets.
    if (! clip.midiControllers.empty())
    {
        for (const auto& controller : clip.midiControllers)
            for (int sample = 0; sample < seg.numSamples; ++sample)
            {
                const double beat = seg.startBeats + sample / seg.samplesPerBeat;
                if (beat < clip.currentStart || beat >= clip.currentEnd) continue;
                const double content = model::clipContentPosition(beat, clip.currentStart, loop.loopStart, loop.loopEnd, loop.startRelative, loop.enabled);
                const double previousBeat = beat - 1.0 / seg.samplesPerBeat;
                const double previousContent = model::clipContentPosition(previousBeat, clip.currentStart, loop.loopStart, loop.loopEnd, loop.startRelative, loop.enabled);
                const int value = model::quantiseMidiController(controller.index, controller.lane.valueAt(content + 1e-10));
                const int previous = model::quantiseMidiController(controller.index, controller.lane.valueAt(previousContent + 1e-10));
                if ((refreshControllers && sample == 0) || previousBeat < clip.currentStart || content < previousContent || value != previous)
                    if (! addMidiControllerEvent(controller.index, value, seg.sampleOffset + sample, out))
                    { if (controllerOverflow) *controllerOverflow = true; break; }
            }
        // Clip ends release its pedal/bend state even when there is no following clip.
        if (inSeg(clip.currentEnd)) releaseMidiControllers(out, toSample(clip.currentEnd));
    }
    if (velocityScale <= 0.0f) return; // zero-velocity note-ons are note-offs; a silent launch emits neither
    for (size_t index = 0; index < clip.notes.size(); ++index)
    {
        const auto& n = clip.notes[index];
        if (! std::isfinite(n.duration) || ! std::isfinite(n.start) || n.duration <= 0 || (disabledFrom && disabledFrom[index] <= seg.startBeats + 1e-10)) continue;
        if (looping && n.start >= loop.loopEnd)
            continue;
        const bool repeats = looping && n.start >= loop.loopStart;

        // Timeline onset of the first occurrence of this note.
        double first = n.start - playStart;
        if (repeats && first < 0.0)
            first += loopLen;
        if (! repeats && first < 0.0)
            continue;
        const double noteLen = looping ? std::min(n.duration, loop.loopEnd - n.start) : n.duration;

        // Iterate the occurrences that could touch [seg.start - noteLen, seg.end).
        double k0 = 0.0;
        if (repeats)
            k0 = std::max(0.0, std::floor((seg.startBeats - clip.currentStart - first - noteLen) / loopLen));
        for (double k = k0;; k += 1.0)
        {
            const double on = clip.currentStart + first + (repeats ? k * loopLen : 0.0);
            if (on >= seg.endBeats() || on >= clip.currentEnd)
                break;
            const double off = std::min(on + noteLen, clip.currentEnd);
            // Stable per occurrence: split blocks and note-offs make the same decision without
            // random-generator state or allocation. Seeking replays that occurrence's decision.
            uint64_t hash = (uint64_t) clip.probabilitySeed ^ std::bit_cast<uint64_t>(n.start)
                            ^ ((uint64_t) (uint32_t) n.pitch << 32) ^ (uint64_t) k;
            hash += 0x9e3779b97f4a7c15ULL;
            hash = (hash ^ (hash >> 30)) * 0xbf58476d1ce4e5b9ULL;
            hash = (hash ^ (hash >> 27)) * 0x94d049bb133111ebULL;
            hash ^= hash >> 31;
            const double chance = (double) (hash >> 11) * 0x1.0p-53;
            if (! std::isfinite(n.probability) || chance >= std::clamp((double) n.probability, 0.0, 1.0))
            {
                if (! repeats) break;
                continue;
            }
            const int vel = juce::jlimit(1, 127, (int) std::lround(n.velocity * velocityScale));
            if (earliestOn && on + 1e-10 < earliestOn[index]) { if (! repeats) break; continue; }
            if (earliestOn && out.data.size() + 18 > midiControllerByteLimit)
            { if (controllerOverflow) *controllerOverflow = true; return; }
            const int channel = index < clip.expressionChannels.size() ? clip.expressionChannels[index] : 1;
            if (channel > 1 && (! disabledFrom || on + 1e-10 < disabledFrom[index]))
            {
                auto send = [&](uint8_t status, int value, int sample) {
                    if (out.data.size() + 9 > midiControllerByteLimit) { if (controllerOverflow) *controllerOverflow = true; return; }
                    const uint8_t data[] = {(uint8_t) (status | (channel - 1)), (uint8_t) (value & 127), (uint8_t) ((value >> 7) & 127)};
                    out.addEvent(data, status == 0xd0 ? 2 : 3, sample);
                };
                auto expressionValue = [](const model::NoteExpression& expression, double time) {
                    const auto next = std::upper_bound(expression.points.begin(), expression.points.end(), time, [](double beat, const auto& point) { return beat < point.time; });
                    if (next == expression.points.begin()) return next->value;
                    const auto& previous = *std::prev(next);
                    if (next == expression.points.end() || next->time - previous.time < 1e-12) return previous.value;
                    return previous.value + (next->value - previous.value) * (time - previous.time) / (next->time - previous.time);
                };
                // Initialise every member dimension before note-on, including when a reused channel had different curves.
                for (int cc : {-2, -1, 74})
                {
                    const auto expression = std::find_if(clip.noteExpressions.begin(), clip.noteExpressions.end(), [&](const auto& lane) { return lane.noteId == n.noteId && lane.cc == cc && ! lane.points.empty(); });
                    const int firstSample = std::max(0, (int) std::clamp(std::ceil((on - seg.startBeats) * seg.samplesPerBeat - 1e-8), 0.0, (double) seg.numSamples));
                    const int lastSample = std::min(seg.numSamples, (int) std::clamp(std::ceil((off - seg.startBeats) * seg.samplesPerBeat - 1e-8), 0.0, (double) seg.numSamples));
                    int previous = -1;
                    for (int sample = firstSample; sample < lastSample; ++sample)
                    {
                        const double time = seg.startBeats + sample / seg.samplesPerBeat - on;
                        const double value = expression == clip.noteExpressions.end() ? (cc == 74 ? 64.0 : 0.0) : expressionValue(*expression, time);
                        const int quantised = cc == -2 ? std::clamp((int) std::lround(8192.0 + value / 48.0 * 8192.0), 0, 16383)
                                                     : std::clamp((int) std::lround(value), 0, 127);
                        if (sample == firstSample || quantised != previous)
                        {
                            if (cc == -2) send(0xe0, quantised, seg.sampleOffset + sample);
                            else if (cc == -1) send(0xd0, quantised, seg.sampleOffset + sample);
                            else send(0xb0, 74 | (quantised << 7), seg.sampleOffset + sample);
                        }
                        previous = quantised;
                    }
                }
            }
            if (channel > 1 && out.data.size() + 18 > midiControllerByteLimit)
            { if (controllerOverflow) *controllerOverflow = true; return; }
            if (inSeg(on) && (! disabledFrom || on + 1e-10 < disabledFrom[index]))
                out.addEvent(juce::MidiMessage::noteOn(channel, n.pitch, (juce::uint8) vel), toSample(on));
            if (inSeg(off) && (! disabledFrom || off + 1e-10 < disabledFrom[index]))
                out.addEvent(juce::MidiMessage::noteOff(channel, n.pitch, (juce::uint8) juce::jlimit(0, 127, (int) n.releaseVelocity)),
                             toSample(off));
            if (! repeats)
                break;
        }
    }
}

// --------------------------------------------------------------------------- audio

double warpBeatsToSeconds(const std::vector<model::WarpMarker>& m, double beats, double fallbackSecPerBeat)
{
    if (m.empty())
        return beats * fallbackSecPerBeat;
    if (m.size() == 1)
        return m[0].seconds + (beats - m[0].beats) * fallbackSecPerBeat;

    // Segment [i-1, i] with i the first index in [1, size-1] whose marker is at or after `beats`
    // (the last segment extrapolates past the end, the first one before the start).
    const auto first = m.begin() + 1, last = m.end() - 1;
    const auto it = std::lower_bound(first, last, beats,
                                     [](const model::WarpMarker& mk, double b) { return mk.beats < b; });
    const auto& a = *(it - 1);
    const auto& b = *it;
    const double span = b.beats - a.beats;
    const double slope = span > 1e-12 ? (b.seconds - a.seconds) / span : fallbackSecPerBeat;
    return a.seconds + (beats - a.beats) * slope;
}

void renderAudioClip(const PreparedClip& clip, const BlockSegment& seg, double tempo, juce::AudioBuffer<float>& out)
{
    const SampleData* sample = clip.sample != nullptr ? clip.sample->get() : nullptr;
    if (sample == nullptr) // missing file, or still loading (ENG-8)
        return;
    if (seg.endBeats() <= clip.currentStart || seg.startBeats >= clip.currentEnd)
        return;

    const auto& src = sample->audio;
    const int srcLen = src.getNumSamples();
    const double srcRate = sample->sampleRate;
    const double secPerBeat = 60.0 / tempo;
    const float* inL = src.getReadPointer(0);
    const float* inR = src.getReadPointer(1);
    float* outL = out.getWritePointer(0);
    float* outR = out.getWritePointer(1);
    const auto& loop = clip.loop;

    for (int i = 0; i < seg.numSamples; ++i)
    {
        const double beat = seg.startBeats + i / seg.samplesPerBeat;
        if (beat < clip.currentStart || beat >= clip.currentEnd)
            continue;

        double seconds;
        if (clip.warped)
        {
            // Warped: loop values are in beats; map content beats -> file seconds.
            const double contentBeats = model::clipContentPosition(beat, clip.currentStart, loop.loopStart,
                                                                   loop.loopEnd, loop.startRelative, loop.enabled);
            seconds = warpBeatsToSeconds(clip.warpMarkers, contentBeats, secPerBeat);
        }
        else
        {
            // Unwarped: loop values are in SECONDS (verified on real sets, docs/ALS_FORMAT.md).
            // Transposing an unwarped clip resamples it (it plays faster/slower), like Live.
            const double elapsedSec = (beat - clip.currentStart) * secPerBeat * clip.pitchRatio;
            seconds = model::clipContentPosition(elapsedSec, 0.0, loop.loopStart, loop.loopEnd, loop.startRelative,
                                                 loop.enabled);
        }

        const double pos = seconds * srcRate;
        const int i0 = (int) std::floor(pos);
        if (i0 < 0 || i0 + 1 >= srcLen)
            continue;
        const float frac = (float) (pos - i0);
        const int o = seg.sampleOffset + i;
        float g = clip.gain;
        if (clip.fadeInBeats > 0.0 || clip.fadeOutBeats > 0.0)
        {
            double fade = 1.0;
            if (clip.fadeInBeats > 0.0) fade = std::min(fade, model::clipFadeGain((beat - clip.currentStart) / clip.fadeInBeats, clip.fadeInCurve));
            if (clip.fadeOutBeats > 0.0) fade = std::min(fade, model::clipFadeGain((clip.currentEnd - beat) / clip.fadeOutBeats, clip.fadeOutCurve));
            g *= (float) std::clamp(fade, 0.0, 1.0);
        }
        outL[o] += (inL[i0] + (inL[i0 + 1] - inL[i0]) * frac) * g;
        outR[o] += (inR[i0] + (inR[i0 + 1] - inR[i0]) * frac) * g;
    }
}

} // namespace sonora::engine
