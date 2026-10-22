#pragma once
// Live's grid options as a popup menu (right-click in the Arrangement or a clip editor): Snap to Grid, Triplet Grid,
// Fixed Grid, Adaptive Grid density, the fixed sizes and Narrow / Widen Grid.

#include "ui/GridModel.h"
#include "ui/Theme.h"
#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <string>
#include <vector>

namespace sonora::ui
{

struct MenuAction
{
    std::string label;
    std::function<void()> run;
    bool enabled = true;
    std::string shortcut{};
};

class GridChoiceTable final : public juce::PopupMenu::CustomComponent
{
public:
    GridChoiceTable(GridSettings&, double beatsPerBar, bool adaptive, std::function<void()> changed, float zoom);
    void getIdealSize(int& w, int& h) override;
    void paint(juce::Graphics&) override;
    void mouseUp(const juce::MouseEvent&) override;
    bool keyPressed(const juce::KeyPress&) override;
    juce::Rectangle<float> cellBounds(int index) const;
    void choose(int index);
private:
    GridSettings& settings_;
    double beatsPerBar_;
    bool adaptive_;
    float zoom_;
    int focused_ = 0;
    std::function<void()> changed_;
};

juce::PopupMenu createGridMenu(GridSettings&, double beatsPerBar, std::function<void()> changed,
                               std::vector<MenuAction> extra = {}, float zoom = 1);
juce::Image renderGridMenuPreview(GridSettings&, double beatsPerBar, const Theme&, std::vector<MenuAction> extra = {}, float scale = 2);

// `settings` must outlive the menu; `changed` runs after every grid pick. `extra` items are listed first, followed by a
// separator (context-specific commands such as "Insert MIDI Clip").
void showGridMenu(GridSettings& settings, double beatsPerBar, std::function<void()> changed,
                  std::vector<MenuAction> extra = {}, float zoom = 1);

// Cmd+1 narrow, Cmd+2 widen, Cmd+3 triplet, Cmd+4 snap, Cmd+5 fixed: returns true when the key was a grid shortcut.
bool handleGridShortcut(GridSettings& settings, int textChar);

} // namespace sonora::ui
