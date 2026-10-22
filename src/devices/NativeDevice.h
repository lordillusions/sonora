#pragma once
// Base class for Sonora's own implementations of Live devices.
// Holds parameters in an array of atomics so the UI / MIDI mapping can write them while
// the audio thread reads them.

#include "core/Model.h"
#include "devices/DeviceProcessor.h"

#include <memory>
#include <cmath>

namespace sonora::devices
{

class NativeDevice : public DeviceProcessor
{
public:
    explicit NativeDevice(std::vector<ParameterInfo> infos)
        : infos_(std::move(infos)), values_(std::make_unique<std::atomic<float>[]>(infos_.size()))
    {
        for (size_t i = 0; i < infos_.size(); ++i)
            values_[i].store(infos_[i].defaultValue);
    }

    const std::vector<ParameterInfo>& parameters() const override { return infos_; }

    float getParameter(int index) const override
    {
        return index >= 0 && (size_t) index < infos_.size() ? values_[(size_t) index].load(std::memory_order_relaxed)
                                                            : 0.0f;
    }

    void setParameter(int index, float value) override
    {
        if (index < 0 || (size_t) index >= infos_.size() || ! std::isfinite(value))
            return;
        const auto& info = infos_[(size_t) index];
        values_[(size_t) index].store(juce::jlimit(info.minValue, info.maxValue, value), std::memory_order_relaxed);
        parametersChanged.store(true, std::memory_order_release);
    }

    // Copies values from the .als model (matching ParameterInfo::id == Parameter::path).
    void loadFromModel(const model::Device& d)
    {
        const auto& descriptors = parameters();
        for (size_t i = 0; i < descriptors.size(); ++i)
        {
            const auto& info = descriptors[i];
            const auto plain = d.plain.find(info.id);
            setParameter((int) i, (float) (info.plainValue && plain != d.plain.end() ? plain->second : d.param(info.id, info.defaultValue)));
        }
        setBypassed(! d.enabled);
    }

protected:
    float value(int index) const { return values_[(size_t) index].load(std::memory_order_relaxed); }

    // Set by setParameter(); audio thread clears it when it recomputes derived state.
    std::atomic<bool> parametersChanged{true};

private:
    std::vector<ParameterInfo> infos_;
    std::unique_ptr<std::atomic<float>[]> values_;
};

} // namespace sonora::devices
