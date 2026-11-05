#include "app/CollabChatDock.h"
#import <AppKit/AppKit.h>

namespace
{
constexpr NSUInteger maxChars = 1000; // collab::maxChatChars; the session cuts longer lines too
}

@interface SonoraChatActions : NSObject <NSTextViewDelegate>
@property(nonatomic, assign) sonora::app::CollabChatDock* owner;
@property(nonatomic, weak) NSTextField* counter;
@property(nonatomic, weak) NSButton* sendButton;
@property(nonatomic, assign) BOOL enabled;
- (void)send:(id)sender;
- (void)close:(id)sender;
- (void)update:(NSTextView*)input;
@end
@implementation SonoraChatActions
- (void)send:(id)sender { (void)sender; if (_owner) _owner->send(); }
- (void)close:(id)sender { (void)sender; if (_owner && _owner->onClose) _owner->onClose(); }
- (void)update:(NSTextView*)input
{
    const NSUInteger chars = [input.string lengthOfBytesUsingEncoding:NSUTF32LittleEndianStringEncoding] / 4;
    self.counter.stringValue = [NSString stringWithFormat:@"%lu / %lu", (unsigned long)chars, (unsigned long)maxChars];
    self.counter.textColor = chars > maxChars ? NSColor.systemRedColor : NSColor.secondaryLabelColor;
    self.sendButton.enabled = self.enabled && chars > 0 && chars <= maxChars;
}
- (void)textDidChange:(NSNotification*)notification { [self update:(NSTextView*)notification.object]; }
@end

@interface SonoraChatInput : NSTextView
@property(nonatomic, weak) SonoraChatActions* actions;
@end
@implementation SonoraChatInput
- (BOOL)becomeFirstResponder
{
    if (self.actions.owner && self.actions.owner->onFocus) self.actions.owner->onFocus(); // computer-MIDI keys let go
    return [super becomeFirstResponder];
}
- (void)keyDown:(NSEvent*)event
{
    // Return sends; an open IME composition (marked text) takes Return itself, Shift+Return adds a line.
    if ((event.keyCode == 36 || event.keyCode == 76) && ! self.hasMarkedText && (event.modifierFlags & NSEventModifierFlagShift) == 0)
    { [self.actions send:self]; return; }
    [super keyDown:event];
}
@end

namespace sonora::app
{
struct CollabChatDock::Impl
{
    NSView* root;
    NSTextField* subtitle;
    NSTextView* transcript;
    SonoraChatInput* input;
    SonoraChatActions* actions;
    NSTextField* status;
    NSTextField* counter;
    NSButton* sendButton;
    size_t signature = 0;
    std::string reason;
    juce::String sentForCheck; // verifyNativeInput
    bool capturing = false;
};

namespace
{
NSString* native(const std::string& s) { auto* r = [NSString stringWithUTF8String:s.c_str()]; return r ? r : @""; }
NSTextField* label(NSString* text, CGFloat size)
{
    auto* l = [NSTextField wrappingLabelWithString:text];
    l.font = [NSFont systemFontOfSize:size];
    return l;
}
NSStackView* stack(NSArray<NSView*>* views, NSUserInterfaceLayoutOrientation orientation, CGFloat spacing = 8)
{
    auto* s = [NSStackView stackViewWithViews:views];
    s.orientation = orientation;
    s.spacing = spacing;
    s.alignment = orientation == NSUserInterfaceLayoutOrientationVertical ? NSLayoutAttributeLeading : NSLayoutAttributeCenterY;
    s.detachesHiddenViews = YES;
    return s;
}
NSScrollView* scroll(NSTextView* text)
{
    auto* view = [[NSScrollView alloc] initWithFrame:NSZeroRect];
    view.hasVerticalScroller = YES;
    view.borderType = NSNoBorder;
    view.drawsBackground = YES;
    view.backgroundColor = [NSColor colorWithCalibratedWhite:.105 alpha:1];
    text.minSize = NSMakeSize(0, 0);
    text.maxSize = NSMakeSize(CGFLOAT_MAX, CGFLOAT_MAX);
    text.verticallyResizable = YES;
    text.horizontallyResizable = NO;
    text.autoresizingMask = NSViewWidthSizable;
    text.textContainer.widthTracksTextView = YES;
    text.font = [NSFont systemFontOfSize:13];
    text.textContainerInset = NSMakeSize(8, 8);
    text.textColor = [NSColor colorWithCalibratedWhite:.9 alpha:1];
    text.backgroundColor = [NSColor colorWithCalibratedWhite:.105 alpha:1];
    text.insertionPointColor = NSColor.whiteColor;
    text.richText = NO;
    text.automaticQuoteSubstitutionEnabled = NO;
    text.automaticDashSubstitutionEnabled = NO;
    text.automaticLinkDetectionEnabled = NO;
    view.documentView = text;
    return view;
}
NSColor* colour(uint32_t argb)
{
    return [NSColor colorWithSRGBRed:((argb >> 16) & 0xff) / 255.0 green:((argb >> 8) & 0xff) / 255.0 blue:(argb & 0xff) / 255.0 alpha:1];
}
// Names in their colour are lightened on the dark transcript so the darker palette entries stay readable.
NSColor* nameColour(uint32_t argb)
{
    NSColor* light = [colour(argb) blendedColorWithFraction:.35 ofColor:NSColor.whiteColor];
    return light != nil ? light : colour(argb);
}
} // namespace

CollabChatDock::CollabChatDock() : impl_(std::make_unique<Impl>())
{
    auto& n = *impl_;
    n.root = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, preferredWidth, 700)];
    n.root.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
    n.root.wantsLayer = YES;
    n.root.layer.backgroundColor = [NSColor colorWithCalibratedWhite:.15 alpha:1].CGColor;
    n.actions = [[SonoraChatActions alloc] init];
    n.actions.owner = this;
    n.actions.enabled = YES;

    auto* title = label(@"Session Chat", 16);
    title.font = [NSFont boldSystemFontOfSize:16];
    auto* close = [NSButton buttonWithTitle:@"Close" target:n.actions action:@selector(close:)];
    close.bezelStyle = NSBezelStyleRecessed;
    close.accessibilityLabel = @"Close the session chat";
    auto* titleRow = stack(@[title, close], NSUserInterfaceLayoutOrientationHorizontal);
    [title setContentHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationHorizontal];
    n.subtitle = label(@"", 11);
    n.subtitle.textColor = NSColor.secondaryLabelColor;

    n.transcript = [[NSTextView alloc] initWithFrame:NSMakeRect(0, 0, preferredWidth - 28, 400)];
    n.transcript.editable = NO;
    n.transcript.selectable = YES;
    n.transcript.accessibilityLabel = @"Session chat messages";
    auto* transcript = scroll(n.transcript);
    [transcript setContentHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationVertical];
    [transcript.heightAnchor constraintGreaterThanOrEqualToConstant:120].active = YES;

    n.status = label(@"", 11);
    n.status.textColor = NSColor.systemOrangeColor;
    n.status.hidden = YES;

    n.input = [[SonoraChatInput alloc] initWithFrame:NSMakeRect(0, 0, preferredWidth - 28, 64)];
    n.input.actions = n.actions;
    n.input.delegate = n.actions;
    n.input.allowsUndo = YES;
    n.input.accessibilityLabel = @"Message to everyone in the session";
    auto* input = scroll(n.input);
    input.borderType = NSBezelBorder;
    [input.heightAnchor constraintEqualToConstant:64].active = YES;

    n.counter = label(@"0 / 1000", 10);
    n.counter.textColor = NSColor.secondaryLabelColor;
    n.sendButton = [NSButton buttonWithTitle:@"Send" target:n.actions action:@selector(send:)];
    n.sendButton.keyEquivalent = @"";
    n.sendButton.enabled = NO;
    auto* hint = label(@"Return sends · Shift+Return: new line", 10);
    hint.textColor = NSColor.tertiaryLabelColor;
    auto* sendRow = stack(@[hint, n.counter, n.sendButton], NSUserInterfaceLayoutOrientationHorizontal);
    [hint setContentHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationHorizontal];
    n.actions.counter = n.counter;
    n.actions.sendButton = n.sendButton;

    auto* content = stack(@[titleRow, n.subtitle, transcript, n.status, input, sendRow], NSUserInterfaceLayoutOrientationVertical, 8);
    content.translatesAutoresizingMaskIntoConstraints = NO;
    [n.root addSubview:content];
    [NSLayoutConstraint activateConstraints:@[
        [content.leadingAnchor constraintEqualToAnchor:n.root.leadingAnchor constant:12],
        [content.trailingAnchor constraintEqualToAnchor:n.root.trailingAnchor constant:-12],
        [content.topAnchor constraintEqualToAnchor:n.root.topAnchor constant:12],
        [content.bottomAnchor constraintEqualToAnchor:n.root.bottomAnchor constant:-12]]];
    for (NSView* child in @[titleRow, n.subtitle, transcript, n.status, input, sendRow])
        [child.widthAnchor constraintEqualToAnchor:content.widthAnchor].active = YES;
    holder_.setView((__bridge void*) n.root);
    addAndMakeVisible(holder_);
    setName("Session Chat");
    setLines({});
}

CollabChatDock::~CollabChatDock()
{
    impl_->actions.owner = nullptr;
    impl_->input.delegate = nil;
    holder_.setView(nullptr);
}

void CollabChatDock::resized() { holder_.setBounds(getLocalBounds()); }

void CollabChatDock::setLines(const std::vector<Line>& lines)
{
    auto& n = *impl_;
    // The session refreshes the view often (presence moves 20 times a second): rebuild only when the lines changed,
    // so a selection in the transcript survives.
    size_t signature = lines.size() * 0x9e3779b97f4a7c15ull;
    for (const auto& l : lines)
        signature = (signature ^ std::hash<std::string>{}(l.name + '\x1f' + l.text + '\x1f' + l.time + '\x1f' + l.failed)
                                 ^ (l.pending ? 0x51ull : 0) ^ ((size_t) l.color << 7)) * 1099511628211ull;
    if (signature == n.signature && n.transcript.textStorage.length > 0) return;
    n.signature = signature;

    auto* out = [[NSMutableAttributedString alloc] init];
    auto* paragraph = [[NSMutableParagraphStyle alloc] init];
    paragraph.paragraphSpacing = 8;
    auto add = [&](NSString* s, NSDictionary* attributes) { [out appendAttributedString:[[NSAttributedString alloc] initWithString:s attributes:attributes]]; };
    NSColor* body = [NSColor colorWithCalibratedWhite:.9 alpha:1];
    NSColor* older = [NSColor colorWithCalibratedWhite:.62 alpha:1];
    NSColor* dim = [NSColor colorWithCalibratedWhite:.5 alpha:1];
    if (lines.empty())
        add(@"Messages go to everyone in the session. They are not saved with the set.", @{NSFontAttributeName: [NSFont systemFontOfSize:12],
            NSForegroundColorAttributeName: dim, NSParagraphStyleAttributeName: paragraph});
    bool historyNoteDone = false;
    for (size_t i = 0; i < lines.size(); ++i)
    {
        const auto& l = lines[i];
        if (! l.history && ! historyNoteDone && i > 0 && lines[i - 1].history)
        {
            add(@"— you joined —\n", @{NSFontAttributeName: [NSFont systemFontOfSize:10], NSForegroundColorAttributeName: dim, NSParagraphStyleAttributeName: paragraph});
            historyNoteDone = true;
        }
        add(native(l.self ? l.name + " (you)" : l.name), @{NSFontAttributeName: [NSFont boldSystemFontOfSize:12],
            NSForegroundColorAttributeName: nameColour(l.color)});
        std::string meta = "  " + l.time;
        if (l.pending && l.failed.empty()) meta += "  sending...";
        add(native(meta), @{NSFontAttributeName: [NSFont systemFontOfSize:10], NSForegroundColorAttributeName: dim});
        if (! l.failed.empty())
            add(native("  not sent: " + l.failed), @{NSFontAttributeName: [NSFont systemFontOfSize:10], NSForegroundColorAttributeName: NSColor.systemRedColor});
        add(@"\n", @{});
        add(native(l.text + (i + 1 < lines.size() ? "\n" : "")), @{NSFontAttributeName: [NSFont systemFontOfSize:13],
            NSForegroundColorAttributeName: l.history ? older : l.pending ? dim : body, NSParagraphStyleAttributeName: paragraph});
    }
    [n.transcript.textStorage setAttributedString:out];
    [n.transcript scrollRangeToVisible:NSMakeRange(n.transcript.string.length, 0)];
}

void CollabChatDock::setComposer(bool enabled, const std::string& reason)
{
    auto& n = *impl_;
    n.actions.enabled = enabled;
    n.input.editable = enabled;
    n.input.textColor = enabled ? [NSColor colorWithCalibratedWhite:.9 alpha:1] : NSColor.secondaryLabelColor;
    if (reason != n.reason || n.status.hidden != reason.empty())
    {
        n.reason = reason;
        n.status.stringValue = native(reason);
        n.status.hidden = reason.empty();
    }
    [n.actions update:n.input];
}

void CollabChatDock::setSubtitle(const std::string& text) { impl_->subtitle.stringValue = native(text); }

void CollabChatDock::focusComposer() { [impl_->root.window makeFirstResponder:impl_->input]; }

void CollabChatDock::send()
{
    auto& n = *impl_;
    if (! n.actions.enabled) return;
    const auto text = juce::String::fromUTF8(n.input.string.UTF8String).trim();
    if (text.isEmpty() || (size_t) text.length() > maxChars) return;
    if (n.capturing) { n.sentForCheck = text; n.input.string = @""; return; }
    const auto why = onSend ? onSend(text.toStdString()) : std::string("Not in a session.");
    n.status.stringValue = native(why);
    n.status.hidden = why.empty() && n.reason.empty();
    if (why.empty())
    {
        n.input.string = @"";
        if (! n.reason.empty()) n.status.stringValue = native(n.reason);
    }
    [n.actions update:n.input];
}

bool CollabChatDock::writeSnapshot(const juce::File& file)
{
    auto* view = impl_->root;
    [view layoutSubtreeIfNeeded];
    auto* bitmap = [view bitmapImageRepForCachingDisplayInRect:view.bounds];
    if (! bitmap) return false;
    [view.effectiveAppearance performAsCurrentDrawingAppearance:^{ [view cacheDisplayInRect:view.bounds toBitmapImageRep:bitmap]; }];
    auto* png = [bitmap representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
    return [png writeToFile:native(file.getFullPathName().toStdString()) atomically:YES];
}

bool CollabChatDock::verifyNativeInput()
{
    auto& n = *impl_;
    auto* input = n.input;
    NSString* original = [input.string copy];
    const BOOL wasEnabled = n.actions.enabled;
    n.actions.enabled = YES;
    n.input.editable = YES;
    n.capturing = true;
    n.sentForCheck.clear();
    auto key = [&](NSEventModifierFlags flags) {
        return [NSEvent keyEventWithType:NSEventTypeKeyDown location:NSZeroPoint modifierFlags:flags timestamp:0
                            windowNumber:n.root.window.windowNumber context:nil characters:@"\r" charactersIgnoringModifiers:@"\r"
                               isARepeat:NO keyCode:36];
    };
    focusComposer();
    input.string = @"";
    [input setMarkedText:@"に" selectedRange:NSMakeRange(1, 0) replacementRange:NSMakeRange(NSNotFound, 0)];
    const bool marked = input.hasMarkedText;
    if (auto* event = key(0)) [input keyDown:event];
    const bool composition = n.sentForCheck.isEmpty();
    [input unmarkText];
    input.string = @"日本語 🎹 groove";
    [input setSelectedRange:NSMakeRange(input.string.length, 0)];
    if (auto* event = key(NSEventModifierFlagShift)) [input keyDown:event];
    const bool newline = [input.string containsString:@"\n"] && n.sentForCheck.isEmpty();
    if (auto* event = key(0)) [input keyDown:event];
    const bool unicode = n.sentForCheck.contains(juce::String::fromUTF8("日本語 🎹 groove")) && input.string.length == 0;
    input.string = original;
    n.capturing = false;
    n.actions.enabled = wasEnabled;
    n.input.editable = wasEnabled;
    [n.actions update:input];
    std::fprintf(stderr, "Chat native input checks: marked=%d composition=%d newline=%d unicode=%d\n", marked, composition, newline, unicode);
    return marked && composition && newline && unicode;
}
} // namespace sonora::app
