#include "devices/native/WavetableModRouting.h"

#include <algorithm>
#include <cmath>

namespace sonora::devices::wavetable_routing
{
namespace
{
using W = WavetableSynth;
int index(float value, int count) { return std::clamp(static_cast<int>(std::lround(value)), 0, count - 1); }
} // namespace

int destinationForParameter(int parameter)
{
    switch (parameter)
    {
        case W::Volume: return W::Amplitude;
        case W::Transpose: return W::Pitch;
        case W::Pan: return W::StereoPan;
        case W::SubLevel: return W::SubAmplitude;
        case W::NoiseLevel: return W::NoiseAmplitude;
        default: break;
    }
    for (int o = 0; o < W::kOscillators; ++o)
    {
        if (parameter == W::coarseParam(o)) return W::Osc1Pitch + o;
        if (parameter == W::mediaParam(o, W::MediaStartPosition)) return W::Osc1MediaPosition + o;
        if (parameter == W::mediaParam(o, W::MediaScanRate)) return W::Osc1MediaScan + o;
        if (parameter == W::mediaParam(o, W::MediaGrainSize)) return W::Osc1GrainSize + o;
        if (parameter < W::oscParam(o, W::On) || parameter >= W::oscParam(o, W::OscParams)) continue;
        const int field = parameter - W::oscParam(o, W::On);
        if (field == W::Position) return W::Osc1Position + o;
        if (field == W::Level) return W::Osc1Level + o;
        if (field == W::Octave || field == W::Semi || field == W::Fine) return W::Osc1Pitch + o;
        if (field == W::OscPan) return W::Osc1Pan + o;
        if (field == W::Warp1Amount) return W::Osc1Warp1 + o;
        if (field == W::Warp2Amount) return W::Osc1Warp2 + o;
        return W::DestOff;
    }
    for (int f = 0; f < W::kFilters; ++f)
    {
        if (parameter == W::filterParam(f, W::Cutoff)) return W::Filter1Cutoff + f;
        if (parameter == W::filterParam(f, W::Resonance)) return W::Filter1Resonance + f;
    }
    for (int e = 0; e < W::kEnvelopes; ++e)
    {
        const W::EnvParam stages[]{W::Attack, W::Decay, W::Sustain, W::Release};
        for (int k = 0; k < 4; ++k)
            if (parameter == W::envParam(e, stages[k])) return W::Env1Attack + e * 4 + k;
    }
    for (int source = 0; source < W::kRoutingSources; ++source)
        for (int bus = 0; bus < 2; ++bus)
            if (parameter == W::sourceRoutingParam(source, bus == 0 ? W::SourceBus1 : W::SourceBus2)) return W::sourceSendDestination(source, bus);
    return W::DestOff;
}

bool isModulationSource(int source) { return source > W::SourceOff && source < W::NumSources; }

std::vector<Route> routesTo(const DeviceProcessor& device, int destination)
{
    std::vector<Route> routes;
    if (destination <= W::DestOff || destination >= W::NumDestinations) return routes;
    for (int slot = 0; slot < W::kModSlots; ++slot)
    {
        Route route;
        route.slot = slot;
        route.source = index(device.getParameter(W::modParam(slot, W::ModSource)), W::NumSources);
        route.destination = index(device.getParameter(W::modParam(slot, W::ModDestination)), W::NumDestinations);
        route.amount = device.getParameter(W::modParam(slot, W::ModAmount));
        if (route.destination != destination || route.source == W::SourceOff || std::abs(route.amount) <= 1e-7f) continue;
        route.bipolar = device.getParameter(W::modParam(slot, W::ModBipolar)) >= 0.5f
            || (route.source >= W::Lfo1 && route.source <= W::Lfo10 && device.getParameter(W::lfoParam(route.source - W::Lfo1, W::Bipolar)) >= 0.5f);
        routes.push_back(route);
    }
    return routes;
}

std::vector<Route> routesForParameter(const DeviceProcessor& device, int parameter)
{
    return routesTo(device, destinationForParameter(parameter));
}

int findSlot(const DeviceProcessor& device, int source, int destination)
{
    for (int slot = 0; slot < W::kModSlots; ++slot)
        if (index(device.getParameter(W::modParam(slot, W::ModSource)), W::NumSources) == source
            && index(device.getParameter(W::modParam(slot, W::ModDestination)), W::NumDestinations) == destination)
            return slot;
    return -1;
}

int freeSlot(const DeviceProcessor& device)
{
    for (int slot = 0; slot < W::kModSlots; ++slot)
        if (index(device.getParameter(W::modParam(slot, W::ModSource)), W::NumSources) == W::SourceOff
            && index(device.getParameter(W::modParam(slot, W::ModDestination)), W::NumDestinations) == W::DestOff)
            return slot;
    return -1;
}

float travelPerAmount(const DeviceProcessor& device, int parameter)
{
    const int destination = destinationForParameter(parameter);
    if (destination == W::DestOff || parameter < 0 || static_cast<size_t>(parameter) >= device.parameters().size()) return 0;
    const auto& info = device.parameters()[static_cast<size_t>(parameter)];
    const float range = info.maxValue - info.minValue;
    if (range <= 0) return 0;
    // Native-domain laws in WavetableSynth::updateControl, expressed along the editor's knob taper.
    if (destination == W::Amplitude) return 4 * 6.0206f / range;                     // 2^(4 x) gain on a dB knob
    if (destination == W::Pitch) return 24 / range;                                  // 24 semitones on Transpose
    if (destination >= W::Osc1Pitch && destination <= W::Osc3Pitch)
    {
        const int o = destination - W::Osc1Pitch;
        const float semitonesPerUnit = parameter == W::oscParam(o, W::Octave) ? 12.0f : parameter == W::oscParam(o, W::Fine) ? 0.01f : 1.0f;
        return 24 / (range * semitonesPerUnit);
    }
    if (destination >= W::Filter1Cutoff && destination <= W::Filter2Cutoff)         // 8 octaves on a logarithmic knob
        return info.minValue > 0 ? 8 / std::log2(info.maxValue / info.minValue) : 8 / range;
    if (destination >= W::Osc1GrainSize && destination <= W::Osc3GrainSize)
        return info.minValue > 0 ? 4 / std::log2(info.maxValue / info.minValue) : 0;
    if (destination >= W::Osc1MediaScan && destination <= W::Osc3MediaScan) return 8 / range;
    if (destination >= W::Env1Attack && destination <= W::Env4Release) return 1;      // the fifth-power time taper / linear sustain
    return 1 / range;                                                                  // additive in the control's own unit
}

float defaultAmount(const DeviceProcessor& device, int parameter)
{
    const float travel = travelPerAmount(device, parameter);
    return travel > 0 ? std::clamp(0.35f / travel, -1.0f, 1.0f) : 0;
}

std::vector<Write> connect(const DeviceProcessor& device, int source, int parameter, float amount, int& slot)
{
    slot = -1;
    const int destination = destinationForParameter(parameter);
    if (destination == W::DestOff || ! isModulationSource(source)) return {};
    amount = std::clamp(amount, -1.0f, 1.0f);
    slot = findSlot(device, source, destination);
    if (slot >= 0) return setAmount(slot, amount);
    slot = freeSlot(device);
    if (slot < 0) return {};
    // Serum convention: a dropped source starts unipolar (Forward); a bipolar LFO is converted first, keeping its other routes.
    auto writes = source >= W::Lfo1 && source <= W::Lfo10 ? unipolarLfo(device, source - W::Lfo1) : std::vector<Write>{};
    // Endpoints before the amount: the engine ignores a slot until its amount is non-zero.
    const std::vector<Write> route{{W::modParam(slot, W::ModAmount), 0}, {W::modParam(slot, W::AuxSource), static_cast<float>(W::SourceOff)},
            {W::modParam(slot, W::ModBipolar), 0}, {W::outParam(slot), 1},
            {W::modParam(slot, W::ModSource), static_cast<float>(source)}, {W::modParam(slot, W::ModDestination), static_cast<float>(destination)},
            {W::modParam(slot, W::ModAmount), amount}};
    writes.insert(writes.end(), route.begin(), route.end());
    return writes;
}

std::vector<Write> setAmount(int slot, float amount)
{
    if (slot < 0 || slot >= W::kModSlots) return {};
    return {{W::modParam(slot, W::ModAmount), std::clamp(amount, -1.0f, 1.0f)}};
}

std::vector<Write> disconnect(int slot)
{
    if (slot < 0 || slot >= W::kModSlots) return {};
    return {{W::modParam(slot, W::ModAmount), 0}, {W::modParam(slot, W::ModSource), static_cast<float>(W::SourceOff)},
            {W::modParam(slot, W::ModDestination), static_cast<float>(W::DestOff)}, {W::modParam(slot, W::AuxSource), static_cast<float>(W::SourceOff)},
            {W::modParam(slot, W::ModBipolar), 0}, {W::outParam(slot), 1}};
}

void apply(DeviceProcessor& device, const std::vector<Write>& writes)
{
    for (const auto& write : writes) device.setParameter(write.parameter, write.value);
}

Direction direction(const DeviceProcessor& device, int slot)
{
    if (slot < 0 || slot >= W::kModSlots) return Direction::Forward;
    const int source = index(device.getParameter(W::modParam(slot, W::ModSource)), W::NumSources);
    const bool lfoBipolar = source >= W::Lfo1 && source <= W::Lfo10 && device.getParameter(W::lfoParam(source - W::Lfo1, W::Bipolar)) >= 0.5f;
    if (lfoBipolar || device.getParameter(W::modParam(slot, W::ModBipolar)) >= 0.5f) return Direction::Both;
    return device.getParameter(W::modParam(slot, W::ModAmount)) < 0 ? Direction::Backward : Direction::Forward;
}

std::vector<Write> unipolarLfo(const DeviceProcessor& device, int lfo)
{
    if (lfo < 0 || lfo >= W::kLfos || device.getParameter(W::lfoParam(lfo, W::Bipolar)) < 0.5f) return {};
    std::vector<Write> writes;
    for (int slot = 0; slot < W::kModSlots; ++slot)
        if (index(device.getParameter(W::modParam(slot, W::ModSource)), W::NumSources) == W::Lfo1 + lfo)
            writes.push_back({W::modParam(slot, W::ModBipolar), 1});
    writes.push_back({W::lfoParam(lfo, W::Bipolar), 0});
    return writes;
}

std::vector<Write> setDirection(const DeviceProcessor& device, int slot, Direction mode)
{
    if (slot < 0 || slot >= W::kModSlots) return {};
    const int source = index(device.getParameter(W::modParam(slot, W::ModSource)), W::NumSources);
    auto writes = source >= W::Lfo1 && source <= W::Lfo10 ? unipolarLfo(device, source - W::Lfo1) : std::vector<Write>{};
    const float amount = std::abs(device.getParameter(W::modParam(slot, W::ModAmount)));
    writes.push_back({W::modParam(slot, W::ModBipolar), mode == Direction::Both ? 1.0f : 0.0f});
    writes.push_back({W::modParam(slot, W::ModAmount), mode == Direction::Backward ? -amount : amount});
    return writes;
}

Span span(const DeviceProcessor& device, int parameter, const Route& route, float at)
{
    const float travel = route.amount * travelPerAmount(device, parameter);
    if (route.bipolar) return {std::clamp(at - std::abs(travel), 0.0f, 1.0f), std::clamp(at + std::abs(travel), 0.0f, 1.0f)};
    return {std::clamp(std::min(at, at + travel), 0.0f, 1.0f), std::clamp(std::max(at, at + travel), 0.0f, 1.0f)};
}
} // namespace sonora::devices::wavetable_routing
