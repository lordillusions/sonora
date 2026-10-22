#pragma once
// Sonora's MIDI velocity curve; preserves event timing, channels and note releases (DEV-17).
#include "devices/NativeDevice.h"
#include <cmath>

namespace sonora::devices
{
class MidiVelocity final : public NativeDevice
{
public:
    MidiVelocity() : NativeDevice({makeParam("Min", "Minimum", 1, 127, 1), makeParam("Max", "Maximum", 1, 127, 127),
                                    makeParam("Curve", "Curve", 0.1f, 4, 1), makeParam("Fixed", "Fixed", 0, 1, 0, "", true)}) {}
    std::string name() const override { return "Sonora Velocity"; }
    void prepare(double, int) override {}
    void process(juce::AudioBuffer<float>&, juce::MidiBuffer& midi) override
    {
        if (isBypassed()) return;
        const float low = std::min(value(0), value(1)), high = std::max(value(0), value(1));
        for (const auto meta : midi)
        {
            if (meta.numBytes == 3 && (meta.data[0] & 0xf0) == 0x90 && meta.data[2] > 0)
            {
                const float velocity = value(3) >= 0.5f ? high : low + (high - low) * std::pow((float) meta.data[2] / 127, value(2));
                // MidiBuffer owns writable storage; changing one data byte preserves event sizes/order.
                // No scratch swap: even a caller with a tiny MIDI buffer remains allocation-free.
                const_cast<juce::uint8*>(meta.data)[2] = (juce::uint8) std::clamp(std::round(velocity), 1.0f, 127.0f);
            }
        }
    }
};
} // namespace sonora::devices
