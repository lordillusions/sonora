// LIB-1: installed packs on disk (PackLibrary.h).
#include "packs/PackLibrary.h"

#include "core/Sha256.h"

#include <algorithm>

namespace sonora::packs
{
namespace
{
constexpr int kMaxEntries = 20000;
constexpr int64_t kMaxUnpackedBytes = int64_t(8) << 30;

std::optional<PackContents> readContents(const juce::File& folder)
{
    const auto file = folder.getChildFile("pack.json");
    if (! file.existsAsFile() || file.getSize() > 16 * 1024 * 1024) return std::nullopt;
    std::string error;
    return parsePackContents(file.loadFileAsString().toStdString(), error);
}

// Names the archive or the system add that are not pack content.
bool ignoredEntry(const juce::String& name)
{
    return name.startsWith("__MACOSX/") || name.endsWith(".DS_Store") || name == ".DS_Store";
}

juce::File uniqueChild(const juce::File& parent, const juce::String& prefix)
{
    for (int attempt = 0;; ++attempt)
    {
        auto candidate = parent.getChildFile(prefix + "-" + juce::String::toHexString(juce::Random::getSystemRandom().nextInt64()));
        if (! candidate.exists() || attempt > 20) return candidate;
    }
}
} // namespace

std::string sha256OfFile(const juce::File& file, const std::atomic<bool>& cancelled)
{
    juce::FileInputStream in(file);
    if (! in.openedOk()) return {};
    crypto::Sha256 hash;
    std::vector<char> buffer(256 * 1024);
    for (;;)
    {
        if (cancelled) return {};
        const int n = in.read(buffer.data(), static_cast<int>(buffer.size()));
        if (n < 0) return {};
        if (n == 0) break;
        hash.update(buffer.data(), static_cast<size_t>(n));
    }
    return crypto::toHex(hash.finish());
}

juce::File PackLibrary::partialDownload(const PackEntry& entry) const
{
    return privateFolder().getChildFile("downloads").getChildFile(juce::String(entry.info.id))
        .getChildFile(juce::String(entry.info.version + "-" + entry.sha256.substr(0, 12)) + ".zip.part");
}

juce::Array<juce::File> PackLibrary::stalePartials(const std::string& id, const juce::File& keep) const
{
    juce::Array<juce::File> stale;
    if (! validPackId(id)) return stale;
    for (const auto& file : privateFolder().getChildFile("downloads").getChildFile(juce::String(id)).findChildFiles(juce::File::findFiles, false))
        if (file != keep) stale.add(file);
    return stale;
}

std::vector<InstalledPack> PackLibrary::installed() const
{
    std::vector<InstalledPack> packs;
    for (const auto& folder : root_.findChildFiles(juce::File::findDirectories, false))
    {
        if (folder.getFileName().startsWith(".")) continue;
        if (auto contents = readContents(folder)) packs.push_back({std::move(*contents), folder});
    }
    std::sort(packs.begin(), packs.end(), [](const auto& a, const auto& b) {
        return juce::String(a.contents.info.name).compareNatural(juce::String(b.contents.info.name)) < 0;
    });
    return packs;
}

std::optional<InstalledPack> PackLibrary::find(const std::string& id) const
{
    for (auto& pack : installed())
        if (pack.contents.info.id == id) return std::move(pack);
    return std::nullopt;
}

PackLibrary::Result PackLibrary::install(const juce::File& archive, const PackEntry& expected, const std::atomic<bool>& cancelled) const
{
    Result result;
    juce::ZipFile zip(archive);
    const int count = zip.getNumEntries();
    if (count == 0) { result.error = "the archive is empty or not a zip file"; return result; }
    if (count > kMaxEntries) { result.error = "the archive holds too many files"; return result; }
    const int64_t limit = expected.installedBytes > 0 ? std::min(kMaxUnpackedBytes, expected.installedBytes * 2 + (1 << 20)) : kMaxUnpackedBytes;
    int64_t total = 0;
    bool hasManifest = false, hasLicence = false;
    std::vector<int> wanted;
    for (int i = 0; i < count; ++i)
    {
        const auto* entry = zip.getEntry(i);
        const auto name = entry->filename;
        if (ignoredEntry(name)) continue;
        if (entry->isSymbolicLink) { result.error = "the archive holds a link (" + name.toStdString() + ")"; return result; }
        if (! safeRelativePath(name.toStdString())) { result.error = "the archive holds an unsafe path (" + name.toStdString() + ")"; return result; }
        total += entry->uncompressedSize;
        if (total > limit) { result.error = "the archive unpacks to more than its catalogue size"; return result; }
        hasManifest |= name == "pack.json";
        hasLicence |= name == "LICENSE.txt";
        wanted.push_back(i);
    }
    if (! hasManifest || ! hasLicence) { result.error = "the archive has no pack.json or LICENSE.txt at its top level"; return result; }
    if (! root_.createDirectory()) { result.error = "cannot create " + root_.getFullPathName().toStdString(); return result; }

    const auto stagingRoot = privateFolder().getChildFile("staging");
    stagingRoot.createDirectory();
    const auto staging = uniqueChild(stagingRoot, juce::String(expected.info.id));
    struct Cleanup { juce::File folder; ~Cleanup() { if (folder.exists()) folder.deleteRecursively(); } } cleanup{staging};
    if (! staging.createDirectory()) { result.error = "cannot create the staging folder"; return result; }
    for (const int i : wanted)
    {
        if (cancelled) { result.error = "cancelled"; return result; }
        const auto unpacked = zip.uncompressEntry(i, staging, juce::ZipFile::OverwriteFiles::yes, juce::ZipFile::FollowSymlinks::no);
        if (unpacked.failed()) { result.error = "unpacking failed: " + unpacked.getErrorMessage().toStdString(); return result; }
    }
    const auto contents = readContents(staging);
    if (! contents) { result.error = "pack.json is unreadable or breaks a rule"; return result; }
    if (contents->info.id != expected.info.id || contents->info.version != expected.info.version)
    { result.error = "pack.json is " + contents->info.id + " " + contents->info.version + ", not the catalogue's pack"; return result; }

    // The folder is named after the pack, so the Browser shows its name; another pack's folder of that name is kept.
    auto name = juce::File::createLegalFileName(juce::String::fromUTF8(contents->info.name.c_str())).trim();
    if (name.isEmpty() || name.startsWith(".")) name = juce::String(contents->info.id);
    auto target = root_.getChildFile(name);
    const auto previous = find(expected.info.id);
    if (target.exists() && ! (previous && previous->folder == target)) target = root_.getChildFile(name + " (" + juce::String(contents->info.id) + ")");
    if (previous && ! previous->folder.deleteRecursively()) { result.error = "cannot replace the installed version"; return result; }
    if (target.exists() && ! target.deleteRecursively()) { result.error = "cannot replace " + target.getFullPathName().toStdString(); return result; }
    if (! staging.moveFileTo(target)) { result.error = "cannot move the pack into " + root_.getFullPathName().toStdString(); return result; }
    result.ok = true;
    result.folder = target;
    return result;
}

bool PackLibrary::remove(const std::string& id, std::string& error, bool toTrash) const
{
    const auto pack = find(id);
    if (! pack) { error = "not installed"; return false; }
    if (! pack->folder.isAChildOf(root_)) { error = "outside the packs folder"; return false; }
    if (toTrash && pack->folder.moveToTrash()) return true;
    if (pack->folder.deleteRecursively()) return true;
    error = "cannot remove " + pack->folder.getFullPathName().toStdString();
    return false;
}

} // namespace sonora::packs
