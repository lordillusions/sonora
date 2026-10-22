#include "engine/Quantisation.h"
#include "engine/AudioEngine.h"

#include "engine/ClipLauncher.h"

#include "core/TimeUtils.h"
#include "core/Edit.h"
#include "midi/MidiClockOut.h"
#include "midi/MidiManager.h"
#include "plugins/PluginHost.h"

namespace sonora::engine
{

AudioEngine::AudioEngine(plugins::PluginHost& host, midi::MidiManager& midi) : host_(host), midi_(midi)
{
    liveMidi_.ensureSize(midi::InputBlock::bufferCapacity);
    samples_.setLoaderPool(&loaderPool_);
    startTimerHz(10);
}

AudioEngine::~AudioEngine()
{
    lifetime_->store(false);
    stopTimer();
    deviceManager_.removeAudioCallback(this);
    deviceManager_.closeAudioDevice();
    recorder_.deviceStopped();
    delete pendingPreview_.exchange(nullptr);
    delete retiredPreview_.exchange(nullptr);
    delete currentPreview_;
    delete pending_.exchange(nullptr);
    delete retired_.exchange(nullptr);
    delete current_;
    current_ = nullptr;
}

juce::String AudioEngine::initialiseAudio(const juce::XmlElement* savedState)
{
    // Open inputs only when audio recording is requested.
    auto error = deviceManager_.initialise(0, 2, savedState, true);
    deviceManager_.addAudioCallback(this);
    return error;
}

int AudioEngine::inputChannelCount() const
{
    if (auto* device = deviceManager_.getCurrentAudioDevice()) return device->getInputChannelNames().size();
    return 0;
}

juce::String AudioEngine::enableRecordingInputs(int requiredChannels)
{
    auto setup = deviceManager_.getAudioDeviceSetup();
    if (setup.inputDeviceName.isEmpty())
        if (auto* type = deviceManager_.getCurrentDeviceTypeObject())
        {
            const auto names = type->getDeviceNames(true);
            if (! names.isEmpty()) setup.inputDeviceName = names[0];
        }
    if (setup.inputDeviceName.isEmpty()) return "No audio input device is available";
    setup.useDefaultInputChannels = false;
    // Keep channel indices physical: enable a contiguous prefix, never a compact sparse mapping.
    setup.inputChannels.clear();
    setup.inputChannels.setRange(0, std::clamp(requiredChannels, 1, 64), true);
    return deviceManager_.setAudioDeviceSetup(setup, true);
}

std::vector<std::string> AudioEngine::loadSong(const model::Song& song)
{
    if (lastSong_.get() != &song)
        lastSong_ = std::make_unique<model::Song>(song);

    samples_.cancelQueued(); // the previous song's pending loads are no longer needed

    devices::BuildContext ctx;
    ctx.pluginHost = &host_;
    ctx.sampleRate = sampleRate_;
    ctx.blockSize = blockSize_;
    ctx.placeholderForMissingInstruments = placeholderInstruments;

    auto result = buildGraph(*lastSong_, ctx, samples_);

    transport_.setTempo(song.tempo);
    transport_.setTimeSignature(song.timeSigNumerator, song.timeSigDenominator);
    transport_.setLoop(song.loopOn, song.loopStart, song.loopLength);
    transport_.setPosition(0.0);

    installGraph(std::move(result.graph));
    host_.clearPreloaded(); // instances the build did not claim
    return result.log;
}

bool AudioEngine::preloadPlugin(const model::Device& device)
{
    const bool plugin = device.kind == model::DeviceKind::PluginVst3 || device.kind == model::DeviceKind::PluginAu
                        || device.kind == model::DeviceKind::PluginVst2;
    if (! plugin || ! device.plugin) return false;
    const auto match = host_.findMatch(*device.plugin, device.kind);
    return match && host_.preload(*match, *device.plugin, device.kind, sampleRate_, blockSize_);
}

void AudioEngine::installGraph(std::unique_ptr<EngineGraph> graph)
{
    graph->prepare(sampleRate_, blockSize_);
    controls_ = graph->controls;
    latest_ = graph.get();

    delete pending_.exchange(graph.release(), std::memory_order_acq_rel);

    if (deviceManager_.getCurrentAudioDevice() == nullptr)
    {
        // No audio thread running: we may swap directly.
        if (auto* p = pending_.exchange(nullptr))
        {
            delete current_;
            current_ = p;
        }
    }
}

void AudioEngine::timerCallback()
{
    recorder_.collectRetiredPreviews();
    delete retired_.exchange(nullptr, std::memory_order_acq_rel);
    delete retiredPreview_.exchange(nullptr, std::memory_order_acq_rel);
}

std::vector<std::string> AudioEngine::updateTrackDevices(int trackIndex, const model::Song& song)
{
    std::vector<std::string> log;
    const auto* track = model::trackAt(song, trackIndex);
    const auto* before = lastSong_ ? model::trackAt(*lastSong_, trackIndex) : nullptr;
    if (! latest_ || ! track || ! before || trackIndex < 0 || (size_t) trackIndex >= latest_->nodes.size()) return log;
    const juce::ScopedValueSetter<bool> changing(changingDevices_, true);
    auto& node = latest_->nodes[(size_t) trackIndex];
    devices::BuildContext context;
    context.song = &song;
    context.samples = &samples_;
    context.pluginHost = &host_;
    context.log = &log;
    context.sampleRate = sampleRate_;
    context.blockSize = blockSize_;
    context.placeholderForMissingInstruments = placeholderInstruments;
    std::vector<std::unique_ptr<devices::DeviceProcessor>> inserted(track->devices.size());
    std::vector<devices::DeviceProcessor*> pointers;
    for (size_t i = 0; i < track->devices.size(); ++i)
    {
        const auto& device = track->devices[i];
        devices::DeviceProcessor* reused = nullptr;
        for (size_t j = 0; j < before->devices.size() && j < node.processorForModelDevice.size(); ++j)
            if (before->devices[j].id == device.id && before->devices[j].type == device.type)
            {
                reused = node.processorForModelDevice[j];
                break;
            }
        if (! reused)
        {
            inserted[i] = devices::createDevice(device, context);
            if (inserted[i]) inserted[i]->prepare(sampleRate_, blockSize_);
            reused = inserted[i].get();
        }
        pointers.push_back(reused);
    }
    const bool running = deviceManager_.getCurrentAudioDevice() != nullptr;
    if (running) deviceManager_.removeAudioCallback(this); // waits for any running callback
    if (auto* pending = pending_.exchange(nullptr))
    {
        delete current_;
        current_ = pending;
    }
    std::vector<std::unique_ptr<devices::DeviceProcessor>> devices;
    for (size_t i = 0; i < pointers.size(); ++i)
    {
        if (inserted[i]) devices.push_back(std::move(inserted[i]));
        else if (pointers[i])
            for (auto& existing : node.devices)
                if (existing.get() == pointers[i]) { devices.push_back(std::move(existing)); break; }
    }
    node.processorForModelDevice = std::move(pointers);
    node.devices = std::move(devices);
    std::erase_if(node.sidechains, [&](const auto& link) {
        return std::find(node.processorForModelDevice.begin(), node.processorForModelDevice.end(), link.device)
                   == node.processorForModelDevice.end();
    });
    const bool clipsChanged = node.clipsEdited || node.clipSwap->pending.load() != nullptr;
    delete node.clipSwap->pending.exchange(nullptr);
    delete node.clipSwap->retired.exchange(nullptr);
    auto set = buildClipSet(*track, node, song, samples_, log);
    node.sessionAvailability = set->hasSessionClip;
    node.clipSwap->pending.store(set.release());
    applyPendingClips(node, (*latest_->controls)[(size_t) node.controlIndex]);
    node.clipsEdited = clipsChanged; // inserting an effect keeps currently sounding instrument voices
    delete node.clipSwap->retired.exchange(nullptr);
    latest_->computeDelayCompensation();
    *model::trackAt(*lastSong_, trackIndex) = *track;
    if (running) deviceManager_.addAudioCallback(this);
    if (reprepareQueued_)
    {
        reprepareQueued_ = false;
        queueReprepare();
    }
    return log;
}

void AudioEngine::reprepareGraph()
{
    if (changingDevices_) { reprepareQueued_ = true; return; }
    if (! latest_) return;
    const juce::ScopedValueSetter<bool> changing(changingDevices_, true);
    if (beforeReprepare) beforeReprepare();
    const bool running = deviceManager_.getCurrentAudioDevice() != nullptr;
    if (running) deviceManager_.removeAudioCallback(this);
    if (auto* pending = pending_.exchange(nullptr))
    {
        delete current_;
        current_ = pending;
    }
    latest_->prepare(sampleRate_, blockSize_);
    if (running) deviceManager_.addAudioCallback(this);
    if (afterReprepare) afterReprepare();
    if (reprepareQueued_)
    {
        reprepareQueued_ = false;
        queueReprepare();
    }
}

void AudioEngine::previewSample(const std::string& path)
{
    auto preview = std::make_unique<Preview>();
    if (! path.empty()) preview->sample = samples_.request(path);
    delete retiredPreview_.exchange(nullptr, std::memory_order_acq_rel);
    delete pendingPreview_.exchange(preview.release(), std::memory_order_acq_rel);
}

void AudioEngine::renderPreview(float* const* outputs, int numOutputs, int numSamples)
{
    if (retiredPreview_.load(std::memory_order_acquire) == nullptr)
        if (auto* next = pendingPreview_.exchange(nullptr, std::memory_order_acq_rel))
        {
            retiredPreview_.store(currentPreview_, std::memory_order_release);
            currentPreview_ = next;
        }
    auto* preview = currentPreview_;
    const auto* data = preview && preview->sample ? preview->sample->get() : nullptr;
    if (! data || data->audio.getNumSamples() == 0) { previewPlaying_.store(false); return; }
    const int frames = data->audio.getNumSamples();
    const double ratio = data->sampleRate / sampleRate_;
    for (int i = 0; i < numSamples && preview->position < frames; ++i, preview->position += ratio)
    {
        const int a = (int) preview->position, b = std::min(frames - 1, a + 1);
        const float fraction = (float) (preview->position - a);
        for (int ch = 0; ch < std::min(2, numOutputs); ++ch)
            if (outputs[ch])
            {
                const float* samples = data->audio.getReadPointer(ch);
                outputs[ch][i] += 0.25f * (samples[a] + fraction * (samples[b] - samples[a]));
            }
    }
    previewPlaying_.store(preview->position < frames, std::memory_order_relaxed);
}

void AudioEngine::audioDeviceAboutToStart(juce::AudioIODevice* device)
{
    const double sr = device->getCurrentSampleRate();
    const int bs = device->getCurrentBufferSizeSamples();
    const bool changed = ! juce::approximatelyEqual(sr, sampleRate_) || bs > blockSize_;
    sampleRate_ = sr;
    metronome_.prepare(sr);
    monitor_.prepare(sr);
    blockSize_ = juce::jmax(bs, blockSize_);

    const auto activeInputs = device->getActiveInputChannels();
    inputPhysicalChannels_.fill(-1);
    numPhysicalInputs_ = std::min(64, activeInputs.getHighestBit() + 1);
    int callbackIndex = 0;
    for (int channel = 0; channel < numPhysicalInputs_ && callbackIndex < 64; ++channel)
        if (activeInputs[channel]) inputPhysicalChannels_[(size_t) callbackIndex++] = channel;
    std::vector<bool> active((size_t) numPhysicalInputs_);
    for (int channel = 0; channel < numPhysicalInputs_; ++channel) active[(size_t) channel] = activeInputs[channel];
    audioCapture_.prepare(active, sr);
    if (changed) queueReprepare();
}

void AudioEngine::audioDeviceStopped() { recorder_.deviceStopped(); }

void AudioEngine::queueReprepare()
{
    const std::weak_ptr<std::atomic<bool>> life = lifetime_;
    juce::MessageManager::callAsync([this, life] {
        const auto alive = life.lock();
        if (alive && alive->load() && ! graphPreparedForDevice()) reprepareGraph();
    });
}

// ============================================================================ audio thread

namespace
{
const PreparedTakeLane* auditionedTake(const TrackNode& node)
{
    if (node.auditionLane >= 0)
        for (const auto& take : node.takes) if (take.id == node.auditionLane) return &take;
    return nullptr;
}
const std::vector<PreparedClip>& audibleArrangement(const TrackNode& node)
{
    const auto* take = auditionedTake(node);
    return take ? take->clips : node.clips;
}
}

void AudioEngine::renderClips(TrackNode& node, const BlockSegment* segs, int numSegs, double tempo)
{
    for (int s = 0; s < numSegs; ++s)
        for (const auto& clip : audibleArrangement(node))
        {
            if (clip.kind == model::Clip::Kind::Midi)
                renderMidiClip(clip, segs[s], node.midi, node.refreshMidiControllers, &node.midiControllerOverflow);
            else
                node.audioState.render(clip, segs[s], tempo, node.buffer);
        }
}

void AudioEngine::renderTrackClips(TrackNode& node, TrackControls& c, const BlockSegment* segs, int numSegs, double tempo,
                                   const Transport::BlockStart& bs, int numSamples, const Recorder::CaptureRange* ranges)
{
    if (bs.justStopped)
    {
        // Stopping the transport stops every session clip (the arrangement stays overridden until
        // "Back to Arrangement", like in Live).
        node.sessionElapsed = 0;
        c.sessionElapsed.store(0, std::memory_order_relaxed);
        c.playingSlot.store(TrackControls::kNoSlot, std::memory_order_relaxed);
        c.queuedSlot.store(TrackControls::kNothingQueued, std::memory_order_relaxed);
        c.repeatSlot.store(TrackControls::kNoSlot, std::memory_order_release);
    }

    // A take owns its clock/voices while recording or auditioning; a held button must not restart it.
    if (node.auditionLane >= 0 || recorder_.audioCapturesTrack(node.controlIndex))
        c.repeatSlot.store(TrackControls::kNoSlot, std::memory_order_release);

    // A pending launch / stop takes effect at the next quantisation boundary inside this block.
    int launchOffset = -1, newSlot = TrackControls::kNoSlot;
    const int queued = c.queuedSlot.load(std::memory_order_acquire);
    const float queuedGain = c.queuedLaunchGain.load(std::memory_order_relaxed);
    const int repeatSlot = c.repeatSlot.load(std::memory_order_acquire);
    const double repeatQuant = c.repeatQuant.load(std::memory_order_relaxed);
    bool repeatPending = queued >= 0 && queued == repeatSlot;
    if (queued != TrackControls::kNothingQueued && bs.playing && numSegs > 0)
    {
        const double quant = c.queuedQuant.load(std::memory_order_relaxed);
        for (int s = 0; s < numSegs; ++s)
        {
            const int frame = quantisedSample(segs[s], quant, transport_.loopEnabled(), transport_.loopStart(),
                                               transport_.loopStart() + transport_.loopLength());
            if (frame >= 0 && frame < segs[s].numSamples)
            {
                launchOffset = segs[s].sampleOffset + frame;
                break;
            }
        }
        if (launchOffset >= 0)
            newSlot = queued;
    }

    int slot = c.playingSlot.load(std::memory_order_relaxed);
    double launchBeat = c.launchBeat.load(std::memory_order_relaxed);
    const uint64_t route = c.inputRoute.load(std::memory_order_relaxed);
    const int monitor = c.monitoring.load(std::memory_order_relaxed);
    const auto source = model::inputKind(route);
    const bool validInput = source == model::InputKind::Midi || source == model::InputKind::AudioMono || source == model::InputKind::AudioStereo;
    const bool baseMonitor = validInput && (monitor == 0 || (monitor == 1 && c.armed.load(std::memory_order_relaxed)));
    auto& mask = node.inputMonitorMask;
    std::fill_n(mask.begin(), numSamples, baseMonitor ? 1 : 0);
    if (node.inputMonitored && (! baseMonitor || node.lastInputRoute != route))
    {
        for (int channel = 1; channel <= 16; ++channel) node.midi.addEvent(juce::MidiMessage::allNotesOff(channel), 0);
        releaseMidiControllers(node.midi, 0);
    }
    node.lastInputRoute = route;
    const bool renderEnabled = monitor != 0;
    auto inhibitInput = [&](const PreparedClip& clip, const BlockSegment& segment, double offset, bool releaseInput = true) {
        if (! baseMonitor || monitor != 1) return;
        const double start = clip.currentStart + offset, end = clip.currentEnd + offset;
        if (start >= segment.endBeats() || end <= segment.startBeats) return;
        const int first = segment.sampleOffset + (int) std::floor(std::clamp((start - segment.startBeats) * segment.samplesPerBeat, 0.0, (double) segment.numSamples));
        const int last = segment.sampleOffset + (int) std::ceil(std::clamp((end - segment.startBeats) * segment.samplesPerBeat, 0.0, (double) segment.numSamples));
        if (first < last)
        {
            const bool wasOn = mask[(size_t) first] != 0 && (first > 0 ? mask[(size_t) first - 1] != 0 : node.inputMonitored);
            if (wasOn && releaseInput)
            {
                for (int channel = 1; channel <= 16; ++channel) node.midi.addEvent(juce::MidiMessage::allNotesOff(channel), first);
                releaseMidiControllers(node.midi, first);
            }
            std::fill(mask.begin() + first, mask.begin() + last, 0);
        }
    };

    auto renderPlayback = [&](const BlockSegment& seg, int part, double partLaunch, bool arrangementActive, bool inhibitMonitoring = true) {
        if (! renderEnabled || seg.numSamples <= 0)
            return;
        if (node.auditionLane >= 0)
        {
            if (inhibitMonitoring) for (const auto& clip : audibleArrangement(node)) inhibitInput(clip, seg, 0);
            renderClips(node, &seg, 1, tempo);
        }
        else if (part >= 0 && (size_t) part < node.sessionClips.size() && node.hasSessionClip[(size_t) part] != 0)
        {
            const auto& clip = node.sessionClips[(size_t) part];
            if (inhibitMonitoring) inhibitInput(clip, seg, partLaunch, ! recorder_.hasOverdubPreview(node.controlIndex, part, clip.probabilitySeed));
            const BlockSegment shifted{seg.sampleOffset, seg.numSamples, seg.startBeats - partLaunch, seg.samplesPerBeat};
            if (clip.kind == model::Clip::Kind::Midi)
            {
                const Recorder::CaptureRange* range = nullptr;
                if (ranges) for (int i = 0; i < numSegs; ++i)
                    if ((ranges[i].offset < seg.sampleOffset + seg.numSamples && ranges[i].offset + ranges[i].count > seg.sampleOffset)
                        || (ranges[i].punchEnd && ranges[i].offset + ranges[i].count >= seg.sampleOffset
                            && ranges[i].offset + ranges[i].count < seg.sampleOffset + seg.numSamples))
                    { range = &ranges[i]; break; }
                if (! recorder_.renderOverdub(node.controlIndex, part, clip, shifted, node.inputMidi, range, node.midi,
                                              node.refreshMidiControllers, &node.midiControllerOverflow, node.sessionLaunchGain))
                    renderMidiClip(clip, shifted, node.midi, node.refreshMidiControllers, &node.midiControllerOverflow,
                                   nullptr, nullptr, node.sessionLaunchGain);
            }
            else
            {
                node.audioState.render(clip, shifted, tempo, node.buffer);
                if (node.sessionLaunchGain < 1.0f)
                    node.buffer.applyGain(seg.sampleOffset, seg.numSamples, node.sessionLaunchGain);
            }
        }
        else if (arrangementActive)
        {
            if (inhibitMonitoring) for (const auto& clip : audibleArrangement(node)) inhibitInput(clip, seg, 0);
            renderClips(node, &seg, 1, tempo);
        }
        node.refreshMidiControllers = false;
    };
    auto renderPart = [&](const BlockSegment& seg, int part, double partLaunch, bool arrangementActive) {
        if (part >= 0) partLaunch = seg.startBeats - node.sessionElapsed;
        auto advanceSession = [&] {
            if (part >= 0)
            {
                const double elapsed = seg.numSamples / seg.samplesPerBeat;
                node.sessionElapsed += elapsed;
                if (part == repeatSlot) node.repeatRemaining -= elapsed;
            }
        };
        const int end = seg.sampleOffset + seg.numSamples;
        int recordStart = -1, recordEnd = -1;
        if (ranges) for (int i = 0; i < numSegs; ++i)
            if (ranges[i].count > 0 && ranges[i].offset < end && ranges[i].offset + ranges[i].count > seg.sampleOffset)
            { recordStart = ranges[i].offset; recordEnd = recordStart + ranges[i].count; break; }
        if (recordStart < 0 || recordEnd <= seg.sampleOffset || recordStart >= end)
        { renderPlayback(seg, part, partLaunch, arrangementActive); advanceSession(); return; }
        const int first = std::max(seg.sampleOffset, recordStart), last = std::min(end, recordEnd);
        const bool overdubbing = recorder_.overdubsTrack(node.controlIndex);
        if (recorder_.audioSessionRecording(node.controlIndex))
        {
            recorder_.beginSessionContent(node.controlIndex, node.sessionElapsed + (first - seg.sampleOffset) / seg.samplesPerBeat);
            if (! overdubbing)
            {
                c.arrangementOn.store(false, std::memory_order_relaxed);
                c.playingSlot.store(TrackControls::kNoSlot, std::memory_order_relaxed);
                slot = TrackControls::kNoSlot;
            }
        }
        renderPlayback({seg.sampleOffset, first - seg.sampleOffset, seg.startBeats, seg.samplesPerBeat}, part, partLaunch, arrangementActive);
        if (overdubbing)
        {
            renderPlayback({first, last - first, seg.startBeats + (first - seg.sampleOffset) / seg.samplesPerBeat, seg.samplesPerBeat}, part, partLaunch, arrangementActive, false);
            if (baseMonitor) std::fill(mask.begin() + first, mask.begin() + last, 1);
        }
        const bool inputWasAudible = baseMonitor && (first > 0 ? mask[(size_t) first - 1] != 0 : node.inputMonitored);
        if (! overdubbing && node.kind == model::TrackKind::Midi && ! inputWasAudible && (! node.recordingClipSuppressed || first > 0))
            for (int channel = 1; channel <= 16; ++channel)
                node.midi.addEvent(juce::MidiMessage::allNotesOff(channel), first);
        if (! recorder_.audioSessionRecording(node.controlIndex) || overdubbing)
            renderPlayback({last, end - last, seg.startBeats + (last - seg.sampleOffset) / seg.samplesPerBeat, seg.samplesPerBeat}, part, partLaunch, arrangementActive);
        advanceSession();
    };

    for (int s = 0; s < numSegs; ++s)
    {
        const auto& seg = segs[s];
        const int end = seg.sampleOffset + seg.numSamples;
        int cursor = seg.sampleOffset;
        while (cursor < end)
        {
            int boundary = end;
            bool launching = launchOffset >= cursor && launchOffset < end;
            if (launching) boundary = launchOffset;
            const double interval = std::max(repeatQuant, 1.0 / seg.samplesPerBeat);
            if (! repeatPending && slot >= 0 && slot == repeatSlot && repeatQuant > 0.0)
            {
                // Preserve fractional-sample overshoot: repeated launches must not accumulate drift.
                const int remaining = (int) std::ceil(std::clamp(node.repeatRemaining * seg.samplesPerBeat - 1e-8,
                                                                0.0, (double) (end - cursor)));
                if (cursor + remaining < boundary)
                { boundary = cursor + remaining; launching = false; }
            }
            const double position = seg.startBeats + (cursor - seg.sampleOffset) / seg.samplesPerBeat;
            renderPart({cursor, boundary - cursor, position, seg.samplesPerBeat}, slot, launchBeat,
                       c.arrangementOn.load(std::memory_order_relaxed));
            if (boundary == end) break;

            // switch: release whatever the old clip was playing, then continue with the new state
            if (renderEnabled) { for (int channel = 1; channel <= 16; ++channel) node.midi.addEvent(juce::MidiMessage::allNotesOff(channel), boundary); releaseMidiControllers(node.midi, boundary); }
            node.audioState.reset();
            node.refreshMidiControllers = true;
            // Legato: the new clip continues at the position the previous one had reached.
            // A held Repeat retrigger always restarts the content, including when Legato is enabled.
            const bool legato = launching && slot >= 0 && newSlot >= 0 && (size_t) newSlot < node.sessionClips.size()
                                && node.hasSessionClip[(size_t) newSlot] != 0 && node.sessionClips[(size_t) newSlot].legato;
            const double carried = legato ? node.sessionElapsed : 0.0;
            node.sessionElapsed = carried;
            if (launching)
            {
                slot = newSlot;
                node.sessionLaunchGain = queuedGain;
                node.repeatRemaining = repeatQuant;
                repeatPending = false;
                c.queuedSlot.store(TrackControls::kNothingQueued, std::memory_order_release);
                launchOffset = -1;
            }
            else
                node.repeatRemaining += interval;
            // The clip's beat 0 is the sample the launch lands on (not the exact boundary, which can
            // lie a fraction of a sample earlier and would clip the clip's first note).
            launchBeat = seg.startBeats + (boundary - seg.sampleOffset) / seg.samplesPerBeat - carried;
            c.playingSlot.store(slot, std::memory_order_relaxed);
            c.launchBeat.store(launchBeat, std::memory_order_relaxed);
            if (slot >= 0)
                c.arrangementOn.store(false, std::memory_order_relaxed);
            cursor = boundary;
        }
    }
    c.sessionElapsed.store(node.sessionElapsed, std::memory_order_relaxed);
    node.inputMonitored = numSamples > 0 && mask[(size_t) numSamples - 1] != 0;
    node.recordingClipSuppressed = ranges && numSegs > 0 && ranges[numSegs - 1].count > 0
                                  && ranges[numSegs - 1].offset + ranges[numSegs - 1].count >= numSamples;
}

void AudioEngine::applyPendingClips(TrackNode& node, TrackControls& controls)
{
    auto& swap = *node.clipSwap;
    if (swap.retired.load(std::memory_order_acquire) != nullptr)
        return; // the message thread has not released the previously replaced set yet
    if (auto* set = swap.pending.exchange(nullptr, std::memory_order_acq_rel))
    {
        // Retired bindings cannot restore a value later, so release active envelope overrides first.
        auto restore = [&](std::vector<ClipEnvelopeBinding>& bindings) {
            for (auto& b : bindings) if (b.active) { applyBindingValue(b.target, controls, b.baseValue); b.active = false; }
        };
        if (! set->takesOnly || node.auditionLane != set->auditionLane) restore(node.clipEnvelopes);
        for (auto& take : node.takes) restore(take.envelopes);
        if (! set->takesOnly || node.auditionLane != set->auditionLane) restore(node.sessionEnvelopes);
        const bool audibleChanged = ! set->takesOnly || node.auditionLane >= 0 || set->auditionLane >= 0;
        if (! set->takesOnly)
        {
            std::swap(node.clips, set->arrangement);
            std::swap(node.automation, set->automation);
            std::swap(node.midiControllers, set->midiControllers);
            std::swap(node.sessionClips, set->session);
            std::swap(node.hasSessionClip, set->hasSessionClip);
            std::swap(node.sessionEnvelopes, set->sessionEnvelopes);
            std::swap(node.clipEnvelopes, set->envelopes);
        }
        std::swap(node.takes, set->takes);
        node.auditionLane = set->auditionLane;
        controls.auditionLane.store(node.auditionLane, std::memory_order_relaxed);
        if (audibleChanged)
        {
            node.clipsEdited = true;
            node.audioState.reset(); // a sounding clip may belong to the retired set
        }
        swap.retired.store(set, std::memory_order_release);
    }
}

void AudioEngine::updateTrackClips(int trackIndex, const model::Song& song, bool takesOnly)
{
    auto* graph = latest_;
    const model::Track* track = nullptr;
    const int nt = (int) song.tracks.size(), nr = (int) song.returns.size();
    if (trackIndex >= 0 && trackIndex < nt)
        track = &song.tracks[(size_t) trackIndex];
    else if (trackIndex >= nt && trackIndex < nt + nr)
        track = &song.returns[(size_t) (trackIndex - nt)];
    else if (trackIndex == nt + nr)
        track = &song.main;
    if (graph == nullptr || track == nullptr || (size_t) trackIndex >= graph->nodes.size())
        return;

    if (lastSong_)
        if (auto* savedTrack = model::trackAt(*lastSong_, trackIndex))
            *savedTrack = *track;

    auto& node = graph->nodes[(size_t) trackIndex];
    auto& controls = (*graph->controls)[(size_t) node.controlIndex];
    controls.inputRoute.store(model::packInput(model::parseInput(*track)), std::memory_order_relaxed);
    controls.monitoring.store(track->monitoring, std::memory_order_relaxed);
    auto& swap = *node.clipSwap;
    // Coalescing a lane edit with an unpublished main edit must preserve that main edit.
    auto* previous = swap.pending.exchange(nullptr, std::memory_order_acq_rel);
    if (previous && ! previous->takesOnly) takesOnly = false;
    delete previous;
    delete swap.retired.exchange(nullptr, std::memory_order_acq_rel);
    std::vector<std::string> log;
    auto set = buildClipSet(*track, node, song, samples_, log, takesOnly);
    if (! takesOnly) node.sessionAvailability = set->hasSessionClip;
    delete swap.pending.exchange(set.release(), std::memory_order_acq_rel);

}

void AudioEngine::applyClipEnvelopes(TrackNode& node, TrackControls& c, double beat)
{
    auto handle = [&](ClipEnvelopeBinding& b) {
        const PreparedClip* clip = nullptr;
        double timeInClip = 0.0;
        if (b.session)
        {
            if (node.auditionLane < 0 && c.playingSlot.load(std::memory_order_relaxed) == b.clipIndex && (size_t) b.clipIndex < node.sessionClips.size())
            {
                clip = &node.sessionClips[(size_t) b.clipIndex];
                timeInClip = c.sessionElapsed.load(std::memory_order_relaxed);
            }
        }
        else if ((node.auditionLane >= 0 || c.arrangementOn.load(std::memory_order_relaxed)) && (size_t) b.clipIndex < audibleArrangement(node).size())
        {
            const auto& candidate = audibleArrangement(node)[(size_t) b.clipIndex];
            if (beat >= candidate.currentStart && beat < candidate.currentEnd)
            {
                clip = &candidate;
                timeInClip = beat - candidate.currentStart;
            }
        }

        if (clip != nullptr)
        {
            // Envelope time is in the clip's content (loop region) beats.
            const double content = model::clipContentPosition(timeInClip, 0.0, clip->loop.loopStart, clip->loop.loopEnd,
                                                              clip->loop.startRelative, clip->loop.enabled);
            applyBindingValue(b.target, c, b.target.lane.valueAt(content));
            b.active = true;
        }
        else if (b.active)
        {
            applyBindingValue(b.target, c, b.baseValue); // the clip stopped: the parameter goes back
            b.active = false;
        }
    };
    // Returning to main restores targets that were driven only by the auditioned take.
    auto* take = const_cast<PreparedTakeLane*>(auditionedTake(node));
    for (auto& b : node.clipEnvelopes)
        if (! take) handle(b);
        else if (b.active) { applyBindingValue(b.target, c, b.baseValue); b.active = false; }
    for (auto& lane : node.takes) for (auto& b : lane.envelopes)
        if (&lane == take) handle(b);
        else if (b.active) { applyBindingValue(b.target, c, b.baseValue); b.active = false; }
    for (auto& b : node.sessionEnvelopes)
        handle(b);
}

void AudioEngine::copyTap(juce::AudioBuffer<float>& tap, const juce::AudioBuffer<float>& src, int n)
{
    for (int ch = 0; ch < 2; ++ch)
        tap.copyFrom(ch, 0, src, ch, 0, n);
}

// Points every sidechain-capable device of `node` at the tapped audio of the track it listens to.
void AudioEngine::feedSidechains(EngineGraph& g, TrackNode& node, int)
{
    for (const auto& link : node.sidechains)
    {
        const auto& source = g.nodes[(size_t) link.sourceNode];
        link.device->setSidechain(source.needTap[(size_t) link.tap] ? &source.tap[(size_t) link.tap] : nullptr, link.gain);
    }
}

void AudioEngine::processNode(TrackNode& node, const TrackControls& ctl, int n, const devices::HostInfo& host)
{
    // The node buffer is allocated for the largest block; devices must only see (and process) the
    // `n` samples of this callback. Handing them the whole buffer made every device run its filters
    // over uninitialised samples whenever the audio device used a smaller buffer than that.
    juce::AudioBuffer<float> view(node.buffer.getArrayOfWritePointers(), 2, n);
    for (auto& d : node.devices)
    {
        d->setHostInfo(host);
        const float inMag = debugCheckDevices ? std::max(view.getMagnitude(0, 0, n), view.getMagnitude(1, 0, n)) : 0.0f;
        d->process(view, node.midi);
        if (debugCheckDevices) // diagnostics only: not for production use on the audio thread
        {
            const float m = std::max(view.getMagnitude(0, 0, n), view.getMagnitude(1, 0, n));
            if (! std::isfinite(m) || m > 50.0f)
            {
                if (onBadAudio)
                    onBadAudio(node.name, d->name(), inMag, m);
                view.clear();
            }
        }
    }

    if (offlinePreMixer_ && offlineStemSlot_)
    {
        const int slot = (*offlineStemSlot_)[(size_t) node.controlIndex];
        if (slot >= 0) copyTap(offlineStemBuffers_[(size_t) slot], node.buffer, n);
    }
    if (node.needTap[1]) // sidechain tap: post-FX, before the mixer
        copyTap(node.tap[1], node.buffer, n);

    // Pan: balance-style law, the far channel is attenuated and the near one stays at unity
    // (matches Live for stereo tracks; TODO(ENG-4b): confirm against Live-rendered audio).
    // The gain ramps linearly across the block so fader/pan moves and automation don't zipper.
    const auto gains = panGains(ctl.volume.load(std::memory_order_relaxed), ctl.pan.load(std::memory_order_relaxed));
    const float gainL = gains.left, gainR = gains.right;
    if (node.lastGainL < 0.0f)
        node.lastGainL = gainL, node.lastGainR = gainR;
    node.buffer.applyGainRamp(0, 0, n, node.lastGainL, gainL);
    node.buffer.applyGainRamp(1, 0, n, node.lastGainR, gainR);
    node.lastGainL = gainL;
    node.lastGainR = gainR;
}

void AudioEngine::audioDeviceIOCallbackWithContext(const float* const* inputs, int numInputs, float* const* outputs, int numOutputs,
                                                   int numSamples, const juce::AudioIODeviceCallbackContext&)
{
    std::array<const float*, 64> physical{};
    for (int channel = 0; channel < std::min(64, numInputs); ++channel)
        if (const int index = inputPhysicalChannels_[(size_t) channel]; index >= 0 && inputs)
            physical[(size_t) index] = inputs[channel];
    renderBlock(outputs, numOutputs, numSamples, physical.data(), numPhysicalInputs_);
    audioCapture_.write(physical.data(), numPhysicalInputs_, numSamples); // live device input only, never offline renders
}

void AudioEngine::renderBlock(float* const* outputs, int numOutputs, int numSamples,
                              const float* const* inputs, int numInputs)
{
    if (numSamples <= 0) return;
    juce::ScopedNoDenormals noDenormals;

    if (retired_.load(std::memory_order_acquire) == nullptr)
        if (auto* p = pending_.exchange(nullptr, std::memory_order_acq_rel))
        {
            retired_.store(current_, std::memory_order_release);
            current_ = p;
        }

    for (int ch = 0; ch < numOutputs; ++ch)
        if (outputs[ch] != nullptr)
            juce::FloatVectorOperations::clear(outputs[ch], numSamples);

    liveMidi_.clear();
    midi_.popForAudio(liveMidi_, numSamples, sampleRate_, &inputMidiBlock_);

    auto* g = current_;
    if (g == nullptr || numSamples > g->nodes.front().buffer.getNumSamples())
    {
        renderPreview(outputs, numOutputs, numSamples);
        return;
    } // graph not ready or prepared for a smaller block (rebuild is pending)

    recorder_.beginBlock();
    auto& ctl = *g->controls;
    for (auto& node : g->nodes)
        applyPendingClips(node, ctl[(size_t) node.controlIndex]);

    const auto bs = transport_.beginBlock(numSamples, sampleRate_ * 60.0 / transport_.tempo());
    if (bs.justStopped) recorder_.requestStop();
    // Tempo automation (main track) is evaluated at the block start.
    if (bs.playing || bs.jumped)
    {
        const auto& mainTrack = g->nodes[(size_t) g->mainNode];
        for (const auto& b : mainTrack.automation)
            if (b.kind == AutomationBinding::Kind::Tempo)
                applyTempoAutomation(b, ctl[(size_t) mainTrack.controlIndex], transport_, bs.position);
    }
    const double tempo = transport_.tempo();
    const double samplesPerBeat = sampleRate_ * 60.0 / tempo;
    devices::HostInfo host{tempo, bs.position, bs.playing, sampleRate_};
    host.timeSigNumerator = transport_.timeSigNumerator(); host.timeSigDenominator = transport_.timeSigDenominator();
    host.looping = transport_.loopEnabled();
    host.loopStart = transport_.loopStart(); host.loopEnd = transport_.loopStart() + transport_.loopLength();

    // ---- split the block at the loop end
    constexpr int maxSegments = RecordingManager::maxSegments; // bounded even for pathological sub-sample loop regions (ENG-14)
    BlockSegment segs[maxSegments];
    bool wraps[maxSegments]{};
    int numSegs = 0;
    double endPos = bs.position;
    if (bs.playing)
    {
        const double loopStart = transport_.loopStart();
        const double loopEnd = loopStart + transport_.loopLength();
        const bool looping = transport_.loopEnabled() && bs.position <= loopEnd + 1e-9;
        int offset = bs.playOffset;
        double beat = bs.position;
        if (looping && beat >= loopEnd - 1e-9) beat = loopStart;
        while (offset < numSamples && numSegs < maxSegments)
        {
            int count = numSamples - offset;
            if (looping)
                count = (int) std::ceil(std::clamp((loopEnd - beat) * samplesPerBeat - 1e-8, 1.0, (double) count));
            segs[numSegs] = {offset, count, beat, samplesPerBeat};
            offset += count; beat += count / samplesPerBeat;
            if (looping && beat >= loopEnd - 1e-9)
            {
                wraps[numSegs] = true;
                const double span = transport_.loopLength();
                double phase = std::fmod(std::max(0.0, beat - loopEnd), span);
                if (phase < 1e-9 || span - phase < 1e-9) phase = 0; // modulo can round an exact boundary to span
                beat = loopStart + phase;
            }
            ++numSegs;
        }
        endPos = beat;
        if (offset < numSamples) { recorder_.requestStop(); transport_.stop(); } // capacity guard, no allocation/logging
    }

    recorder_.prepareBlock(segs, numSegs, bs.playing, bs.jumped);

    if (auto* clock = clockOut_.load(std::memory_order_relaxed))
    {
        const double blockStart = juce::Time::getMillisecondCounterHiRes() * 0.001;
        clock->transportBlock({bs.playing, bs.justStopped, bs.position}, blockStart + bs.playOffset / sampleRate_);
        if (bs.playing)
            for (int s = 0; s < numSegs; ++s)
                clock->clockSegment(segs[s].startBeats, segs[s].sampleOffset, segs[s].numSamples, samplesPerBeat,
                                    sampleRate_, blockStart);
    }

    bool anySolo = false;
    for (size_t i = 0; i < ctl.size(); ++i)
        anySolo |= ctl[i].solo.load(std::memory_order_relaxed);

    for (auto& node : g->nodes)
    {
        node.buffer.clear(0, numSamples);
        node.midi.clear();
        node.inputMidi.clear();
    }

    recorder_.beginPreviewBlock(*g);
    auto& main = g->nodes[(size_t) g->mainNode];
    const bool releaseAll = bs.justStopped || bs.jumped;

    // Automation overwrites the controls while playing and right after a locate (ENG-11).
    // Evaluated once per block at the block start.
    const bool automate = bs.playing || bs.jumped;
    auto automationFor = [&](TrackNode& node) {
        auto& controls = ctl[(size_t) node.controlIndex];
        // Only hand edits made while playing override a lane: after a stop (or a locate while stopped) the next start rewrites everything.
        if (! bs.playing)
            controls.automationWrote.reset();
        if (! automate)
            return;
        if (! node.automation.empty())
        {
            applyAutomation(node.automation, controls, bs.position);
            if (! bs.playing)
                controls.automationWrote.reset();
        }
        if (! node.clipEnvelopes.empty() || ! node.sessionEnvelopes.empty() || ! node.takes.empty())
            applyClipEnvelopes(node, controls, bs.position);
    };

    // ---- tracks (children before their groups)
    for (int idx : g->trackOrder)
    {
        auto& node = g->nodes[(size_t) idx];
        auto& c = ctl[(size_t) node.controlIndex];

        node.midiControllerOverflow = false;
        if (bs.playing && ! node.midiWasPlaying) node.refreshMidiControllers = true;
        node.midiWasPlaying = bs.playing;
        automationFor(node);
        if (node.clipsEdited)
        {
            const int scene = c.playingSlot.load(std::memory_order_relaxed);
            const PreparedClip* replacement = scene >= 0 && (size_t) scene < node.sessionClips.size() && node.hasSessionClip[(size_t) scene]
                                              ? &node.sessionClips[(size_t) scene] : nullptr;
            if (recorder_.commitOverdub(node.controlIndex, scene, replacement, node.sessionElapsed, node.midi) && ! releaseAll)
            {
                node.clipsEdited = false; node.inputMonitored = false; node.refreshMidiControllers = true;
            }
        }
        if (releaseAll || node.clipsEdited)
        {
            node.clipsEdited = false; node.refreshMidiControllers = true;
            releaseMidiControllers(node.midi, 0);
            for (int channel = 1; channel <= 16; ++channel) node.midi.addEvent(juce::MidiMessage::allNotesOff(channel), 0);
            node.audioState.reset();
        }
        if (! recorder_.audioCapturesTrack(idx) && c.playingSlot.load(std::memory_order_relaxed) < 0 && c.queuedSlot.load(std::memory_order_relaxed) < 0) for (int i = 1; i < numSegs; ++i)
        {
            for (int channel = 1; channel <= 16; ++channel) node.midi.addEvent(juce::MidiMessage::allNotesOff(channel), segs[i].sampleOffset);
            releaseMidiControllers(node.midi, segs[i].sampleOffset);
            node.refreshMidiControllers = true;
        }

        const uint64_t route = c.inputRoute.load(std::memory_order_relaxed);
        const auto kind = model::inputKind(route);
        const bool recording = recorder_.audioCapturesTrack(idx);
        if (kind == model::InputKind::Midi && node.kind == model::TrackKind::Midi)
            inputMidiBlock_.addTo(node.inputMidi, model::inputMidiPort(route), model::inputChannel(route));
        if (bs.playing && c.arrangementOn.load(std::memory_order_relaxed) && c.monitoring.load(std::memory_order_relaxed) != 0)
            for (int i = 0; i < numSegs; ++i)
                if (! renderMidiControllers(node.midiControllers, segs[i], node.midi, node.refreshMidiControllers || i > 0)) node.midiControllerOverflow = true;
        renderTrackClips(node, c, segs, numSegs, tempo, bs, numSamples,
                         recording ? recorder_.rangesForTrack(idx) : nullptr);
        if (kind == model::InputKind::Midi && node.kind == model::TrackKind::Midi)
        {
            for (const auto event : node.inputMidi)
                if (event.samplePosition >= 0 && event.samplePosition < numSamples && node.inputMonitorMask[(size_t) event.samplePosition])
                    node.midi.addEvent(event.data, event.numBytes, event.samplePosition);
        }
        if (node.kind == model::TrackKind::Midi)
        {
            const int pitch = c.previewPitch.load(std::memory_order_relaxed);
            if (pitch != node.previewPitch || (releaseAll && pitch >= 0))
            {
                if (node.previewPitch >= 0) node.midi.addEvent(juce::MidiMessage::noteOff(1, node.previewPitch), 0);
                if (pitch >= 0 && pitch <= 127) node.midi.addEvent(juce::MidiMessage::noteOn(1, pitch, (juce::uint8) 100), 0);
                node.previewPitch = pitch;
            }
        }
        if (node.midiControllerOverflow)
        {
            for (int channel = 1; channel <= 16; ++channel) node.midi.addEvent(juce::MidiMessage::allNotesOff(channel), numSamples - 1);
            releaseMidiControllers(node.midi, numSamples - 1);
            node.refreshMidiControllers = true;
            c.midiControllerOverflow.store(true, std::memory_order_relaxed);
        }
        if (node.kind == model::TrackKind::Audio && inputs
            && (kind == model::InputKind::AudioMono || kind == model::InputKind::AudioStereo))
            for (int ch = 0; ch < 2; ++ch)
            {
                const int channel = model::inputChannel(route) + (kind == model::InputKind::AudioMono ? 0 : ch);
                if (channel < numInputs && inputs[channel])
                    for (int sample = 0; sample < numSamples; ++sample)
                        if (node.inputMonitorMask[(size_t) sample]) node.buffer.addSample(ch, sample, inputs[channel][sample]);
            }

        feedSidechains(*g, node, numSamples);
        if (node.needTap[0])
            copyTap(node.tap[0], node.buffer, numSamples);
        if (! node.midi.isEmpty()) c.midiActivity.fetch_add(1, std::memory_order_relaxed);
        processNode(node, c, numSamples, host); // devices, then post-FX tap, then volume / pan
        if (node.needTap[2])
            copyTap(node.tap[2], node.buffer, numSamples);
        if (node.trackDelaySamples > 0)
        {
            juce::AudioBuffer<float> view(node.buffer.getArrayOfWritePointers(), 2, numSamples);
            node.trackDelay.processInPlace(view, numSamples);
        }

        c.peakL.store(node.buffer.getMagnitude(0, 0, numSamples), std::memory_order_relaxed);
        c.peakR.store(node.buffer.getMagnitude(1, 0, numSamples), std::memory_order_relaxed);

        const bool audible = isTrackAudible(*g, ctl, node, anySolo);
        auto& target = node.outputNode >= 0 ? g->nodes[(size_t) node.outputNode].buffer : main.buffer;
        const int stemSlot = offlineStemSlot_ != nullptr ? (*offlineStemSlot_)[(size_t) idx] : -1;
        if (stemSlot >= 0 && ! offlinePreMixer_)
        {
            auto& stem = offlineStemBuffers_[(size_t) stemSlot]; // the delayed signal is captured, then added to the parent as usual
            stem.clear(0, numSamples);
            if (audible && node.outputEnabled)
            {
                node.outDelay.addDelayed(node.buffer, stem, numSamples, 1.0f);
                for (int ch = 0; ch < 2; ++ch)
                    target.addFrom(ch, 0, stem, ch, 0, numSamples);
            }
            else
                node.outDelay.pushSilence(numSamples);
        }
        else if (audible && node.outputEnabled)
            node.outDelay.addDelayed(node.buffer, target, numSamples, 1.0f);
        else
            node.outDelay.pushSilence(numSamples);

        for (size_t r = 0; r < g->returnNodes.size() && r < c.sends.size() && r < node.sendDelays.size(); ++r)
        {
            const float amount = c.sends[r].load(std::memory_order_relaxed);
            if (! audible || amount <= 0.000317f) // Live's -inf (-70 dB)
            {
                node.sendDelays[r].pushSilence(numSamples); // keep the delay line in step
                continue;
            }
            node.sendDelays[r].addDelayed(node.buffer, g->nodes[(size_t) g->returnNodes[r]].buffer, numSamples, amount);
        }
    }

    // ---- returns
    for (int idx : g->returnNodes)
    {
        auto& node = g->nodes[(size_t) idx];
        auto& c = ctl[(size_t) node.controlIndex];
        automationFor(node);
        processNode(node, c, numSamples, host);
        c.peakL.store(node.buffer.getMagnitude(0, 0, numSamples), std::memory_order_relaxed);
        c.peakR.store(node.buffer.getMagnitude(1, 0, numSamples), std::memory_order_relaxed);
        const int stemSlot = offlineStemSlot_ != nullptr ? (*offlineStemSlot_)[(size_t) idx] : -1;
        if (stemSlot >= 0 && ! offlinePreMixer_)
        {
            auto& stem = offlineStemBuffers_[(size_t) stemSlot];
            stem.clear(0, numSamples);
            if (c.active.load(std::memory_order_relaxed))
            {
                node.outDelay.addDelayed(node.buffer, stem, numSamples, 1.0f);
                for (int ch = 0; ch < 2; ++ch)
                    main.buffer.addFrom(ch, 0, stem, ch, 0, numSamples);
            }
            else
                node.outDelay.pushSilence(numSamples);
        }
        else if (c.active.load(std::memory_order_relaxed))
            node.outDelay.addDelayed(node.buffer, main.buffer, numSamples, 1.0f);
        else
            node.outDelay.pushSilence(numSamples);
    }

    // ---- main
    auto& mc = ctl[(size_t) main.controlIndex];
    automationFor(main);
    processNode(main, mc, numSamples, host);
    mc.peakL.store(main.buffer.getMagnitude(0, 0, numSamples), std::memory_order_relaxed);
    mc.peakR.store(main.buffer.getMagnitude(1, 0, numSamples), std::memory_order_relaxed);

    // Internal resampling needs the completed Main signal; external recording remains pre-FX/pre-mixer.
    if (numSegs == 0) recorder_.capture(inputs, numInputs, liveMidi_, bs.position, samplesPerBeat, numSamples,
                                      bs.playing && bs.countInSamples == 0, bs.jumped, false, g);
    else for (int i = 0; i < numSegs; ++i)
        recorder_.capture(inputs, numInputs, liveMidi_, segs[i].startBeats, samplesPerBeat, segs[i].numSamples,
                          bs.playing, i == 0 && bs.jumped, wraps[i], g, segs[i].sampleOffset);

    for (int ch = 0; ch < juce::jmin(2, numOutputs); ++ch)
        if (outputs[ch] != nullptr)
            juce::FloatVectorOperations::copy(outputs[ch], main.buffer.getReadPointer(ch), numSamples);
    monitor_.push(main.buffer.getReadPointer(0), main.buffer.getReadPointer(1), numSamples); // Monitor view: Main, before click and preview

    // Metronome: after the main bus, only while the transport runs.
    if ((bs.countInSamples > 0 || (bs.playing && transport_.metronomeOn())) && numOutputs >= 2 && outputs[0] != nullptr && outputs[1] != nullptr)
    {
        const int num = transport_.timeSigNumerator(), den = transport_.timeSigDenominator();
        BlockSegment clicks[maxSegments + 1];
        int count = 0;
        if (bs.countInSamples > 0) clicks[count++] = {0, bs.countInSamples, bs.countInStart, samplesPerBeat};
        if (transport_.metronomeOn()) for (int i = 0; i < numSegs; ++i) clicks[count++] = segs[i];
        metronome_.render(clicks, count, num * 4.0 / std::max(1, den), numSamples,
                          outputs[0], outputs[1], 4.0 / std::max(1, den));
    }

    renderPreview(outputs, numOutputs, numSamples);
    if (bs.playing)
        transport_.advanceTo(endPos);
}

// ============================================================================ offline render (Export)

bool AudioEngine::beginOffline()
{
    if (offline_ || latest_ == nullptr)
        return false;
    offline_ = true;
    offlineSaved_.deviceWasRunning = deviceManager_.getCurrentAudioDevice() != nullptr;
    if (offlineSaved_.deviceWasRunning)
        deviceManager_.removeAudioCallback(this); // waits for a running callback to finish
    if (auto* pending = pending_.exchange(nullptr))
    {
        delete current_;
        current_ = pending;
    }
    offlineNodes_.clear();
    if (current_)
        for (const auto& node : current_->nodes)
        {
            const auto& control = (*current_->controls)[(size_t) node.controlIndex];
            OfflineNodeState state;
            state.volume = control.volume.load(); state.pan = control.pan.load();
            state.active = control.active.load(); state.solo = control.solo.load();
            for (size_t send = 0; send < state.sends.size(); ++send) state.sends[send] = control.sends[send].load();
            for (const auto& device : node.devices)
            {
                std::vector<float> values;
                for (int i = 0; i < (int) device->parameters().size(); ++i) values.push_back(device->getParameter(i));
                state.parameters.push_back(std::move(values));
            }
            offlineNodes_.push_back(std::move(state));
        }
    offlineSaved_.position = transport_.positionBeats();
    offlineSaved_.tempo = transport_.tempo();
    offlineSaved_.loopOn = transport_.loopEnabled();
    offlineSaved_.loopStart = transport_.loopStart();
    offlineSaved_.loopLength = transport_.loopLength();
    offlineSaved_.metronome = transport_.metronomeOn();
    transport_.setLoopEnabled(false);
    transport_.setMetronome(false);
    // Consume a pending Stop / seek now, or the first rendered block would swallow the Play request.
    transport_.stop();
    std::array<float, 16> l{}, r{};
    float* out[2] = {l.data(), r.data()};
    renderBlock(out, 2, (int) l.size());
    return true;
}

void AudioEngine::endOffline()
{
    if (! offline_)
        return;
    offlineStemSlot_ = nullptr;
    offlineStemBuffers_.clear();
    transport_.stop();
    transport_.setPosition(offlineSaved_.position);
    transport_.setTempo(offlineSaved_.tempo);
    transport_.setLoop(offlineSaved_.loopOn, offlineSaved_.loopStart, offlineSaved_.loopLength);
    transport_.setMetronome(offlineSaved_.metronome);
    if (latest_ != nullptr)
        for (auto& node : latest_->nodes)
            node.audioState.reset();
    // A render evaluates automation; restore hand-set controls before the next model capture.
    if (current_)
        for (size_t index = 0; index < current_->nodes.size() && index < offlineNodes_.size(); ++index)
        {
            auto& node = current_->nodes[index]; auto& state = offlineNodes_[index];
            auto& control = (*current_->controls)[(size_t) node.controlIndex];
            control.volume.store(state.volume); control.pan.store(state.pan);
            control.active.store(state.active); control.solo.store(state.solo);
            for (size_t send = 0; send < state.sends.size(); ++send) control.sends[send].store(state.sends[send]);
            for (size_t device = 0; device < node.devices.size() && device < state.parameters.size(); ++device)
                for (size_t param = 0; param < state.parameters[device].size(); ++param)
                    node.devices[device]->setParameter((int) param, state.parameters[device][param]);
        }
    offlineNodes_.clear();
    offline_ = false;
    if (offlineSaved_.deviceWasRunning)
        deviceManager_.addAudioCallback(this);
    else
    {
        // headless (tools, tests): consume the stop/seek like the next callback would
        std::array<float, 64> l{}, r{};
        float* out[2] = {l.data(), r.data()};
        renderBlock(out, 2, 64);
    }
}

void AudioEngine::renderOffline(double startBeat, const std::vector<int>& stemNodes,
                                const std::function<int(double, double, int)>& nextBlockSize,
                                const std::function<void(const OfflineBlock&)>& onBlock, bool preMixer, int sessionScene)
{
    if (! offline_ || current_ == nullptr)
        return;
    const int maxBlock = std::min(blockSize_, current_->nodes.front().buffer.getNumSamples());

    offlinePreMixer_ = preMixer;
    offlineStemSlotStorage_.assign(current_->nodes.size(), -1);
    offlineStemBuffers_.clear();
    offlineStemBuffers_.reserve(stemNodes.size());
    std::vector<std::array<const float*, 2>> stemPointers;
    for (int node : stemNodes)
        if (node >= 0 && (size_t) node < current_->nodes.size() && offlineStemSlotStorage_[(size_t) node] < 0)
        {
            offlineStemSlotStorage_[(size_t) node] = (int) offlineStemBuffers_.size();
            offlineStemBuffers_.emplace_back(2, maxBlock);
            offlineStemBuffers_.back().clear();
        }
    stemPointers.resize(offlineStemBuffers_.size());
    for (size_t i = 0; i < offlineStemBuffers_.size(); ++i)
        stemPointers[i] = {offlineStemBuffers_[i].getReadPointer(0), offlineStemBuffers_[i].getReadPointer(1)};

    // Each render starts from a deterministic stopped graph, including Session passes.
    for (auto& node : current_->nodes)
    {
        node.audioState.reset(); node.outDelay.reset(); node.trackDelay.reset();
        for (auto& delay : node.sendDelays) delay.reset();
        for (auto& device : node.devices) device->reset();
        node.lastGainL = node.lastGainR = -1;
    }
    // Session clips must not leak into the arrangement render.
    backToArrangement(*current_);
    stopAllClips(*current_, transport_, 0.0);

    transport_.setPosition(startBeat);
    // Offline owns the clock. Publish the starting beat before the exporter's first size/warp
    // query, while retaining the pending seek so renderBlock still resets notes and automation.
    transport_.advanceTo(startBeat);
    if (sessionScene >= 0)
        for (int node : stemNodes)
        {
            launchClip(*current_, transport_, node, sessionScene, 0.0);
            if (current_->nodes[(size_t) node].kind == model::TrackKind::Group)
                for (int child : current_->trackOrder)
                {
                    int parent = current_->nodes[(size_t) child].outputNode;
                    for (size_t depth = 0; parent >= 0 && depth < current_->nodes.size(); ++depth)
                    {
                        if (parent == node) { launchClip(*current_, transport_, child, sessionScene, 0.0); break; }
                        parent = current_->nodes[(size_t) parent].outputNode;
                    }
                }
        }
    transport_.play();
    offlineStemSlot_ = offlineStemSlotStorage_.empty() ? nullptr : &offlineStemSlotStorage_;

    std::vector<float> left((size_t) maxBlock), right((size_t) maxBlock);
    float* out[2] = {left.data(), right.data()};
    for (;;)
    {
        const double tempo = transport_.tempo();
        const double samplesPerBeat = sampleRate_ * 60.0 / tempo;
        const int n = std::min(maxBlock, nextBlockSize(transport_.positionBeats(), samplesPerBeat, maxBlock));
        if (n <= 0)
            break;
        renderBlock(out, 2, n);
        OfflineBlock block;
        block.numFrames = n;
        block.mainL = left.data();
        block.mainR = right.data();
        block.stems = &stemPointers;
        onBlock(block);
    }
    offlineStemSlot_ = nullptr;
    offlinePreMixer_ = false;
}

} // namespace sonora::engine
