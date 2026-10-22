#include "ui/views/WavetableWindowView.h"

#include "devices/RackProcessor.h"
#include "devices/native/WavetableSynth.h"
#include "engine/EngineGraph.h"
#include "ui/Painter.h"
#include "ui/views/WavetableEditor.h"
#include "ui/widgets/Buttons.h"

namespace sonora::ui
{
std::optional<WavetableSite> findWavetable(const UiContext& x, const UiContext::WavetableKey& key)
{
    for (int index = 0; index < x.numControlTracks(); ++index)
    {
        const auto* track = x.trackAt(index);
        if (track == nullptr || track->id != key.trackId) continue;
        for (size_t d = 0; d < track->devices.size(); ++d)
        {
            const auto& top = track->devices[d];
            if (top.id != key.deviceId) continue;
            WavetableSite site;
            site.track = index;
            site.device = (int) d;
            devices::DeviceProcessor* processor = nullptr;
            if (x.graph != nullptr && (size_t) index < x.graph->nodes.size() && d < x.graph->nodes[(size_t) index].processorForModelDevice.size())
                processor = x.graph->nodes[(size_t) index].processorForModelDevice[d];
            if (key.chain < 0)
            {
                if (top.type != "SonoraWavetable") return std::nullopt;
                site.model = &top;
            }
            else
            {
                if ((size_t) key.chain >= top.chains.size()) return std::nullopt;
                const auto& chain = top.chains[(size_t) key.chain].devices;
                for (size_t c = 0; c < chain.size(); ++c)
                    if (chain[c].id == key.childId && chain[c].type == "SonoraWavetable")
                    {
                        site.chain = key.chain;
                        site.child = (int) c;
                        site.model = &chain[c];
                        break;
                    }
                if (site.model == nullptr) return std::nullopt;
                auto* rack = dynamic_cast<devices::RackProcessor*>(processor);
                processor = nullptr;
                if (rack && (size_t) site.chain < rack->chains().size())
                {
                    const auto& processors = rack->chains()[(size_t) site.chain].processorForModelDevice;
                    if ((size_t) site.child < processors.size()) processor = processors[(size_t) site.child];
                }
            }
            site.processor = dynamic_cast<devices::WavetableSynth*>(processor);
            return site;
        }
        return std::nullopt;
    }
    return std::nullopt;
}

DeviceIdsByTrack deviceIdsByTrack(const model::Song& song)
{
    DeviceIdsByTrack ids;
    auto add = [&ids](const model::Track& track) {
        auto& list = ids[track.id];
        for (const auto& device : track.devices) list.push_back(device.id);
    };
    for (const auto& track : song.tracks) add(track);
    for (const auto& track : song.returns) add(track);
    add(song.main);
    return ids;
}

std::vector<ReplacedDevice> replacedDevices(const DeviceIdsByTrack& before, const model::Song& after)
{
    std::vector<ReplacedDevice> replaced;
    for (const auto& [track, ids] : deviceIdsByTrack(after))
    {
        const auto old = before.find(track);
        if (old == before.end() || old->second.size() != ids.size()) continue; // inserted or deleted: ids stay put
        for (size_t i = 0; i < ids.size(); ++i)
            if (old->second[i] != ids[i]) replaced.push_back({track, old->second[i], ids[i]});
    }
    return replaced;
}

WavetableWindowView::WavetableWindowView(UiContext& context, UiContext::WavetableKey key) : RootWidget(context), key_(key)
{
}

void WavetableWindowView::setKey(const UiContext::WavetableKey& key)
{
    if (key == key_) return;
    key_ = key;
    built_ = false;
}

bool WavetableWindowView::stale() const
{
    const auto& x = context();
    return ! built_ || builtSong_ != x.song || builtGraph_ != x.graph || builtStructure_ != x.trackStructure();
}

void WavetableWindowView::sync()
{
    if (stale()) rebuild();
    if (! closeRequested_ && context().song != nullptr && ! context().isWavetableFloating(key_)) requestClose();
    layout();
    markDirty();
}

void WavetableWindowView::rebuild()
{
    auto& x = context();
    removeAllChildren();
    editor_ = nullptr;
    dock_ = nullptr;
    built_ = true;
    builtSong_ = x.song;
    builtGraph_ = x.graph;
    builtStructure_ = x.trackStructure();
    if (x.song == nullptr) return; // a device edit is in progress: the song comes back with the new graph
    const auto site = findWavetable(x, key_);
    if (! site)
    {
        requestClose(); // deleted (or replaced): like a plug-in window whose device went away
        return;
    }
    std::string title = site->model->displayName.empty() ? "Sonora Prism" : site->model->displayName;
    if (const auto* track = x.trackAt(site->track); track && ! track->name.empty()) title += "/" + track->name;
    if (title != title_)
    {
        title_ = title;
        if (onTitleChanged) onTitleChanged(title_);
    }
    dock_ = add<TextButton>("Dock", TextButton::Style::Plain);
    dock_->onClick = [this] { dock(); };
    if (site->processor != nullptr)
        editor_ = add<WavetableEditor>(site->processor, site->track, site->device, site->chain, site->child);
}

void WavetableWindowView::requestClose()
{
    if (closeRequested_) return;
    closeRequested_ = true;
    if (context().closeWavetableWindow) context().closeWavetableWindow(key_);
}

void WavetableWindowView::dock()
{
    auto& x = context();
    const auto key = key_;
    x.setWavetableFloating(key, false);
    // Show the docked editor right away: its track's Device View, inside the rack chain when nested.
    if (const auto site = findWavetable(x, key))
    {
        x.selectedTrack = site->track;
        x.detailTab = DetailTab::Devices;
        x.detailVisible = true;
        x.selectedDeviceId = key.deviceId;
        if (site->chain >= 0)
        {
            x.selectedRackId = key.deviceId;
            x.selectedRackChain = site->chain;
        }
    }
    if (x.requestRepaint) x.requestRepaint();
    requestClose(); // the host destroys the window asynchronously: nothing here may run after it
}

void WavetableWindowView::layout()
{
    if (stale()) rebuild();
    if (dock_ != nullptr) dock_->setBounds({width() - 62, 5, 56, 16});
    if (editor_ != nullptr) editor_->setBounds({6, kHeader, std::max(0.0f, width() - 12), std::max(0.0f, height() - kHeader - 6)});
}

void WavetableWindowView::paint(SkCanvas& c, const Theme& t)
{
    const auto r = localBounds();
    paint::fill(c, r, t.color("SurfaceBackground"));
    paint::fill(c, {0, 0, r.w, kHeader}, t.color("SurfaceHighlight"));
    paint::text(c, title_, {8, 0, r.w - 80, kHeader}, t.color("ControlForeground"), t.metrics.fontSize, Align::Left, true);
    if (editor_ == nullptr && context().song != nullptr)
        paint::text(c, "Device unavailable", {8, kHeader + 8, r.w - 16, 20}, t.color("TextDisabled"), t.metrics.fontSize);
}

void WavetableWindowView::tick()
{
    // Children tick after this: never let the editor tick against a graph that was replaced.
    if (stale())
    {
        layout();
        markDirty();
    }
}
} // namespace sonora::ui
