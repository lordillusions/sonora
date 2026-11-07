// ALS-15: sets Sonora writes open in the reference app (io/als/LiveCompat) and read back unchanged in Sonora.
#include "TestFramework.h"
#include "devices/DeviceFactory.h"
#include "devices/native/CurveLfo.h"
#include "io/als/AlsNewSet.h"
#include "io/als/AlsReader.h"
#include "io/als/AlsTemplates.h"
#include "io/als/AlsWriter.h"
#include "io/als/LiveCompat.h"
#include "plugins/SonoraPluginState.h"

#include <juce_core/juce_core.h>
#include <functional>

using namespace sonora;

namespace
{
juce::XmlElement* firstDevices(juce::XmlElement& e)
{
    if (e.hasTagName("Devices")) return &e;
    for (auto* c : e.getChildIterator())
        if (auto* found = firstDevices(*c)) return found;
    return nullptr;
}

juce::String writtenText(const juce::XmlElement& dom)
{
    juce::TemporaryFile file(".xml");
    std::string error;
    REQUIRE(io::writeAlsDom(dom, file.getFile(), error));
    return file.getFile().loadFileAsString();
}
} // namespace

TEST_CASE("live compat: Sonora devices are written as a loadable VST3 PluginDevice and read back unchanged (ALS-15)")
{
    devices::installSonoraVst3StateEncoder();
    auto dom = io::createNewSetDom();
    auto* list = firstDevices(*dom);
    REQUIRE(list != nullptr);
    auto* device = list->createNewChildElement("SonoraCurveLfo");
    device->setAttribute("Id", 3);
    auto* on = device->createNewChildElement("On");
    on->createNewChildElement("Manual")->setAttribute("Value", "true");
    on->createNewChildElement("AutomationTarget")->setAttribute("Id", 777);
    device->createNewChildElement("UserName")->setAttribute("Value", "Wobble");
    device->createNewChildElement("Rate")->createNewChildElement("Manual")->setAttribute("Value", "7");
    device->createNewChildElement("CompatMode")->setAttribute("Value", "0.00000000000000000"); // an old template's int
    const auto before = device->toString();

    const auto text = writtenText(*dom);
    // What the reference sees: a PluginDevice for Sonora's Curve LFO VST3, the device's own On, the state it loads.
    const auto xml = juce::parseXML(text);
    REQUIRE(xml != nullptr);
    auto* written = firstDevices(*xml);
    REQUIRE(written != nullptr);
    auto* wrapper = written->getChildElement(0);
    REQUIRE(wrapper != nullptr);
    CHECK(wrapper->hasTagName("PluginDevice"));
    CHECK(wrapper->getIntAttribute("Id") == 3);
    CHECK(wrapper->getChildByName("On")->getChildByName("AutomationTarget")->getIntAttribute("Id") == 777);
    CHECK(wrapper->getChildByName("UserName")->getStringAttribute("Value") == "Wobble");
    const auto* info = wrapper->getChildByName("PluginDesc")->getChildByName("Vst3PluginInfo");
    REQUIRE(info != nullptr);
    const auto uid = io::sonoraVst3Uid("SonoraCurveLfo");
    CHECK((uint32_t) info->getChildByName("Uid")->getChildByName("Fields.3")->getIntAttribute("Value") == uid[3]);
    CHECK(info->getChildByName("Name")->getStringAttribute("Value") == "Sonora Curve LFO");
    const auto state = io::decodeHexBlob(info->getChildByName("Preset")->getChildByName("Vst3Preset")
                                             ->getChildByName("ProcessorState")->getAllSubText().toStdString());
    devices::CurveLfo decoded;
    std::string error;
    CHECK(plugins::decodeCurveLfoState(state.data(), state.size(), decoded, error)); // the VST3 restores this
    CHECK(text.contains("<CompatMode Value=\"0\"/>"));
    CHECK(! text.contains("0.00000000000000000"));
    for (auto* child : written->getChildIterator()) CHECK(! io::isSonoraOnlyDevice(*child));
    CHECK(wrapper->getChildByName("SonoraNativeDevice") != nullptr);
    CHECK(dom->toString().contains("<SonoraCurveLfo")); // the caller's DOM is left as it was

    // What Sonora reads: the device in its place, exactly as written apart from the int fix.
    juce::TemporaryFile file(".xml");
    std::string writeError;
    REQUIRE(io::writeAlsDom(*dom, file.getFile(), writeError));
    auto back = io::readAlsDom(file.getFile(), error);
    REQUIRE(back != nullptr);
    auto* restored = firstDevices(*back)->getChildElement(0);
    REQUIRE(restored != nullptr);
    CHECK(restored->hasTagName("SonoraCurveLfo"));
    CHECK(restored->toString() == juce::String(before).replace("0.00000000000000000", "0"));
    auto loaded = io::loadAls(file.getFile());
    REQUIRE(loaded);
    bool found = false;
    for (const auto& track : loaded.song->tracks)
        for (const auto& d : track.devices) found |= d.type == "SonoraCurveLfo" && d.displayName == "Wobble";
    CHECK(found);
}

TEST_CASE("live compat: repeated pointee ids get fresh ids on load, NextPointeeId moves past them (ALS-15)")
{
    auto dom = io::createNewSetDom();
    auto* liveSet = dom->getChildByName("LiveSet");
    REQUIRE(liveSet != nullptr);
    auto* next = liveSet->getChildByName("NextPointeeId");
    REQUIRE(next != nullptr);
    const int start = next->getIntAttribute("Value");
    auto* holder = liveSet->createNewChildElement("FreezeSequencer");
    for (int copy = 0; copy < 2; ++copy)
    {
        auto* volume = holder->createNewChildElement("VolumeModulationTarget"); volume->setAttribute("Id", start + 50);
        auto* transpose = holder->createNewChildElement("TranspositionModulationTarget"); transpose->setAttribute("Id", start + 51);
    }
    CHECK(io::isPointeeElement(*holder->getChildElement(0)));
    CHECK(io::repairDuplicatePointeeIds(*dom) == 2);
    CHECK(holder->getChildElement(0)->getIntAttribute("Id") == start + 50); // the first use keeps its id
    CHECK(holder->getChildElement(1)->getIntAttribute("Id") == start + 51);
    CHECK(holder->getChildElement(2)->getIntAttribute("Id") == start + 52);
    CHECK(holder->getChildElement(3)->getIntAttribute("Id") == start + 53);
    CHECK(next->getIntAttribute("Value") == start + 54);
    CHECK(io::repairDuplicatePointeeIds(*dom) == 0);
}

TEST_CASE("live compat: every element under a slot <Value> carries an Id (fresh Simpler / Sampler, older saves repaired)")
{
    using juce::XmlElement;
    std::function<int(const XmlElement&)> missing = [&](const XmlElement& e) {
        int n = 0;
        for (const auto* c : e.getChildIterator())
            n += (e.hasTagName("Value") && ! c->hasAttribute("Id") ? 1 : 0) + missing(*c);
        return n;
    };
    for (const char* tag : {"OriginalSimpler", "MultiSampler"})
    {
        auto device = devices::nativeDeviceModel(tag);
        device.id = 3;
        const auto xml = juce::parseXML(juce::String(io::deviceTemplate(device)));
        REQUIRE(xml != nullptr);
        CHECK(missing(*xml) == 0);
        CHECK(xml->getChildByName("Filter") != nullptr);
    }
    // A set saved by 0.1.22 or earlier: the save-time pass adds the Ids the reference requires.
    XmlElement root("Ableton");
    auto* value = root.createNewChildElement("Filter")->createNewChildElement("Slot")->createNewChildElement("Value");
    value->createNewChildElement("SimplerFilter")->createNewChildElement("Freq");
    REQUIRE(missing(root) == 1);
    const auto repaired = io::liveCompatibleCopy(root);
    REQUIRE(repaired != nullptr);
    CHECK(missing(*repaired) == 0);
    CHECK(repaired->getChildByName("Filter")->getChildByName("Slot")->getChildByName("Value")->getChildByName("SimplerFilter")->getIntAttribute("Id", -1) == 0);
}

TEST_CASE("live compat: a Simpler LFO's RateType is written as an enum, and older true/false saves are repaired")
{
    auto device = devices::nativeDeviceModel("OriginalSimpler");
    device.id = 3;
    const auto xml = juce::parseXML(juce::String(io::deviceTemplate(device)));
    REQUIRE(xml != nullptr);
    const auto* rate = xml->getChildByName("Lfo")->getChildByName("Slot")->getChildByName("Value")->getChildByName("SimplerLfo")->getChildByName("RateType");
    REQUIRE(rate != nullptr);
    CHECK(rate->getChildByName("Manual")->getStringAttribute("Value") == "0");
    juce::XmlElement root("Ableton");
    root.createNewChildElement("RateType")->createNewChildElement("Manual")->setAttribute("Value", "true");
    const auto repaired = io::liveCompatibleCopy(root);
    REQUIRE(repaired != nullptr);
    CHECK(repaired->getChildByName("RateType")->getChildByName("Manual")->getStringAttribute("Value") == "1");
}
