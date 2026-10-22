#include "app/WindowChrome.h"

#import <AppKit/AppKit.h>

namespace sonora::app
{
void blendWindowTitleBar(juce::Component& component, juce::Colour colour)
{
    if (auto* peer = component.getPeer())
    {
        auto* view = (__bridge NSView*) peer->getNativeHandle();
        auto* window = view.window;
        window.titlebarAppearsTransparent = YES;
        window.backgroundColor = [NSColor colorWithSRGBRed:colour.getFloatRed()
                                                    green:colour.getFloatGreen()
                                                     blue:colour.getFloatBlue()
                                                    alpha:1.0];
        if (@available(macOS 11.0, *)) window.titlebarSeparatorStyle = NSTitlebarSeparatorStyleNone;
    }
}
}
