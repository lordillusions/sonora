#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace sonora::app
{
// Keep the native traffic lights and centered document title, with the canvas background beneath them.
void blendWindowTitleBar(juce::Component&, juce::Colour);
}
