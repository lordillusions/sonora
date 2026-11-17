#pragma once
// The Sonora Seraph VST3's window (PLG-27): the SeraphFace's full layout (ProFaceEditor).

#include "plugin/ProFaceEditor.h"

#include <memory>

namespace sonora::ui { class SeraphFace; }

namespace sonora::plugin
{
class SeraphPlugin;

class SeraphPluginEditor final : public ProFaceEditor
{
public:
    explicit SeraphPluginEditor(SeraphPlugin&);
    ui::SeraphFace& face() { return *face_; }

private:
    ui::SeraphFace* face_ = nullptr;
    std::unique_ptr<juce::FileChooser> chooser_;
};
} // namespace sonora::plugin
