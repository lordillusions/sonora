#pragma once
// App prompts (UI-44): a borderless panel drawn by gfx::brand::drawPrompt (the splash's look: app icon, Sonora type,
// white primary button) instead of the plain system alert. Used for "Save changes?" before closing a set, quitting and
// restarting for an update.
//
// Modal over the app; Return presses the default button, Escape the cancel button, Cmd+<key> a button's shortcut
// (Cmd+D for Don't Save, like macOS). The window deletes itself after calling `done` with the button index.

#include "gfx/Brand.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace sonora::app
{

struct PromptOptions
{
    gfx::brand::PromptState state;
    int defaultButton = 0;           // Return
    int cancelButton = -1;           // Escape (and closing the prompt any other way)
    std::vector<juce::juce_wchar> shortcuts; // Cmd+<char> per button, 0 for none
};

class PromptWindow : public juce::Component
{
public:
    // Shows the prompt centred near the top of the active window. Only one prompt shows at a time: while one is open
    // this brings it to the front and returns false (the caller's `done` is not called).
    static bool show(PromptOptions options, std::function<void(int button)> done);
    static bool isShowing();
    // Sends a key to the open prompt as if typed (SONORA_QUIT_AUTOTEST); false when none is open.
    static bool pressKey(const juce::KeyPress& key);

    ~PromptWindow() override;
    void paint(juce::Graphics& g) override;
    void mouseMove(const juce::MouseEvent& e) override;
    void mouseExit(const juce::MouseEvent& e) override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    bool keyPressed(const juce::KeyPress& key) override;
    void inputAttemptWhenModal() override;

private:
    PromptWindow(PromptOptions options, std::function<void(int)> done);
    int buttonAt(juce::Point<float> p) const;
    void finish(int button);

    PromptOptions options_;
    std::function<void(int)> done_;
    bool finished_ = false;
};

} // namespace sonora::app
