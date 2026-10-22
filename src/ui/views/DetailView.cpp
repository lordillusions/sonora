#include "ui/views/DetailView.h"

#include "ui/WheelScale.h"

#include "ui/views/PianoRoll.h"
#include "ui/views/MpeEditor.h"
#include "ui/views/ClipControls.h"
#include "ui/views/MonitorPanel.h"
#include "ui/views/ClipEnvelopeEditor.h"
#include "ui/views/SampleView.h"
#include "ui/views/NativeEditors.h"
#include "ui/views/DeviceFace.h"
#include "ui/views/ReferenceFace.h"
#include "ui/views/ResynthEditor.h"

#include "core/DeviceCatalog.h"
#include "devices/native/Reverb.h"
#include "ui/views/EffectPanels.h"
#include "ui/views/PluginPanel.h"
#include "devices/RackProcessor.h"
#include "devices/native/Simpler.h"
#include "devices/native/LfoShaper.h"
#include "devices/native/CurveLfo.h"
#include "devices/native/WavetableSynth.h"
#include "devices/native/Delay.h"
#include "ui/views/CurveLfoEditor.h"
#include "ui/views/WavetableEditor.h"
#include "engine/EngineGraph.h"
#include "plugins/PluginDevice.h"
#include "ui/Controls.h"
#include "ui/Painter.h"
#include "ui/UiContext.h"
#include "ui/widgets/Buttons.h"
#include "ui/widgets/Knob.h"
#include "ui/widgets/Splitter.h"

#include "include/core/SkPaint.h"
#include "include/core/SkPathBuilder.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace sonora::ui
{
bool DetailView::browserDrop(const BrowserDrag& payload, const MouseEvent&)
{
    if (payload.kind == BrowserDrag::Kind::Sample) return false;
    if (ctx().dropBrowserItem) ctx().dropBrowserItem(payload, ctx().selectedTrack, 0, -1);
    return true;
}

namespace
{
using devices::DeviceProcessor;

constexpr float kTitle = 18.0f, kDeviceGap = 8.0f, kChainStart = 8.0f;
// UI-42: every device title ends with the reference's hot-swap / save / menu circles (measured 17.5 capture px across,
// centres 58 / 35 / 13 px from the device's right edge at 0.703 units per capture px). Title widgets stay left of them.
constexpr float kTitleIcons = 50.0f, kTitleIconSize = 12.3f;
constexpr float kTitleIconRight[3] = {40.8f, 24.6f, 9.1f};

// Simpler's Sample / Controls title tabs: flat boxes, the selected one lighter (reference look).
class SimplerTab final : public TextButton
{
public:
    explicit SimplerTab(std::string label) : TextButton(std::move(label), Style::Plain) {}
    void paint(SkCanvas& c, const Theme& t) override
    {
        const bool on = isOn && isOn();
        paint::fill(c, localBounds().reduced(0, 1), on ? t.color("ControlBackground") : paint::withAlpha(t.color("ControlContrastFrame"), 40));
        paint::text(c, label(), localBounds(), t.color("ControlForeground"), t.metrics.smallFontSize, Align::Centre);
    }
};

std::string fmt(const char* f, double v)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), f, v);
    return buf;
}

Knob::Source paramSource(DeviceProcessor* dev, int index)
{
    const auto& info = dev->parameters()[(size_t) index];
    Knob::Source s;
    s.get = [dev, index] { return dev->getParameter(index); };
    const bool discrete = ! info.choices.empty();
    if (auto* delay = dynamic_cast<devices::Delay*>(dev))
        s.set = [delay, index, discrete](float v) { delay->setParameterLinked(index, discrete ? std::round(v) : v); };
    else
        s.set = [dev, index, discrete](float v) { dev->setParameter(index, discrete ? std::round(v) : v); };
    s.minValue = info.minValue;
    s.maxValue = info.maxValue;
    s.defaultValue = info.defaultValue;
    s.bipolar = info.minValue < 0 && info.maxValue > 0;
    s.logarithmic = info.unit == "Hz";
    const auto unit = info.unit;
    const auto choices = info.choices;
    s.format = [unit, choices](float v) {
        if (! choices.empty())
            return choices[(size_t) std::clamp((int) std::lround(v), 0, (int) choices.size() - 1)];
        if (unit == "Hz")
            return v >= 1000 ? fmt("%.2f kHz", v / 1000.0) : v < 10 ? fmt("%.2f Hz", v) : fmt("%.0f Hz", v);
        if (unit == "ms")
            return v >= 1000 ? fmt("%.2f s", v / 1000.0) : v < 10 ? fmt("%.2f ms", v) : fmt("%.1f ms", v);
        if (unit == "s")
            return fmt("%.2f s", v);
        if (unit == "pct")
            return fmt("%.0f %%", v);
        if (unit == "int")
            return fmt("%.0f", v);
        if (unit == "note")
            return fmt("1/%g", v);
        if (unit == "dB")
            return fmt("%.1f dB", v);
        if (unit == "%")
            return fmt("%.0f %%", v * 100.0);
        if (unit == "lin")
            return v <= 0.000317f ? std::string("-inf dB") : fmt("%.1f dB", 20.0 * std::log10(v));
        return fmt("%.2f", v);
    };
    return s;
}

// ---------------------------------------------------------------- device box
class RackSelector final : public Widget
{
public:
    explicit RackSelector(DeviceProcessor* processor) : processor_(processor) {}
    void paint(SkCanvas& c, const Theme& t) override
    {
        ctl::valueBox(c, t, localBounds(), "Chain Selector  " + fmt("%.0f", processor_->getParameter(devices::RackProcessor::kChainSelector)));
    }
    void mouseDown(const MouseEvent& e) override
    {
        pressed_ = ! e.right; startX_ = e.x; start_ = processor_->getParameter(devices::RackProcessor::kChainSelector);
    }
    void mouseDrag(const MouseEvent& e) override
    {
        if (pressed_ && ! e.right) { processor_->setParameter(devices::RackProcessor::kChainSelector, std::round(start_ + (e.x - startX_) * (e.shift ? 0.1f : 1.0f))); repaint(); }
    }
    void mouseUp(const MouseEvent&) override { pressed_ = false; }
    bool mouseWheel(const MouseEvent& e) override
    {
        if (e.right || std::abs(e.wheelDeltaX) > std::abs(e.wheelDeltaY)) return false;
        processor_->setParameter(devices::RackProcessor::kChainSelector, processor_->getParameter(devices::RackProcessor::kChainSelector) + (e.wheelDeltaY > 0 ? 1.0f : -1.0f));
        repaint(); return true;
    }
private:
    DeviceProcessor* processor_;
    float startX_ = 0, start_ = 0;
    bool pressed_ = false;
};
class DeviceBox : public Widget
{
public:
    DeviceBox(const model::Device& model, DeviceProcessor* proc, int trackIndex, int deviceIndex, int chain = -1, int child = -1,
              bool floatingWavetable = false)
        : model_(model), proc_(proc), trackIndex_(trackIndex), deviceIndex_(deviceIndex), chain_(chain), child_(child)
    {
        if (model_.plugin)
        {
            pluginPanel_ = add<PluginPanel>(proc_, trackIndex, model.id, deviceIndex, chain);
            editorButton_ = add<IconButton>(IconButton::Icon::Wrench);
            editorButton_->flat = true; // in the second title ring, like the fold triangle in the first
            editorButton_->isEnabled = [this] { return proc_ && proc_->hasEditor(); };
            editorButton_->onClick = [this] {
                if (ctx().openPluginEditor) ctx().openPluginEditor(trackIndex_, deviceIndex_);
            };
        }
        if (proc_ == nullptr)
            return;

        onButton_ = add<TextButton>("", TextButton::Style::Toggle);
        onButton_->roundStyle = true;
        onButton_->midiTarget = midi::ControlTarget{midi::ControlTarget::Kind::DeviceOn, trackIndex_, deviceIndex_, 0, {}};
        onButton_->isOn = [this] { return ! proc_->isBypassed(); };
        onButton_->onClick = [this] { proc_->setBypassed(! proc_->isBypassed()); };
        if (chain_ >= 0) onButton_->midiTarget.reset();
        if (auto* synth = dynamic_cast<devices::Resynth*>(proc_))
        {
            resynth_ = add<ResynthEditor>(synth, trackIndex, deviceIndex, chain, child,
                                         [this](Widget* widget, int parameter) { tag(widget, parameter); });
            return;
        }
        if ((referenceFace_ = addReferenceFace(*this, model, proc_, [this](Widget* widget, int parameter) { tag(widget, parameter); })))
            return;
        if (auto* lfo = dynamic_cast<devices::CurveLfo*>(proc_))
        {
            curveLfo_ = add<CurveLfoEditor>(lfo, [this](Widget* widget, int parameter) { tag(widget, parameter); });
            return;
        }
        if (DeviceFace::supports(proc_))
        {
            deviceFace_ = add<DeviceFace>(model, proc_, trackIndex, deviceIndex, chain, child,
                                         [this](Widget* widget, int parameter) { tag(widget, parameter); });
            if (deviceFace_->kind() == DeviceFace::Kind::Simpler)
                for (int page = 0; page < 2; ++page)
                {
                    auto* button = add<SimplerTab>(page == 0 ? "Sample" : "Controls");
                    button->isOn = [this, page] { return deviceFace_->selectedPage() == page; };
                    button->onClick = [this, page] { deviceFace_->selectPage(page); repaint(); };
                    sampleTabs_.push_back(button);
                }
            return;
        }
        if (chain < 0 && dynamic_cast<devices::RackProcessor*>(proc_))
            nativePanel_ = add<NativeDevicePanel>(model, trackIndex, deviceIndex, chain, child);
        if (auto* lfo = dynamic_cast<devices::LfoShaper*>(proc_)) lfoCurve_ = add<LfoCurveEditor>(lfo);
        if (auto* synth = dynamic_cast<devices::WavetableSynth*>(proc_))
        {
            // UI-27: the dashboard floats in its own window unless this instance was docked; the chain keeps a stub.
            if (floatingWavetable)
            {
                wavetableFloating_ = true;
                wavetableOpen_ = add<IconButton>(IconButton::Icon::Wrench);
                wavetableOpen_->onClick = [this] { showWavetableWindow(); };
                wavetableShow_ = add<TextButton>("Show Editor", TextButton::Style::Plain);
                wavetableShow_->onClick = [this] { showWavetableWindow(); };
                wavetableDock_ = add<TextButton>("Dock", TextButton::Style::Plain);
                wavetableDock_->onClick = [this] { placeWavetable(false); };
                return;
            }
            wavetable_ = add<WavetableEditor>(synth, trackIndex, deviceIndex, chain, child);
            wavetableFloat_ = add<IconButton>(IconButton::Icon::FloatWindow);
            wavetableFloat_->onClick = [this] { placeWavetable(true); };
            return;
        }

        if (const auto groups = proc_->uiGroups(); ! groups.empty())
        {
            buildGroups(groups);
            if (dynamic_cast<devices::Delay*>(proc_) || dynamic_cast<devices::Reverb*>(proc_))
            {
                effectPanel_ = add<EffectPanel>(proc_, trackIndex_, deviceIndex_, chain_ >= 0); // the title's menu icon expands it
            }
        }
        else if (pluginPanel_ != nullptr)
        {
            // The hosted wrapper exposes parameters through the X/Y assignment menus.
        }
        else if (! proc_->parameters().empty())
        {
            // Any other device (racks: their macros): generic controls for its display parameters.
            for (int p : proc_->displayParameters())
            {
                const auto& info = proc_->parameters()[(size_t) p];
                if (nativePanel_ && p == devices::RackProcessor::kChainSelector)
                {
                    rackSelector_ = add<RackSelector>(proc_);
                    tag(rackSelector_, p);
                }
                else if (info.isToggle)
                {
                    auto* b = add<TextButton>(info.name);
                    tag(b, p);
                    b->isOn = [this, p] { return proc_->getParameter(p) > 0.5f; };
                    b->onClick = [this, p] { proc_->setParameter(p, proc_->getParameter(p) > 0.5f ? 0.0f : 1.0f); };
                    toggles_.push_back(b);
                }
                else
                {
                    knobs_.push_back(add<Knob>(info.name, paramSource(proc_, p)));
                    knobs_.back()->style = Knob::Style::Device; // UI-42: the reference's device knob
                    tag(knobs_.back(), p);
                }
            }
        }
        if (knobs_.size() > (nativePanel_ ? 8u : 12u))
        {
            pageButton_ = add<TextButton>("Page 1");
            pageButton_->onClick = [this] {
                const size_t count = nativePanel_ ? 8u : 12u;
                const size_t macros = knobs_.size();
                knobPage_ = (knobPage_ + 1) % (int) ((macros + count - 1) / count); layout(); repaint();
            };
        }
    }

    // ---- sectioned panels (Delay, Reverb): captioned groups of knobs, toggles and cycle buttons
    static constexpr float kGroupCaption = 14.0f, kKnobWidth = 58.0f, kButtonWidth = 64.0f, kButtonRow = 19.0f,
                           kGroupGap = 14.0f;

    struct GroupUi
    {
        std::string title;
        std::vector<int> params;
        std::vector<Widget*> widgets;
        std::vector<bool> isKnob;
        int knobColumns = 0;
        Rect bounds;
    };

    void buildGroups(const std::vector<DeviceProcessor::UiGroup>& groups)
    {
        for (const auto& g : groups)
        {
            GroupUi ui;
            ui.title = g.title;
            ui.knobColumns = g.knobColumns;
            for (int p : g.params)
            {
                const auto& info = proc_->parameters()[(size_t) p];
                Widget* w = nullptr;
                bool knob = false;
                if (info.isToggle)
                {
                    auto* b = add<TextButton>(info.name);
                    b->isOn = [this, p] { return proc_->getParameter(p) > 0.5f; };
                    b->onClick = [this, p] {
                        const float next = proc_->getParameter(p) > 0.5f ? 0.0f : 1.0f;
                        if (auto* delay = dynamic_cast<devices::Delay*>(proc_)) delay->setParameterLinked(p, next);
                        else proc_->setParameter(p, next);
                        layout();
                        repaint();
                    };
                    w = b;
                }
                else if (! info.choices.empty() && info.choices.size() <= 7)
                {
                    auto* b = add<TextButton>(info.choices.front(), TextButton::Style::Plain);
                    b->onClick = [this, p] {
                        const auto count = (int) proc_->parameters()[(size_t) p].choices.size();
                        proc_->setParameter(p, (float) (((int) std::lround(proc_->getParameter(p)) + 1) % count));
                        layout();
                        repaint();
                    };
                    w = b;
                }
                else
                {
                    auto* dial = add<Knob>(info.name, paramSource(proc_, p));
                    dial->style = Knob::Style::Device; // UI-42: the reference's device knob
                    w = dial;
                    knob = true;
                }
                tag(w, p);
                ui.params.push_back(p);
                ui.widgets.push_back(w);
                ui.isKnob.push_back(knob);
            }
            groups_.push_back(std::move(ui));
        }
    }

    // Knob columns a group currently needs (hidden controls take no room).
    int knobColumnsOf(const GroupUi& g) const
    {
        int knobs = 0;
        for (size_t i = 0; i < g.params.size(); ++i)
            if (g.isKnob[i] && proc_->parameterShown(g.params[i]))
                ++knobs;
        return g.knobColumns > 0 ? g.knobColumns : std::max(3, (knobs + 1) / 2);
    }
    // A group is never narrower than three columns, so the buttons below have room for two slots per row.
    int groupColumns(const GroupUi& g) const { return std::max(3, knobColumnsOf(g)); }

    bool isCycleButton(const GroupUi& g, size_t i) const
    {
        const auto& info = proc_->parameters()[(size_t) g.params[i]];
        return ! g.isKnob[i] && ! info.isToggle && ! info.choices.empty();
    }

    float groupsWidth() const
    {
        float w = 12.0f;
        for (const auto& g : groups_)
        {
            w += (float) groupColumns(g) * kKnobWidth + kGroupGap;
        }
        return w;
    }

    std::string groupSignature() const
    {
        std::string sig;
        for (const auto& g : groups_)
            for (size_t i = 0; i < g.params.size(); ++i)
            {
                sig += proc_->parameterShown(g.params[i]) ? '1' : '0';
                if (! g.isKnob[i] && ! proc_->parameters()[(size_t) g.params[i]].isToggle)
                    sig += std::to_string((int) std::lround(proc_->getParameter(g.params[i])));
            }
        return sig;
    }

    void layoutGroups(Rect r)
    {
        float x = r.x;
        for (auto& g : groups_)
        {
            const int cols = groupColumns(g), knobCols = knobColumnsOf(g);
            const float w = (float) cols * kKnobWidth;
            const int perRow = std::max(1, (int) (w / kButtonWidth));
            // Button slots: a cycle button ("Name: choice") spans two slots, a toggle one.
            struct Slot { size_t index; int row, col, span; };
            std::vector<Slot> slots;
            int row = 0, col = 0;
            for (size_t i = 0; i < g.params.size(); ++i)
            {
                if (g.isKnob[i] || ! proc_->parameterShown(g.params[i]))
                    continue;
                const int span = isCycleButton(g, i) && perRow >= 2 ? 2 : 1;
                if (col + span > perRow)
                {
                    ++row;
                    col = 0;
                }
                slots.push_back({i, row, col, span});
                col += span;
            }
            const int rows = slots.empty() ? 0 : row + 1;
            const float knobHeight = std::clamp((r.h - kGroupCaption - 6.0f - (float) rows * kButtonRow) / 2.0f, 36.0f, 62.0f);
            g.bounds = {x, r.y, w, r.h};
            int knobIndex = 0;
            for (size_t i = 0; i < g.params.size(); ++i)
            {
                auto* widget = g.widgets[i];
                widget->visible = proc_->parameterShown(g.params[i]);
                if (widget->visible && g.isKnob[i])
                {
                    widget->setBounds({x + (float) (knobIndex % knobCols) * kKnobWidth,
                                       r.y + kGroupCaption + (float) (knobIndex / knobCols) * knobHeight, kKnobWidth, knobHeight});
                    ++knobIndex;
                }
            }
            const float buttonsTop = r.y + kGroupCaption + 2.0f * knobHeight + 6.0f;
            for (const auto& slot : slots)
            {
                auto* widget = g.widgets[slot.index];
                if (isCycleButton(g, slot.index))
                    if (auto* b = dynamic_cast<TextButton*>(widget))
                    {
                        const auto& info = proc_->parameters()[(size_t) g.params[slot.index]];
                        b->setLabel(info.name + ": " + info.choices[(size_t) std::clamp((int) std::lround(proc_->getParameter(g.params[slot.index])), 0,
                                                                                         (int) info.choices.size() - 1)]);
                    }
                widget->setBounds({x + (float) slot.col * kButtonWidth, buttonsTop + (float) slot.row * kButtonRow,
                                   (float) slot.span * kButtonWidth - 3.0f, 16.0f});
            }
            x += w + kGroupGap;
        }
        shapeSignature_ = groupSignature();
    }

    void tick() override
    {
        if (! groups_.empty() && groupSignature() != shapeSignature_)
        {
            layout();
            repaint();
        }
    }

    // At most 2 rows of knobs fit in a device box.
    int knobColumns() const { return std::clamp((int) std::ceil((double) std::min<size_t>(12, knobs_.size()) / 2.0), 4, 6); }

    // Makes a control MIDI-mappable to parameter `index` of this device.
    void tag(Widget* w, int index)
    {
        if (chain_ >= 0)
        {
            if (auto* knob = dynamic_cast<Knob*>(w))
            {
                const auto info = proc_->parameters()[(size_t) index];
                knob->onRightClick = [this, info] {
                    const int macro = ctx().rackMacroToMap, chain = chain_, child = child_;
                    if (ctx().editDevice) ctx().editDevice(trackIndex_, deviceIndex_, [macro, chain, child, info](auto& rack) {
                        if ((size_t) chain >= rack.chains.size() || (size_t) child >= rack.chains[(size_t) chain].devices.size()) return;
                        auto& device = rack.chains[(size_t) chain].devices[(size_t) child];
                        if (std::none_of(device.parameters.begin(), device.parameters.end(), [&](const auto& p) { return p.path == info.id; }))
                        { model::Parameter p; p.path = info.id; p.value = info.defaultValue; p.booleanValue = info.isToggle; device.parameters.push_back(std::move(p)); }
                        for (auto& p : device.parameters) if (p.path == info.id)
                        {
                            p.macroIndex = macro; p.rangeMin = info.minValue; p.rangeMax = info.maxValue;
                            p.midi.channel = 16; p.midi.isNote = false; p.midi.number = macro;
                            p.macroMappingEdited = true;
                        }
                        rack.chainsEdited = true;
                    }, "Map Rack Macro");
                };
            }
            return;
        }
        w->midiTarget = midi::ControlTarget{midi::ControlTarget::Kind::DeviceParameter, trackIndex_, deviceIndex_, 0,
                                            proc_->parameters()[(size_t) index].id};
    }

    bool isMidiEffect() const { return model::deviceRole(model_) == model::DeviceRole::MidiEffect; }
    IconButton* wavetableFloatButton() const { return wavetableFloat_; }

    // Floating <-> docked for this Wavetable instance. The chain rebuilds on the next layout (placement revision), so
    // nothing here touches this box after the change.
    void placeWavetable(bool floating)
    {
        auto& x = ctx();
        const auto key = x.wavetableKey(trackIndex_, deviceIndex_, chain_, child_);
        if (! key) return;
        x.setWavetableFloating(*key, floating);
        if (floating && x.openWavetableWindow) x.openWavetableWindow(*key);
        if (! floating && x.closeWavetableWindow) x.closeWavetableWindow(*key);
        if (x.requestRepaint) x.requestRepaint();
        repaint();
    }
    void showWavetableWindow()
    {
        auto& x = ctx();
        if (const auto key = x.wavetableKey(trackIndex_, deviceIndex_, chain_, child_); key && x.openWavetableWindow)
            x.openWavetableWindow(*key);
    }
    int deviceIndex() const { return chain_ < 0 ? deviceIndex_ : -1; }

    float preferredWidth() const
    {
        if (pluginPanel_) return 150;
        if (referenceFace_) return referenceFace_->preferredWidth(height() > kTitle ? height() - kTitle : ReferenceFace::kDefaultBody);
        if (deviceFace_) return deviceFace_->preferredWidth() + 8;
        if (resynth_) return ResynthEditor::preferredWidth + 12;
        if (effectPanel_ && ! expandedControls_) return effectPanel_->preferredWidth() + 8;
        if (nativePanel_) return model_.type == "DrumGroupDevice" ? 798.0f : 598.0f;
        if (lfoCurve_) return 730;
        if (curveLfo_) return CurveLfoEditor::kPreferredWidth + 12;
        if (wavetableFloating_) return 150;
        if (wavetable_) return WavetableEditor::kPreferredWidth + 12;
        if (proc_ == nullptr)
            return 150;
        if (! groups_.empty())
            return groupsWidth();
        if (editorButton_ != nullptr || ! knobs_.empty())
            return (float) knobColumns() * 58 + 16;
        return 160;
    }

    float fixedHeight() const
    {
        return resynth_ ? ResynthEditor::preferredHeight + kTitle + 12.0f : 0;
    }

    void layout() override
    {
        auto r = localBounds();
        auto title = r.removeFromTop(kTitle);
        if (onButton_)
            onButton_->setBounds(Rect{title.x + 3, title.y + 2, 13, 13});
        if (referenceFace_)
        {
            referenceFace_->setBounds({0, kTitle, width(), height() - kTitle}); // the face draws the whole body
            return;
        }
        r = (pluginPanel_ || effectPanel_ || deviceFace_) ? r.reduced(4, 2) : r.reduced(6, 6);
        if (resynth_)
        {
            resynth_->setBounds({r.x, r.y, ResynthEditor::preferredWidth, ResynthEditor::preferredHeight});
            return;
        }
        if (curveLfo_) { curveLfo_->setBounds(r); return; }
        if (deviceFace_)
        {
            for (size_t i = 0; i < sampleTabs_.size(); ++i)
                sampleTabs_[i]->setBounds({width() - kTitleIcons - 152 + (float) i * 56, 1, 54, 16});
            deviceFace_->setBounds(r); return;
        }
        if (pluginPanel_)
        {
            pluginPanel_->setBounds(r);
            if (editorButton_) editorButton_->setBounds({35, 2, 14, 14}); // centred on the ring at (42, 9)
            return;
        }
        if (effectPanel_)
        {
            effectPanel_->visible = ! expandedControls_;
            for (auto& group : groups_) for (auto* widget : group.widgets) widget->visible = expandedControls_;
            if (! expandedControls_) { shapeSignature_ = groupSignature(); effectPanel_->setBounds(r); return; }
        }
        if (wavetableFloating_)
        {
            wavetableOpen_->setBounds({width() - kTitleIcons - 21, 1, 17, 16});
            wavetableShow_->setBounds({r.x, r.y + 22, r.w, 18});
            wavetableDock_->setBounds({r.x, r.y + 44, r.w, 18});
            return;
        }
        if (wavetable_)
        {
            wavetableFloat_->setBounds({width() - kTitleIcons - 21, 1, 17, 16});
            wavetable_->setBounds(r);
            return;
        }
        if (pageButton_)
        {
            const size_t count = nativePanel_ ? 8u : 12u;
            const size_t macros = knobs_.size();
            pageButton_->setLabel(std::string(nativePanel_ ? "Macros " : "Page ") + std::to_string(knobPage_ + 1) + "/" + std::to_string((macros + count - 1) / count));
            pageButton_->setBounds({width() - kTitleIcons - 78, 1, 74, 16});
        }

        if (! groups_.empty())
        {
            layoutGroups(r);
            return;
        }
        if (editorButton_)
            editorButton_->setBounds(r.removeFromTop(20).withWidth(28));
        if (nativePanel_)
        {
            nativePanel_->setBounds(r.removeFromRight(model_.type == "DrumGroupDevice" ? 536.0f : 336.0f));
            const float kh = std::min(68.0f, (r.h - 46) * 0.5f);
            const size_t macros = knobs_.size();
            for (size_t i = 0; i < macros; ++i)
            {
                auto* knob = knobs_[i]; knob->visible = i / 8 == (size_t) knobPage_;
                if (knob->visible) knob->setBounds({r.x + (float) (i % 4) * 58, r.y + 20 + (float) ((i % 8) / 4) * kh, 56, kh});
            }
            if (rackSelector_) rackSelector_->setBounds({r.x, r.bottom() - 18, 228, 18});
            return;
        }
        if (lfoCurve_) { lfoCurve_->setBounds(r.removeFromRight(std::max(80.0f, r.w - 302))); r.removeFromRight(4); }
        const float knobHeight = std::min(62.0f, std::max(32.0f, (r.h - (toggles_.empty() ? 0.0f : 24.0f)) / 2));
        auto knobsArea = r.removeFromTop(std::min(r.h, knobHeight));
        const float kw = 58;
        const size_t cols = (size_t) knobColumns();
        for (size_t i = 0; i < knobs_.size(); ++i)
        {
            knobs_[i]->visible = i / 12 == (size_t) knobPage_;
            if (! knobs_[i]->visible) continue;
            const size_t visibleIndex = i % 12;
            if (visibleIndex > 0 && visibleIndex % cols == 0)
                knobsArea = r.removeFromTop(std::min(r.h, knobHeight));
            knobs_[i]->setBounds(knobsArea.removeFromLeft(kw));
        }
        r.removeFromTop(6);
        auto row = r.removeFromTop(16);
        for (auto* tgl : toggles_)
        {
            if (row.w < 52)
                row = r.removeFromTop(16);
            tgl->setBounds(row.removeFromLeft(52));
            row.removeFromLeft(3);
        }
    }

    // Any press inside the box (title, knobs, buttons) selects the device, so Delete removes the one being worked on.
    void descendantPressed(Widget*) override
    {
        if (chain_ >= 0) return;
        ctx().selectedTrack = trackIndex_;
        ctx().statusText = model_.displayName;
        if (ctx().selectedDeviceId != model_.id) { ctx().selectedDeviceId = model_.id; repaint(); }
    }

    bool showsTitleIcons() const { return chain_ < 0 && width() >= 120; }
    // Hot-swap and preset saving do not exist yet (UI-42b): the status bar says so. The menu circle opens the device
    // menu, or (Delay / Reverb) shows the full parameter set, as the reference's menu does for those devices.
    void titleIconClicked(int icon, const MouseEvent& e)
    {
        if (icon == 0) { ctx().statusText = "Hot-swapping presets is not available yet"; repaint(); return; }
        if (icon == 1) { ctx().statusText = "Saving device presets is not available yet"; repaint(); return; }
        if (effectPanel_)
        {
            expandedControls_ = ! expandedControls_;
            if (parent_) parent_->layout();
            repaint();
            return;
        }
        auto menu = e;
        menu.right = true;
        mouseDown(menu);
    }

    void mouseDown(const MouseEvent& e) override
    {
        if (chain_ >= 0) return;
        if (! e.right && e.y < kTitle && showsTitleIcons())
            for (int i = 0; i < 3; ++i)
                if (std::hypot(e.x - (width() - kTitleIconRight[i]), e.y - kTitle * 0.5f) <= kTitleIconSize * 0.5f + 1)
                {
                    titleIconClicked(i, e);
                    return;
                }
        if (! e.right)
        {
            titlePressed_ = e.y < kTitle;
            if (titlePressed_)
            {
                dragOriginX_ = bounds_.x; dragOriginY_ = bounds_.y;
                if (auto* detail = dynamic_cast<DetailView*>(parent_))
                    detail->beginDeviceDrag(model_.id, dragOriginX_ + e.x, dragOriginY_ + e.y);
            }
            return;
        }
        auto* context = &ctx();
        juce::PopupMenu menu;
        const auto* song = context->song;
        const int track = trackIndex_, id = model_.id;
        for (const auto* label : {"Cut", "Copy", "Paste", "Duplicate"}) menu.addItem(juce::PopupMenu::Item(label).setEnabled(false));
        menu.addItem(1, "Delete", ! context->recording && ! context->recordingRequested);
        for (const auto* label : {"Rename", "Edit Info Text"}) menu.addItem(juce::PopupMenu::Item(label).setEnabled(false));
        menu.addSeparator();
        menu.addItem(juce::PopupMenu::Item("Group").setEnabled(false));
        menu.addItem(juce::PopupMenu::Item("Save as Default Preset").setEnabled(false));
        menu.showMenuAsync(juce::PopupMenu::Options(), [context, song, track, id](int result) {
            if (result != 1 || context->song != song || context->recording || context->recordingRequested) return;
            context->selectedTrack = track; context->selectedDeviceId = id;
            if (context->deleteSelectedDevice() && context->modelChanged) context->modelChanged();
        });
    }

    void mouseDrag(const MouseEvent& e) override
    {
        if (titlePressed_)
            if (auto* detail = dynamic_cast<DetailView*>(parent_)) detail->updateDeviceDrag(dragOriginX_ + e.x, dragOriginY_ + e.y);
    }

    void mouseUp(const MouseEvent& e) override
    {
        if (! titlePressed_) return;
        titlePressed_ = false;
        // Committing can destroy this box. No member access after forwarding the release.
        if (auto* detail = dynamic_cast<DetailView*>(parent_)) detail->endDeviceDrag(dragOriginX_ + e.x, dragOriginY_ + e.y);
    }

    void paint(SkCanvas& c, const Theme& t) override
    {
        auto r = localBounds();
        // The Swiss Army Meter (a third-party Max patch) keeps its own dark body in every theme, as in the original.
        const bool darkBody = deviceFace_ && deviceFace_->kind() == DeviceFace::Kind::Meter;
        paint::fillRounded(c, r, 3, darkBody ? SkColorSetRGB(0x33, 0x33, 0x33) : t.color("SurfaceBackground"));
        auto title = r.removeFromTop(kTitle);
        const bool picked = chain_ < 0 && ctx().selectedDeviceId == model_.id && ctx().selectedTrack == trackIndex_;
        // Every device's title band is lighter than its body in the reference (native and plugin alike).
        const SkColor band = picked ? paint::mix(t.color("SurfaceHighlight"), t.color("ChosenAlternative"), 0.2f) : t.color("SurfaceHighlight");
        paint::fillRounded(c, title, 3, band); // rounded top corners, square where it meets the body
        paint::fill(c, {title.x, title.y + 3, title.w, title.h - 3}, band);
        const Rect nameRect = pluginPanel_ ? Rect{52, 0, width() - 56 - kTitleIcons, kTitle} : Rect{20, 0, width() - 24 - kTitleIcons, kTitle};
        if (pluginPanel_)
        {
            ctl::triangle(c, 24, 9, 7, 6, ctl::Dir::Down, t.color("ControlForeground"));
            SkPaint ring;
            ring.setAntiAlias(true); ring.setStyle(SkPaint::kStroke_Style); ring.setStrokeWidth(1); ring.setColor(t.color("ControlContrastFrame"));
            c.drawCircle(24, 9, 6.5f, ring);
            c.drawCircle(42, 9, 6.5f, ring);
        }
        const auto name = deviceFace_ && deviceFace_->kind() == DeviceFace::Kind::Simpler && ! model_.sampleParts.empty()
            && (model_.displayName == "Simpler" || model_.displayName.empty()) ? model_.sampleParts[0].name : model_.displayName;
        paint::text(c, name, sampleTabs_.empty() ? nameRect : Rect{28, 0, width() - kTitleIcons - 184, kTitle}, proc_ ? t.color("ControlForeground") : t.color("TextDisabled"),
                    t.metrics.fontSize, Align::Left, true);
        if (showsTitleIcons())
            for (int i = 0; i < 3; ++i)
                titleIcon(c, (TitleIcon) i, width() - kTitleIconRight[i], kTitle * 0.5f, kTitleIconSize,
                          i == 2 && effectPanel_ && expandedControls_ ? t.color("ChosenDefault") : t.color("ControlForeground"));
        if (pluginPanel_) paint::stroke(c, localBounds(), t.color("ControlContrastFrame"));
        for (size_t gi = 0; gi < (effectPanel_ && ! expandedControls_ ? 0 : groups_.size()); ++gi)
        {
            const auto& g = groups_[gi];
            paint::text(c, g.title, {g.bounds.x, g.bounds.y, g.bounds.w, kGroupCaption},
                        t.color("TextDisabled"), t.metrics.smallFontSize, Align::Left, true);
            if (gi + 1 < groups_.size())
                paint::fill(c, {g.bounds.x + g.bounds.w + kGroupGap * 0.5f - 0.5f, g.bounds.y, 1.0f, g.bounds.h},
                            t.color("ControlContrastFrame"));
        }

        if (proc_ == nullptr && ! pluginPanel_)
        {
            std::string why = model_.kind == model::DeviceKind::MaxForLive ? "Max for Live is not supported"
                              : model_.plugin ? "Plugin not found / failed to load"
                                              : "Not implemented yet";
            paint::text(c, why, r.reduced(8, 0).withHeight(40), t.color("TextDisabled"), t.metrics.fontSize);
            paint::text(c, model_.type, r.reduced(8, 0).translated(0, 22).withHeight(40), t.color("TextDisabled"),
                        t.metrics.smallFontSize);
        }
        else if (auto* rack = dynamic_cast<devices::RackProcessor*>(proc_); rack && ! nativePanel_)
        {
            const int knobRows = knobs_.empty() ? 0 : (int) std::ceil((double) knobs_.size() / (double) knobColumns());
            float y = r.y + 12 + (float) knobRows * 62.0f + (toggles_.empty() ? 0.0f : 18.0f);
            int i = 0;
            for (const auto& chain : model_.chains)
            {
                (void) rack;
                paint::text(c, chain.name.empty() ? "Chain " + std::to_string(i + 1) : chain.name,
                            {r.x + 8, y, r.w - 16, 16}, t.color("ControlForeground"), t.metrics.fontSize);
                y += 16;
                if (++i > 10)
                    break;
            }
        }
        else if (wavetableFloating_)
        {
            paint::text(c, "Editor in its own window", r.reduced(6, 6).withHeight(16), t.color("TextDisabled"), t.metrics.smallFontSize);
        }
        else if (proc_ && proc_->name().find("(placeholder)") != std::string::npos)
        {
            paint::text(c, "Placeholder sine synth", r.reduced(8, 0).withHeight(30), t.color("ChosenAlert"),
                        t.metrics.fontSize);
        }
    }

private:
    bool titlePressed_ = false;
    float dragOriginX_ = 0, dragOriginY_ = 0;
    const model::Device& model_;
    DeviceProcessor* proc_;
    int trackIndex_ = 0, deviceIndex_ = 0;
    int chain_ = -1, child_ = -1, knobPage_ = 0;
    EffectPanel* effectPanel_ = nullptr;
    PluginPanel* pluginPanel_ = nullptr;
    bool expandedControls_ = false;
    NativeDevicePanel* nativePanel_ = nullptr;
    DeviceFace* deviceFace_ = nullptr;
    ReferenceFace* referenceFace_ = nullptr;
    ResynthEditor* resynth_ = nullptr;
    std::vector<TextButton*> sampleTabs_;
    Widget* rackSelector_ = nullptr;
    LfoCurveEditor* lfoCurve_ = nullptr;
    CurveLfoEditor* curveLfo_ = nullptr;
    WavetableEditor* wavetable_ = nullptr;
    bool wavetableFloating_ = false;
    IconButton *wavetableFloat_ = nullptr, *wavetableOpen_ = nullptr;
    TextButton *wavetableShow_ = nullptr, *wavetableDock_ = nullptr;
    TextButton* pageButton_ = nullptr;
    TextButton* onButton_ = nullptr;
    IconButton* editorButton_ = nullptr;
    std::vector<GroupUi> groups_;
    std::string shapeSignature_;
    std::vector<Knob*> knobs_;
    std::vector<TextButton*> toggles_;
};
} // namespace

// UI-27: a Sonora Prism shows a stub in the chain while its editor floats in its own window.
static bool floatsWavetable(const UiContext& x, int device, int chain, int child)
{
    const auto* track = x.trackAt(x.selectedTrack);
    if (track == nullptr || device < 0 || (size_t) device >= track->devices.size()) return false;
    const auto* model = &track->devices[(size_t) device];
    if (chain >= 0)
    {
        if ((size_t) chain >= model->chains.size() || child < 0 || (size_t) child >= model->chains[(size_t) chain].devices.size()) return false;
        model = &model->chains[(size_t) chain].devices[(size_t) child];
    }
    if (model->type != "SonoraWavetable") return false;
    const auto key = x.wavetableKey(x.selectedTrack, device, chain, child);
    return key && x.isWavetableFloating(*key);
}

void DetailView::rebuild()
{
    cancelDeviceDrag();
    removeAllChildren();
    panelSplit_ = nullptr;
    boxes_.clear();
    auto& x = ctx();
    builtSong_ = x.song;
    builtGraph_ = x.graph;
    builtStructure_ = x.trackStructure();
    builtTrack_ = x.selectedTrack;
    builtTab_ = (int) x.detailTab;
    builtRack_ = x.selectedRackId; builtChain_ = x.selectedRackChain;
    builtPlacement_ = x.wavetablePlacementRevision;
    if (scrollTrack_ != x.selectedTrack)
        scrollX_ = 0; // another track starts at its first device; edits on the same track keep the scroll position
    scrollTrack_ = x.selectedTrack;

    if (x.detailTab == DetailTab::Clip)
    {
        add<ClipControls>();
        add<PianoRoll>();
        add<MpeEditor>();
        add<ClipEnvelopeEditor>();
        add<SampleView>();
        auto* split = add<Splitter>(Splitter::Orientation::Vertical);
        split->getSize = [this] { return ctx().theme.metrics.clipPanelWidth * (float) clipColumns_; };
        split->setSize = [this](float w) {
            ctx().theme.metrics.clipPanelWidth = std::clamp(w / (float) clipColumns_, 150.0f, std::max(150.0f, width() * 0.6f));
            layout();
            repaint();
        };
        split->finished = [this] { if (ctx().layoutChanged) ctx().layoutChanged(); };
        panelSplit_ = split;
        return;
    }

    if (x.detailTab == DetailTab::Monitor)
    {
        add<MonitorPanel>();
        return;
    }

    const auto* track = x.trackAt(x.selectedTrack);
    if (track == nullptr)
        return;
    const engine::TrackNode* node = nullptr;
    if (x.graph != nullptr && (size_t) x.selectedTrack < x.graph->nodes.size())
        node = &x.graph->nodes[(size_t) x.selectedTrack];

    for (size_t i = 0; i < track->devices.size(); ++i)
    {
        DeviceProcessor* proc = node && i < node->processorForModelDevice.size() ? node->processorForModelDevice[i]
                                                                                  : nullptr;
        boxes_.push_back(add<DeviceBox>(track->devices[i], proc, x.selectedTrack, (int) i, -1, -1, floatsWavetable(x, (int) i, -1, -1)));
        const auto& model = track->devices[i];
        auto* rack = dynamic_cast<devices::RackProcessor*>(proc);
        if (rack && model.id == x.selectedRackId && x.selectedRackChain >= 0)
        {
            const auto ci = (size_t) x.selectedRackChain;
            if (ci < model.chains.size() && ci < rack->chains().size())
                for (size_t di = 0; di < model.chains[ci].devices.size(); ++di)
                {
                    const auto& processors = rack->chains()[ci].processorForModelDevice;
                    auto* child = di < processors.size() ? processors[di] : nullptr;
                    boxes_.push_back(add<DeviceBox>(model.chains[ci].devices[di], child, x.selectedTrack, (int) i, (int) ci, (int) di,
                                                    floatsWavetable(x, (int) i, (int) ci, (int) di)));
                }
        }
    }
}

void DetailView::layout()
{
    auto& x = ctx();
    if (builtSong_ != x.song || builtGraph_ != x.graph || builtTrack_ != x.selectedTrack || builtTab_ != (int) x.detailTab
        || builtStructure_ != x.trackStructure() || builtRack_ != x.selectedRackId || builtChain_ != x.selectedRackChain
        || builtPlacement_ != x.wavetablePlacementRevision)
        rebuild();
    if (x.detailTab == DetailTab::Clip)
    {
        const auto* clip = x.selectedClip();
        const bool audio = clip && clip->kind == model::Clip::Kind::Audio;
        clipColumns_ = 1;
        for (auto& child : children_)
            if (auto* panel = dynamic_cast<ClipControls*>(child.get()))
                clipColumns_ = std::max(1, panel->columnsFor(height() - 22));
        for (auto& child : children_)
        {
            const bool sidebarPanel = dynamic_cast<ClipControls*>(child.get()) != nullptr;
            const bool envelopes = dynamic_cast<ClipEnvelopeEditor*>(child.get()) != nullptr;
            const bool mpe = dynamic_cast<MpeEditor*>(child.get()) != nullptr;
            if (child.get() == panelSplit_)
            {
                child->visible = true;
                child->setBounds({sidebarWidth() - 3, 22, 6, height() - 22});
                continue;
            }
            child->visible = sidebarPanel ? true
                                          : (dynamic_cast<SampleView*>(child.get()) ? audio : ! audio && (x.clipMpeVisible ? mpe : ! mpe && envelopes == x.clipEnvelopesVisible));
            const float sidebar = sidebarWidth();
            child->setBounds(sidebarPanel ? Rect{0, 22, sidebar, height() - 22}
                                          : Rect{sidebar, 22, width() - sidebar, height() - 22});
        }
        return;
    }
    if (x.detailTab == DetailTab::Monitor)
    {
        for (auto& child : children_)
            child->setBounds({0, 22, width(), height() - 22});
        return;
    }
    // A newly added device scrolls into view (the chain can be wider than the view), like Live.
    if ((int) boxes_.size() > lastBoxCount_ && lastBoxCount_ >= 0)
        scrollX_ = std::max(0.0f, contentWidth() - width() + 12);
    lastBoxCount_ = (int) boxes_.size();
    scrollX_ = std::clamp(scrollX_, 0.0f, std::max(0.0f, contentWidth() - width() + 12));
    // Faces that scale with their height size their width from it, and the scrollbar (which takes height) depends on
    // the total width: settle the heights first, then place every box with the same height.
    for (int pass = 0; pass < 2; ++pass)
    {
        const float chainHeight = height() - 6 - (scrollbarVisible() ? 7.0f : 0.0f);
        for (auto* b : boxes_)
        {
            const float fixedHeight = static_cast<DeviceBox*>(b)->fixedHeight();
            b->setBounds({b->bounds().x, 3, b->width(), fixedHeight > 0 ? fixedHeight : chainHeight});
        }
    }
    float px = kChainStart - scrollX_;
    bool placedInstrumentSlot = false;
    for (auto* b : boxes_)
    {
        if (! placedInstrumentSlot && ! static_cast<DeviceBox*>(b)->isMidiEffect())
        {
            px += instrumentSlotWidth();
            placedInstrumentSlot = true;
        }
        const float w = static_cast<DeviceBox*>(b)->preferredWidth();
        b->setBounds({px, 3, w, b->height()});
        b->cornerRadius = 3;
        px += w + kDeviceGap;
    }
}

float DetailView::chainWidth() const
{
    float total = instrumentSlotWidth();
    for (auto* b : boxes_)
        total += static_cast<DeviceBox*>(b)->preferredWidth() + kDeviceGap;
    return total;
}

float DetailView::contentWidth() const { return chainWidth() + 180.0f; }

bool DetailView::scrollbarVisible() const
{
    return ctx().detailTab == DetailTab::Devices && ! boxes_.empty() && contentWidth() + 6 > width();
}

// The thumb of the horizontal scrollbar along the bottom edge of the device chain.
Rect DetailView::scrollbarThumb() const
{
    const float trackW = std::max(1.0f, width() - 12.0f);
    const float visible = std::clamp(width() / std::max(1.0f, contentWidth() + 6), 0.05f, 1.0f);
    const float range = std::max(1.0f, contentWidth() - width() + 12);
    const float thumbW = std::max(30.0f, trackW * visible);
    const float x = 6 + (trackW - thumbW) * std::clamp(scrollX_ / range, 0.0f, 1.0f);
    return {x, height() - 8.0f, thumbW, 5.0f};
}

void DetailView::tick()
{
    auto& x = ctx();
    if (deviceDragging_ && dragY_ >= 0 && dragY_ < height())
    {
        const float amount = dragX_ < 28 ? -18.0f : dragX_ > width() - 28 ? 18.0f : 0.0f;
        const float next = std::clamp(scrollX_ + amount, 0.0f, std::max(0.0f, contentWidth() - width() + 12));
        if (std::abs(next - scrollX_) > 0.01f) { scrollX_ = next; layout(); updateDeviceDropTarget(); repaint(); }
    }
    if (x.monitor != nullptr)
        x.monitor->setEnabled(visible && x.detailTab == DetailTab::Monitor); // the analysis thread only runs while it is watched
    if (builtSong_ != x.song || builtGraph_ != x.graph || builtTrack_ != x.selectedTrack || builtTab_ != (int) x.detailTab
        || builtStructure_ != x.trackStructure() || builtRack_ != x.selectedRackId || builtChain_ != x.selectedRackChain)
    {
        layout();
        repaint();
    }
}

void DetailView::mouseDrag(const MouseEvent& e)
{
    if (! draggingScrollbar_)
        return;
    const float trackW = std::max(1.0f, width() - 12.0f);
    const float thumbW = scrollbarThumb().w;
    const float range = std::max(1.0f, contentWidth() - width() + 12);
    scrollX_ = std::clamp(scrollGrabX_ + (e.x - scrollGrabMouseX_) * range / std::max(1.0f, trackW - thumbW), 0.0f, range);
    layout();
    repaint();
}

void DetailView::mouseUp(const MouseEvent&) { draggingScrollbar_ = false; }

float DetailView::sidebarWidth() const
{
    // The clip panel's boxes flow into extra columns when the view is short (ClipControls::columnsFor).
    const float column = std::clamp(ctx().theme.metrics.clipPanelWidth, 150.0f, std::max(150.0f, width() * 0.6f));
    return std::min(column * (float) clipColumns_, std::max(column, width() - 240.0f));
}

float DetailView::notesTabsX() const
{
    return std::max(sidebarWidth() + 8.0f, std::min(width() - 464.0f, sidebarWidth() + (width() - sidebarWidth()) * 0.5f - 117.0f));
}

Rect DetailView::noteTabBounds(int tab) const
{
    return {notesTabsX() + (float) tab * 78, 2, 74, 17};
}

Rect DetailView::tabRect(int tab) const
{
    return {width() - 222.0f + (float) tab * 72.0f, 2, 68, 17};
}

void DetailView::mouseDown(const MouseEvent& e)
{
    if (scrollbarVisible() && e.y >= height() - 11)
    {
        const Rect thumb = scrollbarThumb();
        if (thumb.reduced(0, -3).contains(e.x, e.y))
        {
            draggingScrollbar_ = true;
            scrollGrabX_ = scrollX_;
            scrollGrabMouseX_ = e.x;
        }
        else // a click on the track pages towards the pointer
        {
            scrollX_ += e.x < thumb.x ? -width() * 0.8f : width() * 0.8f;
            layout();
            repaint();
        }
        return;
    }
    if (ctx().detailTab == DetailTab::Clip && ctx().selectedClip() && ctx().selectedClip()->kind == model::Clip::Kind::Midi)
        for (int tab = 0; tab < 3; ++tab)
            if (noteTabBounds(tab).contains(e.x, e.y))
            {
                ctx().clipEnvelopesVisible = tab == 1; ctx().clipMpeVisible = tab == 2;
                if (tab == 2) for (const auto& child : children_) if (auto* roll = dynamic_cast<PianoRoll*>(child.get()); roll && ! roll->selectedNotes().empty())
                    for (const auto& editor : children_) if (auto* mpe = dynamic_cast<MpeEditor*>(editor.get())) mpe->selectNote(*roll->selectedNotes().begin());
                layout(); repaint(); return;
            }
    for (int tab = 0; ctx().detailTab != DetailTab::Devices && tab < 3; ++tab)
        if (tabRect(tab).contains(e.x, e.y))
        {
            ctx().detailTab = static_cast<DetailTab>(tab);
            layout();
            repaint();
            return;
        }
}

void DetailView::paint(SkCanvas& c, const Theme& t)
{
    paint::fill(c, localBounds(), t.color("Desktop"));
    if (ctx().detailTab == DetailTab::Devices)
    {
        Rect body{3, 3, width() - 6, height() - 6 - (scrollbarVisible() ? 7.0f : 0.0f)};
        if (instrumentSlotWidth() > 0)
        {
            const Rect slot{instrumentSlotX(), body.y, instrumentSlotWidth() - kDeviceGap, body.h};
            paint::fillRounded(c, slot, 3, t.color("SurfaceBackground"));
            paint::text(c, "Drop Instrument", {slot.x, slot.centreY() - 15, slot.w, 14}, t.color("ControlForeground"), t.metrics.fontSize + 1, Align::Centre);
            paint::text(c, "Here", {slot.x, slot.centreY(), slot.w, 14}, t.color("ControlForeground"), t.metrics.fontSize + 1, Align::Centre);
        }
        SkPaint rail;
        rail.setAntiAlias(true); rail.setColor(t.color("ControlContrastFrame"));
        if (const auto* track = ctx().trackAt(ctx().selectedTrack); track && track->kind == model::TrackKind::Midi)
            for (int i = 0; i < 21; ++i) c.drawCircle(4 - scrollX_, body.centreY() - 50 + (float) i * 5, 1.3f, rail);
        for (auto* box : boxes_)
        {
            const float cx = box->bounds().right() + 2.5f;
            const float top = body.y + body.h * 0.2f, bottom = body.y + body.h * 0.77f;
            paint::line(c, cx, top, cx, bottom, t.color("ControlContrastFrame"), 1.2f);
            paint::line(c, cx + 2, top, cx + 2, bottom, t.color("ControlContrastFrame"), 1.2f);
            paint::line(c, cx - 0.5f, top + 11, cx + 2.5f, top + 11, t.color("SurfaceBackground"), 0.6f);
        }
        const float end = kChainStart + chainWidth() - scrollX_;
        Rect drop{end, body.y, std::max(0.0f, width() - end - 3), body.h};
        paint::fillRounded(c, drop, 3, t.color("SurfaceBackground"));
        if (drop.w > 110) paint::text(c, "Drop Audio Effects Here", drop, t.color("ControlForeground"), t.metrics.fontSize, Align::Centre);
        if (scrollbarVisible())
        {
            paint::fill(c, {6, height() - 8, width() - 12, 5}, t.color("SurfaceArea"));
            paint::fillRounded(c, scrollbarThumb(), 2, t.color("TextDisabled"));
        }
        return;
    }
    paint::fill(c, localBounds(), t.color("SurfaceBackground"));
    if (scrollbarVisible())
    {
        paint::fill(c, {6, height() - 8.0f, width() - 12.0f, 5}, paint::withAlpha(t.color("SurfaceArea"), 120));
        paint::fillRounded(c, scrollbarThumb(), 2.5f, paint::mix(t.color("ControlForeground"), t.color("SurfaceBackground"), 0.55f));
    }
    const std::string title = ctx().trackDisplayName(ctx().selectedTrack);
    paint::text(c, title, {8, 3, width() - 230, 16}, t.color("ControlForeground"), t.metrics.fontSize, Align::Left, true);
    if (ctx().detailTab == DetailTab::Clip && ctx().selectedClip() && ctx().selectedClip()->kind == model::Clip::Kind::Midi)
        for (int tab = 0; tab < 3; ++tab)
        {
            const Rect r = noteTabBounds(tab);
            const bool on = tab == 2 ? ctx().clipMpeVisible : ! ctx().clipMpeVisible && ctx().clipEnvelopesVisible == (tab == 1);
            ctl::framed(c, t, r, on ? t.color("ControlBackground") : t.color("SurfaceBackground"));
            paint::text(c, tab == 0 ? "Notes" : tab == 1 ? "Envelopes" : "MPE", r, t.color("ControlForeground"), t.metrics.fontSize, Align::Centre);
        }
    for (int tab = 0; tab < 3; ++tab)
    {
        const auto r = tabRect(tab);
        const bool on = (int) ctx().detailTab == tab;
        ctl::framed(c, t, r, on ? t.color("ChosenDefault") : t.color("ControlTextBack"));
        paint::text(c, tab == 0 ? "Devices" : tab == 1 ? "Clip" : "Monitor", r, t.color("ControlForeground"), t.metrics.fontSize, Align::Centre);
    }
    if (ctx().detailTab == DetailTab::Clip && ! ctx().selectedClip())
        paint::text(c, "No clip selected.", localBounds().reduced(0, 20), t.color("ControlForeground"), t.metrics.fontSize + 1, Align::Centre);
}

void DetailView::paintOverChildren(SkCanvas& c, const Theme& t)
{
    if (deviceDragging_)
    {
        const auto accent = t.color(deviceDropAllowed_ ? "ChosenAlternative" : "ChosenRecord");
        paint::line(c, insertionX_, 4, insertionX_, height() - 6, accent, 2.5f);
        ctl::triangle(c, insertionX_, 6, 7, 6, ctl::Dir::Down, accent);
        const Rect label{std::clamp(dragX_ + 10, 4.0f, std::max(4.0f, width() - 160)),
                         std::clamp(dragY_ + 16, 4.0f, std::max(4.0f, height() - 25)), 150, 21};
        paint::fillRounded(c, label, 3, t.color("ControlBackground"));
        paint::stroke(c, label, accent);
        paint::text(c, dragLabel_, label.reduced(5, 0), t.color("ControlForeground"), t.metrics.fontSize);
    }
    SkPaint frame;
    frame.setAntiAlias(true);
    frame.setStyle(SkPaint::kStroke_Style);
    frame.setStrokeWidth(2);
    frame.setColor(t.color("ControlContrastFrame"));
    c.drawRoundRect(localBounds().reduced(1).sk(), 4, 4, frame);
}

bool DetailView::keyPressed(const KeyEvent& k)
{
    auto& x = ctx();
    if (visible && x.detailTab == DetailTab::Clip && k.cmd && ! k.alt && ! k.shift && k.keyCode == juce::KeyPress::returnKey)
        for (const auto& child : children_) if (auto* panel = dynamic_cast<ClipControls*>(child.get()); panel && panel->keyPressed(k)) return true;
    if (dragDeviceId_ >= 0 && k.keyCode == juce::KeyPress::escapeKey) { cancelDeviceDrag(); repaint(); return true; }
    if (visible && x.detailTab == DetailTab::Devices && k.cmd && ! k.alt && (k.textChar == 'g' || k.textChar == 'G'))
    { x.statusText = "Device grouping is not implemented"; return true; }
    if (! visible || x.detailTab != DetailTab::Devices || x.selectedDeviceId < 0)
        return false;
    if (k.cmd || k.alt || (k.keyCode != juce::KeyPress::deleteKey && k.keyCode != juce::KeyPress::backspaceKey))
        return false;
    if (! x.deleteSelectedDevice())
        return false;
    layout();
    repaint();
    return true;
}

bool DetailView::mouseWheel(const MouseEvent& e)
{
    scrollX_ = std::clamp(scrollX_ - (e.wheelDeltaX + e.wheelDeltaY) * 120.0f, 0.0f, std::max(0.0f, contentWidth() - width() + 12));
    layout();
    repaint();
    return true;
}

float DetailView::instrumentSlotWidth() const
{
    const auto* track = ctx().trackAt(ctx().selectedTrack);
    if (! track || track->kind != model::TrackKind::Midi) return 0;
    return model::instrumentIndex(*track) < 0 ? 142.0f : 0.0f;
}

float DetailView::instrumentSlotX() const
{
    float x = kChainStart - scrollX_;
    for (auto* box : boxes_)
    {
        if (! static_cast<DeviceBox*>(box)->isMidiEffect()) break;
        x += static_cast<DeviceBox*>(box)->preferredWidth() + kDeviceGap;
    }
    return x;
}

void DetailView::paintOverview(SkCanvas& c, const Theme& t, const Rect& rect)
{
    if (ctx().detailTab != DetailTab::Devices || boxes_.empty()) return;
    const float total = std::max(1.0f, contentWidth());
    c.save();
    c.clipRect(rect.sk());
    c.translate(rect.x, rect.y);
    c.scale(rect.w / total, rect.h / std::max(1.0f, height() - 6));
    c.translate(scrollX_ - kChainStart, -3);
    for (auto* box : boxes_) box->paintTree(c, t);
    c.restore();
    const float left = std::clamp(scrollX_ / total, 0.0f, 1.0f);
    const float fraction = std::min(1.0f - left, width() / total);
    paint::stroke(c, {rect.x + left * rect.w, rect.y, fraction * rect.w, rect.h}, t.color("ChosenAlternative"), 1.2f);
}

void DetailView::navigateOverview(float fraction)
{
    ctx().detailTab = DetailTab::Devices;
    ctx().detailVisible = true;
    layout();
    scrollX_ = std::clamp(fraction * contentWidth() - width() * 0.5f, 0.0f, std::max(0.0f, contentWidth() - width() + 12));
    layout();
    repaint();
}

void DetailView::cancelDeviceDrag()
{
    dragDeviceId_ = dragDestination_ = -1;
    deviceDragging_ = deviceDropAllowed_ = false;
    dragLabel_.clear();
}

void DetailView::beginDeviceDrag(int deviceId, float x, float y)
{
    cancelDeviceDrag();
    const auto* track = ctx().trackAt(ctx().selectedTrack);
    if (! track || ctx().recording || ctx().detailTab != DetailTab::Devices) return;
    for (const auto& device : track->devices)
        if (device.id == deviceId && model::deviceRole(device) == model::DeviceRole::AudioEffect)
        {
            dragDeviceId_ = deviceId; dragLabel_ = device.displayName;
            dragStartX_ = dragX_ = x; dragStartY_ = dragY_ = y;
            break;
        }
}

void DetailView::updateDeviceDrag(float x, float y)
{
    if (dragDeviceId_ < 0) return;
    dragX_ = x; dragY_ = y;
    deviceDragging_ |= std::hypot(x - dragStartX_, y - dragStartY_) >= 5;
    if (deviceDragging_) { updateDeviceDropTarget(); repaint(); }
}

void DetailView::updateDeviceDropTarget()
{
    deviceDropAllowed_ = false;
    const auto* track = ctx().trackAt(ctx().selectedTrack);
    if (! track || ! ctx().song) return;
    int source = -1, gap = (int) track->devices.size();
    for (int i = 0; i < (int) track->devices.size(); ++i) if (track->devices[(size_t) i].id == dragDeviceId_) source = i;
    insertionX_ = kChainStart + chainWidth() - scrollX_ - kDeviceGap * 0.5f;
    for (auto* widget : boxes_)
    {
        const auto* box = static_cast<DeviceBox*>(widget);
        if (box->deviceIndex() < 0) continue;
        if (dragX_ < box->bounds().centreX())
        {
            gap = box->deviceIndex(); insertionX_ = box->bounds().x - kDeviceGap * 0.5f; break;
        }
    }
    dragDestination_ = gap - (source < gap ? 1 : 0);
    if (source >= 0 && localBounds().contains(dragX_, dragY_))
        deviceDropAllowed_ = model::canMoveAudioEffect(*ctx().song, ctx().selectedTrack, dragDeviceId_, dragDestination_);
}

void DetailView::endDeviceDrag(float x, float y)
{
    if (dragDeviceId_ < 0) return;
    updateDeviceDrag(x, y);
    auto& context = ctx();
    model::EditCommand command;
    if (deviceDragging_ && deviceDropAllowed_ && context.song)
        command = model::moveAudioEffect(*context.song, context.selectedTrack, dragDeviceId_, dragDestination_);
    cancelDeviceDrag();
    if (command.apply && context.applyEdit)
    {
        context.applyEdit(std::move(command));
        // The app invalidates children before mutation; simpler test hosts may only update the model.
        builtSong_ = nullptr;
        layout();
    }
    repaint();
}

} // namespace sonora::ui
