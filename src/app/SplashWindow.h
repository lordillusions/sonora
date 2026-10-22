#pragma once
// Launch splash (UI-42): a borderless, always-on-top panel drawn by gfx::brand::drawSplash. It shows while the
// controller, the window and the first document come up, mirrors UiContext's busy task (e.g. "Starting plug-ins 3 of
// 12"), then fades out once the document is ready.

#include "gfx/Brand.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <string>

namespace sonora::app
{

class SplashWindow : public juce::Component, private juce::Timer
{
public:
    explicit SplashWindow(std::string version);
    ~SplashWindow() override;

    // Startup steps that block the message thread call this: it paints synchronously.
    void setStatus(std::string text, float progress = -1.0f);

    // Polled while the message loop runs. Fills the status and returns true once the app is ready; the splash then
    // stays at least minimumMs since it appeared, fades out and calls onFinished (which may delete it).
    std::function<bool(std::string& status, float& progress)> poll;
    std::function<void()> onFinished;

    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent&) override { startFade(); } // a click dismisses it once ready

private:
    void timerCallback() override;
    void startFade();

    gfx::brand::SplashState state_;
    double shownAt_ = 0, fadeStart_ = 0;
    bool ready_ = false;
    static constexpr double minimumMs = 1200, fadeMs = 260;
};

} // namespace sonora::app
