#include "ui/views/InputControls.h"
#include "ui/UiContext.h"
#include "ui/Controls.h"
#include "ui/Painter.h"
#include "core/InputRouting.h"
#include "core/DeviceCatalog.h"

#include <algorithm>
#include <memory>

#include <juce_gui_basics/juce_gui_basics.h>

namespace sonora::ui
{
namespace
{
constexpr float kBox = 13.0f;

std::string outputName(const model::Track& track)
{
    if (model::isMidiOnly(track)) return "No Output";
    const auto& r = track.outputRouting;
    if (r.empty() || r == "AudioOut/Main" || r == "AudioOut/Master")
        return "Main";
    if (r == "AudioOut/None")
        return "No Output";
    if (r.find("GroupTrack") != std::string::npos)
        return "Group";
    if (r.find("External") != std::string::npos)
        return "Ext. Out";
    const auto slash = r.find_last_of('/');
    return slash == std::string::npos ? r : r.substr(slash + 1);
}
} // namespace

InputControls::Rows InputControls::rows() const
{
    const float w = width();
    Rows r;
    if (captions_)
    {
        r.sourceCaption = {5, 3, w - 10, 10};
        r.source = {5, 14, w - 10, kBox};
        r.channel = {5, 30, w - 10, kBox};
        r.monitorCaption = {5, 44, w - 10, 10};
        r.monitorY = 56;
        r.outputCaption = {5, 69, w - 10, 10};
        r.output = {5, 82, w - 10, kBox};
        r.spare = {5, 98, w - 10, kBox};
    }
    else
    {
        r.source = {1, 0, w - 2, kBox};
        r.channel = {1, 16, w - 2, kBox};
        r.monitorY = 32;
        r.output = {1, 48, w - 2, kBox};
        r.spare = {1, 64, w - 2, kBox};
    }
    return r;
}

Rect InputControls::monitorButton(int mode) const
{
    const float x0 = captions_ ? 5.0f : 1.0f;
    const float widths[] = {16, 26, 19};
    float x = x0;
    for (int i = 0; i < mode; ++i)
        x += widths[i] + 4;
    return {x, rows().monitorY, widths[mode], kBox};
}

void InputControls::paint(SkCanvas& canvas, const Theme& theme)
{
    const auto* track = ctx().trackAt(track_);
    if (! track) return;
    const bool midi = track->kind == model::TrackKind::Midi;
    const auto input = model::parseInput(*track);
    std::string source = input.kind == model::InputKind::None ? "No Input" : input.kind == model::InputKind::Resampling ? "Resampling"
                         : midi ? "All Ins" : "Ext. In";
    if (input.kind == model::InputKind::Unsupported) source = track->inputUpperDisplay.empty() ? "Unavailable" : track->inputUpperDisplay;
    if (midi && ! input.device.empty())
    {
        source = input.device;
        bool found = false;
        if (ctx().midiInputDevices)
            for (const auto& [id, name] : ctx().midiInputDevices()) if (id == input.device) { source = name; found = true; }
        if (! found) source += " (offline)";
    }
    std::string channel = midi ? (input.channel == 0 ? "All Channels" : "Ch. " + std::to_string(input.channel)) : std::to_string(input.channel + 1);
    if (input.kind == model::InputKind::AudioStereo) channel += "/" + std::to_string(input.channel + 2);
    if (input.kind == model::InputKind::None || input.kind == model::InputKind::Resampling) channel = "—";
    const bool hasChannel = input.kind != model::InputKind::None && input.kind != model::InputKind::Resampling;
    const bool monitorable = input.kind != model::InputKind::Resampling && input.kind != model::InputKind::None;

    const auto r = rows();
    if (captions_)
    {
        ctl::caption(canvas, theme, r.sourceCaption, midi ? "MIDI From" : "Audio From");
        ctl::caption(canvas, theme, r.monitorCaption, "Monitor");
        ctl::caption(canvas, theme, r.outputCaption, model::isMidiOnly(*track) ? "MIDI To" : "Audio To");
    }
    ctl::dropdown(canvas, theme, r.source, source);
    ctl::dropdown(canvas, theme, r.channel, channel, hasChannel);
    static const char* names[] = {"In", "Auto", "Off"};
    for (int mode = 0; mode < 3; ++mode)
    {
        const bool selected = track->monitoring == mode;
        ctl::toggle(canvas, theme, monitorButton(mode), names[mode], selected, theme.color("ChosenDefault"),
                    monitorable || mode == 2);
    }
    ctl::dropdown(canvas, theme, r.output, outputName(*track));
    ctl::emptyBox(canvas, theme, r.spare);
}

void InputControls::applySelection(model::InputSelection selection, const std::string& displayName)
{
    const auto* track = ctx().trackAt(track_);
    if (! track || ! ctx().applyEdit) return;
    model::Track changed;
    changed.kind = track->kind;
    model::setInputSelection(changed, selection);
    if (! displayName.empty()) changed.inputUpperDisplay = displayName;
    ctx().applyEdit(model::setTrackInput(*ctx().song, track_, changed.inputRouting, changed.inputUpperDisplay, changed.inputLowerDisplay,
                                      selection.kind == model::InputKind::Resampling ? 2 : track->monitoring));
    repaint();
}

void InputControls::mouseDown(const MouseEvent& event)
{
    const auto* track = ctx().trackAt(track_);
    if (! track || ! ctx().applyEdit) return;
    ctx().selectedTrack = track_;
    const auto r = rows();
    for (int mode = 0; mode < 3; ++mode)
        if (monitorButton(mode).contains(event.x, event.y))
        {
            if (model::parseInput(*track).kind == model::InputKind::Resampling) return;
            if (mode != track->monitoring)
                ctx().applyEdit(model::setTrackInput(*ctx().song, track_, track->inputRouting, track->inputUpperDisplay, track->inputLowerDisplay, mode));
            repaint();
            return;
        }
    const auto selection = model::parseInput(*track);
    const bool midi = track->kind == model::TrackKind::Midi;
    const bool onSource = r.source.contains(event.x, event.y), onChannel = r.channel.contains(event.x, event.y);
    if (! onSource && ! onChannel)
        return;

    // Live's dropdowns: a popup listing the choices with the current one ticked.
    struct Choice { std::string label; bool ticked; model::InputSelection selection; std::string display; };
    auto choices = std::make_shared<std::vector<Choice>>();
    if (onSource && midi)
    {
        const bool midiOn = selection.kind == model::InputKind::Midi;
        choices->push_back({"All Ins", midiOn && selection.device.empty(), {model::InputKind::Midi, selection.kind == model::InputKind::Midi ? selection.channel : 0, {}}, "All Ins"});
        if (ctx().midiInputDevices)
            for (const auto& [id, name] : ctx().midiInputDevices())
            {
                model::InputSelection s{model::InputKind::Midi, midiOn ? selection.channel : 0, id};
                choices->push_back({name, midiOn && selection.device == id, s, name});
            }
        choices->push_back({"No Input", selection.kind == model::InputKind::None, {model::InputKind::None, 0, {}}, ""});
    }
    else if (onSource)
    {
        choices->push_back({"Ext. In", selection.kind == model::InputKind::AudioMono || selection.kind == model::InputKind::AudioStereo,
                            {model::InputKind::AudioStereo, 0, {}}, ""});
        choices->push_back({"Resampling", selection.kind == model::InputKind::Resampling, {model::InputKind::Resampling, 0, {}}, ""});
        choices->push_back({"No Input", selection.kind == model::InputKind::None, {model::InputKind::None, 0, {}}, ""});
    }
    else if (midi && selection.kind == model::InputKind::Midi)
    {
        choices->push_back({"All Channels", selection.channel == 0, {model::InputKind::Midi, 0, selection.device}, ""});
        for (int channel = 1; channel <= 16; ++channel)
            choices->push_back({"Ch. " + std::to_string(channel), selection.channel == channel, {model::InputKind::Midi, channel, selection.device}, ""});
    }
    else if (selection.kind == model::InputKind::AudioMono || selection.kind == model::InputKind::AudioStereo)
    {
        const int count = std::clamp(ctx().audioInputChannelCount ? ctx().audioInputChannelCount() : 2, 1, 64);
        for (int channel = 0; channel < count; ++channel)
            choices->push_back({std::to_string(channel + 1), selection.kind == model::InputKind::AudioMono && selection.channel == channel,
                                {model::InputKind::AudioMono, channel, {}}, ""});
        for (int channel = 0; channel + 1 < count; channel += 2)
            choices->push_back({std::to_string(channel + 1) + "/" + std::to_string(channel + 2),
                                selection.kind == model::InputKind::AudioStereo && selection.channel == channel,
                                {model::InputKind::AudioStereo, channel, {}}, ""});
    }
    if (choices->empty())
        return;
    if (ctx().showChoices)
    {
        std::vector<std::string> labels;
        std::vector<bool> ticked;
        for (const auto& choice : *choices) { labels.push_back(choice.label); ticked.push_back(choice.ticked); }
        ctx().showChoices(labels, ticked, [this, choices](int index) {
            if (Widget::isAlive(this) && index >= 0 && (size_t) index < choices->size())
                applySelection((*choices)[(size_t) index].selection, (*choices)[(size_t) index].display);
        });
        return;
    }
    juce::PopupMenu menu;
    for (size_t i = 0; i < choices->size(); ++i)
        menu.addItem((int) i + 1, (*choices)[i].label, true, (*choices)[i].ticked);
    menu.showMenuAsync(juce::PopupMenu::Options(), [this, choices](int result) {
        if (! Widget::isAlive(this) || result < 1 || (size_t) result > choices->size()) return;
        const auto& picked = (*choices)[(size_t) result - 1];
        applySelection(picked.selection, picked.display);
    });
}
} // namespace sonora::ui
