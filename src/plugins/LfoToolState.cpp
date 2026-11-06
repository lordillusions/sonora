#include "plugins/LfoToolState.h"

#include "devices/native/CurveFilter.h"
#include "devices/native/CurveLfo.h"
#include "dsp/CurveGraph.h"
#include "plugins/NativeReplacement.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace sonora::plugins
{
namespace
{
using L = devices::CurveLfo;
using F = devices::CurveFilter;

constexpr size_t kTail = 18432;      // 2304 doubles of graph data, then the settings block
constexpr size_t kMinimumSize = 18816; // the oldest layout; 1.25 appends 8 bytes
constexpr int kGraphPoints = 64;

uint32_t fourCC(const char (&c)[5])
{
    return (uint32_t) (uint8_t) c[0] << 24 | (uint32_t) (uint8_t) c[1] << 16 | (uint32_t) (uint8_t) c[2] << 8 | (uint32_t) (uint8_t) c[3];
}

// An FXP program file ('CcnK' ... 'FPCh', 60-byte header) around a chunk: the chunk.
std::vector<uint8_t> withoutFxpHeader(std::vector<uint8_t> bytes)
{
    if (bytes.size() > 60 && std::memcmp(bytes.data(), "CcnK", 4) == 0 && std::memcmp(bytes.data() + 8, "FPCh", 4) == 0)
        bytes.erase(bytes.begin(), bytes.begin() + 60);
    return bytes;
}

std::string percent(double value)
{
    char text[32];
    std::snprintf(text, sizeof text, "%.0f %%", value * 100);
    return text;
}

// Measured with the plug-in's own parameter text at 0.05 steps; interpolated in the log domain.
constexpr double kCutoffHz[] = {21, 29, 41, 57, 79, 110, 152, 211, 292, 405, 562, 779, 1080, 1497, 2075, 2875, 3985, 5523,
                                7654, 10607, 14700};
// Tension (distance from 0.5) -> k of (e^(k s) - 1) / (e^k - 1), fitted to renders of one segment (rms < 0.003).
constexpr double kTensionU[] = {0, 0.05, 0.1, 0.15, 0.2, 0.25, 0.3, 0.35, 0.4, 0.45, 0.475, 0.49, 0.5};
constexpr double kTensionK[] = {0, 1.1, 2.1, 3.1, 4.2, 5.4, 6.8, 8.6, 11.0, 15.2, 19.4, 24.8, 64.5};
// Warp menu (18 steps, index = round(v * 17)) and Curve LFO's seven warps.
constexpr double kWarps[] = {1.0 / 16, 1.0 / 8, 1.0 / 4, 1.0 / 3, 3.0 / 8, 1.0 / 2, 2.0 / 3, 3.0 / 4, 1, 1.25, 4.0 / 3, 1.5,
                             5.0 / 3, 2, 3, 4, 8, 16};
constexpr const char* kWarpNames[] = {"1/16", "1/8", "1/4", "1/3", "3/8", "1/2", "2/3", "3/4", "1x", "1.25x", "1.33x", "1.5x",
                                      "1.66x", "2x", "3x", "4x", "8x", "16x"};
// Rate menu: 40 entries (index = floor(v * 42)), 16 bar, then per division dotted / triplet / straight from 8 bar to
// 1/32, then shorter divisions. Kind: 0 straight, 1 dotted, 2 triplet. Disabled kinds fall back to the entry before.
struct RateEntry
{
    double beats;
    int kind;
    const char* name;
};
constexpr RateEntry kRates[] = {
    {64, 0, "16 bar"},   {48, 1, "12 bar"},     {64.0 / 3, 2, "8 bar t"}, {32, 0, "8 bar"},
    {24, 1, "6 bar"},    {32.0 / 3, 2, "4 bar t"}, {16, 0, "4 bar"},   {12, 1, "3 bar"},
    {16.0 / 3, 2, "2 bar t"}, {8, 0, "2 bar"}, {6, 1, "1.5 bar"},      {8.0 / 3, 2, "bar t"},
    {4, 0, "bar"},       {3, 1, "1/2."},        {4.0 / 3, 2, "1/2 t"},  {2, 0, "1/2"},
    {1.5, 1, "1/4."},    {2.0 / 3, 2, "1/4 t"}, {1, 0, "1/4"},          {0.75, 1, "1/8."},
    {1.0 / 3, 2, "1/8 t"}, {0.5, 0, "1/8"},    {0.375, 1, "1/16."},    {1.0 / 6, 2, "1/16 t"},
    {0.25, 0, "1/16"},   {0.1875, 1, "1/32."}, {1.0 / 12, 2, "1/32 t"}, {0.125, 0, "1/32"},
    {0.09375, 1, "1/48."}, {1.0 / 24, 2, "1/48 t"}, {0.0625, 0, "1/64"}, {0.09375, 1, "1/64."},
    {1.0 / 24, 2, "1/64 t"}, {0.03125, 0, "1/128"}, {0.046875, 1, "1/128."}, {1.0 / 48, 2, "1/128 t"},
    {0.015625, 0, "1/256"}, {0.0234375, 1, "1/256."}, {1.0 / 96, 2, "1/256 t"}, {0.0078125, 0, "fast"},
};
constexpr int kRateCount = (int) (sizeof kRates / sizeof kRates[0]);

int rateIndex(double normalized, bool dotted, bool triplet)
{
    int index = std::clamp((int) std::floor(std::clamp(normalized, 0.0, 1.0) * 42.0 + 1e-6), 0, kRateCount - 1);
    while (index > 0 && ((kRates[index].kind == 1 && ! dotted) || (kRates[index].kind == 2 && ! triplet))) --index;
    return index;
}

// LFOTool filter list positions 0..20 (verified in 0.99 and 1.16+ presets by name) -> Curve LFO filter types.
int curveFilterFor(int type)
{
    switch (type)
    {
        case 0: return F::Low12;  // SVF LP
        case 1: return F::Band;   // SVF BP
        case 2: return F::High12; // SVF HP
        case 3: return F::Low6;   // MG Low 6..24 (ladder style)
        case 4: return F::Low12;
        case 5: return F::Low18;
        case 6: return F::Ladder;
        case 7: case 8: case 9: case 10: return F::Low6 + (type - 7);  // Low 6..24
        case 11: case 12: case 13: case 14: return F::High6 + (type - 11); // High 6..24
        case 15: case 16: return F::Band;  // Band 12 / 24
        case 17: case 18: return F::Peak;  // Peak 12 / 24
        case 19: case 20: return F::Notch; // Notch 12 / 24
        default: return -1;
    }
}

double expProgress(double s, double k)
{
    s = std::clamp(s, 0.0, 1.0);
    if (std::abs(k) < 1e-6) return s;
    if (k < 0) return 1.0 - expProgress(1.0 - s, -k);
    return std::expm1(k * s) / std::expm1(k);
}

// Curve LFO's segment law (dsp::CurveGraph::shape) that best follows LFOTool's exponential law, and the worst miss.
struct Fit
{
    float tension = 0;
    double error = 0;
};
Fit fitTension(double k)
{
    if (std::abs(k) < 1e-6) return {};
    const auto cost = [k](double c, bool worst) {
        double sum = 0, peak = 0;
        for (int i = 1; i < 32; ++i)
        {
            const double s = i / 32.0;
            const double d = expProgress(s, k) - dsp::CurveGraph::shape((float) s, (float) c);
            sum += d * d;
            peak = std::max(peak, std::abs(d));
        }
        return worst ? peak : sum;
    };
    const double sign = k > 0 ? 1 : -1;
    double lo = 0, hi = 1; // golden-section search on |c|
    for (int i = 0; i < 48; ++i)
    {
        const double a = lo + (hi - lo) * 0.382, b = lo + (hi - lo) * 0.618;
        if (cost(sign * a, false) < cost(sign * b, false)) hi = b; else lo = a;
    }
    const double c = sign * 0.5 * (lo + hi);
    return {(float) c, cost(c, true)};
}

struct Segment
{
    double x0, y0, x1, y1, k;
    double error() const { return x1 - x0 < 1e-9 ? 0.0 : fitTension(k).error * std::abs(y1 - y0); }
};

// One LFOTool graph -> at most 24 Curve LFO points. Points above the limit are merged where they change the curve
// least; curved segments Curve LFO's power law cannot follow get extra points on the exact curve.
dsp::CurveGraph convertGraph(const std::vector<LfoToolPoint>& points, int graph, std::vector<std::string>& notes)
{
    std::vector<LfoToolPoint> p = points;
    if (p.empty()) p = {{0, 0, 0.5}, {1, 0, 0.5}};
    double lastX = 0;
    for (auto& point : p) { point.x = std::max(point.x, lastX); lastX = point.x; } // x never goes back
    const int limit = L::kMaxPoints;
    if ((int) p.size() > limit)
    {
        const size_t original = p.size();
        while ((int) p.size() > limit)
        {
            size_t best = 1;
            double bestMiss = 1e9;
            for (size_t j = 1; j + 1 < p.size(); ++j)
            {
                const double span = p[j + 1].x - p[j - 1].x;
                const double t = span > 1e-9 ? (p[j].x - p[j - 1].x) / span : 0.5;
                const double miss = std::abs(p[j].y - (p[j - 1].y + (p[j + 1].y - p[j - 1].y) * t));
                if (miss < bestMiss) { bestMiss = miss; best = j; }
            }
            p[best - 1].tension = 0.5;
            p.erase(p.begin() + (long) best);
        }
        notes.push_back("Graph " + std::to_string(graph + 1) + " has " + std::to_string(original) + " points; Curve LFO keeps "
                        + std::to_string(limit) + " (the least visible ones were merged)");
    }
    std::vector<Segment> segments;
    for (size_t i = 0; i + 1 < p.size(); ++i)
        segments.push_back({p[i].x, p[i].y, p[i + 1].x, p[i + 1].y, lfoToolTensionK(p[i].tension)});
    int count = (int) p.size();
    while (count < limit && ! segments.empty())
    {
        size_t worst = 0;
        double worstError = 0;
        for (size_t i = 0; i < segments.size(); ++i)
            if (const double e = segments[i].error(); e > worstError) { worstError = e; worst = i; }
        if (worstError < 0.01) break;
        const auto s = segments[worst];
        const double mx = 0.5 * (s.x0 + s.x1), my = s.y0 + (s.y1 - s.y0) * expProgress(0.5, s.k);
        segments[worst] = {s.x0, s.y0, mx, my, s.k / 2};
        segments.insert(segments.begin() + (long) worst + 1, Segment{mx, my, s.x1, s.y1, s.k / 2});
        ++count;
    }
    double residual = 0;
    dsp::CurveGraph g;
    g.count = (int) segments.size() + 1;
    for (size_t i = 0; i < segments.size(); ++i)
    {
        g.x[i] = (float) segments[i].x0;
        g.y[i] = (float) segments[i].y0;
        g.curve[i] = segments[i].x1 - segments[i].x0 < 1e-9 ? 0.0f : fitTension(segments[i].k).tension;
        residual = std::max(residual, segments[i].error());
    }
    const auto& last = segments.empty() ? Segment{p[0].x, p[0].y, p[0].x, p[0].y, 0} : segments.back();
    g.x[segments.size()] = (float) last.x1;
    g.y[segments.size()] = (float) last.y1;
    if (segments.empty()) g.count = 1;
    if (residual > 0.03)
        notes.push_back("Graph " + std::to_string(graph + 1) + ": steep curves are approximated (within "
                        + percent(residual) + " of the graph height)");
    return g;
}
} // namespace

std::vector<uint8_t> lfoToolChunk(const model::PluginInfo& plugin, model::DeviceKind kind)
{
    if (kind == model::DeviceKind::PluginAu) return withoutFxpHeader(plistData(plugin.state, "vstdata"));
    if (kind != model::DeviceKind::PluginVst2) return {};
    // A parameter bank ('FxBk' / 'FxCk') has no chunk: only Live's named values remain.
    if (plugin.vst2PresetType != 0 && plugin.vst2PresetType != fourCC("FBCh") && plugin.vst2PresetType != fourCC("FPCh")) return {};
    return withoutFxpHeader(plugin.state);
}

LfoToolDecode decodeLfoToolChunk(std::span<const uint8_t> chunk)
{
    LfoToolDecode result;
    if (chunk.empty()) { result.error = "the set holds no LFOTool state"; return result; }
    if (std::all_of(chunk.begin(), chunk.end(), [](uint8_t b) { return b == 0; }))
    {
        result.empty = true;
        result.error = "LFOTool saved an empty (all-zero) state";
        return result;
    }
    if (chunk.size() < kMinimumSize) { result.error = "the saved state is too short (" + std::to_string(chunk.size()) + " bytes)"; return result; }
    const auto word = [&](size_t offset) {
        uint64_t bits = 0;
        for (size_t i = 0; i < 8; ++i) bits |= (uint64_t) chunk[offset + i] << (8 * i);
        return std::bit_cast<double>(bits);
    };
    const auto value = [&](size_t slot, float fallback) {
        uint32_t bits = 0;
        for (size_t i = 0; i < 4; ++i) bits |= (uint32_t) chunk[kTail + slot * 4 + i] << (8 * i);
        const float v = std::bit_cast<float>(bits);
        return std::isfinite(v) ? std::clamp(v, 0.0f, 1.0f) : fallback;
    };
    const auto byte = [&](size_t offset) { return chunk[kTail + offset]; };
    auto& s = result.state;
    uint32_t versionBits = 0;
    for (size_t i = 0; i < 4; ++i) versionBits |= (uint32_t) chunk[kTail + 340 + i] << (8 * i);
    s.version = std::bit_cast<float>(versionBits);
    if (! std::isfinite(s.version) || s.version < 0.5f || s.version > 10.0f)
    {
        result.error = "unknown LFOTool state version";
        return result;
    }
    double maxX = 0;
    for (int i = 0; i < 12 * kGraphPoints; ++i)
        if (const double x = word((size_t) (768 + i) * 8); std::isfinite(x)) maxX = std::max(maxX, x);
    const bool normalized = s.version >= 1.2f || maxX > 400; // 1.2+: x 0..65536, y 0..1; before: pixels 388 x 240
    const double scaleX = normalized ? 65536.0 : 388.0, scaleY = normalized ? 1.0 : 240.0;
    for (int g = 0; g < 12; ++g)
    {
        const int count = std::clamp((int) byte(352 + (size_t) g) + 1, 1, kGraphPoints);
        for (int i = 0; i < count; ++i)
        {
            const size_t index = (size_t) (g * kGraphPoints + i);
            const double t = word(index * 8), x = word((768 + index) * 8), y = word((1536 + index) * 8);
            LfoToolPoint point;
            point.tension = std::isfinite(t) ? std::clamp(t, 0.0, 1.0) : 0.5;
            point.x = std::isfinite(x) ? std::clamp(x / scaleX, 0.0, 1.0) : 1.0;
            point.y = std::isfinite(y) ? std::clamp(1.0 - y / scaleY, 0.0, 1.0) : 0.0;
            s.graphs[(size_t) g].push_back(point);
        }
        s.warp[(size_t) g] = value(13 + (size_t) g, 8.0f / 17);
    }
    s.rate = value(26, 0.435f); s.pwm = value(27, 0.5f); s.swing = value(28, 0.5f);
    s.pan = value(29, 0.5f); s.cut = value(30, 0.5f); s.vol = value(31, 1); s.res = value(32, 0.5f);
    s.phase = value(39, 0);
    s.triplet = byte(160) != 0; s.dotted = byte(161) != 0; s.sync = byte(168) == 0;
    s.cutoff = value(80, 0.5f); s.resonance = value(81, 0.01f); s.smooth = value(83, 0); s.drive = value(84, 0);
    for (size_t d = 0; d < 5; ++d) s.graphFor[d] = std::clamp((int) byte(366 + d), 0, 11);
    s.retrig = byte(377) != 0; s.velocityPwm = byte(379) != 0; s.noteRate = byte(380) != 0;
    if (s.version >= 0.97f)
    {
        s.depth = value(35, 1); s.var = value(36, 0.5f);
        s.filterVar = value(77, 0.5f); s.mix = value(78, 1);
        s.gate = byte(378) != 0;
        s.filterOn = value(82, 0) >= 0.5f;
        s.filterType = (int) std::lround(value(76, 0) * (s.version >= 1.0f ? 88.0f : 84.0f));
        if (s.version >= 1.0f)
        {
            s.timing = value(87, 0.5f);
            s.crossover = byte(371) != 0;
        }
    }
    else // 0.95: no depth, Var route, filter mix/var, gate, timing; one value holds the filter's on/type
    {
        s.graphFor[4] = 0;
        s.filterOn = value(82, 0) > 0.01f;
        s.filterType = -1;
    }
    result.ok = true;
    return result;
}

double lfoToolCutoffHz(double normalized)
{
    const double position = std::clamp(normalized, 0.0, 1.0) * 20.0;
    const int i = std::min(19, (int) position);
    const double t = position - i;
    return std::exp(std::log(kCutoffHz[i]) + (std::log(kCutoffHz[i + 1]) - std::log(kCutoffHz[i])) * t);
}

// Measured on a square graph: the 63 % time is 0.35 ms / (1 - smooth)^3 (0.1 -> 0.5 ms, 0.5 -> 2.8 ms, 0.8 -> 43 ms).
double lfoToolSmoothMs(double normalized) { return 0.35 / std::pow(1.0 - std::clamp(normalized, 0.0, 0.999), 3.0); }

double lfoToolRateBeats(double normalized, bool dotted, bool triplet) { return kRates[rateIndex(normalized, dotted, triplet)].beats; }

double lfoToolTensionK(double tension)
{
    const double u = std::clamp(std::abs(tension - 0.5), 0.0, 0.5);
    for (size_t i = 1; i < std::size(kTensionU); ++i)
        if (u <= kTensionU[i])
        {
            const double t = (u - kTensionU[i - 1]) / (kTensionU[i] - kTensionU[i - 1]);
            const double k = kTensionK[i - 1] + (kTensionK[i] - kTensionK[i - 1]) * t;
            return tension >= 0.5 ? k : -k;
        }
    return tension >= 0.5 ? kTensionK[std::size(kTensionK) - 1] : -kTensionK[std::size(kTensionK) - 1];
}

void applyLfoToolState(const LfoToolState& s, model::Device& device, std::vector<std::string>& notes)
{
    const auto set = [&device](const std::string& path, double value) {
        for (auto& p : device.parameters)
            if (p.path == path) { p.value = value; return; }
    };
    // Timing. Curve LFO's rate list has 8 bars .. 1/32, each straight, dotted (x1.5) or triplet (x2/3).
    set("Sync", s.sync ? 1 : 0);
    set("Anchor", 1);
    set("Dotted", s.dotted ? 1 : 0);
    set("Triplet", s.triplet ? 1 : 0);
    const int lfoRate = rateIndex(s.rate, s.dotted, s.triplet);
    const double beats = kRates[lfoRate].beats;
    int best = 0;
    double bestMiss = 1e9;
    for (int i = 0; i < L::kRates; ++i)
    {
        const bool usable = (i % 3 == 0) || (i % 3 == 1 && s.dotted) || (i % 3 == 2 && s.triplet);
        if (! usable) continue;
        const double miss = std::abs(std::log(L::rateBeats(i, s.dotted, s.triplet) / beats));
        if (miss < bestMiss - 1e-9) { bestMiss = miss; best = i; }
    }
    set("Rate", best);
    if (s.sync && bestMiss > 0.01)
        notes.push_back(std::string("Rate ") + kRates[lfoRate].name + " is outside Curve LFO's range (8 bars dotted .. 1/32): the nearest is used");
    const double hertz = 440.0 * std::pow((double) s.rate, 4.0); // measured: free rate text = 440 v^4 Hz
    if (! s.sync) set("Hertz", std::clamp(hertz, 0.01, 40.0));   // one Rate value: synced sets keep Curve LFO's Hz
    if (! s.sync && (hertz > 40.0 || hertz < 0.01))
    {
        char text[160];
        std::snprintf(text, sizeof text, "Free rate %.2f Hz is outside Curve LFO's 0.01..40 Hz", hertz);
        notes.push_back(text);
    }
    const double swing = s.swing - 0.5; // measured: the first cycle of each pair takes `swing` of the pair
    set("Swing", std::clamp(swing, -0.45, 0.45));
    if (std::abs(swing) > 0.45) notes.push_back("Swing " + percent(swing * 2) + " is limited to " + percent(swing > 0 ? 0.9 : -0.9));
    set("Phase", s.phase);
    // PWM: LFOTool bends the cycle as (1 - p) x + p x^16; Curve LFO squeezes two straight halves. The half-way point
    // of the cycle is matched.
    const double p = std::clamp((s.pwm - 0.5) * 2.0, -1.0, 1.0);
    if (std::abs(p) > 0.005)
    {
        const double a = std::abs(p);
        double lo = 0, hi = 1;
        for (int i = 0; i < 60; ++i)
        {
            const double x = 0.5 * (lo + hi);
            ((1 - a) * x + a * std::pow(x, 16.0) < 0.5 ? lo : hi) = x;
        }
        const double mid = p > 0 ? 0.5 * (lo + hi) : 1.0 - 0.5 * (lo + hi);
        set("Pwm", std::clamp((mid - 0.5) / 0.45, -1.0, 1.0));
        notes.push_back("PWM " + percent(p) + " follows Curve LFO's own PWM shape (the cycle's half-way point is matched)");
    }
    const double smoothMs = lfoToolSmoothMs(s.smooth);
    set("Smooth", std::min(200.0, smoothMs));
    if (smoothMs > 200) notes.push_back("Smooth " + percent(s.smooth) + " is longer than Curve LFO's 200 ms: 200 ms is used");
    set("Offset", std::clamp(((double) s.timing - 0.5) * 8192.0, -4096.0, 4096.0));
    set("Depth", s.depth);
    // Routing: Cut, Res, Vol, Pan, Var.
    const float amounts[5] = {s.cut, s.res, s.vol, s.pan, s.var};
    for (int d = 0; d < L::kDestinations; ++d)
    {
        const std::string name = L::destinationName(d);
        set(name + ".Source", s.graphFor[(size_t) d]);
        const double amount = std::clamp(((double) amounts[d] - 0.5) * 2.0, -1.0, 1.0);
        if (d == L::Vol)
        {
            set(name + ".Amount", std::max(0.0, amount));
            if (amount < -0.005) notes.push_back("Volume amount " + percent(amount) + " (inverted) has no Curve LFO equivalent: 0 % is used");
        }
        else
            set(name + ".Amount", amount);
    }
    // Filter.
    set("Filter.On", s.filterOn ? 1 : 0);
    int type = s.filterType >= 0 ? curveFilterFor(s.filterType) : F::Low12;
    if (type < 0)
    {
        type = F::Low12;
        if (s.filterOn) notes.push_back("Filter type #" + std::to_string(s.filterType + 1) + " has no Curve LFO equivalent: Low 12 is used");
    }
    else if (s.filterType < 0 && s.filterOn)
        notes.push_back("This old LFOTool state (version 0.95) does not say which filter type: Low 12 is used");
    set("Filter.Type", type);
    set("Filter.Cutoff", std::clamp(std::log(lfoToolCutoffHz(s.cutoff) / 20.0) / std::log(1000.0), 0.0, 1.0));
    set("Filter.Resonance", s.resonance);
    set("Filter.Mix", s.mix);
    set("Filter.Var", s.filterVar);
    if (s.filterOn && s.drive > 0.005f) notes.push_back("Filter drive " + percent(s.drive) + " has no Curve LFO equivalent");
    if (s.crossover) notes.push_back("The crossover (LFO on one frequency band only) has no Curve LFO equivalent: the whole signal is modulated");
    // MIDI.
    set("Midi.Retrig", s.retrig ? 1 : 0);
    set("Midi.Gate", s.gate ? 1 : 0);
    set("Midi.VelPwm", s.velocityPwm ? 1 : 0);
    set("Midi.NoteRate", s.noteRate ? 1 : 0);
    // Graphs and their warps.
    std::vector<std::string> warpNotes;
    for (int g = 0; g < L::kGraphs; ++g)
    {
        const auto graph = convertGraph(s.graphs[(size_t) g], g, notes);
        const std::string prefix = "G" + std::to_string(g + 1) + ".";
        set(prefix + "Count", graph.count);
        for (int i = 0; i < L::kMaxPoints; ++i)
        {
            const bool used = i < graph.count;
            const std::string point = prefix + std::to_string(i);
            set(point + "X", used ? graph.x[i] : 1.0);
            set(point + "Y", used ? graph.y[i] : 1.0);
            set(point + "C", used ? graph.curve[i] : 0.0);
        }
        const int warp = std::clamp((int) std::lround(s.warp[(size_t) g] * 17.0f), 0, 17);
        int nearest = 0;
        for (int w = 1; w < 7; ++w)
            if (std::abs(std::log(L::warpValue(w) / kWarps[warp])) < std::abs(std::log(L::warpValue(nearest) / kWarps[warp]))) nearest = w;
        set(prefix + "Warp", nearest);
        if (std::abs(L::warpValue(nearest) - kWarps[warp]) > 1e-4)
            warpNotes.push_back(std::to_string(g + 1) + " (" + kWarpNames[warp] + ")");
    }
    if (! warpNotes.empty())
    {
        std::string list;
        for (size_t i = 0; i < warpNotes.size(); ++i) list += (i == 0 ? "" : ", ") + warpNotes[i];
        notes.push_back("Graph warp of graph " + list + " is not in Curve LFO's 1/8x..8x powers of two: the nearest is used");
    }
}
} // namespace sonora::plugins
