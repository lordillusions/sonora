#include "engine/ClipLauncher.h"
#include <cmath>

namespace sonora::engine
{

double quantisationBeats(int q, int num, int den)
{
    const double bar = std::max(1, num) * 4.0 / std::max(1, den);
    switch (q)
    {
        case 0: return 0.0;
        case 1: return 8.0 * bar;
        case 2: return 4.0 * bar;
        case 3: return 2.0 * bar;
        case 4: return bar;
        case 5: return 2.0;
        case 6: return 4.0 / 3.0;
        case 7: return 1.0;
        case 8: return 2.0 / 3.0;
        case 9: return 0.5;
        case 10: return 1.0 / 3.0;
        case 11: return 0.25;
        case 12: return 1.0 / 6.0;
        case 13: return 0.125;
        default: return bar;
    }
}

double clipQuantisationBeats(const model::Clip& clip, int global, int num, int den)
{
    // Clip enum: 0 Global, then the set's list shifted by one (1 None, 2 8 Bars, ... 14 1/32).
    return quantisationBeats(clip.launchQuantisation <= 0 ? global : clip.launchQuantisation - 1, num, den);
}

namespace
{
TrackNode* sessionNode(EngineGraph& g, int track)
{
    if (track < 0 || (size_t) track >= g.nodes.size())
        return nullptr;
    auto& n = g.nodes[(size_t) track];
    return n.kind == model::TrackKind::Audio || n.kind == model::TrackKind::Midi ? &n : nullptr;
}

void queue(EngineGraph& g, int track, int slot, double quant, double repeatQuant = 0.0, float gain = 1.0f)
{
    auto& c = (*g.controls)[(size_t) track];
    c.queuedQuant.store(quant, std::memory_order_relaxed);
    c.queuedLaunchGain.store(gain, std::memory_order_relaxed);
    c.repeatQuant.store(repeatQuant, std::memory_order_relaxed);
    c.repeatSlot.store(repeatQuant > 0.0 && slot >= 0 ? slot : TrackControls::kNoSlot, std::memory_order_release);
    c.queuedSlot.store(slot, std::memory_order_release); // last: publishes the quantisation with it
}

// A launch while the transport is stopped starts it and takes effect right away.
double effectiveQuant(const Transport& t, double quant)
{
    return t.isPlaying() ? quant : 0.0;
}

void launch(EngineGraph& g, Transport& transport, int track, int scene, double quant, bool repeat, float gain = 1.0f)
{
    auto* node = sessionNode(g, track);
    if (node == nullptr)
        return;
    const bool has = scene >= 0 && (size_t) scene < node->sessionAvailability.size() && node->sessionAvailability[(size_t) scene] != 0;
    const double q = effectiveQuant(transport, quant);
    queue(g, track, has ? scene : TrackControls::kQueuedStop, q, repeat ? std::max(0.0, quant) : 0.0, gain);
    if (has && ! transport.isPlaying())
        transport.play();
}
} // namespace

void launchClip(EngineGraph& g, Transport& transport, int track, int scene, double quant)
{
    launch(g, transport, track, scene, quant, false);
}

float clipLaunchGain(const model::Clip& clip, float velocity)
{
    // Linear interpolation: MIDI velocity 1 is silent at 100%, 127 (mouse/keyboard) is unity.
    const float normalised = std::isfinite(velocity) ? std::clamp((velocity * 127.0f - 1.0f) / 126.0f, 0.0f, 1.0f) : 1.0f;
    const float amount = std::isfinite(clip.velocityAmount) ? (float) std::clamp(clip.velocityAmount, 0.0, 1.0) : 0.0f;
    return 1.0f - amount * (1.0f - normalised);
}

void launchClipWithMode(EngineGraph& g, Transport& transport, const model::Clip& clip, int track, int scene, double quant, float velocity)
{
    if (clip.launchMode == 2 && sessionNode(g, track) != nullptr)
    {
        const auto state = slotState(g, track, scene);
        if (state == SlotState::Playing || state == SlotState::Queued)
        {
            stopTrack(g, transport, track, quant);
            return;
        }
    }
    launch(g, transport, track, scene, quant, clip.launchMode == 3, clipLaunchGain(clip, velocity));
}

void releaseClip(EngineGraph& g, Transport& transport, const model::Clip& clip, int track, int scene, double quant)
{
    if (clip.launchMode == 3 && sessionNode(g, track) != nullptr)
    {
        auto& held = (*g.controls)[(size_t) track].repeatSlot;
        int expected = scene;
        held.compare_exchange_strong(expected, TrackControls::kNoSlot, std::memory_order_acq_rel);
    }
    if (clip.launchMode != 1)
        return;
    const auto state = slotState(g, track, scene);
    if (state == SlotState::Playing || state == SlotState::Queued)
        stopTrack(g, transport, track, quant);
}

void stopTrack(EngineGraph& g, Transport& transport, int track, double quant)
{
    if (sessionNode(g, track) != nullptr)
        queue(g, track, TrackControls::kQueuedStop, effectiveQuant(transport, quant));
}

void launchScene(EngineGraph& g, Transport& transport, const model::Song& song, int scene, double quant)
{
    bool anyClip = false;
    for (int t = 0; t < (int) g.nodes.size(); ++t)
    {
        auto* node = sessionNode(g, t);
        if (node == nullptr)
            continue;
        const bool has = scene >= 0 && (size_t) scene < node->sessionAvailability.size() && node->sessionAvailability[(size_t) scene] != 0;
        anyClip |= has;
        auto& c = (*g.controls)[(size_t) t];
        const bool playing = c.playingSlot.load() != TrackControls::kNoSlot || c.queuedSlot.load() >= 0;
        c.repeatSlot.store(TrackControls::kNoSlot, std::memory_order_release);
        if (has)
        {
            double clipQuant = quant;
            if ((size_t) t < song.tracks.size())
            {
                const auto& slots = song.tracks[(size_t) t].slots;
                if ((size_t) scene < slots.size() && slots[(size_t) scene].clip && slots[(size_t) scene].clip->launchQuantisation > 0)
                    clipQuant = clipQuantisationBeats(*slots[(size_t) scene].clip, song.globalQuantisation,
                                                      song.timeSigNumerator, song.timeSigDenominator);
            }
            queue(g, t, scene, effectiveQuant(transport, clipQuant));
        }
        else if (playing)
            queue(g, t, TrackControls::kQueuedStop, effectiveQuant(transport, quant)); // empty slot stops the track
    }
    if (scene >= 0 && (size_t) scene < song.scenes.size() && song.scenes[(size_t) scene].tempo)
        transport.setTempo(*song.scenes[(size_t) scene].tempo);
    if (anyClip && ! transport.isPlaying())
        transport.play();
}

void stopAllClips(EngineGraph& g, Transport& transport, double quant)
{
    for (int t = 0; t < (int) g.nodes.size(); ++t)
        stopTrack(g, transport, t, quant);
}

void backToArrangement(EngineGraph& g)
{
    for (int t = 0; t < (int) g.nodes.size(); ++t)
        if (sessionNode(g, t) != nullptr)
        {
            auto& c = (*g.controls)[(size_t) t];
            c.repeatSlot.store(TrackControls::kNoSlot, std::memory_order_release);
            c.queuedQuant.store(0.0, std::memory_order_relaxed);
            c.queuedSlot.store(TrackControls::kQueuedStop, std::memory_order_release);
            c.arrangementOn.store(true, std::memory_order_relaxed);
        }
}

bool arrangementOverridden(const EngineGraph& g)
{
    for (int t = 0; t < (int) g.nodes.size(); ++t)
        if (g.nodes[(size_t) t].kind != model::TrackKind::Group && ! (*g.controls)[(size_t) t].arrangementOn.load())
            return true;
    return false;
}

SlotState slotState(const EngineGraph& g, int track, int scene)
{
    if (track < 0 || (size_t) track >= g.nodes.size())
        return SlotState::Empty;
    const auto& node = g.nodes[(size_t) track];
    if (scene < 0 || (size_t) scene >= node.sessionAvailability.size() || node.sessionAvailability[(size_t) scene] == 0)
        return SlotState::Empty;
    const auto& c = (*g.controls)[(size_t) track];
    const int queued = c.queuedSlot.load(std::memory_order_acquire);
    if (queued == scene)
        return SlotState::Queued;
    if (c.playingSlot.load() == scene)
        return queued == TrackControls::kQueuedStop ? SlotState::QueuedStop : SlotState::Playing;
    return SlotState::Stopped;
}

} // namespace sonora::engine
