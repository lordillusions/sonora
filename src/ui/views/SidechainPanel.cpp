#include "ui/views/SidechainPanel.h"

#include "devices/DeviceProcessor.h"
#include "ui/Controls.h"
#include "ui/Painter.h"
#include "ui/UiContext.h"
#include "ui/widgets/Knob.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <cmath>
#include <cstdio>

namespace sonora::ui
{
namespace
{
constexpr float kMinDb = -70.0f, kMaxDb = 24.0f; // the reference's Volume range (0.000316 .. 15.85)
float toDb(float gain) { return std::clamp(20.0f * std::log10(std::max(gain, 1e-6f)), kMinDb, kMaxDb); }
float fromDb(float db) { return db <= kMinDb + 1e-3f ? 0.000316228f : std::pow(10.0f, db / 20.0f); }
} // namespace

SidechainPanel::SidechainPanel(devices::DeviceProcessor* processor, int track, int device)
    : processor_(processor), track_(track), device_(device)
{
    Knob::Source gain;
    gain.get = [this] { return processor_ ? toDb(processor_->sidechainGainLevel()) : 0.0f; };
    gain.set = [this](float db) { setLevels(fromDb(db), processor_ ? processor_->sidechainMixLevel() : 1.0f); };
    gain.minValue = kMinDb;
    gain.maxValue = kMaxDb;
    gain.defaultValue = 0;
    gain.format = [](float db) {
        char text[24];
        if (db <= kMinDb + 1e-3f) return std::string("-inf dB");
        std::snprintf(text, sizeof text, "%.2f dB", db);
        return std::string(text);
    };
    gain_ = add<Knob>("Gain", std::move(gain));
    gain_->tooltip = "Gain: the level of the sidechain signal";
    Knob::Source mix;
    mix.get = [this] { return processor_ ? processor_->sidechainMixLevel() : 1.0f; };
    mix.set = [this](float v) { setLevels(processor_ ? processor_->sidechainGainLevel() : 1.0f, v); };
    mix.defaultValue = 1;
    mix.format = [](float v) {
        char text[16];
        std::snprintf(text, sizeof text, "%.0f %%", v * 100);
        return std::string(text);
    };
    mix_ = add<Knob>("Mix", std::move(mix));
    mix_->tooltip = "Mix: how much of the sidechain input is the routed track (the rest is the device's own input)";
}

const model::Sidechain* SidechainPanel::sidechain() const
{
    const auto* track = ctx().trackAt(track_);
    if (track == nullptr || device_ < 0 || (size_t) device_ >= track->devices.size()) return nullptr;
    return &track->devices[(size_t) device_].sidechain;
}

void SidechainPanel::layout()
{
    const float w = width();
    const float knobH = std::clamp((height() - 60 - 24) / 2.0f, 30.0f, 46.0f);
    gain_->setBounds({8, 58, w - 16, knobH});
    mix_->setBounds({8, 58 + knobH + 2, w - 16, knobH});
}

std::string SidechainPanel::sourceLabel() const
{
    const auto* sc = sidechain();
    if (sc == nullptr || sc->sourceTrackId < 0 || ctx().song == nullptr) return "No Input";
    for (const auto& t : ctx().song->tracks)
        if (t.id == sc->sourceTrackId) return t.name.empty() ? "Track " + std::to_string(t.id) : t.name;
    return "No Input";
}

std::string SidechainPanel::tapLabel() const
{
    const auto* sc = sidechain();
    if (sc == nullptr || sc->sourceTrackId < 0) return "";
    return sc->tap == model::Sidechain::Tap::PreFx ? "Pre FX" : sc->tap == model::Sidechain::Tap::PostFx ? "Post FX" : "Post Mixer";
}

void SidechainPanel::edit(std::function<void(model::Sidechain&)> change, std::string label)
{
    if (! ctx().editDevice) return;
    ctx().editDevice(track_, device_, [change](model::Device& d) {
        change(d.sidechain);
        d.sidechain.edited = true;
        for (auto& p : d.parameters) // the device's own switch follows (Hush "Sidechain_on")
            if (p.path == "Sidechain_on") p.value = d.sidechain.routed() ? 1.0 : 0.0;
    }, std::move(label));
}

void SidechainPanel::setLevels(float gain, float mix)
{
    if (processor_) processor_->setSidechainLevels(gain, mix);
    if (ctx().setDeviceSidechainLevels) ctx().setDeviceSidechainLevels(track_, device_, gain, mix);
    repaint();
}

std::vector<SidechainPanel::Choice> SidechainPanel::sourceChoices()
{
    std::vector<Choice> choices;
    const auto* sc = sidechain();
    const auto* host = ctx().trackAt(track_);
    if (sc == nullptr || host == nullptr || ctx().song == nullptr) return choices;
    choices.push_back({"No Input", [this] { edit([](model::Sidechain& s) { s.sourceTrackId = -1; s.on = false; }, "Sidechain off"); },
                       sc->sourceTrackId < 0});
    for (const auto& t : ctx().song->tracks)
    {
        if (t.id == host->id) continue;
        const int id = t.id;
        const std::string name = t.name.empty() ? "Track " + std::to_string(t.id) : t.name;
        choices.push_back({name, [this, id, name] {
            edit([id](model::Sidechain& s) { s.sourceTrackId = id; s.on = true; }, "Sidechain from " + name);
        }, sc->sourceTrackId == id});
    }
    return choices;
}

std::vector<SidechainPanel::Choice> SidechainPanel::tapChoices()
{
    std::vector<Choice> choices;
    const auto* sc = sidechain();
    if (sc == nullptr || sc->sourceTrackId < 0) return choices;
    using Tap = model::Sidechain::Tap;
    for (const auto& [label, tap] : {std::pair<const char*, Tap>{"Pre FX", Tap::PreFx}, {"Post FX", Tap::PostFx}, {"Post Mixer", Tap::PostMixer}})
    {
        const Tap chosen = tap;
        choices.push_back({label, [this, chosen] { edit([chosen](model::Sidechain& s) { s.tap = chosen; }, "Sidechain Tap"); },
                           sc->tap == tap});
    }
    return choices;
}

void SidechainPanel::toggleMute()
{
    const auto* sc = sidechain();
    if (sc == nullptr || sc->sourceTrackId < 0) return;
    const bool muted = ! sc->on;
    edit([muted](model::Sidechain& s) { s.on = muted; }, muted ? "Sidechain On" : "Sidechain Mute");
}

void SidechainPanel::show(std::vector<Choice> choices)
{
    if (choices.empty()) return;
    juce::PopupMenu menu;
    for (const auto& choice : choices)
    {
        juce::PopupMenu::Item item(juce::String::fromUTF8(choice.label.c_str()));
        item.isTicked = choice.ticked;
        item.action = choice.run;
        menu.addItem(std::move(item));
    }
    menu.showMenuAsync(juce::PopupMenu::Options());
}

void SidechainPanel::mouseDown(const MouseEvent& e)
{
    if (sourceBox().contains(e.x, e.y)) show(sourceChoices());
    else if (tapBox().contains(e.x, e.y)) show(tapChoices());
    else if (muteBox().contains(e.x, e.y)) toggleMute();
}

void SidechainPanel::paint(SkCanvas& c, const Theme& t)
{
    const auto* sc = sidechain();
    const bool routed = sc != nullptr && sc->sourceTrackId >= 0;
    paint::text(c, "Sidechain", {5, 1, width() - 10, 15}, t.color("ControlForeground"), t.metrics.fontSize, Align::Left);
    ctl::dropdown(c, t, sourceBox(), sourceLabel());
    if (routed) ctl::dropdown(c, t, tapBox(), tapLabel());
    else ctl::emptyBox(c, t, tapBox());
    ctl::toggle(c, t, muteBox(), "Mute", routed && sc != nullptr && ! sc->on, t.color("ChosenDefault"), routed);
}
} // namespace sonora::ui
