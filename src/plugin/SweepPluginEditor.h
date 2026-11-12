#pragma once
// The Sonora Sweep VST3's window (PLG-19, PLG-23): the SweepFace's full layout (ProFaceEditor). Samples come from a file
// chooser or a drop (the Finder, the host's browser).

#include "plugin/ProFaceEditor.h"

#include <memory>

namespace sonora::ui { class SweepFace; }

namespace sonora::plugin
{
class SweepPlugin;

class SweepPluginEditor final : public ProFaceEditor
{
public:
    explicit SweepPluginEditor(SweepPlugin&);
    ui::SweepFace& face() { return *face_; }

private:
    SweepPlugin& plugin_;
    ui::SweepFace* face_ = nullptr;
    std::unique_ptr<juce::FileChooser> chooser_;
};
} // namespace sonora::plugin
