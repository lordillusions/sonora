#pragma once
// Sonora Curve LFO (DEV-16b): twelve editable curve graphs routed to filter cutoff, resonance,
// volume, pan and a filter "variable" parameter, with tempo or Hz timing, swing/phase/PWM/smooth,
// a multi-mode filter, MIDI graph selection / note triggering, and CC output of the cutoff curve.
// Independent design written from the public product manual's feature description; numeric curves
// are Sonora's own and are not claimed to match any other product.

#include "devices/NativeDevice.h"
#include "devices/native/CurveFilter.h"
#include "dsp/CurveGraph.h"

#include <array>
#include <atomic>

namespace sonora::devices
{

class CurveLfo final : public NativeDevice
{
public:
    static constexpr int kGraphs = 12, kDestinations = 5, kMaxPoints = dsp::CurveGraph::kMaxPoints;
    static constexpr int kRates = 27; // nine straight divisions, each also dotted and triplet
    enum Destination { Cut, Res, Vol, Pan, Var };
    enum Param
    {
        Sync, Rate, Hertz, Anchor, Dotted, Triplet, Swing, Phase, Pwm, Smooth, Offset, Depth,
        RouteStart,                                  // per destination: source graph (0..11), amount
        FilterOn = RouteStart + kDestinations * 2, FilterType, Cutoff, Resonance, FilterMix, FilterVar,
        NoteRetrig,                                  // 0 off, 1 retrigger, 2 envelope (stops at the end)
        NoteGate, VelPwm, NoteRate, NoteCutoff,
        CcOut,                                       // 0 off, 1..128 = CC 0..127
        CcDepth,                                     // 0..127
        SelectedGraph,                               // editor state
        GraphStart
    };
    static constexpr int kGraphStride = 2 + kMaxPoints * 3; // count, warp, then x/y/curve per point
    enum PointField { X, Y, Curve };
    static constexpr int sourceParam(int destination) { return RouteStart + destination * 2; }
    static constexpr int amountParam(int destination) { return RouteStart + destination * 2 + 1; }
    static constexpr int countParam(int graph) { return GraphStart + graph * kGraphStride; }
    static constexpr int warpParam(int graph) { return countParam(graph) + 1; }
    static constexpr int pointParam(int graph, int point, int field) { return countParam(graph) + 2 + point * 3 + field; }

    static double rateBeats(int index, bool dotted, bool triplet);
    static float warpValue(int index);
    static const char* destinationName(int destination);

    CurveLfo();
    std::string name() const override { return "Sonora Curve LFO"; }
    void prepare(double sampleRate, int maxBlockSize) override;
    void reset() override;
    void setHostInfo(const HostInfo& info) override { host_ = info; }
    void process(juce::AudioBuffer<float>& audio, juce::MidiBuffer& midi) override;
    std::vector<int> displayParameters() const override;

    // Graph access for editors and presets (message thread). Reading is always safe.
    dsp::CurveGraph graph(int index) const;
    void setGraph(int index, const dsp::CurveGraph& graph);
    // Position within the cycle (0..1) and the latest raw graph output per destination; for the editor.
    float displayPhase() const { return displayPhase_.load(std::memory_order_relaxed); }
    // Where the playhead sits on graph `index` (phase offset, PWM and that graph's warp applied).
    float displayPosition(int index) const;
    float displayOutput(int destination) const { return displayOutput_[(size_t) destination].load(std::memory_order_relaxed); }
    // Scope behind the graph: peak input and output level per slice of the selected graph's cycle, written while the
    // LFO runs. `displayRevision()` changes whenever the playhead or the scope moved, so editors repaint only then.
    static constexpr int kScopeBins = 256;
    float scopeInput(int bin) const { return scopeIn_[(size_t) std::clamp(bin, 0, kScopeBins - 1)].load(std::memory_order_relaxed); }
    float scopeOutput(int bin) const { return scopeOut_[(size_t) std::clamp(bin, 0, kScopeBins - 1)].load(std::memory_order_relaxed); }
    unsigned displayRevision() const { return displayRevision_.load(std::memory_order_relaxed); }
    // False while the transport is stopped and no note holds the LFO: the playhead and the modulation stay put.
    bool displayRunning() const { return displayRunning_.load(std::memory_order_relaxed); }

private:
    HostInfo host_;
    double sampleRate_ = 44100;
    CurveFilter filter_;
    double freePhase_ = 0, stoppedBeats_ = 0, notePhase_ = 0;
    bool noteStarted_ = false, envDone_ = false, gateOpen_ = true;
    int heldNotes_ = 0, controlCounter_ = 0, lastCc_ = -1;
    int sourceOverride_[kDestinations]{};
    float noteNumber_ = -1, noteVelocity_ = -1;
    float gateLevel_ = 1;
    float smoothed_[kDestinations]{};
    float cutFinal_ = 0.5f, resFinal_ = 0, varFinal_ = 0;
    std::atomic<float> displayPhase_{0}, displayShaped_{0};
    std::array<std::atomic<float>, kDestinations> displayOutput_{};
    std::array<std::atomic<float>, kScopeBins> scopeIn_{}, scopeOut_{};
    std::atomic<unsigned> displayRevision_{0};
    std::atomic<bool> displayRunning_{false};
    int scopeBin_ = -1;
    float binIn_ = 0, binOut_ = 0, followIn_ = 0, followOut_ = 0, followRelease_ = 0.998f;
    void storeScopeBin(int bin);
};

} // namespace sonora::devices
