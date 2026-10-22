#include "app/SplashWindow.h"

#include "include/core/SkCanvas.h"

#include <cmath>

namespace sonora::app
{

SplashWindow::SplashWindow(std::string version)
{
    state_.version = std::move(version);
    state_.status = "Starting...";
    setOpaque(false);
    setSize((int) gfx::brand::splashWidth, (int) gfx::brand::splashHeight);
    if (const auto* display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
        setCentrePosition(display->userBounds.getCentre().toInt().translated(0, -40));
    addToDesktop(juce::ComponentPeer::windowIsTemporary | juce::ComponentPeer::windowHasDropShadow);
    setAlwaysOnTop(true);
    setVisible(true);
    toFront(false);
    shownAt_ = juce::Time::getMillisecondCounterHiRes();
    if (auto* peer = getPeer())
        peer->performAnyPendingRepaintsNow();
    startTimerHz(30);
}

SplashWindow::~SplashWindow()
{
    stopTimer();
}

void SplashWindow::setStatus(std::string text, float progress)
{
    state_.status = std::move(text);
    state_.progress = progress;
    repaint();
    if (auto* peer = getPeer())
        peer->performAnyPendingRepaintsNow();
}

void SplashWindow::paint(juce::Graphics& g)
{
    float scale = 2.0f;
    if (const auto* display = juce::Desktop::getInstance().getDisplays().getDisplayForRect(getScreenBounds()))
        scale = (float) display->scale;
    state_.timeSeconds = juce::Time::getMillisecondCounterHiRes() / 1000.0;
    const int pw = (int) std::lround((float) getWidth() * scale), ph = (int) std::lround((float) getHeight() * scale);
    const auto image = gfx::brand::renderImage(pw, ph, scale, [this](SkCanvas& c) {
        gfx::brand::drawSplash(c, (float) getWidth(), (float) getHeight(), state_);
    });
    g.drawImage(image, getLocalBounds().toFloat());
}

void SplashWindow::startFade()
{
    if (! ready_ || fadeStart_ > 0) return;
    fadeStart_ = juce::Time::getMillisecondCounterHiRes();
}

void SplashWindow::timerCallback()
{
    const double now = juce::Time::getMillisecondCounterHiRes();
    if (fadeStart_ > 0)
    {
        const double t = (now - fadeStart_) / fadeMs;
        if (t >= 1.0)
        {
            stopTimer();
            setVisible(false);
            if (onFinished) onFinished(); // may delete this
            return;
        }
        setAlpha((float) (1.0 - t * t));
        return;
    }
    if (poll)
    {
        std::string status = state_.status;
        float progress = state_.progress;
        ready_ = poll(status, progress);
        state_.status = std::move(status);
        state_.progress = progress;
    }
    repaint();
    if (ready_ && now - shownAt_ >= minimumMs)
        startFade();
}

} // namespace sonora::app
