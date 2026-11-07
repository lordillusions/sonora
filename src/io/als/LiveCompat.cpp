#include "io/als/LiveCompat.h"

#include "core/DeviceCatalog.h"
#include "io/als/AlsReader.h"
#include "io/als/AlsTemplates.h"

#include <algorithm>
#include <mutex>
#include <set>

namespace sonora::io
{
namespace
{
using juce::XmlElement;
const juce::String kWrapperTag{"SonoraNativeDevice"};
constexpr int kWrapperVersion = 1;

std::mutex encoderLock;
SonoraVst3StateEncoder& encoderSlot()
{
    static SonoraVst3StateEncoder encoder; // function-local: safe whatever the static-initialisation order
    return encoder;
}

constexpr uint32_t fourCC(const char (&code)[5])
{
    return (uint32_t) (uint8_t) code[0] << 24 | (uint32_t) (uint8_t) code[1] << 16 | (uint32_t) (uint8_t) code[2] << 8
           | (uint32_t) (uint8_t) code[3];
}

// Old saves wrote whole numbers of plain leaves as juce::String(double, 17): "0.00000000000000000". The reference refuses
// that text in an int node and reads "0" fine in a float node, so such a value is written as the whole number.
juce::String wholeNumberText(const juce::String& text)
{
    const int dot = text.indexOfChar('.');
    if (dot <= 0 || text.length() - dot - 1 != 17 || ! text.substring(dot + 1).containsOnly("0")) return {};
    const auto whole = text.substring(0, dot);
    const auto digits = whole.startsWithChar('-') ? whole.substring(1) : whole;
    if (digits.isEmpty() || ! digits.containsOnly("0123456789")) return {};
    return whole == "-0" ? juce::String("0") : whole;
}

// Saves before 0.1.23 wrote a Simpler LFO's RateType (an enum in the reference) as "true"/"false": "Unexpected value for
// int node". Returns that <Manual>, or nullptr.
XmlElement* booleanEnumManual(const XmlElement& e)
{
    if (! e.hasTagName("RateType")) return nullptr;
    auto* manual = e.getChildByName("Manual");
    if (manual == nullptr) return nullptr;
    const auto v = manual->getStringAttribute("Value");
    return v == "true" || v == "false" ? manual : nullptr;
}

bool needsChange(const XmlElement& e)
{
    if (e.hasAttribute("Value") && wholeNumberText(e.getStringAttribute("Value")).isNotEmpty()) return true;
    if (e.hasTagName("Value"))
        for (const auto* c : e.getChildIterator())
            if (! c->hasAttribute("Id")) return true;
    if (booleanEnumManual(e) != nullptr) return true;
    const bool devices = e.hasTagName("Devices");
    for (const auto* c : e.getChildIterator())
        if ((devices && isSonoraOnlyDevice(*c)) || needsChange(*c)) return true;
    return false;
}

std::unique_ptr<XmlElement> wrapperFor(const XmlElement& original)
{
    const auto tag = original.getTagName().toStdString();
    std::vector<std::string> warnings;
    const auto native = readDeviceXml(original, warnings);

    model::Device plugin;
    plugin.kind = model::DeviceKind::PluginVst3;
    plugin.type = "PluginDevice";
    plugin.id = original.getIntAttribute("Id", 0);
    plugin.enabled = native.enabled;
    const auto* entry = model::findCatalogEntry(tag);
    model::PluginInfo info;
    info.name = entry ? std::string(entry->displayName) : tag;
    info.vendor = "Sonora";
    info.vst3Uid = sonoraVst3Uid(tag);
    info.isInstrument = entry && entry->role == model::DeviceRole::Instrument;
    {
        const std::lock_guard<std::mutex> lock(encoderLock);
        if (encoderSlot()) info.state = encoderSlot()(native);
    }
    plugin.plugin = std::move(info);

    auto wrapper = juce::parseXML(juce::String(deviceTemplate(plugin)));
    if (! wrapper) return nullptr;
    // The device's own On (its automation id and any envelope on it stay valid in the reference) and user name.
    if (const auto* on = original.getChildByName("On"))
        if (auto* mine = wrapper->getChildByName("On")) wrapper->replaceChildElement(mine, new XmlElement(*on));
    if (const auto* name = original.getChildByName("UserName"))
        if (auto* mine = wrapper->getChildByName("UserName")) mine->setAttribute("Value", name->getStringAttribute("Value"));
    return wrapper;
}

void makeCompatible(XmlElement& e)
{
    if (e.hasAttribute("Value"))
        if (const auto whole = wholeNumberText(e.getStringAttribute("Value")); whole.isNotEmpty()) e.setAttribute("Value", whole);
    const bool devices = e.hasTagName("Devices");
    if (auto* manual = booleanEnumManual(e)) manual->setAttribute("Value", manual->getStringAttribute("Value") == "true" ? "1" : "0");
    const bool slotValue = e.hasTagName("Value"); // sets saved before 0.1.23 wrote Simpler slot values without an Id
    for (int i = 0; i < e.getNumChildElements(); ++i)
    {
        auto* c = e.getChildElement(i);
        if (slotValue && ! c->hasAttribute("Id")) c->setAttribute("Id", 0);
        makeCompatible(*c);
        if (! devices || ! isSonoraOnlyDevice(*c)) continue;
        auto wrapper = wrapperFor(*c);
        if (! wrapper) continue; // never fail a save: the device stays as it was
        e.removeChildElement(c, false);
        auto* holder = wrapper->createNewChildElement(kWrapperTag);
        holder->setAttribute("Version", kWrapperVersion);
        holder->addChildElement(c); // the device exactly as written
        e.insertChildElement(wrapper.release(), i);
    }
}

int unwrap(XmlElement& e)
{
    int restored = 0;
    const bool devices = e.hasTagName("Devices");
    for (int i = 0; i < e.getNumChildElements(); ++i)
    {
        auto* c = e.getChildElement(i);
        if (devices && c->hasTagName("PluginDevice"))
            if (auto* holder = c->getChildByName(kWrapperTag); holder && holder->getIntAttribute("Version") <= kWrapperVersion)
                if (auto* inner = holder->getFirstChildElement(); inner && isSonoraOnlyDevice(*inner))
                {
                    holder->removeChildElement(inner, false);
                    e.replaceChildElement(c, inner); // deletes the wrapper
                    c = inner;
                    ++restored;
                }
        restored += unwrap(*c);
    }
    return restored;
}
} // namespace

void setSonoraVst3StateEncoder(SonoraVst3StateEncoder encoder)
{
    const std::lock_guard<std::mutex> lock(encoderLock);
    encoderSlot() = std::move(encoder);
}

std::array<uint32_t, 4> sonoraVst3Uid(std::string_view tag)
{
    // Published VST3s (src/CMakeLists.txt PLUGIN_CODE); the rest are codes no plug-in uses, so the reference reports
    // "Sonora FM" etc. as missing instead of loading something else.
    uint32_t code = fourCC("SnXx");
    if (tag == "SonoraWavetable") code = fourCC("SnPr");
    else if (tag == "SonoraCurveLfo") code = fourCC("SnCl");
    else if (tag == "SonoraHush") code = fourCC("SnHs");
    else if (tag == "SonoraFm") code = fourCC("SnFm");
    else if (tag == "SonoraVector") code = fourCC("SnVc");
    else if (tag == "SonoraResynth") code = fourCC("SnRs");
    else if (tag == "SonoraImpulse") code = fourCC("SnIm");
    else if (tag == "SonoraEcho") code = fourCC("SnEc");
    else if (tag == "SonoraLfo") code = fourCC("SnLf");
    else if (tag == "SonoraVelocity") code = fourCC("SnVl");
    return {0xABCDEF01u, 0x9182FAEBu, fourCC("Snra"), code};
}

bool isSonoraOnlyDevice(const XmlElement& device)
{
    const auto tag = device.getTagName();
    return tag.startsWith("Sonora") && tag != kWrapperTag && device.hasAttribute("Id");
}

std::unique_ptr<XmlElement> liveCompatibleCopy(const XmlElement& root)
{
    if (! needsChange(root)) return nullptr;
    auto copy = std::make_unique<XmlElement>(root);
    makeCompatible(*copy);
    return copy;
}

bool isPointeeElement(const XmlElement& e)
{
    const auto tag = e.getTagName();
    return tag == "AutomationTarget" || tag.endsWith("ModulationTarget") || tag == "Pointee" || tag.startsWith("ControllerTargets.");
}

int repairDuplicatePointeeIds(XmlElement& root)
{
    auto* liveSet = root.hasTagName("LiveSet") ? &root : root.getChildByName("LiveSet");
    if (liveSet == nullptr) return 0;
    std::vector<XmlElement*> order;
    int maxId = 0;
    std::function<void(XmlElement&)> collect = [&](XmlElement& e) {
        if (isPointeeElement(e) && e.getIntAttribute("Id", -1) >= 0)
        {
            order.push_back(&e);
            maxId = std::max(maxId, e.getIntAttribute("Id"));
        }
        for (auto* c : e.getChildIterator()) collect(*c);
    };
    collect(*liveSet);
    auto* next = liveSet->getChildByName("NextPointeeId");
    int fresh = std::max(maxId + 1, next ? next->getIntAttribute("Value", 0) : 0);
    std::set<int> seen;
    int repaired = 0;
    for (auto* e : order)
        if (! seen.insert(e->getIntAttribute("Id")).second)
        {
            e->setAttribute("Id", fresh++);
            ++repaired;
        }
    if (repaired > 0 && next != nullptr) next->setAttribute("Value", fresh);
    return repaired;
}

int unwrapSonoraDevices(XmlElement& root)
{
    return unwrap(root);
}
} // namespace sonora::io
