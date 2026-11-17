#include "plugin/SeraphPluginEditor.h"

#include "plugin/SeraphPlugin.h"
#include "ui/views/SeraphFace.h"

namespace sonora::plugin
{
SeraphPluginEditor::SeraphPluginEditor(SeraphPlugin& plugin) : ProFaceEditor(plugin)
{
    face_ = view().makeFace<ui::SeraphFace>(plugin.device());
    // PLG-27b: sung notes dropped on the window (the Finder, the host's browser) or chosen with Load... become Real voices
    onFilesDropped = [this](const std::vector<std::string>& paths) { face_->loadVoices(paths); };
    face_->onChooseVoices = [this] {
        chooser_ = std::make_unique<juce::FileChooser>("Load sung notes into Sonora Seraph", juce::File(), "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.ogg");
        chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles
                                  | juce::FileBrowserComponent::canSelectMultipleItems,
                              [this](const juce::FileChooser& fc) {
            std::vector<std::string> paths;
            for (const auto& file : fc.getResults()) paths.push_back(file.getFullPathName().toStdString());
            face_->loadVoices(paths);
        });
    };
    open();
}
} // namespace sonora::plugin
