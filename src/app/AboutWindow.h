#pragma once
// About Sonora (COM-8): a borderless panel drawn by gfx::brand::drawAbout (the splash's look) with the version, licence,
// the author's credit and the supporters from resources/supporters.txt. Not modal; one at a time; Escape or Close
// dismisses it, the background drags it, the other buttons open the website, the source and the Ko-fi page.

#include "gfx/Brand.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace sonora::app
{

class AboutWindow : public juce::Component
{
public:
    static void show(); // brings an open one to the front
    static gfx::brand::AboutState currentState(); // reads the supporters file (tests, brand_render)
    ~AboutWindow() override;

    void paint(juce::Graphics& g) override;
    void mouseMove(const juce::MouseEvent& e) override;
    void mouseExit(const juce::MouseEvent& e) override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    bool keyPressed(const juce::KeyPress& key) override;

private:
    AboutWindow();
    int buttonAt(juce::Point<float> p) const;
    void press(int button);
    void close();

    gfx::brand::AboutState state_;
    juce::ComponentDragger dragger_;
    bool dragging_ = false;
};

} // namespace sonora::app
