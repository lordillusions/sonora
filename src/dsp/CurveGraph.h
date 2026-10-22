#pragma once
// Editable multi-point curve with a tension per segment (used by the Sonora Curve LFO, DEV-16b).
// Allocation-free and header-only so the audio thread can evaluate snapshots of it.

#include <algorithm>
#include <cmath>

namespace sonora::dsp
{

struct CurveGraph
{
    static constexpr int kMaxPoints = 24;
    int count = 2;
    float x[kMaxPoints]{};
    float y[kMaxPoints]{};
    float curve[kMaxPoints]{}; // tension of the segment that starts at this point, -1..1

    // 0 linear; > 0 starts slowly (ease-in); < 0 starts quickly (ease-out).
    static float shape(float t, float tension)
    {
        t = std::clamp(t, 0.0f, 1.0f);
        const float c = std::clamp(tension, -1.0f, 1.0f);
        if (std::abs(c) < 1e-4f) return t;
        const float power = 1.0f + 3.0f * std::abs(c);
        return c > 0 ? std::pow(t, power) : 1.0f - std::pow(1.0f - t, power);
    }

    // Value at `phase` (0..1). Tolerates unsorted or partially written points: x is forced to be
    // non-decreasing, vertical segments step at their x.
    float eval(float phase) const
    {
        const int n = std::clamp(count, 1, kMaxPoints);
        phase = std::clamp(phase, 0.0f, 1.0f);
        float leftX = x[0], leftY = y[0], leftC = curve[0];
        if (phase < leftX || n == 1) return std::clamp(y[0], 0.0f, 1.0f);
        for (int i = 1; i < n; ++i)
        {
            const float rightX = std::max(x[i], leftX);
            if (phase < rightX)
            {
                const float t = (phase - leftX) / (rightX - leftX);
                return std::clamp(leftY + (y[i] - leftY) * shape(t, leftC), 0.0f, 1.0f);
            }
            leftX = rightX; leftY = y[i]; leftC = curve[i];
        }
        return std::clamp(leftY, 0.0f, 1.0f);
    }

    // Generated starting shapes for the shape menu. Returns a graph that starts at x = 0, ends at x = 1.
    static constexpr int kShapeCount = 8;
    static const char* shapeName(int kind)
    {
        static constexpr const char* names[kShapeCount] = {"Pump", "Sine", "Triangle", "Saw Up", "Saw Down", "Square", "Steps", "Gate"};
        return names[std::clamp(kind, 0, kShapeCount - 1)];
    }
    static CurveGraph preset(int kind)
    {
        CurveGraph g;
        auto set = [&g](int i, float px, float py, float pc = 0) { g.x[i] = px; g.y[i] = py; g.curve[i] = pc; };
        switch (std::clamp(kind, 0, kShapeCount - 1))
        {
            case 0: g.count = 2; set(0, 0, 0, -0.6f); set(1, 1, 1); break;
            case 1:
                g.count = 17;
                for (int i = 0; i < 17; ++i) set(i, (float) i / 16, 0.5f - 0.5f * std::cos(6.2831853f * (float) i / 16));
                break;
            case 2: g.count = 3; set(0, 0, 0); set(1, 0.5f, 1); set(2, 1, 0); break;
            case 3: g.count = 2; set(0, 0, 0); set(1, 1, 1); break;
            case 4: g.count = 2; set(0, 0, 1); set(1, 1, 0); break;
            case 5: g.count = 4; set(0, 0, 1); set(1, 0.5f, 1); set(2, 0.5f, 0); set(3, 1, 0); break;
            case 6:
                g.count = 8;
                for (int i = 0; i < 4; ++i) { set(i * 2, (float) i / 4, (float) (i + 1) / 4); set(i * 2 + 1, (float) (i + 1) / 4, (float) (i + 1) / 4); }
                g.x[7] = 1;
                break;
            default:
                g.count = 16;
                for (int i = 0; i < 4; ++i)
                {
                    const float start = (float) i / 4;
                    set(i * 4, start, 1); set(i * 4 + 1, start + 0.125f, 1);
                    set(i * 4 + 2, start + 0.125f, 0); set(i * 4 + 3, start + 0.25f, 0);
                }
                break;
        }
        return g;
    }
};

} // namespace sonora::dsp
