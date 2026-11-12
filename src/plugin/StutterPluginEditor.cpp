#include "plugin/StutterPluginEditor.h"

#include "plugin/StutterPlugin.h"
#include "ui/views/StutterFace.h"

namespace sonora::plugin
{
StutterPluginEditor::StutterPluginEditor(StutterPlugin& plugin) : ProFaceEditor(plugin)
{
    face_ = view().makeFace<ui::StutterFace>(plugin.device());
    open();
}
} // namespace sonora::plugin
