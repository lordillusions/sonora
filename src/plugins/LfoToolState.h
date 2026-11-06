#pragma once
// PLG-16b: LFOTool's saved state, read for the swap to Sonora Curve LFO (plugins/NativeReplacement).
//
// The layout was measured black-box: LFOTool's own AU loaded hand-made chunks (fx_render --state-in) and reported its
// parameters, and renders over DC showed the curve and timing laws. No code or presets of the plug-in were used.
// The chunk ('FBCh' VST2 Buffer, or the "vstdata" FXP inside the AU ClassInfo) is 18816+ bytes:
//   doubles [0, 768)     segment tension per point, 12 graphs x 64 (0.5 = straight; > 0.5 starts slowly)
//   doubles [768, 1536)  point x (0..388 before version 1.2, 0..65536 after)
//   doubles [1536, 2304) point y, 0 = top (0..240 before 1.2, 0..1 after)
//   then floats/bytes from byte 18432: warps, rate, amounts, filter, smooth, timing, flags, graph routing, counts and
//   a version float (0.95, 0.99, 1.16, 1.25 seen). See LfoToolState.cpp for each offset.
#include "core/Model.h"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace sonora::plugins
{
struct LfoToolPoint
{
    double x = 0, y = 0;   // 0..1, y = 1 at the top of the graph (full volume / highest cutoff)
    double tension = 0.5;  // the segment that starts here: 0.5 straight, > 0.5 slow start, < 0.5 fast start
};

struct LfoToolState
{
    float version = 0;
    std::array<std::vector<LfoToolPoint>, 12> graphs;
    std::array<float, 12> warp{};          // normalized, 18 steps 1/16x .. 16x
    float rate = 0.435f;                   // normalized; tempo divisions or 440 * v^4 Hz when free
    bool sync = true, dotted = true, triplet = true;
    float swing = 0.5f, phase = 0, pwm = 0.5f, depth = 1;
    float cut = 0.5f, res = 0.5f, vol = 1, pan = 0.5f, var = 0.5f; // normalized, 0.5 = 0 %
    std::array<int, 5> graphFor{};        // Cut, Res, Vol, Pan, Var -> graph 0..11
    bool filterOn = false;
    int filterType = 0;                    // index into LFOTool's list; -1 = not stored (old states)
    float cutoff = 0.5f, resonance = 0.01f, mix = 1, filterVar = 0.5f, drive = 0;
    float smooth = 0, timing = 0.5f;       // timing normalized: -4096..4096 samples
    bool crossover = false, retrig = false, gate = false, velocityPwm = false, noteRate = false;
};

struct LfoToolDecode
{
    bool ok = false;
    bool empty = false;    // the plug-in saved an all-zero chunk (nothing to read)
    std::string error;
    LfoToolState state;
};

// The raw chunk from a VST2 <VstPreset> Buffer, or from an AU ClassInfo (XML plist with an FXP in "vstdata").
std::vector<uint8_t> lfoToolChunk(const model::PluginInfo& plugin, model::DeviceKind kind);
LfoToolDecode decodeLfoToolChunk(std::span<const uint8_t> chunk);

// LFOTool laws measured from the plug-in (exposed for tests and the probe).
double lfoToolCutoffHz(double normalized);              // 21 Hz .. 14.7 kHz
double lfoToolSmoothMs(double normalized);              // one-pole time constant
double lfoToolRateBeats(double normalized, bool dotted, bool triplet); // tempo-synced cycle length, 1 bar = 4 beats
double lfoToolTensionK(double tension);                 // segment law (e^(k s) - 1) / (e^k - 1)

// Writes everything Sonora Curve LFO can represent into `curveLfo` (a SonoraCurveLfo model) and lists what it could
// not carry, or only approximately, in `notes`.
void applyLfoToolState(const LfoToolState& state, model::Device& curveLfo, std::vector<std::string>& notes);
} // namespace sonora::plugins
