#pragma once
// COL-5: the Collaborate panel (File > Collaborate..., or the people button in the top bar).
//
// Before a session: your name, Share on Local Network / Share through Relay (with the relay address), Join with an
// invite link, and the sessions other Sonoras advertise on this network. During one: the invite link to copy, everyone
// with their colour, role and safety code, join requests to allow, the shared-transport switch, recent notices and
// Leave / End Session. Text is typed in native prompts (the app's callbacks); this widget only shows UiContext::collab
// and calls UiContext::collab* callbacks.

#include "ui/Widget.h"

#include <functional>
#include <vector>

namespace sonora::ui
{

class CollabPanel : public Widget
{
public:
    void paint(SkCanvas& c, const Theme& t) override;
    void mouseDown(const MouseEvent& e) override;
    bool keyPressed(const KeyEvent& k) override;

    Rect panelRect() const;

private:
    struct Hit
    {
        Rect area;
        std::function<void()> action;
    };
    void button(SkCanvas& c, const Theme& t, const Rect& r, const std::string& label, std::function<void()> action, bool enabled = true,
                bool primary = false);
    void close();

    std::vector<Hit> hits_;
};

// The people button in the top bar: avatars of everyone in the session, or "Share" before one.
class CollabBadge : public Widget
{
public:
    CollabBadge() { tooltip = "Collaborate: share this set or join someone else's session"; }
    void paint(SkCanvas& c, const Theme& t) override;
    void mouseDown(const MouseEvent& e) override;
    // Width it wants for the current state.
    float preferredWidth() const;
};

} // namespace sonora::ui
