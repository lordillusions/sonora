#include "app/AppController.h"
#include "engine/TempoAnalysis.h"
#include "io/als/AlsReader.h"
#include "ui/AudioClipEdits.h"

namespace sonora::app
{
void AppController::toggleClipWarp(const model::ClipLocation& location)
{
    if (! song_ || exporting_ || stemsRunning_ || editingDevices_ || syncingModel_ || ui_.recording || ui_.recordingRequested) return;
    const auto* clip = model::findClip(*song_, location);
    const auto* track = model::trackAt(*song_, location.track);
    if (! clip || ! track || track->frozen || clip->kind != model::Clip::Kind::Audio) return;
    if (clip->warped || clip->tempoAnalyzed || clip->warpMarkers.size() > 2)
    {
        applyEdit(model::modifyClip(*song_, location, ui::audioedit::toggleWarp(*clip, song_->tempo), clip->warped ? "Warp Off" : "Warp On"));
        return;
    }
    if (warpAnalysisPending_.size() >= 8 || ! warpAnalysisPending_.insert(clip->uid).second) return;
    const auto original = *clip;
    const auto path = io::resolveSamplePath(*song_, clip->sample);
    const auto generation = documentGeneration_;
    const int version = history_.version();
    const double tempo = song_->tempo;
    const std::weak_ptr<bool> alive = alive_;
    ui_.statusText = "Detecting sample tempo...";
    const int busy = ui_.beginBusy(ui_.statusText, true);
    if (ui_.requestRepaint) ui_.requestRepaint();
    pool_.addJob([this, alive, original, path, generation, version, tempo, location, busy] {
        engine::TempoEstimate estimate;
        if (! path.empty())
            if (const auto sample = engine_.sampleCache().load(path)) estimate = engine::estimateSampleTempo(sample->audio, sample->sampleRate, tempo);
        juce::MessageManager::callAsync([this, alive, original, generation, version, tempo, location, estimate, busy] {
            if (alive.expired()) return;
            warpAnalysisPending_.erase(original.uid);
            ui_.endBusy(busy);
            if (generation != documentGeneration_ || ! song_) return;
            if (history_.version() != version || ! model::findClip(*song_, location) || ui_.recording || ui_.recordingRequested || exporting_)
            { ui_.statusText = "The clip changed during tempo analysis; enable Warp again"; return; }
            auto changed = ui::audioedit::enableWarpWithTempo(original, tempo, estimate.bpm);
            const auto applied = applyEdit(model::modifyClip(*song_, location, std::move(changed), "Warp On"));
            if (applied.ok)
                ui_.statusText = estimate.bpm > 0 ? "Detected sample tempo: " + std::to_string(estimate.bpm).substr(0, std::to_string(estimate.bpm).find('.') + 2) + " BPM"
                                                : "No steady tempo detected; using set tempo. The clip BPM remains editable.";
            if (ui_.requestRepaint) ui_.requestRepaint();
        });
    });
}
}
