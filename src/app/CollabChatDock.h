#pragma once
// COL-9: the session chat beside the canvas (View > Session Chat, the Collaborate panel's Chat button).
//
// Native text input under ADR 0008: on macOS an AppKit NSTextView transcript (selectable, names in each person's
// colour) and an NSTextView composer (IME, Unicode, text undo; Return sends unless an IME composition is open,
// Shift+Return adds a line). CollabChatDockPortable.cpp is the JUCE TextEditor version for Windows/Linux. The dock
// only shows lines and hands typed text to onSend; the session and its limits live in collab::CollabSession.
#include <juce_gui_extra/juce_gui_extra.h>

#include <functional>
#include <string>
#include <vector>

namespace sonora::app
{
class CollabChatDock final : public juce::Component
{
public:
    struct Line
    {
        std::string name, text, time; // time "14:05"
        uint32_t color = 0xff808080;   // ARGB, the participant colour
        bool self = false, pending = false, history = false;
        std::string failed;            // own line the host did not pass on
    };

    CollabChatDock();
    ~CollabChatDock() override;
    void resized() override;

    void setLines(const std::vector<Line>&);
    // Disabled composers say why ("The host turned chat off for this session.").
    void setComposer(bool enabled, const std::string& reason);
    void setSubtitle(const std::string&);
    void focusComposer();
    void send();

    // Returns "" when the line went out, else a reason the dock shows.
    std::function<std::string(const std::string&)> onSend;
    std::function<void()> onClose, onFocus;

    // Developer checks (collab_probe --chat-dock): a PNG of the native view, and IME/Unicode/Return behaviour.
    bool writeSnapshot(const juce::File&);
    bool verifyNativeInput();
    static constexpr int preferredWidth = 320;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
#if JUCE_MAC
    juce::NSViewComponent holder_;
#endif
};
} // namespace sonora::app
