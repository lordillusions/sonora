// COM-7: Windows/Linux keep the native title bar; only macOS blends it into the canvas (WindowChrome.mm).
#include "app/WindowChrome.h"

namespace sonora::app
{
void blendWindowTitleBar(juce::Component&, juce::Colour) {}
}
