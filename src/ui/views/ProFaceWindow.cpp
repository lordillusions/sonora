#include "ui/views/ProFaceWindow.h"

#include "core/DeviceCatalog.h"
#include "devices/MidiGenerator.h"
#include "devices/RackProcessor.h"
#include "devices/native/Correlation.h"
#include "devices/native/Kickstract.h"
#include "devices/native/Seraph.h"
#include "devices/native/PhaseAlign.h"
#include "devices/native/Stutter.h"
#include "devices/native/Sweep.h"
#include "devices/native/Phrase.h"
#include "engine/EngineGraph.h"
#include "ui/DeviceMenu.h"
#include "ui/GeneratorClipUi.h"
#include "ui/Painter.h"
#include "ui/views/CorrelationFace.h"
#include "ui/views/KickstractFace.h"
#include "ui/views/SeraphFace.h"
#include "ui/views/PhaseAlignFace.h"
#include "ui/views/StutterFace.h"
#include "ui/views/SweepFace.h"
#include "ui/views/PhraseFace.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <sstream>

namespace sonora::ui
{
ProFace* addProFace(Widget& parent, devices::DeviceProcessor* processor)
{
    if (auto* stutter = dynamic_cast<devices::Stutter*>(processor)) return parent.add<StutterFace>(*stutter);
    if (auto* sweep = dynamic_cast<devices::Sweep*>(processor)) return parent.add<SweepFace>(*sweep);
    if (auto* phase = dynamic_cast<devices::PhaseAlign*>(processor)) return parent.add<PhaseAlignFace>(*phase);
    if (auto* meter = dynamic_cast<devices::Correlation*>(processor)) return parent.add<CorrelationFace>(*meter);
    if (auto* phrase = dynamic_cast<devices::Phrase*>(processor)) return parent.add<PhraseFace>(*phrase); // PLG-25
    if (auto* seraph = dynamic_cast<devices::Seraph*>(processor)) return parent.add<SeraphFace>(*seraph);
    if (auto* kick = dynamic_cast<devices::Kickstract*>(processor)) return parent.add<KickstractFace>(*kick); // PLG-17f
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

// PLG-27b: Real voices into a Seraph as one undoable edit: its content (the live gate and preset name, plus the files) and
// Source = Real. Files that cannot sing are refused with the reason before any edit.
void loadSeraphVoices(UiContext& x, SeraphFace& face, int trackIndex, int deviceIndex, const std::vector<std::string>& paths, bool clear)
{
    if (! x.editDevice) return;
    auto& seraph = face.seraph();
    face.realMessage.clear();
    std::vector<std::string> keep = clear ? std::vector<std::string>{} : seraph.realPaths();
    int added = 0;
    for (const auto& path : paths)
    {
        std::string error;
        if (devices::Seraph::analyseFile(path, error) == nullptr) { face.realMessage = error; continue; }
        if (std::find(keep.begin(), keep.end(), path) == keep.end()) { keep.push_back(path); ++added; }
    }
    if (added == 0 && ! clear) return;
    // the content as the device has it now, with the new list of files
    std::string text;
    {
        std::istringstream in(seraph.encodeContent());
        std::string line;
        while (std::getline(in, line))
            if (line.rfind("real ", 0) != 0) text += line + "\n";
        for (const auto& path : keep) text += "real " + path + "\n";
    }
    x.editDevice(trackIndex, deviceIndex, [text, clear](model::Device& d) {
        auto content = std::make_shared<model::NativeContent>();
        content->format = devices::Seraph::kContentFormat;
        content->data.assign(text.begin(), text.end());
        d.nativeContent = std::move(content);
        d.nativeContentEdited = true;
        if (clear) return;
        bool found = false;
        for (auto& p : d.parameters)
            if (p.path == "Source") { p.value = devices::Seraph::RealSource; found = true; }
        if (! found)
        {
            model::Parameter p;
            p.path = "Source";
            p.value = devices::Seraph::RealSource;
            d.parameters.push_back(std::move(p));
        }
    }, clear ? "Clear Voices" : "Load Voice");
}

void wireProFace(ProFace& face, std::function<UiContext*()> context, int trackIndex, int deviceIndex)
{
    auto* processor = &face.device();
    face.mappingTarget = [processor, trackIndex, deviceIndex](int parameter) -> std::optional<midi::ControlTarget> {
        if (parameter < 0 || (size_t) parameter >= processor->parameters().size()) return std::nullopt;
        return midi::ControlTarget{midi::ControlTarget::Kind::DeviceParameter, trackIndex, deviceIndex, 0,
                                   processor->parameters()[(size_t) parameter].id};
    };
    if (dynamic_cast<devices::MidiGenerator*>(processor) != nullptr) // PLG-25a: generator faces' To Clip button
        face.onToClip = [context, trackIndex, deviceIndex] {
            if (auto* x = context()) generatorToClipFromFace(*x, trackIndex, deviceIndex);
        };
    if (auto* seraph = dynamic_cast<SeraphFace*>(&face)) // PLG-27b: Real voices from the Browser, the Finder or Load...
    {
        // PLG-27h: the Seraph Voices pack from the app's Packs folder, downloaded through the Browser's pack store (the
        // catalogue is fetched first: the store runs its jobs in order). The face showed the credit line before this click.
        if (auto* x = context()) seraph->packsRoot = x->samplePacksRoot;
        seraph->onGetPack = [context] {
            auto* x = context();
            if (x == nullptr) return;
            if (x->showSamplePackList && x->samplePackList != UiContext::SamplePackList::Off) x->showSamplePackList();
            if (x->downloadSamplePack) x->downloadSamplePack(devices::Seraph::kPackId);
        };
        seraph->onVoicesDropped = [context, seraph, trackIndex, deviceIndex](const std::vector<std::string>& paths) {
            if (auto* x = context()) loadSeraphVoices(*x, *seraph, trackIndex, deviceIndex, paths, false);
        };
        seraph->onClearVoices = [context, seraph, trackIndex, deviceIndex] {
            if (auto* x = context()) loadSeraphVoices(*x, *seraph, trackIndex, deviceIndex, {}, true);
        };
        seraph->onChooseVoices = [context, seraph, trackIndex, deviceIndex] {
            auto chooser = std::make_shared<juce::FileChooser>("Load sung notes into Sonora Seraph", juce::File(), "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.ogg");
            chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles
                                     | juce::FileBrowserComponent::canSelectMultipleItems,
                                 [context, chooser, seraph, alive = seraph->lifetime(), trackIndex, deviceIndex](const juce::FileChooser& fc) {
                if (alive.expired()) return; // the face went away while the chooser was open
                std::vector<std::string> paths;
                for (const auto& file : fc.getResults()) paths.push_back(file.getFullPathName().toStdString());
                if (auto* x = context(); x != nullptr && ! paths.empty()) loadSeraphVoices(*x, *seraph, trackIndex, deviceIndex, paths, false);
            });
        };
    }
    if (auto* kick = dynamic_cast<KickstractFace*>(&face)) // PLG-17e: the timeline window from the face's button
        kick->onOpenTimeline = [context, trackIndex, deviceIndex] {
            auto* x = context();
            if (x == nullptr) return;
            if (const auto key = x->wavetableKey(trackIndex, deviceIndex, -1, -1); key && x->openKickstractTimeline) x->openKickstractTimeline(*key);
        };
    if (auto* sweep = dynamic_cast<SweepFace*>(&face))
    {
        sweep->onSampleDropped = [context, trackIndex, deviceIndex](const std::string& path) {
            if (auto* x = context()) loadSweepSample(*x, trackIndex, deviceIndex, path);
        };
        // PLG-19d: the rendered sweep as a WAV, saved by hand or dragged out of the display onto a track (a Browser-style
        // sample drag: the arrangement or the session puts it where it is let go). Renders go next to the set
        // (Samples/Processed/Sweep), or to Music/Sonora/Sweep Renders for a set not saved yet.
        sweep->onDragOut = [context, sweep] {
            auto* x = context();
            if (x == nullptr) return;
            const auto& device = sweep->sweep();
            juce::File folder = juce::File::getSpecialLocation(juce::File::userMusicDirectory).getChildFile("Sonora/Sweep Renders");
            if (x->song != nullptr && ! x->song->sourcePath.empty())
                folder = juce::File(juce::String::fromUTF8(x->song->sourcePath.c_str())).getParentDirectory().getChildFile("Samples/Processed/Sweep");
            const bool sample = std::lround(device.getParameter(devices::Sweep::Source)) == devices::Sweep::Sample && ! device.sampleName().empty();
            const bool up = std::lround(device.getParameter(devices::Sweep::Direction)) == devices::Sweep::Up;
            const std::string name = (sample ? device.sampleName() : std::string("Sweep")) + (up ? " riser" : " downlifter");
            const auto file = folder.getChildFile(juce::String::fromUTF8(name.c_str()) + ".wav").getNonexistentSibling();
            std::string error;
            if (! device.renderToFile(file, error)) { x->statusText = "Could not render the sweep: " + error; return; }
            BrowserDrag drag;
            drag.kind = BrowserDrag::Kind::Sample;
            drag.value = file.getFullPathName().toStdString();
            drag.files = {drag.value};
            drag.label = file.getFileName().toStdString();
            x->browserDrag = drag;
            x->statusText = "Drop the sweep on a track";
        };
        sweep->onSaveWav = [context, sweep, alive = sweep->faceLifetime()] {
            const auto& device = sweep->sweep();
            const std::string base = device.sampleName().empty() ? std::string("Sonora Sweep") : device.sampleName() + " swell";
            auto chooser = std::make_shared<juce::FileChooser>("Save the sweep as a WAV",
                juce::File::getSpecialLocation(juce::File::userMusicDirectory).getChildFile(juce::String::fromUTF8(base.c_str()) + ".wav"), "*.wav");
            chooser->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting,
                                 [context, chooser, sweep, alive](const juce::FileChooser& fc) {
                if (alive.expired()) return; // the face (and its device) went away while the chooser was open
                const auto file = fc.getResult();
                if (file == juce::File()) return;
                std::string error;
                auto* x = context();
                const bool ok = sweep->sweep().renderToFile(file.withFileExtension("wav"), error);
                if (x != nullptr) x->statusText = ok ? "Saved " + file.getFileName().toStdString() : "Could not save the WAV: " + error;
            });
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
    return type == "SonoraStutter" || type == "SonoraSweep" || type == "SonoraPhaseAlign" || type == "SonoraCorrelation"
           || type == "SonoraPhrase" || type == "SonoraSeraph" || type == "SonoraKickstract";
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
    {
        const auto* entry = model::findCatalogEntry(site->model->type);
        title = entry != nullptr ? std::string(entry->displayName) : site->model->type;
    }
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
