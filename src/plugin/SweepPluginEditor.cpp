#include "plugin/SweepPluginEditor.h"

#include "plugin/SweepPlugin.h"
#include "ui/views/SweepFace.h"

#include <cmath>

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
    // PLG-19d: the rendered sweep as a WAV: saved where the person chooses, or dragged out of the display to the host.
    face_->onSaveWav = [this] {
        const auto name = plugin_.device().sampleName().empty() ? juce::String("Sonora Sweep") : juce::String::fromUTF8(plugin_.device().sampleName().c_str()) + " swell";
        chooser_ = std::make_unique<juce::FileChooser>("Save the sweep as a WAV",
                                                       juce::File::getSpecialLocation(juce::File::userMusicDirectory).getChildFile(name + ".wav"), "*.wav");
        chooser_->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting,
                              [this](const juce::FileChooser& fc) {
            const auto file = fc.getResult();
            if (file == juce::File()) return;
            if (std::string error; ! plugin_.device().renderToFile(file.withFileExtension("wav"), error))
                plugin_.buildLog.push_back("Could not save the WAV: " + error);
        });
    };
    face_->onDragOut = [this] {
        const auto file = renderFolder().getChildFile(juce::String::fromUTF8(dragName().c_str()) + ".wav").getNonexistentSibling();
        std::string error;
        if (! plugin_.device().renderToFile(file, error)) { plugin_.buildLog.push_back("Could not render the sweep: " + error); return; }
        juce::DragAndDropContainer::performExternalDragDropOfFiles({file.getFullPathName()}, false, this);
    };
    onFilesDropped = [this](const std::vector<std::string>& paths) {
        if (! paths.empty()) plugin_.loadSample(juce::File(juce::String::fromUTF8(paths.front().c_str())));
    };
    open();
}

juce::File SweepPluginEditor::renderFolder()
{
    return juce::File::getSpecialLocation(juce::File::userMusicDirectory).getChildFile("Sonora/Sweep Renders");
}

std::string SweepPluginEditor::dragName() const
{
    const auto& d = plugin_.device();
    const bool up = std::lround(d.getParameter(devices::Sweep::Direction)) == devices::Sweep::Up;
    std::string base = d.sampleName().empty() || std::lround(d.getParameter(devices::Sweep::Source)) != devices::Sweep::Sample
                           ? std::string("Sonora Sweep")
                           : d.sampleName();
    return base + (up ? " riser" : " downlifter");
}
} // namespace sonora::plugin
