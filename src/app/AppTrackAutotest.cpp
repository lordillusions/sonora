// SONORA_TRACK_SWAP_AUTOTEST=<track name> (UI-111): the owner's steps that froze 0.1.48. 4 s after the set loads, selects
// the named track, duplicates it without automation, then deletes the original, logging how long each step took and the
// tracks left. Quits without saving.
#include "app/AppController.h"

#include "util/Log.h"

namespace sonora::app
{

void AppController::runTrackSwapAutotest()
{
    // <track name>[,play][,windows]: play keeps the transport running; windows opens every plug-in window of both tracks.
    const auto spec = juce::StringArray::fromTokens(juce::SystemStats::getEnvironmentVariable("SONORA_TRACK_SWAP_AUTOTEST", {}), ",", "");
    if (spec.isEmpty() || spec[0].isEmpty() || ! song_) return;
    const auto name = spec[0].toStdString();
    const bool play = spec.contains("play"), windows = spec.contains("windows");
    const std::weak_ptr<bool> alive = alive_;
    juce::Timer::callAfterDelay(4000, [this, alive, name, play, windows] {
        if (alive.expired() || ! song_) return;
        int index = -1;
        for (size_t i = 0; i < song_->tracks.size(); ++i)
            if (song_->tracks[i].name == name) { index = (int) i; break; }
        if (index < 0) { logf("track swap autotest: no track \"%s\"", name.c_str()); juce::JUCEApplicationBase::quit(); return; }
        const auto original = song_->tracks[(size_t) index].id;
        const auto select = [this](int i) {
            ui_.trackSelection = {i};
            ui_.selectedTrack = ui_.trackSelectionAnchor = i;
            ui_.sessionTrackSelection = true;
        };
        select(index);
        if (play)
        {
            if (auto* graph = engine_.latestGraph(); graph != nullptr && graph->mainNode >= 0) // silent: Main at 0
                if (auto controls = engine_.controls())
                    (*controls)[(size_t) graph->nodes[(size_t) graph->mainNode].controlIndex].volume.store(0.0f);
            engine_.transport().setPosition(64.0);
            engine_.transport().play();
        }
        logf("track swap autotest: duplicating \"%s\" (%zu tracks)", name.c_str(), song_->tracks.size());
        const auto start = juce::Time::getMillisecondCounterHiRes();
        duplicateSelectedTracks(false);
        logf("track swap autotest: duplicated in %.0f ms (%zu tracks)", juce::Time::getMillisecondCounterHiRes() - start, song_->tracks.size());
        juce::Timer::callAfterDelay(3000, [this, alive, original, select, windows] {
            if (alive.expired() || ! song_) return;
            int at = -1;
            for (size_t i = 0; i < song_->tracks.size(); ++i)
                if (song_->tracks[i].id == original) { at = (int) i; break; }
            if (at < 0) { logf("track swap autotest: the original is gone"); juce::JUCEApplicationBase::quit(); return; }
            if (auto* graph = engine_.latestGraph())
                for (int t : {at, at + 1})
                    if ((size_t) t < graph->nodes.size())
                        for (auto* proc : graph->nodes[(size_t) t].processorForModelDevice)
                            logf("track swap autotest: track %d: %s%s", t, proc ? proc->name().c_str() : "(none)",
                                 proc && proc->isHostedPlugin() ? " (hosted)" : "");
            if (windows)
                for (int t : {at, at + 1})
                    for (int d = 0; (size_t) t < song_->tracks.size() && d < (int) song_->tracks[(size_t) t].devices.size(); ++d)
                        openPluginEditor(t, d);
            logf("track swap autotest: %d plug-in windows open", pluginWindows_.size());
            select(at);
            logf("track swap autotest: deleting the original (index %d)", at);
            const auto began = juce::Time::getMillisecondCounterHiRes();
            if (ui_.deleteSelectedTracks) ui_.deleteSelectedTracks();
            logf("track swap autotest: deleted in %.0f ms (%zu tracks)", juce::Time::getMillisecondCounterHiRes() - began, song_->tracks.size());
            juce::Timer::callAfterDelay(3000, [this, alive] {
                if (alive.expired()) return;
                logf("track swap autotest: still responsive 3 s later, edited %s", ui_.documentDirty ? "yes" : "no");
                ui_.documentDirty = false;
                juce::JUCEApplicationBase::quit();
            });
        });
    });
}

} // namespace sonora::app
