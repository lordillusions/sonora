#include "collab/Assets.h"

#include "core/AppDataFolder.h"
#include "core/ModelCodec.h"
#include "core/Sha256.h"

#include <set>

namespace sonora::collab
{
namespace
{
void addRef(const model::FileRef& ref, std::set<std::string>& seen, std::vector<model::FileRef>& out)
{
    if (ref.absolutePath.empty() && ref.relativePath.empty()) return;
    if (seen.insert(assetKey(ref)).second) out.push_back(ref);
}

void addClip(const model::Clip& c, std::set<std::string>& seen, std::vector<model::FileRef>& out)
{
    if (c.kind == model::Clip::Kind::Audio) addRef(c.sample, seen, out);
}

void addDevice(const model::Device& d, std::set<std::string>& seen, std::vector<model::FileRef>& out)
{
    for (const auto& part : d.sampleParts) addRef(part.sample, seen, out);
    for (const auto& chain : d.chains)
        for (const auto& child : chain.devices) addDevice(child, seen, out);
}

void addTrack(const model::Track& t, bool withDevicesAndClips, std::set<std::string>& seen, std::vector<model::FileRef>& out)
{
    for (const auto& lane : t.takeLanes)
        for (const auto& c : lane.clips) addClip(c, seen, out);
    for (const auto& c : t.frozenArrangement) addClip(c, seen, out);
    for (const auto& s : t.frozenSlots)
        if (s.clip) addClip(*s.clip, seen, out);
    if (! withDevicesAndClips) return;
    for (const auto& d : t.devices) addDevice(d, seen, out);
    for (const auto& c : t.arrangementClips) addClip(c, seen, out);
    for (const auto& s : t.slots)
        if (s.clip) addClip(*s.clip, seen, out);
}

std::string lowerExtension(const juce::File& file)
{
    auto ext = file.getFileExtension().toLowerCase().toStdString();
    if (ext.size() > 16 || ext.find_first_not_of(".abcdefghijklmnopqrstuvwxyz0123456789") != std::string::npos) ext.clear();
    return ext;
}

bool validHash(const std::string& hash)
{
    return hash.size() == 64 && hash.find_first_not_of("0123456789abcdef") == std::string::npos;
}
} // namespace

std::string assetKey(const model::FileRef& ref)
{
    return std::to_string(ref.absolutePath.size()) + ":" + ref.absolutePath + "|" + std::to_string(ref.relativePath.size()) + ":"
         + ref.relativePath + "|" + ref.sourceDirectory;
}

std::string assetKey(const AssetEntry& e)
{
    model::FileRef ref;
    ref.absolutePath = e.absolutePath;
    ref.relativePath = e.relativePath;
    ref.sourceDirectory = e.sourceDirectory;
    return assetKey(ref);
}

std::vector<model::FileRef> sampleRefs(const model::Song& song)
{
    std::set<std::string> seen;
    std::vector<model::FileRef> out;
    for (const auto& t : song.tracks) addTrack(t, true, seen, out);
    for (const auto& t : song.returns) addTrack(t, true, seen, out);
    addTrack(song.main, true, seen, out);
    return out;
}

std::vector<model::FileRef> sampleRefs(const std::vector<ObjectState>& objects)
{
    std::set<std::string> seen;
    std::vector<model::FileRef> out;
    for (const auto& o : objects)
    {
        if (! o.full) continue;
        try
        {
            switch (o.key.kind)
            {
                case ObjectKey::Kind::Track: addTrack(model::codec::decodeTrackHeader(*o.full), false, seen, out); break;
                case ObjectKey::Kind::Devices:
                    for (const auto& d : model::codec::decodeDevices(*o.full)) addDevice(d, seen, out);
                    break;
                case ObjectKey::Kind::Clip: addClip(model::codec::decodeClip(*o.full), seen, out); break;
                case ObjectKey::Kind::Song:
                case ObjectKey::Kind::Structure:
                case ObjectKey::Kind::Order: break;
            }
        }
        catch (const model::CodecError&) {}
    }
    return out;
}

std::optional<AssetEntry> hashAsset(const model::FileRef& ref, const juce::File& file)
{
    if (! file.existsAsFile()) return std::nullopt;
    const auto size = static_cast<uint64_t>(file.getSize());
    if (size > maxAssetBytes) return std::nullopt;
    juce::FileInputStream in(file);
    if (! in.openedOk()) return std::nullopt;
    crypto::Sha256 sha;
    std::vector<char> buffer(1 << 20);
    uint64_t total = 0;
    for (;;)
    {
        const int n = in.read(buffer.data(), static_cast<int>(buffer.size()));
        if (n <= 0) break;
        sha.update(buffer.data(), static_cast<size_t>(n));
        total += static_cast<uint64_t>(n);
    }
    if (total != size) return std::nullopt;
    AssetEntry e;
    e.absolutePath = ref.absolutePath;
    e.relativePath = ref.relativePath;
    e.sourceDirectory = ref.sourceDirectory;
    e.hash = crypto::toHex(sha.finish());
    e.size = size;
    e.extension = lowerExtension(file);
    return e;
}

void AssetCatalog::clear()
{
    const std::lock_guard<std::mutex> l(lock_);
    byKey_.clear();
    local_.clear();
    byHash_.clear();
}

bool AssetCatalog::add(const AssetEntry& entry, const std::string& localPath)
{
    if (! validHash(entry.hash) || entry.size > maxAssetBytes) return false;
    const std::lock_guard<std::mutex> l(lock_);
    const auto key = assetKey(entry);
    if (auto it = byKey_.find(key); it != byKey_.end() && it->second.hash == entry.hash) return false;
    byKey_[key] = entry;
    byHash_.emplace(entry.hash, entry);
    auto& local = local_[entry.hash];
    if (! localPath.empty()) local = localPath;
    return true;
}

std::optional<AssetEntry> AssetCatalog::byHash(const std::string& hash) const
{
    const std::lock_guard<std::mutex> l(lock_);
    if (auto it = byHash_.find(hash); it != byHash_.end()) return it->second;
    return std::nullopt;
}

std::optional<AssetEntry> AssetCatalog::byKey(const std::string& key) const
{
    const std::lock_guard<std::mutex> l(lock_);
    if (auto it = byKey_.find(key); it != byKey_.end()) return it->second;
    return std::nullopt;
}

std::string AssetCatalog::localPath(const std::string& hash) const
{
    const std::lock_guard<std::mutex> l(lock_);
    if (auto it = local_.find(hash); it != local_.end()) return it->second;
    return {};
}

void AssetCatalog::setLocalPath(const std::string& hash, const std::string& path)
{
    const std::lock_guard<std::mutex> l(lock_);
    if (byHash_.count(hash)) local_[hash] = path;
}

std::string AssetCatalog::resolve(const model::FileRef& ref) const
{
    const std::lock_guard<std::mutex> l(lock_);
    const auto it = byKey_.find(assetKey(ref));
    if (it == byKey_.end()) return {};
    const auto local = local_.find(it->second.hash);
    return local == local_.end() ? std::string() : local->second;
}

std::vector<AssetEntry> AssetCatalog::entries() const
{
    const std::lock_guard<std::mutex> l(lock_);
    std::vector<AssetEntry> out;
    for (const auto& [key, e] : byKey_) out.push_back(e);
    return out;
}

std::vector<std::string> AssetCatalog::missing() const
{
    const std::lock_guard<std::mutex> l(lock_);
    std::vector<std::string> out;
    for (const auto& [hash, path] : local_)
        if (path.empty()) out.push_back(hash);
    return out;
}

juce::File collabCacheDirectory() { return core::appDataDirectory().getChildFile("CollabCache"); }

juce::File cachedAssetFile(const std::string& hash, const std::string& extension)
{
    return collabCacheDirectory().getChildFile(juce::String(hash) + juce::String(extension));
}

AssetDownload::AssetDownload(AssetEntry entry, juce::File cacheDirectory) : entry_(std::move(entry))
{
    cacheDirectory.createDirectory();
    final_ = cacheDirectory.getChildFile(juce::String(entry_.hash) + juce::String(entry_.extension));
    // Unique per download: two Sonoras on one machine may fetch the same file into the shared cache at once.
    partial_ = cacheDirectory.getChildFile(juce::String(entry_.hash) + "." + juce::String::toHexString(juce::Random::getSystemRandom().nextInt64()) + ".part");
    out_ = std::make_unique<juce::FileOutputStream>(partial_);
    if (! out_->openedOk()) out_.reset();
}

AssetDownload::~AssetDownload()
{
    out_.reset();
    if (! complete()) partial_.deleteFile();
}

bool AssetDownload::append(const AssetChunkMsg& chunk)
{
    if (! out_ || chunk.missing || chunk.hash != entry_.hash || chunk.offset != written_ || chunk.total != entry_.size
        || written_ + chunk.bytes.size() > entry_.size || chunk.bytes.empty())
    {
        out_.reset();
        return false;
    }
    if (! out_->write(chunk.bytes.data(), chunk.bytes.size()))
    {
        out_.reset();
        return false;
    }
    written_ += chunk.bytes.size();
    return true;
}

std::string AssetDownload::finish(std::string& error)
{
    if (! out_ || ! complete()) { error = "incomplete download"; return {}; }
    out_->flush();
    out_.reset();
    model::FileRef ref;
    const auto hashed = hashAsset(ref, partial_);
    if (! hashed || hashed->hash != entry_.hash)
    {
        partial_.deleteFile();
        error = "the downloaded file does not match its hash";
        return {};
    }
    if (final_.existsAsFile() && static_cast<uint64_t>(final_.getSize()) == entry_.size)
    {
        partial_.deleteFile(); // someone else finished the same file first
        return final_.getFullPathName().toStdString();
    }
    if (! partial_.moveFileTo(final_)) { error = "can't move the download into the cache"; return {}; }
    return final_.getFullPathName().toStdString();
}

std::optional<std::string> readAssetChunk(const juce::File& file, uint64_t offset, size_t maxBytes)
{
    juce::FileInputStream in(file);
    if (! in.openedOk()) return std::nullopt;
    const auto size = static_cast<uint64_t>(in.getTotalLength());
    if (offset > size) return std::nullopt;
    if (! in.setPosition(static_cast<juce::int64>(offset))) return std::nullopt;
    std::string bytes(static_cast<size_t>(std::min<uint64_t>(maxBytes, size - offset)), '\0');
    if (bytes.empty()) return bytes;
    const int n = in.read(bytes.data(), static_cast<int>(bytes.size()));
    if (n != static_cast<int>(bytes.size())) return std::nullopt;
    return bytes;
}

std::string safeFileName(const std::string& name)
{
    auto base = juce::File::createLegalFileName(juce::String::fromUTF8(name.c_str())).toStdString();
    const auto slash = base.find_last_of("/\\");
    if (slash != std::string::npos) base = base.substr(slash + 1);
    while (! base.empty() && (base.front() == '.' || base.front() == ' ')) base.erase(0, 1);
    if (base.size() > 120)
    {
        const auto dot = base.find_last_of('.');
        const auto ext = dot != std::string::npos && base.size() - dot <= 16 ? base.substr(dot) : std::string();
        base = base.substr(0, 120 - ext.size()) + ext;
    }
    return base.empty() ? std::string("Sample") : base;
}

} // namespace sonora::collab
