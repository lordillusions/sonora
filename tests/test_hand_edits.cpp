// UI-92b: engine::handFingerprint changes when a person moves a control (pan, send, solo, a native device knob, a rack
// chain's mixer) and stays the same for values that automation or clip envelopes drive, so playback never makes the set
// look edited.
#include "TestFramework.h"

#include "devices/DeviceFactory.h"
#include "engine/EngineGraph.h"
#include "engine/SampleCache.h"
#include "io/als/AlsReader.h"

#include <juce_core/juce_core.h>

using namespace sonora;

TEST_CASE("hand edits (UI-92b): moved controls change the fingerprint, automated ones do not")
{
    auto loaded = io::loadAls(juce::File(SONORA_TEST_FIXTURES).getChildFile("minimal_set.xml"));
    REQUIRE(loaded);
    auto& song = *loaded.song;
    engine::SampleCache cache;
    devices::BuildContext context;
    auto built = engine::buildGraph(song, context, cache);
    REQUIRE(built.graph);
    auto& graph = *built.graph;
    REQUIRE(! graph.nodes.empty());
    auto& node = graph.nodes[0];
    auto& controls = (*graph.controls)[(size_t) node.controlIndex];
    const auto base = engine::handFingerprint(graph, song);
    CHECK(engine::handFingerprint(graph, song) == base); // nothing moved

    // Automated on track 1 (fixture envelopes): volume 100, speaker 102, EQ Eight gain 104 and On 105.
    controls.volume.store(controls.volume.load() * 0.5f);
    controls.active.store(! controls.active.load());
    REQUIRE(node.processorForModelDevice.size() >= 2);
    auto* eq = node.processorForModelDevice[1];
    REQUIRE(eq != nullptr);
    int gain = -1;
    for (size_t i = 0; i < eq->parameters().size(); ++i) if (eq->parameters()[i].id == "GlobalGain") gain = (int) i;
    REQUIRE(gain >= 0);
    eq->setParameter(gain, eq->getParameter(gain) + 3.0f);
    eq->setBypassed(! eq->isBypassed());
    CHECK(engine::handFingerprint(graph, song) == base); // what automation plays is not a hand edit

    // Not automated: pan 101, the send 103, solo, and any other knob of the EQ.
    auto moved = [&](auto&& change, auto&& undo) {
        change();
        const bool changed = engine::handFingerprint(graph, song) != base;
        undo();
        return changed;
    };
    const float pan = controls.pan.load();
    CHECK(moved([&] { controls.pan.store(pan + 0.1f); }, [&] { controls.pan.store(pan); }));
    const float send = controls.sends[0].load();
    CHECK(moved([&] { controls.sends[0].store(send * 0.5f + 0.1f); }, [&] { controls.sends[0].store(send); }));
    CHECK(moved([&] { controls.solo.store(true); }, [&] { controls.solo.store(false); }));
    const int other = gain == 0 ? 1 : 0;
    const float knob = eq->getParameter(other);
    const auto& info = eq->parameters()[(size_t) other];
    CHECK(moved([&] { eq->setParameter(other, knob > info.minValue ? info.minValue : info.maxValue); }, [&] { eq->setParameter(other, knob); }));
    CHECK(engine::handFingerprint(graph, song) == base); // every move above was put back

    // Without the envelopes the same volume move counts.
    auto unautomated = song;
    unautomated.tracks[0].automation.clear();
    const auto before = engine::handFingerprint(graph, unautomated);
    controls.volume.store(controls.volume.load() * 0.5f);
    CHECK(engine::handFingerprint(graph, unautomated) != before);
}
