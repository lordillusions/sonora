#include "io/als/DevicePresets.h"

#include "core/DeviceCatalog.h"
#include "io/als/AlsReader.h"
#include "io/als/LiveCompat.h"
#include "platform/ReferenceLibrary.h"

#include <algorithm>
#include <functional>
#include <map>
#include <set>

namespace sonora::io
{
namespace
{
using juce::File;
using juce::XmlElement;

constexpr const char* kFolderInfo = "Ableton Folder Info";

struct RoleFolder
{
    const char* name;
    model::DeviceRole role;
};
constexpr RoleFolder kRoleFolders[]{{"Instruments", model::DeviceRole::Instrument},
                                    {"Audio Effects", model::DeviceRole::AudioEffect},
                                    {"MIDI Effects", model::DeviceRole::MidiEffect}};

bool lessIgnoreCase(const std::string& a, const std::string& b)
{
    return juce::String::fromUTF8(a.c_str()).compareNatural(juce::String::fromUTF8(b.c_str())) < 0;
}

// The catalog entry a device folder of `role` holds presets of, or nullptr.
const model::CatalogEntry* entryForFolder(const File& folder, model::DeviceRole role)
{
    const auto name = folder.getFileName().toStdString();
    for (const auto& entry : model::deviceCatalog())
        if (entry.role == role && entry.displayName == name) return &entry;
    // Display names differ for some devices (the reference's Sampler is "Sonora Sampler" here): read one preset's root.
    for (const auto& entry : juce::RangedDirectoryIterator(folder, true, "*.adv", File::findFiles))
        if (! entry.getFile().getFullPathName().contains(kFolderInfo))
            return model::findCatalogEntry(devicePresetTag(entry.getFile()));
    return nullptr;
}

// The library a preset file lives in: the parent of its nearest "Devices" or "Presets" ancestor.
File ownLibrary(const File& preset)
{
    for (auto folder = preset.getParentDirectory(); folder.getParentDirectory() != folder; folder = folder.getParentDirectory())
        if (folder.getFileName() == "Devices" || folder.getFileName() == "Presets") return folder.getParentDirectory();
    return {};
}
} // namespace

std::vector<PresetLibrary> presetLibraries(const std::string& userLibrary, const std::vector<std::string>& packFolders)
{
    std::vector<PresetLibrary> result;
    std::set<juce::String> seen;
    auto add = [&](const File& folder, const std::string& name) {
        if (folder.isDirectory() && seen.insert(folder.getFullPathName()).second)
            result.push_back({folder.getFullPathName().toStdString(), name});
    };
    const auto forced = juce::SystemStats::getEnvironmentVariable("SONORA_REFERENCE_LIBRARY", {});
    if (forced.isNotEmpty())
    {
        for (const auto& path : juce::StringArray::fromTokens(forced, juce::String::charToString(File::getSeparatorChar() == '\\' ? ';' : ':'), ""))
            if (File::isAbsolutePath(path)) add(File(path), File(path).getFileName().toStdString());
        return result;
    }
    for (const auto& library : platform::referenceCoreLibraries()) add(library, "Core Library");
    add(userLibrary.empty() ? platform::referenceUserLibrary() : File(juce::String::fromUTF8(userLibrary.c_str())), "User Library");
    std::vector<File> packs;
    for (const auto& folder : packFolders) if (File::isAbsolutePath(juce::String::fromUTF8(folder.c_str()))) packs.emplace_back(juce::String::fromUTF8(folder.c_str()));
    for (const auto& folder : platform::referencePackFolders()) packs.push_back(folder);
    for (const auto& folder : packs)
    {
        if (! folder.isDirectory()) continue;
        auto children = folder.findChildFiles(File::findDirectories, false);
        children.sort();
        for (const auto& pack : children) add(pack, pack.getFileName().toStdString());
    }
    return result;
}

std::vector<DevicePreset> scanDevicePresets(const std::vector<PresetLibrary>& libraries)
{
    std::vector<DevicePreset> result;
    for (const auto& library : libraries)
    {
        const File root(juce::String::fromUTF8(library.path.c_str()));
        for (const char* base : {"Devices", "Presets"})
            for (const auto& [roleName, role] : kRoleFolders)
            {
                const auto roleFolder = root.getChildFile(base).getChildFile(roleName);
                if (! roleFolder.isDirectory()) continue;
                for (const auto& deviceFolder : roleFolder.findChildFiles(File::findDirectories, false))
                {
                    if (deviceFolder.getFileName() == kFolderInfo) continue;
                    const auto* entry = entryForFolder(deviceFolder, role);
                    if (entry == nullptr || ! entry->implemented || entry->role != role || model::isRackTag(entry->xmlTag)) continue;
                    for (const auto& item : juce::RangedDirectoryIterator(deviceFolder, true, "*.adv", File::findFiles))
                    {
                        const auto file = item.getFile();
                        const auto relative = file.getRelativePathFrom(deviceFolder);
                        if (relative.contains(kFolderInfo) || file.getFileName().startsWith(".")) continue;
                        const auto parts = juce::StringArray::fromTokens(relative, juce::String::charToString(File::getSeparatorChar()), "");
                        result.push_back({file.getFullPathName().toStdString(), file.getFileNameWithoutExtension().toStdString(),
                                          parts.size() > 1 ? parts[0].toStdString() : std::string(), std::string(entry->xmlTag),
                                          std::string(entry->displayName), library.name});
                    }
                }
            }
    }
    std::sort(result.begin(), result.end(), [](const DevicePreset& a, const DevicePreset& b) {
        if (a.deviceTag != b.deviceTag) return a.deviceTag < b.deviceTag;
        if (a.folder != b.folder) return a.folder.empty() != b.folder.empty() ? ! a.folder.empty() : lessIgnoreCase(a.folder, b.folder);
        if (a.name != b.name) return lessIgnoreCase(a.name, b.name);
        return a.path < b.path;
    });
    return result;
}

std::string devicePresetTag(const File& preset)
{
    std::string error;
    const auto dom = readAlsDom(preset, error);
    if (dom == nullptr || ! dom->hasTagName("Ableton") || dom->getFirstChildElement() == nullptr) return {};
    return dom->getFirstChildElement()->getTagName().toStdString();
}

int localizeLibraryRefs(XmlElement& element, const std::vector<File>& libraries)
{
    int repointed = 0;
    if (element.hasTagName("FileRef"))
    {
        auto* relative = element.getChildByName("RelativePath");
        auto* type = element.getChildByName("RelativePathType");
        const int kind = type != nullptr ? type->getIntAttribute("Value", -1) : -1;
        const auto name = relative != nullptr ? relative->getStringAttribute("Value") : juce::String{};
        if ((kind == 5 || kind == 6) && name.isNotEmpty() && ! File::isAbsolutePath(name) && ! name.contains(".."))
            for (const auto& library : libraries)
                if (const auto local = library.getChildFile(name); local.existsAsFile() && local.isAChildOf(library))
                {
                    auto* path = element.getChildByName("Path");
                    if (path == nullptr) path = element.createNewChildElement("Path");
                    path->setAttribute("Value", local.getFullPathName());
                    ++repointed;
                    break;
                }
    }
    for (auto* child : element.getChildIterator()) repointed += localizeLibraryRefs(*child, libraries);
    return repointed;
}

DevicePresetLoad readDevicePreset(const File& preset, const model::Song& song, const std::vector<File>& libraries)
{
    DevicePresetLoad result;
    const auto name = preset.getFileNameWithoutExtension();
    auto dom = readAlsDom(preset, result.error);
    if (dom == nullptr) return result;
    const auto* source = dom->getFirstChildElement();
    if (! dom->hasTagName("Ableton") || source == nullptr) { result.error = "Not a device preset"; return result; }
    const auto tag = source->getTagName().toStdString();
    const auto* entry = model::findCatalogEntry(tag);
    if (entry == nullptr || ! entry->implemented || model::isRackTag(tag))
    {
        result.error = (entry != nullptr ? std::string(entry->displayName) : tag) + " presets are not supported yet";
        return result;
    }
    XmlElement copy(*source);

    std::vector<File> roots;
    if (const auto own = ownLibrary(preset); own != File()) roots.push_back(own);
    for (const auto& library : libraries)
        if (std::find(roots.begin(), roots.end(), library) == roots.end()) roots.push_back(library);
    localizeLibraryRefs(copy, roots);

    // Presets store 0 for every pointee: each element gets its own id. A non-zero id used once (a preset saved by other
    // software) keeps its PointeeId references working.
    std::map<int, int> uses;
    std::function<void(const XmlElement&)> count = [&](const XmlElement& e) {
        if (isPointeeElement(e)) ++uses[e.getIntAttribute("Id", -1)];
        for (const auto* child : e.getChildIterator()) count(*child);
    };
    count(copy);
    int next = std::max(1, song.nextPointeeId);
    std::map<int, int> renamed;
    std::function<void(XmlElement&)> renumber = [&](XmlElement& e) {
        if (isPointeeElement(e))
        {
            const int old = e.getIntAttribute("Id", -1);
            if (old > 0 && uses[old] == 1) renamed[old] = next;
            e.setAttribute("Id", next++);
        }
        for (auto* child : e.getChildIterator()) renumber(*child);
    };
    renumber(copy);
    std::function<void(XmlElement&)> rebind = [&](XmlElement& e) {
        if (e.hasTagName("PointeeId"))
            if (const auto found = renamed.find(e.getIntAttribute("Value", -1)); found != renamed.end()) e.setAttribute("Value", found->second);
        for (auto* child : e.getChildIterator()) rebind(*child);
    };
    rebind(copy);

    auto* userName = copy.getChildByName("UserName");
    if (userName != nullptr && userName->getStringAttribute("Value").isEmpty()) userName->setAttribute("Value", name);
    copy.setAttribute("Id", song.nextDeviceId);

    result.device = readDeviceXml(copy, result.warnings);
    result.device.id = song.nextDeviceId;
    if (userName == nullptr) result.device.displayName = name.toStdString();
    result.pointeeEnd = next;
    return result;
}

} // namespace sonora::io
