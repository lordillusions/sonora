#include "engine/RecordingManager.h"

#include <algorithm>

namespace sonora::engine
{

RecordingManager::~RecordingManager()
{
    deviceStopped();
    delete pending_.exchange(nullptr); delete retired_.exchange(nullptr); delete audio_;
}

void RecordingManager::publish()
{
    auto bank = std::make_unique<Bank>(); bank->takes = sessions_;
    delete pending_.exchange(bank.release(), std::memory_order_acq_rel);
}

void RecordingManager::collectRetiredPreviews()
{
    const juce::ScopedLock lock(lifecycle_);
    delete retired_.exchange(nullptr, std::memory_order_acq_rel);
    arrangement_.collectRetiredPreviews();
    for (const auto& take : sessions_) take->recorder.collectRetiredPreviews();
    const auto removed = std::erase_if(sessions_, [](const auto& take) {
        return take->collected && take->recorder.previewSettled();
    });
    if (removed != 0) publish();
}

bool RecordingManager::start(const model::Song& song, const ControlBank& controls, const juce::File& directory,
                             double sampleRate, std::string& error, bool arrangement, const SessionPlan* plan)
{
    const juce::ScopedLock lock(lifecycle_);
    collectRetiredPreviews();
    if (! busy()) stopAllRequested_.store(false, std::memory_order_release);
    if (! plan)
    {
        if (busy()) { error = "Finish Session recording before recording the Arrangement"; return false; }
        return arrangement_.start(song, controls, directory, sampleRate, error, arrangement);
    }
    if (arrangement_.busy()) { error = "Finish Arrangement recording before recording Session clips"; return false; }
    std::vector<std::shared_ptr<SessionTake>> prepared;
    std::vector<int> tracks;
    for (const auto& target : plan->targets)
    {
        if (target.track < 0 || (size_t) target.track >= song.tracks.size() || (size_t) target.track >= controls.size()
            || ! controls[(size_t) target.track].armed.load())
        { error = "Arm a valid track before recording its Session slot"; return false; }
        if (capturesTrack(target.track) || std::find(tracks.begin(), tracks.end(), target.track) != tracks.end())
        { error = "This track already has a recording take in progress"; return false; }
        tracks.push_back(target.track);
    }
    for (const auto& target : plan->targets)
    {
        auto take = std::make_shared<SessionTake>(); take->track = target.track; take->scene = target.scene;
        auto single = *plan; single.targets = {target};
        if (! take->recorder.start(song, controls, directory, sampleRate, error, false, &single))
        {
            for (const auto& previous : prepared) previous->recorder.discardUnpublished();
            return false;
        }
        prepared.push_back(std::move(take));
    }
    if (prepared.empty()) { error = "Arm an audio or MIDI track before recording"; return false; }
    sessions_.insert(sessions_.end(), prepared.begin(), prepared.end()); publish();
    return true;
}

bool RecordingManager::active() const
{
    return arrangement_.active() || std::any_of(sessions_.begin(), sessions_.end(), [](const auto& p) { return ! p->collected && p->recorder.active(); });
}

bool RecordingManager::busy() const
{
    return arrangement_.busy() || std::any_of(sessions_.begin(), sessions_.end(), [](const auto& p) { return ! p->collected; });
}

bool RecordingManager::finished() const
{
    return arrangement_.finished() || std::any_of(sessions_.begin(), sessions_.end(), [](const auto& p) { return ! p->collected && p->recorder.finished(); });
}

bool RecordingManager::sessionRecording() const
{
    return std::any_of(sessions_.begin(), sessions_.end(), [](const auto& p) { return ! p->collected; });
}

int RecordingManager::sessionScene(int track) const
{
    for (const auto& p : sessions_) if (p->track == track && ! p->collected) return p->scene;
    return -1;
}

int RecordingManager::sessionPhase(int track) const
{
    int phase = 0;
    for (const auto& p : sessions_) if (! p->collected && (track < 0 || p->track == track))
    {
        const int value = p->recorder.sessionPhase();
        phase = std::max(phase, value == 0 && p->recorder.busy() ? 4 : value);
    }
    return phase;
}

bool RecordingManager::capturesTrack(int track) const
{
    return arrangement_.capturesTrack(track) || sessionScene(track) >= 0;
}

void RecordingManager::requestSessionStop(int track, bool launchAfterRecording)
{
    for (const auto& p : sessions_) if (! p->collected && (track < 0 || p->track == track))
    { p->launchAfterRecording = launchAfterRecording; p->recorder.requestSessionStop(); }
}

void RecordingManager::requestStop()
{
    arrangement_.requestStop();
    stopAllRequested_.store(true, std::memory_order_release);
}

RecordingManager::Result RecordingManager::takeResult()
{
    Result result = arrangement_.finished() ? arrangement_.takeResult() : Result{};
    for (const auto& p : sessions_) if (! p->collected && p->recorder.finished())
    {
        auto take = p->recorder.takeResult(); p->collected = true;
        for (auto& clip : take.session) { clip.launchAfterRecording = p->launchAfterRecording; result.session.push_back(std::move(clip)); }
        if (! take.warning.empty()) result.warning += (result.warning.empty() ? "" : "; ") + take.warning;
    }
    return result;
}

void RecordingManager::deviceStopped()
{
    const juce::ScopedLock lock(lifecycle_);
    arrangement_.deviceStopped();
    for (const auto& p : sessions_) p->recorder.deviceStopped();
}

void RecordingManager::beginBlock()
{
    if (retired_.load(std::memory_order_acquire)) return;
    if (auto* bank = pending_.exchange(nullptr, std::memory_order_acq_rel))
    { auto* old = audio_; audio_ = bank; if (old) retired_.store(old, std::memory_order_release); }
}

void RecordingManager::prepareBlock(const BlockSegment* segments, int count, bool playing, bool jumped)
{
    if (stopAllRequested_.exchange(false, std::memory_order_acq_rel))
        if (audio_) for (const auto& p : audio_->takes) p->recorder.requestStop();
    numRanges_ = std::min(count, maxSegments);
    for (int i = 0; i < numRanges_; ++i)
    {
        segmentOffsets_[(size_t) i] = segments[i].sampleOffset;
        arrangementRanges_[(size_t) i] = arrangement_.prepareCaptureRange(segments[i], playing, i == 0 && jumped);
        if (audio_) for (const auto& p : audio_->takes)
            p->ranges[(size_t) i] = p->recorder.prepareCaptureRange(segments[i], playing, i == 0 && jumped);
    }
}

RecordingManager::SessionTake* RecordingManager::audioTake(int track) const
{
    if (audio_) for (const auto& p : audio_->takes) if (p->track == track && p->recorder.active()) return p.get();
    return nullptr;
}

const RecordingManager::CaptureRange* RecordingManager::rangesForTrack(int track) const
{
    if (auto* p = audioTake(track)) return p->ranges.data();
    return arrangement_.capturesTrack(track) ? arrangementRanges_.data() : nullptr;
}

bool RecordingManager::audioCapturesTrack(int track) const { return arrangement_.capturesTrack(track) || audioTake(track); }
bool RecordingManager::audioSessionRecording(int track) const { return audioTake(track) != nullptr; }
bool RecordingManager::overdubsTrack(int track) const { auto* p = audioTake(track); return p && p->recorder.overdubsTrack(track); }
void RecordingManager::beginSessionContent(int track, double elapsed) { if (auto* p = audioTake(track)) p->recorder.beginSessionContent(track, elapsed); }

void RecordingManager::beginPreviewBlock(const EngineGraph& graph)
{
    arrangement_.beginPreviewBlock(graph);
    if (audio_) for (const auto& p : audio_->takes) p->recorder.beginPreviewBlock(graph);
}

bool RecordingManager::hasOverdubPreview(int track, int scene, uint32_t uid)
{
    if (audio_) for (const auto& p : audio_->takes) if (p->recorder.hasOverdubPreview(track, scene, uid)) return true;
    return false;
}

bool RecordingManager::renderOverdub(int track, int scene, const PreparedClip& clip, const BlockSegment& segment,
                                    const juce::MidiBuffer& input, const CaptureRange* range, juce::MidiBuffer& out, bool refresh, bool* overflow, float velocityScale)
{
    if (audio_) for (auto i = audio_->takes.rbegin(); i != audio_->takes.rend(); ++i)
        if ((*i)->recorder.renderOverdub(track, scene, clip, segment, input, range, out, refresh, overflow, velocityScale)) return true;
    return false;
}

bool RecordingManager::commitOverdub(int track, int scene, const PreparedClip* clip, double elapsed, juce::MidiBuffer& out)
{
    bool preserved = false;
    if (audio_) for (const auto& p : audio_->takes) preserved |= p->recorder.commitOverdub(track, scene, clip, elapsed, out);
    return preserved;
}

void RecordingManager::capture(const float* const* inputs, int numInputs, const juce::MidiBuffer& midi, double beat,
                              double samplesPerBeat, int numSamples, bool playing, bool jumped, bool loopEnd,
                              const EngineGraph* graph, int sampleOffset, const CaptureRange*)
{
    int segment = -1;
    for (int i = 0; i < numRanges_; ++i)
        if (segmentOffsets_[(size_t) i] == sampleOffset)
        { segment = i; break; }
    arrangement_.capture(inputs, numInputs, midi, beat, samplesPerBeat, numSamples, playing, jumped, loopEnd, graph, sampleOffset,
                         segment >= 0 ? &arrangementRanges_[(size_t) segment] : nullptr);
    if (audio_) for (const auto& p : audio_->takes)
        p->recorder.capture(inputs, numInputs, midi, beat, samplesPerBeat, numSamples, playing, jumped, loopEnd, graph, sampleOffset,
                            segment >= 0 ? &p->ranges[(size_t) segment] : nullptr);
}

} // namespace sonora::engine
