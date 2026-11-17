// UI-92b: controls moved by hand count as changes to the set. A fader, a pan or send knob, a native device's knob, a rack
// chain's mixer, a MIDI-mapped control or a plug-in knob changes the running engine without an edit command, and Save
// writes it (syncModelFromEngine), but the set used to stay "saved": no "*" in the title, no question on close or quit, no
// autosave. The app compares what a person can move by hand a few times a second and marks the set edited when it moved
// while nothing went through the history. engine::handFingerprint leaves out everything automation, clip envelopes or
// rack macros drive; plug-ins count their own window's edits only while that window is open, and Sonora's plug-in panel
// edits (PluginDevice::panelEdits) always.
#include "app/AppController.h"

#include "engine/EngineGraph.h"
#include "plugins/PluginDevice.h"
#include "util/Log.h"

namespace sonora::app
{

void AppController::watchHandEdits()
{
    auto* graph = engine_.latestGraph();
    if (! song_ || graph == nullptr || editingDevices_ || engine_.changingDevices() || syncingModel_ || exporting_ || installingCollabSong_
        || collabGuest()) return;
    const auto now = juce::Time::getMillisecondCounterHiRes();
    if (now - handCheckedAt_ < 250) return;
    handCheckedAt_ = now;

    // A rebuilt graph, another song or an edit through the history (whose values the engine just took) starts a new
    // baseline: only what moved between two looks at the same running set is a hand edit.
    const bool fresh = graph != handGraph_ || song_.get() != handSong_ || history_.version() != handVersion_;
    if (fresh) handPlan_ = engine::planHandEdits(*graph, *song_); // what to read: worked out again only when the set changed
    const auto fingerprint = engine::handFingerprint(*graph, handPlan_);
    bool moved = ! fresh && fingerprint != handFingerprint_;
    std::map<const plugins::PluginDevice*, HandPluginSeen> seen;
    forEachHostedPlugin([&](model::Device&, plugins::PluginDevice& plugin) {
        // stateSerial: parameter edits and the plug-in's other state changes (a wavetable, a preset, routing), which it
        // reports as "state changed" instead of as a parameter.
        const HandPluginSeen counts{plugin.stateSerial(), plugin.panelEdits()};
        // Compared even after a graph rebuild (an edit that changes the plug-in's latency rebuilds the graph): the same
        // plug-in instance keeps its counters; a new instance is a new key.
        if (const auto before = handPlugins_.find(&plugin); before != handPlugins_.end())
        {
            if (counts.panel != before->second.panel) moved = true;
            // Some plug-ins report parameters on their own (programs, internal modulation): their window being open is
            // what tells a person's edit apart.
            if (counts.window != before->second.window && pluginEditorOpen(&plugin.instance())) moved = true;
        }
        seen[&plugin] = counts;
        return true;
    });
    handGraph_ = graph;
    handSong_ = song_.get();
    handFingerprint_ = fingerprint;
    handPlugins_ = std::move(seen);
    if (moved) markHandEdited();
    handVersion_ = history_.version();
}

void AppController::markHandEdited()
{
    handEdited_ = true;
    history_.markExternalChange(); // a new version: the autosave sees a change to copy
    if (! ui_.documentDirty)
    {
        ui_.documentDirty = true;
        updateWindowTitle();
    }
}

void AppController::resetHandEdits()
{
    handEdited_ = false;
    handGraph_ = nullptr; // the next look starts a new baseline
    handPlan_ = {};
    handPlugins_.clear();
}

} // namespace sonora::app

namespace sonora::app
{

// SONORA_HAND_EDIT_AUTOTEST=<seconds>[,beat] (UI-92b): 4 s after the set loads, plays it for <seconds> touching nothing
// (automation and clip envelopes play) and logs whether the set counts as edited (it must not), stops, then moves a
// fader and a native device knob the way the UI does (straight into the engine) and logs the edited state after each.
// Quits without saving.
void AppController::runHandEditAutotest()
{
    const auto spec = juce::SystemStats::getEnvironmentVariable("SONORA_HAND_EDIT_AUTOTEST", {});
    if (spec.isEmpty() || ! song_) return;
    const int seconds = std::clamp(spec.upToFirstOccurrenceOf(",", false, false).getIntValue(), 1, 600);
    const double from = spec.contains(",") ? spec.fromFirstOccurrenceOf(",", false, false).getDoubleValue() : 0.0;
    const std::weak_ptr<bool> alive = alive_;
    auto state = [this](const char* when) { logf("hand edit autotest: %s: edited %s", when, ui_.documentDirty ? "yes" : "no"); };
    juce::Timer::callAfterDelay(4000, [this, alive, seconds, from, state] {
        if (alive.expired() || ! song_) return;
        state("loaded");
        if (const auto* graph = engine_.latestGraph())
        {
            const auto start = juce::Time::getMillisecondCounterHiRes();
            const auto plan = engine::planHandEdits(*graph, *song_);
            const auto planned = juce::Time::getMillisecondCounterHiRes();
            for (int i = 0; i < 10; ++i) (void) engine::handFingerprint(*graph, plan);
            logf("hand edit autotest: plan %.3f ms, fingerprint %.3f ms", planned - start,
                 (juce::Time::getMillisecondCounterHiRes() - planned) / 10.0);
        }
        // Silent run: Main down to 0 first (itself a hand edit, so it is then taken as saved before playing).
        if (auto* graph = engine_.latestGraph(); graph != nullptr && graph->mainNode >= 0)
            if (auto controls = engine_.controls())
                (*controls)[(size_t) graph->nodes[(size_t) graph->mainNode].controlIndex].volume.store(0.0f);
        juce::Timer::callAfterDelay(800, [this, alive, seconds, from, state] {
            if (alive.expired() || ! song_) return;
            state("Main volume at 0");
            resetHandEdits();
            history_.markSaved();
            ui_.documentDirty = false;
            engine_.transport().setPosition(from);
            engine_.transport().play();
            juce::Timer::callAfterDelay(seconds * 1000, [this, alive, seconds, state] {
                if (alive.expired() || ! song_) return;
                logf("hand edit autotest: played %d s", seconds);
                state("after playing, nothing touched");
                engine_.transport().stop();
                juce::Timer::callAfterDelay(1000, [this, alive, state] {
                    if (alive.expired() || ! song_) return;
                    state("stopped");
                    auto* graph = engine_.latestGraph();
                    auto controls = engine_.controls();
                    if (graph == nullptr || ! controls) return;
                    // A fader nobody automates: the first track whose volume has no envelope.
                    for (size_t n = 0; n < song_->tracks.size() && n < graph->nodes.size(); ++n)
                    {
                        const auto& track = song_->tracks[n];
                        const int id = track.mixer.volumeTargetId;
                        if (std::any_of(track.automation.begin(), track.automation.end(), [id](const auto& e) { return e.targetId == id; })) continue;
                        auto& volume = (*controls)[(size_t) graph->nodes[n].controlIndex].volume;
                        volume.store(volume.load() * 0.5f);
                        logf("hand edit autotest: moved the fader of \"%s\"", track.name.c_str());
                        break;
                    }
                    juce::Timer::callAfterDelay(800, [this, alive, state] {
                        if (alive.expired() || ! song_) return;
                        state("after the fader");
                        // Saved: no longer edited. Then a native device knob.
                        resetHandEdits();
                        history_.markSaved();
                        ui_.documentDirty = false;
                        juce::Timer::callAfterDelay(800, [this, alive, state] {
                            if (alive.expired() || ! song_) return;
                            state("marked saved");
                            bool moved = false;
                            if (auto* running = engine_.latestGraph())
                                for (auto& node : running->nodes)
                                {
                                    for (auto* proc : node.processorForModelDevice)
                                        if (proc != nullptr && ! proc->isHostedPlugin() && ! proc->parameters().empty())
                                        {
                                            const auto& info = proc->parameters().front();
                                            const float v = proc->getParameter(0);
                                            proc->setParameter(0, v > info.minValue ? info.minValue : info.maxValue);
                                            logf("hand edit autotest: moved \"%s\" of %s", info.name.c_str(), proc->name().c_str());
                                            moved = true;
                                            break;
                                        }
                                    if (moved) break;
                                }
                            juce::Timer::callAfterDelay(800, [this, alive, state] {
                                if (alive.expired() || ! song_) return;
                                state("after the knob");
                                // A hosted plug-in's own window: open it, take the set as saved, then let the plug-in
                                // report a knob move itself (gesture begin, value, gesture end), as its UI does.
                                int track = -1, device = -1;
                                plugins::PluginDevice* hosted = nullptr;
                                const auto wanted = juce::SystemStats::getEnvironmentVariable("SONORA_HAND_EDIT_PLUGIN", {}).toStdString();
                                if (auto* live = engine_.latestGraph())
                                    for (size_t t = 0; t < live->nodes.size() && hosted == nullptr; ++t)
                                        for (size_t d = 0; d < live->nodes[t].processorForModelDevice.size(); ++d)
                                            if (auto* candidate = dynamic_cast<plugins::PluginDevice*>(live->nodes[t].processorForModelDevice[d]);
                                                candidate != nullptr && ! candidate->instance().getParameters().isEmpty()
                                                && (wanted.empty() || candidate->name() == wanted))
                                            {
                                                hosted = candidate; track = (int) t; device = (int) d;
                                                break;
                                            }
                                if (hosted == nullptr)
                                {
                                    logf("hand edit autotest: no hosted plug-in with parameters");
                                    ui_.documentDirty = false;
                                    juce::JUCEApplicationBase::quit();
                                    return;
                                }
                                openPluginEditor(track, device);
                                logf("hand edit autotest: opened %s (window %s)", hosted->name().c_str(),
                                     pluginEditorOpen(&hosted->instance()) ? "open" : "missing");
                                juce::Timer::callAfterDelay(1500, [this, alive, state, hosted] {
                                    if (alive.expired() || ! song_) return;
                                    resetHandEdits();
                                    history_.markSaved();
                                    ui_.documentDirty = false;
                                    // Playing with the window open and nothing touched must not count either.
                                    engine_.transport().setPosition(200.0);
                                    engine_.transport().play();
                                    juce::Timer::callAfterDelay(6000, [this, alive, state, hosted] {
                                        if (alive.expired() || ! song_) return;
                                        state("plug-in window open, 6 s of playback, nothing touched");
                                        engine_.transport().stop();
                                        auto* param = hosted->instance().getParameters()[0];
                                        const float v = param->getValue();
                                        param->beginChangeGesture();
                                        param->setValueNotifyingHost(v > 0.5f ? v - 0.25f : v + 0.25f);
                                        param->endChangeGesture();
                                        logf("hand edit autotest: the plug-in moved \"%s\"", param->getName(64).toRawUTF8());
                                        juce::Timer::callAfterDelay(800, [this, alive, state, hosted] {
                                            if (alive.expired() || ! song_) return;
                                            state("after the plug-in knob");
                                            resetHandEdits();
                                            history_.markSaved();
                                            ui_.documentDirty = false;
                                            juce::Timer::callAfterDelay(800, [this, alive, state, hosted] {
                                                if (alive.expired() || ! song_) return;
                                                state("marked saved again");
                                                // A change that is not a parameter (a wavetable, a preset): the plug-in
                                                // reports its state as changed.
                                                hosted->instance().updateHostDisplay(
                                                    juce::AudioProcessorListener::ChangeDetails().withNonParameterStateChanged(true));
                                                juce::Timer::callAfterDelay(800, [this, alive, state] {
                                                    if (alive.expired() || ! song_) return;
                                                    state("after the plug-in's own state change");
                                                    ui_.documentDirty = false; // a verification run never asks to save
                                                    juce::JUCEApplicationBase::quit();
                                                });
                                            });
                                        });
                                    });
                                });
                            });
                        });
                    });
                });
            });
        });
    });
}

} // namespace sonora::app
