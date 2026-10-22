#include "app/AssistantDock.h"
#include "agent/AgentRunner.h"
#import <AppKit/AppKit.h>

@interface SonoraAssistantActions : NSObject
@property(nonatomic, assign) sonora::app::AssistantDock* owner;
- (void)send:(id)sender;
- (void)undo:(id)sender;
- (void)stop:(id)sender;
- (void)automation:(id)sender;
- (void)configure:(id)sender;
- (void)provider:(id)sender;
- (void)save:(id)sender;
- (void)disable:(id)sender;
- (void)removeKey:(id)sender;
- (void)close:(id)sender;
- (void)details:(id)sender;
@end
@implementation SonoraAssistantActions
- (void)send:(id)sender { (void)sender; if (_owner) _owner->submit(); }
- (void)undo:(id)sender { (void)sender; if (_owner) _owner->undo(); }
- (void)stop:(id)sender { (void)sender; if (_owner) _owner->stop(); }
- (void)automation:(id)sender { if (_owner) _owner->setAutomation([(NSButton*)sender state] == NSControlStateValueOn); }
- (void)configure:(id)sender { (void)sender; if (_owner) _owner->configure(); }
- (void)provider:(id)sender { (void)sender; if (_owner) _owner->chooseProvider(); }
- (void)save:(id)sender { (void)sender; if (_owner) _owner->saveConfiguration(); }
- (void)disable:(id)sender { (void)sender; if (_owner) _owner->disableNative(); }
- (void)removeKey:(id)sender { (void)sender; if (_owner) _owner->removeKey(); }
- (void)close:(id)sender { (void)sender; if (_owner) _owner->closeConfiguration(); }
- (void)details:(id)sender { (void)sender; if (_owner) _owner->details(); }
@end
@interface SonoraPromptInput : NSTextView
@property(nonatomic, weak) SonoraAssistantActions* actions;
@end
@implementation SonoraPromptInput
- (BOOL)becomeFirstResponder
{
    if (self.actions.owner && self.actions.owner->onFocus) self.actions.owner->onFocus();
    return [super becomeFirstResponder];
}
- (void)keyDown:(NSEvent*)event
{
    if ((event.keyCode == 36 || event.keyCode == 76) && ! self.hasMarkedText && (event.modifierFlags & NSEventModifierFlagShift) == 0)
    { [self.actions send:self]; return; }
    [super keyDown:event];
}
@end
namespace sonora::app
{
struct AssistantDock::Impl
{
    NSView* root;
    NSTextView* transcript;
    SonoraPromptInput* input;
    SonoraAssistantActions* actions;
    NSButton* automation;
    NSTextField* connection;
    NSTextField* nativeStatus;
    NSTextField* composerTitle;
    NSStackView* composer;
    NSButton* run;
    NSButton* stop;
    NSButton* undo;
    NSButton* details;
    NSWindow* settingsWindow = nil;
    NSWindow* detailsWindow = nil;
    NSPopUpButton* provider;
    NSPopUpButton* protocol;
    NSTextField* model;
    NSTextField* endpoint;
    NSSecureTextField* key;
    NSTextField* settingsError;
    agent::Settings settings;
    agent::Config config;
    agent::Runner runner;
    control::ControlService* service = nullptr;
    juce::String document;
    juce::var lastResult;
    bool credentials = false, preview = false;
};
namespace
{
NSString* native(const juce::String& value) { auto* result = [NSString stringWithUTF8String:value.toRawUTF8()]; return result ? result : @""; }
juce::String string(NSString* value) { return juce::String::fromUTF8(value.UTF8String); }
NSTextField* label(NSString* text, CGFloat size = 12)
{
    auto* value = [NSTextField wrappingLabelWithString:text]; value.font = [NSFont systemFontOfSize:size]; return value;
}
NSStackView* stack(NSArray<NSView*>* views, NSUserInterfaceLayoutOrientation orientation = NSUserInterfaceLayoutOrientationVertical)
{
    auto* value = [NSStackView stackViewWithViews:views]; value.orientation = orientation; value.spacing = 9;
    value.alignment = orientation == NSUserInterfaceLayoutOrientationVertical ? NSLayoutAttributeLeading : NSLayoutAttributeCenterY;
    value.detachesHiddenViews = YES; return value;
}
void pin(NSView* parent, NSView* child, CGFloat margin = 14)
{
    child.translatesAutoresizingMaskIntoConstraints = NO; [parent addSubview:child];
    [NSLayoutConstraint activateConstraints:@[
        [child.leadingAnchor constraintEqualToAnchor:parent.leadingAnchor constant:margin],
        [child.trailingAnchor constraintEqualToAnchor:parent.trailingAnchor constant:-margin],
        [child.topAnchor constraintEqualToAnchor:parent.topAnchor constant:margin],
        [child.bottomAnchor constraintEqualToAnchor:parent.bottomAnchor constant:-margin]]];
}
NSScrollView* scroll(NSTextView* text)
{
    auto* view = [[NSScrollView alloc] initWithFrame:NSZeroRect]; view.hasVerticalScroller = YES; view.borderType = NSBezelBorder;
    text.minSize = NSMakeSize(0, 0); text.maxSize = NSMakeSize(CGFLOAT_MAX, CGFLOAT_MAX);
    text.verticallyResizable = YES; text.horizontallyResizable = NO;
    text.autoresizingMask = NSViewWidthSizable; text.textContainer.widthTracksTextView = YES;
    text.font = [NSFont systemFontOfSize:12]; text.textContainerInset = NSMakeSize(9, 9);
    text.textColor = [NSColor colorWithCalibratedWhite:.88 alpha:1]; text.backgroundColor = [NSColor colorWithCalibratedWhite:.105 alpha:1];
    text.insertionPointColor = NSColor.whiteColor; text.richText = NO;
    text.automaticQuoteSubstitutionEnabled = NO; text.automaticDashSubstitutionEnabled = NO;
    view.documentView = text; return view;
}
void fillWidth(NSView* view, NSView* parent) { [view.widthAnchor constraintEqualToAnchor:parent.widthAnchor].active = YES; }
template<class Native> agent::Config draft(const Native& n)
{
    auto c = agent::preset(agent::providers()[static_cast<size_t>(n.provider.indexOfSelectedItem)].id);
    c.model = string(n.model.stringValue).trim(); c.baseUrl = string(n.endpoint.stringValue).trim().trimCharactersAtEnd("/");
    c.protocol = static_cast<agent::Protocol>(n.protocol.indexOfSelectedItem); c.enabled = true; return c;
}
juce::String providerName(const agent::Config& config)
{
    for (const auto& provider : agent::providers()) if (config.provider == provider.id) return provider.name;
    return config.provider;
}
}
AssistantDock::AssistantDock(bool loadSettings) : impl_(std::make_unique<Impl>())
{
    auto& n = *impl_;
    if (loadSettings) { n.config = n.settings.load(); n.credentials = n.settings.hasKey(n.config); }
    n.root = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 420, 780)];
    n.root.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua]; n.root.wantsLayer = YES;
    n.root.layer.backgroundColor = [NSColor colorWithCalibratedWhite:.15 alpha:1].CGColor;
    n.actions = [[SonoraAssistantActions alloc] init]; n.actions.owner = this;
    auto* title = label(@"Agent Control", 18); title.font = [NSFont boldSystemFontOfSize:18];
    auto* subtitle = label(@"Connect an external agent, or configure a model to chat inside Sonora.", 11); subtitle.textColor = NSColor.secondaryLabelColor;
    n.automation = [NSButton checkboxWithTitle:@"Allow MCP access to this project" target:n.actions action:@selector(automation:)];
    n.automation.accessibilityLabel = @"Allow external MCP agents to inspect and edit this project";
    n.connection = label(@"MCP access is off", 11); n.connection.textColor = NSColor.secondaryLabelColor;
    auto* mcpHint = label(@"Send MCP requests from Codex or your other MCP client. Access ends when you switch projects or quit.", 11);
    mcpHint.textColor = NSColor.secondaryLabelColor;
    auto* configureButton = [NSButton buttonWithTitle:@"Configure…" target:n.actions action:@selector(configure:)];
    n.nativeStatus = label(@"Native LLM: Off", 11);
    auto* nativeRow = stack(@[n.nativeStatus, configureButton], NSUserInterfaceLayoutOrientationHorizontal);
    [n.nativeStatus setContentHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationHorizontal];
    n.transcript = [[NSTextView alloc] initWithFrame:NSMakeRect(0, 0, 390, 280)]; n.transcript.editable = NO;
    n.transcript.accessibilityLabel = @"Agent action results";
    auto* results = scroll(n.transcript); [results.heightAnchor constraintGreaterThanOrEqualToConstant:100].active = YES;
    [results setContentHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationVertical];
    n.input = [[SonoraPromptInput alloc] initWithFrame:NSMakeRect(0, 0, 390, 100)]; n.input.actions = n.actions;
    n.input.allowsUndo = YES; n.input.accessibilityLabel = @"Request for the configured native LLM";
    auto* input = scroll(n.input); [input.heightAnchor constraintEqualToConstant:100].active = YES;
    n.composerTitle = label(@"Ask the configured model · Enter runs · Shift+Enter adds a line", 10);
    n.run = [NSButton buttonWithTitle:@"Run" target:n.actions action:@selector(send:)];
    n.stop = [NSButton buttonWithTitle:@"Stop" target:n.actions action:@selector(stop:)];
    auto* runRow = stack(@[n.run, n.stop], NSUserInterfaceLayoutOrientationHorizontal);
    n.composer = stack(@[n.composerTitle, input, runRow]);
    n.undo = [NSButton buttonWithTitle:@"Undo agent edit" target:n.actions action:@selector(undo:)];
    n.details = [NSButton buttonWithTitle:@"Result details…" target:n.actions action:@selector(details:)]; n.details.enabled = NO;
    auto* actionRow = stack(@[n.undo, n.details], NSUserInterfaceLayoutOrientationHorizontal);
    auto* content = stack(@[title, subtitle, n.automation, n.connection, mcpHint, nativeRow, results, n.composer, actionRow]);
    pin(n.root, content);
    for (NSView* child in @[subtitle, n.connection, mcpHint, nativeRow, results, n.composer, actionRow]) fillWidth(child, content);
    fillWidth(input, n.composer); fillWidth(n.composerTitle, n.composer);
    n.transcript.string = @"MCP agents send their prompts in the MCP client. Enable access above to let them work on this project.\n\nAction results and errors will appear here. Native LLM chat is optional and starts in Configure.\n";
    holder_.setView((__bridge void*)n.root); addAndMakeVisible(holder_); setName("Agent Control");
    refreshNative(); startTimer(1000);
}
AssistantDock::~AssistantDock()
{
    stopTimer(); impl_->actions.owner = nullptr; impl_->runner.cancel();
    closeConfiguration(); [impl_->detailsWindow close]; holder_.setView(nullptr);
}
void AssistantDock::resized() { holder_.setBounds(getLocalBounds()); }
void AssistantDock::setControlService(control::ControlService& service) { impl_->service = &service; timerCallback(); }
void AssistantDock::focusEditor()
{
    if (impl_->composer.hidden) [impl_->root.window makeFirstResponder:impl_->transcript];
    else [impl_->root.window makeFirstResponder:impl_->input];
}
void AssistantDock::append(const juce::String& text)
{
    const auto combined = string(impl_->transcript.string) + "\n" + text + "\n";
    impl_->transcript.string = native(combined.substring(std::max(0, combined.length() - 65536)));
    [impl_->transcript scrollRangeToVisible:NSMakeRange(impl_->transcript.string.length, 0)];
}
void AssistantDock::showResult(const juce::String& source, const juce::String& operation, const juce::var& result)
{
    impl_->lastResult = result.clone(); impl_->details.enabled = YES;
    append(agent::describeResult(source, operation, result)); timerCallback();
}
void AssistantDock::submit()
{
    auto& n = *impl_;
    if (n.composer.hidden || n.runner.running()) return;
    const auto prompt = string(n.input.string).trim(); if (prompt.isEmpty()) return;
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
    if (started.ok) { append("You\n" + prompt); n.input.string = @""; }
    else append(started.message);
    refreshNative();
}
void AssistantDock::undo()
{
    if (impl_->runner.running()) return;
    if (impl_->service) showResult("Agent Control", "history.undo", impl_->service->submit("undo"));
}
void AssistantDock::stop() { impl_->runner.cancel(); append(juce::String::fromUTF8("Stopping native LLM… Already applied edits will remain.")); }
void AssistantDock::setAutomation(bool enabled)
{
    if (onAutomation) append(onAutomation(enabled));
    else refreshAutomation(false);
    timerCallback();
}
void AssistantDock::refreshAutomation(bool enabled)
{
    impl_->automation.state = enabled ? NSControlStateValueOn : NSControlStateValueOff;
    if (! enabled) impl_->connection.stringValue = @"MCP access is off";
    timerCallback();
}
void AssistantDock::refreshNative()
{
    auto& n = *impl_;
    const bool ready = n.config.enabled && n.credentials && agent::validate(n.config).ok;
    n.composer.hidden = ! ready; n.run.enabled = ready && ! n.runner.running(); n.stop.hidden = ! n.runner.running();
    n.nativeStatus.stringValue = native(ready ? "Native LLM: " + providerName(n.config) + " · " + n.config.model : "Native LLM: Off");
    n.composerTitle.stringValue = native("Ask " + providerName(n.config) + " · Enter runs · Shift+Enter adds a line");
    n.undo.enabled = ! n.runner.running() && n.service && static_cast<bool>(n.service->context()["undoAvailable"]);
    [n.root layoutSubtreeIfNeeded];
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
        n.undo.enabled = ! n.runner.running() && static_cast<bool>(context["undoAvailable"]);
    }
    if (n.automation.state == NSControlStateValueOn)
    {
        const auto activity = onActivity ? onActivity() : control::LocalEndpoint::Activity{};
        if (activity.active > 0) n.connection.stringValue = @"MCP access enabled · processing a request";
        else if (activity.lastRequestMs > 0)
            n.connection.stringValue = native(juce::String::fromUTF8("MCP access enabled · last request ") + juce::Time(activity.lastRequestMs).formatted("%H:%M:%S")
                + " · " + juce::String(activity.requests) + " requests");
        else n.connection.stringValue = @"MCP access enabled · waiting for an agent";
    }
}
void AssistantDock::configure()
{
    auto& n = *impl_;
    if (onFocus) onFocus();
    if (n.settingsWindow) { [n.settingsWindow makeKeyAndOrderFront:nil]; return; }
    n.settingsWindow = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 450, 480)
        styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
    n.settingsWindow.title = @"Native LLM"; n.settingsWindow.appearance = n.root.appearance;
    n.settingsWindow.contentView.wantsLayer = YES;
    n.settingsWindow.contentView.layer.backgroundColor = [NSColor colorWithCalibratedWhite:.15 alpha:1].CGColor;
    n.provider = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    for (const auto& p : agent::providers()) [n.provider addItemWithTitle:native(p.name)];
    n.provider.target = n.actions; n.provider.action = @selector(provider:);
    for (size_t i = 0; i < agent::providers().size(); ++i) if (n.config.provider == agent::providers()[i].id) [n.provider selectItemAtIndex:static_cast<NSInteger>(i)];
    n.model = [NSTextField textFieldWithString:native(n.config.model)];
    n.endpoint = [NSTextField textFieldWithString:native(n.config.baseUrl)];
    n.protocol = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    [n.protocol addItemsWithTitles:@[@"OpenAI Responses", @"OpenAI-compatible Chat Completions", @"Anthropic Messages"]];
    [n.protocol selectItemAtIndex:static_cast<NSInteger>(n.config.protocol)];
    n.key = [[NSSecureTextField alloc] initWithFrame:NSZeroRect];
    n.key.placeholderString = n.settings.hasKey(n.config) ? @"Saved in Keychain · leave blank to keep" : @"Paste provider API key";
    n.key.accessibilityLabel = @"API key, stored in macOS Keychain";
    n.settingsError = label(@"", 11); n.settingsError.textColor = NSColor.systemOrangeColor;
    auto* notice = label(@"Prompts and queried project data are sent to the selected provider. Keys stay in macOS Keychain and are never added to prompts. Audio files are not uploaded.", 11);
    auto* privacy = label(@"Save & Enable reveals the prompt in Agent Control. You can keep using MCP with the native LLM off.", 11);
    auto* save = [NSButton buttonWithTitle:@"Save & Enable" target:n.actions action:@selector(save:)];
    auto* disable = [NSButton buttonWithTitle:@"Disable native LLM" target:n.actions action:@selector(disable:)];
    auto* remove = [NSButton buttonWithTitle:@"Remove saved key" target:n.actions action:@selector(removeKey:)];
    auto* cancel = [NSButton buttonWithTitle:@"Cancel" target:n.actions action:@selector(close:)];
    auto* buttons = stack(@[cancel, save], NSUserInterfaceLayoutOrientationHorizontal);
    auto* content = stack(@[label(@"Provider"), n.provider, label(@"Model ID"), n.model, label(@"Base URL"), n.endpoint,
        label(@"API protocol"), n.protocol, label(@"API key"), n.key, notice, privacy, n.settingsError,
        stack(@[disable, remove], NSUserInterfaceLayoutOrientationHorizontal), buttons]);
    content.spacing = 5; pin(n.settingsWindow.contentView, content, 16);
    for (NSView* child in @[n.provider, n.model, n.endpoint, n.protocol, n.key, notice, privacy, n.settingsError, buttons]) fillWidth(child, content);
    [n.root.window beginSheet:n.settingsWindow completionHandler:nil];
}
void AssistantDock::chooseProvider()
{
    auto& n = *impl_; const auto c = agent::preset(agent::providers()[static_cast<size_t>(n.provider.indexOfSelectedItem)].id);
    n.model.stringValue = native(c.model); n.endpoint.stringValue = native(c.baseUrl); n.key.stringValue = @"";
    [n.protocol selectItemAtIndex:static_cast<NSInteger>(c.protocol)];
    n.key.placeholderString = n.settings.hasKey(c) ? @"Saved in Keychain · leave blank to keep" : @"Paste provider API key";
    n.settingsError.stringValue = @"";
}
void AssistantDock::saveConfiguration()
{
    auto& n = *impl_; const auto c = draft(n); const auto saved = n.settings.save(c, string(n.key.stringValue));
    n.key.stringValue = @"";
    if (! saved.ok) { n.settingsError.stringValue = native(saved.message); return; }
    n.runner.cancel(); n.config = c; n.credentials = true; n.preview = false; closeConfiguration(); refreshNative();
    append("Native LLM enabled: " + providerName(c) + " · " + c.model + ". Requests go to " + c.baseUrl + ".");
}
void AssistantDock::disableNative()
{
    auto& n = *impl_; auto c = n.config; c.enabled = false; const auto saved = n.settings.save(c, {});
    if (! saved.ok) { n.settingsError.stringValue = native(saved.message); return; }
    n.runner.cancel(); n.config = c; closeConfiguration(); refreshNative(); append("Native LLM disabled. MCP access is independent.");
}
void AssistantDock::removeKey()
{
    auto& n = *impl_; const auto c = draft(n); const auto removed = n.settings.removeKey(c);
    if (! removed.ok) { n.settingsError.stringValue = native(removed.message); return; }
    n.key.stringValue = @""; n.key.placeholderString = @"Paste provider API key"; n.settingsError.stringValue = @"Saved key removed for this provider and endpoint.";
    if (c.provider == n.config.provider && c.baseUrl == n.config.baseUrl && c.protocol == n.config.protocol)
    { n.credentials = false; n.runner.cancel(); refreshNative(); }
}
void AssistantDock::closeConfiguration()
{
    auto& n = *impl_; if (! n.settingsWindow) return;
    n.key.stringValue = @""; [n.settingsWindow.sheetParent endSheet:n.settingsWindow]; [n.settingsWindow orderOut:nil]; n.settingsWindow = nil;
}
void AssistantDock::details()
{
    auto& n = *impl_; if (n.lastResult.isVoid()) return;
    if (! n.detailsWindow)
    {
        n.detailsWindow = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 620, 480)
            styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable backing:NSBackingStoreBuffered defer:NO];
        n.detailsWindow.releasedWhenClosed = NO; n.detailsWindow.title = @"Agent result details"; n.detailsWindow.appearance = n.root.appearance;
    }
    for (NSView* child in [n.detailsWindow.contentView.subviews copy]) [child removeFromSuperview];
    auto* text = [[NSTextView alloc] initWithFrame:NSMakeRect(0, 0, 580, 440)]; text.editable = NO;
    auto* view = scroll(text); pin(n.detailsWindow.contentView, view); text.string = native(juce::JSON::toString(n.lastResult, false));
    [n.detailsWindow center]; [n.detailsWindow makeKeyAndOrderFront:nil];
}
bool AssistantDock::writeSnapshot(const juce::File& file)
{
    auto* view = impl_->root; [view layoutSubtreeIfNeeded];
    auto* bitmap = [view bitmapImageRepForCachingDisplayInRect:view.bounds]; if (! bitmap) return false;
    [view.effectiveAppearance performAsCurrentDrawingAppearance:^{ [view cacheDisplayInRect:view.bounds toBitmapImageRep:bitmap]; }];
    auto* png = [bitmap representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
    return [png writeToFile:native(file.getFullPathName()) atomically:YES];
}
bool AssistantDock::writeConfigurationSnapshot(const juce::File& file)
{
    // Developer-only presentation capture. Do not fill, save or read an API key.
    if (! impl_->settingsWindow) return false;
    auto* view = impl_->settingsWindow.contentView; [view layoutSubtreeIfNeeded];
    auto* bitmap = [view bitmapImageRepForCachingDisplayInRect:view.bounds]; if (! bitmap) return false;
    [view.effectiveAppearance performAsCurrentDrawingAppearance:^{ [view cacheDisplayInRect:view.bounds toBitmapImageRep:bitmap]; }];
    auto* png = [bitmap representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
    const bool saved = [png writeToFile:native(file.getFullPathName()) atomically:YES];
    return saved;
}
void AssistantDock::previewNativeConfiguration(bool enabled)
{
    impl_->config = agent::preset("openai"); impl_->config.enabled = enabled; impl_->credentials = enabled; impl_->preview = true;
    refreshNative();
}
bool AssistantDock::verifyPanelModes()
{
    const auto config = impl_->config; const bool credentials = impl_->credentials;
    previewNativeConfiguration(false); const bool off = impl_->composer.hidden;
    previewNativeConfiguration(true); const bool on = ! impl_->composer.hidden;
    impl_->credentials = false; refreshNative(); const bool missingKey = impl_->composer.hidden;
    impl_->config = config; impl_->credentials = credentials; refreshNative();
    const bool access = impl_->automation.state == NSControlStateValueOn;
    auto activity = std::move(onActivity);
    onActivity = [] { return control::LocalEndpoint::Activity{}; };
    refreshAutomation(true); const bool waiting = string(impl_->connection.stringValue).contains("waiting");
    onActivity = [] { return control::LocalEndpoint::Activity{1, 1, juce::Time::currentTimeMillis()}; };
    timerCallback(); const bool processing = string(impl_->connection.stringValue).contains("processing");
    onActivity = [] { return control::LocalEndpoint::Activity{0, 3, juce::Time::currentTimeMillis()}; };
    timerCallback(); const bool observed = string(impl_->connection.stringValue).contains("3 requests");
    refreshAutomation(false); const bool revoked = string(impl_->connection.stringValue).contains("off");
    onActivity = std::move(activity); refreshAutomation(access);
    return off && on && missingKey && waiting && processing && observed && revoked;
}
bool AssistantDock::verifyNativeInput()
{
    auto& n = *impl_; const auto config = n.config; const bool credentials = n.credentials;
    previewNativeConfiguration(true);
    auto* input = n.input; NSString* original = [input.string copy]; NSString* transcript = [n.transcript.string copy];
    auto submitCallback = std::move(onSubmit); int submissions = 0; juce::String received;
    onSubmit = [&](const juce::String& value) { ++submissions; received = value; return juce::var(); };
    const auto key = [&](NSEventModifierFlags flags) {
        return [NSEvent keyEventWithType:NSEventTypeKeyDown location:NSZeroPoint modifierFlags:flags timestamp:0
            windowNumber:n.root.window.windowNumber context:nil characters:@"\r" charactersIgnoringModifiers:@"\r" isARepeat:NO keyCode:36];
    };
    focusEditor(); [input setMarkedText:@"に" selectedRange:NSMakeRange(1, 0) replacementRange:NSMakeRange(NSNotFound, 0)];
    const bool marked = input.hasMarkedText; if (auto* event = key(0)) [input keyDown:event]; const bool composition = submissions == 0;
    [input unmarkText]; input.string = @"日本語 🎹 supersaw"; [input setSelectedRange:NSMakeRange(input.string.length, 0)];
    if (auto* event = key(NSEventModifierFlagShift)) [input keyDown:event]; const bool newline = [input.string containsString:@"\n"] && submissions == 0;
    if (auto* event = key(0)) [input keyDown:event]; const bool unicode = submissions == 1 && received.contains(juce::String::fromUTF8("日本語 🎹"));
    onSubmit = std::move(submitCallback); input.string = original; n.transcript.string = transcript; n.config = config; n.credentials = credentials; refreshNative();
    std::fprintf(stderr, "Native input checks: marked=%d composition=%d newline=%d unicode=%d\n", marked, composition, newline, unicode);
    return marked && composition && newline && unicode;
}
}
