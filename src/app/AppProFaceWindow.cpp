// PLG-23: Sonora Stutter, Sweep and Phase Align in their own window (the face's full layout), like the floating Hush
// (PLG-14g): the window follows its instance by key and closes when the device goes away.
#include "app/AppController.h"
#include "gfx/SkiaCanvasComponent.h"
#include "ui/views/ProFaceWindow.h"

namespace sonora::app
{
namespace
{
class ProFaceWindow final : public juce::DocumentWindow
{
public:
    ProFaceWindow(ui::UiContext& ui, const ui::UiContext::WavetableKey& key, std::function<void(ProFaceWindow*)> onClose,
                  std::function<bool(const juce::KeyPress&)> forwardKey)
        : DocumentWindow("Sonora", juce::Colour(0xFF111317), juce::DocumentWindow::closeButton),
          view_(ui, key), onClose_(std::move(onClose)), forwardKey_(std::move(forwardKey))
    {
        setUsingNativeTitleBar(true);
        view_.onTitleChanged = [this](const std::string& title) { setName(juce::String::fromUTF8(title.c_str())); };
        view_.onGone = [this] {
            juce::MessageManager::callAsync([safe = juce::Component::SafePointer<ProFaceWindow>(this)] {
                if (safe != nullptr) safe->onClose_(safe.getComponent());
            });
        };
        view_.sync();
        const auto size = view_.designSize();
        canvas_ = new gfx::SkiaCanvasComponent(view_);
        const float scale = fitScreen(size, 0.9f);
        canvas_->setSize(juce::roundToInt(size.w * scale), juce::roundToInt(size.h * scale));
        setContentOwned(canvas_, true);
        setResizable(true, false);
        setResizeLimits(juce::roundToInt(size.w * 0.5f), juce::roundToInt(size.h * 0.5f), juce::roundToInt(size.w * 2.5f),
                        juce::roundToInt(size.h * 2.5f));
        getConstrainer()->setFixedAspectRatio(size.w / size.h);
        centreWithSize(getWidth(), getHeight());
        setVisible(true);
        canvas_->grabKeyboardFocus();
    }
    ~ProFaceWindow() override { clearContentComponent(); } // the canvas references view_

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
    // A scale whose window (plus the title bar) fits the screen's free area.
    static float fitScreen(ui::ProFace::Size size, float scale)
    {
        if (const auto* display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
        {
            const auto area = display->userBounds;
            scale = std::min({scale, ((float) area.getWidth() - 40.0f) / size.w, ((float) area.getHeight() - 80.0f) / size.h});
        }
        return std::max(0.4f, scale);
    }

    ui::ProFaceWindowView view_;
    gfx::SkiaCanvasComponent* canvas_ = nullptr; // owned by the window (content component)
    std::function<void(ProFaceWindow*)> onClose_;
    std::function<bool(const juce::KeyPress&)> forwardKey_;
};
} // namespace

void AppController::openProFaceWindow(const ui::UiContext::WavetableKey& key)
{
    for (auto* window : proFaceWindows_)
        if (auto* w = static_cast<ProFaceWindow*>(window); w->key() == key && w->isVisible())
        {
            w->toFront(true);
            return;
        }
    if (! song_ || ! ui_.song || ! ui::ProFaceWindowView::find(ui_, key)) return;
    proFaceWindows_.add(new ProFaceWindow(ui_, key, [this](ProFaceWindow* w) { proFaceWindows_.removeObject(w); },
                                          [this](const juce::KeyPress& k) { return forwardKeyToMainWindow && forwardKeyToMainWindow(k); }));
}

void AppController::closeProFaceWindows() { proFaceWindows_.clear(); }

void AppController::syncProFaceWindows()
{
    for (auto* window : proFaceWindows_)
        if (window->isVisible()) static_cast<ProFaceWindow*>(window)->sync();
}

void AppController::renameProFaceWindows(model::TrackId track, int oldId, int newId)
{
    for (auto* window : proFaceWindows_)
        if (auto* w = static_cast<ProFaceWindow*>(window); w->key().trackId == track && w->key().deviceId == oldId)
        {
            auto key = w->key();
            key.deviceId = newId;
            w->setKey(key);
        }
}
} // namespace sonora::app
