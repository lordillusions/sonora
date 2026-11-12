#include "ui/views/ProFaceWindow.h"

#include "devices/RackProcessor.h"
#include "devices/native/PhaseAlign.h"
#include "devices/native/Stutter.h"
#include "devices/native/Sweep.h"
#include "engine/EngineGraph.h"
#include "ui/DeviceMenu.h"
#include "ui/Painter.h"
#include "ui/views/PhaseAlignFace.h"
#include "ui/views/StutterFace.h"
#include "ui/views/SweepFace.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <cmath>
#include <memory>

namespace sonora::ui
{
ProFace* addProFace(Widget& parent, devices::DeviceProcessor* processor)
{
    if (auto* stutter = dynamic_cast<devices::Stutter*>(processor)) return parent.add<StutterFace>(*stutter);
    if (auto* sweep = dynamic_cast<devices::Sweep*>(processor)) return parent.add<SweepFace>(*sweep);
    if (auto* phase = dynamic_cast<devices::PhaseAlign*>(processor)) return parent.add<PhaseAlignFace>(*phase);
    return nullptr;
}

void loadSweepSample(UiContext& x, int trackIndex, int deviceIndex, const std::string& path)
{
    if (! x.editDevice || path.empty()) return;
    x.editDevice(trackIndex, deviceIndex, [path](model::Device& d) {
        auto content = std::make_shared<model::NativeContent>();
        content->format = "SonoraSweep/Sample";
        content->data.assign(path.begin(), path.end());
        d.nativeContent = std::move(content);
        d.nativeContentEdited = true;
        bool found = false;
        for (auto& p : d.parameters)
            if (p.path == "Source") { p.value = devices::Sweep::Sample; found = true; }
        if (! found)
        {
            model::Parameter p;
            p.path = "Source";
            p.value = devices::Sweep::Sample;
            d.parameters.push_back(std::move(p));
        }
    }, "Load Sample");
}

void wireProFace(ProFace& face, std::function<UiContext*()> context, int trackIndex, int deviceIndex)
{
    auto* processor = &face.device();
    face.mappingTarget = [processor, trackIndex, deviceIndex](int parameter) -> std::optional<midi::ControlTarget> {
        if (parameter < 0 || (size_t) parameter >= processor->parameters().size()) return std::nullopt;
        return midi::ControlTarget{midi::ControlTarget::Kind::DeviceParameter, trackIndex, deviceIndex, 0,
                                   processor->parameters()[(size_t) parameter].id};
    };
    if (auto* sweep = dynamic_cast<SweepFace*>(&face))
    {
        sweep->onSampleDropped = [context, trackIndex, deviceIndex](const std::string& path) {
            if (auto* x = context()) loadSweepSample(*x, trackIndex, deviceIndex, path);
        };
        sweep->onChooseSample = [context, trackIndex, deviceIndex] {
            auto chooser = std::make_shared<juce::FileChooser>("Load a sample into Sonora Sweep", juce::File(),
                                                               "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.ogg");
            chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                                 [context, chooser, trackIndex, deviceIndex](const juce::FileChooser& fc) {
                const auto file = fc.getResult();
                if (file == juce::File()) return;
                if (auto* x = context()) loadSweepSample(*x, trackIndex, deviceIndex, file.getFullPathName().toStdString());
            });
        };
    }
    if (auto* phase = dynamic_cast<PhaseAlignFace*>(&face))
    {
        phase->sidechainName = [context, trackIndex, deviceIndex]() -> std::string {
            auto* x = context();
            if (x == nullptr || x->song == nullptr) return {};
            const auto* track = x->trackAt(trackIndex);
            if (track == nullptr || deviceIndex < 0 || (size_t) deviceIndex >= track->devices.size()) return {};
            const auto& sc = track->devices[(size_t) deviceIndex].sidechain;
            if (! sc.routed()) return {};
            for (const auto& t : x->song->tracks)
                if (t.id == sc.sourceTrackId) return t.name.empty() ? "Track " + std::to_string(t.id) : t.name;
            return {};
        };
        phase->onChooseSidechain = [context, trackIndex, deviceIndex] {
            if (auto* x = context()) showSidechainMenu(*x, trackIndex, deviceIndex);
        };
    }
}

// ---------------------------------------------------------------------------------------------------------------------
bool ProFaceWindowView::isProFaceDevice(const std::string& type)
{
    return type == "SonoraStutter" || type == "SonoraSweep" || type == "SonoraPhaseAlign";
}

std::optional<ProFaceWindowView::Site> ProFaceWindowView::find(const UiContext& x, const UiContext::WavetableKey& key)
{
    for (int index = 0; index < x.numControlTracks(); ++index)
    {
        const auto* track = x.trackAt(index);
        if (track == nullptr || track->id != key.trackId) continue;
        for (size_t d = 0; d < track->devices.size(); ++d)
        {
            const auto& top = track->devices[d];
            if (top.id != key.deviceId) continue;
            Site site;
            site.track = index;
            site.device = (int) d;
            devices::DeviceProcessor* processor = nullptr;
            if (x.graph != nullptr && (size_t) index < x.graph->nodes.size() && d < x.graph->nodes[(size_t) index].processorForModelDevice.size())
                processor = x.graph->nodes[(size_t) index].processorForModelDevice[d];
            if (key.chain < 0) site.model = &top;
            else
            {
                if ((size_t) key.chain >= top.chains.size()) return std::nullopt;
                const auto& chain = top.chains[(size_t) key.chain].devices;
                size_t child = chain.size();
                for (size_t c = 0; c < chain.size(); ++c)
                    if (chain[c].id == key.childId) { child = c; site.model = &chain[c]; }
                if (site.model == nullptr) return std::nullopt;
                auto* rack = dynamic_cast<devices::RackProcessor*>(processor);
                processor = nullptr;
                if (rack && (size_t) key.chain < rack->chains().size())
                {
                    const auto& processors = rack->chains()[(size_t) key.chain].processorForModelDevice;
                    if (child < processors.size()) processor = processors[child];
                }
            }
            if (! isProFaceDevice(site.model->type)) return std::nullopt;
            site.processor = processor;
            return site;
        }
        return std::nullopt;
    }
    return std::nullopt;
}

ProFaceWindowView::ProFaceWindowView(UiContext& context, UiContext::WavetableKey key) : RootWidget(context), key_(key) {}

void ProFaceWindowView::setKey(const UiContext::WavetableKey& key)
{
    if (key == key_) return;
    key_ = key;
    built_ = false;
}

bool ProFaceWindowView::stale() const
{
    const auto& x = context();
    return ! built_ || builtSong_ != x.song || builtGraph_ != x.graph || builtStructure_ != x.trackStructure();
}

void ProFaceWindowView::sync()
{
    if (stale()) rebuild();
    layout();
    markDirty();
}

void ProFaceWindowView::rebuild()
{
    auto& x = context();
    removeAllChildren();
    face_ = nullptr;
    built_ = true;
    builtSong_ = x.song;
    builtGraph_ = x.graph;
    builtStructure_ = x.trackStructure();
    if (x.song == nullptr) return;
    const auto site = find(x, key_);
    if (! site)
    {
        if (! goneSent_ && onGone) { goneSent_ = true; onGone(); }
        return;
    }
    std::string title = site->model->displayName;
    if (title.empty())
        title = site->model->type == "SonoraStutter" ? "Sonora Stutter" : site->model->type == "SonoraSweep" ? "Sonora Sweep" : "Sonora Phase Align";
    if (const auto* track = x.trackAt(site->track); track && ! track->name.empty()) title += "/" + track->name;
    if (title != title_)
    {
        title_ = title;
        if (onTitleChanged) onTitleChanged(title_);
    }
    if (site->processor == nullptr) return;
    face_ = addProFace(*this, site->processor);
    if (face_ == nullptr) return;
    size_ = face_->fullSize();
    if (key_.chain < 0) wireProFace(*face_, [this] { return &context(); }, site->track, site->device);
}

void ProFaceWindowView::layout()
{
    if (stale()) rebuild();
    if (face_ == nullptr) return;
    const auto r = localBounds();
    const auto d = face_->fullSize();
    const float s = std::max(0.01f, std::min(r.w / d.w, r.h / d.h));
    const float w = d.w * s, h = d.h * s;
    face_->setBounds({std::floor((r.w - w) * 0.5f), std::floor((r.h - h) * 0.5f), w, h});
}

void ProFaceWindowView::paint(SkCanvas& c, const Theme& t)
{
    const auto r = localBounds();
    paint::fill(c, r, pro::kBackground);
    if (face_ == nullptr && context().song != nullptr)
        paint::text(c, "Device unavailable", {8, 8, r.w - 16, 20}, t.color("TextDisabled"), t.metrics.fontSize);
}

void ProFaceWindowView::tick()
{
    if (stale())
    {
        layout();
        markDirty();
    }
    RootWidget::tick();
}
} // namespace sonora::ui
