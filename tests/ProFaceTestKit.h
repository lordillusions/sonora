#pragma once
// PLG-23: helpers for the ProFace UI tests (Stutter, Sweep, Phase Align): real clicks on a face's design grid through the
// root, the device chain of a one-track song, and the face's own window.

#include "TestFramework.h"
#include "devices/DeviceFactory.h"
#include "engine/EngineGraph.h"
#include "engine/SampleCache.h"
#include "gfx/SkiaCanvasComponent.h"
#include "ui/RootWidget.h"
#include "ui/views/MainLayout.h"
#include "ui/views/ProFace.h"
#include "ui/views/ProFaceWindow.h"

#include <cmath>
#include <memory>
#include <string>
#include <vector>

namespace sonora::testkit
{
template <typename T> void collect(ui::Widget& w, std::vector<T*>& found)
{
    if (auto* p = dynamic_cast<T*>(&w)) found.push_back(p);
    for (const auto& child : w.children()) collect(*child, found);
}

// A root-coordinate event at a design-grid point of a face.
inline ui::MouseEvent designAt(ui::ProFace& face, float x, float y, int clicks = 1)
{
    auto e = face.designToLocal(x, y);
    const auto origin = face.boundsInRoot();
    e.x += origin.x;
    e.y += origin.y;
    e.clicks = clicks;
    return e;
}

// Clicks the control labelled `label` (its centre); false when the face has no such control.
inline bool click(ui::RootWidget& root, ui::ProFace& face, const std::string& label)
{
    const auto centre = face.controlCentre(label);
    if (! centre) return false;
    root.dispatchMouseDown(designAt(face, centre->first, centre->second));
    root.dispatchMouseUp(designAt(face, centre->first, centre->second));
    return true;
}

// Drags the control labelled `label` vertically by `dy` design pixels.
inline bool drag(ui::RootWidget& root, ui::ProFace& face, const std::string& label, float dy)
{
    const auto centre = face.controlCentre(label);
    if (! centre) return false;
    root.dispatchMouseDown(designAt(face, centre->first, centre->second - 20));
    for (int i = 1; i <= 8; ++i) root.dispatchMouseDrag(designAt(face, centre->first, centre->second - 20 + dy * (float) i / 8.0f));
    root.dispatchMouseUp(designAt(face, centre->first, centre->second - 20 + dy));
    return true;
}

inline juce::File testFile(const std::string& name)
{
    return juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile(name);
}

// A one-track song with `devices`, its graph and a UI context showing the device chain.
struct Chain
{
    model::Song song;
    engine::SampleCache samples;
    devices::BuildContext build;
    engine::GraphBuildResult graph;
    ui::UiContext context;
    std::vector<std::string> edits; // labels passed to editDevice
    std::vector<std::function<void(model::Device&)>> editFns;

    Chain(model::TrackKind kind, std::vector<model::Device> devices, double rate = 48000.0)
    {
        song.tracks.resize(1);
        song.tracks[0].id = 1;
        song.tracks[0].name = "Bass";
        song.tracks[0].kind = kind;
        song.tracks[0].devices = std::move(devices);
        int id = 10;
        for (auto& d : song.tracks[0].devices) d.id = id++;
        graph = engine::buildGraph(song, build, samples);
        graph.graph->prepare(rate, 512);
        context.song = &song;
        context.graph = graph.graph.get();
        context.controls = graph.graph->controls;
        context.browserVisible = false;
        context.detailTab = ui::DetailTab::Devices;
        context.editDevice = [this](int, int, std::function<void(model::Device&)> fn, std::string label) {
            edits.push_back(label);
            editFns.push_back(std::move(fn));
        };
    }
    devices::DeviceProcessor* processor(size_t device = 0) const { return graph.graph->nodes[0].processorForModelDevice[device]; }
};
} // namespace sonora::testkit
