#include "plugin/PhaseAlignPluginEditor.h"

#include "plugin/PhaseAlignPlugin.h"
#include "ui/views/PhaseAlignFace.h"

namespace sonora::plugin
{
PhaseAlignPluginEditor::PhaseAlignPluginEditor(PhaseAlignPlugin& plugin) : ProFaceEditor(plugin)
{
    face_ = view().makeFace<ui::PhaseAlignFace>(plugin.device());
    open();
}
} // namespace sonora::plugin
