#include "app/PromptWindow.h"

#include "include/core/SkCanvas.h"

#include <cmath>

namespace sonora::app
{

namespace
{
PromptWindow* current = nullptr;
}

bool PromptWindow::isShowing() { return current != nullptr; }
bool PromptWindow::pressKey(const juce::KeyPress& key) { return current != nullptr && current->keyPressed(key); }

bool PromptWindow::show(PromptOptions options, std::function<void(int)> done)
{
    if (current != nullptr)
    {
        current->toFront(true);
        return false;
    }
    current = new PromptWindow(std::move(options), std::move(done)); // owned by itself until finish()
    return true;
}

PromptWindow::PromptWindow(PromptOptions options, std::function<void(int)> done)
    : options_(std::move(options)), done_(std::move(done))
{
    options_.state.focused = options_.defaultButton;
    setOpaque(false);
    setWantsKeyboardFocus(true);
    setSize((int) gfx::brand::promptWidth, (int) std::ceil(gfx::brand::promptHeight(options_.state)));

    // Near the top of the window the prompt is about, like a sheet; else centred on the main display.
    juce::Rectangle<int> over;
    if (auto* active = juce::TopLevelWindow::getActiveTopLevelWindow())
        over = active->getScreenBounds();
    else if (const auto* display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
        over = display->userBounds.toNearestInt();
    if (! over.isEmpty())
        setTopLeftPosition(over.getCentreX() - getWidth() / 2, over.getY() + std::min(140, std::max(0, over.getHeight() / 2 - getHeight() / 2)));

    addToDesktop(juce::ComponentPeer::windowHasDropShadow);
    setVisible(true);
    enterModalState(true, nullptr, false);
    toFront(true);
    grabKeyboardFocus();
}

PromptWindow::~PromptWindow()
{
    if (current == this) current = nullptr;
}

void PromptWindow::paint(juce::Graphics& g)
{
    float scale = 2.0f;
    if (const auto* display = juce::Desktop::getInstance().getDisplays().getDisplayForRect(getScreenBounds()))
        scale = (float) display->scale;
    const int pw = (int) std::lround((float) getWidth() * scale), ph = (int) std::lround((float) getHeight() * scale);
    const auto image = gfx::brand::renderImage(pw, ph, scale, [this](SkCanvas& c) {
        gfx::brand::drawPrompt(c, (float) getWidth(), (float) getHeight(), options_.state);
    });
    g.drawImage(image, getLocalBounds().toFloat());
}

int PromptWindow::buttonAt(juce::Point<float> p) const
{
    const auto rects = gfx::brand::promptButtonRects(options_.state, (float) getWidth(), (float) getHeight());
    for (size_t i = 0; i < rects.size(); ++i)
        if (rects[i].contains(p.x, p.y)) return (int) i;
    return -1;
}

void PromptWindow::mouseMove(const juce::MouseEvent& e)
{
    const int hot = buttonAt(e.position);
    if (hot != options_.state.hovered) { options_.state.hovered = hot; repaint(); }
}

void PromptWindow::mouseExit(const juce::MouseEvent&)
{
    if (options_.state.hovered >= 0) { options_.state.hovered = -1; repaint(); }
}

void PromptWindow::mouseDown(const juce::MouseEvent& e)
{
    options_.state.pressed = options_.state.hovered = buttonAt(e.position);
    repaint();
}

void PromptWindow::mouseDrag(const juce::MouseEvent& e) { mouseMove(e); }

void PromptWindow::mouseUp(const juce::MouseEvent& e)
{
    const int pressed = options_.state.pressed;
    options_.state.pressed = -1;
    repaint();
    if (pressed >= 0 && buttonAt(e.position) == pressed) finish(pressed);
}

bool PromptWindow::keyPressed(const juce::KeyPress& key)
{
    if (key == juce::KeyPress::returnKey) { finish(options_.defaultButton); return true; }
    if (key == juce::KeyPress::escapeKey) { if (options_.cancelButton >= 0) finish(options_.cancelButton); return true; }
    if (key.getModifiers().isCommandDown())
        for (size_t i = 0; i < options_.shortcuts.size(); ++i)
            if (options_.shortcuts[i] != 0
                && juce::CharacterFunctions::toLowerCase(key.getTextCharacter() != 0 ? key.getTextCharacter() : (juce::juce_wchar) key.getKeyCode())
                       == juce::CharacterFunctions::toLowerCase(options_.shortcuts[i]))
            {
                finish((int) i);
                return true;
            }
    return true; // modal: nothing else reaches the app
}

void PromptWindow::inputAttemptWhenModal()
{
    toFront(true); // a click on the main window brings the prompt back
}

void PromptWindow::finish(int button)
{
    if (finished_ || button < 0) return;
    finished_ = true;
    exitModalState(button);
    setVisible(false);
    if (current == this) current = nullptr;
    // Delete after this event returns; the callback may quit the app or show another prompt.
    juce::MessageManager::callAsync([this, callback = std::move(done_), button] {
        delete this;
        if (callback) callback(button);
    });
}

} // namespace sonora::app
