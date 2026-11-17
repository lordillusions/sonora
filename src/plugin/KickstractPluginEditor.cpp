#include "plugin/KickstractPluginEditor.h"

#include "gfx/SkiaCanvasComponent.h"
#include "plugin/KickstractPlugin.h"
#include "ui/Painter.h"
#include "ui/views/KickstractFace.h"
#include "ui/views/KickstractTimeline.h"

#include <algorithm>
#include <cmath>

namespace sonora::plugin
{
KickstractView::KickstractView(ui::UiContext& context, KickstractPlugin& plugin) : RootWidget(context), plugin_(plugin)
{
    face_ = add<ui::KickstractFace>(plugin_.device());
#ifdef JucePlugin_VersionString
    face_->version = JucePlugin_VersionString;
#endif
}

void KickstractView::layout()
{
    const auto r = localBounds();
    const auto d = face_->fullSize();
    const float s = std::max(0.01f, std::min(r.w / d.w, r.h / d.h));
    const float w = d.w * s, h = d.h * s;
    face_->setBounds({std::floor((r.w - w) * 0.5f), std::floor((r.h - h) * 0.5f), w, h});
}

void KickstractView::paint(SkCanvas& c, const ui::Theme&)
{
    ui::paint::fill(c, localBounds(), ui::pro::kBackground);
    if (! plugin_.buildLog.empty()) // a load note (a state that could not be restored) under the face
        ui::paint::text(c, plugin_.buildLog.back(), {8, height() - 16, width() - 16, 14}, ui::pro::kDim, 10.5f, ui::Align::Right);
}

namespace
{
// Root of the timeline window: the root never takes input itself, so the timeline is its child.
class TimelineRoot final : public ui::RootWidget
{
public:
    TimelineRoot(ui::UiContext& context, devices::Kickstract& device) : RootWidget(context)
    {
        timeline = add<ui::KickstractTimeline>(&device);
    }
    void layout() override { timeline->setBounds(localBounds().reduced(8)); }
    void paint(SkCanvas& c, const ui::Theme& t) override { ui::paint::fill(c, localBounds(), t.color("SurfaceBackground")); }
    ui::KickstractTimeline* timeline = nullptr;
};
} // namespace

class KickstractPluginEditor::TimelineWindow final : public juce::DocumentWindow
{
public:
    TimelineWindow(KickstractPluginEditor& editor, devices::Kickstract& device)
        : DocumentWindow("Sonora Kickstract Timeline", juce::Colours::darkgrey, juce::DocumentWindow::closeButton | juce::DocumentWindow::minimiseButton),
          editor_(editor), root_(connect(), device)
    {
        setUsingNativeTitleBar(true);
        canvas_ = new gfx::SkiaCanvasComponent(root_);
        canvas_->setSize(juce::roundToInt(ui::KickstractTimeline::kPreferredWidth), juce::roundToInt(ui::KickstractTimeline::kPreferredHeight));
        setContentOwned(canvas_, true);
        setResizable(true, false);
        setResizeLimits(560, 260, 4000, 2400);
        centreWithSize(getWidth(), getHeight());
        setAlwaysOnTop(true); // hosts keep plug-in windows floating; the timeline stays with them
        setVisible(true);
    }
    ~TimelineWindow() override { clearContentComponent(); } // the canvas references root_
    void closeButtonPressed() override
    {
        juce::MessageManager::callAsync([safe = juce::Component::SafePointer<KickstractPluginEditor>(&editor_)] {
            if (safe != nullptr) safe->closeTimeline();
        });
    }
    ui::KickstractTimeline* timeline() const { return root_.timeline; }

private:
    ui::UiContext& connect()
    {
        context_.theme.useBuiltin(false);
        context_.requestRepaint = [this] { if (canvas_) canvas_->requestFrame(); };
        return context_;
    }
    KickstractPluginEditor& editor_;
    ui::UiContext context_;
    TimelineRoot root_;
    gfx::SkiaCanvasComponent* canvas_ = nullptr; // owned by the window
};

void KickstractPluginEditor::openTimeline()
{
    if (timeline_ != nullptr)
    {
        timeline_->toFront(true);
        return;
    }
    timeline_ = std::make_unique<TimelineWindow>(*this, plugin_.device());
}

void KickstractPluginEditor::closeTimeline() { timeline_.reset(); }

ui::KickstractTimeline* KickstractPluginEditor::timeline() const { return timeline_ ? timeline_->timeline() : nullptr; }

KickstractPluginEditor::KickstractPluginEditor(KickstractPlugin& plugin)
    : AudioProcessorEditor(plugin), plugin_(plugin), view_(connect(), plugin)
{
    view_.face().onOpenTimeline = [this] { openTimeline(); };
    canvas_ = std::make_unique<gfx::SkiaCanvasComponent>(view_);
    addAndMakeVisible(*canvas_);
    setResizable(true, true);
    setResizeLimits(juce::roundToInt(KickstractView::kWidth * 0.6f), juce::roundToInt(KickstractView::kHeight * 0.6f),
                    juce::roundToInt(KickstractView::kWidth * 2.5f), juce::roundToInt(KickstractView::kHeight * 2.5f));
    if (auto* constrainer = getConstrainer()) constrainer->setFixedAspectRatio(KickstractView::kWidth / KickstractView::kHeight);
    setSize(juce::roundToInt(KickstractView::kWidth), juce::roundToInt(KickstractView::kHeight));
}

KickstractPluginEditor::~KickstractPluginEditor()
{
    timeline_.reset(); // before the device's editor goes: the window reads the device
    canvas_.reset(); // it references view_
    view_.removeAllChildren();
}

void KickstractPluginEditor::resized()
{
    if (canvas_ != nullptr) canvas_->setBounds(getLocalBounds());
}

ui::UiContext& KickstractPluginEditor::connect()
{
    // Runs before view_ is built: the face reads the context while it builds.
    context_.theme.useBuiltin(false);
    context_.requestRepaint = [this] { if (canvas_) canvas_->requestFrame(); };
    return context_;
}
} // namespace sonora::plugin
