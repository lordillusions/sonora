// LIB-2: the reference library's device presets (.adv) in the Browser and loading them onto tracks.
//
// The presets are read at run time from the installed reference application, the User Library and installed packs
// (io::presetLibraries, platform/ReferenceLibrary.h; SONORA_REFERENCE_LIBRARY replaces them) and never copied. The index
// is a directory listing on a worker. Loading reads one preset as a device of the set (io::readDevicePreset: fresh ids,
// the preset's own XML as the device's source XML), so everything Sonora does not model survives a save.
//
// SONORA_PRESET_AUTOTEST="track:/abs/preset.adv|track@device:/abs/preset.adv|..." loads each preset 4 s after the set
// opens (track@device replaces that top-level device), logs "[sonora] preset:" lines and saves the set in place. Use a copy.
#include "app/AppController.h"
#include "core/DeviceCatalog.h"
#include "io/als/DevicePresets.h"
#include "util/Log.h"

namespace sonora::app
{

void AppController::scanDevicePresets()
{
    const std::weak_ptr<bool> alive = alive_;
    const auto userLibrary = ui_.userLibraryPath;
    const std::vector<std::string> packs{ui_.packsPath};
    pool_.addJob([this, alive, userLibrary, packs] {
        const auto found = io::scanDevicePresets(io::presetLibraries(userLibrary, packs));
        std::vector<ui::UiContext::BrowserPreset> presets;
        presets.reserve(found.size());
        for (const auto& p : found) presets.push_back({p.path, p.name, p.folder, p.deviceTag, p.deviceName});
        juce::MessageManager::callAsync([this, alive, list = std::move(presets)]() mutable {
            if (alive.expired()) return;
            logf("device presets: %zu", list.size());
            ui_.devicePresets = std::move(list);
            ++ui_.browserContentRevision;
            if (ui_.requestRepaint) ui_.requestRepaint();
        });
    });
}

void AppController::loadDevicePreset(const ui::BrowserDrag& item, int trackIndex, int replaceIndex)
{
    if (! song_ || editingDevices_ || syncingModel_ || exporting_) return;
    if (ui_.recording || ui_.recordingRequested) { ui_.statusText = "Finish recording before loading presets"; return; }
    const auto* track = model::trackAt(*song_, trackIndex);
    if (! track) { ui_.statusText = "Drop the preset on a track"; return; }
    if (track->frozen) { ui_.statusText = "Unfreeze this track before editing its clips or devices"; return; }
    const int generation = documentGeneration_;
    syncModelFromEngine(); // the rest of the track keeps the state it has right now
    if (generation != documentGeneration_ || ! song_) return;
    track = model::trackAt(*song_, trackIndex);
    if (! track) return;

    std::vector<juce::File> libraries;
    for (const auto& library : io::presetLibraries(ui_.userLibraryPath, {ui_.packsPath}))
        libraries.emplace_back(juce::String::fromUTF8(library.path.c_str()));
    auto loaded = io::readDevicePreset(juce::File(juce::String::fromUTF8(item.value.c_str())), *song_, libraries);
    if (! loaded.error.empty()) { ui_.statusText = "Could not load " + item.label + ": " + loaded.error; return; }
    for (const auto& warning : loaded.warnings) logf("preset %s: %s", item.label.c_str(), warning.c_str());
    auto& device = loaded.device;
    if (! model::canHoldDevice(*song_, trackIndex, device))
    {
        ui_.statusText = model::deviceRole(device) == model::DeviceRole::Instrument ? "Drop instruments on a MIDI track" : "Drop MIDI effects on a MIDI track";
        return;
    }
    const bool hotSwap = replaceIndex >= 0 && (size_t) replaceIndex < track->devices.size() && track->devices[(size_t) replaceIndex].type == device.type;
    const bool replacingInstrument = ! hotSwap && model::deviceRole(device) == model::DeviceRole::Instrument && model::instrumentIndex(*track) >= 0;
    song_->nextDeviceId = std::max(song_->nextDeviceId, device.id + 1);
    song_->nextPointeeId = std::max(song_->nextPointeeId, loaded.pointeeEnd);
    model::allocateDeviceIds(*song_, device); // parameters the preset has no automation target for
    const int newId = device.id;
    const auto name = device.displayName, trackName = track->name;
    auto command = hotSwap ? model::replaceDevice(*song_, trackIndex, replaceIndex, std::move(device), "Load " + item.label)
                           : model::insertTrackDevice(*song_, trackIndex, std::move(device));
    if (! command.apply) return;
    command.label = "Load " + item.label;
    if (! applyEdit(std::move(command)).ok) return;
    ui_.selectedTrack = trackIndex;
    ui_.selectedDeviceId = newId;
    ui_.detailTab = ui::DetailTab::Devices;
    ui_.detailVisible = true;
    ui_.statusText = (hotSwap ? "Replaced with " : replacingInstrument ? "Replaced instrument with " : "Loaded ") + name
                   + (hotSwap ? std::string() : " on " + trackName);
}

void AppController::runPresetAutotest()
{
    const auto spec = juce::SystemStats::getEnvironmentVariable("SONORA_PRESET_AUTOTEST", {});
    if (spec.isEmpty()) return;
    const std::weak_ptr<bool> alive = alive_;
    juce::Timer::callAfterDelay(4000, [this, alive, spec] {
        if (alive.expired() || ! song_) return;
        for (const auto& step : juce::StringArray::fromTokens(spec, "|", ""))
        {
            const auto target = step.upToFirstOccurrenceOf(":", false, false), path = step.fromFirstOccurrenceOf(":", false, false);
            const int track = target.upToFirstOccurrenceOf("@", false, false).getIntValue();
            const int device = target.contains("@") ? target.fromFirstOccurrenceOf("@", false, false).getIntValue() : -1;
            ui::BrowserDrag drag;
            drag.kind = ui::BrowserDrag::Kind::DevicePreset;
            drag.value = path.toStdString();
            drag.label = juce::File(path).getFileNameWithoutExtension().toStdString();
            loadDevicePreset(drag, track, device);
            std::string chain;
            if (const auto* t = model::trackAt(*song_, track))
                for (const auto& d : t->devices) chain += " " + d.type + "\"" + d.displayName + "\"#" + std::to_string(d.id);
            logf("preset: %s -> \"%s\" |%s", step.toRawUTF8(), ui_.statusText.c_str(), chain.c_str());
        }
        saveFile();
        logf("preset: saved, status \"%s\"", ui_.statusText.c_str());
    });
}

} // namespace sonora::app
