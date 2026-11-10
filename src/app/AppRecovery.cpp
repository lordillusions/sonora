// PRJ-4 crash recovery: autosave of the open set into <data folder>/Recovery/<this run>/ and, on the next launch after a
// run that did not quit cleanly, "Sonora quit unexpectedly: recover <set>?".
//
// Cost on the message thread is one model copy plus reading the mixer/native values (atomics) and the state of the hosted
// plug-ins that reported a change; the audio callback is never locked. The file is written on autosavePool_ (background
// priority). Never while recording or while an offline render runs; never into the user's project or set file.
// See io/recovery/RecoveryStore.h for the files.

#include "app/AppController.h"
#include "app/PromptWindow.h"
#include "core/AppDataFolder.h"
#include "devices/RackProcessor.h"
#include "plugins/PluginDevice.h"
#include "ui/SetView.h"
#include "util/Log.h"

namespace sonora::app
{

namespace
{
double nowMs() { return juce::Time::getMillisecondCounterHiRes(); }

std::string appVersion()
{
    auto* app = juce::JUCEApplicationBase::getInstance();
    return app != nullptr ? app->getApplicationVersion().toStdString() : std::string();
}

juce::File fileOf(const std::string& path) { return juce::File(juce::String::fromUTF8(path.c_str())); }

juce::int64 modifiedMs(const std::string& path)
{
    if (path.empty() || ! juce::File::isAbsolutePath(juce::String::fromUTF8(path.c_str()))) return 0;
    const auto file = fileOf(path);
    return file.existsAsFile() ? file.getLastModificationTime().toMilliseconds() : 0;
}
} // namespace

void AppController::startRecovery()
{
    recovery_ = io::recovery::Store(core::appDataDirectory().getChildFile("Recovery"));
    if (! recovery_.start(appVersion()))
    {
        logf("recovery: cannot write %s; autosave is off", recovery_.root().getFullPathName().toRawUTF8());
        return;
    }
    const int removed = recovery_.prune();
    recoveryOffers_ = recovery_.recoverable();
    recoveryHeartbeatAt_ = nowMs();
    logf("recovery: %s (%zu %s from runs that did not quit cleanly, %d old %s removed)",
         recovery_.folder().getFullPathName().toRawUTF8(), recoveryOffers_.size(), recoveryOffers_.size() == 1 ? "copy" : "copies",
         removed, removed == 1 ? "folder" : "folders");
}

void AppController::autosaveTick()
{
    if (! recovery_.started()) return;
    const auto now = nowMs();
    if (now - recoveryHeartbeatAt_ >= 60000)
    {
        recovery_.heartbeat(); // tells another launch this run is alive
        recoveryHeartbeatAt_ = now;
    }
    if (! song_) return;
    const int version = history_.version();
    if (version != autosaveSeenVersion_)
    {
        autosaveSeenVersion_ = version;
        autosaveLastChangeAt_ = now;
        if (autosavePendingSince_ <= 0) autosavePendingSince_ = now;
    }
    if (! ui_.documentDirty)
    {
        // Saved, or undone back to the saved state: a copy of an older state would only confuse the next launch.
        if (autosaveHasCopy_ && ! autosaveInFlight_)
        {
            ++autosaveEpoch_;
            recovery_.clearDocument(fileOf(song_->patchSourcePath));
            autosaveHasCopy_ = false;
        }
        autosavedVersion_ = version;
        autosavePendingSince_ = 0;
        return;
    }
    io::recovery::AutosaveInputs in;
    in.documentOpen = ! collabGuest() && ! installingCollabSong_; // a session's set of record lives on the host
    in.dirty = true;
    in.changedSinceAutosave = version != autosavedVersion_;
    // TODO(PRJ-4b): a take being recorded when Sonora crashes is lost (its file is on disk but nothing references it).
    in.recording = ui_.recording || ui_.recordingRequested || automationRecording_;
    in.rendering = exporting_ || stemsRunning_ || engine_.offlineActive();
    // Not in the middle of a drag: the copy would land in the gesture (and the gesture's edit comes right after).
    in.busy = autosaveInFlight_ || editingDevices_ || syncingModel_ || juce::ModifierKeys::currentModifiers.isAnyMouseButtonDown();
    in.sinceLastChangeMs = now - autosaveLastChangeAt_;
    in.sincePendingMs = autosavePendingSince_ > 0 ? now - autosavePendingSince_ : 0;
    in.sinceLastAutosaveMs = now - autosaveLastAt_;
    in.lastWriteMs = autosaveLastWriteMs_;
    if (io::recovery::autosaveDue(in))
        (void) autosaveNow(in.sinceLastChangeMs >= io::recovery::AutosavePolicy{}.idleMs ? "idle" : "interval");
    else if (in.documentOpen && in.changedSinceAutosave && ! in.recording && ! in.rendering && ! in.busy && now - pluginRefreshAt_ >= 250)
    {
        pluginRefreshAt_ = now;
        refreshOnePluginState();
    }
}

bool AppController::pluginNeedsCapture(plugins::PluginDevice& plugin, double now)
{
    const auto found = autosavePlugins_.find(&plugin);
    if (found == autosavePlugins_.end()) return plugin.stateSerial() != 0; // created from the model's state: 0 = untouched
    if (found->second.serial != plugin.stateSerial()) return true;
    // Some plug-ins change state from their window without telling the host.
    return now - found->second.at >= 10000 && pluginEditorOpen(&plugin.instance());
}

void AppController::forEachHostedPlugin(const std::function<bool(model::Device&, plugins::PluginDevice&)>& fn)
{
    auto* graph = engine_.latestGraph();
    if (! song_ || graph == nullptr) return;
    std::function<bool(model::Device&, devices::DeviceProcessor*)> visit = [&](model::Device& device, devices::DeviceProcessor* processor) {
        if (processor == nullptr) return true;
        if (auto* plugin = dynamic_cast<plugins::PluginDevice*>(processor)) return ! device.plugin || fn(device, *plugin);
        if (const auto* rack = dynamic_cast<devices::RackProcessor*>(processor))
            for (size_t ci = 0; ci < device.chains.size() && ci < rack->chains().size(); ++ci)
            {
                auto& children = device.chains[ci].devices;
                const auto& processors = rack->chains()[ci].processorForModelDevice;
                for (size_t pi = 0; pi < children.size() && pi < processors.size(); ++pi)
                    if (! visit(children[pi], processors[pi])) return false;
            }
        return true;
    };
    size_t index = 0;
    auto track = [&](model::Track& t) {
        if (index >= graph->nodes.size()) return false;
        const auto& node = graph->nodes[index++];
        for (size_t di = 0; di < t.devices.size() && di < node.processorForModelDevice.size(); ++di)
            if (! visit(t.devices[di], node.processorForModelDevice[di])) return false;
        return true;
    };
    for (auto& t : song_->tracks) if (! track(t)) return;
    for (auto& t : song_->returns) if (! track(t)) return;
    (void) track(song_->main);
}

void AppController::refreshOnePluginState()
{
    if (editingDevices_ || syncingModel_) return;
    const auto now = nowMs();
    const juce::ScopedValueSetter<bool> syncing(syncingModel_, true);
    forEachHostedPlugin([&](model::Device& device, plugins::PluginDevice& plugin) {
        if (! pluginNeedsCapture(plugin, now)) return true;
        autosavePlugins_[&plugin] = {plugin.stateSerial(), now};
        (void) plugin.captureState(device); // into the model, as Save does (the audio keeps running)
        ++pluginsRefreshed_;
        pluginRefreshMaxMs_ = std::max(pluginRefreshMaxMs_, nowMs() - now);
        return false; // one per turn
    });
}

bool AppController::autosaveNow(const char* reason, std::shared_ptr<const model::Song> snapshot)
{
    if (! recovery_.started() || ! song_ || autosaveInFlight_ || collabGuest()) return false;
    const auto start = nowMs();
    double stateMs = 0;
    autosavePluginsSeen_ = autosavePluginsRead_ = 0;
    std::shared_ptr<const model::Song> copy = std::move(snapshot);
    if (! copy)
    {
        if (editingDevices_ || syncingModel_) return false;
        syncModelFromEngine(true);
        stateMs = nowMs() - start;
        auto fresh = std::make_shared<model::Song>(*song_);
        fresh->inKey = ui_.scaleHighlight; // as writeSet does
        ui::captureSetView(ui_, *fresh);
        copy = std::move(fresh);
    }
    const double copyMs = nowMs() - start - stateMs;
    const auto plugins = std::to_string(autosavePluginsRead_) + " of " + std::to_string(autosavePluginsSeen_) + ", "
                         + std::to_string(pluginsRefreshed_) + " read earlier one per turn, slowest "
                         + juce::String(pluginRefreshMaxMs_, 1).toStdString() + " ms";
    pluginsRefreshed_ = 0;
    pluginRefreshMaxMs_ = 0;
    const auto epoch = ++autosaveEpoch_;
    autosaveInFlight_ = true;
    autosavedVersion_ = history_.version();
    autosavePendingSince_ = 0;
    autosaveLastAt_ = start;

    auto info = std::make_shared<io::recovery::Entry>();
    info->documentPath = song_->sourcePath;
    info->untitled = isUntitled();
    info->setName = info->untitled ? "Untitled" : fileOf(song_->sourcePath).getFileNameWithoutExtension().toStdString();
    info->documentModifiedMs = modifiedMs(song_->sourcePath);
    info->appVersion = appVersion();
    info->setFile = recovery_.autosaveFile();
    const auto file = info->setFile;
    std::weak_ptr<bool> alive = alive_;
    autosavePool_.addJob([this, alive, job = std::move(copy), file, epoch, info, stateMs, copyMs, plugins, reason]() mutable {
        const auto begin = nowMs();
        // References stay as the set has them (no relocation): the copy is read back as the set at its own path.
        const auto result = std::make_shared<io::AlsSaveResult>(io::saveAls(*job, file, io::AlsSaveOptions{}));
        job.reset(); // the model copy is freed here, not on the message thread
        const auto writeMs = nowMs() - begin;
        juce::MessageManager::callAsync([this, alive, result, file, epoch, info, stateMs, copyMs, plugins, writeMs, reason] {
            if (alive.expired()) return;
            autosaveInFlight_ = false;
            autosaveLastWriteMs_ = writeMs;
            if (epoch != autosaveEpoch_)
            {
                (void) file.deleteFile(); // the document was saved or replaced meanwhile
                return;
            }
            if (! result->ok)
            {
                logf("autosave failed: %s", result->error.c_str()); // tried again after the next change
                return;
            }
            info->savedAtMs = juce::Time::currentTimeMillis();
            autosaveHasCopy_ = recovery_.commit(*info);
            logf("autosave (%s): \"%s\"; message thread: state %.1f ms (plug-ins read: %s), model copy %.1f ms; written in %.0f ms (%s)",
                 reason, info->setName.c_str(), stateMs, plugins.c_str(), copyMs, writeMs,
                 juce::File::descriptionOfSizeInBytes(file.getSize()).toRawUTF8());
        });
    });
    return true;
}

void AppController::recoveryDocumentInstalled()
{
    ++autosaveEpoch_;
    // The new graph's plug-ins were just created from the model's state: whatever they reported while loading is not a change.
    autosavePlugins_.clear();
    pluginsRefreshed_ = 0;
    pluginRefreshMaxMs_ = 0;
    const auto now = nowMs();
    forEachHostedPlugin([&](model::Device&, plugins::PluginDevice& plugin) {
        autosavePlugins_[&plugin] = {plugin.stateSerial(), now};
        return true;
    });
    autosavedVersion_ = autosaveSeenVersion_ = history_.version();
    autosavePendingSince_ = 0;
    autosaveHasCopy_ = false;
    auto recovered = std::move(pendingRecovery_);
    if (! recovery_.started() || ! song_) return;
    const auto base = fileOf(song_->patchSourcePath);
    recovery_.clearDocument(base); // the previous document's copies; a recovered set's base stays until it is saved
    if (recovered == nullptr || base != recovery_.recoveredFile()) return;
    // The recovered state is this run's copy now, so another crash before the next autosave still offers it.
    auto info = *recovered;
    info.setFile = base;
    info.declined = false;
    autosaveHasCopy_ = recovery_.commit(info);
    const auto when = juce::Time(info.savedAtMs).formatted("%H:%M");
    ui_.statusText = "Recovered the unsaved changes to " + info.setName + " (" + when.toStdString()
                     + "). Save (Cmd+S) to keep them";
    logf("recovery: opened \"%s\" from %s as %s: %zu tracks, tempo %.2f, track 1 \"%s\", %s",
         info.setName.c_str(), base.getFullPathName().toRawUTF8(), song_->sourcePath.c_str(), song_->tracks.size(), song_->tempo,
         song_->tracks.empty() ? "" : song_->tracks.front().name.c_str(), ui_.documentDirty ? "unsaved" : "saved");
    updateWindowTitle();
    if (juce::SystemStats::getEnvironmentVariable("SONORA_RECOVERY_AUTOTEST", {}) == "recover-save")
    {
        const std::weak_ptr<bool> alive = alive_;
        juce::Timer::callAfterDelay(1000, [this, alive] {
            if (alive.expired()) return;
            saveFile(); // verification: the recovered set saved over its own file (with a Backup of the previous version)
            logf("recovery: autotest saved, status \"%s\", recovery copies %s", ui_.statusText.c_str(),
                 recovery_.autosaveFile().existsAsFile() || recovery_.recoveredFile().existsAsFile() ? "remain" : "removed");
        });
    }
}

void AppController::recoveryDocumentSaved()
{
    ++autosaveEpoch_;
    recovery_.clearDocument();
    autosaveHasCopy_ = false;
    autosavedVersion_ = autosaveSeenVersion_ = history_.version();
    autosavePendingSince_ = 0;
}

void AppController::offerRecovery()
{
    if (recoveryOffered_) return;
    recoveryOffered_ = true;
    if (recoveryOffers_.empty()) return;
    auto entry = recoveryOffers_.front();
    // Older copies are not offered; they stay in the Recovery folder until rotation removes them.
    for (size_t i = 1; i < recoveryOffers_.size(); ++i) (void) io::recovery::Store::decline(recoveryOffers_[i]);
    recoveryOffers_.clear();

    const auto name = entry.setName.empty() ? std::string("Untitled") : entry.setName;
    const auto when = juce::Time(entry.savedAtMs);
    const auto today = juce::Time::getCurrentTime().formatted("%Y-%m-%d") == when.formatted("%Y-%m-%d");
    std::string message = "Recover the unsaved changes to “" + name + "” from "
                          + when.formatted(today ? "%H:%M" : "%d %b %Y %H:%M").toStdString() + "?";
    const auto document = fileOf(entry.documentPath);
    if (! entry.untitled && ! entry.documentPath.empty() && ! document.existsAsFile())
        message += " The set is no longer at " + entry.documentPath + "; Save asks where to put it.";
    else if (entry.documentChangedSince())
        message += " “" + document.getFileName().toStdString()
                   + "” was saved again after this copy was made: saving the recovered set replaces that version (it goes to the Backup folder).";
    else
        message += " It opens as an unsaved set; Save writes it to its own file.";

    PromptOptions options;
    options.state.title = "Sonora quit unexpectedly";
    options.state.message = message;
    options.state.buttons = {{"Recover", true, false}, {"Don’t Recover", false, true}};
    options.defaultButton = 0;
    options.cancelButton = 1;
    options.shortcuts = {0, 'd'};
    std::weak_ptr<bool> alive = alive_;
    logf("recovery: offering \"%s\" (%s, saved %s)", name.c_str(), entry.setFile.getFullPathName().toRawUTF8(),
         when.toString(true, true).toRawUTF8());
    const bool shown = PromptWindow::show(std::move(options), [this, alive, entry](int choice) {
        if (alive.expired()) return;
        if (choice == 0)
        {
            openRecovered(entry);
            return;
        }
        (void) io::recovery::Store::decline(entry);
        ui_.statusText = "The recovery copy stays in " + entry.folder.getFullPathName().toStdString() + " for 7 days";
        logf("recovery: declined; kept in %s", entry.folder.getFullPathName().toRawUTF8());
        if (ui_.requestRepaint) ui_.requestRepaint();
    });
    if (! shown)
    {
        recoveryOffers_ = {entry}; // another prompt is open: offer again on the next call
        recoveryOffered_ = false;
        return;
    }
    // Verification without driving the UI: SONORA_RECOVERY_AUTOTEST=recover|recover-save|discard answers the prompt.
    const auto answer = juce::SystemStats::getEnvironmentVariable("SONORA_RECOVERY_AUTOTEST", {});
    if (answer.isNotEmpty())
        juce::Timer::callAfterDelay(1500, [answer] {
            logf("recovery: autotest prompt %s, answering %s", PromptWindow::isShowing() ? "shown" : "MISSING", answer.toRawUTF8());
            PromptWindow::pressKey(answer == "discard" ? juce::KeyPress(juce::KeyPress::escapeKey) : juce::KeyPress(juce::KeyPress::returnKey));
        });
}

void AppController::openRecovered(io::recovery::Entry entry)
{
    if (entry.documentPath.empty() || ! juce::File::isAbsolutePath(juce::String::fromUTF8(entry.documentPath.c_str())))
    {
        ui_.statusText = "The recovery copy does not say which set it belongs to; it stays in " + entry.folder.getFullPathName().toStdString();
        (void) io::recovery::Store::decline(entry);
        return;
    }
    confirmDiscardChanges([this, entry] {
        if (ui_.recording)
        {
            ui_.statusText = "Stop recording before opening another set";
            return;
        }
        pendingRecovery_ = std::make_unique<io::recovery::Entry>(entry);
        const auto base = recovery_.recoveredFile();
        openDocument(entry.setName + " (recovered)", [entry, base] {
            io::AlsLoadResult result;
            // A copy in this run's folder becomes the base the set is patched onto until it is saved; the crashed run's
            // folder stays as a backup until rotation removes it.
            if (! entry.setFile.copyFileTo(base))
            {
                result.error = "could not copy " + entry.setFile.getFullPathName().toStdString();
                return result;
            }
            (void) io::recovery::Store::decline(entry);
            return io::loadAls(base, entry.documentPath);
        });
    });
}

void AppController::runAutosaveAutotest()
{
    // SONORA_AUTOSAVE_AUTOTEST=<bpm>: once the first set opened (not a recovered one), set the tempo and rename track 1,
    // two real undoable edits, so the autosave and a recovery after kill -9 can be verified without driving the UI.
    const auto bpm = juce::SystemStats::getEnvironmentVariable("SONORA_AUTOSAVE_AUTOTEST", {}).getDoubleValue();
    if (bpm <= 0 || autosaveAutotestDone_ || ! song_ || ! song_->patchSourcePath.empty()) return;
    autosaveAutotestDone_ = true;
    const std::weak_ptr<bool> alive = alive_;
    juce::Timer::callAfterDelay(1500, [this, alive, bpm] {
        if (alive.expired() || ! song_) return;
        applyEdit(model::setTempo(*song_, bpm));
        if (! song_->tracks.empty()) applyEdit(model::renameTrack(*song_, 0, "PRJ4 Autosave"));
        logf("autosave: autotest edited \"%s\": tempo %.2f, track 1 \"%s\"", song_->sourcePath.c_str(), song_->tempo,
             song_->tracks.empty() ? "" : song_->tracks.front().name.c_str());
    });
}

} // namespace sonora::app
