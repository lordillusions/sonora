// COL-9 / COM-7: the session chat for Windows/Linux, built from plain JUCE widgets. macOS uses the AppKit views in
// CollabChatDock.mm. Same lines, limits and keys (Return sends, Shift+Return adds a line); no names in colour yet.
// TODO(COM-7): coloured names and snapshots once the dock moves into the Skia toolkit.

#include "app/CollabChatDock.h"

namespace sonora::app
{
namespace
{
constexpr int maxChars = 1000; // collab::maxChatChars

struct ChatInput final : juce::TextEditor
{
    std::function<void()> onEnter, onFocus;
    bool keyPressed(const juce::KeyPress& key) override
    {
        if (key.getKeyCode() == juce::KeyPress::returnKey && ! key.getModifiers().isShiftDown()) { if (onEnter) onEnter(); return true; }
        return juce::TextEditor::keyPressed(key);
    }
    void focusGained(FocusChangeType cause) override { if (onFocus) onFocus(); juce::TextEditor::focusGained(cause); }
};
} // namespace

struct CollabChatDock::Impl
{
    juce::Label title{{}, "Session Chat"}, subtitle, status, counter{{}, "0 / 1000"};
    juce::TextButton close{"Close"}, sendButton{"Send"};
    juce::TextEditor transcript;
    ChatInput input;
    size_t signature = 0;
    std::string reason;
    bool enabled = true;
};

CollabChatDock::CollabChatDock() : impl_(std::make_unique<Impl>())
{
    auto& n = *impl_;
    n.title.setFont(juce::FontOptions(16.0f, juce::Font::bold));
    n.transcript.setMultiLine(true);
    n.transcript.setReadOnly(true);
    n.transcript.setScrollbarsShown(true);
    n.input.setMultiLine(true, true);
    n.input.setReturnKeyStartsNewLine(true);
    n.input.onEnter = [this] { send(); };
    n.input.onFocus = [this] { if (onFocus) onFocus(); };
    n.input.onTextChange = [this] {
        auto& m = *impl_;
        const int chars = m.input.getText().length();
        m.counter.setText(juce::String(chars) + " / " + juce::String(maxChars), juce::dontSendNotification);
        m.sendButton.setEnabled(m.enabled && chars > 0 && chars <= maxChars);
    };
    n.close.onClick = [this] { if (onClose) onClose(); };
    n.sendButton.onClick = [this] { send(); };
    n.sendButton.setEnabled(false);
    for (juce::Component* c : std::initializer_list<juce::Component*>{&n.title, &n.subtitle, &n.status, &n.counter, &n.close, &n.sendButton,
                                                                      &n.transcript, &n.input})
        addAndMakeVisible(c);
    setName("Session Chat");
    setLines({});
}

CollabChatDock::~CollabChatDock() = default;

void CollabChatDock::resized()
{
    auto& n = *impl_;
    auto area = getLocalBounds().reduced(12);
    auto top = area.removeFromTop(26);
    n.close.setBounds(top.removeFromRight(64));
    n.title.setBounds(top);
    n.subtitle.setBounds(area.removeFromTop(20));
    auto bottom = area.removeFromBottom(28);
    n.sendButton.setBounds(bottom.removeFromRight(70));
    n.counter.setBounds(bottom.removeFromRight(90));
    area.removeFromBottom(6);
    n.input.setBounds(area.removeFromBottom(64));
    area.removeFromBottom(6);
    if (! n.reason.empty() || n.status.getText().isNotEmpty()) n.status.setBounds(area.removeFromBottom(20));
    n.transcript.setBounds(area);
}

void CollabChatDock::setLines(const std::vector<Line>& lines)
{
    auto& n = *impl_;
    size_t signature = lines.size();
    for (const auto& l : lines) signature = (signature ^ std::hash<std::string>{}(l.name + l.text + l.time + l.failed) ^ (l.pending ? 1u : 0u)) * 1099511628211ull;
    if (signature == n.signature && n.transcript.getTotalNumChars() > 0) return;
    n.signature = signature;
    juce::String text;
    if (lines.empty()) text = "Messages go to everyone in the session. They are not saved with the set.";
    for (const auto& l : lines)
    {
        text << juce::String::fromUTF8((l.self ? l.name + " (you)" : l.name).c_str()) << "  " << l.time;
        if (l.pending && l.failed.empty()) text << "  sending...";
        if (! l.failed.empty()) text << "  not sent: " << juce::String::fromUTF8(l.failed.c_str());
        text << "\n" << juce::String::fromUTF8(l.text.c_str()) << "\n\n";
    }
    n.transcript.setText(text.trimEnd(), false);
    n.transcript.moveCaretToEnd();
}

void CollabChatDock::setComposer(bool enabled, const std::string& reason)
{
    auto& n = *impl_;
    n.enabled = enabled;
    n.input.setReadOnly(! enabled);
    n.reason = reason;
    n.status.setText(juce::String::fromUTF8(reason.c_str()), juce::dontSendNotification);
    n.input.onTextChange();
    resized();
}

void CollabChatDock::setSubtitle(const std::string& text) { impl_->subtitle.setText(juce::String::fromUTF8(text.c_str()), juce::dontSendNotification); }

void CollabChatDock::focusComposer() { impl_->input.grabKeyboardFocus(); }

void CollabChatDock::send()
{
    auto& n = *impl_;
    const auto text = n.input.getText().trim();
    if (! n.enabled || text.isEmpty() || text.length() > maxChars) return;
    const auto why = onSend ? onSend(text.toStdString()) : std::string("Not in a session.");
    n.status.setText(juce::String::fromUTF8((why.empty() ? n.reason : why).c_str()), juce::dontSendNotification);
    if (why.empty()) n.input.clear();
    n.input.onTextChange();
    resized();
}

bool CollabChatDock::writeSnapshot(const juce::File&) { return false; }
bool CollabChatDock::verifyNativeInput() { return false; }
} // namespace sonora::app
