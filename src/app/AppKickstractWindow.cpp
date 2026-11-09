// PLG-17e: a built-in Sonora Kickstract's timeline (Input / Kick / Removed over time, Freeze) in its own window. Follows
// its instance by key through graph rebuilds like the Hush window (AppHushWindow.cpp) and closes when the device goes.
#include "app/AppController.h"
#include "gfx/SkiaCanvasComponent.h"
#include "ui/views/KickstractTimeline.h"

namespace sonora::app
{
namespace
{
class KickstractTimelineWindow final : public juce::DocumentWindow
{
public:
    KickstractTimelineWindow(ui::UiContext& ui, const ui::UiContext::WavetableKey& key,
                             std::function<void(KickstractTimelineWindow*)> onClose,
                             std::function<bool(const juce::KeyPress&)> forwardKey)
        : DocumentWindow("Sonora Kickstract Timeline", juce::Colours::darkgrey, juce::DocumentWindow::allButtons),
          view_(ui, key), onClose_(std::move(onClose)), forwardKey_(std::move(forwardKey))
    {
        setUsingNativeTitleBar(true);
        view_.onTitleChanged = [this](const std::string& title) { setName(juce::String::fromUTF8(title.c_str())); };
        view_.onGone = [this] {
            juce::MessageManager::callAsync([safe = juce::Component::SafePointer<KickstractTimelineWindow>(this)] {
                if (safe != nullptr) safe->onClose_(safe.getComponent());
            });
        };
        view_.sync();
        canvas_ = new gfx::SkiaCanvasComponent(view_);
        canvas_->setSize(juce::roundToInt(ui::KickstractTimeline::kPreferredWidth), juce::roundToInt(ui::KickstractTimeline::kPreferredHeight));
        setContentOwned(canvas_, true);
        setResizable(true, false);
        setResizeLimits(560, 260, 4000, 2400);
        centreWithSize(getWidth(), getHeight());
        setVisible(true);
        canvas_->grabKeyboardFocus();
    }
    ~KickstractTimelineWindow() override { clearContentComponent(); } // the canvas references view_

    void closeButtonPressed() override { onClose_(this); }
    bool keyPressed(const juce::KeyPress& key) override { return forwardKey_ && forwardKey_(key); }
    const ui::UiContext::WavetableKey& key() const { return view_.key(); }
    void setKey(const ui::UiContext::WavetableKey& key) { view_.setKey(key); }
    void sync()
    {
        view_.sync();
        if (canvas_) canvas_->requestFrame();
    }

private:
    ui::KickstractTimelineWindowView view_;
    gfx::SkiaCanvasComponent* canvas_ = nullptr; // owned by the window (content component)
    std::function<void(KickstractTimelineWindow*)> onClose_;
    std::function<bool(const juce::KeyPress&)> forwardKey_;
};
} // namespace

void AppController::openKickstractTimeline(const ui::UiContext::WavetableKey& key)
{
    for (auto* window : kickstractTimelines_)
        if (auto* w = static_cast<KickstractTimelineWindow*>(window); w->key() == key && w->isVisible())
        {
            w->toFront(true);
            return;
        }
    if (! song_ || ! ui_.song || ! ui::KickstractTimelineWindowView::find(ui_, key)) return;
    kickstractTimelines_.add(new KickstractTimelineWindow(
        ui_, key, [this](KickstractTimelineWindow* w) { kickstractTimelines_.removeObject(w); },
        [this](const juce::KeyPress& k) { return forwardKeyToMainWindow && forwardKeyToMainWindow(k); }));
}

void AppController::closeKickstractTimelines()
{
    kickstractTimelines_.clear();
}

void AppController::syncKickstractTimelines()
{
    for (auto* window : kickstractTimelines_)
        if (window->isVisible()) static_cast<KickstractTimelineWindow*>(window)->sync();
}

void AppController::renameKickstractTimelines(model::TrackId track, int oldId, int newId)
{
    for (auto* window : kickstractTimelines_)
        if (auto* w = static_cast<KickstractTimelineWindow*>(window); w->key().trackId == track && w->key().deviceId == oldId)
        {
            auto key = w->key();
            key.deviceId = newId;
            w->setKey(key);
        }
}
} // namespace sonora::app
