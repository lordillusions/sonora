#include "devices/native/HushSession.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace sonora::devices
{
namespace
{
using H = Hush;
const juce::Identifier kStateTag{"SonoraHush"}, kParamTag{"Param"}, kSlotTag{"SlotB"}, kId{"id"}, kValue{"value"},
    kVersion{"version"};
constexpr int kStateVersion = 1;

juce::String focusText(float f)
{
    if (std::abs(f - 0.5f) < 0.005f) return "even";
    const int left = juce::roundToInt(std::min(1.0f, 2.0f * (1.0f - f)) * 100.0f);
    const int right = juce::roundToInt(std::min(1.0f, 2.0f * f) * 100.0f);
    return juce::String(left) + " / " + juce::String(right);
}

using V = std::map<std::string, float>;
HushPreset makePreset(const char* name, const char* category, const char* description, V values)
{
    HushPreset p;
    p.name = name;
    p.category = category;
    p.description = description;
    p.values = std::move(values);
    p.factory = true;
    return p;
}

// Band values for a factory preset: "Band n field".
void band(V& v, int n, int shape, float hz, float depth = 0.0f, float q = 0.707f, float slope = 5.0f, float focus = 0.5f)
{
    const std::string p = "Band " + std::to_string(n) + " ";
    v[p + "used"] = 1;
    v[p + "enabled"] = 1;
    v[p + "shape"] = (float) shape;
    v[p + "frequency"] = hz;
    v[p + "depth"] = depth;
    v[p + "q"] = q;
    v[p + "slope"] = slope;
    v[p + "focus"] = focus;
}
V noBands()
{
    V v;
    for (int b = 1; b <= H::kBands; ++b) v["Band " + std::to_string(b) + " used"] = 0;
    return v;
}
enum S { LowCut, LowShelf, Bell, BandPass, Notch, Tilt, HighShelf, HighCut };
} // namespace

bool HushSession::isLogarithmic(const ParameterInfo& info)
{
    return info.unit == "Hz" || info.unit == "x" || juce::String(info.name).endsWith(" q");
}

juce::String HushSession::formatValue(const ParameterInfo& info, float v)
{
    const juce::String id(info.name); // the reference name ("Band 1 focus"); ids are XML-safe
    if (info.isToggle) return v >= 0.5f ? "on" : "off";
    if (! info.choices.empty())
        return juce::String(info.choices[(size_t) juce::jlimit(0, (int) info.choices.size() - 1, (int) std::lround(v))]);
    if (id == "Band solo") return v < 0.5f ? juce::String("off") : "band " + juce::String((int) std::lround(v));
    if (id.endsWith("focus")) return focusText(v);
    if (id == "Max cut" && v >= H::kMaxCutOff - 0.01f) return "off";
    if (info.unit == "Hz") return v < 1000.0f ? juce::String(juce::roundToInt(v)) + " Hz" : juce::String(v / 1000.0f, v < 10000.0f ? 1 : 0) + " kHz";
    if (info.unit == "x") return juce::String(v, v < 1.0f ? 2 : 1) + "x";
    if (info.unit == "%") return juce::String(juce::roundToInt(v)) + " %";
    if (info.unit == "dB") return juce::String(v, 1) + " dB";
    return juce::String(v, 1);
}

float HushSession::parseValue(const ParameterInfo& info, const juce::String& text, float fallback)
{
    auto t = text.trim().toLowerCase();
    if (info.isToggle) return t == "on" || t == "1" || t.getFloatValue() >= 0.5f ? 1.0f : 0.0f;
    for (size_t c = 0; c < info.choices.size(); ++c)
        if (t == juce::String(info.choices[c]).toLowerCase()) return (float) c;
    if (t == "off") return juce::String(info.name) == "Max cut" ? H::kMaxCutOff : 0.0f;
    if (t == "even") return 0.5f;
    if (t.isEmpty() || ! t.containsAnyOf("0123456789")) return fallback;
    float v = 0;
    if (info.unit == "Hz")
    {
        // "6300", "6k3", "6.3k", "6,3k" and "6.3 kHz" are all 6300 Hz.
        t = t.removeCharacters(" hz").replaceCharacter(',', '.');
        if (t.containsChar('k'))
        {
            const auto whole = t.upToFirstOccurrenceOf("k", false, false), frac = t.fromFirstOccurrenceOf("k", false, false);
            v = (whole + (frac.isNotEmpty() && ! whole.containsChar('.') ? "." + frac : juce::String())).getFloatValue() * 1000.0f;
        }
        else v = t.getFloatValue();
    }
    else v = t.replaceCharacter(',', '.').getFloatValue();
    return juce::jlimit(info.minValue, info.maxValue, v);
}

const std::vector<HushPreset>& HushSession::factoryPresets()
{
    static const std::vector<HushPreset> presets = [] {
        std::vector<HushPreset> p;
        p.push_back(makePreset("Factory default", "misc", "A low cut, a gentle bell where harshness usually lives and a high cut.", {}));
        {
            V v = noBands();
            v["Depth"] = 0;
            p.push_back(makePreset("Flat start", "sonora", "No bands and no depth: turn depth up and shape the curve.", v));
        }
        {
            V v = noBands();
            band(v, 1, LowCut, 300);
            band(v, 2, Bell, 3200, 4.0f, 0.8f);
            band(v, 3, HighCut, 14000);
            v["Depth"] = 5.5f;
            v["Detail"] = 5.5f;
            v["Attack"] = 4.5f;
            v["Release"] = 3.0f;
            p.push_back(makePreset("Harshness smoother", "sonora", "Softens the 2-6 kHz range of any bright source.", v));
        }
        {
            V v = noBands();
            band(v, 1, LowCut, 4500, 0.0f, 0.707f, 7.0f);
            band(v, 2, HighShelf, 6000, 3.0f, 0.707f, 4.0f);
            v["Mode"] = 1;
            v["Depth"] = 5.0f;
            v["Detail"] = 1.5f;
            v["Attack"] = 1.5f;
            v["Release"] = 1.5f;
            p.push_back(makePreset("Sibilance", "sonora", "A broad, fast de-esser: hard mode on the sibilant range only.", v));
        }
        {
            V v = noBands();
            band(v, 1, LowCut, 180, 0.0f, 0.707f, 6.0f);
            band(v, 2, Bell, 2500, 4.5f, 0.6f);
            band(v, 3, HighCut, 9000, 0.0f, 0.707f, 4.0f);
            band(v, 4, Bell, 400, 3.0f, 0.7f);
            v["Depth"] = 5.0f;
            v["Detail"] = 4.0f;
            v["Attack"] = 3.5f;
            v["Release"] = 3.5f;
            p.push_back(makePreset("Vocal polish", "sonora", "Evens out boxy lows and edgy upper mids of a lead vocal.", v));
        }
        {
            V v = noBands();
            band(v, 1, Bell, 220, 4.0f, 1.1f);
            band(v, 2, HighCut, 600, 0.0f, 0.707f, 5.0f);
            v["Depth"] = 6.5f;
            v["Detail"] = 6.5f;
            v["Attack"] = 4.0f;
            v["Release"] = 6.0f;
            v["Stereo link"] = 0;
            p.push_back(makePreset("Acoustic boom", "sonora", "Tames the body resonance of close-miked acoustic guitar.", v));
        }
        {
            V v = noBands();
            band(v, 1, LowCut, 900, 0.0f, 0.707f, 4.5f);
            band(v, 2, Bell, 3000, 3.5f, 1.2f);
            band(v, 3, HighCut, 9500, 0.0f, 0.707f, 3.0f);
            v["Depth"] = 6.5f;
            v["Detail"] = 7.5f;
            v["Attack"] = 2.5f;
            v["Release"] = 3.0f;
            v["Stereo link"] = 50;
            p.push_back(makePreset("Drum bus ring", "sonora", "Narrow cuts on ringing shells and cymbal build-ups.", v));
        }
        {
            V v = noBands();
            band(v, 1, LowCut, 1500, 0.0f, 0.707f, 8.0f);
            band(v, 2, Bell, 4500, 5.0f, 1.4f);
            band(v, 3, HighCut, 11000, 0.0f, 0.707f, 6.0f);
            v["Depth"] = 7.5f;
            v["Detail"] = 9.0f;
            v["Attack"] = 2.0f;
            v["Release"] = 1.0f;
            v["Stereo mode"] = 1;
            p.push_back(makePreset("Cymbal wash", "sonora", "Calms washy overheads in mid/side with fast, precise cuts.", v));
        }
        {
            V v = noBands();
            band(v, 1, LowShelf, 160, 5.0f);
            band(v, 2, HighCut, 380, 0.0f, 0.707f, 6.0f);
            v["Mode"] = 1;
            v["Depth"] = 7.0f;
            v["Detail"] = 5.5f;
            v["Attack"] = 1.0f;
            v["Release"] = 3.0f;
            p.push_back(makePreset("Low boom", "sonora", "Holds down booming low notes of bass or synth.", v));
        }
        {
            V v = noBands();
            band(v, 1, LowCut, 120, 0.0f, 0.707f, 4.0f);
            band(v, 2, Bell, 2800, 3.0f, 0.7f, 5.0f, 0.5f);
            band(v, 3, HighCut, 12000, 0.0f, 0.707f, 5.0f);
            v["Depth"] = 4.0f;
            v["Detail"] = 2.5f;
            v["Attack"] = 6.0f;
            v["Release"] = 4.0f;
            v["Max cut"] = 4.0f;
            v["Stereo mode"] = 1;
            v["Stereo link"] = 50;
            p.push_back(makePreset("Mix bus gentle", "sonora", "Wide, slow and limited: only the worst build-ups move.", v));
        }
        {
            V v = noBands();
            band(v, 1, Tilt, 900, 2.0f, 0.707f, 0.0f);
            v["Depth"] = 2.5f;
            v["Detail"] = 3.5f;
            v["Attack"] = 5.5f;
            v["Release"] = 4.0f;
            v["Max cut"] = 2.5f;
            v["Stereo mode"] = 1;
            v["Stereo link"] = 0;
            p.push_back(makePreset("Master touch", "sonora", "A very light dynamic EQ for the master: never more than 2.5 dB.", v));
        }
        {
            V v = noBands();
            band(v, 1, BandPass, 700, 6.0f, 0.9f);
            v["Mode"] = 1;
            v["Depth"] = 10.0f;
            v["Detail"] = 10.0f;
            v["Attack"] = 0.0f;
            v["Release"] = 1.0f;
            v["Max cut"] = 12.0f;
            p.push_back(makePreset("Hollow ghost", "sonora", "Removes every tone in the band: what is left is breath and noise.", v));
        }
        return p;
    }();
    return presets;
}

HushSettings& HushSettings::get()
{
    static HushSettings settings;
    static const bool loaded = [] {
        auto& s = settings;
        if (const auto xml = juce::XmlDocument::parse(file()))
        {
            s.showTooltips = xml->getBoolAttribute("showTooltips", true);
            s.renderUltra = xml->getBoolAttribute("renderUltra", false);
            s.lowLatencyAll = xml->getBoolAttribute("lowLatencyAll", false);
            s.listenOnDrag = xml->getBoolAttribute("listenOnDrag", false);
            s.listenDelta = xml->getBoolAttribute("listenDelta", true);
            s.frequencyNearCursor = xml->getBoolAttribute("frequencyNearCursor", false);
            s.defaultPreset = xml->getStringAttribute("defaultPreset").toStdString();
        }
        return true;
    }();
    (void) loaded;
    return settings;
}

juce::File HushSettings::file()
{
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getChildFile("Sonora").getChildFile("Hush.settings");
}

void HushSettings::save() const
{
    juce::XmlElement xml("HushSettings");
    xml.setAttribute("showTooltips", showTooltips.load());
    xml.setAttribute("renderUltra", renderUltra.load());
    xml.setAttribute("lowLatencyAll", lowLatencyAll.load());
    xml.setAttribute("listenOnDrag", listenOnDrag.load());
    xml.setAttribute("listenDelta", listenDelta.load());
    xml.setAttribute("frequencyNearCursor", frequencyNearCursor.load());
    xml.setAttribute("defaultPreset", juce::String::fromUTF8(defaultPreset.c_str()));
    file().getParentDirectory().createDirectory();
    xml.writeTo(file());
}

std::string HushSession::categoryName(int category)
{
    // The reference's preset files number their categories; these names follow the presets filed under each.
    static const char* names[] = {"general", "bass", "misc", "drums", "guitar", "misc", "keys & synths", "misc",
                                  "orchestral", "vocals", "mastering", "mix bus", "mixing", "sound design"};
    return category >= 0 && category < (int) std::size(names) ? names[category] : "misc";
}

std::vector<juce::File> HushSession::referencePresetFolders()
{
    return {juce::File("/Library/Application Support/oeksound/soothe3/presets"),
            juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("oeksound/soothe3/Presets")};
}

// ---------------------------------------------------------------- session
HushSession::HushSession(Hush& hush) : hush_(hush)
{
    presetValues_ = slots_[0] = slots_[1] = snapshot();
    // A preset chosen as the default (preset menu > Set as default) starts every new instance.
    if (const auto& name = HushSettings::get().defaultPreset; ! name.empty() && name != presetName_)
        for (const auto& preset : allPresets())
            if (preset.name == name)
            {
                loadPreset(preset);
                undo_[0].clear();
                slots_[0] = slots_[1] = snapshot();
                break;
            }
    applySettings();
}

void HushSession::applySettings()
{
    const auto& settings = HushSettings::get();
    hush_.bandListenDelta.store(settings.listenDelta.load());
    hush_.setOverrides(settings.renderUltra.load() && offline ? 2 : -1, settings.lowLatencyAll.load());
}

void HushSession::setFromEditor(int index, float plainValue)
{
    const auto& infos = hush_.parameters();
    if (index < 0 || index >= (int) infos.size()) return;
    const auto& info = infos[(size_t) index];
    hush_.setParameter(index, juce::jlimit(info.minValue, info.maxValue, plainValue));
    if (onEdited) onEdited(index);
}

// ---------------------------------------------------------------- snapshots, A/B, undo
HushSession::Snapshot HushSession::snapshot() const
{
    Snapshot s((size_t) H::kParams);
    for (int i = 0; i < H::kParams; ++i) s[(size_t) i] = hush_.getParameter(i);
    return s;
}

void HushSession::restore(const Snapshot& s)
{
    for (int i = 0; i < H::kParams && i < (int) s.size(); ++i)
        if (std::abs(hush_.getParameter(i) - s[(size_t) i]) > 0.0f) setFromEditor(i, s[(size_t) i]);
}

void HushSession::selectSlot(int slot)
{
    slot = slot & 1;
    if (slot == activeSlot_) return;
    slots_[(size_t) activeSlot_] = snapshot();
    activeSlot_ = slot;
    restore(slots_[(size_t) slot]);
}

void HushSession::copyToOtherSlot()
{
    slots_[(size_t) (activeSlot_ ^ 1)] = snapshot();
}

void HushSession::pushUndo()
{
    auto& stack = undo_[(size_t) activeSlot_];
    auto now = snapshot();
    if (! stack.empty() && stack.back() == now) return;
    stack.push_back(std::move(now));
    if (stack.size() > 64) stack.erase(stack.begin());
    redo_[(size_t) activeSlot_].clear();
}

bool HushSession::undo()
{
    auto& stack = undo_[(size_t) activeSlot_];
    if (stack.empty()) return false;
    redo_[(size_t) activeSlot_].push_back(snapshot());
    restore(stack.back());
    stack.pop_back();
    return true;
}

bool HushSession::redo()
{
    auto& stack = redo_[(size_t) activeSlot_];
    if (stack.empty()) return false;
    undo_[(size_t) activeSlot_].push_back(snapshot());
    restore(stack.back());
    stack.pop_back();
    return true;
}

// ---------------------------------------------------------------- presets
juce::File HushSession::userPresetFolder()
{
    return juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("Sonora").getChildFile("Hush Presets");
}

bool HushSession::readPresetFile(const juce::File& file, HushPreset& out)
{
    const auto json = juce::JSON::parse(file.loadFileAsString());
    const auto* object = json.getDynamicObject();
    if (object == nullptr) return false;
    const auto* params = object->getProperty("params").getDynamicObject();
    if (params == nullptr) return false;
    out = {};
    out.name = object->getProperty("name").toString().toStdString();
    if (out.name.empty()) out.name = file.getFileNameWithoutExtension().toStdString();
    const auto category = object->getProperty("category");
    out.category = category.isString() ? category.toString().toStdString()
                 : (category.isInt() || category.isDouble()) ? categoryName((int) category) : "";
    out.description = object->getProperty("description").toString().toStdString();
    for (const auto& p : params->getProperties())
        if (p.value.isDouble() || p.value.isInt() || p.value.isInt64() || p.value.isBool())
            out.values[p.name.toString().toStdString()] = (float) (double) p.value;
    out.file = file;
    return true;
}

std::vector<HushPreset> HushSession::allPresets() const
{
    std::vector<HushPreset> out = factoryPresets();
    const auto addFolder = [&out](const juce::File& folder, const char* fallbackCategory) {
        if (! folder.isDirectory()) return;
        auto files = folder.findChildFiles(juce::File::findFiles, true, "*.json;*.preset");
        files.sort();
        for (const auto& f : files)
        {
            HushPreset p;
            if (! readPresetFile(f, p)) continue;
            if (p.category.empty() || std::all_of(p.category.begin(), p.category.end(), ::isdigit)) p.category = fallbackCategory;
            out.push_back(std::move(p));
        }
    };
    addFolder(userPresetFolder(), "user");
    // The reference plug-in's own presets, in its order (preset_cache.json) when it is installed.
    const auto folders = referencePresetFolders();
    const size_t firstReference = out.size();
    addFolder(folders[0], "misc");
    addFolder(folders[1], "user");
    std::map<std::string, int> order;
    if (const auto cache = juce::JSON::parse(folders[0].getSiblingFile("preset_cache.json")); cache.isObject())
        if (const auto* entries = cache.getProperty("entries", {}).getArray())
            for (const auto& e : *entries) order[e.getProperty("display_name", "").toString().toStdString()] = (int) e.getProperty("idx", 0);
    std::stable_sort(out.begin() + (long) firstReference, out.end(), [&](const HushPreset& a, const HushPreset& b) {
        const auto ia = order.find(a.name), ib = order.find(b.name);
        return (ia != order.end() ? ia->second : 100000) < (ib != order.end() ? ib->second : 100000);
    });
    addFolder(userPresetFolder().getSiblingFile("Hush Imported Presets"), "imported");
    return out;
}

void HushSession::loadPreset(const HushPreset& preset)
{
    pushUndo();
    const auto& infos = hush_.parameters();
    for (int i = 0; i < (int) infos.size(); ++i)
    {
        const auto& info = infos[(size_t) i];
        if ((qualityLocked && (i == H::Quality || i == H::LinearPhase)) || (lowLatencyLocked && i == H::LowLatency)) continue;
        if (i == H::Bypass || i == H::BandListen || i == H::SidechainListen) continue;
        const auto found = preset.values.find(info.name);
        setFromEditor(i, found != preset.values.end() ? found->second : info.defaultValue);
    }
    presetName_ = preset.name;
    presetCategory_ = preset.category.empty() ? "misc" : preset.category;
    presetValues_ = snapshot();
}

bool HushSession::presetModified() const
{
    const auto now = snapshot();
    for (int i = 0; i < H::kParams; ++i)
    {
        if (i == H::Bypass || i == H::Delta || i == H::BandListen || i == H::SidechainListen) continue;
        if (std::abs(now[(size_t) i] - presetValues_[(size_t) i]) > 1.0e-4f) return true;
    }
    return false;
}

void HushSession::stepPreset(int delta)
{
    const auto list = allPresets();
    if (list.empty()) return;
    int index = 0;
    for (int i = 0; i < (int) list.size(); ++i)
        if (list[(size_t) i].name == presetName_) index = i;
    index = (index + delta + (int) list.size()) % (int) list.size();
    loadPreset(list[(size_t) index]);
}

bool HushSession::savePreset(const juce::String& name, const juce::String& category, bool overwrite)
{
    const auto folder = userPresetFolder();
    if (! folder.createDirectory()) return false;
    const auto file = folder.getChildFile(juce::File::createLegalFileName(name) + ".json");
    if (file.exists() && ! overwrite) return false;
    auto params = std::make_unique<juce::DynamicObject>();
    const auto& infos = hush_.parameters();
    for (int i = 0; i < (int) infos.size(); ++i)
        params->setProperty(juce::Identifier(juce::String(infos[(size_t) i].name)), (double) hush_.getParameter(i));
    auto root = std::make_unique<juce::DynamicObject>();
    root->setProperty("name", name);
    root->setProperty("category", category);
    root->setProperty("description", "");
    root->setProperty("params", juce::var(params.release()));
    if (! file.replaceWithText(juce::JSON::toString(juce::var(root.release())), false, false, "\n")) return false;
    presetName_ = name.toStdString();
    presetCategory_ = category.isEmpty() ? "user" : category.toStdString();
    presetValues_ = snapshot();
    return true;
}

// ---------------------------------------------------------------- state
std::vector<uint8_t> HushSession::encodeState() const
{
    juce::XmlElement root(kStateTag);
    root.setAttribute(kVersion, kStateVersion);
    root.setAttribute("preset", juce::String::fromUTF8(presetName_.c_str()));
    root.setAttribute("presetCategory", juce::String::fromUTF8(presetCategory_.c_str()));
    root.setAttribute("slot", activeSlot_);
    root.setAttribute("scale", (double) editorScale);
    root.setAttribute("sidePanel", sidePanelOpen ? 1 : 0);
    root.setAttribute("graphRange", (double) graphRange);
    root.setAttribute("qualityLocked", qualityLocked ? 1 : 0);
    root.setAttribute("lowLatencyLocked", lowLatencyLocked ? 1 : 0);
    const auto& infos = hush_.parameters();
    for (int i = 0; i < (int) infos.size(); ++i)
    {
        auto* p = root.createNewChildElement(kParamTag);
        p->setAttribute(kId, juce::String(infos[(size_t) i].name)); // the reference names, as in states before PLG-14g
        p->setAttribute(kValue, (double) hush_.getParameter(i));
    }
    const auto& other = slots_[(size_t) (activeSlot_ ^ 1)];
    auto* b = root.createNewChildElement(kSlotTag);
    for (int i = 0; i < (int) infos.size() && i < (int) other.size(); ++i)
    {
        auto* p = b->createNewChildElement(kParamTag);
        p->setAttribute(kId, juce::String(infos[(size_t) i].name));
        p->setAttribute(kValue, (double) other[(size_t) i]);
    }
    juce::MemoryBlock block;
    juce::AudioProcessor::copyXmlToBinary(root, block);
    const auto* bytes = static_cast<const uint8_t*>(block.getData());
    return {bytes, bytes + block.getSize()};
}

bool HushSession::decodeState(const void* data, size_t size, std::string& error)
{
    if (data == nullptr || size == 0) { error = "empty state"; return false; }
    const auto root = juce::AudioProcessor::getXmlFromBinary(data, (int) size);
    if (root == nullptr || ! root->hasTagName(kStateTag)) { error = "not a Sonora Hush state"; return false; }
    if (root->getIntAttribute(kVersion) > kStateVersion) { error = "saved by a newer Sonora Hush"; return false; }
    const auto& infos = hush_.parameters();
    std::unordered_map<std::string, int> byName; // reference names, and the XML-safe ids too
    for (int i = 0; i < (int) infos.size(); ++i)
    {
        byName.emplace(infos[(size_t) i].name, i);
        byName.emplace(infos[(size_t) i].id, i);
    }
    const auto read = [&](const juce::XmlElement& parent) {
        Snapshot s((size_t) infos.size());
        for (int i = 0; i < (int) infos.size(); ++i) s[(size_t) i] = infos[(size_t) i].defaultValue;
        for (const auto* p : parent.getChildWithTagNameIterator(kParamTag))
        {
            const auto found = byName.find(p->getStringAttribute(kId).toStdString());
            if (found == byName.end()) continue;
            const double v = p->getDoubleAttribute(kValue, infos[(size_t) found->second].defaultValue);
            if (std::isfinite(v)) s[(size_t) found->second] = (float) v;
        }
        return s;
    };
    const auto current = read(*root);
    for (int i = 0; i < (int) infos.size(); ++i) hush_.setParameter(i, current[(size_t) i]);
    activeSlot_ = root->getIntAttribute("slot", 0) & 1;
    slots_[(size_t) activeSlot_] = current;
    if (const auto* b = root->getChildByName(kSlotTag)) slots_[(size_t) (activeSlot_ ^ 1)] = read(*b);
    else slots_[(size_t) (activeSlot_ ^ 1)] = current;
    presetName_ = root->getStringAttribute("preset", "Factory default").toStdString();
    presetCategory_ = root->getStringAttribute("presetCategory", "misc").toStdString();
    presetValues_ = current;
    editorScale = (float) juce::jlimit(0.5, 3.0, root->getDoubleAttribute("scale", 1.0));
    sidePanelOpen = root->getIntAttribute("sidePanel", 0) != 0;
    graphRange = (float) juce::jlimit(3.0, 40.0, root->getDoubleAttribute("graphRange", 9.0));
    qualityLocked = root->getIntAttribute("qualityLocked", 0) != 0;
    lowLatencyLocked = root->getIntAttribute("lowLatencyLocked", 0) != 0;
    undo_[0].clear(); undo_[1].clear(); redo_[0].clear(); redo_[1].clear();
    return true;
}

} // namespace sonora::devices
