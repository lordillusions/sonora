// Export Audio: the dialog's settings, destination chooser, the worker that renders offline and the hand-over of
// the result. The rendering itself is engine::Exporter; this file owns the threading and the UI state.

#include "app/AppController.h"

#include "util/Log.h"

#include <juce_audio_utils/juce_audio_utils.h>

namespace sonora::app
{

void AppController::loadExportSettings()
{
    auto& p = settings();
    auto& s = ui_.exportSettings;
    s.format = std::clamp(p.getIntValue("exportFormat", 0), 0, 2);
    s.bitDepth = p.getIntValue("exportBitDepth", 24);
    if (s.bitDepth != 16 && s.bitDepth != 24 && s.bitDepth != 32) s.bitDepth = 24;
    if (s.format != 0 && s.bitDepth == 32) s.bitDepth = 24;
    s.normalize = p.getBoolValue("exportNormalize", false);
    s.mono = p.getBoolValue("exportMono", false);
    s.dither = p.getBoolValue("exportDither", true);
    s.tail = std::clamp(p.getIntValue("exportTail", 0), 0, 5);
}

void AppController::saveExportSettings()
{
    auto& p = settings();
    const auto& s = ui_.exportSettings;
    p.setValue("exportFormat", s.format);
    p.setValue("exportBitDepth", s.bitDepth);
    p.setValue("exportNormalize", s.normalize);
    p.setValue("exportMono", s.mono);
    p.setValue("exportDither", s.dither);
    p.setValue("exportTail", s.tail);
    p.saveIfNeeded();
}

void AppController::startExport(const ui::ExportSettings& s)
{
    if (exporting_ || ! song_)
        return;
    if (ui_.recording)
    {
        ui_.exportState.phase = ui::ExportState::Phase::Failed;
        ui_.exportState.message = "Stop recording before exporting.";
        return;
    }
    if (editingDevices_ || syncingModel_)
    {
        ui_.exportState.phase = ui::ExportState::Phase::Failed;
        ui_.exportState.message = "Wait until the device operation has finished.";
        return;
    }
    saveExportSettings();

    engine::ExportOptions o;
    const auto range = engine::Exporter::resolveRange(*song_, s.range, s.startBar, s.endBar);
    o.startBeat = range.startBeat;
    o.endBeat = range.endBeat;
    o.target = s.target == 0 ? engine::ExportTarget::Main : s.target == 1 ? engine::ExportTarget::AllTracks : engine::ExportTarget::SingleTrack;
    o.trackIndex = s.target >= 2 ? s.target - 2 : -1;
    static const double tails[] = {0.0, 1.0, 2.0, 4.0, 8.0, 0.0};
    o.tailSeconds = tails[std::clamp(s.tail, 0, 5)];
    o.autoTail = s.tail == 5;
    o.format = s.format == 1 ? engine::ExportFormat::Aiff : s.format == 2 ? engine::ExportFormat::Flac : engine::ExportFormat::Wav;
    o.bitDepth = s.bitDepth;
    o.dither = s.dither;
    o.normalize = s.normalize;
    o.mono = s.mono;

    const juce::File setFile(juce::String::fromUTF8(song_->sourcePath.c_str()));
    const auto name = isUntitled() ? juce::String("Untitled") : setFile.getFileNameWithoutExtension();
    o.baseName = name.toStdString();
    auto folder = isUntitled() ? juce::File::getSpecialLocation(juce::File::userMusicDirectory) : setFile.getParentDirectory();
    const std::string extension = engine::Exporter::extensionFor(o.format);
    juce::String suggestion = name;
    if (o.target == engine::ExportTarget::SingleTrack)
        if (const auto* track = ui_.trackAt(o.trackIndex))
            suggestion += " - " + juce::String(engine::Exporter::safeFileName(track->name));

    const bool stems = o.target == engine::ExportTarget::AllTracks;
    exportChooser_ = std::make_unique<juce::FileChooser>(stems ? "Choose where to create the stems folder" : "Export Audio",
                                                         stems ? folder : folder.getChildFile(suggestion + juce::String(extension)),
                                                         stems ? juce::String() : juce::String("*") + juce::String(extension));
    const int flags = stems ? juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories
                            : juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                                  | juce::FileBrowserComponent::warnAboutOverwriting;
    std::weak_ptr<bool> alive = alive_;
    exportChooser_->launchAsync(flags, [this, alive, o, stems, name, extension](const juce::FileChooser& chooser) mutable {
        if (alive.expired())
            return;
        auto chosen = chooser.getResult();
        exportChooser_.reset();
        if (chosen == juce::File())
            return; // cancelled: the dialog stays open
        if (stems)
            chosen = chosen.getChildFile(name + " Stems");
        else
            chosen = chosen.withFileExtension(juce::String(extension).substring(1));
        o.destination = chosen;
        beginExport(std::move(o));
    });
}

void AppController::beginExport(engine::ExportOptions options)
{
    if (exporting_ || ! song_)
        return;
    ui_.exportState = {};
    ui_.exportOpen = true;
    engine_.transport().stop();
    if (! engine_.beginOffline())
    {
        ui_.exportState.phase = ui::ExportState::Phase::Failed;
        ui_.exportState.message = "The audio engine is not ready.";
        return;
    }
    exporting_ = true;
    exportCancel_ = false;
    exportProgress_ = 0.0f;
    ui_.exportState.phase = ui::ExportState::Phase::Running;
    exportPoller_.startTimerHz(10);
    if (ui_.requestRepaint)
        ui_.requestRepaint();

    std::weak_ptr<bool> alive = alive_;
    pool_.addJob([this, alive, job = std::move(options)] {
        engine::Exporter exporter(engine_, *song_);
        auto result = std::make_shared<engine::ExportResult>(exporter.run(job, exportProgress_, exportCancel_));
        juce::MessageManager::callAsync([this, alive, result] {
            if (! alive.expired())
                finishExport(*result);
        });
    });
}

void AppController::ExportPoller::timerCallback()
{
    owner_.ui_.exportState.progress = owner_.exportProgress_.load();
    if (owner_.ui_.requestRepaint)
        owner_.ui_.requestRepaint();
}

void AppController::finishExport(const engine::ExportResult& result)
{
    exportPoller_.stopTimer();
    engine_.endOffline();
    exporting_ = false;
    auto& st = ui_.exportState;
    st = {};
    if (result.ok)
    {
        st.phase = ui::ExportState::Phase::Done;
        st.progress = 1.0f;
        for (const auto& f : result.files)
            st.files.push_back(f.file.getFullPathName().toStdString());
        st.integratedLufs = result.integratedLufs;
        st.loudnessRange = result.loudnessRange;
        st.truePeakDb = result.truePeakDb;
        st.samplePeakDb = result.samplePeakDb;
        st.seconds = result.seconds;
        ui_.statusText = "Exported " + std::to_string(result.files.size()) + " file(s)";
    }
    else if (result.cancelled)
    {
        ui_.exportOpen = false;
        ui_.statusText = "Export cancelled";
    }
    else
    {
        st.phase = ui::ExportState::Phase::Failed;
        st.message = result.error;
        ui_.statusText = "Export failed: " + result.error;
        logf("export failed: %s", result.error.c_str());
    }
    if (ui_.modelChanged)
        ui_.modelChanged();
    if (ui_.requestRepaint)
        ui_.requestRepaint();
}

void AppController::cancelExport()
{
    if (exporting_)
        exportCancel_ = true;
}

} // namespace sonora::app
