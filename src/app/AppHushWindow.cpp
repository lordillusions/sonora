// PLG-14g: the built-in Sonora Hush in its own window, at the VST3 editor's size (the device chain shows it scaled to the
// chain's height). Follows its instance by key like the floating Prism (UI-27) and closes when the device goes away.
#include "app/AppController.h"
#include "devices/native/HushSession.h"
#include "gfx/SkiaCanvasComponent.h"
#include "ui/views/HushFace.h"

namespace sonora::app
{
namespace
{
class HushWindow final : public juce::DocumentWindow
{
public:
    HushWindow(ui::UiContext& ui, const ui::UiContext::WavetableKey& key, std::function<void(HushWindow*)> onClose,
               std::function<bool(const juce::KeyPress&)> forwardKey)
        : DocumentWindow("Sonora Hush", juce::Colours::darkgrey, juce::DocumentWindow::closeButton),
          view_(ui, key), onClose_(std::move(onClose)), forwardKey_(std::move(forwardKey))
    {
        setUsingNativeTitleBar(true);
        view_.onTitleChanged = [this](const std::string& title) { setName(juce::String::fromUTF8(title.c_str())); };
        view_.menuHost = [this]() -> juce::Component* { return canvas_; };
        view_.onScaleChosen = [this](float scale) {
            scale = fitScreen(scale);
            if (canvas_ != nullptr)
                canvas_->setSize(juce::roundToInt(ui::HushFace::kWidth * scale), juce::roundToInt(ui::HushFace::kHeight * scale));
        };
        view_.onGone = [this] {
            juce::MessageManager::callAsync([safe = juce::Component::SafePointer<HushWindow>(this)] {
                if (safe != nullptr) safe->onClose_(safe.getComponent());
            });
        };
        view_.sync();
        canvas_ = new gfx::SkiaCanvasComponent(view_);
        float scale = 1050.0f / ui::HushFace::kWidth; // the VST3's default size
        if (const auto site = ui::HushWindowView::find(ui, key); site && site->processor != nullptr)
            scale = std::clamp(site->processor->session().editorScale, 0.5f, 3.0f);
        scale = fitScreen(scale);
        canvas_->setSize(juce::roundToInt(ui::HushFace::kWidth * scale), juce::roundToInt(ui::HushFace::kHeight * scale));
        setContentOwned(canvas_, true);
        setResizable(true, false);
        setResizeLimits(juce::roundToInt(ui::HushFace::kWidth * 0.6f), juce::roundToInt(ui::HushFace::kHeight * 0.6f),
                        juce::roundToInt(ui::HushFace::kWidth * 2.5f), juce::roundToInt(ui::HushFace::kHeight * 2.5f));
        getConstrainer()->setFixedAspectRatio(ui::HushFace::kWidth / ui::HushFace::kHeight);
        centreWithSize(getWidth(), getHeight());
        setVisible(true);
        canvas_->grabKeyboardFocus();
    }
    ~HushWindow() override { clearContentComponent(); } // the canvas references view_

    void closeButtonPressed() override { onClose_(this); }
    // Keys the face did not use bubble up here: transport and view shortcuts still reach the main window.
    bool keyPressed(const juce::KeyPress& key) override { return forwardKey_ && forwardKey_(key); }
    void resized() override
    {
        DocumentWindow::resized();
        if (auto* face = view_.face(); face != nullptr && face->width() > 0)
            face->session().editorScale = face->width() / ui::HushFace::kWidth;
    }
    const ui::UiContext::WavetableKey& key() const { return view_.key(); }
    void setKey(const ui::UiContext::WavetableKey& key) { view_.setKey(key); }
    void sync()
    {
        view_.sync();
        if (canvas_) canvas_->requestFrame();
    }

private:
    // A scale whose window (plus the title bar) fits the screen's free area: a bigger one gets clamped by the system.
    static float fitScreen(float scale)
    {
        if (const auto* display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
        {
            const auto area = display->userBounds;
            scale = std::min({scale, ((float) area.getWidth() - 40.0f) / ui::HushFace::kWidth,
                              ((float) area.getHeight() - 80.0f) / ui::HushFace::kHeight});
        }
        return std::max(0.5f, scale);
    }

    ui::HushWindowView view_;
    gfx::SkiaCanvasComponent* canvas_ = nullptr; // owned by the window (content component)
    std::function<void(HushWindow*)> onClose_;
    std::function<bool(const juce::KeyPress&)> forwardKey_;
};
} // namespace

void AppController::openHushWindow(const ui::UiContext::WavetableKey& key)
{
    for (auto* window : hushWindows_)
        if (auto* w = static_cast<HushWindow*>(window); w->key() == key && w->isVisible())
        {
            w->toFront(true);
            return;
        }
    if (! song_ || ! ui_.song || ! ui::HushWindowView::find(ui_, key)) return;
    hushWindows_.add(new HushWindow(ui_, key, [this](HushWindow* w) { hushWindows_.removeObject(w); },
                                    [this](const juce::KeyPress& k) { return forwardKeyToMainWindow && forwardKeyToMainWindow(k); }));
}

void AppController::closeHushWindows()
{
    hushWindows_.clear();
    closeKickstractTimelines(); // same lifetime (PLG-17e)
}

void AppController::syncHushWindows()
{
    for (auto* window : hushWindows_)
        if (window->isVisible()) static_cast<HushWindow*>(window)->sync();
    syncKickstractTimelines();
}

void AppController::renameHushWindows(model::TrackId track, int oldId, int newId)
{
    renameKickstractTimelines(track, oldId, newId);
    for (auto* window : hushWindows_)
        if (auto* w = static_cast<HushWindow*>(window); w->key().trackId == track && w->key().deviceId == oldId)
        {
            auto key = w->key();
            key.deviceId = newId;
            w->setKey(key);
        }
}
} // namespace sonora::app
