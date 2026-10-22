#pragma once
// DEV-18d: independently fitted imported flex/LFO curves; coefficients are prepared per segment.
// The vendor's nonlinear LFO lookup quantization and extreme curve endpoints remain unverified.
#include "dsp/CurveGraph.h"
#include "dsp/ExponentialEnvelopeCurve.h"
#include <array>

namespace sonora::dsp
{
struct ExponentialCurveGraph
{
    CurveGraph graph;
    std::array<ExponentialEnvelopeCurve, CurveGraph::kMaxPoints> segments{};
    void prepare(const CurveGraph& value)
    {
        graph = value;
        for (int i = 0; i < std::clamp(graph.count, 1, CurveGraph::kMaxPoints); ++i)
            segments[static_cast<size_t>(i)] = ExponentialEnvelopeCurve::fromTension(-graph.curve[i]);
    }
    float eval(float phase) const
    {
        if (! std::isfinite(phase)) return 0;
        phase = std::clamp(phase, 0.0f, 1.0f);
        const int count = std::clamp(graph.count, 1, CurveGraph::kMaxPoints);
        float left = graph.x[0];
        if (phase < left || count == 1) return std::clamp(graph.y[0], 0.0f, 1.0f);
        for (int i = 1; i < count; ++i)
        {
            const float right = std::max(left, graph.x[i]);
            if (phase < right)
            {
                const float progress = segments[static_cast<size_t>(i - 1)].eval((phase - left) / (right - left));
                return std::clamp(graph.y[i - 1] + (graph.y[i] - graph.y[i - 1]) * progress, 0.0f, 1.0f);
            }
            left = right;
        }
        return std::clamp(graph.y[count - 1], 0.0f, 1.0f);
    }
};
} // namespace sonora::dsp
