#pragma once
// The VST3 window of a ProFace device (PLG-23: Stutter, Sweep, Phase Align): the face's full layout on a Skia canvas,
// letterboxed so it keeps its aspect ratio at any window size. Files dropped on the window (Finder, the host's browser)
// go to onFilesDropped when it is set (Sweep loads a sample).

#include "gfx/SkiaCanvasComponent.h"
#include "ui/RootWidget.h"
#include "ui/UiContext.h"
#include "ui/views/ProFace.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace sonora::plugin
{
class ProFaceView final : public ui::RootWidget
{
public:
    explicit ProFaceView(ui::UiContext& context) : RootWidget(context) {}
    template <typename Face, typename... Args>
    Face* makeFace(Args&&... args)
    {
        auto* face = add<Face>(std::forward<Args>(args)...);
        face_ = face;
#ifdef JucePlugin_VersionString
        face_->version = JucePlugin_VersionString;
#endif
        return face;
    }
    ui::ProFace* face() const { return face_; }
    void layout() override
    {
        if (face_ == nullptr) return;
        const auto r = localBounds();
        const auto d = face_->fullSize();
        const float s = std::max(0.01f, std::min(r.w / d.w, r.h / d.h));
        const float w = d.w * s, h = d.h * s;
        face_->setBounds({std::floor((r.w - w) * 0.5f), std::floor((r.h - h) * 0.5f), w, h});
    }
    void paint(SkCanvas& c, const ui::Theme&) override { ui::paint::fill(c, localBounds(), ui::pro::kBackground); }

private:
    ui::ProFace* face_ = nullptr;
};

class ProFaceEditor : public juce::AudioProcessorEditor, public juce::FileDragAndDropTarget
{
public:
    static constexpr float kDefaultScale = 0.85f;

    explicit ProFaceEditor(juce::AudioProcessor& edited) : AudioProcessorEditor(edited), view_(connect()) {}
    ~ProFaceEditor() override
    {
        canvas_.reset(); // it references view_
        view_.removeAllChildren();
    }
    ProFaceView& view() { return view_; }
    std::function<void(const std::vector<std::string>&)> onFilesDropped;

    void resized() override
    {
        if (canvas_ != nullptr) canvas_->setBounds(getLocalBounds());
    }
    bool isInterestedInFileDrag(const juce::StringArray&) override { return (bool) onFilesDropped; }
    void filesDropped(const juce::StringArray& files, int, int) override
    {
        std::vector<std::string> paths;
        for (const auto& f : files) paths.push_back(f.toStdString());
        if (onFilesDropped) onFilesDropped(paths);
        if (canvas_) canvas_->requestFrame();
    }

protected:
    // Call at the end of the subclass constructor, once the face exists.
    void open()
    {
        canvas_ = std::make_unique<gfx::SkiaCanvasComponent>(view_);
        addAndMakeVisible(*canvas_);
        const auto d = view_.face()->fullSize();
        setResizable(true, true);
        if (auto* constrainer = getConstrainer())
        {
            constrainer->setFixedAspectRatio(d.w / d.h);
            constrainer->setSizeLimits(juce::roundToInt(d.w * 0.5f), juce::roundToInt(d.h * 0.5f), juce::roundToInt(d.w * 2.0f),
                                       juce::roundToInt(d.h * 2.0f));
        }
        setSize(juce::roundToInt(d.w * kDefaultScale), juce::roundToInt(d.h * kDefaultScale));
    }

private:
    ui::UiContext& connect()
    {
        // Runs before view_ is built: the face reads the context while it builds.
        context_.theme.useBuiltin(false);
        context_.requestRepaint = [this] { if (canvas_) canvas_->requestFrame(); };
        return context_;
    }

    ui::UiContext context_;
    ProFaceView view_;
    std::unique_ptr<gfx::SkiaCanvasComponent> canvas_;
};
} // namespace sonora::plugin
