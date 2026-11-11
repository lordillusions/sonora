#pragma once
// Authored from the documented file format; no presets or application assets (UI-7).
#include "core/Model.h"

#include <memory>
#include <utility>

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
// ALS-15d: rack branches in the reference's classes: <DrumBranch|InstrumentBranch|AudioEffectBranch|MidiEffectBranch> holding
// <DeviceChain><MidiToAudio|AudioToAudio|MidiToMidiDeviceChain Id="0"><Devices>. rackBranchDevices fixes one branch (the
// layout older Sonora versions wrote is rearranged) and returns its Devices list; repairRackBranches fixes every rack inside x.
std::pair<const char*, const char*> rackBranchClasses(const std::string& rackType);
juce::XmlElement* rackBranchDevices(juce::XmlElement& branch, const std::string& rackType);
bool rackBranchesNeedRepair(const juce::XmlElement& x);
void repairRackBranches(juce::XmlElement& x);
// ALS-15d: Swiss Army Meter's input and output routables (the reference crashes on the device without them); fills only
// empty InRoutings / OutRoutings lists.
void addSwissArmyMeterRoutings(juce::XmlElement& routables);
std::unique_ptr<juce::XmlElement> samplePartXml(const model::SamplePart& part, int id);
} // namespace sonora::io
