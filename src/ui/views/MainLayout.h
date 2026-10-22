#pragma once
// The whole main window, Live 12 layout:
//
//   +---------------------------------------------------------------+
//   | TopBar (control bar)                                          |
//   +-----------+---------------------------------------------------+
//   | Browser   | SessionView  or  ArrangementView  (Tab toggles)   |
//   |           |                                                   |
//   +-----------+---------------------------------------------------+
//   | DetailView (device chain of selected track)                   |
//   +---------------------------------------------------------------+
//   | StatusBar                                                     |
//   +---------------------------------------------------------------+
//
// Global shortcuts live in keyPressed() (see docs/UI_GUIDE.md "Shortcuts").

#include "ui/RootWidget.h"

namespace sonora::ui
{

class TopBar;
class BrowserPanel;
class SessionView;
class ArrangementView;
class DetailView;
class StatusBar;
class PreferencesPanel;
class ExportPanel;
class Splitter;
class IconButton;

class MainLayout : public RootWidget
{
public:
    explicit MainLayout(UiContext& ctx);

    void layout() override;
    void paint(SkCanvas& c, const Theme& t) override;
    void paintOverChildren(SkCanvas& c, const Theme& t) override;
    bool keyPressed(const KeyEvent& k) override;
    void tick() override;

private:
    bool compactDevices();
    bool hasResynthEditor();
    bool lastCompact_ = true;
    bool lastResynth_ = false;
    TopBar* top_;
    IconButton* sessionButton_;
    IconButton* arrangementButton_;
    BrowserPanel* browser_;
    SessionView* session_;
    ArrangementView* arrangement_;
    DetailView* detail_;
    StatusBar* status_;
    Splitter* browserSplit_ = nullptr;
    Splitter* detailSplit_ = nullptr;
    PreferencesPanel* prefs_;
    ExportPanel* export_;
    class CollabPanel* collab_ = nullptr;
    bool lastExport_ = false, lastCollab_ = false;
    Rect mainBounds_;
    bool lastPrefs_ = false;
    DetailTab lastTab_ = DetailTab::Devices;
    MainView lastView_ = MainView::Arrangement;
    bool lastBrowser_ = true, lastDetail_ = true;
    const model::Song* inputSong_ = nullptr;
    const model::Song* padSong_ = nullptr; // retained through temporary nulls during device edits
};

} // namespace sonora::ui
