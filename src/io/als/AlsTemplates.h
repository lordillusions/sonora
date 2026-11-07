#pragma once
// Authored from the documented file format; no presets or application assets (UI-7).
#include "core/Model.h"

#include <memory>

namespace juce
{
class XmlElement;
}

namespace sonora::io
{
std::string audioClipTemplate(const model::Clip& clip);
std::string deviceTemplate(const model::Device& device);
// One <MultiSamplePart> (Simpler / Sampler zone): the part's source XML with the model's values, or for a part Sonora
// made (a loaded sample) every node the reference writes itself (ALS-15b).
std::unique_ptr<juce::XmlElement> samplePartXml(const model::SamplePart& part, int id);
} // namespace sonora::io
