#include "plugin/WavetablePluginEditor.h"

#include "devices/native/OscillatorMedia.h"
#include "devices/native/WavetableContent.h"
#include "gfx/SkiaCanvasComponent.h"
#include "plugin/WavetablePlugin.h"
#include "plugins/SerumLibrary.h"
#include "plugins/PrismPreset.h"
#include "ui/Painter.h"
#include "ui/views/WavetableEditor.h"

#include <algorithm>

namespace sonora::plugin
{
namespace
{
constexpr float kWidth = ui::WavetableEditor::kPreferredWidth + 12, kHeight = 720 + WavetablePluginView::kHeader + 6;
}

WavetablePluginView::WavetablePluginView(ui::UiContext& context, WavetablePlugin& plugin) : RootWidget(context), plugin_(plugin)
{
    rebuild();
}

void WavetablePluginView::rebuild()
{
    removeAllChildren();
    editor_ = nullptr;
    if (auto* synth = plugin_.synth()) editor_ = add<ui::WavetableEditor>(synth, 0, 0);
    layout();
    markDirty();
}

void WavetablePluginView::layout()
{
    if (editor_ != nullptr) editor_->setBounds({6, kHeader, std::max(0.0f, width() - 12), std::max(0.0f, height() - kHeader - 6)});
}

void WavetablePluginView::paint(SkCanvas& c, const ui::Theme& t)
{
    const auto r = localBounds();
    ui::paint::fill(c, r, t.color("SurfaceBackground"));
    ui::paint::fill(c, {0, 0, r.w, kHeader}, t.color("SurfaceHighlight"));
    ui::paint::text(c, plugin_.device().displayName, {8, 0, 240, kHeader}, t.color("ControlForeground"), t.metrics.fontSize,
                    ui::Align::Left, true);
    ui::paint::text(c, context().statusText, {256, 0, std::max(0.0f, r.w - 264), kHeader}, t.color("ControlForeground"),
                    t.metrics.fontSize, ui::Align::Right);
}

WavetablePluginEditor::WavetablePluginEditor(WavetablePlugin& plugin)
    : AudioProcessorEditor(plugin), plugin_(plugin), view_((connect(), context_), plugin)
{
    canvas_ = std::make_unique<gfx::SkiaCanvasComponent>(view_);
    addAndMakeVisible(*canvas_);
    plugin_.onSynthReplaced = [this] { triggerAsyncUpdate(); };
    setResizable(true, false);
    setResizeLimits(720, 320, 4000, 3000);
    setSize(juce::roundToInt(kWidth), juce::roundToInt(kHeight));
}

WavetablePluginEditor::~WavetablePluginEditor()
{
    cancelPendingUpdate();
    plugin_.onSynthReplaced = nullptr;
    canvas_.reset(); // it references view_
    view_.removeAllChildren();
    plugin_.releaseRetired();
}

void WavetablePluginEditor::resized()
{
    canvas_->setBounds(getLocalBounds());
}

void WavetablePluginEditor::handleAsyncUpdate()
{
    view_.rebuild();
    plugin_.releaseRetired();
    canvas_->requestFrame();
}

void WavetablePluginEditor::connect()
{
    // Runs before view_ is built (from its constructor's argument list): the dashboard reads the context while it builds.
    context_.theme.useBuiltin(false);
    context_.song = &plugin_.song();
    context_.selectedTrack = 0;
    context_.requestRepaint = [this] { if (canvas_) canvas_->requestFrame(); };
    context_.editDevice = [this](int, int, std::function<void(model::Device&)> edit, std::string) {
        plugin_.editDevice([&edit](model::Device& device) { edit(device); return true; });
    };
    context_.importWavetable = [this](int, int, int oscillator, const std::string& path, int, int) { importTable(oscillator, path); };
    context_.importOscillatorMedia = [this](int, int, int oscillator, const std::string& path, int, int) { importMedia(oscillator, path); };
    context_.importWavetablePreset = [this](int, int, const std::string& path, int, int) { importPreset(path); };
    context_.editWavetableLfo = [this](int, int, int lfo, const dsp::CurveGraph& curve, int, int) {
        std::string error;
        plugin_.editDevice([&](model::Device& device) { return devices::storeWavetableLfo(device, lfo, curve, error); });
        if (! error.empty()) context_.statusText = "Could not edit LFO: " + error;
    };
    context_.editWavetableModulationCurve = [this](int, int, int slot, bool auxiliary, const dsp::CurveGraph& curve, bool enabled, int, int) {
        std::string error;
        plugin_.editDevice([&](model::Device& device) {
            return devices::storeWavetableModulationCurve(device, slot, auxiliary, curve, enabled, error);
        });
        if (! error.empty()) context_.statusText = "Could not edit modulation curve: " + error;
    };
    context_.setWavetableEffects = [this](int, int, const devices::WavetableEffects::Configuration& configuration, int, int) {
        setEffects(configuration);
    };
    context_.importWavetableImpulseResponse = [this](int, int, devices::WavetableEffects::ModuleId id, const std::string& path, int, int) {
        importImpulse(id, path);
    };
}

// The imports below follow AppController's (app/AppController.cpp) on the plug-in's single device.
void WavetablePluginEditor::importTable(int oscillator, const std::string& path)
{
    const auto loaded = plugins::loadSerumTable(juce::File(juce::String::fromUTF8(path.c_str())));
    if (! loaded) { context_.statusText = "Could not import wavetable: " + loaded.error; return; }
    std::string error;
    if (plugin_.editDevice([&](model::Device& device) { return devices::storeWavetableTable(device, oscillator, *loaded.data, error); }))
        context_.statusText = "Imported " + loaded.data->name();
    else context_.statusText = "Could not import wavetable: " + error;
}

void WavetablePluginEditor::importMedia(int oscillator, const std::string& path)
{
    const juce::File file{juce::String::fromUTF8(path.c_str())};
    const auto loaded = file.hasFileExtension("sfz") ? devices::OscillatorMedia::fromSfz(file, true)
                                                    : devices::OscillatorMedia::fromFile(file, true);
    if (! loaded) { context_.statusText = "Could not import sample bank: " + loaded.error; return; }
    std::string error;
    if (plugin_.editDevice([&](model::Device& device) { return devices::storeOscillatorMedia(device, oscillator, *loaded.data, error); }))
    {
        plugin_.buildLog.insert(plugin_.buildLog.end(), loaded.warnings.begin(), loaded.warnings.end());
        context_.statusText = "Imported " + loaded.data->name();
        if (! loaded.warnings.empty()) context_.statusText += " — " + std::to_string(loaded.warnings.size()) + " sample bank notes";
    }
    else context_.statusText = "Could not import sample bank: " + error;
}

void WavetablePluginEditor::setEffects(const devices::WavetableEffects::Configuration& configuration)
{
    std::string error;
    {
        devices::WavetableEffects prepared;
        if (! prepared.prepare(configuration, plugin_.getSampleRate() > 0 ? plugin_.getSampleRate() : 44100, error))
        { context_.statusText = "Could not edit effects: " + error; return; }
    }
    plugin_.editDevice([&](model::Device& device) { return devices::storeWavetableEffects(device, configuration, error); });
    if (! error.empty()) context_.statusText = "Could not edit effects: " + error;
}

void WavetablePluginEditor::importImpulse(uint32_t moduleId, const std::string& path)
{
    using Effects = devices::WavetableEffects;
    const auto loaded = devices::ConvolutionIR::fromFile(juce::File(juce::String::fromUTF8(path.c_str())));
    if (! loaded) { context_.statusText = "Could not import impulse response: " + loaded.error; return; }
    std::string error;
    const bool changed = plugin_.editDevice([&](model::Device& device) {
        Effects::Configuration configuration;
        if (! devices::readWavetableEffects(device, configuration, error)) return false;
        const auto replace = [&](const auto& self, std::vector<Effects::Module>& modules) -> bool {
            for (auto& module : modules)
            {
                if (module.id == moduleId)
                {
                    if (module.type != Effects::Type::Convolution) { error = "Select a convolution module for this impulse response"; return false; }
                    module.impulse = loaded.data;
                    return true;
                }
                for (auto& band : module.bands) if (self(self, band)) return true;
            }
            return false;
        };
        bool found = false;
        for (auto& bus : configuration.buses) if (replace(replace, bus)) { found = true; break; }
        if (! found) { if (error.empty()) error = "The selected convolution module is no longer available"; return false; }
        Effects prepared;
        if (! prepared.prepare(configuration, plugin_.getSampleRate() > 0 ? plugin_.getSampleRate() : 44100, error)) return false;
        return devices::storeWavetableEffects(device, configuration, error);
    });
    context_.statusText = changed ? "Imported " + loaded.data->name() : "Could not import impulse response: " + error;
}

void WavetablePluginEditor::importPreset(const std::string& path)
{
    const juce::File file{juce::String::fromUTF8(path.c_str())};
    auto imported = plugins::loadPrismPreset(file);
    if (! imported) { context_.statusText = "Could not open preset: " + imported.error; return; }
    plugin_.editDevice([&](model::Device& device) {
        imported.device.id = device.id;
        device = std::move(imported.device);
        return true;
    });
    plugin_.buildLog.insert(plugin_.buildLog.end(), imported.warnings.begin(), imported.warnings.end());
    context_.statusText = "Loaded " + file.getFileNameWithoutExtension().toStdString();
}
} // namespace sonora::plugin
