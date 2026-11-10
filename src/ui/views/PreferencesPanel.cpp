#include "ui/views/PreferencesPanel.h"

#include "engine/Transport.h"
#include "gfx/Brand.h"
#include "ui/Painter.h"
#include "ui/Shortcuts.h"
#include "ui/UiContext.h"
#include "ui/WheelScale.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include "include/core/SkRRect.h"
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

// UI-63: the panel is laid out in points like the prompts; zoom (UI-11) scales the whole canvas.
constexpr float maxWidth = 680, maxHeight = 580, headerHeight = 56, sidebarWidth = 184, pageRowHeight = 30;
constexpr float rowHeight = 30, controlHeight = 24, controlText = 12, switchWidth = 38, switchHeight = 22;
namespace style = gfx::brand::panel;
}

Rect PreferencesPanel::panelRect() const
{
    const float w = std::max(0.0f, std::min(maxWidth, width() - 32));
    const float h = std::max(0.0f, std::min(maxHeight, height() - 32));
    return {(width() - w) * 0.5f, (height() - h) * 0.5f, w, h};
}

Rect PreferencesPanel::pageBounds(Tab page) const
{
    const auto r = panelRect();
    for (int i = 0; i < pageCount; ++i)
        if (pages[i] == page) return {r.x + 10, r.y + headerHeight + 10 + (float) i * pageRowHeight, sidebarWidth - 20, pageRowHeight - 2};
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

bool PreferencesPanel::hot(const Rect& r) const
{
    return r.contains(hoverX_, hoverY_) && (! inBody_ || bodyClip_.contains(hoverX_, hoverY_));
}

void PreferencesPanel::button(SkCanvas& c, Rect r, const std::string& key, const std::string& label, bool on, std::function<void()> action)
{
    const bool enabled = static_cast<bool>(action);
    gfx::brand::drawButton(c, r.sk(), label, {.primary = on, .hot = enabled && hot(r), .enabled = enabled}, controlText);
    if (enabled) hits_.push_back({r, key, std::move(action), inBody_});
}

void PreferencesPanel::choice(SkCanvas& c, Rect r, const std::string& key, const std::string& label,
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
    // A pop-up button: the secondary button with its value on the left and a chevron on the right.
    const gfx::brand::ButtonLook look{.hot = action && hot(r), .enabled = static_cast<bool>(action)};
    gfx::brand::drawButtonFrame(c, r.sk(), look);
    paint::text(c, label, {r.x + 9, r.y, r.w - 30, r.h}, gfx::brand::buttonTextColor(look), controlText);
    const auto arrow = look.enabled ? style::message : style::dim;
    paint::line(c, r.right() - 15, r.centreY() - 2, r.right() - 11.5f, r.centreY() + 1.5f, arrow, 1.5f);
    paint::line(c, r.right() - 11.5f, r.centreY() + 1.5f, r.right() - 8, r.centreY() - 2, arrow, 1.5f);
    if (action) hits_.push_back({r, key, std::move(action), inBody_});
}

void PreferencesPanel::toggle(SkCanvas& c, Rect r, const std::string& key, bool on, std::function<void()> action)
{
    const Rect knob{r.x, r.centreY() - switchHeight / 2, switchWidth, switchHeight};
    gfx::brand::drawSwitch(c, knob.sk(), on, action && hot(knob), static_cast<bool>(action));
    if (action) hits_.push_back({knob, key, std::move(action), inBody_});
}

void PreferencesPanel::paint(SkCanvas& c, const Theme& t)
{
    hits_.clear(); inBody_ = false;
    auto& x = ctx();
    x.settingsPage = std::clamp(x.settingsPage, 0, (int) Tab::Updates);
    if (paintedPage_ != x.settingsPage) { scroll_ = 0; paintedPage_ = x.settingsPage; }
    const auto tab = static_cast<Tab>(x.settingsPage);
    paint::fill(c, localBounds(), paint::withAlpha(SkColorSetRGB(0, 0, 0), 100));

    // Window chrome: the prompts' panel with the app icon and a bold title, the ghost S running off the bottom right.
    const auto panel = panelRect();
    gfx::brand::drawPanel(c, panel.sk());
    c.save();
    c.clipRRect(SkRRect::MakeRectXY(panel.sk(), style::cornerRadius, style::cornerRadius), true);
    {
        const float h = panel.h * 0.95f;
        const SkRect box = SkRect::MakeXYWH(panel.right() - h * gfx::brand::markAspect * 0.7f, panel.y + panel.h * 0.32f,
                                            h * gfx::brand::markAspect, h);
        gfx::brand::drawGhostMark(c, box, panel.bottom(), 12, 3);
    }
    const Rect header{panel.x, panel.y, panel.w, headerHeight};
    {
        // The 1024 icon grid has a 100-unit margin, so the canvas is drawn larger than the visible tile.
        constexpr float icon = 28, iconCanvas = icon * 1024.0f / 824.0f;
        c.save();
        c.translate(header.x + 18 - (iconCanvas - icon) / 2, header.centreY() - iconCanvas / 2);
        gfx::brand::drawAppIcon(c, iconCanvas);
        c.restore();
    }
    paint::text(c, "Settings", {header.x + 58, header.y, 200, header.h}, style::title, style::titleSize, Align::Left, true);
    {
        const Rect close{header.right() - 42, header.centreY() - 12, 24, 24};
        button(c, close, "Close", "", false, [&x] { x.preferencesOpen = false; if (x.modelChanged) x.modelChanged(); });
        const float d = 4;
        paint::line(c, close.centreX() - d, close.centreY() - d, close.centreX() + d, close.centreY() + d, style::buttonText, 1.5f);
        paint::line(c, close.centreX() + d, close.centreY() - d, close.centreX() - d, close.centreY() + d, style::buttonText, 1.5f);
    }
    paint::fill(c, {panel.x, header.bottom(), panel.w, 1}, style::separator);
    paint::fill(c, {panel.x, header.bottom() + 1, sidebarWidth, panel.h - header.h - 1}, style::sidebar);
    paint::fill(c, {panel.x + sidebarWidth, header.bottom() + 1, 1, panel.h - header.h - 1}, style::separator);
    c.restore();

    for (int i = 0; i < pageCount; ++i)
    {
        const auto r = pageBounds(pages[i]);
        const bool chosen = pages[i] == tab;
        if (chosen || hot(r))
            paint::fillRounded(c, r, std::min(style::buttonRadius, r.h * 0.25f), chosen ? style::selection : paint::withAlpha(style::selection, 14));
        paint::text(c, names[i], r.reduced(10, 0), chosen ? style::title : style::text, style::textSize);
        hits_.push_back({r, names[i], [this, i] { setTab(pages[i]); }, false});
    }
    bodyClip_ = {panel.x + sidebarWidth + 22, header.bottom() + 12, panel.w - sidebarWidth - 44, panel.h - header.h - 24};
    c.save(); c.clipRect(bodyClip_.sk()); inBody_ = true;
    const auto fg = style::text, dim = style::message;
    const float fs = style::textSize;
    float y = bodyClip_.y - scroll_;
    const float labelWidth = std::min(238.0f, bodyClip_.w * 0.5f);
    auto row = [&](float h = rowHeight) { Rect r{bodyClip_.x, y, bodyClip_.w, h}; y += h; return r; };
    auto section = [&](const std::string& label) {
        if (y > bodyClip_.y - scroll_) y += 16;
        const auto r = row(28);
        paint::text(c, label, r, style::title, fs, Align::Left, true);
        const float start = std::min(r.right(), r.x + (float) label.size() * fs * 0.6f + 14);
        paint::fill(c, {start, r.centreY(), std::max(0.0f, r.right() - start), 1}, style::separator);
    };
    auto field = [&](const std::string& label) {
        auto r = row(); paint::text(c, label, r.withWidth(labelWidth), fg, fs);
        return Rect{r.x + labelWidth, r.y + (rowHeight - controlHeight) / 2, r.w - labelWidth, controlHeight};
    };
    auto value = [&](const std::string& label, const std::string& text) {
        paint::text(c, text, field(label), dim, fs);
    };
    auto onOff = [&](const std::string& label, bool on, std::function<void()> action) {
        toggle(c, field(label), label, on, std::move(action));
    };
    auto reveal = [&](const std::string& label, const std::string& path) {
        auto r = field(label);
        button(c, r.removeFromRight(64), label, "Show", false, ! path.empty() && x.revealInFinder ? std::function<void()>([&x, path] { x.revealInFinder(path); }) : nullptr);
        r.removeFromRight(8);
        paint::text(c, path.empty() ? "Not set" : path, r, dim, fs);
    };
    auto note = [&](const std::string& text, SkColor color) { paint::text(c, text, row(22), color, fs); };

    if (tab == Tab::Display)
    {
        section("Display"); value("Language", "English");
        auto z = field("Zoom");
        button(c, z.removeFromLeft(30), "Zoom Out", "-", false, [&x] { if (x.setUiZoom) x.setUiZoom(std::max(0.8f, x.uiZoom - 0.1f)); });
        button(c, z.removeFromRight(30), "Zoom In", "+", false, [&x] { if (x.setUiZoom) x.setUiZoom(std::min(2.0f, x.uiZoom + 0.1f)); });
        choice(c, z.reduced(6, 0), "Zoom", number(std::round(x.uiZoom * 100)) + " %",
               {"80 %", "90 %", "100 %", "110 %", "125 %", "150 %", "200 %"}, [&x](int i) {
                   constexpr float zooms[] = {0.8f, 0.9f, 1, 1.1f, 1.25f, 1.5f, 2}; if (x.setUiZoom) x.setUiZoom(zooms[i]);
               });
        button(c, field("Reset Zoom").withWidth(120), "Reset Zoom", "100 %", false, [&x] { if (x.setUiZoom) x.setUiZoom(1); });
        section("Keyboard");
        onOff("Computer MIDI Keyboard", x.computerKeyboard, [&x] { x.computerKeyboard = ! x.computerKeyboard; });
        value("Keyboard Octave", "C" + std::to_string(x.computerKeyboardOctave));
        value("Keyboard Velocity", std::to_string(x.computerKeyboardVelocity));
    }
    else if (tab == Tab::Theme)
    {
        section("Theme"); value("Theme", t.sourceName);
        auto appearance = field("Appearance");
        button(c, appearance.removeFromLeft(84), "Light", "Light", t.sourceName == "Built-in Light", [&x] { if (x.useBuiltinTheme) x.useBuiltinTheme(false); });
        appearance.removeFromLeft(8);
        button(c, appearance.removeFromLeft(84), "Dark", "Dark", t.sourceName == "Built-in Dark", [&x] { if (x.useBuiltinTheme) x.useBuiltinTheme(true); });
        button(c, field("Custom Theme").withWidth(140), "Load Theme", "Browse...", false, x.loadTheme);
        section("Track and Clip Colors");
        auto r = row(56);
        r.y += 4;
        for (int i = 0; i < 28; ++i)
            paint::fillRounded(c, {r.x + (float) (i % 14) * 22, r.y + (float) (i / 14) * 22, 18, 18}, 4, Theme::paletteColor(i));
    }
    else if (tab == Tab::Audio)
    {
        const auto state = x.audioSettings ? x.audioSettings() : UiContext::AudioSettings{};
        section("Audio Device"); value("Driver Type", state.driver.empty() ? "No audio device" : state.driver);
        value("Audio Input Device", state.input.empty() ? "No Device" : state.input);
        choice(c, field("Audio Output Device"), "Audio Output Device", state.output.empty() ? "No Device" : state.output,
               state.outputs, [&x, outputs = state.outputs](int i) { if (x.setAudioSetting) x.setAudioSetting(UiContext::AudioSetting::Output, outputs[(size_t) i]); });
        button(c, field("Channel Configuration"), "Channel Configuration", "Input / Output Config...", false, x.showAudioSettings);
        section("Sample Rate");
        std::vector<std::string> rates; for (double rate : state.sampleRates) rates.push_back(number(rate));
        choice(c, field("In/Out Sample Rate"), "In/Out Sample Rate", state.sampleRate > 0 ? number(state.sampleRate) : "Unavailable", rates,
               [&x, rates](int i) { if (x.setAudioSetting) x.setAudioSetting(UiContext::AudioSetting::SampleRate, rates[(size_t) i]); });
        section("Latency");
        std::vector<std::string> sizes; for (int size : state.bufferSizes) sizes.push_back(std::to_string(size));
        choice(c, field("Buffer Size"), "Buffer Size", std::to_string(state.bufferSize) + " Samples", sizes,
               [&x, sizes](int i) { if (x.setAudioSetting) x.setAudioSetting(UiContext::AudioSetting::BufferSize, sizes[(size_t) i]); });
        value("Input Latency", number(state.inputLatencyMs, 2) + " ms");
        value("Output Latency", number(state.outputLatencyMs, 2) + " ms");
        value("Overall Latency", number(state.inputLatencyMs + state.outputLatencyMs, 2) + " ms");
        section("CPU"); // ENG-14e
        onOff("Multi-Core Processing", x.cpu.multiCore, [&x, on = x.cpu.multiCore] { if (x.setMultiCore) x.setMultiCore(! on); });
        value("Threads", x.cpu.multiCore ? std::to_string(x.cpu.helpers + 1) + " (tracks render in parallel)" : std::string("1 (the audio thread)"));
    }
    else if (tab == Tab::Midi)
    {
        // Port tables: name on the left, a switch centred under each column heading.
        auto column = [](const Rect& r, int fromRight) { return Rect{r.right() - 64.0f * (float) fromRight, r.y, 58, r.h}; };
        auto cell = [&](const Rect& r, int fromRight, const std::string& key, bool on, std::function<void()> action) {
            const auto col = column(r, fromRight);
            toggle(c, {col.centreX() - switchWidth / 2, r.y, switchWidth, r.h}, key, on, std::move(action));
        };
        section("Input Ports");
        auto head = row(22);
        paint::text(c, "MIDI Port", head, dim, fs, Align::Left, true);
        paint::text(c, "Track", column(head, 2), dim, fs, Align::Centre, true);
        paint::text(c, "Remote", column(head, 1), dim, fs, Align::Centre, true);
        const auto inputs = x.midiInputRows ? x.midiInputRows() : std::vector<UiContext::MidiInputRow>{};
        if (inputs.empty()) paint::text(c, "No MIDI inputs connected", row(), dim, fs);
        for (const auto& input : inputs)
        {
            auto r = row();
            paint::text(c, input.name, r.withWidth(r.w - 140), fg, fs);
            cell(r, 2, "Track " + input.id, input.track, [&x, input] {
                if (x.setMidiInputRoles) x.setMidiInputRoles(input.id, ! input.track, input.remote);
            });
            cell(r, 1, "Remote " + input.id, input.remote, [&x, input] {
                if (x.setMidiInputRoles) x.setMidiInputRoles(input.id, input.track, ! input.remote);
            });
        }
        section("Output Ports");
        head = row(22); paint::text(c, "MIDI Port", head, dim, fs, Align::Left, true);
        paint::text(c, "Sync", column(head, 1), dim, fs, Align::Centre, true);
        const auto outputs = x.midiOutputRows ? x.midiOutputRows() : std::vector<UiContext::MidiOutputRow>{};
        if (outputs.empty()) paint::text(c, "No MIDI outputs connected", row(), dim, fs);
        for (const auto& output : outputs)
        {
            auto r = row(); paint::text(c, output.name, r.withWidth(r.w - 75), fg, fs);
            const bool on = x.midiClockEnabled && x.midiClockOutputId == output.id;
            cell(r, 1, "Sync " + output.id, on, [&x, output, on] {
                if (x.setMidiClock) x.setMidiClock(output.id, ! on);
            });
        }
    }
    else if (tab == Tab::Plugins)
    {
        section("Plug-In Sources");
        button(c, field("Rescan Plug-Ins"), "Rescan Plug-Ins", x.scanning ? "Scanning..." : "Rescan", x.scanning,
               x.scanning ? nullptr : x.scanPlugins);
        button(c, field("Audio Units"), "Scan Audio Units", "Rescan VST3 + VST2 + AU", false, x.scanning ? nullptr : x.scanAudioUnits);
        for (size_t i = 0; i < x.pluginFolders.size(); ++i)
            reveal("Plug-In Folder " + std::to_string(i + 1), x.pluginFolders[i]);
        value("Plug-Ins Found", std::to_string(x.pluginNames.size()));
        section("Failed Plug-Ins");
        button(c, field("Retry Failed Plug-Ins"), "Retry Failed Plug-Ins", "Retry", false, x.scanning ? nullptr : std::function<void()>([&x] {
            if (x.clearFailedPlugins) x.clearFailedPlugins(); if (x.scanAudioUnits) x.scanAudioUnits(); else if (x.scanPlugins) x.scanPlugins();
        }));
        if (x.failedPlugins.empty()) paint::text(c, "No failed plug-ins", row(), dim, fs);
        for (const auto& path : x.failedPlugins)
            note(juce::File(juce::String::fromUTF8(path.c_str())).getFileName().toStdString(), style::alert);
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
            button(c, r.removeFromRight(80), label, "Browse...", false, x.chooseLibraryFolder ? std::function<void()>([&x, packs] { x.chooseLibraryFolder(packs); }) : nullptr);
            r.removeFromRight(8);
            paint::text(c, path, r, dim, fs);
        };
        location("Location of User Library", x.userLibraryPath, false); location("Folder for Packs", x.packsPath, true);
        section("Places");
        for (const auto& path : x.browserPlaces) reveal(juce::File(juce::String::fromUTF8(path.c_str())).getFileName().toStdString(), path);
        button(c, field("Add Folder to Browser"), "Add Folder to Browser", "Add Folder...", false, x.addBrowserFolder);
    }
    else if (tab == Tab::Keys)
        paintShortcuts(c, y);
    else if (tab == Tab::Updates)
    {
        const auto state = x.updateSettings ? x.updateSettings() : UiContext::UpdateSettings{.reason = "This build has no updater"};
        section("Application Updates");
        value("Version", state.version.empty() ? "Unknown" : state.version);
        if (! state.available)
            value("Updates", state.reason);
        else
        {
            onOff("Check Automatically", state.automatic, [&x, on = state.automatic] { if (x.setAutomaticUpdateChecks) x.setAutomaticUpdateChecks(! on); });
            if (state.channels.size() > 1)
                choice(c, field("Update Channel").withWidth(140), "Update Channel", state.channels[(size_t) std::clamp(state.channel, 0, (int) state.channels.size() - 1)],
                       state.channels, [&x](int i) { if (x.setUpdateChannel) x.setUpdateChannel(i); });
            else if (! state.channels.empty())
                value("Update Channel", state.channels.front());
            const bool enabled = state.canCheck || state.pending;
            // The one action of this page is the primary button, like a prompt's default.
            button(c, field("Check for Updates").withWidth(160), "Check for Updates",
                   state.pending ? "Install and Restart..." : "Check Now...", enabled,
                   enabled ? x.checkForUpdates : nullptr);
            value("Updater", state.backend);
        }
        section("Safe Restart");
        note("Sonora never restarts for an update while playing, recording or rendering.", dim);
        note("Unsaved changes are offered a save before Sonora restarts.", dim);
    }
    else
    {
        section("Record");
        choice(c, field("Count-In"), "Count-In", x.countInBars == 0 ? "None" : x.countInBars == 1 ? "1 Bar" : std::to_string(x.countInBars) + " Bars",
               {"None", "1 Bar", "2 Bars", "4 Bars"}, [&x](int i) { constexpr int bars[] = {0, 1, 2, 4}; x.countInBars = bars[i]; if (x.setCountInBars) x.setCountInBars(bars[i]); });
        onOff("MIDI Arrangement Overdub", x.arrangementOverdub, [&x] { x.arrangementOverdub = ! x.arrangementOverdub; if (x.layoutChanged) x.layoutChanged(); });
        onOff("Automation Arm", x.automationArm, [&x] { x.automationArm = ! x.automationArm; if (x.layoutChanged) x.layoutChanged(); });
        section("Launch");
        std::vector<std::string> quants(std::begin(quantNames), std::end(quantNames));
        choice(c, field("Global Launch Quantization"), "Global Launch Quantization", x.song ? quantNames[std::clamp(x.song->globalQuantisation, 0, 13)] : "1 Bar",
               x.song ? quants : std::vector<std::string>{}, [&x](int i) { if (x.song && x.applyEdit) x.applyEdit(model::setGlobalQuantisation(*x.song, i)); });
        section("Browser");
        onOff("Browser File Preview", x.browserPreviewEnabled, [&x] {
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
        paint::fillRounded(c, {panel.right() - 10, top, 4, h}, 2, style::scrollbar);
    }
    gfx::brand::drawPanelBorder(c, panel.sk());
}

int PreferencesPanel::hitAt(float px, float py) const
{
    for (size_t i = 0; i < hits_.size(); ++i)
        if (hits_[i].area.contains(px, py) && (! hits_[i].body || bodyClip_.contains(px, py))) return (int) i;
    return -1;
}

void PreferencesPanel::mouseDown(const MouseEvent& e)
{
    if (const int i = hitAt(e.x, e.y); i >= 0)
    {
        auto action = hits_[(size_t) i].action; action(); repaint();
    }
}

void PreferencesPanel::mouseMove(const MouseEvent& e)
{
    // Hover lifts a control like the prompt buttons; repaint only when the control under the pointer changes.
    hoverX_ = e.x; hoverY_ = e.y;
    const int i = hitAt(e.x, e.y);
    if (i != hoverHit_) { hoverHit_ = i; repaint(); }
}

void PreferencesPanel::mouseExit()
{
    hoverX_ = hoverY_ = -1;
    if (hoverHit_ >= 0) { hoverHit_ = -1; repaint(); }
}

bool PreferencesPanel::mouseWheel(const MouseEvent& e)
{
    if (bodyClip_.contains(e.x, e.y)) scroll_ = std::clamp(scroll_ - e.wheelDeltaY * kWheelPixels, 0.0f, std::max(0.0f, contentHeight_ - bodyClip_.h));
    repaint(); return true;
}

void PreferencesPanel::paintShortcuts(SkCanvas& c, float& y)
{
    auto& x = ctx();
    const auto fg = style::text, dim = style::message;
    const float fs = style::textSize;
    auto row = [&](float h = 28.0f) { Rect r{bodyClip_.x, y, bodyClip_.w, h}; y += h; return r; };
    auto* map = x.shortcuts;
    if (! map) { paint::text(c, "Keyboard shortcuts are not available here", row(), dim, fs); return; }
    // The filter is typed anywhere on this page; it shows in a text field like the prompts' wells.
    auto head = row(32);
    const Rect filter{head.x, head.y + 3, head.w - 104, controlHeight};
    gfx::brand::drawField(c, filter.sk());
    paint::text(c, filter_.empty() ? "Type to filter commands" : filter_, filter.reduced(9, 0), filter_.empty() ? style::dim : fg, fs);
    button(c, {head.right() - 92, head.y + 3, 92, controlHeight}, "Reset All", "Reset All", false, [this, &x, map] {
        map->resetAll(); recording_ = 0; shortcutStatus_ = "All shortcuts restored";
        if (x.shortcutsChanged) x.shortcutsChanged();
    });
    const std::string hint = recording_ != 0 ? "Press the new shortcut. Delete clears it, Esc cancels."
                                             : ! shortcutStatus_.empty() ? shortcutStatus_ : "Click a shortcut to change it";
    paint::text(c, hint, row(24), recording_ != 0 ? style::alert : dim, fs);
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
            y += 10;
            const auto r = row(24);
            paint::text(c, menu, r, style::title, fs, Align::Left, true);
            paint::fill(c, {r.x, r.bottom() - 1, r.w, 1}, style::separator);
        }
        const auto r = row();
        const bool custom = map->isCustom(command.id);
        paint::text(c, command.name, r.withWidth(r.w - 196), fg, fs);
        const auto key = map->keyFor(command.id);
        const std::string label = recording_ == command.id ? "Press keys..." : key ? key->text() : "None";
        const int id = command.id;
        const float top = r.y + (r.h - 22) / 2;
        button(c, {r.right() - 190, top, 124, 22}, "Shortcut " + command.storageName(), label, custom || recording_ == id,
               [this, id] { recording_ = recording_ == id ? 0 : id; shortcutStatus_.clear(); });
        if (custom)
            button(c, {r.right() - 58, top, 58, 22}, "Reset " + command.storageName(), "Reset", false, [this, &x, map, id] {
                map->reset(id); recording_ = 0; shortcutStatus_.clear();
                if (x.shortcutsChanged) x.shortcutsChanged();
            });
        else if (command.defaultKey)
            paint::text(c, "Default", {r.right() - 58, r.y, 58, r.h}, style::dim, fs, Align::Centre);
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
