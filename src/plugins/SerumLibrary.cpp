#include "plugins/SerumLibrary.h"

#include "plugins/PrismPreset.h"

#include <algorithm>
#include <string_view>

namespace sonora::plugins
{
namespace
{
juce::StringArray folderParts(const std::string& folder) { return juce::StringArray::fromTokens(juce::String(folder), "/", ""); }
std::string folderPart(const std::string& folder, int index)
{
    const auto parts = folderParts(folder);
    return index < parts.size() ? parts[index].toStdString() : std::string();
}
// [first, last) of the run of items sharing `key(items[index])`; serumLibrary() keeps each bank and group contiguous.
template <typename Key> std::pair<int, int> run(const std::vector<SerumLibraryItem>& items, int index, Key key)
{
    const auto value = key(items[(size_t) index]);
    int first = index, last = index + 1;
    while (first > 0 && key(items[(size_t) first - 1]) == value) --first;
    while (last < (int) items.size() && key(items[(size_t) last]) == value) ++last;
    return {first, last};
}
} // namespace

int serumTableFlag(const juce::File& file)
{
    juce::FileInputStream in(file);
    if (! in.openedOk()) return -1;
    char header[12]{};
    if (in.read(header, 12) != 12 || std::string_view(header, 4) != "RIFF" || std::string_view(header + 8, 4) != "WAVE") return -1;
    while (! in.isExhausted())
    {
        char id[4]{};
        if (in.read(id, 4) != 4) return -1;
        const auto size = static_cast<juce::int64>(static_cast<uint32_t>(in.readInt()));
        const auto next = in.getPosition() + size + (size & 1);
        if (std::string_view(id, 4) == "clm ")
        {
            std::string chunk(static_cast<size_t>(std::min<juce::int64>(size, 64)), '\0');
            if (in.read(chunk.data(), static_cast<int>(chunk.size())) != static_cast<int>(chunk.size())) return -1;
            if (chunk.rfind("<!>", 0) != 0) return -1;
            const auto space = chunk.find(' ');
            if (space == std::string::npos || space + 1 >= chunk.size() || chunk[space + 1] < '0' || chunk[space + 1] > '9') return -1;
            return chunk[space + 1] - '0';
        }
        if (! in.setPosition(next)) return -1;
    }
    return -1;
}

devices::WavetableLoadResult loadSerumTable(const juce::File& file)
{
    const int flag = serumTableFlag(file);
    if (flag < 0) return devices::WavetableData::fromFile(file);
    return devices::WavetableData::fromFile(file, devices::WavetableData::nativeFrameLength, devices::WavetableData::PlaybackProfile::Dense12,
        {flag == 0 ? devices::WavetableFrameLayout::Steps : devices::WavetableFrameLayout::Interpolated256, serumTableGain, true});
}

std::vector<juce::File> serumLibraryRoots()
{
    std::vector<juce::File> roots;
    for (const auto& root : {juce::File("/Library/Audio/Presets/Xfer Records/Serum 2 Presets"),
                              juce::File::getSpecialLocation(juce::File::userHomeDirectory).getChildFile("Library/Audio/Presets/Xfer Records/Serum 2 Presets")})
        if (root.isDirectory()) roots.push_back(root);
    return roots;
}

std::vector<juce::File> presetLibraryRoots()
{
    std::vector<juce::File> roots;
    if (const auto prism = prismLibraryRoot(); prism.isDirectory()) roots.push_back(prism);
    for (const auto& root : serumLibraryRoots()) roots.push_back(root);
    return roots;
}

std::vector<SerumLibraryItem> serumLibrary(const std::string& kind)
{
    const juce::String pattern = kind == "Presets" ? "*.prism;*.SerumPreset" : "*.wav";
    std::vector<SerumLibraryItem> items;
    for (const auto& root : presetLibraryRoots())
    {
        const auto base = root.getChildFile(juce::String(kind));
        if (! base.isDirectory()) continue;
        for (const auto& entry : juce::RangedDirectoryIterator(base, true, pattern, juce::File::findFiles))
        {
            const auto file = entry.getFile();
            SerumLibraryItem item;
            item.path = file.getFullPathName().toStdString();
            item.name = file.getFileNameWithoutExtension().toStdString();
            item.folder = file.getParentDirectory().getRelativePathFrom(base).replaceCharacter('\\', '/').toStdString();
            if (item.folder == ".") item.folder.clear();
            items.push_back(std::move(item));
        }
    }
    // Folder by folder ("Bass" < "Bass/Reese" < "Bass Extra"), so a bank or group is never split by a sibling whose name
    // continues with a character that sorts before '/'.
    std::sort(items.begin(), items.end(), [](const auto& a, const auto& b) {
        const auto left = folderParts(a.folder), right = folderParts(b.folder);
        for (int i = 0; i < std::min(left.size(), right.size()); ++i)
            if (const int order = left[i].compareNatural(right[i]); order != 0) return order < 0;
        if (left.size() != right.size()) return left.size() < right.size();
        return juce::String(a.name).compareNatural(juce::String(b.name)) < 0;
    });
    return items;
}

int serumLibraryStep(const std::vector<SerumLibraryItem>& items, const std::string& pathOrName, int direction)
{
    const int count = static_cast<int>(items.size());
    for (int i = 0; i < count; ++i)
        if (items[static_cast<size_t>(i)].path == pathOrName || items[static_cast<size_t>(i)].name == pathOrName)
            return ((i + direction) % count + count) % count;
    return -1;
}

std::string serumLibraryBank(const SerumLibraryItem& item) { return folderPart(item.folder, 0); }
std::string serumLibraryGroup(const SerumLibraryItem& item) { return folderPart(item.folder, 1); }

int serumLibraryFind(const std::vector<SerumLibraryItem>& items, const std::string& pathOrName)
{
    for (size_t i = 0; i < items.size(); ++i)
        if (items[i].path == pathOrName) return (int) i;
    for (size_t i = 0; i < items.size(); ++i)
        if (items[i].name == pathOrName) return (int) i;
    return -1;
}

std::vector<std::string> serumLibraryBanks(const std::vector<SerumLibraryItem>& items)
{
    std::vector<std::string> banks;
    for (const auto& item : items)
        if (const auto bank = serumLibraryBank(item); std::find(banks.begin(), banks.end(), bank) == banks.end()) banks.push_back(bank);
    return banks;
}

std::vector<std::string> serumLibraryBankFolders(const std::string& kind)
{
    juce::StringArray names;
    for (const auto& root : presetLibraryRoots())
        for (const auto& entry : juce::RangedDirectoryIterator(root.getChildFile(juce::String(kind)), false, "*", juce::File::findDirectories))
            names.addIfNotAlreadyThere(entry.getFile().getFileName());
    std::vector<std::string> banks;
    for (const auto& name : names) banks.push_back(name.toStdString());
    std::sort(banks.begin(), banks.end(), [](const auto& a, const auto& b) { return juce::String(a).compareNatural(juce::String(b)) < 0; });
    return banks;
}

std::vector<std::string> serumLibraryGroups(const std::vector<SerumLibraryItem>& items, const std::string& bank)
{
    std::vector<std::string> groups;
    for (const auto& item : items)
        if (serumLibraryBank(item) == bank)
            if (const auto group = serumLibraryGroup(item); groups.empty() || groups.back() != group) groups.push_back(group);
    return groups;
}

int serumLibraryStepInBank(const std::vector<SerumLibraryItem>& items, const std::string& pathOrName, int direction)
{
    const int index = serumLibraryFind(items, pathOrName);
    if (index < 0) return -1;
    const auto [first, last] = run(items, index, serumLibraryBank);
    const int count = last - first;
    return first + ((index - first + direction) % count + count) % count;
}

int serumLibraryStepGroup(const std::vector<SerumLibraryItem>& items, const std::string& pathOrName, int direction)
{
    const int index = serumLibraryFind(items, pathOrName);
    if (index < 0) return -1;
    const auto [bankFirst, bankLast] = run(items, index, serumLibraryBank);
    const auto groupKey = [](const SerumLibraryItem& item) { return serumLibraryBank(item) + '\n' + serumLibraryGroup(item); };
    const auto [groupFirst, groupLast] = run(items, index, groupKey);
    if (groupFirst == bankFirst && groupLast == bankLast) return -1; // one group only
    if (direction > 0) return groupLast < bankLast ? groupLast : bankFirst;
    return run(items, groupFirst > bankFirst ? groupFirst - 1 : bankLast - 1, groupKey).first; // an item of the previous group
}

std::vector<int> serumLibraryFilter(const std::vector<SerumLibraryItem>& items, const std::string& bank,
                                    const std::optional<std::string>& group, const std::string& query)
{
    const auto words = juce::StringArray::fromTokens(juce::String::fromUTF8(query.c_str()), true);
    std::vector<int> result;
    for (size_t i = 0; i < items.size(); ++i)
    {
        const auto& item = items[i];
        if (serumLibraryBank(item) != bank || (group && serumLibraryGroup(item) != *group)) continue;
        const auto folder = juce::String::fromUTF8(item.folder.c_str()).fromFirstOccurrenceOf("/", false, false);
        const auto name = juce::String::fromUTF8(item.name.c_str());
        bool matches = true;
        for (const auto& word : words)
            matches = matches && (word.isEmpty() || name.containsIgnoreCase(word) || folder.containsIgnoreCase(word));
        if (matches) result.push_back((int) i);
    }
    return result;
}
} // namespace sonora::plugins
