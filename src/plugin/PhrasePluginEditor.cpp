#include "plugin/PhrasePluginEditor.h"

#include "plugin/PhrasePlugin.h"
#include "ui/views/PhraseFace.h"

namespace sonora::plugin
{
PhrasePluginEditor::PhrasePluginEditor(PhrasePlugin& plugin) : ProFaceEditor(plugin), plugin_(plugin)
{
    face_ = view().makeFace<ui::PhraseFace>(plugin.device());
    face_->onDragMidi = [this] { plugin_.dragMidi(this); };
    open();
}
} // namespace sonora::plugin
