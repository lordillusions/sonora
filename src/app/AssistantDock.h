#pragma once
// AGT-5/6a: native Agent Control + optional configured LLM composer (ADR 0008).
#include "agent/AgentConfig.h"
#include "control/LocalEndpoint.h"
#include <juce_gui_extra/juce_gui_extra.h>

namespace sonora::app
{
class AssistantDock final : public juce::Component, private juce::Timer
{
public:
    explicit AssistantDock(bool loadSettings = true);
    ~AssistantDock() override;
    void resized() override;
    void focusEditor();
    void setControlService(control::ControlService&);
    void submit();
    void undo();
    void stop();
    void setAutomation(bool enabled);
    void refreshAutomation(bool enabled);
    void append(const juce::String& text);
    void showResult(const juce::String& source, const juce::String& operation, const juce::var&);
    void configure();
    void chooseProvider();
    void saveConfiguration();
    void disableNative();
    void removeKey();
    void closeConfiguration();
    void details();
    bool writeSnapshot(const juce::File& file);
    bool writeConfigurationSnapshot(const juce::File& file);
    bool verifyNativeInput();
    bool verifyPanelModes();
    void previewNativeConfiguration(bool enabled); // developer snapshots; never writes settings or credentials
    std::function<juce::var(const juce::String&)> onSubmit; // input-probe injection only; normal runs use Runner
    std::function<juce::String(bool)> onAutomation;
    std::function<control::LocalEndpoint::Activity()> onActivity;
    std::function<void()> onFocus;
private:
    void timerCallback() override;
    void refreshNative();
    struct Impl;
    std::unique_ptr<Impl> impl_;
#if JUCE_MAC
    juce::NSViewComponent holder_; // AppKit composer (AssistantDock.mm); AssistantDockPortable.cpp elsewhere (COM-7)
#endif
};
}
