#pragma once
// UI-35: note-relative expression curves; edits commit through the shared clip history.
#include "core/Edit.h"
#include "ui/Widget.h"

namespace sonora::ui
{
class MpeEditor final : public Widget
{
public:
    void paint(SkCanvas&, const Theme&) override;
    void mouseDown(const MouseEvent&) override;
    void mouseDrag(const MouseEvent&) override;
    void mouseUp(const MouseEvent&) override;
    bool keyPressed(const KeyEvent&) override;
    void focusLost() override;
    Rect plot() const;
    Rect dimensionBounds(int dimension) const;
    void selectNote(int index) { sync(); selected_ = index; }
    int selectedNote() const { return selected_; }
private:
    void sync();
    void update(const MouseEvent&);
    const model::NoteExpression* expression(const model::Clip&) const;
    model::ClipLocation location_;
    model::Clip original_, working_;
    const model::Song* song_ = nullptr;
    int uid_ = -1, selected_ = 0, dimension_ = 0;
    int grabbed_ = -1;
    bool dragging_ = false, changed_ = false;
};
} // namespace sonora::ui
