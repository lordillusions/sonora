#include "TestFramework.h"
#include "devices/DeviceFactory.h"
#include "devices/native/CurveFilter.h"
#include "devices/native/CurveLfo.h"
#include "dsp/CurveGraph.h"
#include "plugins/LfoToolState.h"
#include "plugins/NativeReplacement.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <bit>
#include <cmath>

// PLG-16b: LFOTool's saved state carried into Sonora Curve LFO. The chunks are authored here from the measured layout
// (plugins/LfoToolState.h); no LFOTool preset or state from elsewhere is used.
using namespace sonora;

namespace
{
using Bytes = std::vector<uint8_t>;
constexpr size_t kTail = 18432;

struct Chunk
{
    Bytes bytes;
    explicit Chunk(float version, size_t size = 18824) : bytes(size, 0)
    {
        value(85, version);
        for (int g = 0; g < 12; ++g) value(13 + (size_t) g, 8.0f / 17); // warps 1x
        value(26, 18.5f / 42); value(27, 0.5f); value(28, 0.5f); value(29, 0.5f); value(30, 0.5f);
        value(31, 1); value(32, 0.5f); value(35, 1); value(36, 0.5f);
        value(77, 0.5f); value(78, 1); value(80, 0.5f); value(81, 0.01f); value(87, 0.5f);
    }
    void number(size_t index, double v)
    {
        const auto bits = std::bit_cast<uint64_t>(v);
        for (size_t i = 0; i < 8; ++i) bytes[index * 8 + i] = (uint8_t) (bits >> (8 * i));
    }
    void value(size_t slot, float v)
    {
        const auto bits = std::bit_cast<uint32_t>(v);
        for (size_t i = 0; i < 4; ++i) bytes[kTail + slot * 4 + i] = (uint8_t) (bits >> (8 * i));
    }
    void flag(size_t offset, uint8_t v) { bytes[kTail + offset] = v; }
    // Stored units: x 0..scaleX, y 0..scaleY with 0 at the top, tension 0.5 straight.
    void graph(int g, const std::vector<std::array<double, 3>>& points)
    {
        for (size_t i = 0; i < 64; ++i)
        {
            const auto& p = i < points.size() ? points[i] : points.back();
            number((size_t) g * 64 + i, i < points.size() ? p[2] : 0.5);
            number(768 + (size_t) g * 64 + i, p[0]);
            number(1536 + (size_t) g * 64 + i, i < points.size() ? p[1] : 0.0);
        }
        flag(352 + (size_t) g, (uint8_t) (points.size() - 1));
    }
};

model::Device converted(const Bytes& bytes, std::vector<std::string>& notes)
{
    const auto decoded = plugins::decodeLfoToolChunk(bytes);
    REQUIRE(decoded.ok);
    auto device = devices::nativeDeviceModel("SonoraCurveLfo");
    plugins::applyLfoToolState(decoded.state, device, notes);
    return device;
}
double param(const model::Device& d, const std::string& path)
{
    const auto it = std::find_if(d.parameters.begin(), d.parameters.end(), [&](const auto& p) { return p.path == path; });
    REQUIRE(it != d.parameters.end());
    return it->value;
}
long iparam(const model::Device& d, const std::string& path) { return std::lround(param(d, path)); }
dsp::CurveGraph graphOf(const model::Device& d, int g)
{
    dsp::CurveGraph graph;
    const std::string prefix = "G" + std::to_string(g + 1) + ".";
    graph.count = (int) std::lround(param(d, prefix + "Count"));
    for (int i = 0; i < dsp::CurveGraph::kMaxPoints; ++i)
    {
        graph.x[i] = (float) param(d, prefix + std::to_string(i) + "X");
        graph.y[i] = (float) param(d, prefix + std::to_string(i) + "Y");
        graph.curve[i] = (float) param(d, prefix + std::to_string(i) + "C");
    }
    return graph;
}
bool contains(const std::vector<std::string>& notes, const std::string& part)
{
    return std::any_of(notes.begin(), notes.end(), [&](const std::string& n) { return n.find(part) != std::string::npos; });
}
model::Device lfoToolDevice(model::DeviceKind kind)
{
    model::Device d;
    d.kind = kind;
    d.type = kind == model::DeviceKind::PluginAu ? "AuPluginDevice" : "PluginDevice";
    d.displayName = "LFOTool";
    d.id = 5;
    d.plugin.emplace();
    d.plugin->name = "LFOTool";
    if (kind == model::DeviceKind::PluginAu)
    {
        d.plugin->auType = 0x61756d66; d.plugin->auSubType = 0x5866664f; d.plugin->auManufacturer = 0x58464552; // aumf XffO XFER
    }
    else
    {
        d.plugin->vst2UniqueId = 1483105871; // 'XffO'
        d.plugin->vst2PresetType = 0x46424368; // 'FBCh'
    }
    return d;
}
model::Song songWith(model::Device device)
{
    model::Song s;
    s.nextDeviceId = 100;
    model::Track track;
    track.kind = model::TrackKind::Audio; track.id = 1; track.name = "1-Bass";
    track.devices.push_back(std::move(device));
    s.tracks.push_back(std::move(track));
    return s;
}
} // namespace

TEST_CASE("LFOTool state: laws measured from the plug-in")
{
    CHECK_NEAR(plugins::lfoToolCutoffHz(0.0), 21.0, 1e-6);
    CHECK_NEAR(plugins::lfoToolCutoffHz(0.5), 562.0, 1e-6);
    CHECK_NEAR(plugins::lfoToolCutoffHz(1.0), 14700.0, 1e-3);
    CHECK_NEAR(plugins::lfoToolSmoothMs(0.8), 43.75, 0.01);
    CHECK_NEAR(plugins::lfoToolTensionK(0.75), 5.4, 1e-9);
    CHECK_NEAR(plugins::lfoToolTensionK(0.25), -5.4, 1e-9);
    CHECK_NEAR(plugins::lfoToolTensionK(0.5), 0.0, 1e-9);
    // Rate menu: 1/4 at 0.435; a dotted entry falls back to the previous enabled one when dotted is off.
    CHECK_NEAR(plugins::lfoToolRateBeats(0.435, true, true), 1.0, 1e-9);
    CHECK_NEAR(plugins::lfoToolRateBeats(16.5 / 42, true, true), 1.5, 1e-9);   // 1/4.
    CHECK_NEAR(plugins::lfoToolRateBeats(16.5 / 42, false, true), 2.0, 1e-9);  // -> 1/2
    CHECK_NEAR(plugins::lfoToolRateBeats(17.5 / 42, false, false), 2.0, 1e-9); // 1/4 t off -> 1/2
}

TEST_CASE("LFOTool state: a 1.25 chunk carries graphs, timing, routing, filter and MIDI into Curve LFO")
{
    Chunk c(1.25f);
    c.graph(0, {{0, 1, 0.5}, {32768, 0.15, 0.5}, {65536, 0, 0.5}}); // a pump: silent on the beat, full at the end
    c.graph(1, {{0, 0, 0.5}, {65536, 1, 0.5}});
    c.value(26, 18.5f / 42); // 1/4
    c.flag(160, 0); c.flag(161, 1); c.flag(168, 0); // triplet off, dotted on, synced
    c.value(28, 0.75f);      // swing +50 %
    c.value(39, 0.25f);      // phase 90 degrees
    c.value(35, 0.8f);       // depth
    c.value(30, 0.75f);      // cut +50 %
    c.value(29, 0.25f);      // pan -50 %
    c.flag(366, 1); c.flag(367, 0); c.flag(368, 0); c.flag(369, 1); c.flag(370, 11); // graphs per Cut, Res, Vol, Pan, Var
    c.value(82, 1); c.value(76, 7.0f / 88); // filter on, Low 6
    c.value(80, 0.5f); c.value(81, 0.3f); c.value(78, 0.6f); c.value(77, 0.4f);
    c.value(84, 0.2f);       // drive: not carried
    c.flag(371, 1);          // crossover: not carried
    c.value(83, 0.5f);       // smooth
    c.value(87, 0.75f);      // timing +2048 samples
    c.flag(377, 1); c.flag(378, 1); c.flag(379, 0); c.flag(380, 1);
    c.value(13, 13.0f / 17); // graph 1 warp 2x
    c.value(14, 11.0f / 17); // graph 2 warp 1.5x: nearest
    std::vector<std::string> notes;
    const auto d = converted(c.bytes, notes);

    using L = devices::CurveLfo;
    CHECK(iparam(d, "Sync") == 1);
    CHECK(iparam(d, "Dotted") == 1);
    CHECK(iparam(d, "Triplet") == 0);
    CHECK_NEAR(L::rateBeats((int) param(d, "Rate"), true, false), 1.0, 1e-9);
    CHECK_NEAR(param(d, "Swing"), 0.25, 1e-6);
    CHECK_NEAR(param(d, "Phase"), 0.25, 1e-6);
    CHECK_NEAR(param(d, "Depth"), 0.8, 1e-6);
    CHECK_NEAR(param(d, "Cut.Amount"), 0.5, 1e-6);
    CHECK_NEAR(param(d, "Pan.Amount"), -0.5, 1e-6);
    CHECK_NEAR(param(d, "Vol.Amount"), 1.0, 1e-6);
    CHECK(iparam(d, "Cut.Source") == 1);
    CHECK(iparam(d, "Pan.Source") == 1);
    CHECK(iparam(d, "Var.Source") == 11);
    CHECK(iparam(d, "Filter.On") == 1);
    CHECK(iparam(d, "Filter.Type") == devices::CurveFilter::Low6);
    CHECK_NEAR(devices::CurveFilter::positionToHz((float) param(d, "Filter.Cutoff")), 562.0, 0.5);
    CHECK_NEAR(param(d, "Filter.Resonance"), 0.3, 1e-6);
    CHECK_NEAR(param(d, "Filter.Mix"), 0.6, 1e-6);
    CHECK_NEAR(param(d, "Filter.Var"), 0.4, 1e-6);
    CHECK_NEAR(param(d, "Smooth"), 2.8, 0.01);
    CHECK_NEAR(param(d, "Offset"), 2048.0, 0.5);
    CHECK(iparam(d, "Midi.Retrig") == 1);
    CHECK(iparam(d, "Midi.Gate") == 1);
    CHECK(iparam(d, "Midi.VelPwm") == 0);
    CHECK(iparam(d, "Midi.NoteRate") == 1);
    CHECK(iparam(d, "G1.Warp") == 4); // 2x
    CHECK(iparam(d, "G2.Warp") == 4); // 1.5x -> 2x
    const auto pump = graphOf(d, 0);
    REQUIRE(pump.count == 3);
    CHECK_NEAR(pump.y[0], 0.0, 1e-6); // stored y counts from the top
    CHECK_NEAR(pump.x[1], 0.5, 1e-6);
    CHECK_NEAR(pump.y[1], 0.85, 1e-6);
    CHECK_NEAR(pump.y[2], 1.0, 1e-6);
    CHECK_NEAR(pump.eval(0.25f), 0.425, 1e-4);
    const auto saw = graphOf(d, 1);
    CHECK(saw.count == 2);
    CHECK_NEAR(saw.y[0], 1.0, 1e-6);
    CHECK_NEAR(saw.y[1], 0.0, 1e-6);

    // What does not carry is listed; what carries is not.
    CHECK(contains(notes, "Filter drive 20 %"));
    CHECK(contains(notes, "crossover"));
    CHECK(contains(notes, "graph 2 (1.5x)"));
    CHECK(! contains(notes, "Rate"));
    CHECK(! contains(notes, "PWM"));
    CHECK(! contains(notes, "Filter type"));
}

TEST_CASE("LFOTool state: curved segments follow the exponential law with extra points")
{
    Chunk c(1.25f);
    c.graph(0, {{0, 1, 0.75}, {65536, 0, 0.5}}); // rising, slow start (k = 5.4)
    c.graph(1, {{0, 1, 0.1}, {65536, 0, 0.5}});  // rising, fast start
    std::vector<std::string> notes;
    const auto d = converted(c.bytes, notes);
    for (int g = 0; g < 2; ++g)
    {
        const auto graph = graphOf(d, g);
        CHECK(graph.count > 2);
        CHECK(graph.count <= dsp::CurveGraph::kMaxPoints);
        const double k = plugins::lfoToolTensionK(g == 0 ? 0.75 : 0.1);
        double worst = 0;
        for (int i = 1; i < 100; ++i)
        {
            const double s = i / 100.0;
            const double exact = k > 0 ? std::expm1(k * s) / std::expm1(k) : 1.0 - std::expm1(-k * (1 - s)) / std::expm1(-k);
            worst = std::max(worst, std::abs(graph.eval((float) s) - exact));
        }
        CHECK(worst < 0.012);
    }
}

TEST_CASE("LFOTool state: older layouts (pixels before 1.2, fewer settings in 0.95) and long graphs")
{
    Chunk old(1.16f, 18816);
    old.graph(0, {{0, 240, 0.5}, {194, 0, 0.5}, {388, 240, 0.5}}); // pixel units: a triangle peak
    old.value(82, 1); old.value(76, 2.0f / 88);                     // SVF HP
    std::vector<std::string> notes;
    auto d = converted(old.bytes, notes);
    auto g = graphOf(d, 0);
    REQUIRE(g.count == 3);
    CHECK_NEAR(g.y[0], 0.0, 1e-6);
    CHECK_NEAR(g.x[1], 0.5, 1e-6);
    CHECK_NEAR(g.y[1], 1.0, 1e-6);
    CHECK(iparam(d, "Filter.Type") == devices::CurveFilter::High12);

    Chunk v099(0.99f, 18816);
    v099.value(82, 1); v099.value(76, 4.0f / 84); // MG Low 12 in the 85-entry list
    v099.value(87, 0.9f);                         // timing is not stored before 1.0
    notes.clear();
    d = converted(v099.bytes, notes);
    CHECK(iparam(d, "Filter.Type") == devices::CurveFilter::Low12);
    CHECK_NEAR(param(d, "Offset"), 0.0, 1e-9);

    Chunk v095(0.95f, 18816);
    v095.value(35, 0.3f);  // not depth in 0.95
    v095.value(82, 0.33f); // filter on (0.95 keeps on/type in one value)
    notes.clear();
    d = converted(v095.bytes, notes);
    CHECK_NEAR(param(d, "Depth"), 1.0, 1e-9);
    CHECK(iparam(d, "Filter.On") == 1);
    CHECK(contains(notes, "version 0.95"));

    Chunk unknown(1.25f);
    unknown.value(82, 1); unknown.value(76, 60.0f / 88);
    unknown.value(31, 0.25f); // volume -50 %: inverted
    notes.clear();
    d = converted(unknown.bytes, notes);
    CHECK(iparam(d, "Filter.Type") == devices::CurveFilter::Low12);
    CHECK(contains(notes, "Filter type #61 has no Curve LFO equivalent"));
    CHECK(iparam(d, "Vol.Amount") == 0);
    CHECK(contains(notes, "Volume amount -50 %"));

    Chunk steps(1.25f); // a 16-step gate: 32 points, more than Curve LFO's 24
    std::vector<std::array<double, 3>> points;
    for (int i = 0; i < 16; ++i)
    {
        const double y = i % 2 == 0 ? 0.0 : 1.0;
        points.push_back({i * 4096.0, y, 0.5});
        points.push_back({(i + 1) * 4096.0, y, 0.5});
    }
    steps.graph(0, points);
    notes.clear();
    d = converted(steps.bytes, notes);
    CHECK(graphOf(d, 0).count == 24);
    CHECK(contains(notes, "Graph 1 has 32 points; Curve LFO keeps 24"));

    Chunk freeRate(1.25f);
    freeRate.flag(168, 1);             // free (Hz)
    freeRate.value(26, 0.5f);          // 440 * 0.5^4 = 27.5 Hz
    notes.clear();
    d = converted(freeRate.bytes, notes);
    CHECK(iparam(d, "Sync") == 0);
    CHECK_NEAR(param(d, "Hertz"), 27.5, 1e-3);

    Bytes zeros(18824, 0);
    const auto empty = plugins::decodeLfoToolChunk(zeros);
    CHECK(! empty.ok);
    CHECK(empty.empty);
    CHECK(! plugins::decodeLfoToolChunk(Bytes(100, 1)).ok);
}

TEST_CASE("LFOTool state: the swap reads VST2 and AU states; an all-zero state says so")
{
    Chunk c(1.25f);
    c.graph(0, {{0, 1, 0.5}, {65536, 0, 0.5}});
    c.value(39, 0.5f);

    auto vst2 = lfoToolDevice(model::DeviceKind::PluginVst2);
    vst2.plugin->state = c.bytes;
    model::PluginParam phase; phase.name = "Phase"; phase.value = 0.9; phase.automationTargetId = 700;
    vst2.pluginParams = {phase};
    auto found = plugins::findNativeReplacements(songWith(vst2), [](const model::Device&) { return false; });
    REQUIRE(found.size() == 1);
    CHECK_NEAR(param(found[0].device, "Phase"), 0.5, 1e-6); // the chunk wins over Live's named value
    const auto* lane = &*std::find_if(found[0].device.parameters.begin(), found[0].device.parameters.end(),
                                      [](const auto& p) { return p.path == "Phase"; });
    CHECK(lane->automationTargetId == 700); // the Phase lane keeps playing
    CHECK_NEAR(graphOf(found[0].device, 0).y[1], 1.0, 1e-6);
    CHECK(! contains(found[0].notes, "could not be read"));

    // AU: ClassInfo (XML plist) with the chunk inside an FXP under "vstdata".
    Bytes fxp{'C', 'c', 'n', 'K', 0, 0, 0, 0, 'F', 'P', 'C', 'h'};
    fxp.resize(60, 0);
    fxp.insert(fxp.end(), c.bytes.begin(), c.bytes.end());
    const auto plist = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<plist version=\"1.0\"><dict><key>manufacturer</key><integer>1481000274</integer>"
                       "<key>vstdata</key><data>" + juce::Base64::toBase64(fxp.data(), fxp.size()) + "</data></dict></plist>";
    auto au = lfoToolDevice(model::DeviceKind::PluginAu);
    au.plugin->state.assign(plist.toRawUTF8(), plist.toRawUTF8() + plist.getNumBytesAsUTF8());
    found = plugins::findNativeReplacements(songWith(au), [](const model::Device&) { return false; });
    REQUIRE(found.size() == 1);
    CHECK_NEAR(param(found[0].device, "Phase"), 0.5, 1e-6);
    CHECK_NEAR(graphOf(found[0].device, 0).y[1], 1.0, 1e-6);

    auto zero = lfoToolDevice(model::DeviceKind::PluginVst2);
    zero.plugin->state.assign(18824, 0);
    found = plugins::findNativeReplacements(songWith(zero), [](const model::Device&) { return false; });
    REQUIRE(found.size() == 1);
    CHECK(contains(found[0].notes, "LFOTool saved an empty state in this set"));
}
