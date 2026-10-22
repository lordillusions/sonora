#pragma once
// Drag-to-modulate support for Sonora Prism (UI-28): which editor controls are matrix destinations, and the parameter
// writes that create, rescale or remove a route. Pure functions over the device's parameter values (message thread);
// the audio thread only sees ordinary atomic parameter writes, ordered so a half-written route is never audible.
#include "devices/native/WavetableSynth.h"

#include <vector>

namespace sonora::devices::wavetable_routing
{
struct Write { int parameter = -1; float value = 0; };
struct Route
{
    int slot = -1, source = WavetableSynth::SourceOff, destination = WavetableSynth::DestOff;
    float amount = 0;
    bool bipolar = false; // the route swings both ways around the control (bipolar LFO or the slot's Bipolar flag)
};

// Matrix destination a control drives (DestOff: not modulatable). Several controls can share one destination
// (Octave/Semitone/Fine/Coarse all drive the oscillator pitch).
int destinationForParameter(int parameter);
bool isModulationSource(int source);
// Every active route (source, destination and a non-zero amount) into the destination, in slot order.
std::vector<Route> routesTo(const DeviceProcessor&, int destination);
std::vector<Route> routesForParameter(const DeviceProcessor& device, int parameter);
int findSlot(const DeviceProcessor&, int source, int destination); // -1 when absent
int freeSlot(const DeviceProcessor&);                              // first slot with Source and Destination Off; -1 when full
// Fraction of the control's travel (0..1, its own knob taper) that one unit of route amount moves the parameter.
float travelPerAmount(const DeviceProcessor&, int parameter);
// Amount for a route freshly dropped on the control: about a third of its travel.
float defaultAmount(const DeviceProcessor&, int parameter);
// Writes for source -> parameter's destination with `amount`. Reuses the slot already joining them, else the first free one.
// Empty (and slot = -1) when the control is not modulatable, the source is invalid or all slots are taken.
std::vector<Write> connect(const DeviceProcessor&, int source, int parameter, float amount, int& slot);
std::vector<Write> setAmount(int slot, float amount);
std::vector<Write> disconnect(int slot); // amount first, then the endpoints, Via, Bipolar and Out back to defaults
void apply(DeviceProcessor&, const std::vector<Write>&);

// Route direction (UI-28d, Serum's ring modes): Forward moves the control up by the amount, Backward down, Both swings
// either side of it. Both is the slot's Bipolar flag; an LFO's own Bipolar switch is converted to that convention first
// (the LFO turns unipolar and its other routes take the flag, which sounds the same in the native domain).
enum class Direction { Forward, Backward, Both };
Direction direction(const DeviceProcessor&, int slot);
std::vector<Write> setDirection(const DeviceProcessor&, int slot, Direction);
std::vector<Write> unipolarLfo(const DeviceProcessor&, int lfo);
// The span a route sweeps in knob travel (0..1) around the control's current knob position `at`.
struct Span { float from = 0, to = 0; };
Span span(const DeviceProcessor&, int parameter, const Route&, float at);
} // namespace sonora::devices::wavetable_routing
