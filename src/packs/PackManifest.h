#pragma once
// LIB-1: the sample-pack catalogue (packs.json, signed by packs.json.sig) and the pack.json inside each archive.
// Format, signing and the licence rule: docs/SAMPLE_PACKS.md. Built by tools/pack_tool; read by PackStore.
//
//   packs.json   { "format": 1, "packs": [ PackEntry... ] }            Ed25519 over its exact bytes = packs.json.sig (base64)
//   pack.json    { "format": 1, PackInfo fields..., "files": [ { "path": "Kicks/Kick 01.wav", "tags": { "Sound": "Kick" } } ] }

#include <juce_core/juce_core.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sonora::packs
{

constexpr int kManifestFormat = 1;

// What a pack is: shared by the catalogue entry and the pack.json that ships inside the archive.
struct PackInfo
{
    std::string id;      // [a-z0-9-], 1..64, never changes between versions
    std::string name, version, description, author;
    std::string licence; // SPDX identifier; only allowedLicences() pass
    std::vector<std::string> styles, types, tags;
};

struct PackEntry
{
    PackInfo info;
    std::string url;     // archive URL, absolute or relative to the catalogue URL
    std::string sha256;  // lowercase hex of the archive
    int64_t bytes = 0;   // archive size
    int64_t installedBytes = 0;
    int files = 0;
};

struct Catalogue
{
    std::vector<PackEntry> packs;
    std::vector<std::string> warnings; // packs skipped because a field broke a rule
};

struct PackFile
{
    std::string path; // relative, '/' separated
    std::vector<std::pair<std::string, std::string>> tags; // group -> value (Browser tag facets, PLG-10)
};

struct PackContents
{
    PackInfo info;
    std::string source; // how the files were made or where they came from
    std::vector<PackFile> files;
};

// The licence rule: content anyone may redistribute and use commercially without attribution (docs/SAMPLE_PACKS.md).
const std::vector<std::string>& allowedLicences();
bool licenceAllowed(std::string_view spdx);
bool validPackId(std::string_view id);
bool validSha256(std::string_view hex);
// A relative pack path inside an archive: no absolute path, drive, backslash, "." / ".." part or empty part.
bool safeRelativePath(std::string_view path);

// nullopt (and `error`) when the JSON or its format is unreadable; bad packs are skipped into Catalogue::warnings.
std::optional<Catalogue> parseCatalogue(const std::string& json, std::string& error);
std::string catalogueToJson(const Catalogue& catalogue);
std::optional<PackContents> parsePackContents(const std::string& json, std::string& error);
std::string packContentsToJson(const PackContents& contents);

// The archive URL of `entry` against the catalogue URL ("x.zip" next to packs.json).
juce::String resolveArchiveUrl(const juce::String& catalogueUrl, const std::string& url);

// The signed catalogue: true when `signatureBase64` is publicKey's Ed25519 signature of json's exact bytes.
bool verifyCatalogue(const std::string& json, std::string_view signatureBase64, std::string_view publicKeyBase64);

} // namespace sonora::packs
