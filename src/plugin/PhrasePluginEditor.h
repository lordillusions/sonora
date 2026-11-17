#pragma once
// The Sonora Phrase VST3's window (PLG-25): the PhraseFace's full layout (ProFaceEditor), with Drag MIDI in place of
// To Clip.

#include "plugin/ProFaceEditor.h"

namespace sonora::ui { class PhraseFace; }

namespace sonora::plugin
{
class PhrasePlugin;

class PhrasePluginEditor final : public ProFaceEditor
{
public:
    explicit PhrasePluginEditor(PhrasePlugin&);
    ui::PhraseFace& face() { return *face_; }

private:
    PhrasePlugin& plugin_;
    ui::PhraseFace* face_ = nullptr;
};
} // namespace sonora::plugin
