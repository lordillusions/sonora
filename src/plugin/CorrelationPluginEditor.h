#pragma once
// The Sonora Correlation VST3's window (PLG-24): the CorrelationFace's full layout (ProFaceEditor).

#include "plugin/ProFaceEditor.h"

namespace sonora::ui { class CorrelationFace; }

namespace sonora::plugin
{
class CorrelationPlugin;

class CorrelationPluginEditor final : public ProFaceEditor
{
public:
    explicit CorrelationPluginEditor(CorrelationPlugin&);
    ui::CorrelationFace& face() { return *face_; }

private:
    ui::CorrelationFace* face_ = nullptr;
};
} // namespace sonora::plugin
