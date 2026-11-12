#pragma once
// The Sonora Phase Align VST3's window (PLG-21, PLG-23): the PhaseAlignFace's full layout (ProFaceEditor). The kick comes
// in through the plug-in's sidechain bus, routed by the host.

#include "plugin/ProFaceEditor.h"

namespace sonora::ui { class PhaseAlignFace; }

namespace sonora::plugin
{
class PhaseAlignPlugin;

class PhaseAlignPluginEditor final : public ProFaceEditor
{
public:
    explicit PhaseAlignPluginEditor(PhaseAlignPlugin&);
    ui::PhaseAlignFace& face() { return *face_; }

private:
    ui::PhaseAlignFace* face_ = nullptr;
};
} // namespace sonora::plugin
