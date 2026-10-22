#pragma once
// UI-25: Settings categories and controls use the same services as menus/toolbar.
#include "ui/Widget.h"

namespace sonora::ui
{
class PreferencesPanel : public Widget
{
public:
    enum class Tab { Plugins, Midi, Display, Audio, Theme, Files, Library, Record, Keys, Updates };
    void paint(SkCanvas&, const Theme&) override;
    void mouseDown(const MouseEvent&) override;
    bool mouseWheel(const MouseEvent&) override;
    bool keyPressed(const KeyEvent&) override;
    void setTab(Tab);
    Rect panelRect() const;
    Rect pageBounds(Tab) const;
    Rect controlBounds(const std::string& label) const;
    int recordingShortcut() const { return recording_; } // REQ-3a: command id waiting for a key, 0 when none

private:
    struct Hit { Rect area; std::string label; std::function<void()> action; bool body = false; };
    void button(SkCanvas&, const Theme&, Rect, const std::string&, const std::string&, bool,
                std::function<void()>);
    void choice(SkCanvas&, const Theme&, Rect, const std::string&, const std::string&,
                std::vector<std::string>, std::function<void(int)>);
    float scroll_ = 0, contentHeight_ = 0;
    int paintedPage_ = -1;
    bool inBody_ = false;
    Rect bodyClip_;
    std::vector<Hit> hits_;
    void paintShortcuts(SkCanvas&, const Theme&, float& y);
    bool shortcutKey(const KeyEvent&);
    int recording_ = 0;
    std::string filter_, shortcutStatus_;
};
} // namespace sonora::ui
