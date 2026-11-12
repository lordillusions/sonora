#pragma once
// The Sonora Stutter VST3's window (PLG-18, PLG-23): the StutterFace's full layout (ProFaceEditor).

#include "plugin/ProFaceEditor.h"

namespace sonora::ui { class StutterFace; }

namespace sonora::plugin
{
class StutterPlugin;

class StutterPluginEditor final : public ProFaceEditor
{
public:
    explicit StutterPluginEditor(StutterPlugin&);
    ui::StutterFace& face() { return *face_; }

private:
    ui::StutterFace* face_ = nullptr;
};
} // namespace sonora::plugin
