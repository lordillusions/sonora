#include "ui/views/ResynthEditor.h"
#include "ui/UiContext.h"
#include "ui/Painter.h"
#include "ui/Controls.h"
#include "ui/widgets/Buttons.h"
#include "ui/widgets/Knob.h"
#include "engine/AudioFileFormats.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numbers>

namespace sonora::ui
{
namespace
{
using R = devices::Resynth;
class ResynthDial final : public Knob
{
public:
    using Knob::Knob;
    void paint(SkCanvas& canvas, const Theme& theme) override
    {
        const bool enabled = ! isEnabled || isEnabled();
        if (! enabled) canvas.saveLayerAlpha(nullptr, 85);
        Knob::paint(canvas, theme);
        if (! enabled) canvas.restore();
    }
};
std::string number(float value, const char* unit = "", int digits = 1)
{
    char text[48]; std::snprintf(text, sizeof(text), "%.*f%s", digits, (double) value, unit); return text;
}
float level(const model::ResynthFrame& frame, size_t harmonic)
{
    return std::hypot(frame.cosine[harmonic], frame.sine[harmonic]);
}
model::ResynthFrame interpolate(const model::ResynthNote& note, float position)
{
    const float at = position * (float) (note.frames.size() - 1);
    const size_t a = (size_t) at, b = std::min(a + 1, note.frames.size() - 1); const float mix = at - (float) a;
    auto frame = note.frames[a];
    for (size_t h = 0; h < frame.cosine.size(); ++h)
    { frame.cosine[h] = std::lerp(frame.cosine[h], note.frames[b].cosine[h], mix); frame.sine[h] = std::lerp(frame.sine[h], note.frames[b].sine[h], mix); }
    return frame;
}
}
Knob* ResynthEditor::knob(int parameter, int page, Rect bounds, const std::string& label)
{
    const auto& info = processor_->parameters()[(size_t) parameter]; Knob::Source source;
    source.get = [this, parameter] { return processor_->getParameter(parameter); };
    source.set = [this, parameter, integer = info.unit == "int"](float value) {
        if (parameter == R::HybridAmount && (! processor_->model() || processor_->model()->source.empty()
            || std::lround(processor_->getParameter(R::Mode)) != 1)) return;
        processor_->setParameter(parameter, integer ? std::round(value) : value);
    };
    source.minValue = info.minValue; source.maxValue = info.maxValue; source.defaultValue = info.defaultValue;
    source.bipolar = info.minValue < 0; source.logarithmic = info.unit == "Hz" || info.unit == "ms";
    source.format = [this, parameter, unit = info.unit](float value) {
        if (parameter == R::LfoRate && processor_->getParameter(R::LfoSync) > 0.5f) return number(value, " cyc/beat", 2);
        if (unit == "Hz") return value >= 1000 ? number(value / 1000, " kHz", 2) : number(value, " Hz", 1);
        if (unit == "ms") return value >= 1000 ? number(value / 1000, " s", 2) : number(value, " ms", value < 10 ? 1 : 0);
        const auto suffix = unit == "int" ? std::string{} : unit == "%" || unit == "x" || unit.empty() ? unit : " " + unit;
        return number(unit == "%" ? value * 100 : value, suffix.c_str(), unit == "%" || unit == "int" ? 0 : 1);
    };
    auto* control = add<ResynthDial>(label.empty() ? info.name : label, std::move(source));
    control->tooltip = info.name + ": double-click to reset; Shift-drag for fine adjustment.";
    controls_.push_back({control, page, bounds}); if (tag_) tag_(control, parameter); return control;
}
TextButton* ResynthEditor::choice(const std::string& name, int parameter, int value, int page, Rect bounds)
{
    auto* control = add<TextButton>(name);
    control->isOn = [this, parameter, value] { return (int) std::lround(processor_->getParameter(parameter)) == value; };
    control->onClick = [this, parameter, value] { processor_->setParameter(parameter, (float) value); repaint(); };
    controls_.push_back({control, page, bounds}); if (tag_) tag_(control, parameter); return control;
}
TextButton* ResynthEditor::toggle(const std::string& name, int parameter, int page, Rect bounds)
{
    auto* control = choice(name, parameter, 1, page, bounds);
    control->onClick = [this, parameter] { processor_->setParameter(parameter, processor_->getParameter(parameter) > 0.5f ? 0 : 1); repaint(); };
    return control;
}
ResynthEditor::ResynthEditor(R* processor, int track, int device, int chain, int child, std::function<void(Widget*, int)> tag)
    : processor_(processor), track_(track), device_(device), chain_(chain), child_(child), tag_(std::move(tag))
{
    if (const auto* model = processor_->model())
    {
        selected_ = std::clamp((int) std::lround(processor_->getParameter(R::Example)) - 1, 0, (int) model->notes.size() - 1);
        for (size_t i = 0; i < model->source.size(); ++i)
        {
            const size_t bin = std::min(lowPeaks_.size() - 1, i * lowPeaks_.size() / model->source.size());
            lowPeaks_[bin] = std::min(lowPeaks_[bin], model->source[i]); highPeaks_[bin] = std::max(highPeaks_[bin], model->source[i]);
        }
    }
    load_ = add<TextButton>("Load Audio...", TextButton::Style::Plain); load_->onClick = [this] { loadFile(); };
    phrase_ = add<TextButton>("Create MIDI", TextButton::Style::Plain); phrase_->isEnabled = [this] { return processor_->ready(); };
    phrase_->onClick = [this] { const auto action = ctx().createResynthPhrase; if (action) action(track_, device_, chain_, child_); };
    audition_ = add<TextButton>("Audition"); audition_->isEnabled = [this] { return processor_->ready(); };
    audition_->isOn = [this] { return auditioning_; };
    audition_->onClick = [this] {
        auditioning_ = ! auditioning_;
        if (const auto* model = processor_->model()) processor_->audition(auditioning_ ? (int) std::lround(model->notes[(size_t) selected_].root) : -1);
    };
    remove_ = add<TextButton>("Remove recording", TextButton::Style::Plain);
    remove_->tooltip = "Keep the synthesized sound and MIDI; remove embedded reference audio. Undo restores it.";
    remove_->isEnabled = [this] { return processor_->model() && ! processor_->model()->source.empty() && (bool) ctx().editDevice; };
    remove_->onClick = [this] { removeSource(); };
    for (int mode = 0; mode < 3; ++mode)
    {
        auto* control = choice(mode == 0 ? "Synth" : mode == 1 ? "Hybrid" : "Reference", R::Mode, mode, -1, {598 + (float) mode * 80, 2, 76, 20});
        control->isEnabled = [this, mode] { return mode == 0 || (processor_->model() && ! processor_->model()->source.empty()); };
        modes_.push_back(control);
    }
    for (int page = 0; page < 3; ++page)
    {
        auto* control = add<TextButton>(page == 0 ? "Sound" : page == 1 ? "Envelopes" : "Motion & voices");
        control->isOn = [this, page] { return page_ == page; };
        control->onClick = [this, page] { page_ = page; layout(); repaint(); }; tabs_.push_back(control);
    }
    auto* reset = add<TextButton>("Reset spectrum", TextButton::Style::Plain);
    controls_.push_back({reset, -1, {498, 28, 130, 18}});
    reset->onClick = [this] { for (int h = 0; h < model::ResynthFrame::harmonics; ++h) processor_->setParameter(R::Harmonic0 + h, 1); repaint(); };
    auto* automatic = toggle("Auto key map", R::Example, -1, {640, 126, 94, 18});
    automatic->isOn = [this] { return processor_->getParameter(R::Example) < 0.5f; };
    automatic->onClick = [this] { processor_->setParameter(R::Example, processor_->getParameter(R::Example) < 0.5f ? (float) (selected_ + 1) : 0); repaint(); };
    // Sound: each section corresponds to an audible part of the signal path.
    const int harmonicParameters[] = {R::Fundamental, R::OddLevel, R::EvenLevel, R::SpectralTilt, R::PartialCount, R::SubLevel};
    for (int i = 0; i < 6; ++i) knob(harmonicParameters[i], 0, {12 + (float) (i % 3) * 86, 218 + (float) (i / 3) * 57, 82, 48},
        i == 1 ? "Odd" : i == 2 ? "Even" : i == 3 ? "Tilt" : i == 5 ? "Sub sine" : "");
    const int noiseParameters[] = {R::Noise, R::NoiseLow, R::NoiseMid, R::NoiseHigh, R::NoiseAir};
    const char* noiseLabels[] = {"Total", "Low", "Mid", "High", "Air"};
    for (int i = 0; i < 5; ++i) knob(noiseParameters[i], 0, {294 + (float) (i % 2) * 80, 205 + (float) (i / 2) * 45, 76, 42}, noiseLabels[i]);
    auto* dry = add<TextButton>("Dry bass", TextButton::Style::Plain);
    dry->tooltip = "Use harmonic synthesis, mute modeled noise and shorten the note release to 30 ms. Captured tonal room sound may remain.";
    dry->onClick = [this] {
        processor_->setParameter(R::Mode, 0); processor_->setParameter(R::Noise, 0);
        processor_->setParameter(R::HybridAmount, 0); processor_->setParameter(R::Release, 30); repaint();
    };
    controls_.push_back({dry, 0, {380, 300, 72, 20}});
    toggle("Filter", R::FilterOn, 0, {474, 184, 58, 19});
    for (int i = 0; i < 4; ++i) choice(i == 0 ? "LP" : i == 1 ? "HP" : i == 2 ? "BP" : "Notch", R::FilterType, i, 0, {542 + (float) i * 45, 184, 42, 19});
    const int filterParameters[] = {R::Tone, R::Resonance, R::FilterEnv, R::Drive};
    for (int i = 0; i < 4; ++i) knob(filterParameters[i], 0, {482 + (float) (i % 2) * 118, 224 + (float) (i / 2) * 57, 108, 48});
    for (int i = 0; i < 4; ++i) knob(i == 0 ? R::Attack : i == 1 ? R::AmpDecay : i == 2 ? R::AmpSustain : R::Release, 0, {754 + (float) i * 76, 248, 72, 48});
    knob(R::Velocity, 0, {754, 302, 144, 42}); knob(R::Volume, 0, {914, 302, 144, 42});
    // Envelope page has independent amplifier and filter ADSR graphs and controls.
    for (int i = 0; i < 4; ++i)
    {
        knob(i == 0 ? R::Attack : i == 1 ? R::AmpDecay : i == 2 ? R::AmpSustain : R::Release, 1, {16 + (float) i * 127, 286, 118, 48});
        knob(R::FilterAttack + i, 1, {564 + (float) i * 127, 286, 118, 48});
    }
    // Modulation and mapping: discrete choices are buttons, continuous amounts are knobs.
    for (int i = 0; i < 3; ++i) choice(i == 0 ? "Evolve / hold" : i == 1 ? "Loop" : "Freeze", R::ScanMode, i, 2, {12 + (float) i * 94, 209, 90, 20});
    knob(R::TimbreStart, 2, {12, 248, 88, 48}, "Start"); knob(R::Sustain, 2, {108, 248, 88, 48}, "End / hold");
    knob(R::Movement, 2, {204, 248, 88, 48});
    toggle("Pitch contour", R::PitchMotion, 2, {12, 312, 128, 20});
    const char* shapes[] = {"Sine", "Tri", "Down", "Up", "Pulse", "Random"};
    for (int i = 0; i < 6; ++i) choice(shapes[i], R::LfoShape, i, 2, {326 + (float) i * 62, 209, 58, 20});
    const char* destinations[] = {"Pitch", "Scan", "Cutoff", "Amp", "Pan"};
    for (int i = 0; i < 5; ++i) choice(destinations[i], R::LfoDestination, i, 2, {326 + (float) i * 74, 239, 70, 20});
    knob(R::LfoRate, 2, {326, 291, 126, 48}, "Rate"); knob(R::LfoDepth, 2, {462, 291, 112, 48}, "Depth");
    toggle("Tempo sync", R::LfoSync, 2, {588, 309, 102, 20});
    for (int i = 0; i < 3; ++i) choice(i == 0 ? "Mono" : i == 1 ? "Legato" : "Poly 8", R::PlayMode, i, 2, {730 + (float) i * 111, 209, 106, 20});
    knob(R::Glide, 2, {730, 248, 104, 48}); knob(R::BendRange, 2, {842, 248, 104, 48});
    knob(R::RootOffset, 2, {954, 248, 104, 48}, "Root adjust");
    auto* attack = knob(R::HybridAmount, 2, {730, 302, 148, 42});
    attack->isEnabled = [this] { return processor_->getParameter(R::Mode) > 0.5f && processor_->getParameter(R::Mode) < 1.5f && processor_->model() && ! processor_->model()->source.empty(); };
}
ResynthEditor::~ResynthEditor() { processor_->audition(-1); }
void ResynthEditor::layout()
{
    load_->setBounds({4, 2, 92, 20}); phrase_->setBounds({102, 2, 92, 20}); audition_->setBounds({200, 2, 80, 20});
    remove_->setBounds({286, 2, 132, 20});
    for (size_t i = 0; i < tabs_.size(); ++i) tabs_[i]->setBounds({4 + (float) i * 126, 154, 120, 22});
    for (auto& control : controls_) { control.widget->visible = control.page < 0 || control.page == page_; control.widget->setBounds(control.bounds); }
}
void ResynthEditor::tick()
{
    if (const auto* model = processor_->model())
    {
        if (drag_ < 0 && processor_->getParameter(R::Example) < 0.5f && processor_->activeExample() >= 0) selected_ = processor_->activeExample();
        selected_ = std::clamp(selected_, 0, (int) model->notes.size() - 1);
    }
    repaint();
}
void ResynthEditor::envelope(SkCanvas& canvas, const Theme& theme, Rect r, int attack, int decay, int sustain, int release)
{
    const float a = std::sqrt(processor_->getParameter(attack)), d = std::sqrt(processor_->getParameter(decay));
    const float s = processor_->getParameter(sustain), rel = std::sqrt(processor_->getParameter(release));
    const float sum = a + d + rel + 35, x1 = r.x + r.w * a / sum, x2 = x1 + r.w * d / sum, x3 = x2 + r.w * 35 / sum;
    const SkPoint points[] = {{r.x, r.bottom()}, {x1, r.y}, {x2, r.bottom() - s * r.h}, {x3, r.bottom() - s * r.h}, {r.right(), r.bottom()}};
    for (int i = 1; i < 5; ++i) paint::line(canvas, points[i-1].x(), points[i-1].y(), points[i].x(), points[i].y(), theme.color("ChosenDefault"), 1.5f);
    paint::line(canvas, r.x, r.bottom(), r.right(), r.bottom(), theme.color("RetroDisplayScaleText"));
}
void ResynthEditor::paint(SkCanvas& canvas, const Theme& theme)
{
    const auto background = theme.color("RetroDisplayBackground"), foreground = theme.color("RetroDisplayForeground");
    const auto accent = theme.color("ChosenDefault"), muted = theme.color("TextDisabled"), text = theme.color("ControlForeground");
    paint::fillRounded(canvas, {0, 27, 634, 119}, 3, background);
    paint::fillRounded(canvas, {636, 27, 300, 119}, 3, background);
    paint::fillRounded(canvas, {940, 27, 136, 119}, 3, background);
    paint::text(canvas, "HARMONIC SPECTRUM", {8, 28, 200, 18}, foreground, 10, Align::Left, true);
    paint::text(canvas, "NOTE REGIONS", {644, 28, 280, 18}, foreground, 10, Align::Left, true);
    paint::text(canvas, "GENERATED CYCLE", {948, 28, 124, 18}, foreground, 9, Align::Left, true);
    paint::line(canvas, 0, 180, 1076, 180, theme.color("RetroDisplayScaleText"));
    const auto group = [&](Rect r, const char* label) {
        paint::fillRounded(canvas, r, 3, paint::withAlpha(background, 65));
        paint::text(canvas, label, {r.x + 8, r.y + 3, r.w - 16, 18}, text, 10, Align::Left, true);
    };
    if (page_ == 0)
    {
        group({0, 184, 278, 166}, "HARMONICS"); group({286, 184, 174, 166}, "MODELED NOISE");
        group({470, 184, 262, 166}, ""); group({742, 184, 334, 166}, "AMPLIFIER");
        envelope(canvas, theme, {754, 210, 304, 29}, R::Attack, R::AmpDecay, R::AmpSustain, R::Release);
        paint::text(canvas, "ADSR on Envelopes page", {480, 207, 240, 15}, muted, 9);
    }
    else if (page_ == 1)
    {
        group({0, 184, 532, 166}, "AMPLIFIER ENVELOPE"); group({548, 184, 528, 166}, "FILTER ENVELOPE");
        envelope(canvas, theme, {16, 214, 498, 59}, R::Attack, R::AmpDecay, R::AmpSustain, R::Release);
        envelope(canvas, theme, {564, 214, 494, 59}, R::FilterAttack, R::FilterDecay, R::FilterSustain, R::FilterRelease);
    }
    else
    {
        group({0, 184, 308, 166}, "TIMBRE SCAN"); group({318, 184, 390, 166}, "LFO"); group({720, 184, 356, 166}, "VOICES & PITCH");
        paint::text(canvas, "Cycle through the captured harmonic frames", {12, 232, 282, 14}, muted, 9);
        // Preview uses the same LFO shapes, with a fixed seed for the random waveform.
        dsp::Lfo preview; float last = 0;
        for (int x = 0; x < 366; ++x)
        {
            const float value = preview.next(2.0 / 366, (dsp::Lfo::Shape) (int) std::lround(processor_->getParameter(R::LfoShape)));
            if (x > 0) paint::line(canvas, 326 + (float) x - 1, 276 - last * 10, 326 + (float) x, 276 - value * 10, accent);
            last = value;
        }
    }
    const auto* model = processor_->model();
    if (! model)
    {
        paint::text(canvas, "Drop a dry bass or melody stem to build a harmonic model", {12, 62, 610, 30}, foreground, 13);
        paint::text(canvas, "One note at a time, up to 30 seconds. Then create MIDI and play the instrument.", {12, 96, 610, 22}, muted, 11);
        return;
    }
    const bool sourceFree = model->source.empty();
    paint::text(canvas, sourceFree ? "MODEL ONLY" : "RECORDING RETAINED", {426, 2, 164, 20}, sourceFree ? accent : muted, 10, Align::Centre, true);
    paint::text(canvas, model->name, {846, 2, 230, 20}, text, 10, Align::Right);
    const auto& note = model->notes[(size_t) selected_];
    const float position = processor_->activeExample() >= 0 ? processor_->playhead() : processor_->getParameter(R::TimbreStart);
    const auto frame = interpolate(note, std::clamp(position, 0.0f, 1.0f));
    const auto spectrum = spectrumBounds(); float maximum = 0.001f;
    for (size_t h = 0; h < frame.cosine.size(); ++h) maximum = std::max(maximum, level(frame, h));
    const float barWidth = spectrum.w / model::ResynthFrame::harmonics;
    for (int h = 0; h < model::ResynthFrame::harmonics; ++h)
    {
        const float original = std::sqrt(level(frame, (size_t) h) / maximum) * spectrum.h * 0.7f;
        const float changed = std::min(spectrum.h, original * processor_->harmonicGain(h + 1));
        const float x = spectrum.x + (float) h * barWidth;
        paint::fill(canvas, {x, spectrum.bottom() - original, barWidth - 1, original}, paint::withAlpha(foreground, 42));
        paint::fill(canvas, {x, spectrum.bottom() - changed, barWidth - 1, changed}, h == harmonic_ ? foreground : accent);
        if (h % 12 == 0) paint::text(canvas, std::to_string(h + 1), {x, spectrum.bottom(), 25, 13}, foreground, 8);
    }
    paint::text(canvas, harmonic_ >= 0 ? "H" + std::to_string(harmonic_ + 1) + "  " + number(processor_->getParameter(R::Harmonic0 + harmonic_), "x") + "    Drag vertically; double-click to reset"
        : "96 partials / frame  |  Drag a harmonic to shape it  |  Grey = captured spectrum", {8, 132, 620, 13}, foreground, 9);
    // This is generated from coefficients and current harmonic gains, never from the recorded PCM.
    float previous = 0, peak = 0.001f; std::array<float, 256> cycle{};
    for (size_t x = 0; x < cycle.size(); ++x)
    {
        double sample = 0;
        for (int h = 0; h < model::ResynthFrame::harmonics; ++h)
        {
            const double phase = 2 * std::numbers::pi * (h + 1) * (double) x / (double) cycle.size();
            sample += processor_->harmonicGain(h + 1) * (frame.cosine[(size_t) h] * std::cos(phase) + frame.sine[(size_t) h] * std::sin(phase));
        }
        cycle[x] = (float) sample; peak = std::max(peak, std::abs(cycle[x]));
    }
    for (size_t x = 0; x < cycle.size(); ++x)
    {
        const float value = cycle[x] / peak;
        if (x) paint::line(canvas, 944 + (float) (x - 1) * 0.5f, 85 - previous * 31, 944 + (float) x * 0.5f, 85 - value * 31, accent);
        previous = value;
    }
    paint::text(canvas, "Harmonics only", {944, 128, 128, 16}, foreground, 9, Align::Centre);
    const auto r = waveformBounds(); const float seconds = (float) model->duration();
    for (size_t i = 0; i < model->notes.size(); ++i)
    {
        const auto& region = model->notes[i];
        const Rect box{r.x + region.start / seconds * r.w, r.y, (region.end - region.start) / seconds * r.w, r.h};
        paint::fill(canvas, box, paint::withAlpha(accent, (int) i == selected_ ? 65 : 20));
        paint::text(canvas, "M" + std::to_string((int) std::lround(region.root)), {box.x + 3, box.y, box.w - 3, 14}, foreground, 9);
        if (sourceFree)
        {
            for (size_t f = 0; f < region.frames.size(); ++f)
            {
                const float magnitude = std::sqrt(std::min(1.0f, level(region.frames[f], 0)));
                const float x = box.x + box.w * (float) f / (float) region.frames.size();
                paint::line(canvas, x, box.bottom(), x, box.bottom() - magnitude * 42, foreground, std::max(1.0f, box.w / (float) region.frames.size() - 1));
            }
        }
    }
    if (! sourceFree) for (int x = 0; x < (int) r.w; ++x)
        paint::line(canvas, r.x + (float) x, r.centreY() - highPeaks_[(size_t) x] * r.h * 0.4f,
                    r.x + (float) x, r.centreY() - lowPeaks_[(size_t) x] * r.h * 0.4f, foreground);
    const float start = drag_ >= 0 && drag_ < 2 ? boundaryStart_ : note.start, end = drag_ >= 0 && drag_ < 2 ? boundaryEnd_ : note.end;
    for (float at : {start, end}) paint::fill(canvas, {r.x + at / seconds * r.w - 1, r.y, 2, r.h}, accent);
    paint::text(canvas, std::to_string(selected_ + 1) + "/" + std::to_string(model->notes.size()) + "  " + number(note.root, " MIDI") + "  " + number(seconds, " s"), {740, 126, 190, 18}, foreground, 9, Align::Right);
    const char* description = sourceFree ? "Model only: harmonic oscillators + modeled noise. No recording stored or read."
        : processor_->getParameter(R::Mode) < 0.5f ? "Synth: harmonic oscillators + modeled noise. Recording is unused."
        : processor_->getParameter(R::Mode) < 1.5f ? "Hybrid: synthesized body + up to 60 ms of recorded attack."
        : "Reference: recorded mono region, for comparison.";
    paint::text(canvas, description, {390, 155, 686, 20}, muted, 10, Align::Right);
    paint::text(canvas, "Signal: harmonic model + noise + sub  >  drive  >  filter  >  amplifier", {4, 352, 1072, 16}, muted, 9);
}
void ResynthEditor::mouseDown(const MouseEvent& event)
{
    if (event.right) return;
    if (spectrumBounds().contains(event.x, event.y))
    {
        harmonic_ = std::clamp((int) ((event.x - spectrumBounds().x) / spectrumBounds().w * model::ResynthFrame::harmonics), 0, model::ResynthFrame::harmonics - 1);
        harmonicStart_ = processor_->getParameter(R::Harmonic0 + harmonic_); dragY_ = event.y; drag_ = 2;
        if (event.alt) processor_->setParameter(R::Harmonic0 + harmonic_, 1);
        repaint(); return;
    }
    const auto* model = processor_->model(); if (! model || ! waveformBounds().contains(event.x, event.y)) return;
    const auto r = waveformBounds(); const float seconds = (float) model->duration();
    const float at = (event.x - r.x) / r.w * seconds; const auto& selected = model->notes[(size_t) selected_];
    boundaryStart_ = selected.start; boundaryEnd_ = selected.end;
    if (std::abs(at - selected.start) / seconds * r.w < 6) drag_ = 0;
    else if (std::abs(at - selected.end) / seconds * r.w < 6) drag_ = 1;
    else for (size_t i = 0; i < model->notes.size(); ++i) if (at >= model->notes[i].start && at <= model->notes[i].end)
    { selected_ = (int) i; processor_->setParameter(R::Example, (float) (i + 1)); break; }
    repaint();
}
void ResynthEditor::mouseDrag(const MouseEvent& event)
{
    if (event.right || drag_ < 0) return;
    if (drag_ == 2) { processor_->setParameter(R::Harmonic0 + harmonic_, harmonicStart_ + (dragY_ - event.y) * (event.shift ? 0.002f : 0.02f)); repaint(); return; }
    const auto* model = processor_->model(); if (! model) return;
    const auto& note = model->notes[(size_t) selected_];
    const float at = (event.x - waveformBounds().x) / waveformBounds().w * (float) model->duration();
    if (drag_ == 0) boundaryStart_ = std::clamp(at, note.start, boundaryEnd_ - 0.02f);
    else boundaryEnd_ = std::clamp(at, boundaryStart_ + 0.02f, note.end);
    repaint();
}
void ResynthEditor::mouseUp(const MouseEvent& event)
{
    if (event.right || drag_ < 0) return;
    const int drag = drag_; drag_ = -1; if (drag < 2) editBoundary(boundaryStart_, boundaryEnd_);
}
void ResynthEditor::mouseDoubleClick(const MouseEvent& event)
{
    if (spectrumBounds().contains(event.x, event.y) && harmonic_ >= 0) processor_->setParameter(R::Harmonic0 + harmonic_, 1);
    repaint();
}
void ResynthEditor::editBoundary(float start, float end)
{
    if (std::abs(start - processor_->model()->notes[(size_t) selected_].start) < 0.0001f
        && std::abs(end - processor_->model()->notes[(size_t) selected_].end) < 0.0001f) return;
    const auto edit = ctx().editDevice; const int chain = chain_, child = child_, selected = selected_;
    if (! edit) return;
    // Structural edits may destroy this editor; capture values and perform no member access afterward.
    edit(track_, device_, [chain, child, selected, start, end](model::Device& root) {
        auto* device = &root;
        if (chain >= 0)
        {
            if ((size_t) chain >= root.chains.size() || child < 0 || (size_t) child >= root.chains[(size_t) chain].devices.size()) return;
            device = &root.chains[(size_t) chain].devices[(size_t) child];
        }
        if (! device->nativeContent) return;
        std::string error; const auto loaded = model::decodeResynth(*device->nativeContent, error); if (! loaded) return;
        auto changed = *loaded; if (! model::trimResynthNote(changed, selected, start, end)) return;
        const auto content = model::encodeResynth(changed, error); if (! content) return;
        device->nativeContent = content; device->nativeContentEdited = true; if (chain >= 0) root.chainsEdited = true;
    }, "Trim Resynth Note");
}
void ResynthEditor::removeSource()
{
    const auto edit = ctx().editDevice; const int chain = chain_, child = child_; if (! edit) return;
    edit(track_, device_, [chain, child](model::Device& root) {
        auto* device = &root;
        if (chain >= 0)
        {
            if ((size_t) chain >= root.chains.size() || child < 0 || (size_t) child >= root.chains[(size_t) chain].devices.size()) return;
            device = &root.chains[(size_t) chain].devices[(size_t) child];
        }
        if (! device->nativeContent) return;
        std::string error; const auto loaded = model::decodeResynth(*device->nativeContent, error); if (! loaded) return;
        auto changed = *loaded; model::removeResynthSource(changed);
        const auto content = model::encodeResynth(changed, error); if (! content) return;
        device->nativeContent = content; device->nativeContentEdited = true;
        for (auto& parameter : device->parameters) if (parameter.path == "Mode") parameter.value = 0;
        if (chain >= 0) root.chainsEdited = true;
    }, "Remove Resynth Recording");
}
bool ResynthEditor::browserDrop(const BrowserDrag& payload, const MouseEvent&)
{
    if (payload.kind != BrowserDrag::Kind::Sample) return false;
    import(payload.value); return true;
}
void ResynthEditor::import(const std::string& path)
{
    const auto action = ctx().importResynth; if (action) action(track_, device_, path, chain_, child_);
}
void ResynthEditor::loadFile()
{
    auto chooser = std::make_shared<juce::FileChooser>("Analyze Bass / Melody", juce::File(), engine::audioFileWildcard());
    chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        [this, chooser](const juce::FileChooser& result) {
            if (Widget::isAlive(this) && result.getResult().existsAsFile()) import(result.getResult().getFullPathName().toStdString());
        });
}
} // namespace sonora::ui
