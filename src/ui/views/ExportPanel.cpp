#include "ui/views/ExportPanel.h"

#include "engine/Exporter.h"
#include "ui/Controls.h"
#include "ui/Painter.h"
#include "ui/UiContext.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace sonora::ui
{

namespace
{
constexpr float kPanelW = 560.0f, kRowH = 28.0f, kLabelW = 150.0f;
const char* const kTails[] = {"None", "1 s", "2 s", "4 s", "8 s", "Until silent"};
const char* const kFormats[] = {"WAV", "AIFF", "FLAC"};

std::string number(double v, const char* format)
{
    char buf[64];
    std::snprintf(buf, sizeof buf, format, v);
    return buf;
}

std::string levelText(float db, const char* unit)
{
    return db <= -150.0f ? std::string("-inf ") + unit : number((double) db, "%.1f ") + unit;
}
} // namespace

Rect ExportPanel::panelRect() const
{
    const auto phase = const_cast<ExportPanel*>(this)->ctx().exportState.phase;
    const float h = phase == ExportState::Phase::Idle ? 370.0f : phase == ExportState::Phase::Done ? 300.0f : 170.0f;
    const float w = std::min(kPanelW, width() - 40), hh = std::min(h, height() - 40);
    return {(width() - w) * 0.5f, (height() - hh) * 0.5f, w, hh};
}

void ExportPanel::button(SkCanvas& c, const Theme& t, const Rect& r, const std::string& label, bool on, std::function<void()> action, bool enabled)
{
    paint::fill(c, r, on ? t.color("ChosenDefault") : t.color("ControlBackground"));
    paint::stroke(c, r, t.color("ControlContrastFrame"));
    paint::text(c, label, r.reduced(4, 0), enabled ? (on ? paint::contrastText(t.color("ChosenDefault")) : t.color("ControlForeground")) : t.color("TextDisabled"),
                t.metrics.fontSize, Align::Centre);
    if (enabled)
        hits_.push_back({r, std::move(action)});
}

void ExportPanel::dropdownField(SkCanvas& c, const Theme& t, const Rect& r, const std::string& text, std::function<void()> action, bool enabled)
{
    ctl::dropdown(c, t, r, text, enabled);
    if (enabled)
        hits_.push_back({r, std::move(action)});
}

void ExportPanel::numberField(SkCanvas& c, const Theme& t, const Rect& r, int index, double value, bool enabled)
{
    const bool editing = editing_ == index;
    ctl::valueBox(c, t, r, editing ? text_ + "|" : number(value, "%.0f"), Align::Centre, enabled);
    if (editing)
        paint::stroke(c, r, t.color("ChosenDefault"), 2.0f);
    if (enabled)
        hits_.push_back({r, [this, index, value] {
                             commitEdit();
                             editing_ = index;
                             text_.clear();
                             (void) value;
                         }});
}

void ExportPanel::choose(const std::vector<std::string>& labels, int current, std::function<void(int)> onPick)
{
    std::vector<bool> ticked(labels.size(), false);
    if (current >= 0 && (size_t) current < ticked.size())
        ticked[(size_t) current] = true;
    auto& x = ctx();
    auto done = [this, handler = std::move(onPick)](int i) {
        handler(i);
        repaint();
    };
    if (x.showChoices)
    {
        x.showChoices(labels, ticked, std::move(done));
        return;
    }
    juce::PopupMenu menu;
    for (size_t i = 0; i < labels.size(); ++i)
        menu.addItem((int) i + 1, juce::String::fromUTF8(labels[i].c_str()), true, ticked[i]);
    menu.showMenuAsync(juce::PopupMenu::Options(), [done](int result) {
        if (result > 0)
            done(result - 1);
    });
}

void ExportPanel::close()
{
    commitEdit();
    auto& x = ctx();
    x.exportOpen = false;
    if (x.exportState.phase != ExportState::Phase::Running)
        x.exportState = {};
    if (x.modelChanged)
        x.modelChanged();
}

void ExportPanel::commitEdit()
{
    if (editing_ < 0)
        return;
    auto& s = ctx().exportSettings;
    if (! text_.empty())
    {
        const double v = std::max(1.0, std::floor(std::atof(text_.c_str())));
        if (editing_ == 0) { s.startBar = v; s.endBar = std::max(s.endBar, v + 1.0); }
        else s.endBar = std::max(v, s.startBar + 1.0);
    }
    editing_ = -1;
    text_.clear();
}

std::string ExportPanel::lengthText() const
{
    auto& x = const_cast<ExportPanel*>(this)->ctx();
    if (x.song == nullptr)
        return "";
    const auto range = engine::Exporter::resolveRange(*x.song, x.exportSettings.range, x.exportSettings.startBar, x.exportSettings.endBar);
    const double bar = std::max(1.0, x.song->timeSigNumerator * 4.0 / std::max(1, x.song->timeSigDenominator));
    const double seconds = (range.endBeat - range.startBeat) * 60.0 / std::max(20.0, x.song->tempo);
    char buf[96];
    std::snprintf(buf, sizeof buf, "%g bars, about %d:%04.1f at %.0f BPM", (range.endBeat - range.startBeat) / bar, (int) seconds / 60,
                  std::fmod(seconds, 60.0), x.song->tempo);
    return buf;
}

void ExportPanel::tick()
{
    const auto& st = const_cast<ExportPanel*>(this)->ctx().exportState;
    if (visible && (std::abs(st.progress - shownProgress_) > 1e-6f || (int) st.phase != shownPhase_))
    {
        shownProgress_ = st.progress;
        shownPhase_ = (int) st.phase;
        repaint();
    }
}

void ExportPanel::paint(SkCanvas& c, const Theme& t)
{
    hits_.clear();
    auto& x = ctx();
    auto& s = x.exportSettings;
    const auto& st = x.exportState;
    paint::fill(c, localBounds(), paint::withAlpha(SkColorSetRGB(0, 0, 0), 150)); // scrim
    const auto panel = panelRect();
    paint::fill(c, panel, t.color("SurfaceBackground"));
    paint::stroke(c, panel, t.color("ControlContrastFrame"));
    const SkColor ink = t.color("ControlForeground"), dim = t.color("TextDisabled");
    const float fs = t.metrics.fontSize;

    auto r = panel;
    auto title = r.removeFromTop(30);
    paint::text(c, "Export Audio", title.reduced(12, 0), ink, fs + 2, Align::Left, true);
    r = r.reduced(14, 4);

    if (st.phase == ExportState::Phase::Running)
    {
        paint::text(c, st.message.empty() ? "Rendering..." : st.message, r.removeFromTop(24), ink, fs + 1, Align::Left, true);
        const auto bar = r.removeFromTop(16);
        paint::fill(c, bar, t.color("RetroDisplayBackground"));
        paint::fill(c, {bar.x + 1, bar.y + 1, (bar.w - 2) * std::clamp(st.progress, 0.0f, 1.0f), bar.h - 2}, t.color("ChosenDefault"));
        paint::text(c, number((double) std::clamp(st.progress, 0.0f, 1.0f) * 100.0, "%.0f %%"), r.removeFromTop(22), dim, fs, Align::Left);
        button(c, t, {panel.right() - 104, panel.bottom() - 36, 90, 24}, "Cancel", false, [&x] { if (x.cancelExport) x.cancelExport(); });
        return;
    }
    if (st.phase == ExportState::Phase::Failed)
    {
        paint::text(c, st.message.empty() ? "The export failed." : st.message, r.removeFromTop(48), t.color("ChosenAlert"), fs + 1, Align::Left);
        button(c, t, {panel.right() - 104, panel.bottom() - 36, 90, 24}, "Close", false, [this] { close(); });
        return;
    }
    if (st.phase == ExportState::Phase::Done)
    {
        paint::text(c, st.files.size() == 1 ? "Export finished" : "Export finished: " + std::to_string(st.files.size()) + " files", r.removeFromTop(24), ink,
                    fs + 1, Align::Left, true);
        for (const auto& f : st.files)
        {
            if (r.h < 120) break;
            paint::text(c, juce::File(juce::String::fromUTF8(f.c_str())).getFileName().toStdString(), r.removeFromTop(16), dim, fs - 1, Align::Left);
            if (st.files.size() > 3 && &f == &st.files[2]) { paint::text(c, "...", r.removeFromTop(14), dim, fs, Align::Left); break; }
        }
        r.removeFromTop(8);
        struct Row { const char* label; std::string value; };
        const Row rows[] = {{"Integrated loudness", levelText(st.integratedLufs, "LUFS")},
                            {"Loudness range", number((double) st.loudnessRange, "%.1f LU")},
                            {"True peak", levelText(st.truePeakDb, "dBTP")},
                            {"Sample peak", levelText(st.samplePeakDb, "dBFS")},
                            {"Length", number(st.seconds, "%.1f s")}};
        for (const auto& row : rows)
        {
            auto line = r.removeFromTop(20);
            paint::text(c, row.label, line.removeFromLeft(170), dim, fs, Align::Left);
            const bool alert = std::string(row.label) == "True peak" && st.truePeakDb > -1.0f;
            paint::text(c, row.value, line, alert ? t.color("ChosenAlert") : ink, fs, Align::Left, true);
        }
        button(c, t, {panel.x + 14, panel.bottom() - 36, 130, 24}, "Show in Finder", false, [&x, files = st.files] { if (x.revealInFinder && ! files.empty()) x.revealInFinder(files.front()); });
        button(c, t, {panel.right() - 104, panel.bottom() - 36, 90, 24}, "Close", false, [this] { close(); });
        return;
    }

    // ---- settings
    auto row = [&](const char* label) {
        auto line = r.removeFromTop(kRowH);
        paint::text(c, label, line.removeFromLeft(kLabelW), ink, fs, Align::Left);
        return line.reduced(0, 3);
    };

    // rendered track
    std::vector<std::string> trackLabels{"Main", "All Individual Tracks"};
    std::vector<int> trackIndices{-1, -1};
    for (int i = 0; x.song != nullptr && i < x.numControlTracks(); ++i)
        if (const auto* track = x.trackAt(i); track != nullptr && track->kind != model::TrackKind::Main)
        {
            trackLabels.push_back(track->name);
            trackIndices.push_back(i);
        }
    int targetLabel = std::clamp(s.target, 0, 1);
    if (s.target >= 2)
    {
        targetLabel = 0;
        for (size_t i = 2; i < trackIndices.size(); ++i)
            if (trackIndices[i] == s.target - 2)
                targetLabel = (int) i;
        if (targetLabel == 0)
            s.target = 0;
    }
    {
        const auto field = row("Rendered Track").removeFromLeft(260);
        dropdownField(c, t, field, trackLabels[(size_t) targetLabel], [this, trackLabels, trackIndices, targetLabel, &s] {
            choose(trackLabels, targetLabel, [&s, trackIndices](int i) { s.target = i < 2 ? i : 2 + trackIndices[(size_t) i]; });
        });
    }
    {
        auto line = row("Render Range");
        const char* names[] = {"Entire Arrangement", "Loop Selection", "Custom"};
        dropdownField(c, t, line.removeFromLeft(260), names[std::clamp(s.range, 0, 2)], [this, &s] {
            choose({"Entire Arrangement", "Loop Selection", "Custom"}, s.range, [&s](int i) { s.range = i; });
        });
    }
    {
        auto line = row("Start / End Bar");
        const bool custom = s.range == 2;
        const auto range = x.song ? engine::Exporter::resolveRange(*x.song, s.range, s.startBar, s.endBar) : engine::Exporter::Range{};
        const double bar = x.song ? std::max(1.0, x.song->timeSigNumerator * 4.0 / std::max(1, x.song->timeSigDenominator)) : 4.0;
        numberField(c, t, line.removeFromLeft(70), 0, custom ? s.startBar : range.startBeat / bar + 1.0, custom);
        line.removeFromLeft(10);
        numberField(c, t, line.removeFromLeft(70), 1, custom ? s.endBar : range.endBeat / bar + 1.0, custom);
        paint::text(c, lengthText(), line.reduced(10, 0), dim, fs - 1, Align::Left);
    }
    {
        auto line = row("Tail");
        dropdownField(c, t, line.removeFromLeft(260), kTails[std::clamp(s.tail, 0, 5)], [this, &s] {
            choose({kTails[0], kTails[1], kTails[2], kTails[3], kTails[4], kTails[5]}, s.tail, [&s](int i) { s.tail = i; });
        });
    }
    {
        auto line = row("Options");
        button(c, t, line.removeFromLeft(110), "Normalize", s.normalize, [&s] { s.normalize = ! s.normalize; });
        line.removeFromLeft(8);
        button(c, t, line.removeFromLeft(130), "Convert to Mono", s.mono, [&s] { s.mono = ! s.mono; });
    }
    {
        auto line = row("File Type");
        dropdownField(c, t, line.removeFromLeft(120), kFormats[std::clamp(s.format, 0, 2)], [this, &s] {
            choose({kFormats[0], kFormats[1], kFormats[2]}, s.format, [&s](int i) {
                s.format = i;
                if (i != 0 && s.bitDepth == 32) s.bitDepth = 24; // only WAV can hold 32-bit float
            });
        });
        line.removeFromLeft(10);
        const std::string depth = s.bitDepth == 32 ? "32 bit float" : std::to_string(s.bitDepth) + " bit";
        dropdownField(c, t, line.removeFromLeft(130), depth, [this, &s] {
            std::vector<std::string> labels{"16 bit", "24 bit"};
            std::vector<int> depths{16, 24};
            if (s.format == 0) { labels.push_back("32 bit float"); depths.push_back(32); }
            int current = 0;
            for (size_t i = 0; i < depths.size(); ++i) if (depths[i] == s.bitDepth) current = (int) i;
            choose(labels, current, [&s, depths](int i) { s.bitDepth = depths[(size_t) i]; });
        });
        line.removeFromLeft(10);
        button(c, t, line.removeFromLeft(90), "Dither", s.dither && s.bitDepth < 32, [&s] { s.dither = ! s.dither; }, s.bitDepth < 32);
    }
    r.removeFromTop(6);
    const std::string note = s.target == 1 ? "One file per track and return, post-fader, before send effects and Main effects."
                             : s.target >= 2 ? "The track's own output, post-fader, before send effects and Main effects."
                                             : "The complete mix with return and Main effects.";
    paint::text(c, note, r.removeFromTop(34), dim, fs - 1, Align::Left);

    const bool canExport = x.song != nullptr && x.startExport != nullptr;
    button(c, t, {panel.right() - 214, panel.bottom() - 36, 90, 24}, "Cancel", false, [this] { close(); });
    button(c, t, {panel.right() - 114, panel.bottom() - 36, 100, 24}, "Export...", true, [this, &x] {
        commitEdit();
        if (x.startExport)
            x.startExport(x.exportSettings);
    }, canExport);
}

void ExportPanel::mouseDown(const MouseEvent& e)
{
    const bool wasEditing = editing_ >= 0;
    for (const auto& h : hits_)
        if (h.area.contains(e.x, e.y))
        {
            auto action = h.action; // the action may rebuild hits_
            action();
            repaint();
            return;
        }
    if (wasEditing)
        commitEdit();
    if (! panelRect().contains(e.x, e.y) && ctx().exportState.phase != ExportState::Phase::Running)
        close(); // a click on the scrim closes
    repaint();
}

bool ExportPanel::keyPressed(const KeyEvent& k)
{
    auto& x = ctx();
    if (! x.exportOpen)
        return false; // closed (the layout has not hidden the panel yet)
    if (editing_ >= 0)
    {
        if (k.keyCode == juce::KeyPress::escapeKey) { editing_ = -1; text_.clear(); }
        else if (k.keyCode == juce::KeyPress::returnKey || k.keyCode == juce::KeyPress::tabKey) commitEdit();
        else if (k.keyCode == juce::KeyPress::backspaceKey || k.keyCode == juce::KeyPress::deleteKey) { if (! text_.empty()) text_.pop_back(); }
        else if (k.textChar >= '0' && k.textChar <= '9' && text_.size() < 5) text_ += (char) k.textChar;
        repaint();
        return true;
    }
    if (k.keyCode == juce::KeyPress::escapeKey)
    {
        if (x.exportState.phase == ExportState::Phase::Running) { if (x.cancelExport) x.cancelExport(); }
        else close();
        return true;
    }
    return true; // modal: nothing underneath reacts to keys
}

} // namespace sonora::ui
