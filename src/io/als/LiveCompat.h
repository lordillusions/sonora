#pragma once
// ALS-15: sets Sonora saves must open in the reference app (12.x). Its loader refuses the whole set as "corrupt" on an
// element class it does not know ("Unknown class 'SonoraCurveLfo'") and on a fractional text in an int node
// ("Unexpected value for int node: 0.00000000000000000"). Sonora's own devices (Sonora Prism = <SonoraWavetable>, Curve
// LFO, Hush, FM, ...) are therefore written inside a <PluginDevice> the reference can load:
//
//   <PluginDevice Id="n"> ...the device's own <On>, UserName...
//     <PluginDesc><Vst3PluginInfo> Sonora's VST3 of that device (JUCE class id {ABCDEF01, 9182FAEB, 'Snra', code}),
//       its <ProcessorState> = what that VST3 saves (Prism, Curve LFO, Hush play there with the same sound) </...>
//     <SonoraNativeDevice Version="1"> <SonoraCurveLfo Id="n">...the device exactly as Sonora wrote it...</...> </...>
//   </PluginDevice>
//
// The reference ignores the unknown <SonoraNativeDevice> child (verified with the reference 12.4.6); Sonora's reader puts the
// inner element back in place before anything else reads the DOM, so a Sonora project round-trips unchanged. A set the
// reference saved again has lost the inner element: Prism/Curve LFO/Hush then come back as their VST3s (played by the
// built-in stand-ins without the VST3, COL-4c); devices without a VST3 (FM, Echo, ...) come back as missing plug-ins.
// See docs/ALS_FORMAT.md "Sonora devices in the reference app".

#include "core/Model.h"

#include <juce_core/juce_core.h>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>
#include <vector>

namespace sonora::io
{
// The state Sonora's VST3 of this device would save (an empty vector: none, the VST3 starts from its defaults). The
// devices layer installs it (it needs the device code); io never depends on it.
using SonoraVst3StateEncoder = std::function<std::vector<uint8_t>(const model::Device&)>;
void setSonoraVst3StateEncoder(SonoraVst3StateEncoder encoder);

// The JUCE VST3 class id of Sonora's VST3 for this device tag ("SonoraCurveLfo" -> {..., 'Snra', 'SnCl'}); devices
// without a published VST3 get a code of their own so the reference names them as a missing plug-in.
std::array<uint32_t, 4> sonoraVst3Uid(std::string_view deviceTag);

// True for a device element only Sonora knows (its tag starts with "Sonora").
bool isSonoraOnlyDevice(const juce::XmlElement& device);

// A copy of `root` the reference app can open, or nullptr when `root` already can (nothing to change).
std::unique_ptr<juce::XmlElement> liveCompatibleCopy(const juce::XmlElement& root);

// An element whose Id is a pointee id of the set (AutomationTarget, ModulationTarget, VolumeModulationTarget... in a
// FreezeSequencer, Pointee, rack ControllerTargets.N). The reference refuses a set where two of them share an id.
bool isPointeeElement(const juce::XmlElement& e);

// Gives every repeated pointee id after its first use a fresh id from NextPointeeId on (and moves NextPointeeId past
// them). Sets saved by Sonora before 0.1.14 could repeat a duplicated track's FreezeSequencer modulation targets; the
// reference then asks to "repair" the set. Deterministic, so the loader and the writer's DOM agree. Returns the count.
int repairDuplicatePointeeIds(juce::XmlElement& root);

// Puts every wrapped Sonora device back in place of its wrapper. Returns how many were restored.
int unwrapSonoraDevices(juce::XmlElement& root);
} // namespace sonora::io
