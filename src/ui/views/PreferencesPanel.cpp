#include "ui/views/PreferencesPanel.h"

#include "engine/Transport.h"
#include "ui/Painter.h"
#include "ui/Shortcuts.h"
#include "ui/UiContext.h"
#include "ui/WheelScale.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <cmath>

namespace sonora::ui
{
namespace
{
using Tab = PreferencesPanel::Tab;
constexpr Tab pages[] = {Tab::Display, Tab::Theme, Tab::Audio, Tab::Midi, Tab::Files, Tab::Library, Tab::Plugins, Tab::Record, Tab::Keys, Tab::Updates};
constexpr const char* names[] = {"Display & Input", "Theme & Colors", "Audio", "Tempo & MIDI", "File & Folder", "Library", "Plug-Ins", "Record, Warp & Launch", "Keyboard Shortcuts", "Updates"};
constexpr int pageCount = (int) std::size(pages);
constexpr const char* quantNames[] = {"None", "8 Bars", "4 Bars", "2 Bars", "1 Bar", "1/2", "1/2T", "1/4", "1/4T", "1/8", "1/8T", "1/16", "1/16T", "1/32"};
std::string number(double value, int decimals = 0) { return juce::String(value, decimals).toStdString(); }
}

Rect PreferencesPanel::panelRect() const
{
    const float w = std::max(0.0f, std::min(640.0f, width() - 32));
    const float h = std::max(0.0f, std::min(560.0f, height() - 32));
    return {(width() - w) * 0.5f, (height() - h) * 0.5f, w, h};
}

Rect PreferencesPanel::pageBounds(Tab page) const
{
    const auto r = panelRect();
    for (int i = 0; i < pageCount; ++i)
        if (pages[i] == page) return {r.x, r.y + 42 + (float) i * 28, 148, 28};
    return {};
}

Rect PreferencesPanel::controlBounds(const std::string& label) const
{
    for (const auto& hit : hits_)
        if (hit.label == label && (! hit.body || (hit.area.y >= bodyClip_.y && hit.area.bottom() <= bodyClip_.bottom())))
            return hit.area;
    return {};
}

void PreferencesPanel::setTab(Tab page)
{
    ctx().settingsPage = static_cast<int>(page);
    scroll_ = 0;
    recording_ = 0;
    hits_.clear();
    repaint();
}

void PreferencesPanel::button(SkCanvas& c, const Theme& t, Rect r, const std::string& key, const std::string& label,
                              bool on, std::function<void()> action)
{
    const bool enabled = static_cast<bool>(action);
    paint::fill(c, r, on ? t.color("ChosenDefault") : t.color("ControlTextBack"));
    paint::stroke(c, r, t.color("ControlContrastFrame"));
    paint::text(c, label, r.reduced(5, 0), ! enabled ? t.color("TextDisabled") : on ? paint::contrastText(t.color("ChosenDefault"))
                                                                                 : t.color("ControlForeground"),
                t.metrics.fontSize, Align::Centre);
    if (enabled) hits_.push_back({r, key, std::move(action), inBody_});
}

void PreferencesPanel::choice(SkCanvas& c, const Theme& t, Rect r, const std::string& key, const std::string& label,
                              std::vector<std::string> choices, std::function<void(int)> selected)
{
    auto action = choices.empty() || ! selected ? std::function<void()>{} : [this, choices, selected, label] {
        juce::PopupMenu menu;
        for (size_t i = 0; i < choices.size(); ++i)
            menu.addItem((int) i + 1, juce::String::fromUTF8(choices[i].c_str()), true, choices[i] == label);
        menu.showMenuAsync(juce::PopupMenu::Options(), [this, selected](int result) {
            if (result > 0 && Widget::isAlive(this)) { selected(result - 1); repaint(); }
        });
    };
    button(c, t, r, key, label, false, std::move(action));
    paint::line(c, r.right() - 11, r.centreY() - 1, r.right() - 7, r.centreY() + 3, t.color("ControlForeground"));
    paint::line(c, r.right() - 7, r.centreY() + 3, r.right() - 3, r.centreY() - 1, t.color("ControlForeground"));
}

void PreferencesPanel::paint(SkCanvas& c, const Theme& t)
{
    hits_.clear(); inBody_ = false;
    auto& x = ctx();
    x.settingsPage = std::clamp(x.settingsPage, 0, (int) Tab::Updates);
    if (paintedPage_ != x.settingsPage) { scroll_ = 0; paintedPage_ = x.settingsPage; }
    const auto tab = static_cast<Tab>(x.settingsPage);
    paint::fill(c, localBounds(), paint::withAlpha(SkColorSetRGB(0, 0, 0), 100));
    const auto panel = panelRect();
    paint::fillRounded(c, panel, 8, t.color("SurfaceBackground"));
    paint::stroke(c, panel, t.color("ControlContrastFrame"));
    const Rect title{panel.x, panel.y, panel.w, 32};
    paint::fill(c, title, t.color("Desktop"));
    paint::text(c, "Settings", title, paint::contrastText(t.color("Desktop")), t.metrics.fontSize + 2, Align::Centre, true);
    button(c, t, {panel.right() - 29, panel.y + 6, 20, 20}, "Close", "x", false, [&x] {
        x.preferencesOpen = false; if (x.modelChanged) x.modelChanged();
    });
    for (int i = 0; i < pageCount; ++i)
    {
        const auto r = pageBounds(pages[i]);
        if (pages[i] == tab) paint::fill(c, r, t.color("SurfaceHighlight"));
        paint::text(c, names[i], r.reduced(12, 0), t.color("ControlForeground"), t.metrics.fontSize, Align::Left, true);
        hits_.push_back({r, names[i], [this, i] { setTab(pages[i]); }, false});
    }
    paint::fill(c, {panel.x + 148, title.bottom(), 1, panel.h - title.h}, t.color("SurfaceArea"));
    bodyClip_ = {panel.x + 164, title.bottom() + 12, panel.w - 180, panel.h - 56};
    c.save(); c.clipRect(bodyClip_.sk()); inBody_ = true;
    const auto fg = t.color("ControlForeground"), dim = t.color("TextDisabled");
    const float fs = t.metrics.fontSize;
    float y = bodyClip_.y - scroll_;
    const float labelWidth = std::min(238.0f, bodyClip_.w * 0.51f);
    auto row = [&](float h = 26.0f) { Rect r{bodyClip_.x, y, bodyClip_.w, h}; y += h; return r; };
    auto section = [&](const std::string& label) {
        if (y > bodyClip_.y - scroll_) y += 14;
        const auto r = row(24);
        paint::text(c, label, r, fg, fs + 0.5f, Align::Left, true);
        const float start = std::min(r.right(), r.x + (float) label.size() * fs * 0.58f + 12);
        paint::fill(c, {start, r.centreY(), std::max(0.0f, r.right() - start), 1}, t.color("SurfaceArea"));
    };
    auto field = [&](const std::string& label) {
        auto r = row(); paint::text(c, label, r.withWidth(labelWidth), fg, fs);
        return Rect{r.x + labelWidth, r.y + 3, r.w - labelWidth - 4, 19};
    };
    auto value = [&](const std::string& label, const std::string& text) {
        paint::text(c, text, field(label), dim, fs);
    };
    auto toggle = [&](const std::string& label, bool on, std::function<void()> action) {
        button(c, t, field(label).withWidth(72), label, on ? "On" : "Off", on, std::move(action));
    };
    auto reveal = [&](const std::string& label, const std::string& path) {
        auto r = field(label);
        button(c, t, r.removeFromRight(62), label, "Show", false, ! path.empty() && x.revealInFinder ? std::function<void()>([&x, path] { x.revealInFinder(path); }) : nullptr);
        paint::text(c, path.empty() ? "Not set" : path, r.reduced(0, 0), dim, fs);
    };

    if (tab == Tab::Display)
    {
        section("Display"); value("Language", "English");
        auto z = field("Zoom");
        button(c, t, z.removeFromLeft(26), "Zoom Out", "-", false, [&x] { if (x.setUiZoom) x.setUiZoom(std::max(0.8f, x.uiZoom - 0.1f)); });
        button(c, t, z.removeFromRight(26), "Zoom In", "+", false, [&x] { if (x.setUiZoom) x.setUiZoom(std::min(2.0f, x.uiZoom + 0.1f)); });
        choice(c, t, z.reduced(3, 0), "Zoom", number(std::round(x.uiZoom * 100)) + " %",
               {"80 %", "90 %", "100 %", "110 %", "125 %", "150 %", "200 %"}, [&x](int i) {
                   constexpr float zooms[] = {0.8f, 0.9f, 1, 1.1f, 1.25f, 1.5f, 2}; if (x.setUiZoom) x.setUiZoom(zooms[i]);
               });
        button(c, t, field("Reset Zoom").withWidth(120), "Reset Zoom", "100 %", false, [&x] { if (x.setUiZoom) x.setUiZoom(1); });
        section("Keyboard");
        toggle("Computer MIDI Keyboard", x.computerKeyboard, [&x] { x.computerKeyboard = ! x.computerKeyboard; });
        value("Keyboard Octave", "C" + std::to_string(x.computerKeyboardOctave));
        value("Keyboard Velocity", std::to_string(x.computerKeyboardVelocity));
    }
    else if (tab == Tab::Theme)
    {
        section("Theme"); value("Theme", t.sourceName);
        auto appearance = field("Appearance");
        button(c, t, appearance.removeFromLeft(80), "Light", "Light", t.sourceName == "Built-in Light", [&x] { if (x.useBuiltinTheme) x.useBuiltinTheme(false); });
        appearance.removeFromLeft(6);
        button(c, t, appearance.removeFromLeft(80), "Dark", "Dark", t.sourceName == "Built-in Dark", [&x] { if (x.useBuiltinTheme) x.useBuiltinTheme(true); });
        button(c, t, field("Custom Theme").withWidth(140), "Load Theme", "Browse...", false, x.loadTheme);
        section("Track and Clip Colors");
        auto r = row(52);
        for (int i = 0; i < 28; ++i)
            paint::fill(c, {r.x + (float) (i % 14) * 22, r.y + (float) (i / 14) * 22, 18, 18}, Theme::paletteColor(i));
    }
    else if (tab == Tab::Audio)
    {
        const auto state = x.audioSettings ? x.audioSettings() : UiContext::AudioSettings{};
        section("Audio Device"); value("Driver Type", state.driver.empty() ? "No audio device" : state.driver);
        value("Audio Input Device", state.input.empty() ? "No Device" : state.input);
        choice(c, t, field("Audio Output Device"), "Audio Output Device", state.output.empty() ? "No Device" : state.output,
               state.outputs, [&x, outputs = state.outputs](int i) { if (x.setAudioSetting) x.setAudioSetting(UiContext::AudioSetting::Output, outputs[(size_t) i]); });
        button(c, t, field("Channel Configuration"), "Channel Configuration", "Input / Output Config...", false, x.showAudioSettings);
        section("Sample Rate");
        std::vector<std::string> rates; for (double rate : state.sampleRates) rates.push_back(number(rate));
        choice(c, t, field("In/Out Sample Rate"), "In/Out Sample Rate", state.sampleRate > 0 ? number(state.sampleRate) : "Unavailable", rates,
               [&x, rates](int i) { if (x.setAudioSetting) x.setAudioSetting(UiContext::AudioSetting::SampleRate, rates[(size_t) i]); });
        section("Latency");
        std::vector<std::string> sizes; for (int size : state.bufferSizes) sizes.push_back(std::to_string(size));
        choice(c, t, field("Buffer Size"), "Buffer Size", std::to_string(state.bufferSize) + " Samples", sizes,
               [&x, sizes](int i) { if (x.setAudioSetting) x.setAudioSetting(UiContext::AudioSetting::BufferSize, sizes[(size_t) i]); });
        value("Input Latency", number(state.inputLatencyMs, 2) + " ms");
        value("Output Latency", number(state.outputLatencyMs, 2) + " ms");
        value("Overall Latency", number(state.inputLatencyMs + state.outputLatencyMs, 2) + " ms");
    }
    else if (tab == Tab::Midi)
    {
        section("Input Ports");
        auto head = row(20);
        paint::text(c, "MIDI Port", head, fg, fs, Align::Left, true);
        paint::text(c, "Track", {head.right() - 128, head.y, 58, head.h}, fg, fs, Align::Centre, true);
        paint::text(c, "Remote", {head.right() - 64, head.y, 58, head.h}, fg, fs, Align::Centre, true);
        const auto inputs = x.midiInputRows ? x.midiInputRows() : std::vector<UiContext::MidiInputRow>{};
        if (inputs.empty()) paint::text(c, "No MIDI inputs connected", row(), dim, fs);
        for (const auto& input : inputs)
        {
            auto r = row();
            paint::text(c, input.name, r.withWidth(r.w - 140), fg, fs);
            button(c, t, {r.right() - 128, r.y + 3, 58, 19}, "Track " + input.id, input.track ? "On" : "Off", input.track, [&x, input] {
                if (x.setMidiInputRoles) x.setMidiInputRoles(input.id, ! input.track, input.remote);
            });
            button(c, t, {r.right() - 64, r.y + 3, 58, 19}, "Remote " + input.id, input.remote ? "On" : "Off", input.remote, [&x, input] {
                if (x.setMidiInputRoles) x.setMidiInputRoles(input.id, input.track, ! input.remote);
            });
        }
        section("Output Ports");
        head = row(20); paint::text(c, "MIDI Port", head, fg, fs, Align::Left, true);
        paint::text(c, "Sync", {head.right() - 64, head.y, 58, head.h}, fg, fs, Align::Centre, true);
        const auto outputs = x.midiOutputRows ? x.midiOutputRows() : std::vector<UiContext::MidiOutputRow>{};
        if (outputs.empty()) paint::text(c, "No MIDI outputs connected", row(), dim, fs);
        for (const auto& output : outputs)
        {
            auto r = row(); paint::text(c, output.name, r.withWidth(r.w - 75), fg, fs);
            const bool on = x.midiClockEnabled && x.midiClockOutputId == output.id;
            button(c, t, {r.right() - 64, r.y + 3, 58, 19}, "Sync " + output.id, on ? "On" : "Off", on, [&x, output, on] {
                if (x.setMidiClock) x.setMidiClock(output.id, ! on);
            });
        }
    }
    else if (tab == Tab::Plugins)
    {
        section("Plug-In Sources");
        button(c, t, field("Rescan Plug-Ins"), "Rescan Plug-Ins", x.scanning ? "Scanning..." : "Rescan", x.scanning,
               x.scanning ? nullptr : x.scanPlugins);
        button(c, t, field("Audio Units"), "Scan Audio Units", "Rescan VST3 + AU", false, x.scanning ? nullptr : x.scanAudioUnits);
        for (size_t i = 0; i < x.pluginFolders.size(); ++i)
            reveal("VST3 System Folder " + std::to_string(i + 1), x.pluginFolders[i]);
        value("Plug-Ins Found", std::to_string(x.pluginNames.size()));
        section("Failed Plug-Ins");
        button(c, t, field("Retry Failed Plug-Ins"), "Retry Failed Plug-Ins", "Retry", false, x.scanning ? nullptr : std::function<void()>([&x] {
            if (x.clearFailedPlugins) x.clearFailedPlugins(); if (x.scanAudioUnits) x.scanAudioUnits(); else if (x.scanPlugins) x.scanPlugins();
        }));
        if (x.failedPlugins.empty()) paint::text(c, "No failed plug-ins", row(), dim, fs);
        for (const auto& path : x.failedPlugins)
            paint::text(c, juce::File(juce::String::fromUTF8(path.c_str())).getFileName().toStdString(), row(20), t.color("ChosenAlert"), fs);
    }
    else if (tab == Tab::Files)
    {
        section("Data Handling");
        reveal("Temporary Projects", x.temporaryProjectPath);
        reveal("Settings Folder", x.settingsPath);
        reveal("Plug-In Cache", x.pluginCachePath);
    }
    else if (tab == Tab::Library)
    {
        section("Content Locations");
        auto location = [&](const std::string& label, const std::string& path, bool packs) {
            auto r = field(label);
            button(c, t, r.removeFromRight(70), label, "Browse...", false, x.chooseLibraryFolder ? std::function<void()>([&x, packs] { x.chooseLibraryFolder(packs); }) : nullptr);
            paint::text(c, path, r, dim, fs);
        };
        location("Location of User Library", x.userLibraryPath, false); location("Folder for Packs", x.packsPath, true);
        section("Places");
        for (const auto& path : x.browserPlaces) reveal(juce::File(juce::String::fromUTF8(path.c_str())).getFileName().toStdString(), path);
        button(c, t, field("Add Folder to Browser"), "Add Folder to Browser", "Add Folder...", false, x.addBrowserFolder);
    }
    else if (tab == Tab::Keys)
        paintShortcuts(c, t, y);
    else if (tab == Tab::Updates)
    {
        const auto state = x.updateSettings ? x.updateSettings() : UiContext::UpdateSettings{.reason = "This build has no updater"};
        section("Application Updates");
        value("Version", state.version.empty() ? "Unknown" : state.version);
        if (! state.available)
            value("Updates", state.reason);
        else
        {
            toggle("Check Automatically", state.automatic, [&x, on = state.automatic] { if (x.setAutomaticUpdateChecks) x.setAutomaticUpdateChecks(! on); });
            if (state.channels.size() > 1)
                choice(c, t, field("Update Channel").withWidth(140), "Update Channel", state.channels[(size_t) std::clamp(state.channel, 0, (int) state.channels.size() - 1)],
                       state.channels, [&x](int i) { if (x.setUpdateChannel) x.setUpdateChannel(i); });
            else if (! state.channels.empty())
                value("Update Channel", state.channels.front());
            const bool enabled = state.canCheck || state.pending;
            button(c, t, field("Check for Updates").withWidth(140), "Check for Updates",
                   state.pending ? "Install and Restart..." : "Check Now...", false,
                   enabled ? x.checkForUpdates : nullptr);
            value("Updater", state.backend);
        }
        section("Safe Restart");
        paint::text(c, "Sonora never restarts for an update while playing, recording or rendering.", row(20), dim, fs);
        paint::text(c, "Unsaved changes are offered a save before Sonora restarts.", row(20), dim, fs);
    }
    else
    {
        section("Record");
        choice(c, t, field("Count-In"), "Count-In", x.countInBars == 0 ? "None" : x.countInBars == 1 ? "1 Bar" : std::to_string(x.countInBars) + " Bars",
               {"None", "1 Bar", "2 Bars", "4 Bars"}, [&x](int i) { constexpr int bars[] = {0, 1, 2, 4}; x.countInBars = bars[i]; if (x.setCountInBars) x.setCountInBars(bars[i]); });
        toggle("MIDI Arrangement Overdub", x.arrangementOverdub, [&x] { x.arrangementOverdub = ! x.arrangementOverdub; if (x.layoutChanged) x.layoutChanged(); });
        toggle("Automation Arm", x.automationArm, [&x] { x.automationArm = ! x.automationArm; if (x.layoutChanged) x.layoutChanged(); });
        section("Launch");
        std::vector<std::string> quants(std::begin(quantNames), std::end(quantNames));
        choice(c, t, field("Global Launch Quantization"), "Global Launch Quantization", x.song ? quantNames[std::clamp(x.song->globalQuantisation, 0, 13)] : "1 Bar",
               x.song ? quants : std::vector<std::string>{}, [&x](int i) { if (x.song && x.applyEdit) x.applyEdit(model::setGlobalQuantisation(*x.song, i)); });
        section("Browser");
        toggle("Browser File Preview", x.browserPreviewEnabled, [&x] {
            x.browserPreviewEnabled = ! x.browserPreviewEnabled; if (! x.browserPreviewEnabled && x.previewSample) x.previewSample("");
            if (x.layoutChanged) x.layoutChanged();
        });
    }
    contentHeight_ = y - bodyClip_.y + scroll_;
    c.restore(); inBody_ = false;
    if (contentHeight_ > bodyClip_.h)
    {
        const float h = std::max(18.0f, bodyClip_.h * bodyClip_.h / contentHeight_);
        const float top = bodyClip_.y + (bodyClip_.h - h) * scroll_ / std::max(1.0f, contentHeight_ - bodyClip_.h);
        paint::fillRounded(c, {panel.right() - 8, top, 3, h}, 1.5f, t.color("ScrollbarInnerHandle"));
    }
}

void PreferencesPanel::mouseDown(const MouseEvent& e)
{
    for (const auto& hit : hits_)
        if (hit.area.contains(e.x, e.y) && (! hit.body || bodyClip_.contains(e.x, e.y)))
        {
            auto action = hit.action; action(); repaint(); return;
        }
}

bool PreferencesPanel::mouseWheel(const MouseEvent& e)
{
    if (bodyClip_.contains(e.x, e.y)) scroll_ = std::clamp(scroll_ - e.wheelDeltaY * kWheelPixels, 0.0f, std::max(0.0f, contentHeight_ - bodyClip_.h));
    repaint(); return true;
}

void PreferencesPanel::paintShortcuts(SkCanvas& c, const Theme& t, float& y)
{
    auto& x = ctx();
    const auto fg = t.color("ControlForeground"), dim = t.color("TextDisabled");
    const float fs = t.metrics.fontSize;
    auto row = [&](float h = 24.0f) { Rect r{bodyClip_.x, y, bodyClip_.w, h}; y += h; return r; };
    auto* map = x.shortcuts;
    if (! map) { paint::text(c, "Keyboard shortcuts are not available here", row(), dim, fs); return; }
    auto head = row(26);
    paint::text(c, filter_.empty() ? "Type to filter commands" : "Filter: " + filter_, head.withWidth(head.w - 96), filter_.empty() ? dim : fg, fs);
    button(c, t, {head.right() - 90, head.y + 3, 86, 19}, "Reset All", "Reset All", false, [this, &x, map] {
        map->resetAll(); recording_ = 0; shortcutStatus_ = "All shortcuts restored";
        if (x.shortcutsChanged) x.shortcutsChanged();
    });
    const std::string hint = recording_ != 0 ? "Press the new shortcut. Delete clears it, Esc cancels."
                                             : ! shortcutStatus_.empty() ? shortcutStatus_ : "Click a shortcut to change it";
    paint::text(c, hint, row(20), recording_ != 0 ? t.color("ChosenAlert") : dim, fs);
    const auto lower = [](std::string s) { for (auto& ch : s) ch = (char) std::tolower((unsigned char) ch); return s; };
    const auto needle = lower(filter_);
    std::string menu;
    for (const auto& command : map->commands())
    {
        if (! needle.empty() && lower(command.name).find(needle) == std::string::npos && lower(command.menu).find(needle) == std::string::npos)
            continue;
        if (command.menu != menu)
        {
            menu = command.menu;
            y += 6;
            const auto r = row(20);
            paint::text(c, menu, r, fg, fs + 0.5f, Align::Left, true);
            paint::fill(c, {r.x, r.bottom() - 1, r.w, 1}, t.color("SurfaceArea"));
        }
        const auto r = row();
        const bool custom = map->isCustom(command.id);
        paint::text(c, command.name, r.withWidth(r.w - 190), fg, fs);
        const auto key = map->keyFor(command.id);
        const std::string label = recording_ == command.id ? "Press keys..." : key ? key->text() : "None";
        const int id = command.id;
        button(c, t, {r.right() - 186, r.y + 3, 124, 19}, "Shortcut " + command.storageName(), label, custom || recording_ == id,
               [this, id] { recording_ = recording_ == id ? 0 : id; shortcutStatus_.clear(); });
        if (custom)
            button(c, t, {r.right() - 56, r.y + 3, 52, 19}, "Reset " + command.storageName(), "Reset", false, [this, &x, map, id] {
                map->reset(id); recording_ = 0; shortcutStatus_.clear();
                if (x.shortcutsChanged) x.shortcutsChanged();
            });
        else if (command.defaultKey)
            paint::text(c, "Default", {r.right() - 56, r.y, 52, r.h}, dim, fs, Align::Centre);
    }
}

bool PreferencesPanel::shortcutKey(const KeyEvent& k)
{
    auto& x = ctx();
    auto* map = x.shortcuts;
    if (! map) return false;
    if (recording_ != 0)
    {
        const int id = recording_;
        const bool plain = ! k.cmd && ! k.alt && ! k.ctrl && ! k.shift;
        if (k.keyCode == juce::KeyPress::escapeKey && plain) { recording_ = 0; repaint(); return true; }
        const auto* command = map->command(id);
        if (! command) { recording_ = 0; return true; }
        if (plain && (k.keyCode == juce::KeyPress::backspaceKey || k.keyCode == juce::KeyPress::deleteKey))
        {
            map->assign(id, std::nullopt);
            shortcutStatus_ = command->name + ": no shortcut";
        }
        else
        {
            const auto combo = KeyCombo::fromEvent(k);
            if (! combo.valid() || combo.reserved())
            {
                shortcutStatus_ = (combo.valid() ? combo.text() : std::string("That key")) + " is reserved for editing; add Cmd, Option or Control";
                recording_ = 0; repaint(); return true;
            }
            const int displaced = map->assign(id, combo);
            shortcutStatus_ = command->name + ": " + combo.text();
            if (const auto* other = displaced ? map->command(displaced) : nullptr) shortcutStatus_ += " (removed from " + other->name + ")";
            else if (! combo.cmd && ! combo.alt && ! combo.ctrl) shortcutStatus_ += " (now overrides that key in every view)";
        }
        recording_ = 0;
        if (x.shortcutsChanged) x.shortcutsChanged();
        repaint();
        return true;
    }
    if (k.cmd || k.alt || k.ctrl) return false;
    if (k.keyCode == juce::KeyPress::backspaceKey || k.keyCode == juce::KeyPress::deleteKey)
    {
        if (! filter_.empty()) filter_.pop_back();
        scroll_ = 0; repaint(); return true;
    }
    if (k.textChar >= 32 && k.textChar < 127 && k.keyCode != juce::KeyPress::escapeKey)
    {
        filter_ += (char) k.textChar;
        scroll_ = 0; repaint(); return true;
    }
    return false;
}

bool PreferencesPanel::keyPressed(const KeyEvent& k)
{
    if (static_cast<Tab>(ctx().settingsPage) == Tab::Keys && shortcutKey(k)) return true;
    if (k.keyCode == juce::KeyPress::escapeKey || (k.cmd && k.textChar == ','))
    { ctx().preferencesOpen = false; if (ctx().modelChanged) ctx().modelChanged(); }
    return true;
}
} // namespace sonora::ui
