#include "plugin/CorrelationPluginEditor.h"

#include "plugin/CorrelationPlugin.h"
#include "ui/views/CorrelationFace.h"

namespace sonora::plugin
{
CorrelationPluginEditor::CorrelationPluginEditor(CorrelationPlugin& plugin) : ProFaceEditor(plugin)
{
    face_ = view().makeFace<ui::CorrelationFace>(plugin.device());
    open();
}
} // namespace sonora::plugin
