#include "plugin/SweepPluginEditor.h"

#include "plugin/SweepPlugin.h"
#include "ui/views/SweepFace.h"

namespace sonora::plugin
{
SweepPluginEditor::SweepPluginEditor(SweepPlugin& plugin) : ProFaceEditor(plugin), plugin_(plugin)
{
    face_ = view().makeFace<ui::SweepFace>(plugin.device());
    face_->onSampleDropped = [this](const std::string& path) { plugin_.loadSample(juce::File(juce::String::fromUTF8(path.c_str()))); };
    face_->onChooseSample = [this] {
        chooser_ = std::make_unique<juce::FileChooser>("Load a sample into Sonora Sweep", juce::File(), "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.ogg");
        chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [this](const juce::FileChooser& fc) {
            if (const auto file = fc.getResult(); file != juce::File()) plugin_.loadSample(file);
        });
    };
    onFilesDropped = [this](const std::vector<std::string>& paths) {
        if (! paths.empty()) plugin_.loadSample(juce::File(juce::String::fromUTF8(paths.front().c_str())));
    };
    open();
}
} // namespace sonora::plugin
