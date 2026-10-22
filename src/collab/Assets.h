#pragma once
// COL-4: the files a shared set references, by content hash.
//
// The host hashes every sample its song references and lists them (AssetEntry: the FileRef as the model stores it,
// plus SHA-256, size and extension). Guests fetch the bytes they lack in 64 KiB chunks into a per-user
// content-addressed cache, so joining the same project again downloads nothing. Peers only ever ask for a hash
// from the catalog: no remote request can name a path, and a guest resolves samples only through the catalog.

#include "collab/Protocol.h"
#include "core/Model.h"

#include <juce_core/juce_core.h>

#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace sonora::collab
{

inline constexpr size_t assetChunkBytes = 64 * 1024;
inline constexpr uint64_t maxAssetBytes = 4ull << 30;

std::string assetKey(const model::FileRef&);
std::string assetKey(const AssetEntry&);

// Every sample reference of a song (arrangement, Session, take-lane and frozen clips, sampler zones), deduplicated.
std::vector<model::FileRef> sampleRefs(const model::Song&);
std::vector<model::FileRef> sampleRefs(const std::vector<ObjectState>&);

// Hashes one file (streaming). nullopt when it can't be read or is too large.
std::optional<AssetEntry> hashAsset(const model::FileRef& ref, const juce::File& file);

// Thread-safe catalog of one session's assets. The local path of an entry is the host's own file, or on a guest the
// completed cache file.
class AssetCatalog
{
public:
    void clear();
    // Returns false when the key was already known with this hash (nothing new).
    bool add(const AssetEntry& entry, const std::string& localPath = {});
    std::optional<AssetEntry> byHash(const std::string& hash) const;
    std::optional<AssetEntry> byKey(const std::string& key) const;
    std::string localPath(const std::string& hash) const;
    void setLocalPath(const std::string& hash, const std::string& path);
    // Guest resolution: the cached file, or "" when not available (yet).
    std::string resolve(const model::FileRef&) const;
    std::vector<AssetEntry> entries() const;
    // Hashes whose bytes this replica doesn't have yet.
    std::vector<std::string> missing() const;
    bool knows(const model::FileRef& ref) const { return byKey(assetKey(ref)).has_value(); }

private:
    mutable std::mutex lock_;
    std::map<std::string, AssetEntry> byKey_;   // assetKey -> entry
    std::map<std::string, std::string> local_;  // hash -> local file ("" while missing)
    std::map<std::string, AssetEntry> byHash_;  // one entry per hash (first seen)
};

// The per-user cache folder (<app data>/CollabCache) and a hash's file in it.
juce::File collabCacheDirectory();
juce::File cachedAssetFile(const std::string& hash, const std::string& extension);

// A guest's download of one asset: chunks must arrive in order; the file is verified before it becomes visible.
class AssetDownload
{
public:
    AssetDownload(AssetEntry entry, juce::File cacheDirectory);
    ~AssetDownload();
    // Returns false (and gives up) on an out-of-order, oversize or unwritable chunk.
    bool append(const AssetChunkMsg&);
    bool complete() const { return written_ == entry_.size; }
    // After the last chunk: verifies the hash and moves the file into place. Returns the final path or "".
    std::string finish(std::string& error);
    uint64_t written() const { return written_; }
    const AssetEntry& entry() const { return entry_; }

private:
    AssetEntry entry_;
    juce::File partial_, final_;
    std::unique_ptr<juce::FileOutputStream> out_;
    uint64_t written_ = 0;
};

// Reads one chunk of a host file (nullopt when unreadable).
std::optional<std::string> readAssetChunk(const juce::File& file, uint64_t offset, size_t maxBytes = assetChunkBytes);

// A file name that is safe to create inside a folder: no separators, no leading dots, bounded, keeps the extension.
std::string safeFileName(const std::string& name);

} // namespace sonora::collab
