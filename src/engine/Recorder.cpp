#include "engine/Quantisation.h"
#include "core/Edit.h"
#include "core/TimeUtils.h"
#include "engine/Recorder.h"
#include "engine/Automation.h"
#include "io/als/AlsTemplates.h"

#include <algorithm>
#include <limits>
#include <cmath>

namespace sonora::engine
{

Recorder::Recorder() : juce::Thread("Sonora recording writer") {}

Recorder::~Recorder()
{
    // AudioEngine removes its device callback before destroying us.
    deviceStopped();
    stopThread(10000);
    delete pendingPreview_.exchange(nullptr); delete retiredPreview_.exchange(nullptr); delete preview_;
}

bool Recorder::start(const model::Song& song, const ControlBank& controls, const juce::File& directory,
                     double sampleRate, std::string& error, bool arrangement, const SessionPlan* session)
{
    if (state_.load(std::memory_order_acquire) != State::Idle)
    {
        error = "A recording take is still being captured or saved";
        return false;
    }
    collectPreviews();
    auto previews = std::make_unique<PreviewBank>();
    std::vector<std::unique_ptr<Take>> prepared;
    for (size_t i = 0; i < song.tracks.size() && i < controls.size(); ++i)
    {
        const auto& track = song.tracks[i];
        if (! controls[i].armed.load() || (track.kind != model::TrackKind::Midi && track.kind != model::TrackKind::Audio)) continue;
        const SessionTarget* target = nullptr;
        if (session)
        {
            for (const auto& candidate : session->targets) if (candidate.track == (int) i) { target = &candidate; break; }
            if (! target) continue;
        }
        auto take = std::make_unique<Take>();
        if (target)
        {
            take->scene = target->scene; take->expectedUid = target->uid;
            if (target->scene < 0) { error = "Choose a Session scene before recording"; return false; }
            if (target->uid >= 0)
            {
                const auto* clip = model::findClip(song, model::ClipLocation::session((int) i, target->uid, target->scene));
                if (! clip || clip->kind != model::Clip::Kind::Midi) { error = "The MIDI overdub target changed"; return false; }
                take->overdub = *clip;
            }
            else if ((size_t) target->scene < track.slots.size() && track.slots[(size_t) target->scene].clip)
            { error = "Choose an empty audio slot or a MIDI clip to overdub"; return false; }
        }
        take->track = (int) i;
        take->inputRoute = controls[i].inputRoute.load();
        if (take->inputRoute == 0) take->inputRoute = model::packInput(model::parseInput(track));
        const auto inputKind = model::inputKind(take->inputRoute);
        if (inputKind == model::InputKind::None || inputKind == model::InputKind::Unsupported)
        { error = "Choose a supported input for '" + track.name + "' before recording"; return false; }
        take->held.fill(-1); take->inputControllers.fill(-32768);
        take->clip.kind = track.kind == model::TrackKind::Midi ? model::Clip::Kind::Midi : model::Clip::Kind::Audio;
        take->clip.colorIndex = track.colorIndex;
        take->clip.name = "Recording " + track.name;
        take->clip.loop.enabled = false;
        take->clip.resetSourceMarkers = true; take->clip.stripSourceEnvelopes = true;
        bool foundTemplate = false;
        for (const auto& source : song.tracks)
        {
            for (const auto& clip : source.arrangementClips)
                if (clip.kind == take->clip.kind && clip.sourceXml)
                {
                    take->clip.cloneOfXmlId = clip.xmlId;
                    take->clip.sourceXml = clip.sourceXml;
                    take->clip.cloneOfTrackId = source.id;
                    foundTemplate = true;
                    break;
                }
            if (foundTemplate) break;
        }
        if (! foundTemplate)
            for (const auto& source : song.tracks)
            {
                for (size_t slot = 0; slot < source.slots.size(); ++slot)
                    if (source.slots[slot].clip && source.slots[slot].clip->kind == take->clip.kind)
                    {
                        take->clip.cloneOfSessionSlot = (int) slot;
                        take->clip.sourceXml = source.slots[slot].clip->sourceXml;
                        take->clip.cloneOfTrackId = source.id;
                        foundTemplate = true;
                        break;
                    }
                if (foundTemplate) break;
            }
        if (take->overdub) { take->clip = *take->overdub; foundTemplate = true; }
        if (! foundTemplate && track.kind == model::TrackKind::Midi)
        {
            error = "This set needs an existing " + std::string(track.kind == model::TrackKind::Midi ? "MIDI" : "audio")
                    + " clip as a save-format template";
            return false;
        }
        if (track.kind == model::TrackKind::Midi)
        {
            take->notes.resize(kMaxNotes); take->controllers.resize(kMaxControllers);
        }
        else
        {
            if (! directory.createDirectory()) { error = "Could not create the recording folder"; return false; }
            const auto file = directory.getNonexistentChildFile("Recording-" + juce::String((int) i + 1) + "-"
                                                               + juce::Time::getCurrentTime().formatted("%Y%m%d-%H%M%S"), ".wav");
            std::unique_ptr<juce::OutputStream> stream = file.createOutputStream();
            if (! stream) { error = "Could not open a recording WAV"; return false; }
            juce::WavAudioFormat format;
            take->writer = format.createWriterFor(stream, juce::AudioFormatWriterOptions{}
                                                    .withSampleRate(sampleRate).withNumChannels(inputKind == model::InputKind::AudioMono ? 1 : 2).withBitsPerSample(24));
            if (! take->writer) { error = "Could not create the WAV writer"; return false; }
            take->clip.sample.absolutePath = file.getFullPathName().toStdString();
            take->clip.sampleRate = sampleRate;
            take->fifo = std::make_unique<juce::AbstractFifo>(kAudioRing);
            take->ring.setSize(2, kAudioRing);
        }
        if (take->overdub)
        {
            previews->clips.push_back(std::make_unique<OverdubPreview>(take->track, take->scene, *take->overdub));
            previews->channels.push_back(model::inputChannel(take->inputRoute)); previews->disabled.push_back(0);
        }
        prepared.push_back(std::move(take));
    }
    if (prepared.empty()) { error = "Arm an audio or MIDI track before recording"; return false; }
    takes_ = std::move(prepared);
    sampleRate_ = sampleRate;
    arrangement_ = arrangement;
    sessionScheduled_ = session != nullptr;
    sessionPlan_ = session ? *session : SessionPlan{};
    sessionStarted_ = sessionStopping_ = sessionEnded_ = false;
    sessionStopRequested_.store(false); sessionPhase_.store(session ? 1 : 0);
    repeating_ = arrangement && song.loopOn;
    passes_.resize(kMaxPasses); tempoMarks_.resize(kMaxTempoMarks);
    numPasses_ = numTempoMarks_ = 0; totalFrames_ = 0; previousSamplesPerBeat_ = 0; newPass_ = true;
    punchStart_ = arrangement && song.punchIn ? song.loopStart : -std::numeric_limits<double>::infinity();
    punchEnd_ = arrangement && song.punchOut ? song.loopStart + song.loopLength : std::numeric_limits<double>::infinity();
    started_ = false;
    startBeat_ = lengthBeats_ = 0;
    stopRequested_.store(false);
    failed_.store(false);
    discontinuity_.store(false);
    if (! startThread())
    {
        takes_.clear();
        state_.store(State::Idle, std::memory_order_release);
        error = "Could not start the recording writer";
        return false;
    }
    discardPreview_.store(false, std::memory_order_release);
    if (! previews->clips.empty())
    {
        delete pendingPreview_.exchange(previews.release(), std::memory_order_acq_rel);
    }
    state_.store(State::Capturing, std::memory_order_release);
    return true;
}

void Recorder::collectPreviews()
{
    delete retiredPreview_.exchange(nullptr, std::memory_order_acq_rel);
}

void Recorder::updatePreviewBank()
{
    if (retiredPreview_.load(std::memory_order_acquire)) return;
    if (auto* fresh = pendingPreview_.exchange(nullptr, std::memory_order_acq_rel))
    {
        auto* old = preview_; preview_ = fresh;
        previewLive_.store(true, std::memory_order_release);
        if (old) retiredPreview_.store(old, std::memory_order_release);
    }
    if (discardPreview_.exchange(false, std::memory_order_acq_rel) && preview_)
    {
        // There may already be a retired old bank from the publication above; retry next block.
        if (retiredPreview_.load(std::memory_order_acquire)) { discardPreview_.store(true); return; }
        auto* old = preview_; preview_ = nullptr; retiredPreview_.store(old, std::memory_order_release);
        previewLive_.store(false, std::memory_order_release);
    }
}

void Recorder::beginPreviewBlock(const EngineGraph& graph)
{
    updatePreviewBank();
    if (! preview_) return;
    for (size_t i = 0; i < preview_->clips.size(); ++i)
    {
        const auto& p = *preview_->clips[i];
        const bool validTrack = p.track() >= 0 && (size_t) p.track() < graph.nodes.size();
        const bool validSlot = validTrack && p.scene() >= 0 && (size_t) p.scene() < graph.nodes[(size_t) p.track()].sessionClips.size();
        if (! validSlot || ! graph.nodes[(size_t) p.track()].hasSessionClip[(size_t) p.scene()]
            || graph.nodes[(size_t) p.track()].sessionClips[(size_t) p.scene()].probabilitySeed != (uint32_t) p.uid()) preview_->disabled[i] = 1;
    }
    if (std::all_of(preview_->disabled.begin(), preview_->disabled.end(), [](uint8_t x) { return x != 0; })
        && ! retiredPreview_.load(std::memory_order_acquire))
    {
        auto* old = preview_; preview_ = nullptr; retiredPreview_.store(old, std::memory_order_release);
        previewLive_.store(false, std::memory_order_release);
    }
}

bool Recorder::hasOverdubPreview(int track, int scene, uint32_t uid)
{
    updatePreviewBank();
    if (! preview_) return false;
    for (size_t i = 0; i < preview_->clips.size(); ++i)
        if (! preview_->disabled[i] && preview_->clips[i]->track() == track && preview_->clips[i]->scene() == scene
            && (uint32_t) preview_->clips[i]->uid() == uid && preview_->clips[i]->started()) return true;
    return false;
}

bool Recorder::renderOverdub(int track, int scene, const PreparedClip& clip, const BlockSegment& segment,
                            const juce::MidiBuffer& input, const CaptureRange* range, juce::MidiBuffer& out,
                            bool refresh, bool* overflow, float velocityScale)
{
    updatePreviewBank();
    if (! preview_) return false;
    for (size_t i = 0; i < preview_->clips.size(); ++i)
    {
        auto& p = *preview_->clips[i];
        if (preview_->disabled[i] || p.track() != track || p.scene() != scene || (uint32_t) p.uid() != clip.probabilitySeed) continue;
        BlockSegment capture;
        const bool capturing = active() && ! stopRequested_.load(std::memory_order_relaxed);
        if (range && capturing) capture = {range->offset, range->count, 0, segment.samplesPerBeat};
        if (! p.render(segment, input, preview_->channels[i], range && capturing ? &capture : nullptr,
                       range && range->punchEnd, ! capturing, out, refresh, overflow, velocityScale)) failed_.store(true);
        return true;
    }
    return false;
}

bool Recorder::commitOverdub(int track, int scene, const PreparedClip* replacement, double elapsed, juce::MidiBuffer& out)
{
    updatePreviewBank();
    if (! preview_ || active()) return false;
    bool preserved = false;
    for (size_t i = 0; i < preview_->clips.size(); ++i)
    {
        auto& p = *preview_->clips[i];
        if (preview_->disabled[i] || p.track() != track) continue;
        p.finish(elapsed, 0, out); preview_->disabled[i] = 1;
        preserved = scene == p.scene() && replacement && replacement->probabilitySeed == (uint32_t) p.uid();
    }
    if (std::all_of(preview_->disabled.begin(), preview_->disabled.end(), [](uint8_t x) { return x != 0; })
        && ! retiredPreview_.load(std::memory_order_acquire))
    {
        auto* old = preview_; preview_ = nullptr; retiredPreview_.store(old, std::memory_order_release);
        previewLive_.store(false, std::memory_order_release);
    }
    return preserved;
}

int Recorder::sessionScene(int track) const
{
    if (! sessionRecording()) return -1;
    for (const auto& take : takes_) if (take->track == track) return take->scene;
    return -1;
}

bool Recorder::overdubsTrack(int track) const
{
    if (! active()) return false;
    for (const auto& take : takes_) if (take->track == track) return take->overdub.has_value();
    return false;
}

void Recorder::beginSessionContent(int track, double elapsed)
{
    for (auto& take : takes_) if (take->track == track && ! take->contentStarted)
    {
        take->contentStarted = true;
        if (take->overdub)
        {
            const auto& loop = take->overdub->loop;
            take->contentStart = model::clipContentPosition(elapsed, 0, loop.loopStart, loop.loopEnd, loop.startRelative, loop.enabled);
        }
    }
}

Recorder::CaptureRange Recorder::prepareCaptureRange(const BlockSegment& segment, bool playing, bool jumped)
{
    CaptureRange result{segment.sampleOffset, 0, segment.startBeats, false};
    // Acquire Capturing before reading the plan published by start().
    if (! active()) return result;
    if (! sessionScheduled_) return captureRange(segment.startBeats, segment.samplesPerBeat, segment.numSamples, playing, jumped, segment.sampleOffset);
    if (sessionEnded_ || ! capturesCurrentBlock(playing, jumped)) return result;
    if (sessionStopRequested_.exchange(false, std::memory_order_acq_rel))
    {
        if (! sessionStarted_) { sessionEnded_ = true; result.punchEnd = true; return result; }
        sessionStopping_ = true; sessionPhase_.store(3, std::memory_order_relaxed);
    }
    int first = 0, last = segment.numSamples;
    if (! sessionStarted_)
    {
        first = quantisedSample(segment, sessionPlan_.immediateStart ? 0 : sessionPlan_.quant, sessionPlan_.looping, sessionPlan_.loopStart, sessionPlan_.loopEnd);
        if (first < 0) return result;
        sessionStarted_ = true; sessionPhase_.store(2, std::memory_order_relaxed);
    }
    if (sessionStopping_)
    {
        const int stop = quantisedSample(segment, sessionPlan_.quant, sessionPlan_.looping, sessionPlan_.loopStart, sessionPlan_.loopEnd);
        if (stop >= 0) { last = stop; result.punchEnd = true; sessionEnded_ = true; }
    }
    result.offset += first; result.count = std::max(0, last - first);
    result.beat += first / segment.samplesPerBeat;
    return result;
}

bool Recorder::capturesAudioTrack(int track) const
{
    if (! active()) return false;
    for (const auto& take : takes_)
        if (take->track == track && take->fifo) return true;
    return false;
}

bool Recorder::capturesTrack(int track) const
{
    if (! active()) return false;
    for (const auto& take : takes_) if (take->track == track) return true;
    return false;
}

Recorder::CaptureRange Recorder::captureRange(double beat, double samplesPerBeat, int numSamples,
                                              bool playing, bool jumped, int sampleOffset) const
{
    CaptureRange range{sampleOffset, 0, beat, false};
    if (! capturesCurrentBlock(playing, jumped)) return range;
    auto frame = [&](double boundary) {
        return (int) std::ceil(std::clamp((boundary - beat) * samplesPerBeat - 1e-8, 0.0, (double) numSamples));
    };
    const int first = frame(punchStart_), last = frame(punchEnd_);
    range.offset += first;
    range.count = std::max(0, last - first);
    range.beat += first / samplesPerBeat;
    if (std::abs(range.beat - punchStart_) < 1e-9) range.beat = punchStart_;
    range.punchEnd = beat + numSamples / samplesPerBeat >= punchEnd_ - 1e-9;
    return range;
}

void Recorder::capture(const float* const* inputs, int numInputs, const juce::MidiBuffer& midi,
                       double beat, double samplesPerBeat, int numSamples, bool playing, bool jumped, bool loopEnd,
                       const EngineGraph* graph, int sampleOffset, const CaptureRange* preparedRange)
{
    if (! active()) return;
    if (stopRequested_.load(std::memory_order_acquire) || (! playing && started_) || (jumped && started_))
    {
        if (jumped && started_) discontinuity_.store(true);
        endCapture();
        return;
    }
    const auto range = preparedRange ? *preparedRange : captureRange(beat, samplesPerBeat, numSamples, playing, jumped, sampleOffset);
    const bool firstCapture = range.count > 0 && newPass_;
    if (range.count > 0)
    {
        // Check all rings/capacities before publishing any samples from this segment.
        bool full = firstCapture && numPasses_ == kMaxPasses;
        const bool mark = numTempoMarks_ == 0 || std::abs(samplesPerBeat - previousSamplesPerBeat_) > 1e-8;
        full |= mark && numTempoMarks_ == kMaxTempoMarks - 1; // reserve the final endpoint
        for (const auto& take : takes_) if (take->fifo) full |= take->fifo->getFreeSpace() < range.count;
        if (full) { failed_.store(true); endCapture(); return; }
        if (mark)
        {
            tempoMarks_[(size_t) numTempoMarks_++] = {(double) totalFrames_ / sampleRate_, lengthBeats_};
            previousSamplesPerBeat_ = samplesPerBeat;
        }
        if (firstCapture)
        {
            if (! started_) startBeat_ = range.beat;
            passes_[(size_t) numPasses_++] = {range.beat, lengthBeats_, 0, totalFrames_, 0};
            newPass_ = false; started_ = true;
        }
    }
    for (auto& take : takes_)
    {
        if (take->fifo)
        {
            if (range.count == 0) continue;
            int a, na, b, nb;
            take->fifo->prepareToWrite(range.count, a, na, b, nb);
            if (na + nb != range.count) { failed_.store(true); continue; }
            for (int ch = 0; ch < 2; ++ch)
            {
                const auto kind = model::inputKind(take->inputRoute);
                const int channel = model::inputChannel(take->inputRoute) + (kind == model::InputKind::AudioMono ? 0 : ch);
                const float* source = nullptr;
                if (kind == model::InputKind::Resampling && graph && graph->mainNode >= 0)
                    source = graph->nodes[(size_t) graph->mainNode].buffer.getReadPointer(ch);
                else if ((kind == model::InputKind::AudioMono || kind == model::InputKind::AudioStereo)
                         && inputs && channel < numInputs) source = inputs[channel];
                if (source)
                {
                    source += range.offset;
                    take->ring.copyFrom(ch, a, source, na);
                    take->ring.copyFrom(ch, b, source + na, nb);
                }
                else
                {
                    take->ring.clear(ch, a, na);
                    take->ring.clear(ch, b, nb);
                }
            }
            take->fifo->finishedWrite(range.count);
            take->frames += range.count;
        }
        else
        {
            const auto& routedMidi = graph && (size_t) take->track < graph->nodes.size() ? graph->nodes[(size_t) take->track].inputMidi : midi;
            auto addController = [&](int index, int value, double time) {
                if (take->numControllers == kMaxControllers) { failed_.store(true); return; }
                take->controllers[(size_t) take->numControllers++] = {time, (int16_t) value, (uint8_t) index};
            };
            bool boundaryEntered = ! firstCapture;
            auto enterBoundary = [&] {
                if (boundaryEntered) return;
                boundaryEntered = true;
                for (int index = 0; index < 130; ++index)
                    if (take->inputControllers[(size_t) index] != -32768)
                        addController(index, take->inputControllers[(size_t) index], lengthBeats_);
                for (size_t key = 0; key < take->inputHeld.size(); ++key)
                    if (take->inputHeld[key] > 0)
                    {
                        if (take->numNotes == kMaxNotes) { failed_.store(true); break; }
                        const int index = take->numNotes++;
                        take->held[key] = index;
                        auto& note = take->notes[(size_t) index];
                        note.start = lengthBeats_;
                        note.pitch = (int) key % 128;
                        note.velocity = take->inputHeld[key];
                    }
            };
            for (const auto event : routedMidi)
            {
                if (event.samplePosition < 0 || event.samplePosition >= sampleOffset + numSamples) continue;
                if (event.samplePosition < sampleOffset && (numPasses_ > 1 || lengthBeats_ > 0)) continue; // do not reconsume the earlier wrap segment
                // Inspect raw bytes to avoid constructing a MidiMessage on the audio thread.
                if (event.numBytes < 2) continue;
                const auto* data = event.data;
                const int selectedChannel = model::inputChannel(take->inputRoute);
                if (selectedChannel != 0 && (data[0] & 0x0f) + 1 != selectedChannel) continue;
                const int controller = model::midiControllerIndex(data, event.numBytes);
                if (controller >= 0)
                {
                    if (range.count > 0 && event.samplePosition >= range.offset) enterBoundary();
                    const int value = model::midiControllerValue(controller, data);
                    take->inputControllers[(size_t) controller] = value;
                    if (range.count > 0 && event.samplePosition >= range.offset && event.samplePosition < range.offset + range.count)
                        addController(controller, value, lengthBeats_ + (event.samplePosition - range.offset) / samplesPerBeat);
                    continue;
                }
                if (event.numBytes < 3) continue;
                const int status = data[0] & 0xf0, key = (data[0] & 0x0f) * 128 + (data[1] & 0x7f);
                if (status != 0x90 && status != 0x80) continue;
                if (range.count > 0 && event.samplePosition >= range.offset) enterBoundary();
                take->inputHeld[(size_t) key] = status == 0x90 ? data[2] : 0;
                if (range.count == 0 || event.samplePosition < range.offset || event.samplePosition >= range.offset + range.count) continue;
                const double time = lengthBeats_ + (event.samplePosition - range.offset) / samplesPerBeat;
                int& held = take->held[(size_t) key];
                if (held >= 0)
                {
                    take->notes[(size_t) held].duration = std::max(1.0 / samplesPerBeat, time - take->notes[(size_t) held].start);
                    take->notes[(size_t) held].releaseVelocity = (float) data[2];
                    held = -1;
                }
                if (status == 0x90 && data[2] > 0)
                {
                    if (take->numNotes == kMaxNotes) { failed_.store(true); continue; }
                    held = take->numNotes++;
                    auto& note = take->notes[(size_t) held];
                    note.start = time;
                    note.pitch = data[1] & 0x7f;
                    note.velocity = (float) data[2];
                }
            }
            if (range.count > 0) enterBoundary();
        }
    }
    lengthBeats_ += range.count / samplesPerBeat;
    totalFrames_ += range.count;
    if (range.count > 0)
    {
        auto& pass = passes_[(size_t) numPasses_ - 1];
        pass.length += range.count / samplesPerBeat; pass.frames += range.count;
    }
    if (! repeating_ && range.punchEnd && started_ && std::abs(startBeat_ + lengthBeats_ - punchEnd_) < 1e-9)
        lengthBeats_ = punchEnd_ - startBeat_;
    if (repeating_ && arrangement_ && (loopEnd || range.punchEnd))
    {
        for (auto& take : takes_) for (auto& index : take->held)
            if (index >= 0)
            {
                take->notes[(size_t) index].duration = std::max(1e-6, lengthBeats_ - take->notes[(size_t) index].start);
                index = -1;
            }
        newPass_ = true;
    }
    if (failed_.load() || (range.punchEnd && sessionScheduled_) || (! repeating_ && ((arrangement_ && loopEnd) || range.punchEnd)))
    {
        if (arrangement_ && loopEnd) discontinuity_.store(true);
        endCapture();
    }
}

void Recorder::endCapture()
{
    if (! active()) return;
    for (auto& take : takes_)
        for (int index : take->held)
            if (index >= 0)
                take->notes[(size_t) index].duration = std::max(1e-6, lengthBeats_ - take->notes[(size_t) index].start);
    sessionPhase_.store(0, std::memory_order_relaxed);
    state_.store(State::Draining, std::memory_order_release);
}

void Recorder::deviceStopped()
{
    endCapture();
}

void Recorder::discardUnpublished()
{
    deviceStopped(); stopThread(10000);
    if (finished()) (void) takeResult(); // closes writers and removes empty WAVs
}

void Recorder::drain(Take& take)
{
    int a, na, b, nb;
    take.fifo->prepareToRead(kAudioRing, a, na, b, nb);
    if (na > 0 && ! take.writer->writeFromAudioSampleBuffer(take.ring, a, na)) failed_.store(true);
    if (nb > 0 && ! take.writer->writeFromAudioSampleBuffer(take.ring, b, nb)) failed_.store(true);
    take.fifo->finishedRead(na + nb);
}

void Recorder::run()
{
    for (;;)
    {
        const auto state = state_.load(std::memory_order_acquire);
        for (auto& take : takes_) if (take->fifo) drain(*take);
        if (state == State::Draining)
        {
            // Producer published Draining before this final drain; nothing can arrive afterwards.
            for (auto& take : takes_)
                if (take->fifo) { drain(*take); take->writer.reset(); }
            state_.store(State::Finished, std::memory_order_release);
            return;
        }
        if (threadShouldExit()) return;
        wait(5);
    }
}

void Recorder::mergeControllers(Take& take, model::Clip& clip, bool overdub)
{
    // FloatEvent duplicates make exact steps, as observed in the reference. All vectors here
    // are created after the worker has released the take, never during capture.
    for (int index = 0; index < 130; ++index)
    {
        model::AutomationEnvelope captured; captured.midiController = index;
        double previous = model::midiControllerDefault(index), first = -1;
        captured.points.push_back({-63072000, previous});
        for (int i = 0; i < take.numControllers; ++i)
        {
            const auto& event = take.controllers[(size_t) i];
            if (event.index != index) continue;
            if (first < 0) first = event.time;
            captured.points.push_back({event.time, previous});
            captured.points.push_back({event.time, (double) event.value});
            previous = event.value;
        }
        if (first < 0) continue;
        auto found = std::find_if(clip.envelopes.begin(), clip.envelopes.end(), [index](const auto& e) { return e.midiController == index; });
        if (! overdub)
        {
            captured.targetId = -1;
            clip.envelopes.push_back(std::move(captured));
            continue;
        }
        if (found == clip.envelopes.end())
        {
            model::AutomationEnvelope env; env.midiController = index;
            env.points = {{-63072000, (double) model::midiControllerDefault(index)}};
            clip.envelopes.push_back(std::move(env)); found = clip.envelopes.end() - 1;
        }
        const AutomationLane input(captured);
        const auto& loop = clip.loop;
        const double span = loop.loopEnd - loop.loopStart;
        int cursor = 0;
        while (cursor < take.numControllers)
        {
            while (cursor < take.numControllers && take.controllers[(size_t) cursor].index != index) ++cursor;
            if (cursor == take.numControllers) break;
            const double source = take.controllers[(size_t) cursor].time;
            if (source >= lengthBeats_ - 1e-12) break;
            double destination = take.contentStart + source;
            if (loop.enabled && span > 0 && destination >= loop.loopEnd)
                destination = loop.loopStart + std::fmod(destination - loop.loopEnd, span);
            const double count = loop.enabled && span > 0 ? std::min(lengthBeats_ - source, loop.loopEnd - destination) : lengthBeats_ - source;
            if (count <= 1e-12) break;
            const double end = destination + count;
            const AutomationLane old(*found);
            std::erase_if(found->points, [destination, end](const auto& p) { return p.time >= destination && p.time <= end; });
            found->points.push_back({destination, old.valueAt(std::nextafter(destination, -std::numeric_limits<double>::infinity()))});
            found->points.push_back({destination, input.valueAt(source)});
            for (const auto& p : captured.points)
                if (p.time > source && p.time < source + count)
                    found->points.push_back({destination + (p.time - source), p.value});
            found->points.push_back({end, input.valueAt(std::nextafter(source + count, -std::numeric_limits<double>::infinity()))});
            found->points.push_back({end, old.valueAt(end)});
            std::stable_sort(found->points.begin(), found->points.end(), [](const auto& a, const auto& b) { return a.time < b.time; });
            const double stop = source + count;
            do { ++cursor; } while (cursor < take.numControllers && take.controllers[(size_t) cursor].time < stop - 1e-12);
        }
    }
}

Recorder::Result Recorder::takeResult()
{
    Result result;
    if (! finished()) return result;
    stopThread(10000);
    collectPreviews();
    delete pendingPreview_.exchange(nullptr, std::memory_order_acq_rel);
    if (failed_.load()) result.warning = "Recording stopped: audio disk buffer, MIDI note/controller, loop-pass or tempo-map capacity was exceeded, or WAV writing failed";
    else if (discontinuity_.load()) result.warning = "Recording ended at a loop boundary or timeline seek";
    if (started_ && lengthBeats_ > 0)
        for (auto& take : takes_)
        {
            if (take->fifo && take->frames == 0) continue;
            auto& clip = take->clip;
            if (take->overdub)
            {
                if (take->numNotes == 0 && take->numControllers == 0) continue;
                auto merged = *take->overdub;
                const auto& loop = merged.loop;
                const double span = loop.loopEnd - loop.loopStart;
                for (int i = 0; i < take->numNotes; ++i)
                {
                    auto note = take->notes[(size_t) i];
                    note.start += take->contentStart;
                    if (loop.enabled && span > 0)
                    {
                        if (note.start >= loop.loopEnd) note.start = loop.loopStart + std::fmod(note.start - loop.loopEnd, span);
                        note.duration = std::min(note.duration, span);
                    }
                    // Overdub replaces overlapping notes of the same pitch, retaining the other voices.
                    std::erase_if(merged.notes, [&](const model::MidiNote& old) {
                        if (old.pitch != note.pitch) return false;
                        if (old.start < note.start + note.duration && old.start + old.duration > note.start) return true;
                        return loop.enabled && note.start + note.duration > loop.loopEnd
                            && old.start < loop.loopStart + note.start + note.duration - loop.loopEnd;
                    });
                    merged.notes.push_back(note);
                }
                mergeControllers(*take, merged, true);
                std::stable_sort(merged.notes.begin(), merged.notes.end(), [](const model::MidiNote& a, const model::MidiNote& b) { return a.start < b.start; });
                result.session.push_back({take->track, take->scene, take->expectedUid, std::move(merged)});
                continue;
            }
            clip.currentStart = clip.time = startBeat_;
            clip.currentEnd = startBeat_ + lengthBeats_;
            clip.loop.loopStart = clip.loop.startRelative = 0;
            if (take->fifo)
            {
                clip.lengthSamples = take->frames;
                clip.warped = true; clip.warpMode = model::WarpMode::Beats; // observed default for new recorded clips
                clip.loop.loopEnd = lengthBeats_;
                clip.warpMarkers.assign(tempoMarks_.begin(), tempoMarks_.begin() + numTempoMarks_);
                clip.warpMarkers.push_back({(double) totalFrames_ / sampleRate_, lengthBeats_});
                if (! clip.sourceXml) clip.sourceXml = std::make_shared<const std::string>(io::audioClipTemplate(clip));
            }
            else
            {
                clip.loop.loopEnd = lengthBeats_;
                take->notes.resize((size_t) take->numNotes);
                clip.notes = std::move(take->notes);
                mergeControllers(*take, clip, false);
            }
            if (arrangement_)
            {
                auto sourceNotes = std::move(clip.notes); // each lane allocates only its own notes, not the full recording capacity
                std::vector<model::Clip> passClips;
                std::vector<model::WarpMarker> map(tempoMarks_.begin(), tempoMarks_.begin() + numTempoMarks_);
                map.push_back({(double) totalFrames_ / sampleRate_, lengthBeats_});
                for (int i = 0; i < numPasses_; ++i)
                {
                    const auto& pass = passes_[(size_t) i];
                    if (pass.length <= 0) continue;
                    auto part = clip;
                    part.currentStart = part.time = pass.timeline; part.currentEnd = pass.timeline + pass.length;
                    part.loop = {pass.source, pass.source + pass.length, 0, false};
                    part.name += " " + std::to_string(i + 1);
                    if (take->fifo)
                    {
                        part.warped = true; part.warpMode = model::WarpMode::Beats;
                        part.warpMarkers = map; // one continuous file, including tempo changes
                    }
                    else
                    {
                        part.notes.clear();
                        for (const auto& note : sourceNotes)
                        {
                            const double first = std::max(note.start, pass.source), last = std::min(note.start + note.duration, pass.source + pass.length);
                            if (last <= first + 1e-9) continue;
                            auto n = note; n.start = first; n.duration = last - first; part.notes.push_back(n);
                        }
                    }
                    passClips.push_back(std::move(part));
                }
                result.passes.emplace_back(take->track, std::move(passClips));
                clip.notes = std::move(sourceNotes);
            }
            if (! arrangement_) clip.loop.enabled = true;
            if (take->scene >= 0) result.session.push_back({take->track, take->scene, -1, std::move(clip)});
            else result.clips.emplace_back(take->track, std::move(clip));
        }
    for (const auto& take : takes_)
        if (take->fifo && take->frames == 0)
            if (! juce::File(take->clip.sample.absolutePath).deleteFile())
                result.warning += (result.warning.empty() ? "" : "; ") + std::string("Could not remove an empty recording WAV");
    takes_.clear();
    if (result.session.empty())
    {
        discardPreview_.store(true, std::memory_order_release);
    }
    state_.store(State::Idle, std::memory_order_release);
    return result;
}

} // namespace sonora::engine
