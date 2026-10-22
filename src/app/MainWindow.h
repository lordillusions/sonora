#pragma once
// Main document window + macOS menu bar. Hosts the Skia canvas with ui::MainLayout.

#include "app/AppController.h"
#include "app/AssistantDock.h"
#include "app/Updater.h"
#include "gfx/SkiaCanvasComponent.h"
#include "ui/views/MainLayout.h"
#include "ui/MenuLookAndFeel.h"
#include "ui/Shortcuts.h"

#include <map>

#include <juce_gui_basics/juce_gui_basics.h>

namespace sonora::app
{

class MainWindow : public juce::DocumentWindow, private juce::MenuBarModel
{
public:
    explicit MainWindow(AppController& app);
    ~MainWindow() override;

    void closeButtonPressed() override;
    void toggleAssistant();

private:
    // juce::MenuBarModel
    juce::StringArray getMenuBarNames() override;
    juce::PopupMenu getMenuForIndex(int index, const juce::String& name) override;
    void menuItemSelected(int itemId, int topLevelMenuIndex) override;

    void updateChrome();
    // REQ-3a: the menu bar is the registry of rebindable commands; defaults come from the shortcut column of each item.
    void buildShortcutRegistry();
    bool commandEnabled(int id);
    ui::ShortcutMap shortcuts_;
    std::map<int, int> commandMenu_; // menu item id -> top-level menu index
    bool shortcutsReady_ = false;
    ui::MenuLookAndFeel menuLook_;
    juce::Colour chromeColour_{0};
    AppController& app_;
    std::vector<juce::File> recentMenuFiles_; // stable menu IDs even when an asynchronous open updates history
    std::unique_ptr<ui::MainLayout> layout_;
    gfx::SkiaCanvasComponent* canvas_ = nullptr; // owned by the window (content component)
    AssistantDock* assistant_ = nullptr;
    void createUpdater(); // COM-4
    std::unique_ptr<Updater> updater_;
};

} // namespace sonora::app
