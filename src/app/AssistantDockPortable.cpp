// COM-7: Agent Control dock for Windows/Linux, built from plain JUCE widgets. macOS uses the AppKit
// composer in AssistantDock.mm. Same behaviour for MCP access, the native LLM run/stop and undo; the
// provider setup sheet and result-details window are macOS-only for now: on other platforms the
// native LLM is configured through Agent.json and the secret file (see docs/PLATFORMS.md).
// TODO(COM-7): portable provider setup + snapshots once the dock moves into the Skia toolkit.

#include "app/AssistantDock.h"
#include "agent/AgentRunner.h"
#include "platform/Paths.h"

namespace sonora::app
{
namespace
{
juce::String providerName(const agent::Config& config)
{
    for (const auto& provider : agent::providers()) if (config.provider == provider.id) return provider.name;
    return config.provider;
}
struct PromptInput final : juce::TextEditor
{
    std::function<void()> onEnter, onFocus;
    bool keyPressed(const juce::KeyPress& key) override
    {
        if (key.getKeyCode() == juce::KeyPress::returnKey && ! key.getModifiers().isShiftDown()) { if (onEnter) onEnter(); return true; }
        return juce::TextEditor::keyPressed(key);
    }
    void focusGained(FocusChangeType cause) override { if (onFocus) onFocus(); juce::TextEditor::focusGained(cause); }
};
}
struct AssistantDock::Impl
{
    juce::Label title{{}, "Agent Control"}, connection{{}, "MCP access is off"}, nativeStatus{{}, "Native LLM: Off"}, composerTitle;
    juce::ToggleButton automation{"Allow MCP access to this project"};
    juce::TextEditor transcript;
    PromptInput input;
    juce::TextButton configureButton{"Configure..."}, run{"Run"}, stop{"Stop"}, undo{"Undo agent edit"};
    agent::Settings settings;
    agent::Config config;
    agent::Runner runner;
    control::ControlService* service = nullptr;
    juce::String document;
    juce::var lastResult;
    bool credentials = false, composerVisible = false;
};
AssistantDock::AssistantDock(bool loadSettings) : impl_(std::make_unique<Impl>())
{
    auto& n = *impl_;
    if (loadSettings) { n.config = n.settings.load(); n.credentials = n.settings.hasKey(n.config); }
    n.title.setFont(juce::FontOptions(18.0f, juce::Font::bold));
    n.transcript.setMultiLine(true); n.transcript.setReadOnly(true); n.transcript.setScrollbarsShown(true);
    n.input.setMultiLine(true, true); n.input.setReturnKeyStartsNewLine(true);
    n.input.onEnter = [this] { submit(); };
    n.input.onFocus = [this] { if (onFocus) onFocus(); };
    n.automation.onClick = [this] { setAutomation(impl_->automation.getToggleState()); };
    n.configureButton.onClick = [this] { configure(); };
    n.run.onClick = [this] { submit(); };
    n.stop.onClick = [this] { stop(); };
    n.undo.onClick = [this] { undo(); };
    for (juce::Component* c : std::initializer_list<juce::Component*>{&n.title, &n.automation, &n.connection, &n.nativeStatus, &n.configureButton,
                                                                      &n.transcript, &n.composerTitle, &n.input, &n.run, &n.stop, &n.undo})
        addAndMakeVisible(c);
    n.transcript.setText("MCP agents send their prompts in the MCP client. Enable access above to let them work on this project.\n\n"
                         "Action results and errors will appear here. Native LLM chat is optional and starts in Configure.\n", false);
    setName("Agent Control");
    refreshNative(); startTimer(1000);
}
AssistantDock::~AssistantDock() { stopTimer(); impl_->runner.cancel(); }
void AssistantDock::resized()
{
    auto& n = *impl_;
    auto area = getLocalBounds().reduced(14);
    n.title.setBounds(area.removeFromTop(26)); area.removeFromTop(6);
    n.automation.setBounds(area.removeFromTop(24));
    n.connection.setBounds(area.removeFromTop(20)); area.removeFromTop(6);
    auto nativeRow = area.removeFromTop(26);
    n.configureButton.setBounds(nativeRow.removeFromRight(110)); n.nativeStatus.setBounds(nativeRow);
    area.removeFromTop(8);
    n.undo.setBounds(area.removeFromBottom(26).removeFromLeft(140)); area.removeFromBottom(8);
    if (n.composerVisible)
    {
        auto runRow = area.removeFromBottom(26);
        n.run.setBounds(runRow.removeFromLeft(80)); runRow.removeFromLeft(8); n.stop.setBounds(runRow.removeFromLeft(80));
        area.removeFromBottom(6); n.input.setBounds(area.removeFromBottom(100));
        n.composerTitle.setBounds(area.removeFromBottom(20)); area.removeFromBottom(8);
    }
    n.transcript.setBounds(area);
}
void AssistantDock::setControlService(control::ControlService& service) { impl_->service = &service; timerCallback(); }
void AssistantDock::focusEditor()
{
    if (impl_->composerVisible) impl_->input.grabKeyboardFocus(); else impl_->transcript.grabKeyboardFocus();
}
void AssistantDock::append(const juce::String& text)
{
    const auto combined = impl_->transcript.getText() + "\n" + text + "\n";
    impl_->transcript.setText(combined.substring(std::max(0, combined.length() - 65536)), false);
    impl_->transcript.moveCaretToEnd();
}
void AssistantDock::showResult(const juce::String& source, const juce::String& operation, const juce::var& result)
{
    impl_->lastResult = result.clone();
    append(agent::describeResult(source, operation, result)); timerCallback();
}
void AssistantDock::submit()
{
    auto& n = *impl_;
    if (! n.composerVisible || n.runner.running()) return;
    const auto prompt = n.input.getText().trim(); if (prompt.isEmpty()) return;
    if (onSubmit) { onSubmit(prompt); return; }
    if (! n.service) { append("No editable project is connected."); return; }
    juce::String key;
    const auto credential = n.settings.readKey(n.config, key);
    if (! credential.ok) { append(credential.message); n.credentials = false; refreshNative(); return; }
    const juce::Component::SafePointer<AssistantDock> safe(this);
    agent::Runner::Events events;
    events.text = [safe](const juce::String& text) { if (safe) safe->append("Model reply\n" + text); };
    events.tool = [safe](const juce::String& operation, const juce::var& result) { if (safe) safe->showResult("Native LLM", operation, result); };
    events.finished = [safe](const juce::String& result) { if (safe) { safe->append(result); safe->refreshNative(); } };
    const auto started = n.runner.start(*n.service, n.config, std::move(key), prompt, std::move(events));
    if (started.ok) { append("You\n" + prompt); n.input.clear(); }
    else append(started.message);
    refreshNative();
}
void AssistantDock::undo()
{
    if (impl_->runner.running()) return;
    if (impl_->service) showResult("Agent Control", "history.undo", impl_->service->submit("undo"));
}
void AssistantDock::stop() { impl_->runner.cancel(); append("Stopping native LLM... Already applied edits will remain."); }
void AssistantDock::setAutomation(bool enabled)
{
    if (onAutomation) append(onAutomation(enabled));
    else refreshAutomation(false);
    timerCallback();
}
void AssistantDock::refreshAutomation(bool enabled)
{
    impl_->automation.setToggleState(enabled, juce::dontSendNotification);
    if (! enabled) impl_->connection.setText("MCP access is off", juce::dontSendNotification);
    timerCallback();
}
void AssistantDock::refreshNative()
{
    auto& n = *impl_;
    const bool ready = n.config.enabled && n.credentials && agent::validate(n.config).ok;
    n.composerVisible = ready;
    for (juce::Component* c : std::initializer_list<juce::Component*>{&n.composerTitle, &n.input, &n.run}) c->setVisible(ready);
    n.run.setEnabled(ready && ! n.runner.running()); n.stop.setVisible(ready && n.runner.running());
    n.nativeStatus.setText(ready ? "Native LLM: " + providerName(n.config) + " - " + n.config.model : "Native LLM: Off", juce::dontSendNotification);
    n.composerTitle.setText("Ask " + providerName(n.config) + " - Enter runs - Shift+Enter adds a line", juce::dontSendNotification);
    n.undo.setEnabled(! n.runner.running() && n.service && static_cast<bool>(n.service->context()["undoAvailable"]));
    resized();
}
void AssistantDock::timerCallback()
{
    auto& n = *impl_;
    if (n.service)
    {
        const auto context = n.service->context(); const auto document = context["documentId"].toString();
        if (n.document.isNotEmpty() && n.document != document)
        { n.runner.cancel(); append("Project changed. MCP access was revoked; start a new request for this project."); }
        n.document = document;
        n.undo.setEnabled(! n.runner.running() && static_cast<bool>(context["undoAvailable"]));
    }
    if (n.automation.getToggleState())
    {
        const auto activity = onActivity ? onActivity() : control::LocalEndpoint::Activity{};
        juce::String text = "MCP access enabled - waiting for an agent";
        if (activity.active > 0) text = "MCP access enabled - processing a request";
        else if (activity.lastRequestMs > 0)
            text = "MCP access enabled - last request " + juce::Time(activity.lastRequestMs).formatted("%H:%M:%S") + " - " + juce::String(activity.requests) + " requests";
        n.connection.setText(text, juce::dontSendNotification);
    }
}
void AssistantDock::configure()
{
    append("Provider setup is macOS-only in this build. Write Agent.json and Secrets.json in "
           + platform::appDataFolder().getFullPathName() + " as described in docs/PLATFORMS.md.");
}
void AssistantDock::chooseProvider() {}
void AssistantDock::saveConfiguration() {}
void AssistantDock::disableNative()
{
    auto c = impl_->config; c.enabled = false;
    const auto saved = impl_->settings.save(c, {});
    if (saved.ok) impl_->config = c; else append(saved.message);
    refreshNative();
}
void AssistantDock::removeKey()
{
    const auto removed = impl_->settings.removeKey(impl_->config);
    if (! removed.ok) append(removed.message);
    impl_->credentials = impl_->settings.hasKey(impl_->config);
    refreshNative();
}
void AssistantDock::closeConfiguration() {}
void AssistantDock::details()
{
    if (! impl_->lastResult.isVoid()) append(juce::JSON::toString(impl_->lastResult));
}
bool AssistantDock::writeSnapshot(const juce::File& file)
{
    const auto image = createComponentSnapshot(getLocalBounds());
    juce::FileOutputStream out(file);
    juce::PNGImageFormat png;
    return out.openedOk() && out.setPosition(0) && out.truncate().wasOk() && png.writeImageToStream(image, out);
}
bool AssistantDock::writeConfigurationSnapshot(const juce::File&) { return false; }
void AssistantDock::previewNativeConfiguration(bool enabled)
{
    impl_->config = agent::preset("openai"); impl_->config.enabled = enabled; impl_->credentials = enabled;
    refreshNative();
}
bool AssistantDock::verifyNativeInput() { return false; }
bool AssistantDock::verifyPanelModes() { return false; }
}
